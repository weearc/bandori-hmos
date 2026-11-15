/* Allocation ceiling for one serialized diagnostic VM. Counters and intrusive
 * ownership list are protected by one mutex, including foreign host threads.
 * This does not make simultaneous interpreter entry safe. */
#include "next_arena.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include "dvm/dvm_internal.h"
int sdk_trace_fprintf(FILE *,const char *,...);
typedef union block block;
union block {max_align_t align;struct{size_t charge,requested;block *prev,*next;bool owned;} h;};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool active;
static size_t limit,used,peak,denied;
static block *head;
static _Thread_local unsigned emergency;
static struct vm_arena_diagnostic diagnostic;
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
static size_t threshold(void){size_t reserve=limit>4u*1024u*1024u?2u*1024u*1024u:limit/8;return emergency?limit:limit-reserve;}
static void allocation_denied(size_t requested,size_t charge,unsigned reason,uintptr_t caller){
 if(!active)return;denied++;diagnostic.bins[histogram_bin(requested)].denied++;
 diagnostic.last_requested=requested;diagnostic.last_charge=charge;diagnostic.last_used=used;
 diagnostic.last_threshold=threshold();diagnostic.last_limit=limit;diagnostic.last_emergency=emergency;
 diagnostic.last_reason=reason;diagnostic.last_caller=caller;
}
bool vm_arena_begin(size_t maximum){pthread_mutex_lock(&lock);bool ok=!active&&!head;if(ok){active=true;limit=maximum;used=peak=denied=0;memset(&diagnostic,0,sizeof diagnostic);}pthread_mutex_unlock(&lock);return ok;}
void vm_arena_emergency_enter(void){++emergency;}
void vm_arena_emergency_leave(void){if(emergency)--emergency;}
void *vm_arena_malloc(size_t n){
 if(n>SIZE_MAX-sizeof(block)){pthread_mutex_lock(&lock);allocation_denied(n,SIZE_MAX,3,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);errno=ENOMEM;return NULL;}
 size_t charge=sizeof(block)+(n?n:1);pthread_mutex_lock(&lock);
 if(active&&(used>threshold()||charge>threshold()-used)){allocation_denied(n,charge,1,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);errno=ENOMEM;return NULL;}
 block *b=malloc(charge);if(!b){allocation_denied(n,charge,2,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);return NULL;}
 b->h.charge=charge;b->h.requested=n;b->h.owned=active;b->h.prev=NULL;b->h.next=NULL;
 if(active){b->h.next=head;if(head)head->h.prev=b;head=b;used+=charge;if(used>peak)peak=used;allocation_added(n,charge);}
 pthread_mutex_unlock(&lock);return b+1;
}
void vm_arena_free(void *p){if(!p)return;block *b=(block *)p-1;pthread_mutex_lock(&lock);if(b->h.owned){if(b->h.prev)b->h.prev->h.next=b->h.next;else head=b->h.next;if(b->h.next)b->h.next->h.prev=b->h.prev;used-=b->h.charge;allocation_removed(b->h.requested,b->h.charge);}free(b);pthread_mutex_unlock(&lock);}
void *vm_arena_calloc(size_t n,size_t size){if(size&&n>SIZE_MAX/size){pthread_mutex_lock(&lock);allocation_denied(SIZE_MAX,SIZE_MAX,3,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);errno=ENOMEM;return NULL;}size_t bytes=n*size;void *p=vm_arena_malloc(bytes);if(p)memset(p,0,bytes);return p;}
void *vm_arena_realloc(void *p,size_t n){
 if(!p)return vm_arena_malloc(n);if(!n){vm_arena_free(p);return NULL;}if(n>SIZE_MAX-sizeof(block)){pthread_mutex_lock(&lock);allocation_denied(n,SIZE_MAX,3,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);errno=ENOMEM;return NULL;}
 pthread_mutex_lock(&lock);block *old=(block *)p-1;size_t charge=sizeof(block)+n;
 if(old->h.owned){size_t base=used-old->h.charge;if(base>threshold()||charge>threshold()-base){allocation_denied(n,charge,1,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);errno=ENOMEM;return NULL;}}
 block *prev=old->h.prev,*next=old->h.next;size_t original=old->h.charge,original_requested=old->h.requested;bool owned=old->h.owned;
 block *b=realloc(old,charge);if(!b){if(owned)allocation_denied(n,charge,2,(uintptr_t)__builtin_return_address(0));pthread_mutex_unlock(&lock);return NULL;}
 b->h.charge=charge;b->h.requested=n;if(owned){if(prev)prev->h.next=b;else head=b;if(next)next->h.prev=b;used=used-original+charge;if(used>peak)peak=used;allocation_removed(original_requested,original);allocation_added(n,charge);}
 pthread_mutex_unlock(&lock);return b+1;
}
char *vm_arena_strdup(const char *s){size_t n=strlen(s);char *p=vm_arena_malloc(n+1);if(p)memcpy(p,s,n+1);return p;}
char *vm_arena_strndup(const char *s,size_t max){size_t n=strnlen(s,max);char *p=vm_arena_malloc(n+1);if(p){memcpy(p,s,n);p[n]=0;}return p;}
#define READ(name,var) size_t name(void){pthread_mutex_lock(&lock);size_t n=active?var:0;pthread_mutex_unlock(&lock);return n;}
READ(vm_arena_limit,limit) READ(vm_arena_used,used) READ(vm_arena_peak,peak) READ(vm_arena_denied,denied)
void vm_arena_diagnostic_snapshot(struct vm_arena_diagnostic *out){
 if(!out)return;pthread_mutex_lock(&lock);*out=diagnostic;
 out->limit=active?limit:0;out->used=active?used:0;out->peak=active?peak:0;out->denied=active?denied:0;
 size_t reserve=limit>4u*1024u*1024u?2u*1024u*1024u:limit/8;
 out->ordinary_limit=active?limit-reserve:0;out->header_size=sizeof(block);pthread_mutex_unlock(&lock);
}
static void diagnostic_write_all(int fd,const char *text,size_t n){
 while(n){ssize_t count=write(fd,text,n);if(count<0&&errno==EINTR)continue;if(count<=0)break;text+=count;n-=(size_t)count;}
}
void vm_arena_diagnostic_write(int fd,const char *phase){
 if(fd<0)return;int saved_errno=errno;struct vm_arena_diagnostic d;vm_arena_diagnostic_snapshot(&d);
 /* Stack storage only. Formatting and IO are strictly outside allocator lock. */
 char text[1024];int n=snprintf(text,sizeof text,"VM_ARENA_SNAPSHOT phase=%.64s used=%zu peak=%zu ordinary_limit=%zu limit=%zu denied=%zu header_size=%zu last_requested=%zu last_charge=%zu last_used=%zu last_threshold=%zu last_limit=%zu last_emergency=%u last_reason=%u last_caller=0x%llx\n",
  phase?phase:"unknown",d.used,d.peak,d.ordinary_limit,d.limit,d.denied,d.header_size,d.last_requested,d.last_charge,d.last_used,d.last_threshold,d.last_limit,d.last_emergency,d.last_reason,(unsigned long long)d.last_caller);
 if(n>0)diagnostic_write_all(fd,text,(size_t)n<sizeof text?(size_t)n:sizeof text-1);
 static const char *labels[VM_ARENA_HISTOGRAM_BINS]={"0","1-8","9-32","33-128","129-512","513-4096","4097-65536","65537+"};
 for(unsigned i=0;i<VM_ARENA_HISTOGRAM_BINS;i++){
  struct vm_arena_histogram_bin *b=&d.bins[i];
  n=snprintf(text,sizeof text,"VM_ARENA_HIST phase=%.64s bin=%s allocations=%zu frees=%zu live_blocks=%zu live_requested=%zu live_charge=%zu peak_charge=%zu denied=%zu\n",phase?phase:"unknown",labels[i],b->allocations,b->frees,b->live_blocks,b->live_requested,b->live_charge,b->peak_charge,b->denied);
  if(n>0)diagnostic_write_all(fd,text,(size_t)n<sizeof text?(size_t)n:sizeof text-1);
 }
 errno=saved_errno;
}
void vm_arena_end(void){
 pthread_mutex_lock(&lock);size_t residue=used,maximum=limit,high=peak,failures=denied;
 /* VM teardown owns remaining arena allocations; no Java worker can exist in
  * this candidate after thread_runtime_shutdown has joined them. Reclaim unreachable data
  * only after dvm_destroy has released all object payloads and descriptors. */
 while(head){block *b=head;head=b->h.next;allocation_removed(b->h.requested,b->h.charge);free(b);}used=0;active=false;
 pthread_mutex_unlock(&lock);
 sdk_trace_fprintf(stderr,"VM_ARENA_END limit=%zu peak=%zu denied=%zu residual_reclaimed=%zu\n",maximum,high,failures,residue);
}
struct test_thread{bool okay;};
static void *test_worker(void *p){struct test_thread *t=p;t->okay=true;for(int i=0;i<1000;++i){char *s=vm_arena_malloc(32);if(!s){t->okay=false;break;}memset(s,17,32);vm_arena_free(s);}return NULL;}
bool vm_arena_selftest(void){
 bool ok=vm_arena_begin(8192);if(!ok)return false;char *a=vm_arena_malloc(1024);ok=ok&&a;size_t before=vm_arena_used();if(a)memset(a,29,1024);
 void *failed=vm_arena_realloc(a,8192);ok=ok&&!failed&&vm_arena_used()==before&&a&&a[0]==29&&a[1023]==29;
  void *overflow=vm_arena_calloc(SIZE_MAX,2);ok=ok&&!overflow;
  struct vm_arena_diagnostic ds;vm_arena_diagnostic_snapshot(&ds);
  ok=ok&&ds.denied>=2&&ds.last_reason==3&&ds.last_requested==SIZE_MAX&&ds.used==before;
 char *grown=vm_arena_realloc(a,2048);ok=ok&&grown&&grown[1023]==29;if(grown)a=grown;
 struct test_thread cases[2]={{0},{0}};pthread_t threads[2];int c0=pthread_create(&threads[0],NULL,test_worker,&cases[0]),c1=pthread_create(&threads[1],NULL,test_worker,&cases[1]);if(!c0)pthread_join(threads[0],NULL);if(!c1)pthread_join(threads[1],NULL);ok=ok&&!c0&&!c1&&cases[0].okay&&cases[1].okay;
  vm_arena_diagnostic_snapshot(&ds);size_t histogram_used=0;for(unsigned i=0;i<VM_ARENA_HISTOGRAM_BINS;i++)histogram_used+=ds.bins[i].live_charge;
  ok=ok&&histogram_used==vm_arena_used()&&ds.bins[5].live_blocks==1;
  vm_arena_free(a);vm_arena_diagnostic_snapshot(&ds);for(unsigned i=0;i<VM_ARENA_HISTOGRAM_BINS;i++)ok=ok&&ds.bins[i].live_blocks==0&&ds.bins[i].live_charge==0&&ds.bins[i].live_requested==0;
  ok=ok&&vm_arena_used()==0&&vm_arena_peak()<=8192&&vm_arena_denied()>0;vm_arena_end();
 sdk_trace_fprintf(stderr,"ARENA_SELFTEST %s candidate=real-thread-v9 failed_realloc_preserves_old=1 calloc_overflow=1 concurrent_allocator_threads=2\n",ok?"PASS":"FAIL");return ok;
}
bool vm_arena_java_selftest(struct dvm *vm){
 size_t before=vm_arena_denied();
 dvm_ref impossible=dvm_new_array(vm,'B',"B",VM_ARENA_LIMIT);
 bool denied_array=!impossible&&vm_arena_denied()>before&&vm_arena_used()<=vm_arena_limit();
 dvm__throw(vm,"java/lang/OutOfMemoryError","managed arena test: actual Java array exceeds remaining enforced cap");
 dvm_ref error=dvm_exception(vm);struct dvm_class *type=dvm_object_class(vm,error);
 bool exception=error&&type&&!strcmp(type->name,"java/lang/OutOfMemoryError");
 dvm_clear_exception(vm);
 sdk_trace_fprintf(stderr,"JAVA_ARENA_SELFTEST %s marker=real-thread-v9 actual_VM_array_denied=%d oom_constructed=%d\n",denied_array&&exception?"PASS":"FAIL",denied_array,exception);
 return denied_array&&exception;
}
