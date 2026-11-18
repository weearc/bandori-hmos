/* MPL-2.0 derivative: the storage and basic operations below are extracted
 * from pinned Lunaria dvm_runtime.c. See collections-provenance.json and the
 * complete generated source/license. Dispatch, bounds/OOM fixes, constructors,
 * bulk snapshots and fail-fast iterator adaptation are local additions. */
#include "collections.h"
#include "gc_operation.h"
#include "java_basics.h"
#include "portable.h"
#include <stdlib.h>
#include <strings.h>
#include <math.h>
#define ARG(i) args[i]
#define RETI(x) do{out->i=(x);return true;}while(0)
#define RETL(x) do{out->l=(x);return true;}while(0)
#define RETV() return true
#define fprintf sdk_trace_fprintf
/* The shared generated storage declares this payload destructor by name. */
static void list_gc_destroy(void *p){free(p);}
#include "upstream_collections.inc"

/* Check the ordering contract used by the LinkedHashMap dispatcher. */
bool collections_ordered_map_selftest(struct dvm *v){
 const char *c="java/util/LinkedHashMap";dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,c));union dvm_value out={0},a[2]={{0},{0}};
 if(!r||!collections_call(v,c,"<init>","()V",r,NULL,0,&out))return false;
 dvm_ref first=dvm_new_string(v,"first-key"),second=dvm_new_string(v,"second-key"),value=dvm_new_string(v,"value");
 a[0].l=first;a[1].l=value;if(!collections_call(v,c,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out))return false;
 a[0].l=second;if(!collections_call(v,c,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out))return false;
 a[0].l=first;a[1].l=second;if(!collections_call(v,c,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out)||out.l!=value)return false;
 struct rt_map *p=dvm__obj(v,r)->data;if(!p||p->size!=2||p->e[0].k!=first||p->e[1].k!=second)return false;
 if(!collections_call(v,c,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",r,a,1,&out)||out.l!=second)return false;
 if(!collections_call(v,c,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out))return false;
 p=dvm__obj(v,r)->data;if(p->size!=2||p->e[0].k!=second||p->e[1].k!=first)return false;
 a[0].l=0;a[1].l=0;if(!collections_call(v,c,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out))return false;
 if(!collections_call(v,c,"containsKey","(Ljava/lang/Object;)Z",r,a,1,&out)||!out.i)return false;
 sdk_trace_fprintf(stderr,"ORDERED_MAP_SELFTEST PASS update_keeps_order=1 reinsert_appends=1 null_mapping=1\n");return !dvm_exception(v);
}

static bool is_type(struct dvm_class *c,const char *name){for(;c;c=c->super)if(!strcmp(c->name,name))return true;return false;}
static struct dvm_class *metadata(struct dvm *v,const char *desc,const char *parent,bool interface,const char *const *ifaces,int n){
 struct dvm_class *c=dvm__class_by_desc(v,desc);if(!c)return NULL;c->is_interface=interface;
 if(interface)c->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;
 if(parent)c->super=dvm__class_by_desc(v,parent);
 c->nifaces=n;c->ifaces=n?calloc((size_t)n,sizeof *c->ifaces):NULL;
 if(n&&!c->ifaces){c->nifaces=0;dvm__throw(v,"java/lang/OutOfMemoryError","collection type metadata");return c;}
 for(int i=0;i<n;++i)c->ifaces[i]=dvm__class_by_desc(v,ifaces[i]);return c;
}
void collections_vm_start(struct dvm *v){
 const char *iterable[]={"Ljava/lang/Iterable;"},*collection[]={"Ljava/util/Collection;"},*list[]={"Ljava/util/List;"},*map[]={"Ljava/util/Map;"};
 const char *array_interfaces[]={"Ljava/util/List;","Ljava/util/RandomAccess;","Ljava/lang/Cloneable;","Ljava/io/Serializable;"};
 const char *map_interfaces[]={"Ljava/util/Map;","Ljava/lang/Cloneable;","Ljava/io/Serializable;"};
 const char *iterator[]={"Ljava/util/Iterator;"};
 metadata(v,"Ljava/lang/Iterable;",NULL,true,NULL,0);metadata(v,"Ljava/util/Collection;",NULL,true,iterable,1);metadata(v,"Ljava/util/List;",NULL,true,collection,1);metadata(v,"Ljava/util/Map;",NULL,true,NULL,0);
 metadata(v,"Ljava/util/Iterator;",NULL,true,NULL,0);metadata(v,"Ljava/util/RandomAccess;",NULL,true,NULL,0);metadata(v,"Ljava/lang/Cloneable;",NULL,true,NULL,0);metadata(v,"Ljava/io/Serializable;",NULL,true,NULL,0);
 metadata(v,"Ljava/util/AbstractCollection;","Ljava/lang/Object;",false,collection,1);metadata(v,"Ljava/util/AbstractList;","Ljava/util/AbstractCollection;",false,list,1);metadata(v,"Ljava/util/AbstractMap;","Ljava/lang/Object;",false,map,1);
 metadata(v,"Ljava/util/ArrayList;","Ljava/util/AbstractList;",false,array_interfaces,4);metadata(v,"Ljava/util/HashMap;","Ljava/util/AbstractMap;",false,map_interfaces,3);metadata(v,"Ljava/util/LinkedHashMap;","Ljava/util/HashMap;",false,NULL,0);
 const char *queue[]={"Ljava/util/Queue;"};
 const char *linked[]={"Ljava/util/List;","Ljava/util/Deque;","Ljava/lang/Cloneable;","Ljava/io/Serializable;"};
 metadata(v,"Ljava/util/Queue;",NULL,true,collection,1);metadata(v,"Ljava/util/Deque;",NULL,true,queue,1);
 metadata(v,"Ljava/util/AbstractSequentialList;","Ljava/util/AbstractList;",false,NULL,0);
 metadata(v,"Ljava/util/LinkedList;","Ljava/util/AbstractSequentialList;",false,linked,4);
 const char *deque_interfaces[]={"Ljava/util/Deque;","Ljava/lang/Cloneable;","Ljava/io/Serializable;"};
 metadata(v,"Ljava/util/ArrayDeque;","Ljava/util/AbstractCollection;",false,deque_interfaces,3);
 metadata(v,"Ljava/util/ArrayList$Itr;","Ljava/lang/Object;",false,iterator,1);metadata(v,"Ljava/util/concurrent/CopyOnWriteArrayList;","Ljava/lang/Object;",false,array_interfaces,4);
}
typedef bool (*method_fn)(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool invoke(method_fn fn,struct dvm *v,dvm_ref r,const union dvm_value *a,int n,union dvm_value *out){
 bool ok=fn(v,r,a,n,out);if(fn==list_toArray&&!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","collection array");if(!ok&&!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","collection operation did not implement its result");return true;
}
static dvm_ref snapshot(struct dvm *v,dvm_ref source){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,source))return 0;
 if(!source){dvm__throw(v,"java/lang/NullPointerException","source Collection");return 0;}
 struct dvm_object *o=dvm__obj(v,source);union dvm_value result={0};
 if(o&&(is_type(o->cls,"java/util/ArrayList")||is_type(o->cls,"java/util/LinkedList"))){if(!list_toArray(v,source,NULL,0,&result))return 0;}
 else if(o&&is_type(o->cls,"java/util/concurrent/CopyOnWriteArrayList")){if(!java_basics_call(v,"java/util/concurrent/CopyOnWriteArrayList","toArray","()[Ljava/lang/Object;",source,NULL,0,&result))return 0;}
 else {struct dvm_method *m=o?dvm_find_method(v,o->cls,"toArray","()[Ljava/lang/Object;"):NULL;
  if(!m){o=dvm__obj(v,source);sdk_trace_missing_api(o&&o->cls?o->cls->name:"?","toArray","()[Ljava/lang/Object;");dvm__throw(v,"java/lang/NoSuchMethodError","Collection.toArray");return 0;}
  if(!dvm_call(v,m,source,NULL,0,&result))return 0;
 }
 struct dvm_object *array=dvm__obj(v,result.l);
 if(!array||array->kind!=DVM_OBJ_ARRAY||array->elem_kind!='L'){dvm__throw(v,"java/lang/IllegalStateException","Collection.toArray returned no object array");return 0;}
 gc_operation_result(&op,"()[Ljava/lang/Object;",result.l);return result.l;
}
static bool bulk_add(struct dvm *v,dvm_ref self,dvm_ref source,int at,union dvm_value *out){
 dvm_ref array=snapshot(v,source);if(!array)return true;
 struct rt_list *l=list_of(v,self);if(at<0||!l||(uint32_t)at>l->size){dvm__throw(v,"java/lang/IndexOutOfBoundsException","addAll index");return true;}
 uint32_t n=dvm_array_length(v,array);dvm_ref *items=dvm_array_data(v,array);
 for(uint32_t i=0;i<n;++i){items=dvm_array_data(v,array);if(!items)return true;union dvm_value a[]={{.i=at+(int)i},{.l=items[i]}},ignored={0};if(!list_add_at(v,self,a,2,&ignored))return true;}
 out->i=n!=0;return true;
}
struct iterator{dvm_ref target;uint32_t cursor,expected;int64_t last;};
static bool iterator_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,union dvm_value *out){
 if(strcmp(sig,"()Z")&&strcmp(sig,"()Ljava/lang/Object;")&&strcmp(sig,"()V"))return false;
 bool has=!strcmp(m,"hasNext")&&!strcmp(sig,"()Z"),next=!strcmp(m,"next")&&!strcmp(sig,"()Ljava/lang/Object;"),remove=!strcmp(m,"remove")&&!strcmp(sig,"()V");if(!has&&!next&&!remove)return false;
 struct dvm_object *o=dvm__obj(v,self);struct iterator *it=o?o->data:NULL;struct rt_list *l=it?list_of(v,it->target):NULL;
 if(!it||!l){dvm__throw(v,"java/lang/IllegalStateException","ArrayList iterator uninitialized");return true;}
 if(has){out->i=it->cursor!=l->size;return true;}
 if(it->expected!=l->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","ArrayList structurally modified");return true;}
 if(next){if(it->cursor>=l->size)dvm__throw(v,"java/util/NoSuchElementException","iterator exhausted");else{it->last=it->cursor;out->l=l->items[it->cursor++];}return true;}
 if(it->last<0){dvm__throw(v,"java/lang/IllegalStateException","iterator.remove requires next");return true;}
 union dvm_value index={.i=(int)it->last},ignored={0};if(!list_remove(v,it->target,&index,1,&ignored))return true;
 it->cursor=(uint32_t)it->last;it->last=-1;it->expected=list_of(v,it->target)->mod_count;return true;
}
#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
static bool collections_call_gc9_operation(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)c;memset(out,0,sizeof *out);struct dvm_object *o=dvm__obj(v,self);
 if(o&&o->cls&&!strcmp(o->cls->name,"java/util/ArrayList$Itr"))return iterator_call(v,m,sig,self,out);
 bool linked=o&&is_type(o->cls,"java/util/LinkedList"), arraydeque=o&&is_type(o->cls,"java/util/ArrayDeque");
 if(arraydeque){
  if(!strcmp(m,"<init>")){if(!IS("<init>","()V"))return false;return invoke(list_init,v,self,NULL,0,out);}
  if(!o->data){dvm__throw(v,"java/lang/IllegalStateException","ArrayDeque constructor not called");return true;}
  bool first=IS("addFirst","(Ljava/lang/Object;)V")||IS("offerFirst","(Ljava/lang/Object;)Z")||IS("push","(Ljava/lang/Object;)V");
  bool tailadd=IS("addLast","(Ljava/lang/Object;)V")||IS("offerLast","(Ljava/lang/Object;)Z")||IS("offer","(Ljava/lang/Object;)Z")||IS("add","(Ljava/lang/Object;)Z");
  if(first||tailadd){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","ArrayDeque rejects null");return true;}union dvm_value x[2]={{.i=first?0:(int)list_of(v,self)->size},{.l=a[0].l}};if(first)invoke(list_add_at,v,self,x,2,out);else invoke(list_add,v,self,a,1,out);if(!dvm_exception(v))out->i=1;return true;}
  bool head=IS("getFirst","()Ljava/lang/Object;")||IS("element","()Ljava/lang/Object;")||IS("peekFirst","()Ljava/lang/Object;")||IS("peek","()Ljava/lang/Object;")||IS("pollFirst","()Ljava/lang/Object;")||IS("poll","()Ljava/lang/Object;")||IS("removeFirst","()Ljava/lang/Object;")||IS("remove","()Ljava/lang/Object;")||IS("pop","()Ljava/lang/Object;");
  bool tail=IS("getLast","()Ljava/lang/Object;")||IS("peekLast","()Ljava/lang/Object;")||IS("pollLast","()Ljava/lang/Object;")||IS("removeLast","()Ljava/lang/Object;");
  if(head||tail){struct rt_list*l=list_of(v,self);if(!l->size){if(!strncmp(m,"peek",4)||!strncmp(m,"poll",4))return true;dvm__throw(v,"java/util/NoSuchElementException","empty ArrayDeque");return true;}union dvm_value ix={.i=head?0:(int)l->size-1};bool rem=!strncmp(m,"poll",4)||!strncmp(m,"remove",6)||!strcmp(m,"pop");return invoke(rem?list_remove:list_get,v,self,&ix,1,out);}
  if(IS("size","()I"))return invoke(list_size,v,self,a,nargs,out);if(IS("isEmpty","()Z"))return invoke(list_isEmpty,v,self,a,nargs,out);
  if(IS("clear","()V"))return invoke(list_clear,v,self,a,nargs,out);
  if(IS("contains","(Ljava/lang/Object;)Z"))return invoke(list_contains,v,self,a,nargs,out);
  if(IS("remove","(Ljava/lang/Object;)Z")||IS("removeFirstOccurrence","(Ljava/lang/Object;)Z"))return invoke(list_remove_obj,v,self,a,nargs,out);
  if(IS("iterator","()Ljava/util/Iterator;")){
   out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/ArrayList$Itr"));struct dvm_object *iter=dvm__obj(v,out->l);struct iterator *it=calloc(1,sizeof *it);
   if(!iter||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","deque iterator");return true;}
   it->target=self;it->expected=list_of(v,self)->mod_count;it->last=-1;iter->data=it;return true;
  }
  return false;
 }
 bool list=o&&(is_type(o->cls,"java/util/ArrayList")||linked),map=o&&is_type(o->cls,"java/util/HashMap");if(!list&&!map)return false;
 if(list){
  bool ctor=IS("<init>","()V")||(!linked&&IS("<init>","(I)V"))||IS("<init>","(Ljava/util/Collection;)V");
  if(ctor){if(IS("<init>","(I)V")&&a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative ArrayList capacity");return true;}
   if(!list_init(v,self,NULL,0,out))return true;
   if(IS("<init>","(I)V")&&a[0].i>8){if(a[0].i>1000000){dvm__throw(v,"java/lang/OutOfMemoryError","diagnostic collection limit");return true;}struct rt_list *l=realloc(o->data,sizeof *l+(size_t)a[0].i*sizeof(dvm_ref));if(!l){dvm__throw(v,"java/lang/OutOfMemoryError","ArrayList capacity");return true;}o->data=l;l->cap=(uint32_t)a[0].i;}
   if(IS("<init>","(Ljava/util/Collection;)V"))return bulk_add(v,self,a[0].l,0,out);return true;
  }
  if(!o->data){dvm__throw(v,"java/lang/IllegalStateException","ArrayList constructor not called");return true;}
  if(linked){
   bool first=IS("addFirst","(Ljava/lang/Object;)V")||IS("offerFirst","(Ljava/lang/Object;)Z")||IS("push","(Ljava/lang/Object;)V");
   bool last=IS("addLast","(Ljava/lang/Object;)V")||IS("offerLast","(Ljava/lang/Object;)Z")||IS("offer","(Ljava/lang/Object;)Z");
   if(first||last){union dvm_value args[2]={{.i=first?0:(int32_t)list_of(v,self)->size},{.l=a[0].l}};invoke(list_add_at,v,self,args,2,out);if(!dvm_exception(v))out->i=1;return true;}
   bool head=IS("getFirst","()Ljava/lang/Object;")||IS("element","()Ljava/lang/Object;")||IS("peekFirst","()Ljava/lang/Object;")||IS("peek","()Ljava/lang/Object;")||IS("pollFirst","()Ljava/lang/Object;")||IS("poll","()Ljava/lang/Object;")||IS("removeFirst","()Ljava/lang/Object;")||IS("remove","()Ljava/lang/Object;")||IS("pop","()Ljava/lang/Object;");
   bool tail=IS("getLast","()Ljava/lang/Object;")||IS("peekLast","()Ljava/lang/Object;")||IS("pollLast","()Ljava/lang/Object;")||IS("removeLast","()Ljava/lang/Object;");
   if(head||tail){
    struct rt_list *l=list_of(v,self);
    if(!l->size){if(!strncmp(m,"peek",4)||!strncmp(m,"poll",4))out->l=0;else dvm__throw(v,"java/util/NoSuchElementException","empty LinkedList");return true;}
    union dvm_value index={.i=head?0:(int32_t)l->size-1};
    bool remove=!strncmp(m,"poll",4)||!strncmp(m,"remove",6)||!strcmp(m,"pop");
    return invoke(remove?list_remove:list_get,v,self,&index,1,out);
   }
  }
  if(IS("iterator","()Ljava/util/Iterator;")){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/ArrayList$Itr"));struct dvm_object *iter=dvm__obj(v,out->l);struct iterator *it=calloc(1,sizeof *it);if(!iter||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","ArrayList iterator");return true;}it->target=self;it->expected=list_of(v,self)->mod_count;it->last=-1;iter->data=it;return true;}
  if(IS("addAll","(Ljava/util/Collection;)Z"))return bulk_add(v,self,a[0].l,(int)list_of(v,self)->size,out);
  if(IS("addAll","(ILjava/util/Collection;)Z"))return bulk_add(v,self,a[1].l,a[0].i,out);
  if(IS("indexOf","(Ljava/lang/Object;)I")||IS("lastIndexOf","(Ljava/lang/Object;)I")){
   struct rt_list *l=list_of(v,self);out->i=-1;for(uint32_t i=0;i<l->size;++i){bool hit=java_basics_equal(v,a[0].l,l->items[i]);if(dvm_exception(v))return true;if(hit){out->i=(int)i;if(IS("indexOf","(Ljava/lang/Object;)I"))break;}}return true;}
#define LIST(name,signature,fn) if(IS(name,signature))return invoke(fn,v,self,a,nargs,out)
  LIST("size","()I",list_size);LIST("isEmpty","()Z",list_isEmpty);LIST("clear","()V",list_clear);
  LIST("add","(Ljava/lang/Object;)Z",list_add);LIST("add","(ILjava/lang/Object;)V",list_add_at);
  LIST("get","(I)Ljava/lang/Object;",list_get);LIST("set","(ILjava/lang/Object;)Ljava/lang/Object;",list_set);
  LIST("remove","(I)Ljava/lang/Object;",list_remove);LIST("remove","(Ljava/lang/Object;)Z",list_remove_obj);LIST("contains","(Ljava/lang/Object;)Z",list_contains);
  LIST("toArray","()[Ljava/lang/Object;",list_toArray);LIST("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",list_toArray);
  return false;
 }
 bool ctor=IS("<init>","()V")||IS("<init>","(I)V")||IS("<init>","(IF)V")||IS("<init>","(Ljava/util/Map;)V");
 if(ctor){if((IS("<init>","(I)V")||IS("<init>","(IF)V"))&&a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative HashMap capacity");return true;}if(IS("<init>","(IF)V")&&(!(a[1].f>0)||isnan(a[1].f))){dvm__throw(v,"java/lang/IllegalArgumentException","invalid HashMap load factor");return true;}if(!map_init(v,self,NULL,0,out))return true;if(!IS("<init>","(Ljava/util/Map;)V"))return true;}
 if(!o->data){dvm__throw(v,"java/lang/IllegalStateException","HashMap constructor not called");return true;}
 if(IS("putAll","(Ljava/util/Map;)V")||IS("<init>","(Ljava/util/Map;)V")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","source Map");return true;}struct dvm_object *src=dvm__obj(v,a[0].l);
  if(!src||!is_type(src->cls,"java/util/HashMap")||!src->data){sdk_trace_missing_api(src&&src->cls?src->cls->name:"?","entrySet","()Ljava/util/Set;");dvm__throw(v,"java/lang/UnsupportedOperationException","Map source requires implemented entrySet view");return true;}
  struct rt_map *mapdata=src->data;size_t bytes=sizeof *mapdata+(size_t)mapdata->cap*sizeof mapdata->e[0];struct rt_map *copy=malloc(bytes);if(!copy){dvm__throw(v,"java/lang/OutOfMemoryError","Map snapshot");return true;}memcpy(copy,mapdata,bytes);
  for(uint32_t i=0;i<copy->size;++i){union dvm_value args[]={{.l=copy->e[i].k},{.l=copy->e[i].v}},ignored={0};if(!map_put(v,self,args,2,&ignored))break;}free(copy);return true;
 }
 if(IS("containsValue","(Ljava/lang/Object;)Z")){struct rt_map *mp=o->data;for(uint32_t i=0;i<mp->size;++i){bool hit=java_basics_equal(v,a[0].l,mp->e[i].v);if(dvm_exception(v))return true;if(hit){out->i=1;break;}}return true;}
 if(IS("getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")){if(!map_get(v,self,a,1,out))return true;if(!out->l){union dvm_value present={0};if(!map_containsKey(v,self,a,1,&present))return true;if(!present.i)out->l=a[1].l;}return true;}
#define MAP(name,signature,fn) if(IS(name,signature))return invoke(fn,v,self,a,nargs,out)
 MAP("put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map_put);MAP("get","(Ljava/lang/Object;)Ljava/lang/Object;",map_get);MAP("remove","(Ljava/lang/Object;)Ljava/lang/Object;",map_remove);MAP("containsKey","(Ljava/lang/Object;)Z",map_containsKey);MAP("size","()I",map_size);MAP("isEmpty","()Z",map_isEmpty);MAP("clear","()V",map_clear);
 return false;
}

bool collections_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){

 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,self))return true;
 bool handled=collections_call_gc9_operation(v,c,m,sig,self,a,nargs,out);
 if(handled&&!dvm_exception(v))gc_operation_result(&op,sig,out->l);
 return handled;
}
