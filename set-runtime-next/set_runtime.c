#include "set_runtime.h"
#include "collections.h"
#include "java_basics.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
extern bool list_views_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
typedef struct {struct dvm *vm;dvm_ref map,source;dvm_ref *items;uint32_t size,cap,mod;} set;
typedef struct {struct dvm *vm;dvm_ref owner;uint32_t index,expected;} set_cursor;
static void release(void *p){set *s=p;if(s){if(s->map)dvm_unpin(s->vm,s->map);if(s->source)dvm_unpin(s->vm,s->source);free(s->items);free(s);}}
static void release_cursor(void *p){set_cursor *i=p;if(i){dvm_unpin(i->vm,i->owner);free(i);}}
/* Exact native payload/capacity bytes. Referenced maps are counted once by
 * their owning collection module, never recursively through the set. */
size_t set_runtime_storage_bytes(struct dvm *v){
 size_t bytes=0;for(dvm_ref r=1;r<=v->heap_size;r++){struct dvm_object *o=dvm__obj(v,r);if(!o||!o->data)continue;
  if(o->data_destroy==release){set *s=o->data;bytes+=sizeof *s+(size_t)s->cap*sizeof *s->items;}
  else if(o->data_destroy==release_cursor)bytes+=sizeof(set_cursor);
 }return bytes;
}
static bool append(struct dvm *v,set *s,dvm_ref r){if(s->size==s->cap){uint32_t cap=s->cap?s->cap*2:8;dvm_ref *p=realloc(s->items,(size_t)cap*sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","set elements");return false;}s->items=p;s->cap=cap;}s->items[s->size++]=r;s->mod++;return true;}
static bool add_item(struct dvm *v,set *s,dvm_ref r,union dvm_value *out){union dvm_value key={.l=r},found={0};if(!collections_call(v,"java/util/Map","containsKey","(Ljava/lang/Object;)Z",s->map,&key,1,&found)||dvm_exception(v))return true;if(found.i){out->i=0;return true;}if(s->size==s->cap){uint32_t cap=s->cap?s->cap*2:8;dvm_ref *p=realloc(s->items,(size_t)cap*sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","set elements");return true;}s->items=p;s->cap=cap;}union dvm_value pair[2]={{.l=r},{.l=r}},ignored={0};collections_call(v,"java/util/Map","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",s->map,pair,2,&ignored);if(dvm_exception(v))return true;append(v,s,r);out->i=1;return true;}
static bool source_call(struct dvm *v,dvm_ref src,const char *m,const char *sig,const union dvm_value *a,int n,union dvm_value *out){struct dvm_object *o=dvm__obj(v,src);if(!o||!o->cls)return false;if(o->data_destroy==release)return set_runtime_call(v,o->cls->name,m,sig,src,a,n,out);if(list_views_call(v,o->cls->name,m,sig,src,a,n,out))return true;if(collections_call(v,o->cls->name,m,sig,src,a,n,out))return true;struct dvm_method *method=dvm_find_method(v,o->cls,m,sig);return method?dvm_call(v,method,src,a,n,out):false;}
static bool copy_iterator(struct dvm *v,set *s,dvm_ref iterator){for(;;){union dvm_value more={0},item={0},ignored={0};bool ok=source_call(v,iterator,"hasNext","()Z",NULL,0,&more);if(dvm_exception(v))return true;if(!ok){dvm__throw(v,"java/lang/UnsupportedOperationException","Collection iterator hasNext");return true;}if(!more.i)break;ok=source_call(v,iterator,"next","()Ljava/lang/Object;",NULL,0,&item);if(dvm_exception(v))return true;if(!ok){dvm__throw(v,"java/lang/UnsupportedOperationException","Collection iterator next");return true;}add_item(v,s,item.l,&ignored);if(dvm_exception(v))return true;}return true;}
static bool copy_source(struct dvm *v,set *s,dvm_ref src){if(!src){dvm__throw(v,"java/lang/NullPointerException","set source Collection");return true;}union dvm_value it={0};bool ok=source_call(v,src,"iterator","()Ljava/util/Iterator;",NULL,0,&it);if(dvm_exception(v))return true;if(!ok||!it.l){dvm__throw(v,"java/lang/UnsupportedOperationException","Collection.iterator");return true;}return copy_iterator(v,s,it.l);}
static set *base_set(struct dvm *v,dvm_ref r){for(int depth=0;depth<64;depth++){struct dvm_object *o=r?dvm__obj(v,r):NULL;set *s=o&&o->data_destroy==release?o->data:NULL;if(!s)return NULL;if(!s->source)return s;r=s->source;}dvm__throw(v,"java/lang/IllegalStateException","set view cycle");return NULL;}
void set_runtime_vm_start(struct dvm *v){
 struct dvm_class *iface=dvm_find_class_or_external(v,"java/util/Set"),*collection=dvm_find_class_or_external(v,"java/util/Collection");
 iface->is_interface=true;iface->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;
 if(!iface->ifaces){iface->ifaces=calloc(1,sizeof *iface->ifaces);if(iface->ifaces){iface->ifaces[0]=collection;iface->nifaces=1;}}
 struct dvm_class *base=dvm_find_class_or_external(v,"java/util/HashSet"),*linked=dvm_find_class_or_external(v,"java/util/LinkedHashSet");
 base->super=dvm_find_class_or_external(v,"java/lang/Object");linked->super=base;
 if(!base->ifaces){base->ifaces=calloc(1,sizeof *base->ifaces);if(base->ifaces){base->ifaces[0]=iface;base->nifaces=1;}}
 struct dvm_class *it=dvm_find_class_or_external(v,"java/util/HashSet$Itr");it->super=dvm_find_class_or_external(v,"java/lang/Object");if(!it->ifaces){it->ifaces=calloc(1,sizeof *it->ifaces);if(it->ifaces){it->ifaces[0]=dvm_find_class_or_external(v,"java/util/Iterator");it->nifaces=1;}}
 struct dvm_class *ro=dvm_find_class_or_external(v,"java/util/Collections$UnmodifiableSet");ro->super=dvm_find_class_or_external(v,"java/lang/Object");if(!ro->ifaces){ro->ifaces=calloc(1,sizeof *ro->ifaces);if(ro->ifaces){ro->ifaces[0]=iface;ro->nifaces=1;}}
}
#define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))
bool set_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!strcmp(c,"java/util/Collections")&&IS("unmodifiableSet","(Ljava/util/Set;)Ljava/util/Set;")&&n==1){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","unmodifiableSet source");return true;}struct dvm_object *src=dvm__obj(v,a[0].l);if(!src||src->data_destroy!=release)return false;out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/Collections$UnmodifiableSet"));struct dvm_object *dst=dvm__obj(v,out->l);set *view=calloc(1,sizeof *view);if(!dst||!view){free(view);dvm__throw(v,"java/lang/OutOfMemoryError","unmodifiableSet");return true;}view->vm=v;view->source=a[0].l;dvm_pin(v,view->source);dst->data=view;dst->data_destroy=release;return true;}
 struct dvm_object *o=self?dvm__obj(v,self):NULL;if(!o||!o->cls)return false;
 if(!strcmp(o->cls->name,"java/util/HashSet$Itr")){if(!IS("hasNext","()Z")&&!IS("next","()Ljava/lang/Object;")&&!IS("remove","()V"))return false;set_cursor *it=o->data_destroy==release_cursor?o->data:NULL;set *s=it?base_set(v,it->owner):NULL;if(!s){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","set iterator");return true;}if(it->expected!=s->mod){dvm__throw(v,"java/util/ConcurrentModificationException","set modified");return true;}if(IS("hasNext","()Z")){out->i=it->index<s->size;return true;}if(IS("next","()Ljava/lang/Object;")){if(it->index>=s->size){dvm__throw(v,"java/util/NoSuchElementException","set iterator");return true;}out->l=s->items[it->index++];return true;}dvm__throw(v,"java/lang/UnsupportedOperationException","set iterator remove");return true;}
 bool type=false;
 for(struct dvm_class *k=o->cls;k;k=k->super)if(!strcmp(k->name,"java/util/HashSet")||!strcmp(k->name,"java/util/Collections$UnmodifiableSet"))type=true;if(!type)return false;
 if((IS("<init>","()V")&&!n)||(IS("<init>","(Ljava/util/Collection;)V")&&n==1)||(IS("<init>","(I)V")&&n==1)){
  /* Capacity is a sizing hint; storage grows on demand, including capacity zero. */
  if(IS("<init>","(I)V")&&a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative set capacity");return true;}
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","set already initialized");return true;}
  set *s=calloc(1,sizeof *s);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","set state");return true;}
  s->vm=v;s->map=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashMap"));union dvm_value ignored={0};
  if(!s->map||!collections_call(v,"java/util/HashMap","<init>","()V",s->map,NULL,0,&ignored)||dvm_exception(v)){free(s);if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","set map");return true;}
  dvm_pin(v,s->map);o->data=s;o->data_destroy=release;if(IS("<init>","(Ljava/util/Collection;)V"))copy_source(v,s,a[0].l);return true;
 }
 if(o->data_destroy!=release)return false;set *view=o->data,*s=base_set(v,self);if(!s)return dvm_exception(v)?true:false;
 if(IS("add","(Ljava/lang/Object;)Z")||IS("remove","(Ljava/lang/Object;)Z")){
  if(view->source){dvm__throw(v,"java/lang/UnsupportedOperationException","unmodifiable set");return true;}
  union dvm_value found={0};if(!collections_call(v,"java/util/Map","containsKey","(Ljava/lang/Object;)Z",s->map,a,1,&found)||dvm_exception(v))return true;
  if(IS("add","(Ljava/lang/Object;)Z"))return add_item(v,s,a[0].l,out);
  union dvm_value ignored={0};collections_call(v,"java/util/Map","remove","(Ljava/lang/Object;)Ljava/lang/Object;",s->map,a,1,&ignored);out->i=found.i;if(found.i&&!dvm_exception(v)){for(uint32_t i=0;i<s->size;i++){bool same=java_basics_equal(v,s->items[i],a[0].l);if(dvm_exception(v))return true;if(same){memmove(s->items+i,s->items+i+1,(size_t)(s->size-i-1)*sizeof(dvm_ref));s->size--;s->mod++;break;}}}return true;
 }
 if(IS("iterator","()Ljava/util/Iterator;")){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashSet$Itr"));struct dvm_object *io=dvm__obj(v,out->l);set_cursor *it=calloc(1,sizeof *it);if(!io||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","set iterator");return true;}it->vm=v;it->owner=self;it->expected=s->mod;dvm_pin(v,self);io->data=it;io->data_destroy=release_cursor;return true;}
 if(IS("toArray","()[Ljava/lang/Object;")){out->l=dvm_new_array(v,'L',"Ljava/lang/Object;",s->size);if(!out->l){dvm__throw(v,"java/lang/OutOfMemoryError","set array");return true;}memcpy(dvm_array_data(v,out->l),s->items,(size_t)s->size*sizeof(dvm_ref));return true;}
 if(IS("contains","(Ljava/lang/Object;)Z"))return collections_call(v,"java/util/Map","containsKey",sig,s->map,a,n,out);
 if(IS("size","()I")){out->i=s->size;return true;}if(IS("isEmpty","()Z")){out->i=!s->size;return true;}if(IS("clear","()V")){if(view->source){dvm__throw(v,"java/lang/UnsupportedOperationException","unmodifiable set");return true;}bool had=s->size!=0;bool ok=collections_call(v,"java/util/Map",m,sig,s->map,a,n,out);if(ok&&!dvm_exception(v)&&had){s->size=0;s->mod++;}return ok;}
 return false;
}
bool set_runtime_selftest(struct dvm *v){
 for(int capacity=0;capacity<=16;capacity+=16){
  dvm_ref sized=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/LinkedHashSet"));union dvm_value hint={.i=capacity},got={0};
  if(!set_runtime_call(v,"java/util/LinkedHashSet","<init>","(I)V",sized,&hint,1,&got)||dvm_exception(v))return false;
  dvm_ref ordered[]={dvm_new_string(v,"capacity-first"),dvm_new_string(v,"capacity-second")};
  for(int i=0;i<3;i++){union dvm_value item={.l=ordered[i%2]};if(!set_runtime_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",sized,&item,1,&got)||got.i!=(i<2)||dvm_exception(v))return false;}
  if(!set_runtime_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",sized,NULL,0,&got)||!got.l)return false;dvm_ref cursor=got.l;
  for(int i=0;i<2;i++)if(!set_runtime_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",cursor,NULL,0,&got)||got.l!=ordered[i])return false;
  if(!set_runtime_call(v,"java/util/Iterator","hasNext","()Z",cursor,NULL,0,&got)||got.i)return false;
 }
 dvm_ref invalid=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/LinkedHashSet"));union dvm_value negative={.i=-1},ignored={0};
 if(!set_runtime_call(v,"java/util/LinkedHashSet","<init>","(I)V",invalid,&negative,1,&ignored)||!dvm_exception(v))return false;dvm_clear_exception(v);
 if(dvm__obj(v,invalid)->data)return false;
 dvm_ref src=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/ArrayList"));union dvm_value tmp={0},arg={0};if(!collections_call(v,"java/util/ArrayList","<init>","()V",src,NULL,0,&tmp))return false;
 dvm_ref first=dvm_new_string(v,"first"),second=dvm_new_string(v,"second"),third=dvm_new_string(v,"third"),values[]={first,second,first,0};for(int i=0;i<4;i++){arg.l=values[i];if(!collections_call(v,"java/util/List","add","(Ljava/lang/Object;)Z",src,&arg,1,&tmp))return false;}
 dvm_ref copy=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/LinkedHashSet"));arg.l=src;if(!set_runtime_call(v,"java/util/LinkedHashSet","<init>","(Ljava/util/Collection;)V",copy,&arg,1,&tmp)||dvm_exception(v))return false;
 if(!set_runtime_call(v,"java/util/Set","size","()I",copy,NULL,0,&tmp)||tmp.i!=3)return false;arg.l=third;collections_call(v,"java/util/List","add","(Ljava/lang/Object;)Z",src,&arg,1,&tmp);if(!set_runtime_call(v,"java/util/Set","size","()I",copy,NULL,0,&tmp)||tmp.i!=3)return false;
 arg.l=copy;if(!set_runtime_call(v,"java/util/Collections","unmodifiableSet","(Ljava/util/Set;)Ljava/util/Set;",0,&arg,1,&tmp)||!tmp.l)return false;dvm_ref ro=tmp.l;arg.l=third;if(!set_runtime_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",copy,&arg,1,&tmp)||!tmp.i)return false;if(!set_runtime_call(v,"java/util/Set","size","()I",ro,NULL,0,&tmp)||tmp.i!=4)return false;
 arg.l=ro;if(!set_runtime_call(v,"java/util/Collections","unmodifiableSet","(Ljava/util/Set;)Ljava/util/Set;",0,&arg,1,&tmp)||!tmp.l)return false;dvm_ref nested=tmp.l;if(!set_runtime_call(v,"java/util/Set","size","()I",nested,NULL,0,&tmp)||tmp.i!=4)return false;
 if(!set_runtime_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",ro,NULL,0,&tmp)||!tmp.l)return false;dvm_ref iter=tmp.l,expected[]={first,second,0,third};for(int i=0;i<4;i++)if(!set_runtime_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",iter,NULL,0,&tmp)||tmp.l!=expected[i])return false;
 arg.l=first;if(!set_runtime_call(v,"java/util/Set","remove","(Ljava/lang/Object;)Z",ro,&arg,1,&tmp)||!dvm_exception(v))return false;dvm_clear_exception(v);if(!set_runtime_call(v,"java/util/Iterator","remove","()V",iter,NULL,0,&tmp)||!dvm_exception(v))return false;dvm_clear_exception(v);
 if(!set_runtime_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",nested,NULL,0,&tmp)||!tmp.l)return false;if(!set_runtime_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",tmp.l,NULL,0,&tmp)||tmp.l!=first)return false;
 dvm_ref broken=dvm_new_object(v,dvm_find_class_or_external(v,"test/MissingHasNextIterator"));set scratch={0};if(!broken||!copy_iterator(v,&scratch,broken)||!dvm_exception(v))return false;dvm_clear_exception(v);
 dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/LinkedHashSet"));union dvm_value a={.l=dvm_new_string(v,"route")},out={0};
 if(!set_runtime_call(v,"java/util/LinkedHashSet","<init>","()V",r,NULL,0,&out)||dvm_exception(v))return false;
 if(!set_runtime_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",r,&a,1,&out)||!out.i)return false;
 if(!set_runtime_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",r,&a,1,&out)||out.i)return false;
 if(!set_runtime_call(v,"java/util/Set","contains","(Ljava/lang/Object;)Z",r,&a,1,&out)||!out.i)return false;
 if(!set_runtime_call(v,"java/util/Set","remove","(Ljava/lang/Object;)Z",r,&a,1,&out)||!out.i)return false;
 a.l=0;if(!set_runtime_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",r,&a,1,&out)||!out.i)return false;
 if(!set_runtime_call(v,"java/util/Set","contains","(Ljava/lang/Object;)Z",r,&a,1,&out)||!out.i)return false;
 sdk_trace_fprintf(stderr,"SET_RUNTIME_SELFTEST PASS duplicate_rejected=1 contains_remove=1 null_element=1 collection_copy=1 readonly_backed=1 ordered_iterator=1\n");return !dvm_exception(v);
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int set_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;if(o->data_destroy==release){set *p=o->data;if(p->size>p->cap||(!p->items&&p->size))return DVM_GC_PAYLOAD_INVALID;
 if(visit){visit(&p->map,user);visit(&p->source,user);for(uint32_t i=0;i<p->size;i++)visit(&p->items[i],user);}return DVM_GC_PAYLOAD_REFS;}
 if(o->data_destroy==release_cursor){if(visit)visit(&((set_cursor*)o->data)->owner,user);return DVM_GC_PAYLOAD_REFS;}return DVM_GC_PAYLOAD_UNKNOWN;
}
