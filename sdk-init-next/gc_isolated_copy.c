#include "gc_isolated_copy.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdalign.h>
#include <time.h>

#define GCI_MAGIC UINT64_C(0x474349434f505931)
#define GCI_CHUNK 256u
struct gci_object {
 uint64_t magic;
 struct gci_object *brooks;
 size_t bytes,nrefs,payload_bytes;
 struct gci_object *refs[];
};
struct space { unsigned char *data; size_t used,objects; };
struct gci_heap {
 size_t capacity,region_size;
 struct space active,to,old[2];
 int status;
 gci_object **roots;size_t root_count,root_index,scan,scan_ref;
 bool scan_header;
 /* At most one object copy in flight; its target is not exposed until all
  * bytes are copied. The source Brooks word is installed only at completion. */
 gci_object *copy_src,*copy_dst;size_t copy_offset;
 struct gci_stats stats;
};
static bool add_size(size_t a,size_t b,size_t *r){if(a>SIZE_MAX-b)return false;*r=a+b;return true;}
static bool object_size(size_t refs,size_t payload,size_t *r){
 size_t n;if(refs>SIZE_MAX/sizeof(gci_object *))return false;
 if(!add_size(sizeof(gci_object),refs*sizeof(gci_object *),&n)||!add_size(n,payload,&n))return false;
 size_t a=alignof(max_align_t);if(!add_size(n,a-1,&n))return false;*r=n/a*a;return true;
}
/* Validate an exact allocation boundary, never dereference an arbitrary input
 * before proving it belongs to one of our retained spaces. */
static bool member(const struct space *s,const gci_object *p){
 if(!s->data||!p)return false;uintptr_t b=(uintptr_t)s->data,x=(uintptr_t)p;
 if(x<b||x-b>=s->used)return false;
 size_t off=0;while(off<s->used){const gci_object *o=(const gci_object *)(s->data+off);
  if(o->magic!=GCI_MAGIC||o->bytes<sizeof *o||o->bytes>s->used-off)return false;
  if(off==x-b)return true;off+=o->bytes;
 }return false;
}
static bool owned(gci_heap *h,gci_object *p){return member(&h->active,p)||member(&h->to,p)||member(&h->old[0],p)||member(&h->old[1],p);}
static int fail(gci_heap *h,int status){h->status=status;return status;}
gci_heap *gci_create(size_t cap,size_t region){
 if(!cap||!region)return NULL;gci_heap *h=calloc(1,sizeof *h);if(!h)return NULL;
 h->active.data=calloc(1,cap);if(!h->active.data){free(h);return NULL;}
 h->capacity=cap;h->region_size=region;return h;
}
void gci_destroy(gci_heap *h){if(!h)return;free(h->active.data);free(h->to.data);free(h->old[0].data);free(h->old[1].data);free(h);}
gci_object *gci_resolve(gci_heap *h,gci_object *p){
 if(!h||!p)return NULL;
 for(unsigned i=0;i<5;i++){if(!owned(h,p)||p->magic!=GCI_MAGIC){fail(h,GCI_INVALID);return NULL;}
  if(p->brooks==p){if(member(&h->active,p)||member(&h->to,p))return p;fail(h,GCI_INVALID);return NULL;}p=p->brooks;
 }fail(h,GCI_INVALID);return NULL;
}
gci_object *gci_alloc(gci_heap *h,size_t refs,size_t payload){
 if(!h||h->status<0)return NULL;if(h->status==GCI_MORE)return NULL;
 size_t n;if(!object_size(refs,payload,&n)||n>h->capacity-h->active.used){fail(h,GCI_OOM);return NULL;}
 gci_object *o=(gci_object *)(h->active.data+h->active.used);memset(o,0,n);
 o->magic=GCI_MAGIC;o->brooks=o;o->bytes=n;o->nrefs=refs;o->payload_bytes=payload;
 h->active.used+=n;h->active.objects++;h->stats.allocated_objects++;h->stats.allocated_bytes+=n;
 h->stats.live_objects=h->active.objects;h->stats.live_bytes=h->active.used;return o;
}
bool gci_set_ref(gci_heap *h,gci_object *o,size_t i,gci_object *value){
 if(!h||h->status<0||h->status==GCI_MORE)return false;o=gci_resolve(h,o);if(!o||i>=o->nrefs)return false;
 if(value){value=gci_resolve(h,value);if(!value)return false;}o->refs[i]=value;return true;
}
gci_object *gci_get_ref(gci_heap *h,gci_object *o,size_t i){o=gci_resolve(h,o);if(!o||i>=o->nrefs)return NULL;return gci_resolve(h,o->refs[i]);}
void *gci_payload(gci_heap *h,gci_object *o,size_t *len){
 if(!h||h->status==GCI_MORE)return NULL;o=gci_resolve(h,o);if(!o)return NULL;if(len)*len=o->payload_bytes;return (unsigned char *)o->refs+o->nrefs*sizeof *o->refs;
}
size_t gci_region(gci_heap *h,gci_object *o){o=gci_resolve(h,o);if(!o)return SIZE_MAX;
 if(member(&h->active,o))return ((uintptr_t)o-(uintptr_t)h->active.data)/h->region_size;
 if(member(&h->to,o))return ((uintptr_t)o-(uintptr_t)h->to.data)/h->region_size;
 return SIZE_MAX;
}
bool gci_is_large(gci_heap *h,gci_object *o){o=gci_resolve(h,o);return o&&o->bytes>h->region_size;}
int gci_begin(gci_heap *h,gci_object **roots,size_t count){
 if(!h)return GCI_INVALID;if(h->status<0)return h->status;if(h->status==GCI_MORE)return GCI_BUSY;
 if(count&&!roots)return fail(h,GCI_INVALID);
 h->to.data=calloc(1,h->capacity);if(!h->to.data)return fail(h,GCI_OOM);
 h->roots=roots;h->root_count=count;h->root_index=h->scan=h->scan_ref=0;h->scan_header=false;h->status=GCI_MORE;return GCI_MORE;
}
/* Request copy; caller retries the same root/reference slot after chunks run. */
static int forward(gci_heap *h,gci_object **slot){
 if(!*slot)return 1;gci_object *o=gci_resolve(h,*slot);if(!o)return -1;
 if(member(&h->to,o)){*slot=o;return 1;}
 if(!member(&h->active,o)){fail(h,GCI_INVALID);return -1;}
 if(o->bytes>h->capacity-h->to.used){fail(h,GCI_OOM);return -1;}
 h->copy_src=o;h->copy_dst=(gci_object *)(h->to.data+h->to.used);h->copy_offset=0;return 0;
}
static void finish(gci_heap *h){
 size_t dead_objects=h->active.objects-h->to.objects,dead_bytes=h->active.used-h->to.used;
 free(h->old[1].data);h->old[1]=h->old[0];h->old[0]=h->active;h->active=h->to;memset(&h->to,0,sizeof h->to);
 h->stats.reclaimed_objects+=dead_objects;h->stats.reclaimed_bytes+=dead_bytes;
 h->stats.live_objects=h->active.objects;h->stats.live_bytes=h->active.used;h->stats.collections++;
 h->roots=NULL;h->root_count=0;h->status=GCI_DONE;
}
int gci_slice(gci_heap *h,size_t budget){
 if(!h)return GCI_INVALID;if(h->status!=GCI_MORE)return h->status;if(!budget)return GCI_INVALID;
 size_t work=0;while(work<budget&&h->status==GCI_MORE){
  if(h->copy_src){size_t n=h->copy_src->bytes-h->copy_offset;if(n>GCI_CHUNK)n=GCI_CHUNK;
   memcpy((unsigned char *)h->copy_dst+h->copy_offset,(unsigned char *)h->copy_src+h->copy_offset,n);h->copy_offset+=n;work++;
   if(h->copy_offset==h->copy_src->bytes){h->copy_dst->brooks=h->copy_dst;h->copy_src->brooks=h->copy_dst;
    h->to.used+=h->copy_dst->bytes;h->to.objects++;h->stats.copied_objects++;h->stats.copied_bytes+=h->copy_dst->bytes;h->copy_src=h->copy_dst=NULL;}
   continue;
  }
  if(h->root_index<h->root_count){int r=forward(h,&h->roots[h->root_index]);work++;if(r>0)h->root_index++;continue;}
  if(h->scan>=h->to.used){finish(h);break;}
  gci_object *o=(gci_object *)(h->to.data+h->scan);
  if(!h->scan_header){h->scan_header=true;work++;continue;}
  if(h->scan_ref<o->nrefs){int r=forward(h,&o->refs[h->scan_ref]);work++;if(r>0)h->scan_ref++;continue;}
  h->scan+=o->bytes;h->scan_ref=0;h->scan_header=false;
 }
 h->stats.steps+=work;h->stats.last_slice_work=work;if(work>h->stats.max_slice_work)h->stats.max_slice_work=work;return h->status;
}
int gci_status(const gci_heap *h){return h?h->status:GCI_INVALID;}
void gci_get_stats(const gci_heap *h,struct gci_stats *s){if(s){if(h){*s=h->stats;s->retained_forwarding_bytes=h->old[0].used+h->old[1].used;s->physical_space_bytes=h->capacity*((h->active.data!=NULL)+(h->to.data!=NULL)+(h->old[0].data!=NULL)+(h->old[1].data!=NULL));}else memset(s,0,sizeof *s);}}

static uint64_t wall_ns(void){struct timespec t;if(timespec_get(&t,TIME_UTC)!=TIME_UTC)return 0;return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;}
static bool run_copy(gci_heap *h,gci_object **r,size_t n,FILE *csv){
 if(gci_begin(h,r,n)!=GCI_MORE)return false;size_t watchdog=1000000;
 while(gci_status(h)==GCI_MORE&&watchdog--){
  size_t copied=h->stats.copied_bytes,reclaimed=h->stats.reclaimed_bytes;unsigned cycle=h->stats.collections+1;uint64_t begin=wall_ns();int status=gci_slice(h,3);uint64_t end=wall_ns();
  if(csv){struct gci_stats measured;gci_get_stats(h,&measured);fprintf(csv,"isolated-copy,%u,%zu,%llu,%zu,%zu,%zu,%zu,%zu,%d\n",cycle,1000000-watchdog,(unsigned long long)(end>=begin?end-begin:0),h->stats.last_slice_work,h->stats.copied_bytes-copied,h->stats.reclaimed_bytes-reclaimed,measured.retained_forwarding_bytes,measured.physical_space_bytes,status);}
  if(status<0||h->stats.last_slice_work>3)return false;
 }
 return gci_status(h)==GCI_DONE;
}
static bool selftest(FILE *f,FILE *csv){
 if(csv)fprintf(csv,"stage,cycle,slice,wall_ns,work_units,copied_bytes,logical_reclaimed_bytes,retained_forwarding_bytes,physical_space_bytes,status\n");
 bool ok=true;gci_heap *h=gci_create(32768,256);if(!h)return false;
#define CHECK(name,expr) do{bool pass=(expr);if(f)fprintf(f,"GC3_ISOLATED %s=%d\n",name,pass);ok=ok&&pass;}while(0)
 gci_object *a=gci_alloc(h,2,17),*b=gci_alloc(h,1,400),*c=gci_alloc(h,1,4096),*dead=gci_alloc(h,0,733);
 if(!a||!b||!c||!dead){gci_destroy(h);return false;}
 size_t dead_bytes=dead->bytes;memset(gci_payload(h,c,NULL),0xa7,4096);
 CHECK("depth3_cycle",gci_set_ref(h,a,0,b)&&gci_set_ref(h,b,0,c)&&gci_set_ref(h,c,0,a)&&gci_set_ref(h,a,1,c));
 CHECK("cross_region_large",gci_region(h,a)!=gci_region(h,c)&&gci_is_large(h,c));
 gci_object *roots[]={a};gci_object *first=a;CHECK("cheney_first_copy",run_copy(h,roots,1,csv));
 gci_object *second=roots[0];CHECK("brooks_idempotent",gci_resolve(h,first)==second&&gci_resolve(h,second)==second);
 CHECK("cycle_and_shared",gci_get_ref(h,gci_get_ref(h,gci_get_ref(h,second,0),0),0)==second&&gci_get_ref(h,second,1)==gci_get_ref(h,gci_get_ref(h,second,0),0));
 CHECK("dead_object_bytes",h->stats.reclaimed_objects==1&&h->stats.reclaimed_bytes==dead_bytes&&h->stats.live_objects==3);
 CHECK("cheney_second_copy",run_copy(h,roots,1,csv));
 CHECK("two_level_forwarding",first->brooks==second&&second->brooks==roots[0]&&gci_resolve(h,first)==roots[0]&&gci_resolve(h,gci_resolve(h,first))==roots[0]);
 unsigned char *p=gci_payload(h,gci_get_ref(h,roots[0],1),NULL);bool bytes=p!=NULL;for(size_t i=0;p&&i<4096;i++)if(p[i]!=0xa7)bytes=false;CHECK("large_payload_preserved",bytes);
 CHECK("slice_budget",h->stats.max_slice_work<=3&&h->stats.steps>10);
 CHECK("drop_all_reclaims",run_copy(h,NULL,0,csv)&&h->stats.live_objects==0&&h->stats.live_bytes==0&&h->stats.reclaimed_objects==4);
 gci_destroy(h);h=gci_create(128,64);if(!h)return false;CHECK("explicit_oom",gci_alloc(h,0,1024)==NULL&&gci_status(h)==GCI_OOM);gci_destroy(h);
#undef CHECK
 return ok;
}

bool gci_selftest(FILE *evidence){return selftest(evidence,NULL);}
int gc_isolated_selftest(FILE *evidence,FILE *csv){return selftest(evidence,csv)?0:1;}
