/* Region allocator for all candidate VM allocations. This is storage only:
 * it never traces, moves or frees a live guest object. All backing/descriptor/
 * allocation-start bitmap capacity is inside the existing arena ceiling. */
#define _GNU_SOURCE
#include "next_arena.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "dvm/dvm_internal.h"
#include "perf_attr.h"
#include "leak_scope.h"
#include "gc_metrics.h"
#include "page_geometry.h"
int sdk_trace_fprintf(FILE *,const char *,...);
void *vm_gc_scratch_alloc(size_t bytes){return malloc(bytes);}
void vm_gc_scratch_free(void *pointer){free(pointer);}

#define ALIGNMENT _Alignof(max_align_t)
#define FREE_BINS (sizeof(size_t)*8u)
typedef union block block;
typedef struct region region;
union block {
 max_align_t alignment;
 struct {size_t span,previous_span,requested;region *owner;
  block *free_prev,*free_next;bool allocated,external;} h;
};
struct region {
 region *prev,*next;unsigned char *data,*bitmap,*object_bitmap;
 block *last;
 size_t capacity,bump,charge,bitmap_bytes,metadata_bytes,live_allocations,id;
 bool humongous,emergency_only;
};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool active;
static size_t limit,used,peak,denied,live_bytes,live_requested,peak_live,allocations,reused;
static size_t next_region_id;
static region *regions;
static block *free_bins[FREE_BINS];
static _Thread_local unsigned emergency;
static struct vm_arena_diagnostic diagnostic;
static unsigned region_failure_reason;
/* Observation never allocates guest objects or acquires the GIL. Fixed ring
 * writes under the arena mutex; filesystem I/O happens after releasing it. */
#define HEAP_DIR "/data/storage/el2/base/haps/entry/files/sdkdex/"
struct heap_failure {size_t serial,requested,charge,used,live,limit;uint64_t mono_ms;long tid;unsigned reason;uintptr_t caller;};
static struct heap_failure heap_failures[64];
static size_t heap_failure_serial;
static unsigned heap_generation;
static struct vm_external_snapshot external;
static bool heap_observe;
static uint64_t heap_mono_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
size_t vm_arena_configured_limit(void){
 FILE *escape=fopen(HEAP_DIR "mem-escape-observe","wx");if(escape){fputs("0\n",escape);fclose(escape);chmod(HEAP_DIR "mem-escape-observe",0666);}
 FILE *seed=fopen(HEAP_DIR "arena-limit-mib","wx");if(seed){fputs("256\n",seed);fclose(seed);chmod(HEAP_DIR "arena-limit-mib",0666);}
 seed=fopen(HEAP_DIR "heap-observe","wx");if(seed){fputs("0\n",seed);fclose(seed);chmod(HEAP_DIR "heap-observe",0666);}
 FILE *f=fopen(HEAP_DIR "arena-limit-mib","r");unsigned mib=256;char extra;
 if(f){if(fscanf(f,"%u %c",&mib,&extra)!=1||(mib!=128&&mib!=192&&mib!=256))mib=256;fclose(f);}
 sdk_trace_fprintf(stderr,"HEAP_LIMIT_CONFIG pid=%d mib=%u default_mib=256 max_mib=256 moving_changed=0\n",getpid(),mib);
 return (size_t)mib*1024*1024;
}
/* Dedicated observer only: try-lock, copy bounded native counters, release,
 * then do all filesystem I/O. Never called by VM/render/vsync callbacks. */
static void *heap_observer(void *unused){
 (void)unused;unsigned observed_generation=0;size_t last_failure=0;
 for(;;){
  struct timespec delay={1,0};nanosleep(&delay,NULL);
  leak_scope_status();
  dvm_gc_metrics_worker();
  FILE *control=fopen(HEAP_DIR "mem-escape-observe","r");int escape_on=0;
  if(control){(void)fscanf(control,"%d",&escape_on);fclose(control);}
  extern void dvm_mem_idle_configure(int) __attribute__((weak));if(dvm_mem_idle_configure)dvm_mem_idle_configure(escape_on==1);
  control=fopen(HEAP_DIR "heap-observe","r");int observe_on=0;if(control){(void)fscanf(control,"%d",&observe_on);fclose(control);}if(observe_on!=1)continue;

  struct heap_failure pending[64];size_t count=0;unsigned generation;
  struct vm_arena_region_snapshot s={0};struct vm_external_snapshot x={0};
  if(pthread_mutex_trylock(&lock))continue;
  struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);
  if(!active||!heap_observe){pthread_mutex_unlock(&lock);continue;}
  generation=heap_generation;if(observed_generation!=generation){observed_generation=generation;last_failure=0;}
  size_t first=heap_failure_serial>64?heap_failure_serial-64:0;if(first<last_failure)first=last_failure;
  for(size_t i=first;i<heap_failure_serial;i++)pending[count++]=heap_failures[i%64];last_failure=heap_failure_serial;
  s.limit_bytes=limit;s.committed_bytes=used;s.live_bytes=live_bytes;s.live_requested_bytes=live_requested;
  s.live_allocations=allocations;s.reuse_hits=reused;s.denied=denied;
  for(region *r=regions;r;r=r->next)s.metadata_bytes+=r->metadata_bytes;
  s.free_bytes=used-s.metadata_bytes-live_bytes;x=external;
  clock_gettime(CLOCK_MONOTONIC,&end);pthread_mutex_unlock(&lock);
  uint64_t pause_ns=(uint64_t)(end.tv_sec-begin.tv_sec)*1000000000ull+end.tv_nsec-begin.tv_nsec;
  /* Observer-owned metadata: repair legacy modes through the app-owned fd.
   * No rotation policy is introduced; also repair an existing backup. */
  int old_fd=open(HEAP_DIR "mem-leak-curve.csv.1",O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
  if(old_fd>=0){(void)fchmod(old_fd,0644);close(old_fd);}
  int fd=open(HEAP_DIR "mem-leak-curve.csv",O_WRONLY|O_CREAT|O_APPEND,0644);
  if(fd>=0&&fchmod(fd,0644)){close(fd);fd=-1;}
  if(fd>=0){if(lseek(fd,0,SEEK_END)==0)dprintf(fd,"pid,mono_ms,limit,committed,live_charge,live_requested,free,metadata,allocations,reuse,denied,external,external_denied,observer_lock_ns\n");
   dprintf(fd,"%d,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%llu\n",getpid(),(unsigned long long)heap_mono_ms(),s.limit_bytes,s.committed_bytes,s.live_bytes,s.live_requested_bytes,s.free_bytes,s.metadata_bytes,s.live_allocations,s.reuse_hits,s.denied,x.bytes,x.denied,(unsigned long long)pause_ns);close(fd);}
  extern void dvm_mem_idle_export(void) __attribute__((weak));if(dvm_mem_idle_export)dvm_mem_idle_export();
  if(count){fd=open(HEAP_DIR "heap-denied.txt",O_WRONLY|O_CREAT|O_APPEND,0666);
   for(size_t i=0;i<count;i++){struct heap_failure *e=&pending[i];if(fd>=0)dprintf(fd,"pid=%d serial=%zu mono_ms=%llu tid=%ld requested=%zu charge=%zu committed=%zu live=%zu limit=%zu reason=%u caller=%p\n",getpid(),e->serial,(unsigned long long)e->mono_ms,e->tid,e->requested,e->charge,e->used,e->live,e->limit,e->reason,(void*)e->caller);}if(fd>=0)close(fd);}
 }
 return NULL;
}
static pthread_once_t heap_observer_once=PTHREAD_ONCE_INIT;
static void heap_observer_start(void){pthread_t t;int e=pthread_create(&t,NULL,heap_observer,NULL);if(!e)pthread_detach(t);else sdk_trace_fprintf(stderr,"HEAP_OBSERVER_CREATE_FAILED error=%d\n",e);}
static bool block_span(size_t,size_t *);

extern void dvm_large_allocation_note(size_t,uintptr_t) __attribute__((weak));
void vm_external_snapshot(struct vm_external_snapshot *s){if(!s)return;pthread_mutex_lock(&lock);*s=external;s->limit=VM_EXTERNAL_LIMIT;pthread_mutex_unlock(&lock);}
void *vm_external_calloc(size_t n){
 if(dvm_large_allocation_note)dvm_large_allocation_note(n,(uintptr_t)__builtin_return_address(0));
 size_t span;size_t page=host_page_size();if(page<=0||!block_span(n,&span)||span>SIZE_MAX-(size_t)page)return NULL;
 span=(span+(size_t)page-1)/(size_t)page*(size_t)page;
 pthread_mutex_lock(&lock);
 if(span>VM_EXTERNAL_LIMIT-external.bytes){external.denied++;pthread_mutex_unlock(&lock);errno=ENOMEM;return NULL;}
 block *b=mmap(NULL,span,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(b==MAP_FAILED){external.denied++;pthread_mutex_unlock(&lock);return NULL;}
 b->h.span=span;b->h.requested=n;b->h.allocated=true;b->h.external=true;
 external.bytes+=span;if(external.bytes>external.peak)external.peak=external.bytes;
 if(external.bytes>=VM_EXTERNAL_LIMIT*4u/5u)sdk_trace_fprintf(stderr,"EXTERNAL_PAYLOAD_WARNING used=%zu limit=%u\n",external.bytes,VM_EXTERNAL_LIMIT);
 pthread_mutex_unlock(&lock);return b+1;
}
static unsigned histogram_bin(size_t n){
 static const size_t upper[VM_ARENA_HISTOGRAM_BINS]={0,8,32,128,512,4096,65536,SIZE_MAX};
 unsigned i=0;while(i+1<VM_ARENA_HISTOGRAM_BINS&&n>upper[i])i++;return i;
}
static void allocation_added(size_t requested,size_t charge){
 struct vm_arena_histogram_bin *b=&diagnostic.bins[histogram_bin(requested)];
 b->allocations++;b->live_blocks++;b->live_requested+=requested;b->live_charge+=charge;
 if(b->live_charge>b->peak_charge)b->peak_charge=b->live_charge;
}
static void allocation_removed(size_t requested,size_t charge){
 struct vm_arena_histogram_bin *b=&diagnostic.bins[histogram_bin(requested)];
 b->frees++;b->live_blocks--;b->live_requested-=requested;b->live_charge-=charge;
}

static size_t align_down(size_t n){return n-n%ALIGNMENT;}
static bool align_up(size_t n,size_t *out){
 size_t extra=(ALIGNMENT-n%ALIGNMENT)%ALIGNMENT;
 if(n>SIZE_MAX-extra)return false;*out=n+extra;return true;
}
static bool block_span(size_t n,size_t *span){
 size_t payload;if(!align_up(n?n:1,&payload)||payload>SIZE_MAX-sizeof(block))return false;
 *span=sizeof(block)+payload;return true;
}
static size_t ordinary_threshold(void){
 size_t reserve=limit>4u*1024u*1024u?2u*1024u*1024u:limit/8;
 return limit-reserve;
}
static size_t threshold(void){return emergency?limit:ordinary_threshold();}
static void allocation_denied(size_t requested,size_t charge,unsigned reason,uintptr_t caller){
 if(!active)return;denied++;diagnostic.bins[histogram_bin(requested)].denied++;
 diagnostic.last_requested=requested;diagnostic.last_charge=charge;diagnostic.last_used=used;
 diagnostic.last_threshold=threshold();diagnostic.last_limit=limit;diagnostic.last_emergency=emergency;
 diagnostic.last_reason=reason;diagnostic.last_caller=caller;
 if(heap_observe){size_t serial=heap_failure_serial++;heap_failures[serial%64]=(struct heap_failure){serial,requested,charge,used,live_bytes,limit,heap_mono_ms(),(long)syscall(SYS_gettid),reason,caller};}
}
static unsigned bin_for(size_t span){unsigned bin=0;while(span>1&&bin+1<FREE_BINS){span=span/2+span%2;bin++;}return bin;}
static void free_unlink(block *b){
 unsigned bin=bin_for(b->h.span);
 if(b->h.free_prev)b->h.free_prev->h.free_next=b->h.free_next;
 else free_bins[bin]=b->h.free_next;
 if(b->h.free_next)b->h.free_next->h.free_prev=b->h.free_prev;
 b->h.free_prev=b->h.free_next=NULL;
}
static void free_link(block *b){
 unsigned bin=bin_for(b->h.span);b->h.allocated=false;b->h.requested=0;
 b->h.free_prev=NULL;b->h.free_next=free_bins[bin];
 if(free_bins[bin])free_bins[bin]->h.free_prev=b;free_bins[bin]=b;
}
static void bitmap_set(region *r,block *b,bool value){
 size_t index=((unsigned char *)(b+1)-r->data)/ALIGNMENT;
 unsigned char mask=(unsigned char)(1u<<(index%8));
 if(value)r->bitmap[index/8]|=mask;else r->bitmap[index/8]&=(unsigned char)~mask;
}
static bool bitmap_has(region *r,block *b){
 size_t index=((unsigned char *)(b+1)-r->data)/ALIGNMENT;
 return (r->bitmap[index/8]&(1u<<(index%8)))!=0;
}
static void object_bitmap_clear(region *r,const void *start,size_t length){
 if(!length)return;
 size_t offset=(size_t)((const unsigned char *)start-r->data);
 size_t first=offset/ALIGNMENT+(offset%ALIGNMENT!=0);
 size_t end=(offset+length)/ALIGNMENT+((offset+length)%ALIGNMENT!=0);
 while(first<end&&(first%8)){r->object_bitmap[first/8]&=(unsigned char)~(1u<<(first%8));first++;}
 size_t bytes=(end-first)/8;if(bytes){memset(r->object_bitmap+first/8,0,bytes);first+=8*bytes;}
 while(first<end){r->object_bitmap[first/8]&=(unsigned char)~(1u<<(first%8));first++;}
}
static block *following(region *r,block *b){
 unsigned char *p=(unsigned char *)b+b->h.span;
 return p<r->data+r->bump?(block *)p:NULL;
}
static void fix_following(region *r,block *b){block *next=following(r,b);if(next)next->h.previous_span=b->h.span;else r->last=b;}
static size_t region_capacity(void){
 size_t cap=VM_ARENA_REGION_BYTES;
 if(limit<2u*VM_ARENA_REGION_BYTES)cap=align_down(limit/2);
 return cap;
}
static region *region_new(size_t needed){
 size_t normal=region_capacity(),capacity=normal;bool huge=needed>normal;
 if(huge)capacity=needed;
 size_t meta,bitmap=capacity/ALIGNMENT/8+(capacity/ALIGNMENT%8!=0);region_failure_reason=3;
 if(!capacity||!align_up(sizeof(region),&meta)||bitmap>(SIZE_MAX-meta)/2||
    !align_up(meta+2*bitmap,&meta)||capacity>SIZE_MAX-meta)return NULL;
 size_t charge=meta+capacity,budget=threshold();
 region_failure_reason=1;if(used>budget||charge>budget-used)return NULL;
 region_failure_reason=2;region *r=malloc(charge);if(!r)return NULL;
 memset(r,0,sizeof *r);r->capacity=capacity;r->charge=charge;
 r->id=++next_region_id;
 r->bitmap_bytes=2*bitmap;r->metadata_bytes=meta;r->humongous=huge;
 r->emergency_only=used+charge>ordinary_threshold();
 size_t prefix;align_up(sizeof(region),&prefix);r->bitmap=(unsigned char *)r+prefix;
 r->object_bitmap=r->bitmap+bitmap;memset(r->bitmap,0,2*bitmap);r->data=(unsigned char *)r+meta;
 r->next=regions;if(regions)regions->prev=r;regions=r;
 used+=charge;if(used>peak)peak=used;return r;
}
static void region_drop(region *r){
 /* Called only for an empty region. Remove all free cells before backing. */
 for(size_t at=0;at<r->bump;){block *b=(block *)(r->data+at);size_t span=b->h.span;
  if(!b->h.allocated)free_unlink(b);at+=span;}
 if(r->prev)r->prev->next=r->next;else regions=r->next;
 if(r->next)r->next->prev=r->prev;used-=r->charge;free(r);
}
static void *allocate_locked(size_t n,uintptr_t caller){
 size_t span;if(!block_span(n,&span)){allocation_denied(n,SIZE_MAX,3,caller);errno=ENOMEM;return NULL;}
 if(!active){block *b=malloc(span);if(!b){errno=ENOMEM;return NULL;}
  memset(b,0,sizeof *b);b->h.span=span;b->h.requested=n;b->h.allocated=true;return b+1;}
 if(span>limit){allocation_denied(n,span,1,caller);errno=ENOMEM;return NULL;}
 block *b=NULL;region *r=NULL;
 for(unsigned bin=bin_for(span);bin<FREE_BINS&&!b;bin++)
  for(block *at=free_bins[bin];at;at=at->h.free_next)
   if(at->h.span>=span&&(emergency||!at->h.owner->emergency_only)){b=at;break;}
 if(b){
  r=b->h.owner;free_unlink(b);reused++;
  size_t remaining=b->h.span-span;
  if(remaining>=sizeof(block)+ALIGNMENT){
   b->h.span=span;block *tail=(block *)((unsigned char *)b+span);
   memset(tail,0,sizeof *tail);tail->h.owner=r;tail->h.span=remaining;tail->h.previous_span=span;
   fix_following(r,tail);free_link(tail);
  }
 }else{
  for(region *at=regions;at;at=at->next)
   if(!at->humongous&&(emergency||!at->emergency_only)&&at->capacity-at->bump>=span){r=at;break;}
  if(!r)r=region_new(span);
  if(!r){allocation_denied(n,span,region_failure_reason,caller);errno=ENOMEM;return NULL;}
  b=(block *)(r->data+r->bump);memset(b,0,sizeof *b);
  b->h.previous_span=r->last?r->last->h.span:0;b->h.span=span;b->h.owner=r;r->bump+=span;r->last=b;
 }
 b->h.allocated=true;b->h.requested=n;b->h.free_prev=b->h.free_next=NULL;
 bitmap_set(r,b,true);r->live_allocations++;allocations++;
 live_bytes+=b->h.span;live_requested+=n;if(live_bytes>peak_live)peak_live=live_bytes;
 allocation_added(n,b->h.span);
 return b+1;
}
static void release_locked(block *b){
 if(b->h.external){size_t span=b->h.span;if(!munmap(b,span)){external.bytes-=span;external.physical_reclaimed_bytes+=span;}else sdk_trace_fprintf(stderr,"EXTERNAL_PAYLOAD_RELEASE_FAILED errno=%d bytes=%zu\n",errno,span);return;}
 region *r=b->h.owner;if(!r){free(b);return;}
 object_bitmap_clear(r,b+1,b->h.span-sizeof(block));
 bitmap_set(r,b,false);allocation_removed(b->h.requested,b->h.span);live_bytes-=b->h.span;live_requested-=b->h.requested;
 allocations--;r->live_allocations--;b->h.allocated=false;b->h.requested=0;
 block *next=following(r,b);
 if(next&&!next->h.allocated){free_unlink(next);b->h.span+=next->h.span;}
 if(b->h.previous_span){block *previous=(block *)((unsigned char *)b-b->h.previous_span);
  if(!previous->h.allocated){free_unlink(previous);previous->h.span+=b->h.span;b=previous;}}
 fix_following(r,b);free_link(b);
 if(!r->live_allocations)region_drop(r);
}
bool vm_arena_begin(size_t maximum){
 pthread_mutex_lock(&lock);bool ok=!active&&!regions&&maximum&&maximum<=VM_ARENA_CONFIG_MAX;
 if(ok){active=true;limit=maximum;used=peak=denied=live_bytes=live_requested=peak_live=allocations=reused=next_region_id=0;memset(free_bins,0,sizeof free_bins);memset(&diagnostic,0,sizeof diagnostic);}
 unsigned generation=0;bool observe=false;
 if(ok){heap_failure_serial=0;generation=++heap_generation;FILE *f=fopen(HEAP_DIR "heap-observe","r");int flag=0;if(f){if(fscanf(f,"%d",&flag)!=1)flag=0;fclose(f);}heap_observe=observe=flag==1;}
 pthread_mutex_unlock(&lock);
 FILE *reclaim_control=fopen(HEAP_DIR "leak-proxy-reclaim","r");int reclaim=0;
 if(reclaim_control){if(fscanf(reclaim_control,"%d",&reclaim)!=1)reclaim=0;fclose(reclaim_control);}
 perf_leak_proxy_reclaim_configure(reclaim);
 /* GC storage counters remain available with both observation/reclaim off. */
 (void)generation;(void)observe;if(ok)pthread_once(&heap_observer_once,heap_observer_start);
 return ok;
}
void vm_arena_emergency_enter(void){++emergency;}
void vm_arena_emergency_leave(void){if(emergency)--emergency;}
void *vm_arena_malloc(size_t n){if(n>=VM_EXTERNAL_THRESHOLD&&dvm_large_allocation_note)dvm_large_allocation_note(n,(uintptr_t)__builtin_return_address(0));pthread_mutex_lock(&lock);void *p=allocate_locked(n,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);return p;}
void vm_arena_free(void *p){if(!p)return;pthread_mutex_lock(&lock);release_locked((block *)p-1);pthread_mutex_unlock(&lock);}
void *vm_arena_calloc(size_t n,size_t size){
 if(size&&n>SIZE_MAX/size){pthread_mutex_lock(&lock);allocation_denied(SIZE_MAX,SIZE_MAX,3,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);errno=ENOMEM;return NULL;}
 size_t bytes=n*size;void *p=vm_arena_malloc(bytes);if(p)memset(p,0,bytes);return p;
}
void *vm_arena_realloc(void *p,size_t n){
 if(n>=VM_EXTERNAL_THRESHOLD&&dvm_large_allocation_note)dvm_large_allocation_note(n,(uintptr_t)__builtin_return_address(0));
 if(!p)return vm_arena_malloc(n);if(!n){vm_arena_free(p);return NULL;}
 pthread_mutex_lock(&lock);block *b=(block *)p-1;size_t span;
 if(b->h.external){size_t old=b->h.requested;pthread_mutex_unlock(&lock);void *next=vm_external_calloc(n);if(next){memcpy(next,p,old<n?old:n);vm_arena_free(p);}return next;}
 if(!block_span(n,&span)){allocation_denied(n,SIZE_MAX,3,(uintptr_t)__builtin_return_address(0));errno=ENOMEM;pthread_mutex_unlock(&lock);return NULL;}
 region *r=b->h.owner;
 if(!r){block *moved=realloc(b,span);if(moved){moved->h.span=span;moved->h.requested=n;}
  pthread_mutex_unlock(&lock);return moved?moved+1:NULL;}
 size_t original=b->h.span,prior_requested=b->h.requested;
 if(span<=original){
  if(n<prior_requested)object_bitmap_clear(r,(unsigned char *)p+n,prior_requested-n);
  size_t remaining=original-span;
  if(remaining>=sizeof(block)+ALIGNMENT){
   b->h.span=span;block *tail=(block *)((unsigned char *)b+span);memset(tail,0,sizeof *tail);
   tail->h.owner=r;tail->h.span=remaining;tail->h.previous_span=span;
   block *next=following(r,tail);if(next&&!next->h.allocated){free_unlink(next);tail->h.span+=next->h.span;}
   fix_following(r,tail);free_link(tail);live_bytes-=remaining;
  }
  allocation_removed(prior_requested,original);allocation_added(n,b->h.span);
  b->h.requested=n;live_requested=live_requested-prior_requested+n;
  pthread_mutex_unlock(&lock);return p;
 }
 block *next=following(r,b);size_t combined=original;
 if(next&&!next->h.allocated)combined+=next->h.span;
 if((emergency||!r->emergency_only)&&combined>=span){
  free_unlink(next);b->h.span=combined;
  if(combined-span>=sizeof(block)+ALIGNMENT){
   b->h.span=span;block *tail=(block *)((unsigned char *)b+span);memset(tail,0,sizeof *tail);
   tail->h.owner=r;tail->h.span=combined-span;tail->h.previous_span=span;fix_following(r,tail);free_link(tail);
  }else fix_following(r,b);
  allocation_removed(prior_requested,original);allocation_added(n,b->h.span);
  live_bytes+=b->h.span-original;b->h.requested=n;live_requested=live_requested-prior_requested+n;
  if(live_bytes>peak_live)peak_live=live_bytes;pthread_mutex_unlock(&lock);return p;
 }
 if((emergency||!r->emergency_only)&&!next&&r->capacity-r->bump>=span-original){
  r->bump+=span-original;b->h.span=span;b->h.requested=n;
  allocation_removed(prior_requested,original);allocation_added(n,b->h.span);
  live_bytes+=span-original;live_requested=live_requested-prior_requested+n;
  if(live_bytes>peak_live)peak_live=live_bytes;pthread_mutex_unlock(&lock);return p;
 }
 void *replacement=allocate_locked(n,(uintptr_t)__builtin_return_address(0));
 if(replacement){memcpy(replacement,p,prior_requested<n?prior_requested:n);release_locked(b);}
 pthread_mutex_unlock(&lock);return replacement;
}
char *vm_arena_strdup(const char *s){size_t n=strlen(s);char *p=vm_arena_malloc(n+1);if(p)memcpy(p,s,n+1);return p;}
char *vm_arena_strndup(const char *s,size_t max){size_t n=strnlen(s,max);char *p=vm_arena_malloc(n+1);if(p){memcpy(p,s,n);p[n]=0;}return p;}
#define READ(name,var) size_t name(void){pthread_mutex_lock(&lock);size_t n=active?var:0;pthread_mutex_unlock(&lock);return n;}
READ(vm_arena_limit,limit) READ(vm_arena_used,used) READ(vm_arena_peak,peak) READ(vm_arena_denied,denied)
void vm_arena_diagnostic_snapshot(struct vm_arena_diagnostic *out){
 if(!out)return;pthread_mutex_lock(&lock);*out=diagnostic;
 out->limit=active?limit:0;out->used=active?used:0;out->peak=active?peak:0;out->denied=active?denied:0;
 out->ordinary_limit=active?ordinary_threshold():0;out->header_size=sizeof(block);
 pthread_mutex_unlock(&lock);
}
static void diagnostic_write_all(int fd,const char *text,size_t n){
 while(n){ssize_t count=write(fd,text,n);if(count<0&&errno==EINTR)continue;
  if(count<=0)break;text+=count;n-=(size_t)count;}
}
void vm_arena_diagnostic_write(int fd,const char *phase){
 int saved_errno=errno;struct vm_arena_diagnostic snap;vm_arena_diagnostic_snapshot(&snap);
 char line[1024];if(!phase)phase="unknown";
 int n=snprintf(line,sizeof line,"VM_ARENA_SNAPSHOT phase=%.64s used=%zu peak=%zu ordinary_limit=%zu limit=%zu denied=%zu header_size=%zu last_requested=%zu last_charge=%zu last_used=%zu last_threshold=%zu last_limit=%zu last_emergency=%u last_reason=%u last_caller=0x%llx used_kind=committed histogram_charge_kind=live_cell\n",phase,snap.used,snap.peak,snap.ordinary_limit,snap.limit,snap.denied,snap.header_size,snap.last_requested,snap.last_charge,snap.last_used,snap.last_threshold,snap.last_limit,snap.last_emergency,snap.last_reason,(unsigned long long)snap.last_caller);
 if(n>0)diagnostic_write_all(fd,line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
 static const char *labels[VM_ARENA_HISTOGRAM_BINS]={"0","1-8","9-32","33-128","129-512","513-4096","4097-65536","65537+"};
 for(unsigned i=0;i<VM_ARENA_HISTOGRAM_BINS;i++){
  const struct vm_arena_histogram_bin *b=&snap.bins[i];
  n=snprintf(line,sizeof line,"VM_ARENA_HIST phase=%.64s bin=%s allocations=%zu frees=%zu live_blocks=%zu live_requested=%zu live_charge=%zu peak_charge=%zu denied=%zu\n",phase,labels[i],b->allocations,b->frees,b->live_blocks,b->live_requested,b->live_charge,b->peak_charge,b->denied);
  if(n>0)diagnostic_write_all(fd,line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
 }
 errno=saved_errno;
}
bool vm_arena_region_snapshot(struct vm_arena_region_snapshot *out){
 if(!out)return false;pthread_mutex_lock(&lock);memset(out,0,sizeof *out);out->active=active;
 if(active){out->limit_bytes=limit;out->committed_bytes=used;out->peak_committed_bytes=peak;
  out->live_bytes=live_bytes;out->live_requested_bytes=live_requested;out->peak_live_bytes=peak_live;
  out->live_allocations=allocations;out->reuse_hits=reused;out->denied=denied;
  for(region *r=regions;r;r=r->next){out->regions++;out->humongous_regions+=r->humongous;
   out->metadata_bytes+=r->metadata_bytes;out->bitmap_bytes+=r->bitmap_bytes;
  }
  /* Every charged byte is metadata, a live span, or reusable capacity.
   * live_bytes is updated under this same lock on alloc/free/realloc; avoid
   * scanning every native allocation merely to obtain collection counters. */
  out->free_bytes=used-out->metadata_bytes-live_bytes;
  out->waste_bytes=live_bytes-live_requested;
 }
 pthread_mutex_unlock(&lock);return out->active;
}
bool vm_arena_locate(const void *pointer,size_t *region_id,bool *allocation_start){
 if(region_id)*region_id=0;if(allocation_start)*allocation_start=false;
 if(!pointer)return false;
 uintptr_t address=(uintptr_t)pointer;bool found=false;
 pthread_mutex_lock(&lock);
 for(region *r=regions;r;r=r->next){uintptr_t base=(uintptr_t)r->data;
  if(address<base||address-base>=r->capacity)continue;
  size_t offset=(size_t)(address-base);found=true;if(region_id)*region_id=r->id;
  if(allocation_start&&offset%ALIGNMENT==0){size_t bit=offset/ALIGNMENT;
   *allocation_start=(r->bitmap[bit/8]&(1u<<(bit%8)))!=0;}
  break;
 }
 pthread_mutex_unlock(&lock);return found;
}
bool vm_arena_object_start_set(const void *pointer,bool present){
 if(!pointer)return false;uintptr_t address=(uintptr_t)pointer;bool valid=false;
 pthread_mutex_lock(&lock);
 for(region *r=regions;r;r=r->next){uintptr_t base=(uintptr_t)r->data;
  if(address<base||address-base>=r->capacity)continue;
  size_t offset=(size_t)(address-base);if(offset%ALIGNMENT)break;
  if(!present)valid=true;
  else for(size_t at=0;at<r->bump;){block *b=(block *)(r->data+at);
   size_t payload=at+sizeof(block);
   if(b->h.allocated&&offset>=payload&&offset-payload<b->h.requested){valid=true;break;}
   if(offset<at+b->h.span)break;at+=b->h.span;
  }
  if(valid){size_t bit=offset/ALIGNMENT;unsigned char mask=(unsigned char)(1u<<(bit%8));
   if(present)r->object_bitmap[bit/8]|=mask;else r->object_bitmap[bit/8]&=(unsigned char)~mask;}
  break;
 }
 pthread_mutex_unlock(&lock);return valid;
}
bool vm_arena_object_start(const void *pointer){
 if(!pointer)return false;uintptr_t address=(uintptr_t)pointer;bool found=false;
 pthread_mutex_lock(&lock);
 for(region *r=regions;r;r=r->next){uintptr_t base=(uintptr_t)r->data;
  if(address<base||address-base>=r->capacity)continue;size_t offset=(size_t)(address-base);
  if(offset%ALIGNMENT==0){size_t bit=offset/ALIGNMENT;found=(r->object_bitmap[bit/8]&(1u<<(bit%8)))!=0;}break;
 }
 pthread_mutex_unlock(&lock);return found;
}
void vm_arena_end(void){
 pthread_mutex_lock(&lock);size_t residue=used,maximum=limit,high=peak,failures=denied,live=live_bytes;
 while(regions){region *r=regions;regions=r->next;free(r);}memset(free_bins,0,sizeof free_bins);
 used=live_bytes=live_requested=allocations=0;active=false;pthread_mutex_unlock(&lock);
 sdk_trace_fprintf(stderr,"VM_ARENA_END limit=%zu peak=%zu denied=%zu residual_reclaimed=%zu live_charge=%zu\n",maximum,high,failures,residue,live);
}
struct test_thread{bool okay;};
static void *test_worker(void *p){struct test_thread *t=p;t->okay=true;for(int i=0;i<1000;i++){
 char *s=vm_arena_malloc(32);if(!s){t->okay=false;break;}memset(s,17,32);vm_arena_free(s);}return NULL;}
bool vm_pressure_selftest_enabled(void){
 int saved=errno;bool enabled=false;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/gc.selftest.pressure","rb");
 /* Let device diagnostics edit an existing app-owned control file; shell
  * cannot create new entries in the app-private directory. Never overwrite
  * an existing setting, including a concurrently created explicit opt-in. */
 if(!f&&errno==ENOENT){FILE *initial=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/gc.selftest.pressure","wx");if(initial){fputs("0\n",initial);fclose(initial);}}
 if(f){char value[4]={0};size_t n=fread(value,1,sizeof value,f);
  enabled=!ferror(f)&&((n==1&&value[0]=='1')||(n==2&&!memcmp(value,"1\n",2))||(n==3&&!memcmp(value,"1\r\n",3)));
  fclose(f);
 }
 errno=saved;return enabled;
}
bool vm_pressure_selftest_gate(const char *name){
 int saved=errno;bool enabled=vm_pressure_selftest_enabled();
 sdk_trace_fprintf(stderr,"PRESSURE_SELFTEST_GATE pid=%d test=%s enabled=%d action=%s switch=gc.selftest.pressure\n",getpid(),name,enabled,enabled?"RUN":"SKIP");
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/pressure-selftest-gates.txt","a");
 if(f){fprintf(f,"PRESSURE_SELFTEST_GATE pid=%d test=%s enabled=%d action=%s switch=gc.selftest.pressure\n",getpid(),name,enabled,enabled?"RUN":"SKIP");fclose(f);}
 errno=saved;return enabled;
}
bool vm_arena_selftest(void){
 if(!vm_pressure_selftest_gate("vm_arena_selftest"))return true;
 void *outside=vm_arena_malloc(19);if(!outside)return false;
 bool ok=vm_arena_begin(8192);if(!ok){vm_arena_free(outside);return false;}
 char *anchor=vm_arena_malloc(16),*a=vm_arena_malloc(1024),*guard=vm_arena_malloc(16);
 ok=ok&&anchor&&a&&guard;struct vm_arena_region_snapshot snap={0};
 vm_arena_region_snapshot(&snap);ok=ok&&snap.regions==1&&snap.metadata_bytes>0&&snap.bitmap_bytes>0&&snap.committed_bytes<=8192;
 if(a){memset(a,29,1024);block *b=(block *)a-1;ok=ok&&b->h.owner&&bitmap_has(b->h.owner,b)&&((uintptr_t)a%ALIGNMENT==0);
  size_t rid=0,inside=0;bool start=false,interior=true;
  ok=ok&&vm_arena_locate(a,&rid,&start)&&rid&&start&&vm_arena_locate(a+1,&inside,&interior)&&inside==rid&&!interior;
  bool guest_set=vm_arena_object_start_set(a+ALIGNMENT,true);
  bool guest_query=vm_arena_object_start(a+ALIGNMENT);
  bool bad_alignment=vm_arena_object_start_set(a+1,true),bad_foreign=vm_arena_object_start_set(outside,true);
  bool guest_clear=vm_arena_object_start_set(a+ALIGNMENT,false);
  ok=ok&&guest_set&&guest_query&&!bad_alignment&&!bad_foreign&&guest_clear&&!vm_arena_object_start(a+ALIGNMENT);
  guest_set=vm_arena_object_start_set(a+ALIGNMENT,true);ok=ok&&guest_set;}
 size_t before=vm_arena_used();void *failed=vm_arena_realloc(a,8192);
 ok=ok&&!failed&&vm_arena_used()==before&&a&&a[0]==29&&a[1023]==29;
 void *overflow=vm_arena_calloc(SIZE_MAX,2);ok=ok&&!overflow;
 void *overflow_realloc=vm_arena_realloc(a,SIZE_MAX);ok=ok&&!overflow_realloc&&a&&a[1023]==29;
 struct vm_arena_diagnostic diag;vm_arena_diagnostic_snapshot(&diag);
 ok=ok&&diag.last_reason==3&&diag.last_requested==SIZE_MAX&&diag.used==vm_arena_used();
 vm_arena_free(a);size_t freed_region=0;bool freed_start=true;
 ok=ok&&vm_arena_locate(a,&freed_region,&freed_start)&&freed_region&&!freed_start&&!vm_arena_object_start(a+ALIGNMENT);
 char *again=vm_arena_malloc(1024);ok=ok&&again==a;
 if(again){memset(again,29,1024);ok=ok&&!vm_arena_object_start(again+ALIGNMENT);}
 char *grown=vm_arena_realloc(again,2048);ok=ok&&grown&&grown[1023]==29;if(grown)again=grown;
 if(again){bool set=vm_arena_object_start_set(again+2*ALIGNMENT,true);ok=ok&&set;}
 char *shrunk=vm_arena_realloc(again,31);ok=ok&&shrunk&&shrunk[30]==29;if(shrunk)again=shrunk;
 ok=ok&&!vm_arena_object_start(again+2*ALIGNMENT);
 char *left=vm_arena_malloc(64),*middle=vm_arena_malloc(64),*right=vm_arena_malloc(16);
 ok=ok&&left&&middle&&right;if(left)memset(left,41,64);
 vm_arena_free(middle);char *neighbor=vm_arena_realloc(left,160);
 ok=ok&&neighbor==left&&neighbor&&neighbor[63]==41;if(neighbor)left=neighbor;
 vm_arena_free(left);vm_arena_free(right);
 struct test_thread cases[2]={{0},{0}};pthread_t threads[2];
 int c0=pthread_create(&threads[0],NULL,test_worker,&cases[0]),c1=pthread_create(&threads[1],NULL,test_worker,&cases[1]);
 if(!c0)pthread_join(threads[0],NULL);if(!c1)pthread_join(threads[1],NULL);
 ok=ok&&!c0&&!c1&&cases[0].okay&&cases[1].okay;
 vm_arena_region_snapshot(&snap);ok=ok&&snap.reuse_hits>0&&snap.committed_bytes==snap.live_bytes+snap.free_bytes+snap.metadata_bytes;
 vm_arena_diagnostic_snapshot(&diag);size_t hist_charge=0,hist_requested=0,hist_blocks=0;
 for(unsigned i=0;i<VM_ARENA_HISTOGRAM_BINS;i++){hist_charge+=diag.bins[i].live_charge;hist_requested+=diag.bins[i].live_requested;hist_blocks+=diag.bins[i].live_blocks;}
 ok=ok&&hist_charge==snap.live_bytes&&hist_requested==snap.live_requested_bytes&&hist_blocks==snap.live_allocations&&diag.used==snap.committed_bytes;
 vm_arena_free(again);vm_arena_free(guard);vm_arena_free(anchor);vm_arena_free(outside);
 vm_arena_region_snapshot(&snap);ok=ok&&snap.live_bytes==0&&snap.live_allocations==0&&snap.committed_bytes==0&&vm_arena_peak()<=8192&&vm_arena_denied()>0;
 vm_arena_diagnostic_snapshot(&diag);
 for(unsigned i=0;i<VM_ARENA_HISTOGRAM_BINS;i++)ok=ok&&!diag.bins[i].live_blocks&&!diag.bins[i].live_charge&&!diag.bins[i].live_requested;
 vm_arena_end();
 bool second_arena=vm_arena_begin(8u*1024u*1024u);ok=ok&&second_arena;if(!second_arena)return false;
 void *huge=vm_arena_malloc(VM_ARENA_REGION_BYTES+64u);vm_arena_region_snapshot(&snap);
 ok=ok&&huge&&snap.humongous_regions==1&&snap.committed_bytes<=8u*1024u*1024u;
 vm_arena_free(huge);ok=ok&&vm_arena_used()==0;
 void *ordinary[16]={0};unsigned count=0;
 while(count<16){void *p=vm_arena_malloc(VM_ARENA_REGION_BYTES-sizeof(block));if(!p)break;ordinary[count++]=p;}
 ok=ok&&count>0&&count<16&&vm_arena_used()<=6u*1024u*1024u;
 vm_arena_emergency_enter();void *rescue=vm_arena_malloc(VM_ARENA_REGION_BYTES-sizeof(block));vm_arena_emergency_leave();
 ok=ok&&rescue&&vm_arena_used()<=8u*1024u*1024u;
 void *ordinary_denied=vm_arena_malloc(VM_ARENA_REGION_BYTES-sizeof(block));ok=ok&&!ordinary_denied;
 vm_arena_free(ordinary_denied);vm_arena_free(rescue);for(unsigned i=0;i<count;i++)vm_arena_free(ordinary[i]);
 ok=ok&&vm_arena_used()==0;vm_arena_end();
 sdk_trace_fprintf(stderr,"ARENA_SELFTEST %s candidate=region-arena-gc1 real_region_backing=1 bitmap=1 reuse=1 aligned=1 adjacent_realloc=1 failed_realloc_preserves_old=1 calloc_overflow=1 concurrent_allocator_threads=2 humongous=1 empty_region_release=1 physical_limit=1 emergency_reserve=1 outside_origin_free=1\n",ok?"PASS":"FAIL");return ok;
}
bool vm_arena_java_selftest(struct dvm *vm){
 if(!vm_pressure_selftest_gate("vm_arena_java_selftest"))return true;
 size_t before=vm_arena_denied();dvm_ref impossible=dvm_new_array(vm,'B',"B",VM_ARENA_LIMIT);
 bool denied_array=!impossible&&vm_arena_denied()>before&&vm_arena_used()<=vm_arena_limit();
 dvm__throw(vm,"java/lang/OutOfMemoryError","managed arena test: actual Java array exceeds remaining enforced cap");
 dvm_ref error=dvm_exception(vm);struct dvm_class *type=dvm_object_class(vm,error);
 bool exception=error&&type&&!strcmp(type->name,"java/lang/OutOfMemoryError");dvm_clear_exception(vm);
 sdk_trace_fprintf(stderr,"JAVA_ARENA_SELFTEST %s marker=region-arena-gc1 actual_VM_array_denied=%d oom_constructed=%d\n",denied_array&&exception?"PASS":"FAIL",denied_array,exception);return denied_array&&exception;
}
