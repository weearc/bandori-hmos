/* MPL-2.0 derivative: the storage and basic operations below are extracted
 * from pinned Lunaria dvm_runtime.c. See collections-provenance.json and the
 * complete generated source/license. Dispatch, bounds/OOM fixes, constructors,
 * bulk snapshots and fail-fast iterator adaptation are local additions. */
#include "collections.h"
#include "java_basics.h"
#include "gc_operation.h"
#include "portable.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <pthread.h>
#define ARG(i) args[i]
#define RETI(x) do{out->i=(x);return true;}while(0)
#define RETL(x) do{out->l=(x);return true;}while(0)
#define RETV() return true
#define fprintf sdk_trace_fprintf
static void list_gc_destroy(void *p){free(p);}
static void iterator_gc_destroy(void *p){free(p);}
/* Included storage helpers must reject a failed live body/payload admission,
 * too. Their payload returns borrow the dispatch/builtin operation interval. */
#define dvm__obj gc_operation_object
#include "upstream_collections.inc"
#undef dvm__obj
extern bool string_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);

bool collections_synchronized_map_selftest(struct dvm *v);
bool collections_synchronized_list_selftest(struct dvm *v);
bool collections_singleton_list_selftest(struct dvm *v);
static bool collections_remove_all_and_empty_selftest(struct dvm *v);
bool collections_weak_map_selftest(struct dvm *v);
/* Check the ordering contract used by the LinkedHashMap dispatcher. */
static bool collections_ordered_map_selftest_operation(struct dvm *v){
 {
  dvm_ref vector=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/Vector"));union dvm_value out={0},a={.l=dvm_new_string(v,"event")};
  if(!vector||!collections_call(v,"java/util/Vector","<init>","()V",vector,NULL,0,&out)||dvm_exception(v))return false;
  if(!collections_call(v,"java/util/Vector","size","()I",vector,NULL,0,&out)||out.i!=0)return false;
  if(!collections_call(v,"java/util/Vector","add","(Ljava/lang/Object;)Z",vector,&a,1,&out)||!out.i)return false;
  union dvm_value index={.i=0};if(!collections_call(v,"java/util/Vector","get","(I)Ljava/lang/Object;",vector,&index,1,&out)||out.l!=a.l)return false;
  if(!collections_call(v,"java/util/List","contains","(Ljava/lang/Object;)Z",vector,&a,1,&out)||!out.i)return false;
  if(!collections_call(v,"java/util/Vector","clear","()V",vector,NULL,0,&out)||!collections_call(v,"java/util/List","isEmpty","()Z",vector,NULL,0,&out)||!out.i||dvm_exception(v))return false;
  if(!collections_call(v,"java/util/Vector","isEmpty","()Z",vector,NULL,0,&out)||!out.i)return false;
  union dvm_value second={.l=dvm_new_string(v,"second")},nil={0};
  if(!collections_call(v,"java/util/Vector","add","(Ljava/lang/Object;)Z",vector,&a,1,&out)||!out.i)return false;
  if(!collections_call(v,"java/util/Vector","add","(Ljava/lang/Object;)Z",vector,&second,1,&out)||!out.i)return false;
  if(!collections_call(v,"java/util/Vector","add","(Ljava/lang/Object;)Z",vector,&nil,1,&out)||!out.i)return false;
  if(!collections_call(v,"java/util/Vector","size","()I",vector,NULL,0,&out)||out.i!=3)return false;
  if(!collections_call(v,"java/util/Vector","isEmpty","()Z",vector,NULL,0,&out)||out.i)return false;
  dvm_ref expected[]={a.l,second.l,0};
  for(int i=0;i<3;i++){index.i=i;if(!collections_call(v,"java/util/Vector","get","(I)Ljava/lang/Object;",vector,&index,1,&out)||out.l!=expected[i])return false;}
  for(int i=0;i<2;i++){
   index.i=i?3:-1;if(!collections_call(v,"java/util/Vector","get","(I)Ljava/lang/Object;",vector,&index,1,&out)||!dvm_exception(v))return false;
   const dvm_ref gc_ref_error_11=(dvm_exception(v)); if(!gc_operation_admit(v,gc_ref_error_11))return 0;if(!dvm__obj(v,gc_ref_error_11)||strcmp(dvm__obj(v,gc_ref_error_11)->cls->name,"java/lang/IndexOutOfBoundsException"))return false;dvm_clear_exception(v);
  }
  sdk_trace_fprintf(stderr,"TASK5_VECTOR_ARRAY_SELFTEST PASS size=3 ordered=1 null_element=1 isEmpty=1 negative_bound=1 upper_bound=1\n");
  dvm_ref stack=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/Stack"));
  if(!stack||!collections_call(v,"java/util/Stack","<init>","()V",stack,NULL,0,&out))return false;
  if(!collections_call(v,"java/util/Stack","empty","()Z",stack,NULL,0,&out)||!out.i)return false;
  if(!collections_call(v,"java/util/Stack","push","(Ljava/lang/Object;)Ljava/lang/Object;",stack,&a,1,&out)||out.l!=a.l)return false;
  if(!collections_call(v,"java/util/Vector","lastElement","()Ljava/lang/Object;",stack,NULL,0,&out)||out.l!=a.l)return false;
  if(!collections_call(v,"java/util/Stack","peek","()Ljava/lang/Object;",stack,NULL,0,&out)||out.l!=a.l)return false;
  if(!collections_call(v,"java/util/Stack","pop","()Ljava/lang/Object;",stack,NULL,0,&out)||out.l!=a.l)return false;
  if(!collections_call(v,"java/util/Stack","pop","()Ljava/lang/Object;",stack,NULL,0,&out)||!dvm_exception(v))return false;
  const dvm_ref gc_ref_empty_12=(dvm_exception(v)); if(!gc_operation_admit(v,gc_ref_empty_12))return 0;if(!dvm__obj(v,gc_ref_empty_12)||strcmp(dvm__obj(v,gc_ref_empty_12)->cls->name,"java/util/EmptyStackException"))return false;dvm_clear_exception(v);
  sdk_trace_fprintf(stderr,"TASK5_STACK_SELFTEST PASS push=1 peek=1 pop=1 lastElement=1 empty_exception=1\n");
  sdk_trace_fprintf(stderr,"TASK5_VECTOR_SELFTEST PASS constructor=1 add_get=1 interface_dispatch=1 clear=1 recursive_lock=1\n");
  union dvm_value pair[2]={{0},{.l=a.l}};
  if(!collections_call(v,"java/util/Collections","singletonMap","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/util/Map;",0,pair,2,&out)||!out.l)return false;
  dvm_ref map=out.l;
  if(!collections_call(v,"java/util/Map","isEmpty","()Z",map,NULL,0,&out)||out.i||dvm_exception(v))return false;
  if(!collections_call(v,"java/util/Map","size","()I",map,NULL,0,&out)||out.i!=1)return false;
  if(!collections_call(v,"java/util/Map","get","(Ljava/lang/Object;)Ljava/lang/Object;",map,pair,1,&out)||out.l!=a.l)return false;
  if(!collections_call(v,"java/util/Map","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,pair,2,&out)||!dvm_exception(v))return false;
  const dvm_ref gc_ref_error_13=(dvm_exception(v)); if(!gc_operation_admit(v,gc_ref_error_13))return 0;if(!dvm__obj(v,gc_ref_error_13)||strcmp(dvm__obj(v,gc_ref_error_13)->cls->name,"java/lang/UnsupportedOperationException"))return false;dvm_clear_exception(v);
  sdk_trace_fprintf(stderr,"TASK5_SINGLETON_MAP_SELFTEST PASS size=1 null_key=1 value=1 immutable=1\n");
 }
 if(!collections_remove_all_and_empty_selftest(v))return false;
 if(!collections_weak_map_selftest(v))return false;
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
 /* collections_test.c is owned by the integration pass, so the synchronized
  * map checks are reached through this existing hook.  They print their own
  * SYNCHRONIZED_MAP_SELFTEST marker and fail the collection selftest here. */
 if(!collections_synchronized_map_selftest(v))return false;
 if(!collections_synchronized_list_selftest(v))return false;
 if(!collections_singleton_list_selftest(v))return false;
 sdk_trace_fprintf(stderr,"ORDERED_MAP_SELFTEST PASS update_keeps_order=1 reinsert_appends=1 null_mapping=1\n");return !dvm_exception(v);
}
bool collections_ordered_map_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=collections_ordered_map_selftest_operation(v);
 return result;
}


static bool is_type(struct dvm_class *c,const char *name){for(;c;c=c->super)if(!strcmp(c->name,name))return true;return false;}
/* Map.entrySet() view classes are registered by entry_views_vm_start().  They
 * are real VM classes (Map.Entry / Set / Iterator assignable) rather than
 * external HashMap$Node objects whose absence of equals() made Set.containsKey
 * throw for the second distinct entry. */
static void entry_views_vm_start(struct dvm *v);
static void empty_list_vm_start(struct dvm *v);
static void empty_map_set_vm_start(struct dvm *v);
static void map_values_vm_start(struct dvm *v);
static void synchronized_map_vm_start(struct dvm *v);
static void readonly_map_vm_start(struct dvm *v);
static void synchronized_list_vm_start(struct dvm *v);
static void singleton_list_vm_start(struct dvm *v);
static void ordinary_map_methods_vm_start(struct dvm *v);
static struct dvm_class *metadata(struct dvm *v,const char *desc,const char *parent,bool interface,const char *const *ifaces,int n){
 struct dvm_class *c=dvm__class_by_desc(v,desc);if(!c)return NULL;c->is_interface=interface;
 if(interface)c->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;
 if(parent)c->super=dvm__class_by_desc(v,parent);
 c->nifaces=n;c->ifaces=n?calloc((size_t)n,sizeof *c->ifaces):NULL;
 if(n&&!c->ifaces){c->nifaces=0;dvm__throw(v,"java/lang/OutOfMemoryError","collection type metadata");return c;}
 for(int i=0;i<n;++i)c->ifaces[i]=dvm__class_by_desc(v,ifaces[i]);return c;
}
static void collections_vm_start_operation(struct dvm *v){
 const char *iterable[]={"Ljava/lang/Iterable;"},*collection[]={"Ljava/util/Collection;"},*list[]={"Ljava/util/List;"},*map[]={"Ljava/util/Map;"};
 const char *array_interfaces[]={"Ljava/util/List;","Ljava/util/RandomAccess;","Ljava/lang/Cloneable;","Ljava/io/Serializable;"};
 const char *map_interfaces[]={"Ljava/util/Map;","Ljava/lang/Cloneable;","Ljava/io/Serializable;"};
 const char *iterator[]={"Ljava/util/Iterator;"};
 metadata(v,"Ljava/lang/Iterable;",NULL,true,NULL,0);metadata(v,"Ljava/util/Collection;",NULL,true,iterable,1);metadata(v,"Ljava/util/List;",NULL,true,collection,1);metadata(v,"Ljava/util/Map;",NULL,true,NULL,0);
 dvm__class_by_desc(v,"Ljava/lang/Iterable;")->runtime_signature="<T:Ljava/lang/Object;>Ljava/lang/Object;";
 dvm__class_by_desc(v,"Ljava/util/Collection;")->runtime_signature="<E:Ljava/lang/Object;>Ljava/lang/Object;Ljava/lang/Iterable<TE;>;";
 dvm__class_by_desc(v,"Ljava/util/List;")->runtime_signature="<E:Ljava/lang/Object;>Ljava/lang/Object;Ljava/util/Collection<TE;>;";
 metadata(v,"Ljava/util/Iterator;",NULL,true,NULL,0);metadata(v,"Ljava/util/ListIterator;",NULL,true,iterator,1);metadata(v,"Ljava/util/RandomAccess;",NULL,true,NULL,0);metadata(v,"Ljava/lang/Cloneable;",NULL,true,NULL,0);metadata(v,"Ljava/io/Serializable;",NULL,true,NULL,0);
 metadata(v,"Ljava/util/AbstractCollection;","Ljava/lang/Object;",false,collection,1);metadata(v,"Ljava/util/AbstractList;","Ljava/util/AbstractCollection;",false,list,1);metadata(v,"Ljava/util/AbstractMap;","Ljava/lang/Object;",false,map,1);
 metadata(v,"Ljava/util/ArrayList;","Ljava/util/AbstractList;",false,array_interfaces,4);metadata(v,"Ljava/util/HashMap;","Ljava/util/AbstractMap;",false,map_interfaces,3);metadata(v,"Ljava/util/LinkedHashMap;","Ljava/util/HashMap;",false,NULL,0);
 metadata(v,"Ljava/util/WeakHashMap;","Ljava/util/AbstractMap;",false,map,1);
 metadata(v,"Ljava/util/Vector;","Ljava/util/AbstractList;",false,array_interfaces,4);
 metadata(v,"Ljava/util/Stack;","Ljava/util/Vector;",false,NULL,0);
 const char *queue[]={"Ljava/util/Queue;"};
 const char *linked[]={"Ljava/util/List;","Ljava/util/Deque;","Ljava/lang/Cloneable;","Ljava/io/Serializable;"};
 metadata(v,"Ljava/util/Queue;",NULL,true,collection,1);metadata(v,"Ljava/util/Deque;",NULL,true,queue,1);
 metadata(v,"Ljava/util/AbstractSequentialList;","Ljava/util/AbstractList;",false,NULL,0);
 metadata(v,"Ljava/util/LinkedList;","Ljava/util/AbstractSequentialList;",false,linked,4);
 const char *deque_interfaces[]={"Ljava/util/Deque;","Ljava/lang/Cloneable;","Ljava/io/Serializable;"};
 metadata(v,"Ljava/util/ArrayDeque;","Ljava/util/AbstractCollection;",false,deque_interfaces,3);
 metadata(v,"Ljava/util/ArrayList$Itr;","Ljava/lang/Object;",false,iterator,1);metadata(v,"Ljava/util/concurrent/CopyOnWriteArrayList;","Ljava/lang/Object;",false,array_interfaces,4);
 /* Set interface metadata plus the CopyOnWriteArraySet facade.  Without the
  * Set edge, instanceof/assignability checks on a synthesized COW Set failed
  * even though its add/iterator dispatch exists in java_basics. */
 const char *set_interfaces[]={"Ljava/util/Set;"};
 metadata(v,"Ljava/util/Set;",NULL,true,collection,1);
 metadata(v,"Ljava/util/concurrent/CopyOnWriteArraySet;","Ljava/lang/Object;",false,set_interfaces,1);
 entry_views_vm_start(v);
 empty_list_vm_start(v);
 empty_map_set_vm_start(v);
 map_values_vm_start(v);
 synchronized_map_vm_start(v);
 readonly_map_vm_start(v);
 synchronized_list_vm_start(v);
 singleton_list_vm_start(v);
 ordinary_map_methods_vm_start(v);
}
void collections_vm_start(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return;
 collections_vm_start_operation(v);
}

typedef bool (*method_fn)(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool entry_set_call(struct dvm *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool key_set_call(struct dvm *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool values_call(struct dvm *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool invoke(method_fn fn,struct dvm *v,dvm_ref r,const union dvm_value *a,int n,union dvm_value *out){
 bool ok=fn(v,r,a,n,out);if(fn==list_toArray&&!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","collection array");if(!ok&&!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","collection operation did not implement its result");return true;
}
static dvm_ref snapshot(struct dvm *v,dvm_ref source){
 if(!source){dvm__throw(v,"java/lang/NullPointerException","source Collection");return 0;}
 const dvm_ref gc_ref_o_14=(source); if(!gc_operation_admit(v,gc_ref_o_14))return 0;union dvm_value result={0};
 if(dvm__obj(v,gc_ref_o_14)&&(is_type(dvm__obj(v,gc_ref_o_14)->cls,"java/util/ArrayList")||is_type(dvm__obj(v,gc_ref_o_14)->cls,"java/util/LinkedList"))){if(!list_toArray(v,source,NULL,0,&result))return 0;}
 else if(dvm__obj(v,gc_ref_o_14)&&is_type(dvm__obj(v,gc_ref_o_14)->cls,"java/util/concurrent/CopyOnWriteArrayList")){if(!java_basics_call(v,"java/util/concurrent/CopyOnWriteArrayList","toArray","()[Ljava/lang/Object;",source,NULL,0,&result))return 0;}
 else if(dvm__obj(v,gc_ref_o_14)&&dvm__obj(v,gc_ref_o_14)->cls&&!strcmp(dvm__obj(v,gc_ref_o_14)->cls->name,"sdktrace/collections/KeySet")){if(!key_set_call(v,"toArray","()[Ljava/lang/Object;",source,NULL,0,&result))return 0;}
 else if(dvm__obj(v,gc_ref_o_14)&&dvm__obj(v,gc_ref_o_14)->cls&&!strcmp(dvm__obj(v,gc_ref_o_14)->cls->name,"sdktrace/collections/Values")){if(!values_call(v,"toArray","()[Ljava/lang/Object;",source,NULL,0,&result))return 0;}
 else if(dvm__obj(v,gc_ref_o_14)&&dvm__obj(v,gc_ref_o_14)->cls&&!strcmp(dvm__obj(v,gc_ref_o_14)->cls->name,"sdktrace/collections/EntrySet")){if(!entry_set_call(v,"toArray","()[Ljava/lang/Object;",source,NULL,0,&result))return 0;}
 else {struct dvm_method *m=dvm__obj(v,gc_ref_o_14)?dvm_find_method(v,dvm__obj(v,gc_ref_o_14)->cls,"toArray","()[Ljava/lang/Object;"):NULL;
  if(!m){sdk_trace_missing_api(dvm__obj(v,gc_ref_o_14)&&dvm__obj(v,gc_ref_o_14)->cls?dvm__obj(v,gc_ref_o_14)->cls->name:"?","toArray","()[Ljava/lang/Object;");dvm__throw(v,"java/lang/NoSuchMethodError","Collection.toArray");return 0;}
  if(!dvm_call(v,m,source,NULL,0,&result))return 0;
 }
 const dvm_ref gc_ref_array_15=(result.l); if(!gc_operation_admit(v,gc_ref_array_15))return 0;
 if(!dvm__obj(v,gc_ref_array_15)||dvm__obj(v,gc_ref_array_15)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_15)->elem_kind!='L'){dvm__throw(v,"java/lang/IllegalStateException","Collection.toArray returned no object array");return 0;}
 return result.l;
}
static bool bulk_add(struct dvm *v,dvm_ref self,dvm_ref source,int at,union dvm_value *out){
 dvm_ref array=snapshot(v,source);if(!array)return true;
 struct rt_list *l=list_of(v,self);if(at<0||!l||(uint32_t)at>l->size){dvm__throw(v,"java/lang/IndexOutOfBoundsException","addAll index");return true;}
 uint32_t n=dvm_array_length(v,array);dvm_ref *items=dvm_array_data(v,array);
 for(uint32_t i=0;i<n;++i){union dvm_value a[]={{.i=at+(int)i},{.l=items[i]}},ignored={0};if(!list_add_at(v,self,a,2,&ignored))return true;}
 out->i=n!=0;return true;
}
/* ArrayList/LinkedList.removeAll(Collection).  The argument is snapshotted
 * first, so removing against the list itself or against a backed view cannot
 * observe indices shifting underneath the scan.  Elements are matched with
 * Java equals (java_basics_equal), the same comparator as contains/remove.
 * The surviving elements are compacted in place and mod_count changes only
 * when at least one element was actually removed, matching AbstractCollection.
 * If equals throws, elements removed before the failing element stay removed
 * and the unprocessed tail is moved down, matching ArrayList.batchRemove's
 * finally block. */
static bool list_remove_all_abort(struct dvm *v,struct rt_list *l,uint32_t keep,uint32_t at){
 if(keep!=at){memmove(&l->items[keep],&l->items[at],(size_t)(l->size-at)*sizeof(dvm_ref));l->size=keep+(l->size-at);++l->mod_count;}
 return true;
}
static bool list_remove_all(struct dvm *v,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;
 dvm_ref array=snapshot(v,a&&nargs>0?a[0].l:0);if(!array)return true;
 struct rt_list *l=list_of(v,self);
 if(!l){dvm__throw(v,"java/lang/IllegalStateException","ArrayList constructor not called");return true;}
 uint32_t n=dvm_array_length(v,array),keep=0,i=0;dvm_ref *items=dvm_array_data(v,array);bool changed=false;
 for(;i<l->size;++i){
  bool hit=false;
  for(uint32_t j=0;j<n;++j){bool equal=java_basics_equal(v,l->items[i],items[j]);if(dvm_exception(v))return list_remove_all_abort(v,l,keep,i);if(equal){hit=true;break;}}
  if(hit)changed=true;else l->items[keep++]=l->items[i];
 }
 if(changed){l->size=keep;++l->mod_count;}
 out->i=changed;return true;
}
struct iterator{dvm_ref target;uint32_t cursor,expected;int64_t last;};
static bool iterator_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,union dvm_value *out){
 if(strcmp(sig,"()Z")&&strcmp(sig,"()Ljava/lang/Object;")&&strcmp(sig,"()V"))return false;
 bool has=!strcmp(m,"hasNext")&&!strcmp(sig,"()Z"),next=!strcmp(m,"next")&&!strcmp(sig,"()Ljava/lang/Object;"),remove=!strcmp(m,"remove")&&!strcmp(sig,"()V");if(!has&&!next&&!remove)return false;
 const dvm_ref gc_ref_o_16=(self); if(!gc_operation_admit(v,gc_ref_o_16))return 0;struct iterator *it=dvm__obj(v,gc_ref_o_16)?dvm__obj(v,gc_ref_o_16)->data:NULL;struct rt_list *l=it?list_of(v,it->target):NULL;
 if(!it||!l){dvm__throw(v,"java/lang/IllegalStateException","ArrayList iterator uninitialized");return true;}
 if(has){out->i=it->cursor!=l->size;return true;}
 if(it->expected!=l->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","ArrayList structurally modified");return true;}
 if(next){if(it->cursor>=l->size)dvm__throw(v,"java/util/NoSuchElementException","iterator exhausted");else{it->last=it->cursor;out->l=l->items[it->cursor++];}return true;}
 if(it->last<0){dvm__throw(v,"java/lang/IllegalStateException","iterator.remove requires next");return true;}
 union dvm_value index={.i=(int)it->last},ignored={0};if(!list_remove(v,it->target,&index,1,&ignored))return true;
 it->cursor=(uint32_t)it->last;it->last=-1;it->expected=list_of(v,it->target)->mod_count;return true;
}
/* ---- Map.entrySet() backed view --------------------------------------
 * entrySet() must be a live view over the map, not a HashSet snapshot of
 * HashMap$Node objects: adding the second distinct Node used to reach
 * java_basics_equal() on an external class with no equals() and throw.  The
 * view classes below are VM builtins, so Map.Entry/Set/Iterator checks hold;
 * iteration walks rt_map.e[] directly and therefore follows insertion order.
 * Entries compare by key and value; setValue writes the mapping in place.
 */
typedef struct {struct dvm *vm;dvm_ref map,key,value;} entry_view;
typedef struct {struct dvm *vm;dvm_ref map;} entry_set_view;
typedef struct {struct dvm *vm;dvm_ref owner;uint32_t expected,index;int64_t last;} entry_iter_view;

static void entry_view_destroy(void *p){entry_view *e=p;if(e){if(e->map)dvm_unpin(e->vm,e->map);if(e->key)dvm_unpin(e->vm,e->key);if(e->value)dvm_unpin(e->vm,e->value);free(e);}}
static void entry_set_destroy(void *p){entry_set_view *s=p;if(s){if(s->map)dvm_unpin(s->vm,s->map);free(s);}}
static void entry_iter_destroy(void *p){entry_iter_view *i=p;if(i){if(i->owner)dvm_unpin(i->vm,i->owner);free(i);}}
static struct rt_map *entry_map_data(struct dvm *v,dvm_ref map){map_expunge_stale(v,map);const dvm_ref gc_ref_o_17=(map); if(!gc_operation_admit(v,gc_ref_o_17))return 0;return dvm__obj(v,gc_ref_o_17)?dvm__obj(v,gc_ref_o_17)->data:NULL;}
static bool entry_live_value(struct dvm *v,entry_view *e,dvm_ref *value){
 int64_t at=map_find_key(v,e->map,e->key);
 if(dvm_exception(v))return false;
 struct rt_map *mp=entry_map_data(v,e->map);
 if(!mp||at<0||(uint32_t)at>=mp->size)return false;
 *value=mp->e[at].v;return true;
}
static void entry_store_snapshot(struct dvm *v,entry_view *e,dvm_ref value){
 if(e->value==value)return;
 if(e->value)dvm_unpin(v,e->value);
 e->value=value;
 if(value)dvm_pin(v,value);
}
static dvm_ref entry_current_value(struct dvm *v,entry_view *e){
 dvm_ref live=0;
 if(entry_live_value(v,e,&live))entry_store_snapshot(v,e,live);
 return e->value;
}
static dvm_ref entry_new(struct dvm *v,dvm_ref map,dvm_ref key,dvm_ref value){
 struct dvm_class *cls=dvm_find_class_or_external(v,"sdktrace/collections/MapEntry");
 entry_view *e=calloc(1,sizeof *e);
 if(!cls||!e){free(e);dvm__throw(v,"java/lang/OutOfMemoryError","Map.Entry view");return 0;}
 dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_o_18=(ref); if(!gc_operation_admit(v,gc_ref_o_18))return 0;
 if(!dvm__obj(v,gc_ref_o_18)){free(e);dvm__throw(v,"java/lang/OutOfMemoryError","Map.Entry object");return 0;}
 e->vm=v;e->map=map;e->key=key;e->value=value;
 if(map)dvm_pin(v,map);if(key)dvm_pin(v,key);if(value)dvm_pin(v,value);
 dvm__obj(v,gc_ref_o_18)->data=e;dvm__obj(v,gc_ref_o_18)->data_destroy=entry_view_destroy;
 return ref;
}
/* Reads (key,value) from these entries and from foreign Map.Entry
 * implementations (methods first, then the key/value fields used by
 * HashMap$Node).  False without an exception means "not an entry"; callers
 * still check dvm_exception() because user code may throw. */
static bool entry_read_pair(struct dvm *v,dvm_ref ref,dvm_ref *key,dvm_ref *value){
 *key=0;*value=0;
 if(!ref)return false;
 const dvm_ref gc_ref_o_19=(ref); if(!gc_operation_admit(v,gc_ref_o_19))return 0;
 if(!dvm__obj(v,gc_ref_o_19))return false;
 if(dvm__obj(v,gc_ref_o_19)->data_destroy==entry_view_destroy){
  entry_view *e=dvm__obj(v,gc_ref_o_19)->data;
  *key=e->key;*value=entry_current_value(v,e);return !dvm_exception(v);
 }
 struct dvm_class *iface=dvm_find_class_or_external(v,"java/util/Map$Entry");
 if(!iface||!dvm__obj(v,gc_ref_o_19)->cls||!dvm__class_assignable(v,dvm__obj(v,gc_ref_o_19)->cls,iface))return false;
 struct dvm_method *get_key=dvm_find_method(v,dvm__obj(v,gc_ref_o_19)->cls,"getKey","()Ljava/lang/Object;");
 struct dvm_method *get_value=dvm_find_method(v,dvm__obj(v,gc_ref_o_19)->cls,"getValue","()Ljava/lang/Object;");
 if(get_key&&get_value&&(get_key->has_code||get_key->builtin)&&(get_value->has_code||get_value->builtin)){
  union dvm_value k={0},val={0};
  if(!dvm_call(v,get_key,ref,NULL,0,&k)||!dvm_call(v,get_value,ref,NULL,0,&val))return false;
  *key=k.l;*value=val.l;return true;
 }
 union dvm_value k={0},val={0};
 if(!dvm_get_field(v,ref,"key","Ljava/lang/Object;",&k)||!dvm_get_field(v,ref,"value","Ljava/lang/Object;",&val))return false;
 *key=k.l;*value=val.l;return true;
}
extern bool string_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
static uint32_t entry_hash_part(struct dvm *v,dvm_ref ref){
 if(!ref)return 0;
 const dvm_ref gc_ref_o_20=(ref); if(!gc_operation_admit(v,gc_ref_o_20))return 0;
 union dvm_value hash={0};
 if(dvm__obj(v,gc_ref_o_20)&&dvm__obj(v,gc_ref_o_20)->kind==DVM_OBJ_STRING){
  if(string_runtime_call(v,"java/lang/String","hashCode","()I",ref,NULL,0,&hash)&&!dvm_exception(v))return (uint32_t)hash.i;
  return 0;
 }
 if(dvm__obj(v,gc_ref_o_20)&&dvm__obj(v,gc_ref_o_20)->cls){
  struct dvm_method *m=dvm_find_method(v,dvm__obj(v,gc_ref_o_20)->cls,"hashCode","()I");
  if(m&&(m->has_code||m->builtin)){if(dvm_call(v,m,ref,NULL,0,&hash)&&!dvm_exception(v))return (uint32_t)hash.i;return 0;}
  if(java_basics_call(v,dvm__obj(v,gc_ref_o_20)->cls->name,"hashCode","()I",ref,NULL,0,&hash)){if(!dvm_exception(v))return (uint32_t)hash.i;return 0;}
 }
 /* Object.hashCode identity fallback for classes without an implementation. */
 return (uint32_t)(uintptr_t)ref;
}
extern bool set_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
extern bool list_views_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
static bool entry_tostring_builtin(struct dvm*,dvm_ref,const union dvm_value*,int,union dvm_value*);
static bool entry_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;const dvm_ref gc_ref_o_21=(self); if(!gc_operation_admit(v,gc_ref_o_21))return 0;entry_view *e=dvm__obj(v,gc_ref_o_21)?dvm__obj(v,gc_ref_o_21)->data:NULL;if(!e)return false;
 if(IS("getKey","()Ljava/lang/Object;")){out->l=e->key;return true;}
 if(IS("getValue","()Ljava/lang/Object;")){dvm_ref value=entry_current_value(v,e);if(dvm_exception(v))return true;out->l=value;return true;}
 if(IS("setValue","(Ljava/lang/Object;)Ljava/lang/Object;")){
  int64_t at=map_find_key(v,e->map,e->key);
  if(dvm_exception(v))return true;
  struct rt_map *mp=entry_map_data(v,e->map);
  if(!mp||at<0||(uint32_t)at>=mp->size){dvm__throw(v,"java/lang/IllegalStateException","Map.Entry key is no longer mapped");return true;}
  dvm_ref old=mp->e[at].v;
  if(mp->weak_keys)dvm_pin(v,a[0].l);
  mp->e[at].v=a[0].l;
  if(mp->e[at].node)(void)dvm_set_field(v,mp->e[at].node,"value","Ljava/lang/Object;",a[0]);
  if(mp->weak_keys)dvm_unpin(v,old);else if(a[0].l)dvm_pin(v,a[0].l);
  entry_store_snapshot(v,e,a[0].l);
  out->l=old;return true;
 }
 if(IS("equals","(Ljava/lang/Object;)Z")){
  dvm_ref other=a[0].l;out->i=0;
  if(other==self){out->i=1;return true;}
  dvm_ref key=0,value=0;
  if(!entry_read_pair(v,other,&key,&value))return true;
  dvm_ref own=entry_current_value(v,e);if(dvm_exception(v))return true;
  out->i=java_basics_equal(v,e->key,key)&&!dvm_exception(v)&&java_basics_equal(v,own,value)&&!dvm_exception(v);
  return true;
 }
 if(IS("hashCode","()I")){
  dvm_ref value=entry_current_value(v,e);if(dvm_exception(v))return true;
  uint32_t hash=entry_hash_part(v,e->key);if(dvm_exception(v))return true;
  hash^=entry_hash_part(v,value);if(dvm_exception(v))return true;
  out->i=(int32_t)hash;return true;
 }
 /* collections_call() reaches Entry methods directly, so this dispatch must
  * stay in sync with the builtins installed by entry_views_vm_start(). */
 if(IS("toString","()Ljava/lang/String;"))return entry_tostring_builtin(v,self,a,nargs,out);
 return false;
}
static bool entry_getkey_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return entry_call(v,"getKey","()Ljava/lang/Object;",self,a,n,out);}
static bool entry_getvalue_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return entry_call(v,"getValue","()Ljava/lang/Object;",self,a,n,out);}
static bool entry_setvalue_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return entry_call(v,"setValue","(Ljava/lang/Object;)Ljava/lang/Object;",self,a,n,out);}
static bool entry_equals_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return entry_call(v,"equals","(Ljava/lang/Object;)Z",self,a,n,out);}
static bool entry_hash_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return entry_call(v,"hashCode","()I",self,a,n,out);}
static dvm_ref entry_render(struct dvm *v,dvm_ref ref){
 if(!ref)return dvm_new_string(v,"null");
 const dvm_ref gc_ref_o_22=(ref); if(!gc_operation_admit(v,gc_ref_o_22))return 0;
 if(dvm__obj(v,gc_ref_o_22)&&dvm__obj(v,gc_ref_o_22)->kind==DVM_OBJ_STRING)return ref;
 union dvm_value text={0};
 if(dvm__obj(v,gc_ref_o_22)&&dvm__obj(v,gc_ref_o_22)->cls){
  struct dvm_method *m=dvm_find_method(v,dvm__obj(v,gc_ref_o_22)->cls,"toString","()Ljava/lang/String;");
  if(m&&(m->has_code||m->builtin)){if(dvm_call(v,m,ref,NULL,0,&text)&&!dvm_exception(v)&&text.l)return text.l;return 0;}
  if(java_basics_call(v,dvm__obj(v,gc_ref_o_22)->cls->name,"toString","()Ljava/lang/String;",ref,NULL,0,&text)&&!dvm_exception(v))return text.l;
 }
 return dvm_new_string(v,dvm__obj(v,gc_ref_o_22)&&dvm__obj(v,gc_ref_o_22)->cls&&dvm__obj(v,gc_ref_o_22)->cls->name?dvm__obj(v,gc_ref_o_22)->cls->name:"null");
}
static bool entry_tostring_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;const dvm_ref gc_ref_o_23=(self); if(!gc_operation_admit(v,gc_ref_o_23))return 0;entry_view *e=dvm__obj(v,gc_ref_o_23)?dvm__obj(v,gc_ref_o_23)->data:NULL;if(!e)return false;
 dvm_ref key=entry_render(v,e->key);if(dvm_exception(v))return true;
 dvm_ref value=entry_render(v,entry_current_value(v,e));if(dvm_exception(v))return true;
 if(!key||!value){dvm__throw(v,"java/lang/OutOfMemoryError","Map.Entry toString");return true;}
 const char *ks=dvm_string_utf8(v,key),*vs=dvm_string_utf8(v,value);
 if(!ks)ks="null";if(!vs)vs="null";
 int size=snprintf(NULL,0,"%s=%s",ks,vs);
 if(size<0){dvm__throw(v,"java/lang/IllegalStateException","Map.Entry toString formatting");return true;}
 char *text=malloc((size_t)size+1);
 if(!text){dvm__throw(v,"java/lang/OutOfMemoryError","Map.Entry toString buffer");return true;}
 snprintf(text,(size_t)size+1,"%s=%s",ks,vs);
 out->l=dvm_new_string_n(v,text,(size_t)size);free(text);
 if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Map.Entry string");
 return true;
}
static bool entry_set_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;const dvm_ref gc_ref_o_24=(self); if(!gc_operation_admit(v,gc_ref_o_24))return 0;entry_set_view *s=dvm__obj(v,gc_ref_o_24)?dvm__obj(v,gc_ref_o_24)->data:NULL;if(!s)return false;
 struct rt_map *mp=entry_map_data(v,s->map);
 if(IS("size","()I")){out->i=mp?(int32_t)mp->size:0;return true;}
 if(IS("isEmpty","()Z")){out->i=!(mp&&mp->size);return true;}
 if(IS("contains","(Ljava/lang/Object;)Z")){
  dvm_ref key=0,value=0;out->i=0;
  if(!entry_read_pair(v,a[0].l,&key,&value))return true;
  int64_t at=map_find_key(v,s->map,key);
  if(dvm_exception(v))return true;
  mp=entry_map_data(v,s->map);
  if(mp&&at>=0&&(uint32_t)at<mp->size)out->i=java_basics_equal(v,mp->e[at].v,value)&&!dvm_exception(v);
  return true;
 }
 if(IS("remove","(Ljava/lang/Object;)Z")){
  dvm_ref key=0,value=0;out->i=0;
  if(!entry_read_pair(v,a[0].l,&key,&value))return true;
  int64_t at=map_find_key(v,s->map,key);
  if(dvm_exception(v))return true;
  if(at>=0){mp=entry_map_data(v,s->map);if(mp&&(uint32_t)at<mp->size){bool match=java_basics_equal(v,mp->e[at].v,value);if(dvm_exception(v))return true;if(match){map_erase_at(v,s->map,(uint32_t)at);out->i=1;}}}
  return true; /* HashMap.EntrySet.remove requires both key and value to match. */
 }
 if(IS("clear","()V"))return map_clear(v,s->map,NULL,0,out);
 if(IS("iterator","()Ljava/util/Iterator;")){
  struct dvm_class *cls=dvm_find_class_or_external(v,"sdktrace/collections/EntryIterator");
  entry_iter_view *it=calloc(1,sizeof *it);
  if(!cls||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","entrySet iterator");return true;}
  dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_io_25=(ref); if(!gc_operation_admit(v,gc_ref_io_25))return 0;
  if(!dvm__obj(v,gc_ref_io_25)){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","entrySet iterator object");return true;}
  it->vm=v;it->owner=self;it->expected=mp?mp->mod_count:0;it->last=-1;
  dvm_pin(v,self);
  dvm__obj(v,gc_ref_io_25)->data=it;dvm__obj(v,gc_ref_io_25)->data_destroy=entry_iter_destroy;
  out->l=ref;return true;
 }
 if(IS("toArray","()[Ljava/lang/Object;")){
  uint32_t count=mp?mp->size:0;
  dvm_ref array=dvm_new_array(v,'L',"Ljava/lang/Object;",count);
  if(!array){dvm__throw(v,"java/lang/OutOfMemoryError","entrySet array");return true;}
  dvm_ref *items=dvm_array_data(v,array);
  for(uint32_t i=0;i<count;i++){items[i]=entry_new(v,s->map,mp->e[i].k,mp->e[i].v);if(!items[i])return true;}
  out->l=array;return true;
 }
 if(IS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","entrySet.toArray array");return true;}
  dvm_ref gc_ref_array_26=(a[0].l); if(!gc_operation_admit(v,gc_ref_array_26))return 0;
  if(!dvm__obj(v,gc_ref_array_26)||dvm__obj(v,gc_ref_array_26)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_26)->elem_kind!='L'){dvm__throw(v,"java/lang/ArrayStoreException","entrySet.toArray requires an object array");return true;}
  uint32_t count=mp?mp->size:0;dvm_ref result=a[0].l;
  if(dvm__obj(v,gc_ref_array_26)->length<count){const char *elem_desc=(dvm__obj(v,gc_ref_array_26)->cls&&dvm__obj(v,gc_ref_array_26)->cls->desc&&dvm__obj(v,gc_ref_array_26)->cls->desc[0]=='[')?dvm__obj(v,gc_ref_array_26)->cls->desc+1:"Ljava/lang/Object;";result=dvm_new_array(v,'L',elem_desc,count);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","entrySet array");return true;}gc_ref_array_26=result; if(!gc_operation_admit(v,gc_ref_array_26))return true;}
  dvm_ref *items=dvm_array_data(v,result);
  for(uint32_t i=0;i<count;i++){items[i]=entry_new(v,s->map,mp->e[i].k,mp->e[i].v);if(!items[i])return true;}
  if(dvm__obj(v,gc_ref_array_26)->length>count)items[count]=0;
  out->l=result;return true;
 }
 if(IS("add","(Ljava/lang/Object;)Z")||IS("addAll","(Ljava/util/Collection;)Z")){dvm__throw(v,"java/lang/UnsupportedOperationException","Map.entrySet does not support add");return true;}
 return false;
}
static bool entry_iter_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;(void)a;const dvm_ref gc_ref_o_27=(self); if(!gc_operation_admit(v,gc_ref_o_27))return 0;entry_iter_view *it=dvm__obj(v,gc_ref_o_27)?dvm__obj(v,gc_ref_o_27)->data:NULL;if(!it)return false;
 const dvm_ref gc_ref_owner_28=(it->owner); if(!gc_operation_admit(v,gc_ref_owner_28))return 0;entry_set_view *s=dvm__obj(v,gc_ref_owner_28)&&dvm__obj(v,gc_ref_owner_28)->data_destroy==entry_set_destroy?dvm__obj(v,gc_ref_owner_28)->data:NULL;
 struct rt_map *mp=s?entry_map_data(v,s->map):NULL;
 if(!s||!mp){dvm__throw(v,"java/lang/IllegalStateException","entrySet iterator detached");return true;}
 if(IS("hasNext","()Z")){out->i=it->index<mp->size;return true;}
 if(IS("next","()Ljava/lang/Object;")){
  if(it->expected!=mp->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","entrySet iterator");return true;}
  if(it->index>=mp->size){dvm__throw(v,"java/util/NoSuchElementException","entrySet iterator exhausted");return true;}
  uint32_t at=it->index;
  dvm_ref entry=entry_new(v,s->map,mp->e[at].k,mp->e[at].v);
  if(!entry)return true;
  it->last=(int64_t)at;it->index=at+1;out->l=entry;return true;
 }
 if(IS("remove","()V")){
  if(it->last<0){dvm__throw(v,"java/lang/IllegalStateException","iterator.remove requires next");return true;}
  if(it->expected!=mp->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","entrySet iterator");return true;}
  map_erase_at(v,s->map,(uint32_t)it->last);
  it->index=(uint32_t)it->last;it->last=-1;
  mp=entry_map_data(v,s->map);it->expected=mp?mp->mod_count:it->expected;
  return true;
 }
 return false;
}
/* ---- Map.keySet() backed view ----------------------------------------
 * keySet() is a live view over the same rt_map.e[] storage as entrySet(),
 * not a HashSet snapshot: size/isEmpty/contains/remove/clear read and mutate
 * the backing map, and iteration walks the map's own slots.  Iteration order
 * therefore follows this Map implementation's storage order (insertion order
 * for HashMap/LinkedHashMap here) and makes no Java-specified order promise.
 * add()/addAll() are rejected exactly as HashMap.KeySet does. */
typedef struct {struct dvm *vm;dvm_ref map;} key_set_view;
typedef struct {struct dvm *vm;dvm_ref owner;uint32_t expected,index;int64_t last;} key_iter_view;

static void key_set_destroy(void *p){key_set_view *s=p;if(s){if(s->map)dvm_unpin(s->vm,s->map);free(s);}}
static void key_iter_destroy(void *p){key_iter_view *i=p;if(i){if(i->owner)dvm_unpin(i->vm,i->owner);free(i);}}

static bool key_set_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;const dvm_ref gc_ref_o_29=(self); if(!gc_operation_admit(v,gc_ref_o_29))return 0;key_set_view *s=dvm__obj(v,gc_ref_o_29)?dvm__obj(v,gc_ref_o_29)->data:NULL;if(!s)return false;
 struct rt_map *mp=entry_map_data(v,s->map);
 if(IS("size","()I")){out->i=mp?(int32_t)mp->size:0;return true;}
 if(IS("isEmpty","()Z")){out->i=!(mp&&mp->size);return true;}
 if(IS("contains","(Ljava/lang/Object;)Z")){
  int64_t at=map_find_key(v,s->map,a[0].l);
  if(dvm_exception(v))return true;
  out->i=at>=0;return true;
 }
 if(IS("remove","(Ljava/lang/Object;)Z")){
  out->i=0;
  int64_t at=map_find_key(v,s->map,a[0].l);
  if(dvm_exception(v))return true;
  if(at>=0){map_erase_at(v,s->map,(uint32_t)at);out->i=1;}
  return true;
 }
 if(IS("clear","()V"))return map_clear(v,s->map,NULL,0,out);
 if(IS("iterator","()Ljava/util/Iterator;")){
  struct dvm_class *cls=dvm_find_class_or_external(v,"sdktrace/collections/KeyIterator");
  key_iter_view *it=calloc(1,sizeof *it);
  if(!cls||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","keySet iterator");return true;}
  dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_io_30=(ref); if(!gc_operation_admit(v,gc_ref_io_30))return 0;
  if(!dvm__obj(v,gc_ref_io_30)){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","keySet iterator object");return true;}
  it->vm=v;it->owner=self;it->expected=mp?mp->mod_count:0;it->last=-1;
  dvm_pin(v,self);
  dvm__obj(v,gc_ref_io_30)->data=it;dvm__obj(v,gc_ref_io_30)->data_destroy=key_iter_destroy;
  out->l=ref;return true;
 }
 if(IS("toArray","()[Ljava/lang/Object;")){
  uint32_t count=mp?mp->size:0;
  dvm_ref array=dvm_new_array(v,'L',"Ljava/lang/Object;",count);
  if(!array){dvm__throw(v,"java/lang/OutOfMemoryError","keySet array");return true;}
  dvm_ref *items=dvm_array_data(v,array);
  for(uint32_t i=0;i<count;i++)items[i]=mp->e[i].k;
  out->l=array;return true;
 }
 if(IS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","keySet.toArray array");return true;}
  dvm_ref gc_ref_array_31=(a[0].l); if(!gc_operation_admit(v,gc_ref_array_31))return 0;
  if(!dvm__obj(v,gc_ref_array_31)||dvm__obj(v,gc_ref_array_31)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_31)->elem_kind!='L'){dvm__throw(v,"java/lang/ArrayStoreException","keySet.toArray requires an object array");return true;}
  uint32_t count=mp?mp->size:0;dvm_ref result=a[0].l;
  if(dvm__obj(v,gc_ref_array_31)->length<count){const char *elem_desc=(dvm__obj(v,gc_ref_array_31)->cls&&dvm__obj(v,gc_ref_array_31)->cls->desc&&dvm__obj(v,gc_ref_array_31)->cls->desc[0]=='[')?dvm__obj(v,gc_ref_array_31)->cls->desc+1:"Ljava/lang/Object;";result=dvm_new_array(v,'L',elem_desc,count);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","keySet array");return true;}gc_ref_array_31=result; if(!gc_operation_admit(v,gc_ref_array_31))return true;}
  dvm_ref *items=dvm_array_data(v,result);
  for(uint32_t i=0;i<count;i++)items[i]=mp->e[i].k;
  if(dvm__obj(v,gc_ref_array_31)->length>count)items[count]=0;
  out->l=result;return true;
 }
 if(IS("add","(Ljava/lang/Object;)Z")||IS("addAll","(Ljava/util/Collection;)Z")){dvm__throw(v,"java/lang/UnsupportedOperationException","Map.keySet does not support add");return true;}
 return false;
}
static bool key_iter_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;(void)a;const dvm_ref gc_ref_o_32=(self); if(!gc_operation_admit(v,gc_ref_o_32))return 0;key_iter_view *it=dvm__obj(v,gc_ref_o_32)?dvm__obj(v,gc_ref_o_32)->data:NULL;
 const dvm_ref gc_ref_owner_33=(it?it->owner:0); if(!gc_operation_admit(v,gc_ref_owner_33))return 0;key_set_view *s=dvm__obj(v,gc_ref_owner_33)&&dvm__obj(v,gc_ref_owner_33)->data_destroy==key_set_destroy?dvm__obj(v,gc_ref_owner_33)->data:NULL;
 struct rt_map *mp=s?entry_map_data(v,s->map):NULL;
 if(!it||!s||!mp){dvm__throw(v,"java/lang/IllegalStateException","keySet iterator detached");return true;}
 if(IS("hasNext","()Z")){out->i=it->index<mp->size;return true;}
 if(IS("next","()Ljava/lang/Object;")){
  if(it->expected!=mp->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","keySet iterator");return true;}
  if(it->index>=mp->size){dvm__throw(v,"java/util/NoSuchElementException","keySet iterator exhausted");return true;}
  uint32_t at=it->index;
  out->l=mp->e[at].k;it->last=(int64_t)at;it->index=at+1;return true;
 }
 if(IS("remove","()V")){
  if(it->last<0){dvm__throw(v,"java/lang/IllegalStateException","iterator.remove requires next");return true;}
  if(it->expected!=mp->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","keySet iterator");return true;}
  map_erase_at(v,s->map,(uint32_t)it->last);
  it->index=(uint32_t)it->last;it->last=-1;
  mp=entry_map_data(v,s->map);it->expected=mp?mp->mod_count:it->expected;
  return true;
 }
 return false;
}
static bool entry_install_method(struct dvm_class *cls,int index,const char *name,const char *sig,dvm_builtin_fn fn,char ret,int args,int slots){
 struct dvm_method *method=&cls->methods[index];
 method->cls=cls;method->name=name;method->sig=strdup(sig);method->builtin=fn;method->ret_kind=ret;method->arg_count=args;method->arg_slots=slots;
 return method->sig!=NULL;
}

/* JNI GetMethodID needs declarations, not just collections_call's C switch.
 * Keep interface declarations abstract and install the concrete HashMap
 * methods on HashMap, preserving inherited dispatch for LinkedHashMap. */
static bool ordinary_map_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 return m&&collections_call(v,m->cls->name,m->name,m->sig,self,a,n,out);
}
static void ordinary_map_methods_vm_start(struct dvm *v){
 static const struct {const char *name,*sig;char ret;int args,slots;} methods[]={
  {"get","(Ljava/lang/Object;)Ljava/lang/Object;",'L',1,1},
  {"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",'L',2,2},
  {"remove","(Ljava/lang/Object;)Ljava/lang/Object;",'L',1,1},
  {"containsKey","(Ljava/lang/Object;)Z",'Z',1,1},
  {"containsValue","(Ljava/lang/Object;)Z",'Z',1,1},
  {"size","()I",'I',0,0},{"isEmpty","()Z",'Z',0,0},
  {"clear","()V",'V',0,0},{"putAll","(Ljava/util/Map;)V",'V',1,1},
  {"entrySet","()Ljava/util/Set;",'L',0,0},
  {"keySet","()Ljava/util/Set;",'L',0,0},
  {"values","()Ljava/util/Collection;",'L',0,0},
  {"getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",'L',2,2},
  {"equals","(Ljava/lang/Object;)Z",'Z',1,1},
  {"hashCode","()I",'I',0,0},{"toString","()Ljava/lang/String;",'L',0,0},
  {"<init>","()V",'V',0,0},{"<init>","(I)V",'V',1,1},
  {"<init>","(IF)V",'V',2,2},{"<init>","(Ljava/util/Map;)V",'V',1,1}
 };
 const char *classes[]={"Ljava/util/Map;","Ljava/util/HashMap;","Ljava/util/WeakHashMap;"};
 for(int t=0;t<3;t++){
  struct dvm_class *c=dvm__class_by_desc(v,classes[t]);if(!c)return;
  int count=t?(int)(sizeof methods/sizeof methods[0]):14;
  /* These are platform classes; avoid reallocating metadata once method IDs
   * can have escaped. Existing declared methods must never be overwritten. */
  if(c->nmethods)continue;
  c->methods=calloc((size_t)count,sizeof *c->methods);
  if(!c->methods){dvm__throw(v,"java/lang/OutOfMemoryError","Map method metadata");return;}
  c->nmethods=count;
  for(int i=0;i<count;i++){
   int j=(!t&&i>=12)?i+1:i; /* Leave default getOrDefault and Object.toString undeclared here. */
   if(!entry_install_method(c,i,methods[j].name,methods[j].sig,t?ordinary_map_builtin:NULL,methods[j].ret,methods[j].args,methods[j].slots)){
    dvm__throw(v,"java/lang/OutOfMemoryError","Map method signature");return;
   }
   c->methods[i].access=DEX_ACC_PUBLIC|(t?0:DEX_ACC_ABSTRACT);
  }
 }
}
static struct dvm_class *entry_register_class(struct dvm *v,const char *name,const char *desc,const char *const *ifaces,int n){
 (void)name;
 /* Do not probe with dvm_find_class() first: that would create a synthesized
  * external placeholder which dvm__register_builtin() would then shadow.
  * entry_views_vm_start runs once per VM, so there is no duplicate path. */
 struct dvm_class *cls=dvm__register_builtin(v,desc);
 if(!cls){dvm__throw(v,"java/lang/OutOfMemoryError","entry view class");return NULL;}
 cls->super=dvm_find_class_or_external(v,"java/lang/Object");
 if(n){
  cls->ifaces=calloc((size_t)n,sizeof *cls->ifaces);
  if(!cls->ifaces){dvm__throw(v,"java/lang/OutOfMemoryError","entry view interfaces");return cls;}
  for(int i=0;i<n;i++)cls->ifaces[i]=dvm_find_class_or_external(v,ifaces[i]);
  cls->nifaces=n;
 }
 return cls;
}
/* ---- Map values()/equals()/hashCode()/toString() ---------------------
 * HashMap/LinkedHashMap had no values() view and no Object-level map
 * contract.  Collections.synchronizedMap() delegates every Map method to the
 * backing map, so those gaps became wrapper failures.  The helpers below keep
 * the data in the existing rt_map storage and reuse the key/entry views'
 * equality and rendering helpers; they are not a second Map implementation. */
static bool map_equals(struct dvm *v,dvm_ref self,dvm_ref other,union dvm_value *out);
static bool map_hashcode(struct dvm *v,dvm_ref self,union dvm_value *out);
static bool map_tostring(struct dvm *v,dvm_ref self,union dvm_value *out);

typedef struct {struct dvm *vm;dvm_ref map;} values_view;
typedef struct {struct dvm *vm;dvm_ref owner;uint32_t expected,index;int64_t last;} value_iter_view;

static void values_view_destroy(void *p){values_view *s=p;if(s){if(s->map)dvm_unpin(s->vm,s->map);free(s);}}
static void value_iter_destroy(void *p){value_iter_view *i=p;if(i){if(i->owner)dvm_unpin(i->vm,i->owner);free(i);}}
/* Live Collection view over the map's rt_map.e[].v slots: later put/remove
 * calls are immediately visible, exactly like HashMap.values().  Iteration
 * order is this Map's storage order and carries no Java-specified promise.
 * add()/addAll() are unsupported, matching HashMap.Values. */
static bool values_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;const dvm_ref gc_ref_o_34=(self); if(!gc_operation_admit(v,gc_ref_o_34))return 0;values_view *s=dvm__obj(v,gc_ref_o_34)?dvm__obj(v,gc_ref_o_34)->data:NULL;if(!s)return false;
 struct rt_map *mp=entry_map_data(v,s->map);
 if(IS("size","()I")){out->i=mp?(int32_t)mp->size:0;return true;}
 if(IS("isEmpty","()Z")){out->i=!(mp&&mp->size);return true;}
 if(IS("contains","(Ljava/lang/Object;)Z")){
  out->i=0;if(!mp)return true;
  for(uint32_t i=0;i<mp->size;++i){bool hit=java_basics_equal(v,a[0].l,mp->e[i].v);if(dvm_exception(v))return true;if(hit){out->i=1;break;}}
  return true;
 }
 if(IS("iterator","()Ljava/util/Iterator;")){
  struct dvm_class *cls=dvm_find_class_or_external(v,"sdktrace/collections/ValueIterator");
  value_iter_view *it=calloc(1,sizeof *it);
  if(!cls||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","values iterator");return true;}
  dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_io_35=(ref); if(!gc_operation_admit(v,gc_ref_io_35))return 0;
  if(!dvm__obj(v,gc_ref_io_35)){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","values iterator object");return true;}
  it->vm=v;it->owner=self;it->expected=mp?mp->mod_count:0;it->last=-1;
  dvm_pin(v,self);
  dvm__obj(v,gc_ref_io_35)->data=it;dvm__obj(v,gc_ref_io_35)->data_destroy=value_iter_destroy;
  out->l=ref;return true;
 }
 if(IS("toArray","()[Ljava/lang/Object;")){
  uint32_t count=mp?mp->size:0;dvm_ref array=dvm_new_array(v,'L',"Ljava/lang/Object;",count);
  if(!array){dvm__throw(v,"java/lang/OutOfMemoryError","values array");return true;}
  dvm_ref *items=dvm_array_data(v,array);
  for(uint32_t i=0;i<count;i++)items[i]=mp->e[i].v;
  out->l=array;return true;
 }
 if(IS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","values.toArray array");return true;}
  dvm_ref gc_ref_array_36=(a[0].l); if(!gc_operation_admit(v,gc_ref_array_36))return 0;
  if(!dvm__obj(v,gc_ref_array_36)||dvm__obj(v,gc_ref_array_36)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_36)->elem_kind!='L'){dvm__throw(v,"java/lang/ArrayStoreException","values.toArray requires an object array");return true;}
  uint32_t count=mp?mp->size:0;dvm_ref result=a[0].l;
  if(dvm__obj(v,gc_ref_array_36)->length<count){const char *elem_desc=(dvm__obj(v,gc_ref_array_36)->cls&&dvm__obj(v,gc_ref_array_36)->cls->desc&&dvm__obj(v,gc_ref_array_36)->cls->desc[0]=='[')?dvm__obj(v,gc_ref_array_36)->cls->desc+1:"Ljava/lang/Object;";result=dvm_new_array(v,'L',elem_desc,count);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","values array");return true;}gc_ref_array_36=result; if(!gc_operation_admit(v,gc_ref_array_36))return true;}
  dvm_ref *items=dvm_array_data(v,result);
  for(uint32_t i=0;i<count;i++)items[i]=mp->e[i].v;
  if(dvm__obj(v,gc_ref_array_36)->length>count)items[count]=0;
  out->l=result;return true;
 }
 if(IS("clear","()V"))return map_clear(v,s->map,NULL,0,out);
 if(IS("remove","(Ljava/lang/Object;)Z")){
  out->i=0;if(!mp)return true;
  for(uint32_t i=0;i<mp->size;++i){bool hit=java_basics_equal(v,a[0].l,mp->e[i].v);if(dvm_exception(v))return true;if(hit){map_erase_at(v,s->map,i);out->i=1;break;}}
  return true;
 }
 if(IS("add","(Ljava/lang/Object;)Z")||IS("addAll","(Ljava/util/Collection;)Z")){dvm__throw(v,"java/lang/UnsupportedOperationException","Map.values does not support add");return true;}
 return false;
}
static bool values_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 return m&&values_call(v,m->name,m->sig,self,a,n,out);
}
static bool value_iter_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;(void)a;const dvm_ref gc_ref_o_37=(self); if(!gc_operation_admit(v,gc_ref_o_37))return 0;value_iter_view *it=dvm__obj(v,gc_ref_o_37)?dvm__obj(v,gc_ref_o_37)->data:NULL;
 const dvm_ref gc_ref_owner_38=(it?it->owner:0); if(!gc_operation_admit(v,gc_ref_owner_38))return 0;values_view *s=dvm__obj(v,gc_ref_owner_38)&&dvm__obj(v,gc_ref_owner_38)->data_destroy==values_view_destroy?dvm__obj(v,gc_ref_owner_38)->data:NULL;
 struct rt_map *mp=s?entry_map_data(v,s->map):NULL;
 if(!it||!s||!mp){dvm__throw(v,"java/lang/IllegalStateException","values iterator detached");return true;}
 if(IS("hasNext","()Z")){out->i=it->index<mp->size;return true;}
 if(IS("next","()Ljava/lang/Object;")){
  if(it->expected!=mp->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","values iterator");return true;}
  if(it->index>=mp->size){dvm__throw(v,"java/util/NoSuchElementException","values iterator exhausted");return true;}
  uint32_t at=it->index;
  out->l=mp->e[at].v;it->last=(int64_t)at;it->index=at+1;return true;
 }
 if(IS("remove","()V")){
  if(it->last<0){dvm__throw(v,"java/lang/IllegalStateException","iterator.remove requires next");return true;}
  if(it->expected!=mp->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","values iterator");return true;}
  map_erase_at(v,s->map,(uint32_t)it->last);
  it->index=(uint32_t)it->last;it->last=-1;
  mp=entry_map_data(v,s->map);it->expected=mp?mp->mod_count:it->expected;
  return true;
 }
 return false;
}
static void map_values_vm_start(struct dvm *v){
 const char *collection_ifaces[]={"java/util/Collection"},*iterator_ifaces[]={"java/util/Iterator"};
 struct dvm_class *values=entry_register_class(v,"sdktrace/collections/Values","Lsdktrace/collections/Values;",collection_ifaces,1);
 if(!values)return;
 if(!values->nmethods){
  values->methods=calloc(10,sizeof *values->methods);
  if(!values->methods){dvm__throw(v,"java/lang/OutOfMemoryError","Values methods");return;}
  values->nmethods=10;
  if(!entry_install_method(values,0,"size","()I",values_builtin,'I',0,0)||
     !entry_install_method(values,1,"isEmpty","()Z",values_builtin,'Z',0,0)||
     !entry_install_method(values,2,"contains","(Ljava/lang/Object;)Z",values_builtin,'Z',1,1)||
     !entry_install_method(values,3,"iterator","()Ljava/util/Iterator;",values_builtin,'L',0,0)||
     !entry_install_method(values,4,"toArray","()[Ljava/lang/Object;",values_builtin,'L',0,0)||
     !entry_install_method(values,5,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",values_builtin,'L',1,1)||
     !entry_install_method(values,6,"clear","()V",values_builtin,'V',0,0)||
     !entry_install_method(values,7,"remove","(Ljava/lang/Object;)Z",values_builtin,'Z',1,1)||
     !entry_install_method(values,8,"add","(Ljava/lang/Object;)Z",values_builtin,'Z',1,1)||
     !entry_install_method(values,9,"addAll","(Ljava/util/Collection;)Z",values_builtin,'Z',1,1)){
   dvm__throw(v,"java/lang/OutOfMemoryError","Values method signature");return;
  }
 }
 entry_register_class(v,"sdktrace/collections/ValueIterator","Lsdktrace/collections/ValueIterator;",iterator_ifaces,1);
}
/* AbstractMap-style contracts over the existing rt_map storage.  Keys and
 * values are hashed/rendered through the same helpers used by Map.Entry, so
 * boxed values, strings and guest toString()/hashCode() keep their own
 * semantics.  A pending exception stops the walk and leaves the result to the
 * interpreter. */
static bool map_hashcode(struct dvm *v,dvm_ref self,union dvm_value *out){
 const dvm_ref gc_ref_o_39=(self); if(!gc_operation_admit(v,gc_ref_o_39))return 0;struct rt_map *mp=dvm__obj(v,gc_ref_o_39)?dvm__obj(v,gc_ref_o_39)->data:NULL;
 if(!mp){dvm__throw(v,"java/lang/IllegalStateException","HashMap constructor not called");return true;}
 uint32_t hash=0;
 for(uint32_t i=0;i<mp->size;++i){
  /* Guest hashCode() callbacks may mutate the map; re-check the slot. */
  uint32_t key_hash=entry_hash_part(v,mp->e[i].k);if(dvm_exception(v))return true;
  if(i>=mp->size)break;
  uint32_t value_hash=entry_hash_part(v,mp->e[i].v);if(dvm_exception(v))return true;
  hash+=key_hash^value_hash;
 }
 out->i=(int32_t)hash;return true;
}
static bool map_tostring(struct dvm *v,dvm_ref self,union dvm_value *out){
 const dvm_ref gc_ref_o_40=(self); if(!gc_operation_admit(v,gc_ref_o_40))return 0;struct rt_map *mp=dvm__obj(v,gc_ref_o_40)?dvm__obj(v,gc_ref_o_40)->data:NULL;
 if(!mp){dvm__throw(v,"java/lang/IllegalStateException","HashMap constructor not called");return true;}
 size_t len=0,cap=32;char *text=malloc(cap);
 if(!text){dvm__throw(v,"java/lang/OutOfMemoryError","Map.toString buffer");return true;}
 text[len++]='{';text[len]='\0';
 for(uint32_t i=0;i<mp->size;++i){
  dvm_ref key=entry_render(v,mp->e[i].k);
  if(!key){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Map key toString");free(text);return true;}
  /* A guest toString() may mutate the map; never index a vanished slot. */
  if(i>=mp->size)break;
  dvm_ref value=entry_render(v,mp->e[i].v);
  if(!value){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Map value toString");free(text);return true;}
  const char *key_text=dvm_string_utf8(v,key),*value_text=dvm_string_utf8(v,value);
  size_t key_len=dvm_string_utf8_length(v,key),value_len=dvm_string_utf8_length(v,value);
  if(!key_text){key_text="null";key_len=4;}if(!value_text){value_text="null";value_len=4;}
  size_t need=(i?2:0)+key_len+1+value_len;
  if(len+need+2>cap){size_t next=cap;while(len+need+2>next)next*=2;char *grown=realloc(text,next);if(!grown){free(text);dvm__throw(v,"java/lang/OutOfMemoryError","Map.toString buffer");return true;}text=grown;cap=next;}
  if(i){text[len++]=',';text[len++]=' ';}
  memcpy(text+len,key_text,key_len);len+=key_len;
  text[len++]='=';
  memcpy(text+len,value_text,value_len);len+=value_len;
 }
 text[len++]='}';text[len]='\0';
 out->l=dvm_new_string_n(v,text,len);free(text);
 if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Map.toString");
 return true;
}
static bool map_equals(struct dvm *v,dvm_ref self,dvm_ref other,union dvm_value *out){
 if(other==self){out->i=1;return true;}
 const dvm_ref gc_ref_o_41=(self); if(!gc_operation_admit(v,gc_ref_o_41))return 0;struct rt_map *mp=dvm__obj(v,gc_ref_o_41)?dvm__obj(v,gc_ref_o_41)->data:NULL;
 if(!mp){dvm__throw(v,"java/lang/IllegalStateException","HashMap constructor not called");return true;}
 out->i=0;if(!other)return true;
 const dvm_ref gc_ref_other_object_42=(other); if(!gc_operation_admit(v,gc_ref_other_object_42))return 0;
 struct dvm_class *map_iface=dvm_find_class_or_external(v,"java/util/Map");
 if(!dvm__obj(v,gc_ref_other_object_42)||!dvm__obj(v,gc_ref_other_object_42)->cls||!map_iface||!dvm__class_assignable(v,dvm__obj(v,gc_ref_other_object_42)->cls,map_iface))return true;
 union dvm_value size={0};
 if(!collections_call(v,dvm__obj(v,gc_ref_other_object_42)->cls->name,"size","()I",other,NULL,0,&size))return false;
 if(dvm_exception(v))return true;
 if(size.i!=(int32_t)mp->size)return true;
 for(uint32_t i=0;i<mp->size;++i){
  union dvm_value key={.l=mp->e[i].k},has={0},value={0};
  if(!collections_call(v,dvm__obj(v,gc_ref_other_object_42)->cls->name,"containsKey","(Ljava/lang/Object;)Z",other,&key,1,&has))return false;
  if(dvm_exception(v))return true;
  if(!has.i)return true;
  if(!collections_call(v,dvm__obj(v,gc_ref_other_object_42)->cls->name,"get","(Ljava/lang/Object;)Ljava/lang/Object;",other,&key,1,&value))return false;
  if(dvm_exception(v))return true;
  /* Other Map's get() may have reentered and mutated this map. */
  if(i>=mp->size)return true;
  if(!java_basics_equal(v,mp->e[i].v,value.l))return true;
  if(dvm_exception(v))return true;
 }
 out->i=1;return true;
}
/* ---- Collections.synchronizedMap(Map) -------------------------------
 * The wrapper owns a per-instance recursive mutex (the JDK monitor is
 * reentrant, and synchronizedMap(synchronizedMap(x)) nests locks) and a
 * pinned reference to the backing Map.  Every supported Map call is made
 * while holding that mutex and is delegated through collections_call() to
 * the backing map's own runtime, so duplicate keys, null handling and
 * UnsupportedOperationException behavior stay the backing map's.  Unknown
 * signatures and foreign receivers return false without taking over.
 *
 * keySet()/values()/entrySet() return the backing map's live views.  The JDK
 * returns SynchronizedSet/SynchronizedCollection views sharing the same
 * mutex; here map-level calls are serialized by wrapper_lock but view methods
 * are not re-locked, so iteration must still be externally serialized with
 * the wrapper, exactly as the Java documentation requires for iteration.
 * Views are never null or empty stand-ins.
 *
 * Locking boundary: delegating to another SynchronizedMap acquires the inner
 * instance's mutex while the outer mutex is held, matching nested monitor
 * acquisition order.  No lock is held across any other blocking operation. */
typedef struct {struct dvm *vm;dvm_ref backing;pthread_mutex_t lock;bool lock_ready;} synchronized_map_view;

static void synchronized_map_destroy(void *p){
 synchronized_map_view *s=p;
 if(!s)return;
 if(s->lock_ready)(void)pthread_mutex_destroy(&s->lock);
 if(s->backing)dvm_unpin(s->vm,s->backing);
 free(s);
}
static bool synchronized_map_signature(const char *m,const char *sig){
 return IS("size","()I")||IS("isEmpty","()Z")||
  IS("containsKey","(Ljava/lang/Object;)Z")||IS("containsValue","(Ljava/lang/Object;)Z")||
  IS("get","(Ljava/lang/Object;)Ljava/lang/Object;")||
  IS("put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")||
  IS("remove","(Ljava/lang/Object;)Ljava/lang/Object;")||
  IS("putAll","(Ljava/util/Map;)V")||IS("clear","()V")||
  IS("keySet","()Ljava/util/Set;")||IS("values","()Ljava/util/Collection;")||
  IS("entrySet","()Ljava/util/Set;")||IS("equals","(Ljava/lang/Object;)Z")||
  IS("hashCode","()I")||IS("toString","()Ljava/lang/String;");
}
static int synchronized_map_arity(const char *m,const char *sig){
 if(IS("size","()I")||IS("isEmpty","()Z")||IS("clear","()V")||
    IS("keySet","()Ljava/util/Set;")||IS("values","()Ljava/util/Collection;")||
    IS("entrySet","()Ljava/util/Set;")||IS("hashCode","()I")||IS("toString","()Ljava/lang/String;"))return 0;
 if(IS("put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"))return 2;
 if(synchronized_map_signature(m,sig))return 1;
 return -1;
}
static bool synchronized_map_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 const dvm_ref gc_ref_o_43=(self); if(!gc_operation_admit(v,gc_ref_o_43))return 0;

 synchronized_map_view *s=dvm__obj(v,gc_ref_o_43)&&dvm__obj(v,gc_ref_o_43)->data_destroy==synchronized_map_destroy?(synchronized_map_view *)dvm__obj(v,gc_ref_o_43)->data:NULL;
 if(!s)return false;
 int arity=synchronized_map_arity(m,sig);
 if(arity<0||nargs!=arity)return false;
 if(nargs>0&&!a)return false;
 /* Collections.SynchronizedMap.equals() short-circuits identity before the
  * backing map is consulted; keep that and avoid a self-recursive lock. */
 if(IS("equals","(Ljava/lang/Object;)Z")&&a[0].l==self){out->i=1;return true;}
 int rc=pthread_mutex_lock(&s->lock);
 if(rc){dvm__throw(v,"java/lang/IllegalStateException","SynchronizedMap lock failed (%d)",rc);return true;}
 const dvm_ref gc_ref_backing_44=(s->backing); if(!gc_operation_admit(v,gc_ref_backing_44))return 0;
 const char *backing_class=dvm__obj(v,gc_ref_backing_44)&&dvm__obj(v,gc_ref_backing_44)->cls&&dvm__obj(v,gc_ref_backing_44)->cls->name?dvm__obj(v,gc_ref_backing_44)->cls->name:"java/util/Map";
 bool ok=collections_call(v,backing_class,m,sig,s->backing,a,nargs,out);
 (void)pthread_mutex_unlock(&s->lock);
 return ok;
}
static bool synchronized_map_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 return m&&synchronized_map_call(v,m->name,m->sig,self,a,n,out);
}
static dvm_ref synchronized_map_new(struct dvm *v,dvm_ref backing){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$SynchronizedMap");
 synchronized_map_view *s=calloc(1,sizeof *s);
 if(!cls||!s){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedMap");return 0;}
 pthread_mutexattr_t attr;
 int rc=pthread_mutexattr_init(&attr);
 if(!rc){
  rc=pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
  if(!rc)rc=pthread_mutex_init(&s->lock,&attr);
  (void)pthread_mutexattr_destroy(&attr);
 }
 if(rc){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedMap mutex (%d)",rc);return 0;}
 s->lock_ready=true;
 dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_o_45=(ref); if(!gc_operation_admit(v,gc_ref_o_45))return 0;
 if(!dvm__obj(v,gc_ref_o_45)){(void)pthread_mutex_destroy(&s->lock);free(s);dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedMap object");return 0;}
 s->vm=v;s->backing=backing;dvm_pin(v,backing);
 dvm__obj(v,gc_ref_o_45)->data=s;dvm__obj(v,gc_ref_o_45)->data_destroy=synchronized_map_destroy;
 return ref;
}
/* UnmodifiableMap and its live views retain their backing objects. Entries
 * are wrapped too, so entrySet iteration/arrays cannot expose setValue. */
enum readonly_kind {RO_MAP,RO_SET,RO_VALUES,RO_ENTRIES,RO_ITERATOR,RO_ENTRY_ITERATOR,RO_ENTRY,RO_COUNT};
typedef struct {struct dvm *vm;dvm_ref backing;enum readonly_kind kind;} readonly_view;
static const char *readonly_names[RO_COUNT]={"sdktrace/collections/UnmodifiableMap","sdktrace/collections/UnmodifiableSet","sdktrace/collections/UnmodifiableValues","sdktrace/collections/UnmodifiableEntries","sdktrace/collections/UnmodifiableIterator","sdktrace/collections/UnmodifiableEntryIterator","sdktrace/collections/UnmodifiableEntry"};
static void readonly_destroy(void *p){readonly_view *r=p;if(r){dvm_unpin(r->vm,r->backing);free(r);}}
static dvm_ref readonly_new(struct dvm *v,dvm_ref backing,enum readonly_kind kind){
 if(!backing){dvm__throw(v,"java/lang/NullPointerException","unmodifiable backing");return 0;}
 readonly_view *r=calloc(1,sizeof *r);
 dvm_ref ref=r?dvm_new_object(v,dvm_find_class_or_external(v,readonly_names[kind])):0;
 const dvm_ref gc_ref_o_46=(ref); if(!gc_operation_admit(v,gc_ref_o_46))return 0;
 if(!dvm__obj(v,gc_ref_o_46)){free(r);dvm__throw(v,"java/lang/OutOfMemoryError","unmodifiable view");return 0;}
 r->vm=v;r->backing=backing;r->kind=kind;dvm_pin(v,backing);dvm__obj(v,gc_ref_o_46)->data=r;dvm__obj(v,gc_ref_o_46)->data_destroy=readonly_destroy;return ref;
}
static bool readonly_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 const dvm_ref gc_ref_o_47=(self); if(!gc_operation_admit(v,gc_ref_o_47))return 0;readonly_view *r=dvm__obj(v,gc_ref_o_47)&&dvm__obj(v,gc_ref_o_47)->data_destroy==readonly_destroy?dvm__obj(v,gc_ref_o_47)->data:NULL;
 if(!r)return false;
 if(!strcmp(m,"put")||!strcmp(m,"putAll")||!strcmp(m,"remove")||!strcmp(m,"clear")||!strcmp(m,"add")||!strcmp(m,"addAll")||!strcmp(m,"removeAll")||!strcmp(m,"retainAll")||!strcmp(m,"setValue")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","unmodifiable view");return true;
 }
 if(IS("equals","(Ljava/lang/Object;)Z")&&a[0].l==self){out->i=1;return true;}
 /* Collection wrappers intentionally retain Object identity semantics. */
 if(r->kind==RO_VALUES&&IS("equals","(Ljava/lang/Object;)Z")){out->i=a[0].l==self;return true;}
 if(r->kind==RO_VALUES&&IS("hashCode","()I")){out->i=(int32_t)self;return true;}
 const dvm_ref gc_ref_b_48=(r->backing); if(!gc_operation_admit(v,gc_ref_b_48))return 0;
 if(!dvm__obj(v,gc_ref_b_48)||!dvm__obj(v,gc_ref_b_48)->cls)return false;
 bool ok=collections_call(v,dvm__obj(v,gc_ref_b_48)->cls->name,m,sig,r->backing,a,n,out);
 if(!ok||dvm_exception(v))return ok;
 if(IS("keySet","()Ljava/util/Set;"))out->l=readonly_new(v,out->l,RO_SET);
 else if(IS("values","()Ljava/util/Collection;"))out->l=readonly_new(v,out->l,RO_VALUES);
 else if(IS("entrySet","()Ljava/util/Set;"))out->l=readonly_new(v,out->l,RO_ENTRIES);
 else if(IS("iterator","()Ljava/util/Iterator;"))out->l=readonly_new(v,out->l,r->kind==RO_ENTRIES?RO_ENTRY_ITERATOR:RO_ITERATOR);
 else if(r->kind==RO_ENTRY_ITERATOR&&IS("next","()Ljava/lang/Object;"))out->l=readonly_new(v,out->l,RO_ENTRY);
 else if(r->kind==RO_ENTRIES&&(!strcmp(m,"toArray"))){
  const dvm_ref gc_ref_array_49=(out->l); if(!gc_operation_admit(v,gc_ref_array_49))return 0;
  if(dvm__obj(v,gc_ref_array_49)&&dvm__obj(v,gc_ref_array_49)->kind==DVM_OBJ_ARRAY){dvm_ref *items=dvm__obj(v,gc_ref_array_49)->data;for(uint32_t i=0;i<dvm__obj(v,gc_ref_array_49)->length&&items[i];++i){items[i]=readonly_new(v,items[i],RO_ENTRY);if(dvm_exception(v))break;}}
 }
 return true;
}
static bool readonly_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){struct dvm_method *m=dvm__builtin_method();return m&&readonly_call(v,m->name,m->sig,self,a,n,out);}
static void readonly_map_vm_start(struct dvm *v){
 static const struct {const char *name,*sig;char ret;int n;unsigned mask;} methods[]={
  {"size","()I",'I',0,15},{"isEmpty","()Z",'Z',0,15},
  {"containsKey","(Ljava/lang/Object;)Z",'Z',1,1},{"containsValue","(Ljava/lang/Object;)Z",'Z',1,1},
  {"get","(Ljava/lang/Object;)Ljava/lang/Object;",'L',1,1},{"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",'L',2,1},
  {"putAll","(Ljava/util/Map;)V",'V',1,1},{"remove","(Ljava/lang/Object;)Ljava/lang/Object;",'L',1,1},
  {"clear","()V",'V',0,15},{"keySet","()Ljava/util/Set;",'L',0,1},{"values","()Ljava/util/Collection;",'L',0,1},{"entrySet","()Ljava/util/Set;",'L',0,1},
  {"contains","(Ljava/lang/Object;)Z",'Z',1,14},{"containsAll","(Ljava/util/Collection;)Z",'Z',1,14},
  {"add","(Ljava/lang/Object;)Z",'Z',1,14},{"remove","(Ljava/lang/Object;)Z",'Z',1,14},
  {"addAll","(Ljava/util/Collection;)Z",'Z',1,14},{"removeAll","(Ljava/util/Collection;)Z",'Z',1,14},{"retainAll","(Ljava/util/Collection;)Z",'Z',1,14},
  {"iterator","()Ljava/util/Iterator;",'L',0,14},{"toArray","()[Ljava/lang/Object;",'L',0,14},{"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",'L',1,14},
  {"hasNext","()Z",'Z',0,48},{"next","()Ljava/lang/Object;",'L',0,48},{"remove","()V",'V',0,48},
  {"getKey","()Ljava/lang/Object;",'L',0,64},{"getValue","()Ljava/lang/Object;",'L',0,64},{"setValue","(Ljava/lang/Object;)Ljava/lang/Object;",'L',1,64},
  {"equals","(Ljava/lang/Object;)Z",'Z',1,79},{"hashCode","()I",'I',0,79},{"toString","()Ljava/lang/String;",'L',0,79}
 };
 const char *ifaces[RO_COUNT]={"java/util/Map","java/util/Set","java/util/Collection","java/util/Set","java/util/Iterator","java/util/Iterator","java/util/Map$Entry"};
 for(int k=0;k<RO_COUNT;++k){
  char desc[128];snprintf(desc,sizeof desc,"L%s;",readonly_names[k]);const char *iface[]={ifaces[k]};
  struct dvm_class *cls=entry_register_class(v,readonly_names[k],desc,iface,1);if(!cls)return;
  int count=0;for(size_t i=0;i<sizeof methods/sizeof methods[0];++i)if(methods[i].mask&(1u<<k))++count;
  cls->methods=calloc((size_t)count,sizeof *cls->methods);if(!cls->methods){dvm__throw(v,"java/lang/OutOfMemoryError","unmodifiable methods");return;}cls->nmethods=count;
  int at=0;for(size_t i=0;i<sizeof methods/sizeof methods[0];++i)if(methods[i].mask&(1u<<k))if(!entry_install_method(cls,at++,methods[i].name,methods[i].sig,readonly_builtin,methods[i].ret,methods[i].n,methods[i].n)){dvm__throw(v,"java/lang/OutOfMemoryError","unmodifiable method signature");return;}
 }
}
static void synchronized_map_vm_start(struct dvm *v){
 const char *map_ifaces[]={"java/util/Map"};
 struct dvm_class *cls=entry_register_class(v,"java/util/Collections$SynchronizedMap","Ljava/util/Collections$SynchronizedMap;",map_ifaces,1);
 if(!cls)return;
 cls->super=dvm_find_class_or_external(v,"java/util/AbstractMap");
 if(cls->nmethods)return;
 cls->methods=calloc(15,sizeof *cls->methods);
 if(!cls->methods){dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedMap methods");return;}
 cls->nmethods=15;
 if(!entry_install_method(cls,0,"size","()I",synchronized_map_builtin,'I',0,0)||
    !entry_install_method(cls,1,"isEmpty","()Z",synchronized_map_builtin,'Z',0,0)||
    !entry_install_method(cls,2,"containsKey","(Ljava/lang/Object;)Z",synchronized_map_builtin,'Z',1,1)||
    !entry_install_method(cls,3,"containsValue","(Ljava/lang/Object;)Z",synchronized_map_builtin,'Z',1,1)||
    !entry_install_method(cls,4,"get","(Ljava/lang/Object;)Ljava/lang/Object;",synchronized_map_builtin,'L',1,1)||
    !entry_install_method(cls,5,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",synchronized_map_builtin,'L',2,2)||
    !entry_install_method(cls,6,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",synchronized_map_builtin,'L',1,1)||
    !entry_install_method(cls,7,"putAll","(Ljava/util/Map;)V",synchronized_map_builtin,'V',1,1)||
    !entry_install_method(cls,8,"clear","()V",synchronized_map_builtin,'V',0,0)||
    !entry_install_method(cls,9,"keySet","()Ljava/util/Set;",synchronized_map_builtin,'L',0,0)||
    !entry_install_method(cls,10,"values","()Ljava/util/Collection;",synchronized_map_builtin,'L',0,0)||
    !entry_install_method(cls,11,"entrySet","()Ljava/util/Set;",synchronized_map_builtin,'L',0,0)||
    !entry_install_method(cls,12,"equals","(Ljava/lang/Object;)Z",synchronized_map_builtin,'Z',1,1)||
    !entry_install_method(cls,13,"hashCode","()I",synchronized_map_builtin,'I',0,0)||
    !entry_install_method(cls,14,"toString","()Ljava/lang/String;",synchronized_map_builtin,'L',0,0)){
  dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedMap method signature");return;
 }
}
/* White-box selftest for the wrapper and its delegation contracts.  It runs
 * inside the collection selftest and prints synchronized_map=1 on success. */
static bool collections_synchronized_map_selftest_operation(struct dvm *v){
 const char *hm="java/util/HashMap",*mp="java/util/Map",*set="java/util/Set",*collection="java/util/Collection";
 union dvm_value out={0},a[2]={{0},{0}};
 bool okay=true;
#define SCHECK(expr) do{if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL synchronized_map line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 /* synchronizedMap(null) must raise NullPointerException. */
 a[0].l=0;
 bool null_call=collections_call(v,"java/util/Collections","synchronizedMap","(Ljava/util/Map;)Ljava/util/Map;",0,a,1,&out);
 struct dvm_class *thrown=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 SCHECK(null_call&&thrown&&!strcmp(thrown->name,"java/lang/NullPointerException"));
 if(dvm_exception(v))dvm_clear_exception(v);
 dvm_ref back=dvm_new_object(v,dvm_find_class_or_external(v,hm));
 SCHECK(back!=0);
 SCHECK(collections_call(v,hm,"<init>","()V",back,NULL,0,&out)&&!dvm_exception(v));
 a[0].l=back;
 bool wrap_call=collections_call(v,"java/util/Collections","synchronizedMap","(Ljava/util/Map;)Ljava/util/Map;",0,a,1,&out);
 dvm_ref wrap=out.l;
 struct dvm_class *map_iface=dvm_find_class_or_external(v,mp);
 SCHECK(wrap_call&&wrap&&map_iface&&dvm__class_assignable(v,dvm_object_class(v,wrap),map_iface)&&!dvm_exception(v));
 /* put/get/containsKey/containsValue/size/isEmpty through the wrapper. */
 dvm_ref key=dvm_new_string(v,"key"),value=dvm_new_string(v,"value");
 a[0].l=key;a[1].l=value;
 SCHECK(collections_call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",wrap,a,2,&out)&&!out.l&&!dvm_exception(v));
 a[0].l=key;
 SCHECK(collections_call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",wrap,a,1,&out)&&out.l==value&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"containsKey","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=value;
 SCHECK(collections_call(v,mp,"containsValue","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"size","()I",wrap,NULL,0,&out)&&out.i==1&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"isEmpty","()Z",wrap,NULL,0,&out)&&out.i==0&&!dvm_exception(v));
 /* The wrapper and the backing map share one live storage both ways. */
 a[0].l=key;
 SCHECK(collections_call(v,hm,"get","(Ljava/lang/Object;)Ljava/lang/Object;",back,a,1,&out)&&out.l==value&&!dvm_exception(v));
 dvm_ref key2=dvm_new_string(v,"second"),value2=dvm_new_string(v,"other");
 a[0].l=key2;a[1].l=value2;
 SCHECK(collections_call(v,hm,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",back,a,2,&out)&&!dvm_exception(v));
 a[0].l=key2;
 SCHECK(collections_call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",wrap,a,1,&out)&&out.l==value2&&!dvm_exception(v));
 /* putAll delegates to HashMap.putAll for a real source map. */
 dvm_ref source=dvm_new_object(v,dvm_find_class_or_external(v,hm));
 SCHECK(source&&collections_call(v,hm,"<init>","()V",source,NULL,0,&out)&&!dvm_exception(v));
 dvm_ref key3=dvm_new_string(v,"third"),value3=dvm_new_string(v,"three");
 a[0].l=key3;a[1].l=value3;
 SCHECK(collections_call(v,hm,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",source,a,2,&out)&&!dvm_exception(v));
 a[0].l=source;
 SCHECK(collections_call(v,mp,"putAll","(Ljava/util/Map;)V",wrap,a,1,&out)&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"size","()I",wrap,NULL,0,&out)&&out.i==3&&!dvm_exception(v));
 /* keySet/entrySet/values are live views with real contents. */
 dvm_ref keys=0,entries=0,values=0;
 SCHECK(collections_call(v,mp,"keySet","()Ljava/util/Set;",wrap,NULL,0,&out)&&(keys=out.l)&&!dvm_exception(v));
 SCHECK(keys&&collections_call(v,set,"size","()I",keys,NULL,0,&out)&&out.i==3&&!dvm_exception(v));
 a[0].l=key2;
 SCHECK(keys&&collections_call(v,set,"contains","(Ljava/lang/Object;)Z",keys,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"entrySet","()Ljava/util/Set;",wrap,NULL,0,&out)&&(entries=out.l)&&!dvm_exception(v));
 SCHECK(entries&&collections_call(v,set,"size","()I",entries,NULL,0,&out)&&out.i==3&&!dvm_exception(v));
 union dvm_value iterator={0};dvm_ref entry=0;
 SCHECK(entries&&collections_call(v,set,"iterator","()Ljava/util/Iterator;",entries,NULL,0,&iterator)&&iterator.l&&!dvm_exception(v));
 SCHECK(iterator.l&&collections_call(v,"java/util/Iterator","hasNext","()Z",iterator.l,NULL,0,&out)&&out.i==1);
 SCHECK(iterator.l&&collections_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",iterator.l,NULL,0,&out)&&(entry=out.l)&&!dvm_exception(v));
 a[0].l=entry;
 SCHECK(entry&&collections_call(v,set,"contains","(Ljava/lang/Object;)Z",entries,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"values","()Ljava/util/Collection;",wrap,NULL,0,&out)&&(values=out.l)&&!dvm_exception(v));
 SCHECK(values&&collections_call(v,collection,"size","()I",values,NULL,0,&out)&&out.i==3&&!dvm_exception(v));
 a[0].l=value2;
 SCHECK(values&&collections_call(v,collection,"contains","(Ljava/lang/Object;)Z",values,a,1,&out)&&out.i==1&&!dvm_exception(v));
 /* equals/hashCode/toString follow the backing map's contract. */
 a[0].l=back;
 SCHECK(collections_call(v,mp,"equals","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 dvm_ref twin=dvm_new_object(v,dvm_find_class_or_external(v,hm));
 SCHECK(twin&&collections_call(v,hm,"<init>","()V",twin,NULL,0,&out)&&!dvm_exception(v));
 a[0].l=key;a[1].l=value;
 SCHECK(collections_call(v,hm,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",twin,a,2,&out)&&!dvm_exception(v));
 a[0].l=key2;a[1].l=value2;
 SCHECK(collections_call(v,hm,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",twin,a,2,&out)&&!dvm_exception(v));
 a[0].l=key3;a[1].l=value3;
 SCHECK(collections_call(v,hm,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",twin,a,2,&out)&&!dvm_exception(v));
 a[0].l=twin;
 SCHECK(collections_call(v,mp,"equals","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"toString","()Ljava/lang/String;",wrap,NULL,0,&out)&&out.l&&!dvm_exception(v));
 const char *text=out.l?dvm_string_utf8(v,out.l):NULL;
 SCHECK(text&&strstr(text,"key=value")&&strstr(text,"second=other")&&strstr(text,"third=three"));
 SCHECK(collections_call(v,mp,"hashCode","()I",wrap,NULL,0,&out)&&!dvm_exception(v));
 int wrapper_hash=out.i;
 SCHECK(collections_call(v,hm,"hashCode","()I",back,NULL,0,&out)&&out.i==wrapper_hash&&!dvm_exception(v));
 /* remove/clear reach the backing storage. */
 a[0].l=key;
 SCHECK(collections_call(v,mp,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",wrap,a,1,&out)&&out.l==value&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"size","()I",wrap,NULL,0,&out)&&out.i==2&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"clear","()V",wrap,NULL,0,&out)&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"isEmpty","()Z",wrap,NULL,0,&out)&&out.i==1&&!dvm_exception(v));
 SCHECK(collections_call(v,hm,"isEmpty","()Z",back,NULL,0,&out)&&out.i==1&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"toString","()Ljava/lang/String;",wrap,NULL,0,&out)&&out.l&&!dvm_exception(v));
 SCHECK(out.l&&!strcmp(dvm_string_utf8(v,out.l),"{}"));
 /* synchronizedMap(synchronizedMap(x)) delegates through both instances. */
 a[0].l=wrap;
 bool outer_call=collections_call(v,"java/util/Collections","synchronizedMap","(Ljava/util/Map;)Ljava/util/Map;",0,a,1,&out);
 dvm_ref outer=out.l;
 SCHECK(outer_call&&outer&&!dvm_exception(v));
 a[0].l=key;a[1].l=value;
 SCHECK(collections_call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",outer,a,2,&out)&&!dvm_exception(v));
 a[0].l=key;
 SCHECK(collections_call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",outer,a,1,&out)&&out.l==value&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",wrap,a,1,&out)&&out.l==value&&!dvm_exception(v));
 SCHECK(collections_call(v,hm,"get","(Ljava/lang/Object;)Ljava/lang/Object;",back,a,1,&out)&&out.l==value&&!dvm_exception(v));
 a[0].l=wrap;
 SCHECK(collections_call(v,mp,"equals","(Ljava/lang/Object;)Z",outer,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SCHECK(collections_call(v,mp,"toString","()Ljava/lang/String;",outer,NULL,0,&out)&&out.l&&!dvm_exception(v));
 SCHECK(out.l&&strstr(dvm_string_utf8(v,out.l),"key=value"));
 SCHECK(collections_call(v,mp,"size","()I",outer,NULL,0,&out)&&out.i==1&&!dvm_exception(v));
 /* The per-instance mutex must really be recursive. */
 const dvm_ref gc_ref_wrap_object_50=(wrap); if(!gc_operation_admit(v,gc_ref_wrap_object_50))return 0;
 synchronized_map_view *wrap_view=dvm__obj(v,gc_ref_wrap_object_50)&&dvm__obj(v,gc_ref_wrap_object_50)->data_destroy==synchronized_map_destroy?(synchronized_map_view *)dvm__obj(v,gc_ref_wrap_object_50)->data:NULL;
 bool recursive=false;
 if(wrap_view&&pthread_mutex_lock(&wrap_view->lock)==0){
  recursive=pthread_mutex_lock(&wrap_view->lock)==0;
  if(recursive)(void)pthread_mutex_unlock(&wrap_view->lock);
  (void)pthread_mutex_unlock(&wrap_view->lock);
 }
 SCHECK(recursive);
 /* Unknown signatures, arities and foreign receivers stay fail-closed and
  * do not raise or mutate anything. */
 a[0].l=back;
 SCHECK(!collections_call(v,"java/util/Collections","synchronizedMap","(Ljava/util/Map;)Ljava/lang/Object;",0,a,1,&out)&&!dvm_exception(v));
 SCHECK(!collections_call(v,"java/util/Collections","synchronizedMap","(Ljava/util/Map;)Ljava/util/Map;",0,a,0,&out)&&!dvm_exception(v));
 SCHECK(!collections_call(v,mp,"get","()Ljava/lang/Object;",wrap,NULL,0,&out)&&!dvm_exception(v));
 SCHECK(!collections_call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",wrap,NULL,0,&out)&&!dvm_exception(v));
 SCHECK(!collections_call(v,mp,"unimplemented","()V",wrap,NULL,0,&out)&&!dvm_exception(v));
 dvm_ref stranger=dvm_new_string(v,"not-a-map");
 SCHECK(!collections_call(v,"java/util/Collections$SynchronizedMap","get","(Ljava/lang/Object;)Ljava/lang/Object;",stranger,a,1,&out)&&!dvm_exception(v));
 if(dvm_exception(v))dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"SYNCHRONIZED_MAP_SELFTEST %s synchronized_map=1 null_npe=1 delegate_ops=1 backing_visible=1 views=1 equals_hash_tostring=1 nested=1 recursive_mutex=1 fail_closed=1\n",okay?"PASS":"FAIL");
#undef SCHECK
 return okay;
}
bool collections_synchronized_map_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=collections_synchronized_map_selftest_operation(v);
 return result;
}

/* ---- Collections.synchronizedList(List) -----------------------------
 * Same ownership model as synchronizedMap above: one recursive mutex per
 * wrapper, a pinned backing List, marker-destructor dispatch, installed
 * builtins and delegation through collections_call() into the backing List's
 * real storage.  ConnectivityMonitor and com/gsc/progurad.b.<clinit> call
 * Collections.synchronizedList() and then use size/isEmpty/contains/get/
 * add/remove/iterator/listIterator, so those calls must never fall through to
 * NoSuchMethodError.  Unknown signatures and foreign receivers fail closed.
 *
 * equals/hashCode/toString/containsAll take a snapshot of the backing list
 * through its own toArray() while the wrapper mutex is held, then apply the
 * java.util.List contract.  A backing that cannot provide toArray() makes the
 * call fail closed rather than returning an invented result.  iterator()
 * returns the backing iterator; as documented by java.util.Collections,
 * callers must hold the wrapper mutex while iterating.  listIterator() is a
 * real cursor over the backing list, and its mutators are serialized by the
 * wrapper mutex. */
typedef struct {struct dvm *vm;dvm_ref backing;pthread_mutex_t lock;bool lock_ready;} synchronized_list_view;

static void synchronized_list_destroy(void *p){
 synchronized_list_view *s=p;
 if(!s)return;
 if(s->lock_ready)(void)pthread_mutex_destroy(&s->lock);
 if(s->backing)dvm_unpin(s->vm,s->backing);
 free(s);
}
static bool synchronized_list_signature(const char *m,const char *sig){
 return IS("size","()I")||IS("isEmpty","()Z")||
  IS("contains","(Ljava/lang/Object;)Z")||
  IS("get","(I)Ljava/lang/Object;")||
  IS("set","(ILjava/lang/Object;)Ljava/lang/Object;")||
  IS("add","(Ljava/lang/Object;)Z")||IS("add","(ILjava/lang/Object;)V")||
  IS("remove","(I)Ljava/lang/Object;")||IS("remove","(Ljava/lang/Object;)Z")||
  IS("clear","()V")||
  IS("addAll","(Ljava/util/Collection;)Z")||IS("addAll","(ILjava/util/Collection;)Z")||
  IS("iterator","()Ljava/util/Iterator;")||
  IS("listIterator","()Ljava/util/ListIterator;")||IS("listIterator","(I)Ljava/util/ListIterator;")||
  IS("equals","(Ljava/lang/Object;)Z")||IS("hashCode","()I")||
  IS("toString","()Ljava/lang/String;")||
  IS("toArray","()[Ljava/lang/Object;")||IS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")||
  IS("containsAll","(Ljava/util/Collection;)Z")||
  IS("indexOf","(Ljava/lang/Object;)I")||IS("lastIndexOf","(Ljava/lang/Object;)I");
}
static int synchronized_list_arity(const char *m,const char *sig){
 if(IS("size","()I")||IS("isEmpty","()Z")||IS("clear","()V")||
    IS("iterator","()Ljava/util/Iterator;")||IS("listIterator","()Ljava/util/ListIterator;")||
    IS("hashCode","()I")||IS("toString","()Ljava/lang/String;")||
    IS("toArray","()[Ljava/lang/Object;"))return 0;
 if(IS("set","(ILjava/lang/Object;)Ljava/lang/Object;")||IS("add","(ILjava/lang/Object;)V")||
    IS("addAll","(ILjava/util/Collection;)Z"))return 2;
 if(synchronized_list_signature(m,sig))return 1;
 return -1;
}
/* Snapshot through the backing's own toArray(); rt_list backed lists use the
 * pinned storage helper directly.  The returned array is a fresh object. */
static dvm_ref synchronized_list_snapshot(struct dvm *v,synchronized_list_view *s){
 struct rt_list *l=list_of(v,s->backing);
 union dvm_value r={0};
 if(l){
  if(!list_toArray(v,s->backing,NULL,0,&r))return 0;
  return r.l;
 }
 const dvm_ref gc_ref_b_51=(s->backing); if(!gc_operation_admit(v,gc_ref_b_51))return 0;
 const char *bc=dvm__obj(v,gc_ref_b_51)&&dvm__obj(v,gc_ref_b_51)->cls&&dvm__obj(v,gc_ref_b_51)->cls->name?dvm__obj(v,gc_ref_b_51)->cls->name:"java/util/List";
 if(!collections_call(v,bc,"toArray","()[Ljava/lang/Object;",s->backing,NULL,0,&r))return 0;
 return r.l;
}
/* Element contracts shared by SynchronizedList and SingletonList.  These call
 * the element's real hashCode()/toString() implementation; when the runtime
 * cannot provide one the caller fails closed instead of inventing a value. */
static bool synchronized_list_element_hash(struct dvm *v,dvm_ref x,int32_t *hash){
 if(!x){*hash=0;return true;}
 const dvm_ref gc_ref_o_52=(x); if(!gc_operation_admit(v,gc_ref_o_52))return 0;
 const char *cn=dvm__obj(v,gc_ref_o_52)&&dvm__obj(v,gc_ref_o_52)->cls&&dvm__obj(v,gc_ref_o_52)->cls->name?dvm__obj(v,gc_ref_o_52)->cls->name:"java/lang/Object";
 union dvm_value r={0};
 if(!strcmp(cn,"java/lang/String")&&string_runtime_call(v,cn,"hashCode","()I",x,NULL,0,&r)){*hash=r.i;return true;}
 if(java_basics_call(v,cn,"hashCode","()I",x,NULL,0,&r)){*hash=r.i;return true;}
 if(dvm_exception(v))return false;
 struct dvm_method *m=dvm__obj(v,gc_ref_o_52)&&dvm__obj(v,gc_ref_o_52)->cls?dvm_find_method(v,dvm__obj(v,gc_ref_o_52)->cls,"hashCode","()I"):NULL;
 if(m&&(m->has_code||m->builtin)&&dvm_call(v,m,x,NULL,0,&r)){*hash=r.i;return true;}
 return false;
}
static bool synchronized_list_element_string(struct dvm *v,dvm_ref x,dvm_ref *result){
 if(!x){*result=dvm_new_string(v,"null");return *result!=0;}
 const dvm_ref gc_ref_o_53=(x); if(!gc_operation_admit(v,gc_ref_o_53))return 0;
 const char *cn=dvm__obj(v,gc_ref_o_53)&&dvm__obj(v,gc_ref_o_53)->cls&&dvm__obj(v,gc_ref_o_53)->cls->name?dvm__obj(v,gc_ref_o_53)->cls->name:"java/lang/Object";
 union dvm_value r={0};
 if(!strcmp(cn,"java/lang/String")){*result=x;return true;}
 if(java_basics_call(v,cn,"toString","()Ljava/lang/String;",x,NULL,0,&r)&&r.l){*result=r.l;return true;}
 if(dvm_exception(v))return false;
 struct dvm_method *m=dvm__obj(v,gc_ref_o_53)&&dvm__obj(v,gc_ref_o_53)->cls?dvm_find_method(v,dvm__obj(v,gc_ref_o_53)->cls,"toString","()Ljava/lang/String;"):NULL;
 if(m&&(m->has_code||m->builtin)&&dvm_call(v,m,x,NULL,0,&r)&&r.l){*result=r.l;return true;}
 return false;
}
static bool list_text_reserve(char **buf,size_t *cap,size_t need){
 if(need<=*cap)return true;
 size_t n=*cap?*cap:64;
 while(n<need)n*=2;
 char *p=realloc(*buf,n);
 if(!p)return false;
 *buf=p;*cap=n;return true;
}
static bool synchronized_list_join(struct dvm *v,dvm_ref array,dvm_ref *result){
 uint32_t n=dvm_array_length(v,array);dvm_ref *items=dvm_array_data(v,array);
 size_t cap=64,len=0;char *buf=malloc(cap);
 if(!buf){dvm__throw(v,"java/lang/OutOfMemoryError","List.toString");return false;}
 buf[len++]='[';
 for(uint32_t i=0;i<n;i++){
  if(i){
   if(!list_text_reserve(&buf,&cap,len+2)){free(buf);dvm__throw(v,"java/lang/OutOfMemoryError","List.toString");return false;}
   buf[len++]=',';buf[len++]=' ';
  }
  dvm_ref text=0;
  if(!synchronized_list_element_string(v,items[i],&text)){
   free(buf);
   if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","List element toString");
   return false;
  }
  const char *u=dvm_string_utf8(v,text);size_t sl=u?strlen(u):0;
  if(!list_text_reserve(&buf,&cap,len+sl+1)){free(buf);dvm__throw(v,"java/lang/OutOfMemoryError","List.toString");return false;}
  if(sl)memcpy(buf+len,u,sl);
  len+=sl;
 }
 if(!list_text_reserve(&buf,&cap,len+1)){free(buf);dvm__throw(v,"java/lang/OutOfMemoryError","List.toString");return false;}
 buf[len++]=']';
 dvm_ref text=dvm_new_string_n(v,buf,len);
 free(buf);
 if(!text){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","List.toString");return false;}
 *result=text;return true;
}
static bool synchronized_list_hash_value(struct dvm *v,dvm_ref array,union dvm_value *out){
 uint32_t n=dvm_array_length(v,array);dvm_ref *items=dvm_array_data(v,array);
 int32_t h=1;
 for(uint32_t i=0;i<n;i++){
  int32_t eh=0;
  if(!synchronized_list_element_hash(v,items[i],&eh)){
   if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","List element hashCode");
   return false;
  }
  h=31*h+eh;
 }
 out->i=h;return true;
}
static bool synchronized_list_equals_value(struct dvm *v,dvm_ref array,dvm_ref other,union dvm_value *out){
 out->i=0;
 if(!other)return true;
 const dvm_ref gc_ref_oo_54=(other); if(!gc_operation_admit(v,gc_ref_oo_54))return 0;
 struct dvm_class *li=dvm_find_class_or_external(v,"java/util/List");
 if(!dvm__obj(v,gc_ref_oo_54)||!dvm__obj(v,gc_ref_oo_54)->cls||!li||!dvm__class_assignable(v,dvm__obj(v,gc_ref_oo_54)->cls,li))return true;
 const char *oc=dvm__obj(v,gc_ref_oo_54)->cls->name;
 union dvm_value size={0};
 if(!collections_call(v,oc,"size","()I",other,NULL,0,&size)){
  if(dvm_exception(v))return false;
  return true;
 }
 uint32_t n=dvm_array_length(v,array);
 if(size.i!=(int32_t)n)return true;
 dvm_ref *items=dvm_array_data(v,array);
 for(uint32_t i=0;i<n;i++){
  union dvm_value index={.i=(int32_t)i},value={0};
  if(!collections_call(v,oc,"get","(I)Ljava/lang/Object;",other,&index,1,&value)){
   if(dvm_exception(v))return false;
   return true;
  }
  bool hit=java_basics_equal(v,items[i],value.l);
  if(dvm_exception(v))return false;
  if(!hit)return true;
 }
 out->i=1;return true;
}
static bool synchronized_list_contains_all_value(struct dvm *v,dvm_ref array,dvm_ref c,union dvm_value *out){
 if(!c){dvm__throw(v,"java/lang/NullPointerException","collection");return true;}
 const dvm_ref gc_ref_co_55=(c); if(!gc_operation_admit(v,gc_ref_co_55))return 0;
 const char *cc=dvm__obj(v,gc_ref_co_55)&&dvm__obj(v,gc_ref_co_55)->cls&&dvm__obj(v,gc_ref_co_55)->cls->name?dvm__obj(v,gc_ref_co_55)->cls->name:"java/util/Collection";
 union dvm_value it={0};
 if(!collections_call(v,cc,"iterator","()Ljava/util/Iterator;",c,NULL,0,&it)||!it.l){
  if(dvm_exception(v))return false;
  dvm__throw(v,"java/lang/UnsupportedOperationException","containsAll requires an iterable Collection");
  return true;
 }
 uint32_t n=dvm_array_length(v,array);dvm_ref *items=dvm_array_data(v,array);
 for(;;){
  union dvm_value has={0};
  if(!collections_call(v,"java/util/Iterator","hasNext","()Z",it.l,NULL,0,&has)){
   if(dvm_exception(v))return false;
   return false;
  }
  if(!has.i)break;
  union dvm_value x={0};
  if(!collections_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",it.l,NULL,0,&x)){
   if(dvm_exception(v))return false;
   return false;
  }
  bool found=false;
  for(uint32_t i=0;i<n;i++){
   if(java_basics_equal(v,items[i],x.l)){found=true;break;}
   if(dvm_exception(v))return false;
  }
  if(!found){out->i=0;return true;}
 }
 out->i=1;return true;
}
typedef struct {struct dvm *vm;dvm_ref owner;uint32_t cursor;uint32_t expected;int64_t last;} synchronized_list_iterator_view;
static void synchronized_list_iterator_destroy(void *p){
 synchronized_list_iterator_view *it=p;
 if(!it)return;
 if(it->owner)dvm_unpin(it->vm,it->owner);
 free(it);
}
static bool synchronized_list_iterator_make(struct dvm *v,dvm_ref owner,dvm_ref backing,int index,union dvm_value *result){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$SynchronizedListIterator");
 struct rt_list *l=list_of(v,backing);
 if(!cls||!l)return false;
 synchronized_list_iterator_view *it=calloc(1,sizeof *it);
 if(!it){dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedListIterator");return false;}
 it->vm=v;it->owner=owner;it->cursor=(uint32_t)index;it->expected=l->mod_count;it->last=-1;
 dvm_pin(v,owner);
 dvm_ref r=dvm_new_object(v,cls);const dvm_ref gc_ref_o_56=(r); if(!gc_operation_admit(v,gc_ref_o_56))return 0;
 if(!dvm__obj(v,gc_ref_o_56)){dvm_unpin(v,owner);free(it);dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedListIterator object");return false;}
 dvm__obj(v,gc_ref_o_56)->data=it;dvm__obj(v,gc_ref_o_56)->data_destroy=synchronized_list_iterator_destroy;result->l=r;return true;
}
static bool synchronized_list_iterator_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 const dvm_ref gc_ref_o_57=(self); if(!gc_operation_admit(v,gc_ref_o_57))return 0;
 synchronized_list_iterator_view *it=dvm__obj(v,gc_ref_o_57)&&dvm__obj(v,gc_ref_o_57)->data_destroy==synchronized_list_iterator_destroy?(synchronized_list_iterator_view *)dvm__obj(v,gc_ref_o_57)->data:NULL;
 if(!it)return false;
 int arity=0;
 if(IS("hasNext","()Z")||IS("next","()Ljava/lang/Object;")||IS("hasPrevious","()Z")||IS("previous","()Ljava/lang/Object;")||IS("nextIndex","()I")||IS("previousIndex","()I")||IS("remove","()V"))arity=0;
 else if(IS("set","(Ljava/lang/Object;)V")||IS("add","(Ljava/lang/Object;)V"))arity=1;
 else return false;
 if(nargs!=arity||(arity>0&&!a))return false;
 const dvm_ref gc_ref_wo_58=(it->owner); if(!gc_operation_admit(v,gc_ref_wo_58))return 0;
 synchronized_list_view *s=dvm__obj(v,gc_ref_wo_58)&&dvm__obj(v,gc_ref_wo_58)->data_destroy==synchronized_list_destroy?(synchronized_list_view *)dvm__obj(v,gc_ref_wo_58)->data:NULL;
 if(!s)return false;
 int rc=pthread_mutex_lock(&s->lock);
 if(rc){dvm__throw(v,"java/lang/IllegalStateException","SynchronizedListIterator lock failed (%d)",rc);return true;}
 bool ok=true;
 struct rt_list *l=list_of(v,s->backing);
 bool modifying=IS("next","()Ljava/lang/Object;")||IS("previous","()Ljava/lang/Object;")||IS("remove","()V")||IS("set","(Ljava/lang/Object;)V")||IS("add","(Ljava/lang/Object;)V");
 if(!l)ok=false;
 else if(modifying&&it->expected!=l->mod_count){dvm__throw(v,"java/util/ConcurrentModificationException","list modified while iterating");ok=true;}
 else if(IS("hasNext","()Z")){out->i=it->cursor<l->size;}
 else if(IS("next","()Ljava/lang/Object;")){
  if(it->cursor>=l->size){dvm__throw(v,"java/util/NoSuchElementException","ListIterator.next");ok=true;}
  else {it->last=(int64_t)it->cursor;out->l=l->items[it->cursor++];}
 }
 else if(IS("hasPrevious","()Z")){out->i=it->cursor>0;}
 else if(IS("previous","()Ljava/lang/Object;")){
  if(it->cursor==0){dvm__throw(v,"java/util/NoSuchElementException","ListIterator.previous");ok=true;}
  else {--it->cursor;it->last=(int64_t)it->cursor;out->l=l->items[it->cursor];}
 }
 else if(IS("nextIndex","()I")){out->i=(int32_t)it->cursor;}
 else if(IS("previousIndex","()I")){out->i=(int32_t)it->cursor-1;}
 else if(IS("remove","()V")){
  if(it->last<0){dvm__throw(v,"java/lang/IllegalStateException","ListIterator.remove without next/previous");ok=true;}
  else{
   union dvm_value index={.i=(int32_t)it->last},ignored={0};
   if(!list_remove(v,s->backing,&index,1,&ignored))ok=false;
   else{it->cursor=(uint32_t)it->last;it->last=-1;struct rt_list *now=list_of(v,s->backing);it->expected=now?now->mod_count:0;}
  }
 }
 else if(IS("set","(Ljava/lang/Object;)V")){
  if(it->last<0){dvm__throw(v,"java/lang/IllegalStateException","ListIterator.set without next/previous");ok=true;}
  else{
   union dvm_value args[2]={{.i=(int32_t)it->last},{.l=a[0].l}},ignored={0};
   if(!list_set(v,s->backing,args,2,&ignored))ok=false;
  }
 }
 else if(IS("add","(Ljava/lang/Object;)V")){
  union dvm_value args[2]={{.i=(int32_t)it->cursor},{.l=a[0].l}},ignored={0};
  if(!list_add_at(v,s->backing,args,2,&ignored))ok=false;
  else{it->cursor++;it->last=-1;struct rt_list *now=list_of(v,s->backing);it->expected=now?now->mod_count:0;}
 }
 (void)pthread_mutex_unlock(&s->lock);
 return ok;
}
static bool synchronized_list_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out);
static bool synchronized_list_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 return m&&synchronized_list_call(v,m->name,m->sig,self,a,n,out);
}
static bool synchronized_list_iterator_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 return m&&synchronized_list_iterator_call(v,m->name,m->sig,self,a,n,out);
}
static bool synchronized_list_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 const dvm_ref gc_ref_o_59=(self); if(!gc_operation_admit(v,gc_ref_o_59))return 0;
 synchronized_list_view *s=dvm__obj(v,gc_ref_o_59)&&dvm__obj(v,gc_ref_o_59)->data_destroy==synchronized_list_destroy?(synchronized_list_view *)dvm__obj(v,gc_ref_o_59)->data:NULL;
 if(!s)return false;
 int arity=synchronized_list_arity(m,sig);
 if(arity<0||nargs!=arity)return false;
 if(nargs>0&&!a)return false;
 if(IS("equals","(Ljava/lang/Object;)Z")&&a[0].l==self){out->i=1;return true;}
 int rc=pthread_mutex_lock(&s->lock);
 if(rc){dvm__throw(v,"java/lang/IllegalStateException","SynchronizedList lock failed (%d)",rc);return true;}
 const dvm_ref gc_ref_backing_60=(s->backing); if(!gc_operation_admit(v,gc_ref_backing_60))return 0;
 const char *backing_class=dvm__obj(v,gc_ref_backing_60)&&dvm__obj(v,gc_ref_backing_60)->cls&&dvm__obj(v,gc_ref_backing_60)->cls->name?dvm__obj(v,gc_ref_backing_60)->cls->name:"java/util/List";
 bool ok=false;
 if(IS("listIterator","()Ljava/util/ListIterator;")){
  ok=synchronized_list_iterator_make(v,self,s->backing,0,out);
 }
 else if(IS("listIterator","(I)Ljava/util/ListIterator;")){
  struct rt_list *l=list_of(v,s->backing);
  if(!l)ok=false;
  else if(a[0].i<0||a[0].i>(int32_t)l->size){
   dvm__throw(v,"java/lang/IndexOutOfBoundsException","ListIterator index");ok=true;
  }
  else ok=synchronized_list_iterator_make(v,self,s->backing,a[0].i,out);
 }
 else if(IS("equals","(Ljava/lang/Object;)Z")){
  dvm_ref array=synchronized_list_snapshot(v,s);
  ok=array&&synchronized_list_equals_value(v,array,a[0].l,out);
 }
 else if(IS("hashCode","()I")||IS("toString","()Ljava/lang/String;")||IS("containsAll","(Ljava/util/Collection;)Z")){
  dvm_ref array=synchronized_list_snapshot(v,s);
  if(array&&IS("hashCode","()I"))ok=synchronized_list_hash_value(v,array,out);
  else if(array&&IS("toString","()Ljava/lang/String;")){
   dvm_ref text=0;
   ok=synchronized_list_join(v,array,&text);
   if(ok)out->l=text;
  }
  else if(array)ok=synchronized_list_contains_all_value(v,array,a[0].l,out);
 }
 else ok=collections_call(v,backing_class,m,sig,s->backing,a,nargs,out);
 (void)pthread_mutex_unlock(&s->lock);
 return ok;
}
static dvm_ref synchronized_list_new(struct dvm *v,dvm_ref backing){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$SynchronizedList");
 synchronized_list_view *s=calloc(1,sizeof *s);
 if(!cls||!s){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedList");return 0;}
 pthread_mutexattr_t attr;
 int rc=pthread_mutexattr_init(&attr);
 if(!rc){
  rc=pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
  if(!rc)rc=pthread_mutex_init(&s->lock,&attr);
  (void)pthread_mutexattr_destroy(&attr);
 }
 if(rc){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedList mutex (%d)",rc);return 0;}
 s->lock_ready=true;
 dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_o_61=(ref); if(!gc_operation_admit(v,gc_ref_o_61))return 0;
 if(!dvm__obj(v,gc_ref_o_61)){(void)pthread_mutex_destroy(&s->lock);free(s);dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedList object");return 0;}
 s->vm=v;s->backing=backing;dvm_pin(v,backing);
 dvm__obj(v,gc_ref_o_61)->data=s;dvm__obj(v,gc_ref_o_61)->data_destroy=synchronized_list_destroy;
 return ref;
}
static void synchronized_list_vm_start(struct dvm *v){
 const char *list_ifaces[]={"java/util/List"},*iter_ifaces[]={"java/util/ListIterator"};
 struct dvm_class *cls=entry_register_class(v,"java/util/Collections$SynchronizedList","Ljava/util/Collections$SynchronizedList;",list_ifaces,1);
 if(!cls)return;
 cls->super=dvm_find_class_or_external(v,"java/util/AbstractList");
 if(!cls->nmethods){
  cls->methods=calloc(23,sizeof *cls->methods);
  if(!cls->methods){dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedList methods");return;}
  cls->nmethods=23;
  if(!entry_install_method(cls,0,"size","()I",synchronized_list_builtin,'I',0,0)||
     !entry_install_method(cls,1,"isEmpty","()Z",synchronized_list_builtin,'Z',0,0)||
     !entry_install_method(cls,2,"contains","(Ljava/lang/Object;)Z",synchronized_list_builtin,'Z',1,1)||
     !entry_install_method(cls,3,"get","(I)Ljava/lang/Object;",synchronized_list_builtin,'L',1,1)||
     !entry_install_method(cls,4,"set","(ILjava/lang/Object;)Ljava/lang/Object;",synchronized_list_builtin,'L',2,2)||
     !entry_install_method(cls,5,"add","(Ljava/lang/Object;)Z",synchronized_list_builtin,'Z',1,1)||
     !entry_install_method(cls,6,"add","(ILjava/lang/Object;)V",synchronized_list_builtin,'V',2,2)||
     !entry_install_method(cls,7,"remove","(I)Ljava/lang/Object;",synchronized_list_builtin,'L',1,1)||
     !entry_install_method(cls,8,"remove","(Ljava/lang/Object;)Z",synchronized_list_builtin,'Z',1,1)||
     !entry_install_method(cls,9,"clear","()V",synchronized_list_builtin,'V',0,0)||
     !entry_install_method(cls,10,"addAll","(Ljava/util/Collection;)Z",synchronized_list_builtin,'Z',1,1)||
     !entry_install_method(cls,11,"addAll","(ILjava/util/Collection;)Z",synchronized_list_builtin,'Z',2,2)||
     !entry_install_method(cls,12,"iterator","()Ljava/util/Iterator;",synchronized_list_builtin,'L',0,0)||
     !entry_install_method(cls,13,"listIterator","()Ljava/util/ListIterator;",synchronized_list_builtin,'L',0,0)||
     !entry_install_method(cls,14,"listIterator","(I)Ljava/util/ListIterator;",synchronized_list_builtin,'L',1,1)||
     !entry_install_method(cls,15,"equals","(Ljava/lang/Object;)Z",synchronized_list_builtin,'Z',1,1)||
     !entry_install_method(cls,16,"hashCode","()I",synchronized_list_builtin,'I',0,0)||
     !entry_install_method(cls,17,"toString","()Ljava/lang/String;",synchronized_list_builtin,'L',0,0)||
     !entry_install_method(cls,18,"toArray","()[Ljava/lang/Object;",synchronized_list_builtin,'L',0,0)||
     !entry_install_method(cls,19,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",synchronized_list_builtin,'L',1,1)||
     !entry_install_method(cls,20,"containsAll","(Ljava/util/Collection;)Z",synchronized_list_builtin,'Z',1,1)||
     !entry_install_method(cls,21,"indexOf","(Ljava/lang/Object;)I",synchronized_list_builtin,'I',1,1)||
     !entry_install_method(cls,22,"lastIndexOf","(Ljava/lang/Object;)I",synchronized_list_builtin,'I',1,1)){
   dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedList method signature");return;
  }
 }
 struct dvm_class *it=entry_register_class(v,"java/util/Collections$SynchronizedListIterator","Ljava/util/Collections$SynchronizedListIterator;",iter_ifaces,1);
 if(!it)return;
 if(!it->nmethods){
  it->methods=calloc(9,sizeof *it->methods);
  if(!it->methods){dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedListIterator methods");return;}
  it->nmethods=9;
  if(!entry_install_method(it,0,"hasNext","()Z",synchronized_list_iterator_builtin,'Z',0,0)||
     !entry_install_method(it,1,"next","()Ljava/lang/Object;",synchronized_list_iterator_builtin,'L',0,0)||
     !entry_install_method(it,2,"hasPrevious","()Z",synchronized_list_iterator_builtin,'Z',0,0)||
     !entry_install_method(it,3,"previous","()Ljava/lang/Object;",synchronized_list_iterator_builtin,'L',0,0)||
     !entry_install_method(it,4,"nextIndex","()I",synchronized_list_iterator_builtin,'I',0,0)||
     !entry_install_method(it,5,"previousIndex","()I",synchronized_list_iterator_builtin,'I',0,0)||
     !entry_install_method(it,6,"remove","()V",synchronized_list_iterator_builtin,'V',0,0)||
     !entry_install_method(it,7,"set","(Ljava/lang/Object;)V",synchronized_list_iterator_builtin,'V',1,1)||
     !entry_install_method(it,8,"add","(Ljava/lang/Object;)V",synchronized_list_iterator_builtin,'V',1,1)){
   dvm__throw(v,"java/lang/OutOfMemoryError","SynchronizedListIterator method signature");return;
  }
 }
}
/* ---- Collections.singletonList(Object) ------------------------------
 * Real immutable one-element List.  The element is pinned; the marker
 * destructor and installed builtins make both direct C dispatch and bytecode
 * invoke-interface reach this implementation.  Every structural mutation
 * throws UnsupportedOperationException, as required for SingletonList. */
typedef struct {struct dvm *vm;dvm_ref element;} singleton_list_view;
static void singleton_list_destroy(void *p){
 singleton_list_view *s=p;
 if(!s)return;
 if(s->element)dvm_unpin(s->vm,s->element);
 free(s);
}
static dvm_ref singleton_list_new(struct dvm *v,dvm_ref element){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$SingletonList");
 singleton_list_view *s=calloc(1,sizeof *s);
 if(!cls||!s){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList");return 0;}
 s->vm=v;s->element=element;
 if(element)dvm_pin(v,element);
 dvm_ref r=dvm_new_object(v,cls);const dvm_ref gc_ref_o_62=(r); if(!gc_operation_admit(v,gc_ref_o_62))return 0;
 if(!dvm__obj(v,gc_ref_o_62)){if(element)dvm_unpin(v,element);free(s);dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList object");return 0;}
 dvm__obj(v,gc_ref_o_62)->data=s;dvm__obj(v,gc_ref_o_62)->data_destroy=singleton_list_destroy;
 return r;
}
static int singleton_list_arity(const char *m,const char *sig){
 if(IS("size","()I")||IS("isEmpty","()Z")||IS("iterator","()Ljava/util/Iterator;")||
    IS("listIterator","()Ljava/util/ListIterator;")||IS("hashCode","()I")||
    IS("toString","()Ljava/lang/String;")||IS("toArray","()[Ljava/lang/Object;")||
    IS("clear","()V"))return 0;
 if(IS("add","(ILjava/lang/Object;)V")||IS("addAll","(ILjava/util/Collection;)Z")||
    IS("set","(ILjava/lang/Object;)Ljava/lang/Object;"))return 2;
 if(IS("contains","(Ljava/lang/Object;)Z")||IS("get","(I)Ljava/lang/Object;")||
    IS("indexOf","(Ljava/lang/Object;)I")||IS("lastIndexOf","(Ljava/lang/Object;)I")||
    IS("containsAll","(Ljava/util/Collection;)Z")||IS("listIterator","(I)Ljava/util/ListIterator;")||
    IS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")||
    IS("add","(Ljava/lang/Object;)Z")||IS("addAll","(Ljava/util/Collection;)Z")||
    IS("remove","(I)Ljava/lang/Object;")||IS("remove","(Ljava/lang/Object;)Z")||
    IS("removeAll","(Ljava/util/Collection;)Z")||IS("retainAll","(Ljava/util/Collection;)Z")||
    IS("equals","(Ljava/lang/Object;)Z")||
    IS("forEach","(Ljava/util/function/Consumer;)V")||
    IS("replaceAll","(Ljava/util/function/UnaryOperator;)V")||
    IS("sort","(Ljava/util/Comparator;)V")||
    IS("removeIf","(Ljava/util/function/Predicate;)Z"))return 1;
 return -1;
}
typedef struct {struct dvm *vm;dvm_ref owner;uint32_t cursor;int32_t last;} singleton_list_iterator_view;
static void singleton_list_iterator_destroy(void *p){
 singleton_list_iterator_view *it=p;
 if(!it)return;
 if(it->owner)dvm_unpin(it->vm,it->owner);
 free(it);
}
static dvm_ref singleton_list_iterator_new(struct dvm *v,dvm_ref owner,int index){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$SingletonListIterator");
 singleton_list_iterator_view *it=calloc(1,sizeof *it);
 if(!cls||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","SingletonListIterator");return 0;}
 it->vm=v;it->owner=owner;it->cursor=(uint32_t)index;it->last=-1;
 dvm_pin(v,owner);
 dvm_ref r=dvm_new_object(v,cls);const dvm_ref gc_ref_o_63=(r); if(!gc_operation_admit(v,gc_ref_o_63))return 0;
 if(!dvm__obj(v,gc_ref_o_63)){dvm_unpin(v,owner);free(it);dvm__throw(v,"java/lang/OutOfMemoryError","SingletonListIterator object");return 0;}
 dvm__obj(v,gc_ref_o_63)->data=it;dvm__obj(v,gc_ref_o_63)->data_destroy=singleton_list_iterator_destroy;
 return r;
}
static bool singleton_list_iterator_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 const dvm_ref gc_ref_o_64=(self); if(!gc_operation_admit(v,gc_ref_o_64))return 0;
 singleton_list_iterator_view *it=dvm__obj(v,gc_ref_o_64)&&dvm__obj(v,gc_ref_o_64)->data_destroy==singleton_list_iterator_destroy?(singleton_list_iterator_view *)dvm__obj(v,gc_ref_o_64)->data:NULL;
 if(!it)return false;
 if(IS("hasNext","()Z")){out->i=it->cursor<1;return true;}
 if(IS("next","()Ljava/lang/Object;")){
  if(it->cursor>=1){dvm__throw(v,"java/util/NoSuchElementException","SingletonListIterator.next");return true;}
  it->last=(int32_t)it->cursor++;
  const dvm_ref gc_ref_lo_65=(it->owner); if(!gc_operation_admit(v,gc_ref_lo_65))return 0;
  singleton_list_view *s=dvm__obj(v,gc_ref_lo_65)&&dvm__obj(v,gc_ref_lo_65)->data_destroy==singleton_list_destroy?(singleton_list_view *)dvm__obj(v,gc_ref_lo_65)->data:NULL;
  out->l=s?s->element:0;return true;
 }
 if(IS("hasPrevious","()Z")){out->i=it->cursor>0;return true;}
 if(IS("previous","()Ljava/lang/Object;")){
  if(it->cursor==0){dvm__throw(v,"java/util/NoSuchElementException","SingletonListIterator.previous");return true;}
  --it->cursor;it->last=(int32_t)it->cursor;
  const dvm_ref gc_ref_lo_66=(it->owner); if(!gc_operation_admit(v,gc_ref_lo_66))return 0;
  singleton_list_view *s=dvm__obj(v,gc_ref_lo_66)&&dvm__obj(v,gc_ref_lo_66)->data_destroy==singleton_list_destroy?(singleton_list_view *)dvm__obj(v,gc_ref_lo_66)->data:NULL;
  out->l=s?s->element:0;return true;
 }
 if(IS("nextIndex","()I")){out->i=(int32_t)it->cursor;return true;}
 if(IS("previousIndex","()I")){out->i=(int32_t)it->cursor-1;return true;}
 if(IS("remove","()V")||IS("set","(Ljava/lang/Object;)V")||IS("add","(Ljava/lang/Object;)V")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","Collections.singletonList is immutable");return true;
 }
 (void)nargs;(void)a;
 return false;
}
static bool singleton_list_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out);
static bool singleton_list_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 return m&&singleton_list_call(v,m->name,m->sig,self,a,n,out);
}
static bool singleton_list_iterator_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 return m&&singleton_list_iterator_call(v,m->name,m->sig,self,a,n,out);
}
static bool singleton_list_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 const dvm_ref gc_ref_o_67=(self); if(!gc_operation_admit(v,gc_ref_o_67))return 0;
 singleton_list_view *s=dvm__obj(v,gc_ref_o_67)&&dvm__obj(v,gc_ref_o_67)->data_destroy==singleton_list_destroy?(singleton_list_view *)dvm__obj(v,gc_ref_o_67)->data:NULL;
 if(!s)return false;
 int arity=singleton_list_arity(m,sig);
 if(arity<0||nargs!=arity)return false;
 if(nargs>0&&!a)return false;
 if(IS("size","()I")){out->i=1;return true;}
 if(IS("isEmpty","()Z")){out->i=0;return true;}
 if(IS("contains","(Ljava/lang/Object;)Z")){
  bool hit=java_basics_equal(v,a[0].l,s->element);
  if(dvm_exception(v))return false;
  out->i=hit;return true;
 }
 if(IS("get","(I)Ljava/lang/Object;")){
  if(a[0].i!=0){dvm__throw(v,"java/lang/IndexOutOfBoundsException","SingletonList index 0");return true;}
  out->l=s->element;return true;
 }
 if(IS("indexOf","(Ljava/lang/Object;)I")||IS("lastIndexOf","(Ljava/lang/Object;)I")){
  bool hit=java_basics_equal(v,a[0].l,s->element);
  if(dvm_exception(v))return false;
  out->i=hit?0:-1;return true;
 }
 if(IS("containsAll","(Ljava/util/Collection;)Z")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","collection");return true;}
  const dvm_ref gc_ref_co_68=(a[0].l); if(!gc_operation_admit(v,gc_ref_co_68))return 0;
  const char *cc=dvm__obj(v,gc_ref_co_68)&&dvm__obj(v,gc_ref_co_68)->cls&&dvm__obj(v,gc_ref_co_68)->cls->name?dvm__obj(v,gc_ref_co_68)->cls->name:"java/util/Collection";
  union dvm_value arg={.l=s->element},r={0};
  if(collections_call(v,cc,"contains","(Ljava/lang/Object;)Z",a[0].l,&arg,1,&r)){out->i=r.i;return true;}
  if(dvm_exception(v))return false;
  dvm__throw(v,"java/lang/UnsupportedOperationException","containsAll requires Collection.contains");
  return true;
 }
 if(IS("iterator","()Ljava/util/Iterator;")||IS("listIterator","()Ljava/util/ListIterator;")){
  out->l=singleton_list_iterator_new(v,self,0);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList iterator");
  return true;
 }
 if(IS("listIterator","(I)Ljava/util/ListIterator;")){
  if(a[0].i<0||a[0].i>1){dvm__throw(v,"java/lang/IndexOutOfBoundsException","SingletonList listIterator index");return true;}
  out->l=singleton_list_iterator_new(v,self,a[0].i);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList listIterator");
  return true;
 }
 if(IS("toArray","()[Ljava/lang/Object;")){
  out->l=dvm_new_array(v,'L',"Ljava/lang/Object;",1);
  if(!out->l){dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList toArray");return true;}
  ((dvm_ref *)dvm_array_data(v,out->l))[0]=s->element;
  return true;
 }
 if(IS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","SingletonList toArray");return true;}
  dvm_ref gc_ref_array_69=(a[0].l); if(!gc_operation_admit(v,gc_ref_array_69))return 0;
  if(!dvm__obj(v,gc_ref_array_69)||dvm__obj(v,gc_ref_array_69)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_69)->elem_kind!='L'){dvm__throw(v,"java/lang/ArrayStoreException","SingletonList toArray requires an object array");return true;}
  dvm_ref result=a[0].l;
  if(dvm__obj(v,gc_ref_array_69)->length==0){
   const char *ed=dvm__obj(v,gc_ref_array_69)->cls&&dvm__obj(v,gc_ref_array_69)->cls->desc&&dvm__obj(v,gc_ref_array_69)->cls->desc[0]=='['?dvm__obj(v,gc_ref_array_69)->cls->desc+1:"Ljava/lang/Object;";
   result=dvm_new_array(v,'L',ed,1);
   if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList toArray");return true;}
   gc_ref_array_69=result; if(!gc_operation_admit(v,gc_ref_array_69))return true;
  }
  dvm_ref *items=dvm_array_data(v,result);
  items[0]=s->element;
  if(dvm__obj(v,gc_ref_array_69)->length>1)items[1]=0;
  out->l=result;return true;
 }
 if(IS("equals","(Ljava/lang/Object;)Z")){
  out->i=0;
  if(!a[0].l)return true;
  if(a[0].l==self){out->i=1;return true;}
  const dvm_ref gc_ref_oo_70=(a[0].l); if(!gc_operation_admit(v,gc_ref_oo_70))return 0;
  struct dvm_class *li=dvm_find_class_or_external(v,"java/util/List");
  if(!dvm__obj(v,gc_ref_oo_70)||!dvm__obj(v,gc_ref_oo_70)->cls||!li||!dvm__class_assignable(v,dvm__obj(v,gc_ref_oo_70)->cls,li))return true;
  const char *oc=dvm__obj(v,gc_ref_oo_70)->cls->name;
  union dvm_value size={0};
  if(!collections_call(v,oc,"size","()I",a[0].l,NULL,0,&size)){
   if(dvm_exception(v))return false;
   return true;
  }
  if(size.i!=1)return true;
  union dvm_value index={.i=0},value={0};
  if(!collections_call(v,oc,"get","(I)Ljava/lang/Object;",a[0].l,&index,1,&value)){
   if(dvm_exception(v))return false;
   return true;
  }
  bool hit=java_basics_equal(v,s->element,value.l);
  if(dvm_exception(v))return false;
  out->i=hit;return true;
 }
 if(IS("hashCode","()I")){
  int32_t eh=0;
  if(!synchronized_list_element_hash(v,s->element,&eh)){
   if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","SingletonList element hashCode");
   return true;
  }
  out->i=31+eh;return true;
 }
 if(IS("toString","()Ljava/lang/String;")){
  dvm_ref element=0;
  if(!synchronized_list_element_string(v,s->element,&element)){
   if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","SingletonList element toString");
   return true;
  }
  const char *u=dvm_string_utf8(v,element);size_t n=u?strlen(u):0;
  char *buf=malloc(n+3);
  if(!buf){dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList toString");return true;}
  buf[0]='[';if(n)memcpy(buf+1,u,n);buf[n+1]=']';
  dvm_ref text=dvm_new_string_n(v,buf,n+2);
  free(buf);
  if(!text){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList toString");return true;}
  out->l=text;return true;
 }
 if(IS("forEach","(Ljava/util/function/Consumer;)V")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","SingletonList forEach");return true;}
  const dvm_ref gc_ref_co_71=(a[0].l); if(!gc_operation_admit(v,gc_ref_co_71))return 0;
  struct dvm_method *accept=dvm__obj(v,gc_ref_co_71)&&dvm__obj(v,gc_ref_co_71)->cls?dvm_find_method(v,dvm__obj(v,gc_ref_co_71)->cls,"accept","(Ljava/lang/Object;)V"):NULL;
  if(!accept||(!accept->has_code&&!accept->builtin)){
   dvm__throw(v,"java/lang/UnsupportedOperationException","Consumer.accept is unavailable");return true;
  }
  union dvm_value arg={.l=s->element},ignored={0};
  if(!dvm_call(v,accept,a[0].l,&arg,1,&ignored))return false;
  return true;
 }
 if(IS("sort","(Ljava/util/Comparator;)V"))return true;
 if(IS("add","(Ljava/lang/Object;)Z")||IS("add","(ILjava/lang/Object;)V")||
    IS("addAll","(Ljava/util/Collection;)Z")||IS("addAll","(ILjava/util/Collection;)Z")||
    IS("clear","()V")||IS("remove","(I)Ljava/lang/Object;")||IS("remove","(Ljava/lang/Object;)Z")||
    IS("removeAll","(Ljava/util/Collection;)Z")||IS("retainAll","(Ljava/util/Collection;)Z")||
    IS("set","(ILjava/lang/Object;)Ljava/lang/Object;")||
    IS("replaceAll","(Ljava/util/function/UnaryOperator;)V")||
    IS("removeIf","(Ljava/util/function/Predicate;)Z")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","Collections.singletonList is immutable");return true;
 }
 return false;
}
static void singleton_list_vm_start(struct dvm *v){
 const char *list_ifaces[]={"java/util/List"},*iter_ifaces[]={"java/util/ListIterator"};
 struct dvm_class *cls=entry_register_class(v,"java/util/Collections$SingletonList","Ljava/util/Collections$SingletonList;",list_ifaces,1);
 if(!cls)return;
 cls->super=dvm_find_class_or_external(v,"java/util/AbstractList");
 if(!cls->nmethods){
  cls->methods=calloc(29,sizeof *cls->methods);
  if(!cls->methods){dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList methods");return;}
  cls->nmethods=29;
  if(!entry_install_method(cls,0,"size","()I",singleton_list_builtin,'I',0,0)||
     !entry_install_method(cls,1,"isEmpty","()Z",singleton_list_builtin,'Z',0,0)||
     !entry_install_method(cls,2,"contains","(Ljava/lang/Object;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,3,"get","(I)Ljava/lang/Object;",singleton_list_builtin,'L',1,1)||
     !entry_install_method(cls,4,"indexOf","(Ljava/lang/Object;)I",singleton_list_builtin,'I',1,1)||
     !entry_install_method(cls,5,"lastIndexOf","(Ljava/lang/Object;)I",singleton_list_builtin,'I',1,1)||
     !entry_install_method(cls,6,"containsAll","(Ljava/util/Collection;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,7,"iterator","()Ljava/util/Iterator;",singleton_list_builtin,'L',0,0)||
     !entry_install_method(cls,8,"listIterator","()Ljava/util/ListIterator;",singleton_list_builtin,'L',0,0)||
     !entry_install_method(cls,9,"listIterator","(I)Ljava/util/ListIterator;",singleton_list_builtin,'L',1,1)||
     !entry_install_method(cls,10,"toArray","()[Ljava/lang/Object;",singleton_list_builtin,'L',0,0)||
     !entry_install_method(cls,11,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",singleton_list_builtin,'L',1,1)||
     !entry_install_method(cls,12,"equals","(Ljava/lang/Object;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,13,"hashCode","()I",singleton_list_builtin,'I',0,0)||
     !entry_install_method(cls,14,"toString","()Ljava/lang/String;",singleton_list_builtin,'L',0,0)||
     !entry_install_method(cls,15,"forEach","(Ljava/util/function/Consumer;)V",singleton_list_builtin,'V',1,1)||
     !entry_install_method(cls,16,"replaceAll","(Ljava/util/function/UnaryOperator;)V",singleton_list_builtin,'V',1,1)||
     !entry_install_method(cls,17,"sort","(Ljava/util/Comparator;)V",singleton_list_builtin,'V',1,1)||
     !entry_install_method(cls,18,"removeIf","(Ljava/util/function/Predicate;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,19,"add","(Ljava/lang/Object;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,20,"add","(ILjava/lang/Object;)V",singleton_list_builtin,'V',2,2)||
     !entry_install_method(cls,21,"addAll","(Ljava/util/Collection;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,22,"addAll","(ILjava/util/Collection;)Z",singleton_list_builtin,'Z',2,2)||
     !entry_install_method(cls,23,"clear","()V",singleton_list_builtin,'V',0,0)||
     !entry_install_method(cls,24,"remove","(I)Ljava/lang/Object;",singleton_list_builtin,'L',1,1)||
     !entry_install_method(cls,25,"remove","(Ljava/lang/Object;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,26,"removeAll","(Ljava/util/Collection;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,27,"retainAll","(Ljava/util/Collection;)Z",singleton_list_builtin,'Z',1,1)||
     !entry_install_method(cls,28,"set","(ILjava/lang/Object;)Ljava/lang/Object;",singleton_list_builtin,'L',2,2)){
   dvm__throw(v,"java/lang/OutOfMemoryError","SingletonList method signature");return;
  }
 }
 struct dvm_class *it=entry_register_class(v,"java/util/Collections$SingletonListIterator","Ljava/util/Collections$SingletonListIterator;",iter_ifaces,1);
 if(!it)return;
 if(!it->nmethods){
  it->methods=calloc(9,sizeof *it->methods);
  if(!it->methods){dvm__throw(v,"java/lang/OutOfMemoryError","SingletonListIterator methods");return;}
  it->nmethods=9;
  if(!entry_install_method(it,0,"hasNext","()Z",singleton_list_iterator_builtin,'Z',0,0)||
     !entry_install_method(it,1,"next","()Ljava/lang/Object;",singleton_list_iterator_builtin,'L',0,0)||
     !entry_install_method(it,2,"hasPrevious","()Z",singleton_list_iterator_builtin,'Z',0,0)||
     !entry_install_method(it,3,"previous","()Ljava/lang/Object;",singleton_list_iterator_builtin,'L',0,0)||
     !entry_install_method(it,4,"nextIndex","()I",singleton_list_iterator_builtin,'I',0,0)||
     !entry_install_method(it,5,"previousIndex","()I",singleton_list_iterator_builtin,'I',0,0)||
     !entry_install_method(it,6,"remove","()V",singleton_list_iterator_builtin,'V',0,0)||
     !entry_install_method(it,7,"set","(Ljava/lang/Object;)V",singleton_list_iterator_builtin,'V',1,1)||
     !entry_install_method(it,8,"add","(Ljava/lang/Object;)V",singleton_list_iterator_builtin,'V',1,1)){
   dvm__throw(v,"java/lang/OutOfMemoryError","SingletonListIterator method signature");return;
  }
 }
}
static bool collections_selftest_threw(struct dvm *v,const char *name){
 dvm_ref e=dvm_exception(v);
 bool hit=false;
 if(e){struct dvm_class *c=dvm_object_class(v,e);hit=c&&c->name&&!strcmp(c->name,name);}
 if(e)dvm_clear_exception(v);
 return hit;
}
static dvm_ref collections_selftest_list(struct dvm *v,const char *const *items,int n){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/ArrayList");
 dvm_ref r=cls?dvm_new_object(v,cls):0;
 union dvm_value out={0},a={0};
 if(!r||!collections_call(v,"java/util/ArrayList","<init>","()V",r,NULL,0,&out))return 0;
 for(int i=0;i<n;i++){
  a.l=dvm_new_string(v,items[i]);
  if(!a.l||!collections_call(v,"java/util/ArrayList","add","(Ljava/lang/Object;)Z",r,&a,1,&out))return 0;
 }
 return r;
}
static bool sl_text_is(struct dvm *v,dvm_ref ref,const char *want){
 const char *text=ref?dvm_string_utf8(v,ref):NULL;
 return text&&!strcmp(text,want);
}
static bool collections_synchronized_list_selftest_operation(struct dvm *v){
 const char *al="java/util/ArrayList",*lp="java/util/List",*itf="java/util/Iterator",*litf="java/util/ListIterator";
 union dvm_value out={0},a[2]={{0},{0}};
 bool okay=true;
#define SLCHECK(expr) do{if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL synchronized_list line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 a[0].l=0;
 bool null_call=collections_call(v,"java/util/Collections","synchronizedList","(Ljava/util/List;)Ljava/util/List;",0,a,1,&out);
 SLCHECK(null_call&&collections_selftest_threw(v,"java/lang/NullPointerException"));
 dvm_ref back=dvm_new_object(v,dvm_find_class_or_external(v,al));
 SLCHECK(back!=0);
 SLCHECK(collections_call(v,al,"<init>","()V",back,NULL,0,&out)&&!dvm_exception(v));
 dvm_ref ea=dvm_new_string(v,"a"),eb=dvm_new_string(v,"b"),ec=dvm_new_string(v,"c");
 a[0].l=ea;SLCHECK(collections_call(v,al,"add","(Ljava/lang/Object;)Z",back,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=eb;SLCHECK(collections_call(v,al,"add","(Ljava/lang/Object;)Z",back,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=back;
 bool wrap_call=collections_call(v,"java/util/Collections","synchronizedList","(Ljava/util/List;)Ljava/util/List;",0,a,1,&out);
 dvm_ref wrap=out.l;
 struct dvm_class *list_iface=dvm_find_class_or_external(v,lp);
 SLCHECK(wrap_call&&wrap&&list_iface&&dvm__class_assignable(v,dvm_object_class(v,wrap),list_iface)&&!dvm_exception(v));
 /* The same static may be invoked with owner java/util/List. */
 union dvm_value alt={0};a[0].l=back;
 SLCHECK(collections_call(v,lp,"synchronizedList","(Ljava/util/List;)Ljava/util/List;",0,a,1,&alt)&&alt.l&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"size","()I",wrap,NULL,0,&out)&&out.i==2&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"isEmpty","()Z",wrap,NULL,0,&out)&&out.i==0&&!dvm_exception(v));
 a[0].l=ea;SLCHECK(collections_call(v,lp,"contains","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].i=1;SLCHECK(collections_call(v,lp,"get","(I)Ljava/lang/Object;",wrap,a,1,&out)&&out.l==eb&&!dvm_exception(v));
 a[0].i=0;a[1].l=eb;
 SLCHECK(collections_call(v,lp,"set","(ILjava/lang/Object;)Ljava/lang/Object;",wrap,a,2,&out)&&out.l==ea&&!dvm_exception(v));
 a[0].i=0;SLCHECK(collections_call(v,al,"get","(I)Ljava/lang/Object;",back,a,1,&out)&&out.l==eb&&!dvm_exception(v));
 /* add(int,Object), add(Object), remove(int), remove(Object), clear. */
 a[0].i=1;a[1].l=ea;SLCHECK(collections_call(v,lp,"add","(ILjava/lang/Object;)V",wrap,a,2,&out)&&!dvm_exception(v));
 a[0].l=ec;SLCHECK(collections_call(v,lp,"add","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"size","()I",wrap,NULL,0,&out)&&out.i==4&&!dvm_exception(v));
 a[0].i=1;SLCHECK(collections_call(v,lp,"remove","(I)Ljava/lang/Object;",wrap,a,1,&out)&&out.l==ea&&!dvm_exception(v));
 a[0].l=ec;SLCHECK(collections_call(v,lp,"remove","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"size","()I",wrap,NULL,0,&out)&&out.i==2&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"clear","()V",wrap,NULL,0,&out)&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"isEmpty","()Z",wrap,NULL,0,&out)&&out.i==1&&!dvm_exception(v));
 /* addAll(Collection) then addAll(int,Collection). */
 dvm_ref src=collections_selftest_list(v,(const char *const[]){"a","c","b"},3);
 dvm_ref src2=collections_selftest_list(v,(const char *const[]){"z"},1);
 SLCHECK(src&&src2);
 a[0].l=src;SLCHECK(collections_call(v,lp,"addAll","(Ljava/util/Collection;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"size","()I",wrap,NULL,0,&out)&&out.i==3&&!dvm_exception(v));
 a[0].i=1;a[1].l=src2;
 SLCHECK(collections_call(v,lp,"addAll","(ILjava/util/Collection;)Z",wrap,a,2,&out)&&out.i==1&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"size","()I",wrap,NULL,0,&out)&&out.i==4&&!dvm_exception(v));
 a[0].i=1;SLCHECK(collections_call(v,lp,"remove","(I)Ljava/lang/Object;",wrap,a,1,&out)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"z")&&!dvm_exception(v));
 /* Backing iterator remains a real live view. */
 dvm_ref iter=0;
 SLCHECK(collections_call(v,lp,"iterator","()Ljava/util/Iterator;",wrap,NULL,0,&out)&&(iter=out.l)&&!dvm_exception(v));
 SLCHECK(iter&&collections_call(v,itf,"hasNext","()Z",iter,NULL,0,&out)&&out.i==1);
 SLCHECK(iter&&collections_call(v,itf,"next","()Ljava/lang/Object;",iter,NULL,0,&out)&&sl_text_is(v,out.l,"a"));
 SLCHECK(iter&&collections_call(v,itf,"next","()Ljava/lang/Object;",iter,NULL,0,&out)&&sl_text_is(v,out.l,"c"));
 SLCHECK(iter&&collections_call(v,itf,"next","()Ljava/lang/Object;",iter,NULL,0,&out)&&sl_text_is(v,out.l,"b"));
 SLCHECK(iter&&collections_call(v,itf,"hasNext","()Z",iter,NULL,0,&out)&&out.i==0);
 /* listIterator() and listIterator(int) are real cursors over the backing. */
 dvm_ref lit=0;
 SLCHECK(collections_call(v,lp,"listIterator","()Ljava/util/ListIterator;",wrap,NULL,0,&out)&&(lit=out.l)&&!dvm_exception(v));
 SLCHECK(lit&&collections_call(v,litf,"nextIndex","()I",lit,NULL,0,&out)&&out.i==0);
 SLCHECK(lit&&collections_call(v,litf,"next","()Ljava/lang/Object;",lit,NULL,0,&out)&&sl_text_is(v,out.l,"a"));
 SLCHECK(lit&&collections_call(v,litf,"previousIndex","()I",lit,NULL,0,&out)&&out.i==0);
 SLCHECK(lit&&collections_call(v,litf,"previous","()Ljava/lang/Object;",lit,NULL,0,&out)&&sl_text_is(v,out.l,"a"));
 a[0].i=3;lit=0;
 SLCHECK(collections_call(v,lp,"listIterator","(I)Ljava/util/ListIterator;",wrap,a,1,&out)&&(lit=out.l)&&!dvm_exception(v));
 SLCHECK(lit&&collections_call(v,litf,"hasPrevious","()Z",lit,NULL,0,&out)&&out.i==1);
 SLCHECK(lit&&collections_call(v,litf,"previous","()Ljava/lang/Object;",lit,NULL,0,&out)&&sl_text_is(v,out.l,"b"));
 a[0].i=99;
 bool bad_index=collections_call(v,lp,"listIterator","(I)Ljava/util/ListIterator;",wrap,a,1,&out);
 SLCHECK(bad_index&&collections_selftest_threw(v,"java/lang/IndexOutOfBoundsException"));
 /* equals, hashCode, toString, toArray, containsAll. */
 dvm_ref twin=collections_selftest_list(v,(const char *const[]){"a","c","b"},3);
 SLCHECK(twin!=0);
 a[0].l=twin;SLCHECK(collections_call(v,lp,"equals","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=wrap;SLCHECK(collections_call(v,lp,"equals","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=0;SLCHECK(collections_call(v,lp,"equals","(Ljava/lang/Object;)Z",wrap,a,1,&out)&&out.i==0&&!dvm_exception(v));
 union dvm_value ha={0},hc={0},hb={0};
 SLCHECK(string_runtime_call(v,"java/lang/String","hashCode","()I",ea,NULL,0,&ha));
 SLCHECK(string_runtime_call(v,"java/lang/String","hashCode","()I",ec,NULL,0,&hc));
 SLCHECK(string_runtime_call(v,"java/lang/String","hashCode","()I",eb,NULL,0,&hb));
 int32_t expected_hash=31*(31*(31*1+ha.i)+hc.i)+hb.i;
 SLCHECK(collections_call(v,lp,"hashCode","()I",wrap,NULL,0,&out)&&out.i==expected_hash&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"toString","()Ljava/lang/String;",wrap,NULL,0,&out)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"[a, c, b]")&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"toArray","()[Ljava/lang/Object;",wrap,NULL,0,&out)&&out.l&&dvm_array_length(v,out.l)==3&&!dvm_exception(v));
 a[0].l=twin;SLCHECK(collections_call(v,lp,"containsAll","(Ljava/util/Collection;)Z",wrap,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=src2;SLCHECK(collections_call(v,lp,"containsAll","(Ljava/util/Collection;)Z",wrap,a,1,&out)&&out.i==0&&!dvm_exception(v));
 dvm_ref arr=dvm_new_array(v,'L',"Ljava/lang/Object;",5);
 SLCHECK(arr!=0);a[0].l=arr;
 SLCHECK(collections_call(v,lp,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",wrap,a,1,&out)&&out.l==arr&&dvm_array_length(v,arr)==5&&!dvm_exception(v));
 if(dvm__obj(v,arr)&&dvm_array_data(v,arr)){
  dvm_ref *dst=dvm_array_data(v,arr);
  SLCHECK(sl_text_is(v,dst[0],"a")&&sl_text_is(v,dst[1],"c")&&sl_text_is(v,dst[2],"b")&&dst[3]==0);
 }
 /* Backing mutation is visible through the wrapper. */
 a[0].l=ea;SLCHECK(collections_call(v,al,"add","(Ljava/lang/Object;)Z",back,a,1,&out)&&!dvm_exception(v));
 SLCHECK(collections_call(v,lp,"size","()I",wrap,NULL,0,&out)&&out.i==4&&!dvm_exception(v));
 /* The per-instance mutex must really be recursive. */
 const dvm_ref gc_ref_wrap_object_72=(wrap); if(!gc_operation_admit(v,gc_ref_wrap_object_72))return 0;
 synchronized_list_view *wrap_view=dvm__obj(v,gc_ref_wrap_object_72)&&dvm__obj(v,gc_ref_wrap_object_72)->data_destroy==synchronized_list_destroy?(synchronized_list_view *)dvm__obj(v,gc_ref_wrap_object_72)->data:NULL;
 bool recursive=false;
 if(wrap_view&&pthread_mutex_lock(&wrap_view->lock)==0){
  recursive=pthread_mutex_lock(&wrap_view->lock)==0;
  if(recursive)(void)pthread_mutex_unlock(&wrap_view->lock);
  (void)pthread_mutex_unlock(&wrap_view->lock);
 }
 SLCHECK(recursive);
 /* listIterator mutators really change the backing list. */
 dvm_ref back2=collections_selftest_list(v,(const char *const[]){"a","b","c"},3);
 dvm_ref wrap2=0;
 a[0].l=back2;
 SLCHECK(back2&&collections_call(v,"java/util/Collections","synchronizedList","(Ljava/util/List;)Ljava/util/List;",0,a,1,&out)&&(wrap2=out.l)&&!dvm_exception(v));
 dvm_ref lit2=0;
 SLCHECK(wrap2&&collections_call(v,lp,"listIterator","()Ljava/util/ListIterator;",wrap2,NULL,0,&out)&&(lit2=out.l)&&!dvm_exception(v));
 SLCHECK(lit2&&collections_call(v,litf,"next","()Ljava/lang/Object;",lit2,NULL,0,&out)&&sl_text_is(v,out.l,"a"));
 SLCHECK(lit2&&collections_call(v,litf,"next","()Ljava/lang/Object;",lit2,NULL,0,&out)&&sl_text_is(v,out.l,"b"));
 a[0].l=ea;SLCHECK(lit2&&collections_call(v,litf,"set","(Ljava/lang/Object;)V",lit2,a,1,&out)&&!dvm_exception(v));
 a[0].i=1;SLCHECK(wrap2&&collections_call(v,lp,"get","(I)Ljava/lang/Object;",wrap2,a,1,&out)&&out.l==ea&&!dvm_exception(v));
 SLCHECK(lit2&&collections_call(v,litf,"remove","()V",lit2,NULL,0,&out)&&!dvm_exception(v));
 SLCHECK(wrap2&&collections_call(v,lp,"size","()I",wrap2,NULL,0,&out)&&out.i==2&&!dvm_exception(v));
 a[0].l=eb;SLCHECK(lit2&&collections_call(v,litf,"add","(Ljava/lang/Object;)V",lit2,a,1,&out)&&!dvm_exception(v));
 SLCHECK(wrap2&&collections_call(v,lp,"size","()I",wrap2,NULL,0,&out)&&out.i==3&&!dvm_exception(v));
 a[0].i=2;SLCHECK(wrap2&&collections_call(v,lp,"get","(I)Ljava/lang/Object;",wrap2,a,1,&out)&&sl_text_is(v,out.l,"c")&&!dvm_exception(v));
 /* Fail-fast after a structural backing change. */
 dvm_ref back3=collections_selftest_list(v,(const char *const[]){"a","b"},2);
 dvm_ref wrap3=0;
 a[0].l=back3;
 SLCHECK(back3&&collections_call(v,"java/util/Collections","synchronizedList","(Ljava/util/List;)Ljava/util/List;",0,a,1,&out)&&(wrap3=out.l)&&!dvm_exception(v));
 dvm_ref lit3=0;
 SLCHECK(wrap3&&collections_call(v,lp,"listIterator","()Ljava/util/ListIterator;",wrap3,NULL,0,&out)&&(lit3=out.l)&&!dvm_exception(v));
 SLCHECK(lit3&&collections_call(v,litf,"next","()Ljava/lang/Object;",lit3,NULL,0,&out)&&sl_text_is(v,out.l,"a"));
 a[0].l=ec;SLCHECK(collections_call(v,al,"add","(Ljava/lang/Object;)Z",back3,a,1,&out)&&!dvm_exception(v));
 bool cme_call=lit3&&collections_call(v,litf,"next","()Ljava/lang/Object;",lit3,NULL,0,&out);
 SLCHECK(cme_call&&collections_selftest_threw(v,"java/util/ConcurrentModificationException"));
 /* Unknown signatures, arities and foreign receivers stay fail-closed. */
 a[0].l=back;
 SLCHECK(!collections_call(v,"java/util/Collections","synchronizedList","(Ljava/util/List;)Ljava/lang/Object;",0,a,1,&out)&&!dvm_exception(v));
 SLCHECK(!collections_call(v,"java/util/Collections","synchronizedList","(Ljava/util/List;)Ljava/util/List;",0,a,0,&out)&&!dvm_exception(v));
 SLCHECK(!collections_call(v,lp,"get","()Ljava/lang/Object;",wrap,NULL,0,&out)&&!dvm_exception(v));
 SLCHECK(!collections_call(v,lp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",wrap,NULL,0,&out)&&!dvm_exception(v));
 SLCHECK(!collections_call(v,lp,"unimplemented","()V",wrap,NULL,0,&out)&&!dvm_exception(v));
 dvm_ref stranger=dvm_new_string(v,"not-a-list");
 SLCHECK(!collections_call(v,"java/util/Collections$SynchronizedList","get","(I)Ljava/lang/Object;",stranger,a,1,&out)&&!dvm_exception(v));
 if(dvm_exception(v))dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"SYNCHRONIZED_LIST_SELFTEST %s synchronized_list=1 null_npe=1 delegate_ops=1 backing_visible=1 iterator=1 list_iterator=1 equals_hash_tostring=1 toarray=1 recursive_mutex=1 fail_fast=1 fail_closed=1\n",okay?"PASS":"FAIL");
#undef SLCHECK
 return okay;
}
bool collections_synchronized_list_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=collections_synchronized_list_selftest_operation(v);
 return result;
}

static bool collections_singleton_list_selftest_operation(struct dvm *v){
 const char *lp="java/util/List",*al="java/util/ArrayList",*itf="java/util/Iterator",*litf="java/util/ListIterator";
 union dvm_value out={0},a[2]={{0},{0}};
 bool okay=true;
#define SGCHECK(expr) do{if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL singleton_list line=%d expression=%s\n",__LINE__,#expr);}}while(0)
#define SG_UOE(name,sig,count) do{bool u=collections_call(v,lp,name,sig,single,a,count,&out);SGCHECK(u&&collections_selftest_threw(v,"java/lang/UnsupportedOperationException"));}while(0)
 dvm_ref only=dvm_new_string(v,"only"),other=dvm_new_string(v,"other");
 a[0].l=only;
 bool call=collections_call(v,"java/util/Collections","singletonList","(Ljava/lang/Object;)Ljava/util/List;",0,a,1,&out);
 dvm_ref single=out.l;
 struct dvm_class *list_iface=dvm_find_class_or_external(v,lp);
 SGCHECK(call&&single&&list_iface&&dvm__class_assignable(v,dvm_object_class(v,single),list_iface)&&!dvm_exception(v));
 SGCHECK(collections_call(v,lp,"size","()I",single,NULL,0,&out)&&out.i==1&&!dvm_exception(v));
 SGCHECK(collections_call(v,lp,"isEmpty","()Z",single,NULL,0,&out)&&out.i==0&&!dvm_exception(v));
 a[0].l=only;SGCHECK(collections_call(v,lp,"contains","(Ljava/lang/Object;)Z",single,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=other;SGCHECK(collections_call(v,lp,"contains","(Ljava/lang/Object;)Z",single,a,1,&out)&&out.i==0&&!dvm_exception(v));
 a[0].i=0;SGCHECK(collections_call(v,lp,"get","(I)Ljava/lang/Object;",single,a,1,&out)&&out.l==only&&!dvm_exception(v));
 a[0].i=1;bool g1=collections_call(v,lp,"get","(I)Ljava/lang/Object;",single,a,1,&out);
 SGCHECK(g1&&collections_selftest_threw(v,"java/lang/IndexOutOfBoundsException"));
 a[0].i=-1;bool g2=collections_call(v,lp,"get","(I)Ljava/lang/Object;",single,a,1,&out);
 SGCHECK(g2&&collections_selftest_threw(v,"java/lang/IndexOutOfBoundsException"));
 a[0].l=only;SGCHECK(collections_call(v,lp,"indexOf","(Ljava/lang/Object;)I",single,a,1,&out)&&out.i==0&&!dvm_exception(v));
 SGCHECK(collections_call(v,lp,"lastIndexOf","(Ljava/lang/Object;)I",single,a,1,&out)&&out.i==0&&!dvm_exception(v));
 a[0].l=other;SGCHECK(collections_call(v,lp,"indexOf","(Ljava/lang/Object;)I",single,a,1,&out)&&out.i==-1&&!dvm_exception(v));
 /* Real single-element iterator. */
 dvm_ref iter=0;
 SGCHECK(collections_call(v,lp,"iterator","()Ljava/util/Iterator;",single,NULL,0,&out)&&(iter=out.l)&&!dvm_exception(v));
 SGCHECK(iter&&collections_call(v,itf,"hasNext","()Z",iter,NULL,0,&out)&&out.i==1);
 SGCHECK(iter&&collections_call(v,itf,"next","()Ljava/lang/Object;",iter,NULL,0,&out)&&out.l==only);
 SGCHECK(iter&&collections_call(v,itf,"hasNext","()Z",iter,NULL,0,&out)&&out.i==0);
 bool nse=iter&&collections_call(v,itf,"next","()Ljava/lang/Object;",iter,NULL,0,&out);
 SGCHECK(nse&&collections_selftest_threw(v,"java/util/NoSuchElementException"));
 dvm_ref lit=0;
 SGCHECK(collections_call(v,lp,"listIterator","()Ljava/util/ListIterator;",single,NULL,0,&out)&&(lit=out.l)&&!dvm_exception(v));
 SGCHECK(lit&&collections_call(v,litf,"nextIndex","()I",lit,NULL,0,&out)&&out.i==0);
 SGCHECK(lit&&collections_call(v,litf,"next","()Ljava/lang/Object;",lit,NULL,0,&out)&&out.l==only);
 SGCHECK(lit&&collections_call(v,litf,"previous","()Ljava/lang/Object;",lit,NULL,0,&out)&&out.l==only);
 a[0].i=1;lit=0;
 SGCHECK(collections_call(v,lp,"listIterator","(I)Ljava/util/ListIterator;",single,a,1,&out)&&(lit=out.l)&&!dvm_exception(v));
 SGCHECK(lit&&collections_call(v,litf,"hasPrevious","()Z",lit,NULL,0,&out)&&out.i==1);
 SGCHECK(lit&&collections_call(v,litf,"previous","()Ljava/lang/Object;",lit,NULL,0,&out)&&out.l==only);
 /* toArray() and toArray(T[]). */
 SGCHECK(collections_call(v,lp,"toArray","()[Ljava/lang/Object;",single,NULL,0,&out)&&out.l&&dvm_array_length(v,out.l)==1&&!dvm_exception(v));
 if(out.l){dvm_ref *items=dvm_array_data(v,out.l);SGCHECK(items[0]==only);}
 dvm_ref arr=dvm_new_array(v,'L',"Ljava/lang/Object;",5);
 SGCHECK(arr!=0);a[0].l=arr;
 SGCHECK(collections_call(v,lp,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",single,a,1,&out)&&out.l==arr&&!dvm_exception(v));
 if(dvm_array_data(v,arr)){dvm_ref *items=dvm_array_data(v,arr);SGCHECK(items[0]==only&&items[1]==0);}
 /* equals/hashCode/toString. */
 dvm_ref twin=collections_selftest_list(v,(const char *const[]){"only"},1);
 dvm_ref twin2=collections_selftest_list(v,(const char *const[]){"other"},1);
 SGCHECK(twin&&twin2);
 a[0].l=twin;SGCHECK(collections_call(v,lp,"equals","(Ljava/lang/Object;)Z",single,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=twin2;SGCHECK(collections_call(v,lp,"equals","(Ljava/lang/Object;)Z",single,a,1,&out)&&out.i==0&&!dvm_exception(v));
 a[0].l=single;SGCHECK(collections_call(v,lp,"equals","(Ljava/lang/Object;)Z",single,a,1,&out)&&out.i==1&&!dvm_exception(v));
 a[0].l=0;SGCHECK(collections_call(v,lp,"equals","(Ljava/lang/Object;)Z",single,a,1,&out)&&out.i==0&&!dvm_exception(v));
 union dvm_value h={0};
 SGCHECK(string_runtime_call(v,"java/lang/String","hashCode","()I",only,NULL,0,&h));
 SGCHECK(collections_call(v,lp,"hashCode","()I",single,NULL,0,&out)&&out.i==31+h.i&&!dvm_exception(v));
 SGCHECK(collections_call(v,lp,"toString","()Ljava/lang/String;",single,NULL,0,&out)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"[only]")&&!dvm_exception(v));
 /* Null element is legal and fully supported. */
 union dvm_value zero={0};
 bool null_element=collections_call(v,"java/util/Collections","singletonList","(Ljava/lang/Object;)Ljava/util/List;",0,&zero,1,&out);
 dvm_ref single0=out.l;
 SGCHECK(null_element&&single0&&!dvm_exception(v));
 SGCHECK(single0&&collections_call(v,lp,"size","()I",single0,NULL,0,&out)&&out.i==1&&!dvm_exception(v));
 a[0].i=0;SGCHECK(single0&&collections_call(v,lp,"get","(I)Ljava/lang/Object;",single0,a,1,&out)&&out.l==0&&!dvm_exception(v));
 a[0].l=0;SGCHECK(single0&&collections_call(v,lp,"contains","(Ljava/lang/Object;)Z",single0,a,1,&out)&&out.i==1&&!dvm_exception(v));
 SGCHECK(single0&&collections_call(v,lp,"toString","()Ljava/lang/String;",single0,NULL,0,&out)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"[null]")&&!dvm_exception(v));
 SGCHECK(single0&&collections_call(v,lp,"hashCode","()I",single0,NULL,0,&out)&&out.i==31&&!dvm_exception(v));
 /* Every structural mutation throws UnsupportedOperationException. */
 a[0].l=only;
 SG_UOE("add","(Ljava/lang/Object;)Z",1);
 a[0].i=0;a[1].l=only;
 SG_UOE("add","(ILjava/lang/Object;)V",2);
 a[0].l=twin;
 SG_UOE("addAll","(Ljava/util/Collection;)Z",1);
 a[0].i=0;a[1].l=twin;
 SG_UOE("addAll","(ILjava/util/Collection;)Z",2);
 SG_UOE("clear","()V",0);
 a[0].i=0;
 SG_UOE("remove","(I)Ljava/lang/Object;",1);
 a[0].l=only;
 SG_UOE("remove","(Ljava/lang/Object;)Z",1);
 a[0].l=twin;
 SG_UOE("removeAll","(Ljava/util/Collection;)Z",1);
 SG_UOE("retainAll","(Ljava/util/Collection;)Z",1);
 a[0].i=0;a[1].l=only;
 SG_UOE("set","(ILjava/lang/Object;)Ljava/lang/Object;",2);
 a[0].l=0;
 SG_UOE("replaceAll","(Ljava/util/function/UnaryOperator;)V",1);
 SG_UOE("removeIf","(Ljava/util/function/Predicate;)Z",1);
 /* Its iterator is also immutable. */
 dvm_ref lit2=0;
 SGCHECK(collections_call(v,lp,"listIterator","()Ljava/util/ListIterator;",single,NULL,0,&out)&&(lit2=out.l)&&!dvm_exception(v));
 SGCHECK(lit2&&collections_call(v,litf,"next","()Ljava/lang/Object;",lit2,NULL,0,&out)&&out.l==only);
 a[0].l=only;
 bool ir=lit2&&collections_call(v,litf,"remove","()V",lit2,NULL,0,&out);
 SGCHECK(ir&&collections_selftest_threw(v,"java/lang/UnsupportedOperationException"));
 bool is=lit2&&collections_call(v,litf,"set","(Ljava/lang/Object;)V",lit2,a,1,&out);
 SGCHECK(is&&collections_selftest_threw(v,"java/lang/UnsupportedOperationException"));
 bool ia=lit2&&collections_call(v,litf,"add","(Ljava/lang/Object;)V",lit2,a,1,&out);
 SGCHECK(ia&&collections_selftest_threw(v,"java/lang/UnsupportedOperationException"));
 /* Unknown signatures and foreign receivers fail closed. */
 a[0].l=only;
 SGCHECK(!collections_call(v,lp,"size","(I)I",single,a,1,&out)&&!dvm_exception(v));
 SGCHECK(!collections_call(v,lp,"singletonList","(Ljava/lang/Object;)Ljava/util/List;",0,NULL,0,&out)&&!dvm_exception(v));
 SGCHECK(!collections_call(v,lp,"unimplemented","()V",single,NULL,0,&out)&&!dvm_exception(v));
 dvm_ref stranger=dvm_new_string(v,"not-a-singleton");
 SGCHECK(!collections_call(v,"java/util/Collections$SingletonList","get","(I)Ljava/lang/Object;",stranger,a,1,&out)&&!dvm_exception(v));
 if(dvm_exception(v))dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"SINGLETON_LIST_SELFTEST %s singleton_list=1 size_one=1 immutable_uoe=1 iterator=1 list_iterator=1 equals_hash_tostring=1 toarray=1 null_element=1 fail_closed=1\n",okay?"PASS":"FAIL");
#undef SGCHECK
#undef SG_UOE
 return okay;
}
bool collections_singleton_list_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=collections_singleton_list_selftest_operation(v);
 return result;
}

/* ---- Collections.emptyList() ----------------------------------------
 * A real immutable List, not an ArrayList: the concrete class answers
 * instanceof List correctly, while every structural mutator throws
 * UnsupportedOperationException.  Fresh objects per call avoid per-VM state;
 * the marker destructors let collections_call() dispatch direct C callers as
 * well as the VM's builtin calls.  EmptyIterator mirrors the JDK iterator. */
static void empty_list_destroy(void *p){(void)p;}
static void empty_iterator_destroy(void *p){(void)p;}
static dvm_ref empty_iterator_new(struct dvm *v);
static dvm_ref empty_list_new(struct dvm *v);
static bool empty_iterator_call(struct dvm *v,const char *m,const char *sig,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)a;(void)nargs;
 if(IS("hasNext","()Z")){out->i=0;return true;}
 if(IS("next","()Ljava/lang/Object;")){dvm__throw(v,"java/util/NoSuchElementException","empty iterator");return true;}
 if(IS("remove","()V")){dvm__throw(v,"java/lang/IllegalStateException","empty iterator remove");return true;}
 return false;
}
static bool empty_list_call(struct dvm *v,const char *m,const char *sig,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;
 if(IS("size","()I")){out->i=0;return true;}
 if(IS("isEmpty","()Z")){out->i=1;return true;}
 if(IS("contains","(Ljava/lang/Object;)Z")){out->i=0;return true;}
 if(IS("iterator","()Ljava/util/Iterator;")){out->l=empty_iterator_new(v);if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptyList iterator");return true;}
 if(IS("get","(I)Ljava/lang/Object;")){dvm__throw(v,"java/lang/IndexOutOfBoundsException","empty list index");return true;}
 if(IS("indexOf","(Ljava/lang/Object;)I")||IS("lastIndexOf","(Ljava/lang/Object;)I")){out->i=-1;return true;}
 if(IS("toArray","()[Ljava/lang/Object;")){out->l=dvm_new_array(v,'L',"Ljava/lang/Object;",0);if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptyList array");return true;}
 if(IS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty list toArray");return true;}
  const dvm_ref gc_ref_array_73=(a[0].l); if(!gc_operation_admit(v,gc_ref_array_73))return 0;
  if(!dvm__obj(v,gc_ref_array_73)||dvm__obj(v,gc_ref_array_73)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_73)->elem_kind!='L'){dvm__throw(v,"java/lang/ArrayStoreException","empty list toArray requires an object array");return true;}
  if(dvm__obj(v,gc_ref_array_73)->length>0)((dvm_ref *)dvm_array_data(v,a[0].l))[0]=0;
  out->l=a[0].l;return true;
 }
 if(IS("toString","()Ljava/lang/String;")){out->l=dvm_new_string(v,"[]");if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptyList toString");return true;}
 if(IS("hashCode","()I")){out->i=1;return true;}
 /* These cannot observe or change any element, so they match the JDK. */
 if(IS("forEach","(Ljava/util/function/Consumer;)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty list forEach");return true;}return true;}
 if(IS("removeIf","(Ljava/util/function/Predicate;)Z")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty list removeIf");return true;}out->i=0;return true;}
 if(IS("replaceAll","(Ljava/util/function/UnaryOperator;)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty list replaceAll");return true;}return true;}
 if(IS("sort","(Ljava/util/Comparator;)V"))return true;
 /* Structural mutation is rejected, never silently answered false. */
 if(IS("add","(Ljava/lang/Object;)Z")||IS("add","(ILjava/lang/Object;)V")||
    IS("addAll","(Ljava/util/Collection;)Z")||IS("addAll","(ILjava/util/Collection;)Z")||
    IS("clear","()V")||IS("remove","(I)Ljava/lang/Object;")||IS("remove","(Ljava/lang/Object;)Z")||
    IS("removeAll","(Ljava/util/Collection;)Z")||IS("retainAll","(Ljava/util/Collection;)Z")||
    IS("set","(ILjava/lang/Object;)Ljava/lang/Object;")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","Collections.emptyList is immutable");return true;
 }
 return false;
}
static bool empty_list_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;struct dvm_method *m=dvm__builtin_method();
 return m&&empty_list_call(v,m->name,m->sig,a,n,out);
}
static bool empty_iterator_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;struct dvm_method *m=dvm__builtin_method();
 return m&&empty_iterator_call(v,m->name,m->sig,a,n,out);
}
static dvm_ref empty_iterator_new(struct dvm *v){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$EmptyIterator");
 dvm_ref r=cls?dvm_new_object(v,cls):0;const dvm_ref gc_ref_o_74=(r); if(!gc_operation_admit(v,gc_ref_o_74))return 0;
 if(!dvm__obj(v,gc_ref_o_74)){dvm__throw(v,"java/lang/OutOfMemoryError","Collections.EmptyIterator");return 0;}
 dvm__obj(v,gc_ref_o_74)->data=0;dvm__obj(v,gc_ref_o_74)->data_destroy=empty_iterator_destroy;return r;
}
static dvm_ref empty_list_new(struct dvm *v){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$EmptyList");
 dvm_ref r=cls?dvm_new_object(v,cls):0;const dvm_ref gc_ref_o_75=(r); if(!gc_operation_admit(v,gc_ref_o_75))return 0;
 if(!dvm__obj(v,gc_ref_o_75)){dvm__throw(v,"java/lang/OutOfMemoryError","Collections.EmptyList");return 0;}
 dvm__obj(v,gc_ref_o_75)->data=0;dvm__obj(v,gc_ref_o_75)->data_destroy=empty_list_destroy;return r;
}
static void empty_list_vm_start(struct dvm *v){
 const char *list_ifaces[]={"java/util/List"},*iter_ifaces[]={"java/util/Iterator"};
 struct dvm_class *list=entry_register_class(v,"java/util/Collections$EmptyList","Ljava/util/Collections$EmptyList;",list_ifaces,1);
 if(!list)return;
 list->super=dvm_find_class_or_external(v,"java/util/AbstractList");
 if(!list->nmethods){
  list->methods=calloc(25,sizeof *list->methods);
  if(!list->methods){dvm__throw(v,"java/lang/OutOfMemoryError","EmptyList methods");return;}
  list->nmethods=25;
  if(!entry_install_method(list,0,"size","()I",empty_list_builtin,'I',0,0)||
     !entry_install_method(list,1,"isEmpty","()Z",empty_list_builtin,'Z',0,0)||
     !entry_install_method(list,2,"contains","(Ljava/lang/Object;)Z",empty_list_builtin,'Z',1,1)||
     !entry_install_method(list,3,"iterator","()Ljava/util/Iterator;",empty_list_builtin,'L',0,0)||
     !entry_install_method(list,4,"get","(I)Ljava/lang/Object;",empty_list_builtin,'L',1,1)||
     !entry_install_method(list,5,"indexOf","(Ljava/lang/Object;)I",empty_list_builtin,'I',1,1)||
     !entry_install_method(list,6,"lastIndexOf","(Ljava/lang/Object;)I",empty_list_builtin,'I',1,1)||
     !entry_install_method(list,7,"toArray","()[Ljava/lang/Object;",empty_list_builtin,'L',0,0)||
     !entry_install_method(list,8,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",empty_list_builtin,'L',1,1)||
     !entry_install_method(list,9,"toString","()Ljava/lang/String;",empty_list_builtin,'L',0,0)||
     !entry_install_method(list,10,"hashCode","()I",empty_list_builtin,'I',0,0)||
     !entry_install_method(list,11,"forEach","(Ljava/util/function/Consumer;)V",empty_list_builtin,'V',1,1)||
     !entry_install_method(list,12,"removeIf","(Ljava/util/function/Predicate;)Z",empty_list_builtin,'Z',1,1)||
     !entry_install_method(list,13,"replaceAll","(Ljava/util/function/UnaryOperator;)V",empty_list_builtin,'V',1,1)||
     !entry_install_method(list,14,"sort","(Ljava/util/Comparator;)V",empty_list_builtin,'V',1,1)||
     !entry_install_method(list,15,"add","(Ljava/lang/Object;)Z",empty_list_builtin,'Z',1,1)||
     !entry_install_method(list,16,"add","(ILjava/lang/Object;)V",empty_list_builtin,'V',2,2)||
     !entry_install_method(list,17,"addAll","(Ljava/util/Collection;)Z",empty_list_builtin,'Z',1,1)||
     !entry_install_method(list,18,"addAll","(ILjava/util/Collection;)Z",empty_list_builtin,'Z',2,2)||
     !entry_install_method(list,19,"clear","()V",empty_list_builtin,'V',0,0)||
     !entry_install_method(list,20,"remove","(I)Ljava/lang/Object;",empty_list_builtin,'L',1,1)||
     !entry_install_method(list,21,"remove","(Ljava/lang/Object;)Z",empty_list_builtin,'Z',1,1)||
     !entry_install_method(list,22,"removeAll","(Ljava/util/Collection;)Z",empty_list_builtin,'Z',1,1)||
     !entry_install_method(list,23,"retainAll","(Ljava/util/Collection;)Z",empty_list_builtin,'Z',1,1)||
     !entry_install_method(list,24,"set","(ILjava/lang/Object;)Ljava/lang/Object;",empty_list_builtin,'L',2,2)){
   dvm__throw(v,"java/lang/OutOfMemoryError","EmptyList method signature");return;
  }
 }
 struct dvm_class *iter=entry_register_class(v,"java/util/Collections$EmptyIterator","Ljava/util/Collections$EmptyIterator;",iter_ifaces,1);
 if(!iter)return;
 if(!iter->nmethods){
  iter->methods=calloc(3,sizeof *iter->methods);
  if(!iter->methods){dvm__throw(v,"java/lang/OutOfMemoryError","EmptyIterator methods");return;}
  iter->nmethods=3;
  if(!entry_install_method(iter,0,"hasNext","()Z",empty_iterator_builtin,'Z',0,0)||
     !entry_install_method(iter,1,"next","()Ljava/lang/Object;",empty_iterator_builtin,'L',0,0)||
     !entry_install_method(iter,2,"remove","()V",empty_iterator_builtin,'V',0,0)){
   dvm__throw(v,"java/lang/OutOfMemoryError","EmptyIterator method signature");return;
  }
 }
}
/* ---- Collections.emptyMap() / emptySet() -----------------------------
 * The JDK singletons are real immutable Map/Set objects, not mutable empty
 * HashMap/HashSet stand-ins.  Fresh marker objects per call keep no per-VM
 * state; the marker destructors make collections_call() dispatch direct C
 * callers while the installed builtins serve bytecode invoke-interface.
 * Reads report emptiness.  The OpenJDK contracts are kept precisely where
 * callers depend on them: EmptyMap.remove(Object) returns null and clear()
 * is a no-op (there is no mapping to delete), EmptySet.remove/removeAll/
 * retainAll/clear are no-ops, while operations that would add a mapping or
 * element throw UnsupportedOperationException.  keySet()/entrySet()/values()
 * return empty EmptySet views, and iterator() returns the empty iterator
 * above. */
static void empty_map_destroy(void *p){(void)p;}
static void empty_set_destroy(void *p){(void)p;}
static dvm_ref empty_map_new(struct dvm *v);
static dvm_ref empty_set_new(struct dvm *v);
static bool empty_map_call(struct dvm *v,const char *m,const char *sig,const union dvm_value *a,int nargs,union dvm_value *out);
static bool empty_set_call(struct dvm *v,const char *m,const char *sig,const union dvm_value *a,int nargs,union dvm_value *out);
/* A foreign Map/Set equals an empty one only when its own isEmpty() says so.
 * HashMap/HashSet dispatch through the collection runtimes; other builtins
 * are reached through their method table. */
static bool empty_other_is_empty(struct dvm *v,dvm_ref ref,const char *iface_name,union dvm_value *out){
 out->i=0;if(!ref)return true;
 const dvm_ref gc_ref_o_76=(ref); if(!gc_operation_admit(v,gc_ref_o_76))return 0;
 struct dvm_class *iface=dvm_find_class_or_external(v,iface_name);
 if(!dvm__obj(v,gc_ref_o_76)||!dvm__obj(v,gc_ref_o_76)->cls||!iface||!dvm__class_assignable(v,dvm__obj(v,gc_ref_o_76)->cls,iface))return true;
 if(dvm__obj(v,gc_ref_o_76)->data_destroy==empty_map_destroy||dvm__obj(v,gc_ref_o_76)->data_destroy==empty_set_destroy){out->i=1;return true;}
 union dvm_value nested={0};
 if(collections_call(v,iface_name,"isEmpty","()Z",ref,NULL,0,&nested)){
  if(dvm_exception(v))return false;
  out->i=nested.i!=0;return true;
 }
 if(dvm_exception(v))return false;
 if(!strcmp(iface_name,"java/util/Set")&&set_runtime_call(v,iface_name,"isEmpty","()Z",ref,NULL,0,&nested)){
  if(dvm_exception(v))return false;
  out->i=nested.i!=0;return true;
 }
 struct dvm_method *method=dvm_find_method(v,dvm__obj(v,gc_ref_o_76)->cls,"isEmpty","()Z");
 if(!method||(!method->has_code&&!method->builtin))return true;
 if(!dvm_call(v,method,ref,NULL,0,&nested))return false;
 out->i=nested.i!=0;return true;
}
static bool empty_set_call(struct dvm *v,const char *m,const char *sig,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;
 if(IS("size","()I")){out->i=0;return true;}
 if(IS("isEmpty","()Z")){out->i=1;return true;}
 if(IS("contains","(Ljava/lang/Object;)Z")){out->i=0;return true;}
 if(IS("containsAll","(Ljava/util/Collection;)Z")){
  /* JDK EmptySet.containsAll(c) is c.isEmpty(); a null argument is an NPE. */
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty set containsAll");return true;}
  (void)empty_other_is_empty(v,a[0].l,"java/util/Collection",out);
  return true;
 }
 if(IS("iterator","()Ljava/util/Iterator;")){out->l=empty_iterator_new(v);if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptySet iterator");return true;}
 if(IS("toArray","()[Ljava/lang/Object;")){out->l=dvm_new_array(v,'L',"Ljava/lang/Object;",0);if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptySet array");return true;}
 if(IS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty set toArray");return true;}
  const dvm_ref gc_ref_array_77=(a[0].l); if(!gc_operation_admit(v,gc_ref_array_77))return 0;
  if(!dvm__obj(v,gc_ref_array_77)||dvm__obj(v,gc_ref_array_77)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_77)->elem_kind!='L'){dvm__throw(v,"java/lang/ArrayStoreException","empty set toArray requires an object array");return true;}
  if(dvm__obj(v,gc_ref_array_77)->length>0)((dvm_ref *)dvm_array_data(v,a[0].l))[0]=0;
  out->l=a[0].l;return true;
 }
 if(IS("toString","()Ljava/lang/String;")){out->l=dvm_new_string(v,"[]");if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptySet toString");return true;}
 if(IS("hashCode","()I")){out->i=0;return true;}
 if(IS("equals","(Ljava/lang/Object;)Z")){
  if(a[0].l==0){out->i=0;return true;}
  return empty_other_is_empty(v,a[0].l,"java/util/Set",out);
 }
 /* These cannot observe or change any element, so they match the JDK. */
 if(IS("forEach","(Ljava/util/function/Consumer;)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty set forEach");return true;}return true;}
 if(IS("removeIf","(Ljava/util/function/Predicate;)Z")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty set removeIf");return true;}out->i=0;return true;}
 /* OpenJDK EmptySet: nothing can be added, but deleting from an empty set is
  * a genuine no-op.  removeAll/retainAll validate their argument like
  * AbstractSet, then report that nothing changed. */
 if(IS("remove","(Ljava/lang/Object;)Z")){out->i=0;return true;}
 if(IS("clear","()V"))return true;
 if(IS("removeAll","(Ljava/util/Collection;)Z")||IS("retainAll","(Ljava/util/Collection;)Z")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty set bulk remove");return true;}
  out->i=0;return true;
 }
 if(IS("add","(Ljava/lang/Object;)Z")||IS("addAll","(Ljava/util/Collection;)Z")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","Collections.emptySet has no insertion");return true;
 }
 return false;
}
static bool empty_set_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;struct dvm_method *m=dvm__builtin_method();
 return m&&empty_set_call(v,m->name,m->sig,a,n,out);
}
static bool empty_map_call(struct dvm *v,const char *m,const char *sig,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;
 if(IS("size","()I")){out->i=0;return true;}
 if(IS("isEmpty","()Z")){out->i=1;return true;}
 if(IS("containsKey","(Ljava/lang/Object;)Z")||IS("containsValue","(Ljava/lang/Object;)Z")){out->i=0;return true;}
 if(IS("get","(Ljava/lang/Object;)Ljava/lang/Object;")){out->l=0;return true;}
 if(IS("getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")){out->l=a[1].l;return true;}
 if(IS("keySet","()Ljava/util/Set;")||IS("entrySet","()Ljava/util/Set;")||IS("values","()Ljava/util/Collection;")){
  out->l=empty_set_new(v);if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptyMap view");return true;
 }
 if(IS("toString","()Ljava/lang/String;")){out->l=dvm_new_string(v,"{}");if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptyMap toString");return true;}
 if(IS("hashCode","()I")){out->i=0;return true;}
 if(IS("equals","(Ljava/lang/Object;)Z")){
  if(a[0].l==0){out->i=0;return true;}
  return empty_other_is_empty(v,a[0].l,"java/util/Map",out);
 }
 /* These cannot observe or change any element, so they match the JDK. */
 if(IS("forEach","(Ljava/util/function/BiConsumer;)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty map forEach");return true;}return true;}
 if(IS("replaceAll","(Ljava/util/function/BiFunction;)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","empty map replaceAll");return true;}return true;}
 /* OpenJDK EmptyMap: remove(Object) has no mapping to delete and returns
  * null; clear() has nothing to clear.  Request.Builder.tag(Class,Object)
  * relies on the former for requests without a tag.  Operations that would
  * insert a mapping stay rejected. */
 if(IS("remove","(Ljava/lang/Object;)Ljava/lang/Object;")){out->l=0;return true;}
 if(IS("clear","()V"))return true;
 if(IS("put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")||
    IS("putAll","(Ljava/util/Map;)V")||
    IS("putIfAbsent","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")||
    IS("remove","(Ljava/lang/Object;Ljava/lang/Object;)Z")||
    IS("replace","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")||
    IS("replace","(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z")||
    IS("computeIfAbsent","(Ljava/lang/Object;Ljava/util/function/Function;)Ljava/lang/Object;")||
    IS("computeIfPresent","(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;")||
    IS("compute","(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;")||
    IS("merge","(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","Collections.emptyMap has no insertion");return true;
 }
 return false;
}
static bool empty_map_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;struct dvm_method *m=dvm__builtin_method();
 return m&&empty_map_call(v,m->name,m->sig,a,n,out);
}
static dvm_ref empty_set_new(struct dvm *v){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$EmptySet");
 dvm_ref r=cls?dvm_new_object(v,cls):0;const dvm_ref gc_ref_o_78=(r); if(!gc_operation_admit(v,gc_ref_o_78))return 0;
 if(!dvm__obj(v,gc_ref_o_78)){dvm__throw(v,"java/lang/OutOfMemoryError","Collections.EmptySet");return 0;}
 dvm__obj(v,gc_ref_o_78)->data=0;dvm__obj(v,gc_ref_o_78)->data_destroy=empty_set_destroy;return r;
}
static dvm_ref empty_map_new(struct dvm *v){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Collections$EmptyMap");
 dvm_ref r=cls?dvm_new_object(v,cls):0;const dvm_ref gc_ref_o_79=(r); if(!gc_operation_admit(v,gc_ref_o_79))return 0;
 if(!dvm__obj(v,gc_ref_o_79)){dvm__throw(v,"java/lang/OutOfMemoryError","Collections.EmptyMap");return 0;}
 dvm__obj(v,gc_ref_o_79)->data=0;dvm__obj(v,gc_ref_o_79)->data_destroy=empty_map_destroy;return r;
}
static void empty_map_set_vm_start(struct dvm *v){
 const char *map_ifaces[]={"java/util/Map"},*set_ifaces[]={"java/util/Set"};
 struct dvm_class *map=entry_register_class(v,"java/util/Collections$EmptyMap","Ljava/util/Collections$EmptyMap;",map_ifaces,1);
 if(!map)return;
 map->super=dvm_find_class_or_external(v,"java/util/AbstractMap");
 if(!map->nmethods){
  map->methods=calloc(26,sizeof *map->methods);
  if(!map->methods){dvm__throw(v,"java/lang/OutOfMemoryError","EmptyMap methods");return;}
  map->nmethods=26;
  if(!entry_install_method(map,0,"size","()I",empty_map_builtin,'I',0,0)||
     !entry_install_method(map,1,"isEmpty","()Z",empty_map_builtin,'Z',0,0)||
     !entry_install_method(map,2,"containsKey","(Ljava/lang/Object;)Z",empty_map_builtin,'Z',1,1)||
     !entry_install_method(map,3,"containsValue","(Ljava/lang/Object;)Z",empty_map_builtin,'Z',1,1)||
     !entry_install_method(map,4,"get","(Ljava/lang/Object;)Ljava/lang/Object;",empty_map_builtin,'L',1,1)||
     !entry_install_method(map,5,"getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",empty_map_builtin,'L',2,2)||
     !entry_install_method(map,6,"keySet","()Ljava/util/Set;",empty_map_builtin,'L',0,0)||
     !entry_install_method(map,7,"entrySet","()Ljava/util/Set;",empty_map_builtin,'L',0,0)||
     !entry_install_method(map,8,"values","()Ljava/util/Collection;",empty_map_builtin,'L',0,0)||
     !entry_install_method(map,9,"equals","(Ljava/lang/Object;)Z",empty_map_builtin,'Z',1,1)||
     !entry_install_method(map,10,"hashCode","()I",empty_map_builtin,'I',0,0)||
     !entry_install_method(map,11,"toString","()Ljava/lang/String;",empty_map_builtin,'L',0,0)||
     !entry_install_method(map,12,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",empty_map_builtin,'L',2,2)||
     !entry_install_method(map,13,"putAll","(Ljava/util/Map;)V",empty_map_builtin,'V',1,1)||
     !entry_install_method(map,14,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",empty_map_builtin,'L',1,1)||
     !entry_install_method(map,15,"clear","()V",empty_map_builtin,'V',0,0)||
     !entry_install_method(map,16,"putIfAbsent","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",empty_map_builtin,'L',2,2)||
     !entry_install_method(map,17,"remove","(Ljava/lang/Object;Ljava/lang/Object;)Z",empty_map_builtin,'Z',2,2)||
     !entry_install_method(map,18,"replace","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",empty_map_builtin,'L',2,2)||
     !entry_install_method(map,19,"replace","(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z",empty_map_builtin,'Z',3,3)||
     !entry_install_method(map,20,"computeIfAbsent","(Ljava/lang/Object;Ljava/util/function/Function;)Ljava/lang/Object;",empty_map_builtin,'L',2,2)||
     !entry_install_method(map,21,"computeIfPresent","(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;",empty_map_builtin,'L',2,2)||
     !entry_install_method(map,22,"compute","(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;",empty_map_builtin,'L',2,2)||
     !entry_install_method(map,23,"merge","(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;",empty_map_builtin,'L',3,3)||
     !entry_install_method(map,24,"forEach","(Ljava/util/function/BiConsumer;)V",empty_map_builtin,'V',1,1)||
     !entry_install_method(map,25,"replaceAll","(Ljava/util/function/BiFunction;)V",empty_map_builtin,'V',1,1)){
   dvm__throw(v,"java/lang/OutOfMemoryError","EmptyMap method signature");return;
  }
 }
 struct dvm_class *set=entry_register_class(v,"java/util/Collections$EmptySet","Ljava/util/Collections$EmptySet;",set_ifaces,1);
 if(!set)return;
 if(!set->nmethods){
  set->methods=calloc(18,sizeof *set->methods);
  if(!set->methods){dvm__throw(v,"java/lang/OutOfMemoryError","EmptySet methods");return;}
  set->nmethods=18;
  if(!entry_install_method(set,0,"size","()I",empty_set_builtin,'I',0,0)||
     !entry_install_method(set,1,"isEmpty","()Z",empty_set_builtin,'Z',0,0)||
     !entry_install_method(set,2,"contains","(Ljava/lang/Object;)Z",empty_set_builtin,'Z',1,1)||
     !entry_install_method(set,3,"containsAll","(Ljava/util/Collection;)Z",empty_set_builtin,'Z',1,1)||
     !entry_install_method(set,4,"iterator","()Ljava/util/Iterator;",empty_set_builtin,'L',0,0)||
     !entry_install_method(set,5,"toArray","()[Ljava/lang/Object;",empty_set_builtin,'L',0,0)||
     !entry_install_method(set,6,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",empty_set_builtin,'L',1,1)||
     !entry_install_method(set,7,"toString","()Ljava/lang/String;",empty_set_builtin,'L',0,0)||
     !entry_install_method(set,8,"hashCode","()I",empty_set_builtin,'I',0,0)||
     !entry_install_method(set,9,"equals","(Ljava/lang/Object;)Z",empty_set_builtin,'Z',1,1)||
     !entry_install_method(set,10,"add","(Ljava/lang/Object;)Z",empty_set_builtin,'Z',1,1)||
     !entry_install_method(set,11,"addAll","(Ljava/util/Collection;)Z",empty_set_builtin,'Z',1,1)||
     !entry_install_method(set,12,"remove","(Ljava/lang/Object;)Z",empty_set_builtin,'Z',1,1)||
     !entry_install_method(set,13,"removeAll","(Ljava/util/Collection;)Z",empty_set_builtin,'Z',1,1)||
     !entry_install_method(set,14,"retainAll","(Ljava/util/Collection;)Z",empty_set_builtin,'Z',1,1)||
     !entry_install_method(set,15,"clear","()V",empty_set_builtin,'V',0,0)||
     !entry_install_method(set,16,"forEach","(Ljava/util/function/Consumer;)V",empty_set_builtin,'V',1,1)||
     !entry_install_method(set,17,"removeIf","(Ljava/util/function/Predicate;)Z",empty_set_builtin,'Z',1,1)){
   dvm__throw(v,"java/lang/OutOfMemoryError","EmptySet method signature");return;
  }
 }
}
/* End-to-end checks for the two collection gaps reported by the rebuild72at
 * device log: List.removeAll(Collection) on ArrayList/LinkedList and
 * Collections.emptyMap().entrySet().  They call collections_call(), the same
 * dispatch the VM builtins use, and assert real element removal, the changed
 * return value, mod_count behavior, and the immutable empty views. */
static bool collections_remove_all_and_empty_selftest(struct dvm *v){
 bool ok=true;union dvm_value out={0},args[2]={{0},{0}};
#define REMOVE_ALL_CHECK(x) do{if(!(x)){ok=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL collections removeAll/empty line=%d expression=%s\n",__LINE__,#x);}}while(0)
 const char *al="java/util/ArrayList",*ll="java/util/LinkedList",*li="java/util/List";
 const char *lists[2]={al,ll};
 for(int t=0;t<2;++t){
  dvm_ref list=dvm_new_object(v,dvm_find_class_or_external(v,lists[t]));
  REMOVE_ALL_CHECK(list!=0);
  REMOVE_ALL_CHECK(collections_call(v,lists[t],"<init>","()V",list,NULL,0,&out)&&!dvm_exception(v));
  dvm_ref a=dvm_new_string(v,"a"),b=dvm_new_string(v,"b"),c=dvm_new_string(v,"c"),b2=dvm_new_string(v,"b");
  args[0].l=a;REMOVE_ALL_CHECK(collections_call(v,li,"add","(Ljava/lang/Object;)Z",list,args,1,&out)&&out.i==1);
  args[0].l=b;REMOVE_ALL_CHECK(collections_call(v,li,"add","(Ljava/lang/Object;)Z",list,args,1,&out)&&out.i==1);
  args[0].l=c;REMOVE_ALL_CHECK(collections_call(v,li,"add","(Ljava/lang/Object;)Z",list,args,1,&out)&&out.i==1);
  args[0].l=b2;REMOVE_ALL_CHECK(collections_call(v,li,"add","(Ljava/lang/Object;)Z",list,args,1,&out)&&out.i==1);
  dvm_ref removals=dvm_new_object(v,dvm_find_class_or_external(v,al));
  REMOVE_ALL_CHECK(removals&&collections_call(v,al,"<init>","()V",removals,NULL,0,&out));
  args[0].l=b;REMOVE_ALL_CHECK(collections_call(v,al,"add","(Ljava/lang/Object;)Z",removals,args,1,&out));
  args[0].l=removals;
  REMOVE_ALL_CHECK(collections_call(v,li,"removeAll","(Ljava/util/Collection;)Z",list,args,1,&out)&&out.i==1&&!dvm_exception(v));
  struct rt_list *l=dvm__obj(v,list)?dvm__obj(v,list)->data:NULL;
  REMOVE_ALL_CHECK(l&&l->size==2&&l->items[0]==a&&l->items[1]==c);
  uint32_t expected_mod=l?l->mod_count:0;
  /* No matching element: report false and leave mod_count untouched. */
  REMOVE_ALL_CHECK(collections_call(v,li,"removeAll","(Ljava/util/Collection;)Z",list,args,1,&out)&&out.i==0&&!dvm_exception(v));
  REMOVE_ALL_CHECK(l&&l->mod_count==expected_mod&&l->size==2);
  dvm_ref empty=dvm_new_object(v,dvm_find_class_or_external(v,al));
  REMOVE_ALL_CHECK(empty&&collections_call(v,al,"<init>","()V",empty,NULL,0,&out));
  args[0].l=empty;
  REMOVE_ALL_CHECK(collections_call(v,li,"removeAll","(Ljava/util/Collection;)Z",list,args,1,&out)&&out.i==0&&!dvm_exception(v));
  REMOVE_ALL_CHECK(l&&l->mod_count==expected_mod&&l->size==2);
  /* Removing against the list itself must snapshot the argument first. */
  args[0].l=list;
  REMOVE_ALL_CHECK(collections_call(v,li,"removeAll","(Ljava/util/Collection;)Z",list,args,1,&out)&&out.i==1&&!dvm_exception(v));
  REMOVE_ALL_CHECK(l&&l->size==0&&l->mod_count==expected_mod+1);
  /* A null Collection is an NPE and leaves the list untouched. */
  args[0].l=0;
  REMOVE_ALL_CHECK(collections_call(v,li,"removeAll","(Ljava/util/Collection;)Z",list,args,1,&out));
  REMOVE_ALL_CHECK(dvm_exception(v)!=0);
  dvm_clear_exception(v);
  REMOVE_ALL_CHECK(l&&l->size==0&&l->mod_count==expected_mod+1);
 }
 /* Collections.emptyMap().entrySet() is a real, immutable empty Set. */
 struct dvm_class *empty_map_cls=dvm_find_class_or_external(v,"java/util/Collections$EmptyMap");
 struct dvm_class *empty_list_cls=dvm_find_class_or_external(v,"java/util/Collections$EmptyList");
 struct dvm_class *empty_set_cls=dvm_find_class_or_external(v,"java/util/Collections$EmptySet");
 REMOVE_ALL_CHECK(empty_map_cls&&empty_map_cls->nmethods>0);
 REMOVE_ALL_CHECK(empty_list_cls&&empty_list_cls->nmethods>0);
 REMOVE_ALL_CHECK(empty_set_cls&&empty_set_cls->nmethods>0);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Collections","emptyMap","()Ljava/util/Map;",0,NULL,0,&out)&&out.l&&!dvm_exception(v));
 dvm_ref empty_map=out.l;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Map","entrySet","()Ljava/util/Set;",empty_map,NULL,0,&out)&&out.l&&!dvm_exception(v));
 dvm_ref empty_entries=out.l;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","size","()I",empty_entries,NULL,0,&out)&&out.i==0);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","isEmpty","()Z",empty_entries,NULL,0,&out)&&out.i==1);
 args[0].l=dvm_new_string(v,"absent");
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","contains","(Ljava/lang/Object;)Z",empty_entries,args,1,&out)&&out.i==0);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","remove","(Ljava/lang/Object;)Z",empty_entries,args,1,&out)&&out.i==0);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","removeAll","(Ljava/util/Collection;)Z",empty_entries,args,1,&out)&&out.i==0);
 args[0].l=0;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","removeAll","(Ljava/util/Collection;)Z",empty_entries,args,1,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",empty_entries,NULL,0,&out)&&out.l&&!dvm_exception(v));
 dvm_ref empty_iter=out.l;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Iterator","hasNext","()Z",empty_iter,NULL,0,&out)&&out.i==0);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",empty_iter,NULL,0,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 /* The immutable empty entrySet view is a real Set: add() must fail, and the
  * HashMap copy path must consume it through entrySet() rather than reject
  * the source map. */
 args[0].l=dvm_new_string(v,"x");
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",empty_entries,args,1,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 dvm_ref empty_copy=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashMap"));
 REMOVE_ALL_CHECK(empty_copy!=0);
 args[0].l=empty_map;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/HashMap","<init>","(Ljava/util/Map;)V",empty_copy,args,1,&out)&&!dvm_exception(v));
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Map","size","()I",empty_copy,NULL,0,&out)&&out.i==0);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/HashMap","putAll","(Ljava/util/Map;)V",empty_copy,args,1,&out)&&!dvm_exception(v));
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Map","size","()I",empty_copy,NULL,0,&out)&&out.i==0);
 /* A non-HashMap Map source (the synchronized wrapper) must copy through its
  * entrySet view; this is the path that used to report EmptyMap.entrySet. */
 dvm_ref backing=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashMap"));
 REMOVE_ALL_CHECK(backing!=0);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/HashMap","<init>","()V",backing,NULL,0,&out)&&!dvm_exception(v));
 args[0].l=dvm_new_string(v,"a");args[1].l=dvm_new_string(v,"1");
 REMOVE_ALL_CHECK(collections_call(v,"java/util/HashMap","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",backing,args,2,&out)&&!dvm_exception(v));
 args[0].l=backing;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Collections","synchronizedMap","(Ljava/util/Map;)Ljava/util/Map;",0,args,1,&out)&&out.l&&!dvm_exception(v));
 dvm_ref wrapper=out.l;
 dvm_ref copied=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashMap"));
 REMOVE_ALL_CHECK(copied!=0);
 args[0].l=wrapper;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/HashMap","<init>","(Ljava/util/Map;)V",copied,args,1,&out)&&!dvm_exception(v));
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Map","size","()I",copied,NULL,0,&out)&&out.i==1);
 args[0].l=dvm_new_string(v,"a");
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Map","get","(Ljava/lang/Object;)Ljava/lang/Object;",copied,args,1,&out)&&out.l&&java_basics_equal(v,out.l,dvm_new_string(v,"1"))&&!dvm_exception(v));
 args[0].l=wrapper;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/HashMap","putAll","(Ljava/util/Map;)V",copied,args,1,&out)&&!dvm_exception(v));
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Map","size","()I",copied,NULL,0,&out)&&out.i==1);
 /* EmptyMap.put must reject insertion instead of reporting a change. */
 args[0].l=dvm_new_string(v,"k");args[1].l=dvm_new_string(v,"v");
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Map","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",empty_map,args,2,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 /* EmptyList/EmptySet reject structural mutation. */
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Collections","emptyList","()Ljava/util/List;",0,NULL,0,&out)&&out.l&&!dvm_exception(v));
 dvm_ref empty_list=out.l;
 args[0].l=dvm_new_string(v,"x");
 REMOVE_ALL_CHECK(collections_call(v,"java/util/List","add","(Ljava/lang/Object;)Z",empty_list,args,1,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Collections","emptySet","()Ljava/util/Set;",0,NULL,0,&out)&&out.l&&!dvm_exception(v));
 dvm_ref empty_set=out.l;
 args[0].l=0;
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","removeAll","(Ljava/util/Collection;)Z",empty_set,args,1,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 REMOVE_ALL_CHECK(collections_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",empty_set,args,1,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
#undef REMOVE_ALL_CHECK
 return ok;
}
static void entry_views_vm_start(struct dvm *v){
 struct dvm_class *entry_iface=dvm_find_class_or_external(v,"java/util/Map$Entry");
 struct dvm_class *set_iface=dvm_find_class_or_external(v,"java/util/Set");
 if(entry_iface){entry_iface->is_interface=true;entry_iface->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;}
 if(set_iface){set_iface->is_interface=true;set_iface->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;}
 const char *entry_ifaces[]={"java/util/Map$Entry"},*set_ifaces[]={"java/util/Set"},*iter_ifaces[]={"java/util/Iterator"};
 struct dvm_class *entry=entry_register_class(v,"sdktrace/collections/MapEntry","Lsdktrace/collections/MapEntry;",entry_ifaces,1);
 if(entry&&!entry->nmethods){
  entry->methods=calloc(6,sizeof *entry->methods);
  if(!entry->methods){dvm__throw(v,"java/lang/OutOfMemoryError","Map.Entry methods");return;}
  entry->nmethods=6;
  if(!entry_install_method(entry,0,"getKey","()Ljava/lang/Object;",entry_getkey_builtin,'L',0,0)||
     !entry_install_method(entry,1,"getValue","()Ljava/lang/Object;",entry_getvalue_builtin,'L',0,0)||
     !entry_install_method(entry,2,"setValue","(Ljava/lang/Object;)Ljava/lang/Object;",entry_setvalue_builtin,'L',1,1)||
     !entry_install_method(entry,3,"equals","(Ljava/lang/Object;)Z",entry_equals_builtin,'Z',1,1)||
     !entry_install_method(entry,4,"hashCode","()I",entry_hash_builtin,'I',0,0)||
     !entry_install_method(entry,5,"toString","()Ljava/lang/String;",entry_tostring_builtin,'L',0,0)){
   dvm__throw(v,"java/lang/OutOfMemoryError","Map.Entry method signature");return;
  }
 }
 entry_register_class(v,"sdktrace/collections/EntrySet","Lsdktrace/collections/EntrySet;",set_ifaces,1);
 entry_register_class(v,"sdktrace/collections/EntryIterator","Lsdktrace/collections/EntryIterator;",iter_ifaces,1);
 entry_register_class(v,"sdktrace/collections/KeySet","Lsdktrace/collections/KeySet;",set_ifaces,1);
 entry_register_class(v,"sdktrace/collections/KeyIterator","Lsdktrace/collections/KeyIterator;",iter_ifaces,1);
}
/* Collections.addAll(Collection,Object[]) is the first host API reached by
 * okhttp3/Headers.newBuilder(); it copies Headers.namesAndValues (String[])
 * into the fresh builder's ArrayList and the boolean is discarded.  Java
 * requires target.add(element) once per slot in array order, and the result
 * is true iff at least one add() reported a change.  Dispatch through the
 * runtimes that own the receiver so Set duplicates, immutable views (UOE)
 * and custom Collection classes keep their own semantics; this helper never
 * edits the receiver's storage directly.  A receiver no runtime can add to
 * stays unhandled, and collections_add_all() then reports fail-closed before
 * any element has been inserted. */
static bool collection_add_one(struct dvm *v,dvm_ref target,dvm_ref element,union dvm_value *out,bool *handled){
 const dvm_ref gc_ref_o_80=(target); if(!gc_operation_admit(v,gc_ref_o_80))return 0;union dvm_value arg={.l=element};
 *handled=false;
 if(!dvm__obj(v,gc_ref_o_80)||!dvm__obj(v,gc_ref_o_80)->cls||!dvm__obj(v,gc_ref_o_80)->cls->name)return true;
 const char *cls=dvm__obj(v,gc_ref_o_80)->cls->name;
 if(set_runtime_call(v,cls,"add","(Ljava/lang/Object;)Z",target,&arg,1,out)){*handled=true;return true;}
 if(dvm_exception(v))return true;
 if(list_views_call(v,cls,"add","(Ljava/lang/Object;)Z",target,&arg,1,out)){*handled=true;return true;}
 if(dvm_exception(v))return true;
 if(collections_call(v,cls,"add","(Ljava/lang/Object;)Z",target,&arg,1,out)){*handled=true;return true;}
 if(dvm_exception(v))return true;
 if(java_basics_call(v,cls,"add","(Ljava/lang/Object;)Z",target,&arg,1,out)){*handled=true;return true;}
 if(dvm_exception(v))return true;
 struct dvm_method *method=dvm_find_method(v,dvm__obj(v,gc_ref_o_80)->cls,"add","(Ljava/lang/Object;)Z");
 if(method&&(method->has_code||method->builtin)){
  /* A guest override keeps its own null/immutability checks and exceptions. */
  if(!dvm_call(v,method,target,&arg,1,out))return true;
  *handled=true;
 }
 return true;
}
static bool collections_add_all(struct dvm *v,dvm_ref collection,dvm_ref elements,union dvm_value *out){
 if(!collection){dvm__throw(v,"java/lang/NullPointerException","Collections.addAll collection");return true;}
 if(!elements){dvm__throw(v,"java/lang/NullPointerException","Collections.addAll elements");return true;}
 const dvm_ref gc_ref_array_81=(elements); if(!gc_operation_admit(v,gc_ref_array_81))return 0;
 if(!dvm__obj(v,gc_ref_array_81)||dvm__obj(v,gc_ref_array_81)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_81)->elem_kind!='L')return false;
 uint32_t n=dvm_array_length(v,elements);bool changed=false;
 for(uint32_t i=0;i<n;++i){
  union dvm_value added={0};bool handled=false;
  if(!collection_add_one(v,collection,((dvm_ref *)dvm_array_data(v,elements))[i],&added,&handled))return false;
  if(dvm_exception(v))return true;
  if(!handled)return false;
  if(added.i)changed=true;
 }
 out->i=changed;return true;
}
/* Collections.sort(List[,Comparator]): in-place stable insertion sort.
 * The one-argument form supports the String elements used by
 * CommonParamUtils.sortParams; other element types fail closed with a real
 * ClassCastException.  A comparator, when supplied, is invoked through its
 * own compare(Object,Object)I method. */
static bool collection_sort(struct dvm *v,dvm_ref list,dvm_ref comparator,union dvm_value *out){
 (void)out;
 const dvm_ref gc_ref_o_82=(list); if(!gc_operation_admit(v,gc_ref_o_82))return 0;
 if(!dvm__obj(v,gc_ref_o_82)){dvm__throw(v,"java/lang/NullPointerException","Collections.sort list");return false;}
 struct rt_list *l=list_of(v,list);
 if(!l){dvm__throw(v,"java/lang/UnsupportedOperationException","Collections.sort receiver is not a mutable list");return false;}
 struct dvm_method *cmp=NULL;struct dvm_class *cc=NULL;
 if(comparator){
  cc=dvm_object_class(v,comparator);
  if(!cc||!(cmp=dvm_find_method(v,cc,"compare","(Ljava/lang/Object;Ljava/lang/Object;)I"))){
   dvm__throw(v,"java/lang/UnsupportedOperationException","Comparator.compare(Object,Object) is unavailable");return false;
  }
 }
 for(uint32_t i=1;i<l->size;i++){
  dvm_ref key=l->items[i];int64_t j=(int64_t)i-1;
  while(j>=0){
   int32_t order=0;
   if(cmp){
    union dvm_value args[2]={{.l=key},{.l=l->items[j]}},result={0};
    if(!dvm_call(v,cmp,comparator,args,2,&result)||dvm_exception(v))return false;
    order=result.i;
   }else{
    union dvm_value arg={.l=l->items[j]},result={0};
    if(!string_runtime_call(v,"java/lang/String","compareTo","(Ljava/lang/String;)I",key,&arg,1,&result)){
     if(!dvm_exception(v))dvm__throw(v,"java/lang/ClassCastException","Collections.sort without comparator requires String elements");
     return false;
    }
    order=result.i;
   }
   if(order>=0)break;
   l->items[j+1]=l->items[j];--j;
  }
  l->items[j+1]=key;
 }
 l->mod_count++;
 return true;
}
/* Call a collection method through the builtin dispatch first, then through a
 * real guest method when the builtin layer does not own the receiver.  This
 * lets a Map source that is neither this runtime's HashMap nor one of the
 * sdktrace view classes still expose a normal entrySet()/iterator() pair. */
static bool collection_dispatch_method(struct dvm *v,const char *cls,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out,bool *handled){
 *handled=false;
 if(list_views_call(v,cls,m,sig,self,a,n,out)){*handled=true;return true;}
 if(collections_call(v,cls,m,sig,self,a,n,out)){*handled=true;return true;}
 if(dvm_exception(v))return false;
 const dvm_ref gc_ref_o_83=(self); if(!gc_operation_admit(v,gc_ref_o_83))return 0;
 if(dvm__obj(v,gc_ref_o_83)&&dvm__obj(v,gc_ref_o_83)->cls){
  struct dvm_method *method=dvm_find_method(v,dvm__obj(v,gc_ref_o_83)->cls,m,sig);
  if(method&&method->has_code){
   if(!dvm_call(v,method,self,a,n,out))return false;
   *handled=true;return true;
  }
 }
 return false;
}
/* Copy an arbitrary Map source through its public entrySet view.  The old
 * putAll/HashMap(Map) branch accepted only the same runtime's HashMap and
 * reported MISSING_API for Collections$EmptyMap.entrySet, although EmptyMap's
 * entrySet was already implemented.  EmptyMap now iterates to zero entries
 * without touching the insertion path, and any non-empty Map view is copied
 * entry by entry. */
static bool map_copy_from_source(struct dvm *v,dvm_ref self,dvm_ref source,union dvm_value *out){
 (void)out;
 const dvm_ref gc_ref_src_84=(source); if(!gc_operation_admit(v,gc_ref_src_84))return 0;
 if(!dvm__obj(v,gc_ref_src_84)||!dvm__obj(v,gc_ref_src_84)->cls){dvm__throw(v,"java/lang/UnsupportedOperationException","Map source has no runtime class");return true;}
 bool handled=false;
 union dvm_value view={0};
 if(!collection_dispatch_method(v,dvm__obj(v,gc_ref_src_84)->cls->name,"entrySet","()Ljava/util/Set;",source,NULL,0,&view,&handled)){
  if(dvm_exception(v))return true;
  sdk_trace_missing_api(dvm__obj(v,gc_ref_src_84)->cls->name,"entrySet","()Ljava/util/Set;");
  dvm__throw(v,"java/lang/UnsupportedOperationException","Map source has no entrySet view");
  return true;
 }
 if(!view.l){dvm__throw(v,"java/lang/UnsupportedOperationException","Map.entrySet returned null");return true;}
 union dvm_value iter={0};
 if(!collection_dispatch_method(v,"java/util/Set","iterator","()Ljava/util/Iterator;",view.l,NULL,0,&iter,&handled)){
  if(dvm_exception(v))return true;
  dvm__throw(v,"java/lang/UnsupportedOperationException","Map.entrySet iterator is unavailable");
  return true;
 }
 if(!iter.l){dvm__throw(v,"java/lang/UnsupportedOperationException","Map.entrySet iterator returned null");return true;}
 uint32_t copied=0;
 for(;;){
  union dvm_value has={0},entry={0},key={0},value={0};
  if(!collection_dispatch_method(v,"java/util/Iterator","hasNext","()Z",iter.l,NULL,0,&has,&handled)){
   if(dvm_exception(v))return true;
   dvm__throw(v,"java/lang/UnsupportedOperationException","Map entry iterator is unavailable");
   return true;
  }
  if(!has.i)break;
  if(++copied>1000000){dvm__throw(v,"java/lang/IllegalStateException","Map entry iterator did not terminate");return true;}
  if(!collection_dispatch_method(v,"java/util/Iterator","next","()Ljava/lang/Object;",iter.l,NULL,0,&entry,&handled)){
   if(dvm_exception(v))return true;
   dvm__throw(v,"java/lang/UnsupportedOperationException","Map entry iterator is unavailable");
   return true;
  }
  if(!entry.l){dvm__throw(v,"java/lang/IllegalStateException","Map.entrySet returned a null entry");return true;}
  if(!collection_dispatch_method(v,"java/util/Map$Entry","getKey","()Ljava/lang/Object;",entry.l,NULL,0,&key,&handled)){
   if(dvm_exception(v))return true;
   dvm__throw(v,"java/lang/UnsupportedOperationException","Map.Entry.getKey is unavailable");
   return true;
  }
  if(!collection_dispatch_method(v,"java/util/Map$Entry","getValue","()Ljava/lang/Object;",entry.l,NULL,0,&value,&handled)){
   if(dvm_exception(v))return true;
   dvm__throw(v,"java/lang/UnsupportedOperationException","Map.Entry.getValue is unavailable");
   return true;
  }
  union dvm_value args[]={{.l=key.l},{.l=value.l}},ignored={0};
  if(!map_put(v,self,args,2,&ignored)){
   if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Map.put failed while copying");
   return true;
  }
 }
 return true;
}
static bool collections_call_operation(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 memset(out,0,sizeof *out);
 /* AbstractMap's protected constructor has no storage initialization. Guest
  * subclasses (Gson LinkedTreeMap) own their own tree and sentinel fields. */
 if((!strcmp(c,"java/util/AbstractMap")||!strcmp(c,"java/util/AbstractSet")||!strcmp(c,"java/util/AbstractCollection"))&&IS("<init>","()V")&&!nargs){
  if(!self)dvm__throw(v,"java/lang/NullPointerException","abstract collection receiver");return true;
 }
 if(!strcmp(c,"java/util/Collections")&&IS("singletonMap","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/util/Map;")&&nargs==2&&a){
  dvm_ref backing=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashMap"));union dvm_value ignored={0};
  if(!backing||!collections_call(v,"java/util/HashMap","<init>","()V",backing,NULL,0,&ignored)||dvm_exception(v))return true;
  if(!collections_call(v,"java/util/HashMap","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",backing,a,2,&ignored)||dvm_exception(v))return true;
  out->l=readonly_new(v,backing,RO_MAP);return true;
 }
 if((!strcmp(c,"java/util/Collections")||!strcmp(c,"java/util/List"))&&IS("singletonList","(Ljava/lang/Object;)Ljava/util/List;")&&nargs==1&&a){
  /* Real immutable one-element List; a null element is legal.  The DEX
   * callers see java.util.List semantics, so mutators must throw UOE. */
  out->l=singleton_list_new(v,a[0].l);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.singletonList");
  return true;
 }
 if((!strcmp(c,"java/util/Collections")||!strcmp(c,"java/util/List"))&&IS("synchronizedList","(Ljava/util/List;)Ljava/util/List;")){
  /* Exact signature and arity; a null backing list is an NPE as in the JDK.
   * ConnectivityMonitor.<init> and com/gsc/progurad.b.<clinit> call this and
   * then use the wrapper through ordinary List methods. */
  if(nargs!=1||!a)return false;
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","list");return true;}
  out->l=synchronized_list_new(v,a[0].l);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.synchronizedList");
  return true;
 }
 if(!strcmp(c,"java/util/Arrays")&&IS("equals","([Ljava/lang/Object;[Ljava/lang/Object;)Z")&&nargs==2&&a){
  if(a[0].l==a[1].l){out->i=1;return true;}if(!a[0].l||!a[1].l){out->i=0;return true;}
  const dvm_ref gc_ref_left_85=(a[0].l); if(!gc_operation_admit(v,gc_ref_left_85))return true;const dvm_ref gc_ref_right_86=(a[1].l); if(!gc_operation_admit(v,gc_ref_right_86))return true;
  if(!dvm__obj(v,gc_ref_left_85)||!dvm__obj(v,gc_ref_right_86)||dvm__obj(v,gc_ref_left_85)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_right_86)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_left_85)->elem_kind!='L'||dvm__obj(v,gc_ref_right_86)->elem_kind!='L'){dvm__throw(v,"java/lang/ClassCastException","Arrays.equals Object arrays");return true;}
  uint32_t count=dvm_array_length(v,a[0].l);if(count!=dvm_array_length(v,a[1].l)){out->i=0;return true;}
  for(uint32_t i=0;i<count;i++){dvm_ref x=((dvm_ref*)dvm_array_data(v,a[0].l))[i],y=((dvm_ref*)dvm_array_data(v,a[1].l))[i];bool equal=java_basics_equal(v,x,y);if(dvm_exception(v))return true;if(!equal){out->i=0;return true;}}
  out->i=1;return true;
 }
 if(!strcmp(c,"java/util/Arrays")&&IS("hashCode","([Ljava/lang/Object;)I")&&nargs==1&&a){
  if(!a[0].l){out->i=0;return true;}
  const dvm_ref gc_ref_array_87=(a[0].l); if(!gc_operation_admit(v,gc_ref_array_87))return true;
  if(!dvm__obj(v,gc_ref_array_87)||dvm__obj(v,gc_ref_array_87)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_array_87)->elem_kind!='L'){dvm__throw(v,"java/lang/ClassCastException","Arrays.hashCode Object array");return true;}
  uint32_t hash=1;dvm_ref *items=dvm_array_data(v,a[0].l);
  for(uint32_t i=0;i<dvm__obj(v,gc_ref_array_87)->length;i++){uint32_t element=entry_hash_part(v,items[i]);if(dvm_exception(v))return true;hash=31u*hash+element;}
  out->i=(int32_t)hash;return true;
 }
 if(!strcmp(c,"java/util/Collections")&&IS("emptyList","()Ljava/util/List;")){
  /* Static call sites pass self==0; the result is a real immutable List. */
  out->l=empty_list_new(v);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptyList");
  return true;
 }
 if(!strcmp(c,"java/util/Collections")&&IS("emptyMap","()Ljava/util/Map;")){
  /* Real immutable empty Map; Request.Builder.<init> depends on it. */
  out->l=empty_map_new(v);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptyMap");
  return true;
 }
 if(!strcmp(c,"java/util/Collections")&&IS("reverse","(Ljava/util/List;)V")&&nargs==1&&a){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Collections.reverse list");return true;}
  union dvm_value size={0};bool handled=false;
  if(!collection_dispatch_method(v,"java/util/List","size","()I",a[0].l,NULL,0,&size,&handled)||dvm_exception(v))goto reverse_failed;
  for(int32_t left=0,right=size.i-1;left<right;left++,right--){
   union dvm_value l={.i=left},r={.i=right},first={0},last={0},ignored={0};
   if(!collection_dispatch_method(v,"java/util/List","get","(I)Ljava/lang/Object;",a[0].l,&l,1,&first,&handled)||dvm_exception(v))goto reverse_failed;
   if(!collection_dispatch_method(v,"java/util/List","get","(I)Ljava/lang/Object;",a[0].l,&r,1,&last,&handled)||dvm_exception(v))goto reverse_failed;
   union dvm_value put[2]={{.i=left},{.l=last.l}};
   if(!collection_dispatch_method(v,"java/util/List","set","(ILjava/lang/Object;)Ljava/lang/Object;",a[0].l,put,2,&ignored,&handled)||dvm_exception(v))goto reverse_failed;
   put[0].i=right;put[1].l=first.l;
   if(!collection_dispatch_method(v,"java/util/List","set","(ILjava/lang/Object;)Ljava/lang/Object;",a[0].l,put,2,&ignored,&handled)||dvm_exception(v))goto reverse_failed;
  }
  return true;
 reverse_failed:
  if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","Collections.reverse List method unavailable");return true;
 }
 if(!strcmp(c,"java/util/Collections")&&(IS("sort","(Ljava/util/List;)V")||IS("sort","(Ljava/util/List;Ljava/util/Comparator;)V"))){
  if(nargs<1||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Collections.sort list");return true;}
  dvm_ref comparator=IS("sort","(Ljava/util/List;Ljava/util/Comparator;)V")?a[1].l:0;
  if(!collection_sort(v,a[0].l,comparator,out))return true;
  return true;
 }
 if(!strcmp(c,"java/util/Collections")&&IS("emptySet","()Ljava/util/Set;")){
  /* Real immutable empty Set; also backs emptyMap keySet/entrySet/values. */
  out->l=empty_set_new(v);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.emptySet");
  return true;
 }
 if(!strcmp(c,"java/util/Collections")&&IS("addAll","(Ljava/util/Collection;[Ljava/lang/Object;)Z")&&nargs==2){
  /* Exact public signature only; Headers.newBuilder() relies on the real
   * per-element add() semantics described above collections_add_all(). */
  return collections_add_all(v,a[0].l,a[1].l,out);
 }
 if(!strcmp(c,"java/util/Collections")&&IS("unmodifiableMap","(Ljava/util/Map;)Ljava/util/Map;")&&nargs==1&&a){out->l=readonly_new(v,a[0].l,RO_MAP);return true;}
 if(!strcmp(c,"java/util/Collections")&&IS("synchronizedMap","(Ljava/util/Map;)Ljava/util/Map;")){
  /* Exact signature and arity; a null backing map is an NPE as in the JDK.
   * com/gsc/base/service/a.<init> stores this wrapper in a final Map field. */
  if(nargs!=1||!a)return false;
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","map");return true;}
  out->l=synchronized_map_new(v,a[0].l);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Collections.synchronizedMap");
  return true;
 }
 const dvm_ref gc_ref_o_88=(self); if(!gc_operation_admit(v,gc_ref_o_88))return true;
 /* Vector's supported List operations reuse actual recursive-lock storage,
  * not an unsynchronised ArrayList alias. Unsupported Vector-only APIs fail
  * normal dispatch rather than returning invented values. */
 if(dvm__obj(v,gc_ref_o_88)&&is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/Vector")&&!strcmp(m,"<init>")){
  if(strcmp(sig,"()V")&&strcmp(sig,"(I)V")&&strcmp(sig,"(Ljava/util/Collection;)V"))return false;
  if(dvm__obj(v,gc_ref_o_88)->data){dvm__throw(v,"java/lang/IllegalStateException","Vector already constructed");return true;}
  dvm_ref backing=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/ArrayList"));
  if(!backing||!collections_call(v,"java/util/ArrayList","<init>",sig,backing,a,nargs,out)||dvm_exception(v))return true;
  dvm_ref wrapper=synchronized_list_new(v,backing);const dvm_ref gc_ref_w_89=(wrapper); if(!gc_operation_admit(v,gc_ref_w_89))return true;
  if(!dvm__obj(v,gc_ref_w_89))return true;dvm__obj(v,gc_ref_o_88)->data=dvm__obj(v,gc_ref_w_89)->data;dvm__obj(v,gc_ref_o_88)->data_destroy=dvm__obj(v,gc_ref_w_89)->data_destroy;dvm__obj(v,gc_ref_w_89)->data=NULL;dvm__obj(v,gc_ref_w_89)->data_destroy=NULL;return true;
 }
 /* Vector/Stack compound operations hold the same recursive lock as List calls. */
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==synchronized_list_destroy&&is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/Vector")){
  bool last=IS("lastElement","()Ljava/lang/Object;"),peek=IS("peek","()Ljava/lang/Object;"),pop=IS("pop","()Ljava/lang/Object;");
  if(last||peek||pop||IS("empty","()Z")||IS("push","(Ljava/lang/Object;)Ljava/lang/Object;")){
   synchronized_list_view *state=dvm__obj(v,gc_ref_o_88)->data;int rc=pthread_mutex_lock(&state->lock);
   if(rc){dvm__throw(v,"java/lang/IllegalStateException","Vector lock failed");return true;}
   union dvm_value size={0};synchronized_list_call(v,"size","()I",self,NULL,0,&size);
   if(!dvm_exception(v)){
    if(IS("empty","()Z"))out->i=size.i==0;
    else if(IS("push","(Ljava/lang/Object;)Ljava/lang/Object;")){
     if(nargs!=1||!a){pthread_mutex_unlock(&state->lock);return false;}
     synchronized_list_call(v,"add","(Ljava/lang/Object;)Z",self,a,1,out);out->l=a[0].l;
    }else if(size.i==0)dvm__throw(v,last?"java/util/NoSuchElementException":"java/util/EmptyStackException","empty Vector/Stack");
    else {union dvm_value index={.i=size.i-1};synchronized_list_call(v,pop?"remove":"get","(I)Ljava/lang/Object;",self,&index,1,out);}
   }
   pthread_mutex_unlock(&state->lock);return true;
  }
 }
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==readonly_destroy)return readonly_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==empty_list_destroy)return empty_list_call(v,m,sig,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==empty_iterator_destroy)return empty_iterator_call(v,m,sig,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==empty_map_destroy)return empty_map_call(v,m,sig,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==empty_set_destroy)return empty_set_call(v,m,sig,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==entry_view_destroy)return entry_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==entry_set_destroy)return entry_set_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==entry_iter_destroy)return entry_iter_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==key_set_destroy)return key_set_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==key_iter_destroy)return key_iter_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==values_view_destroy)return values_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==value_iter_destroy)return value_iter_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==synchronized_map_destroy)return synchronized_map_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==synchronized_list_destroy)return synchronized_list_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==synchronized_list_iterator_destroy)return synchronized_list_iterator_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==singleton_list_destroy)return singleton_list_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->data_destroy==singleton_list_iterator_destroy)return singleton_list_iterator_call(v,m,sig,self,a,nargs,out);
 if(dvm__obj(v,gc_ref_o_88)&&dvm__obj(v,gc_ref_o_88)->cls&&!strcmp(dvm__obj(v,gc_ref_o_88)->cls->name,"java/util/ArrayList$Itr"))return iterator_call(v,m,sig,self,out);
 bool linked=dvm__obj(v,gc_ref_o_88)&&is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/LinkedList"), arraydeque=dvm__obj(v,gc_ref_o_88)&&is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/ArrayDeque");
 if(arraydeque){
  if(!strcmp(m,"<init>")){if(!IS("<init>","()V"))return false;return invoke(list_init,v,self,NULL,0,out);}
  if(!dvm__obj(v,gc_ref_o_88)->data){dvm__throw(v,"java/lang/IllegalStateException","ArrayDeque constructor not called");return true;}
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
   out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/ArrayList$Itr"));const dvm_ref gc_ref_iter_90=(out->l); if(!gc_operation_admit(v,gc_ref_iter_90))return true;struct iterator *it=calloc(1,sizeof *it);
   if(!dvm__obj(v,gc_ref_iter_90)||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","deque iterator");return true;}
   it->target=self;it->expected=list_of(v,self)->mod_count;it->last=-1;dvm__obj(v,gc_ref_iter_90)->data=it;dvm__obj(v,gc_ref_iter_90)->data_destroy=iterator_gc_destroy;return true;
  }
  return false;
 }
 /* AbstractMap.isEmpty uses the concrete receiver's virtual size. */
 if(dvm__obj(v,gc_ref_o_88)&&!is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/HashMap")&&!is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/WeakHashMap")&&IS("isEmpty","()Z")&&
    (is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/AbstractMap")||dvm__class_assignable(v,dvm__obj(v,gc_ref_o_88)->cls,dvm_find_class_or_external(v,"java/util/Map")))){
  struct dvm_method *size=dvm_find_method(v,dvm__obj(v,gc_ref_o_88)->cls,"size","()I");
  if(size&&(size->has_code||size->builtin)){union dvm_value count={0};if(dvm_call(v,size,self,NULL,0,&count))out->i=count.i==0;return true;}
 }
 bool list=dvm__obj(v,gc_ref_o_88)&&(is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/ArrayList")||linked),map=dvm__obj(v,gc_ref_o_88)&&(is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/HashMap")||is_type(dvm__obj(v,gc_ref_o_88)->cls,"java/util/WeakHashMap"));if(!list&&!map)return false;
 if(list){
  bool ctor=IS("<init>","()V")||(!linked&&IS("<init>","(I)V"))||IS("<init>","(Ljava/util/Collection;)V");
  if(ctor){if(IS("<init>","(I)V")&&a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative ArrayList capacity");return true;}
   if(!list_init(v,self,NULL,0,out))return true;
   if(IS("<init>","(I)V")&&a[0].i>8){if(a[0].i>1000000){dvm__throw(v,"java/lang/OutOfMemoryError","diagnostic collection limit");return true;}struct rt_list *l=realloc(dvm__obj(v,gc_ref_o_88)->data,sizeof *l+(size_t)a[0].i*sizeof(dvm_ref));if(!l){dvm__throw(v,"java/lang/OutOfMemoryError","ArrayList capacity");return true;}dvm__obj(v,gc_ref_o_88)->data=l;l->cap=(uint32_t)a[0].i;}
   if(IS("<init>","(Ljava/util/Collection;)V"))return bulk_add(v,self,a[0].l,0,out);return true;
  }
  if(!dvm__obj(v,gc_ref_o_88)->data){dvm__throw(v,"java/lang/IllegalStateException","ArrayList constructor not called");return true;}
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
  if(IS("iterator","()Ljava/util/Iterator;")){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/ArrayList$Itr"));const dvm_ref gc_ref_iter_91=(out->l); if(!gc_operation_admit(v,gc_ref_iter_91))return true;struct iterator *it=calloc(1,sizeof *it);if(!dvm__obj(v,gc_ref_iter_91)||!it){free(it);dvm__throw(v,"java/lang/OutOfMemoryError","ArrayList iterator");return true;}it->target=self;it->expected=list_of(v,self)->mod_count;it->last=-1;dvm__obj(v,gc_ref_iter_91)->data=it;dvm__obj(v,gc_ref_iter_91)->data_destroy=iterator_gc_destroy;return true;}
  if(IS("addAll","(Ljava/util/Collection;)Z"))return bulk_add(v,self,a[0].l,(int)list_of(v,self)->size,out);
  if(IS("addAll","(ILjava/util/Collection;)Z"))return bulk_add(v,self,a[1].l,a[0].i,out);
  if(IS("indexOf","(Ljava/lang/Object;)I")||IS("lastIndexOf","(Ljava/lang/Object;)I")){
   struct rt_list *l=list_of(v,self);out->i=-1;for(uint32_t i=0;i<l->size;++i){bool hit=java_basics_equal(v,a[0].l,l->items[i]);if(dvm_exception(v))return true;if(hit){out->i=(int)i;if(IS("indexOf","(Ljava/lang/Object;)I"))break;}}return true;}
#define LIST(name,signature,fn) if(IS(name,signature))return invoke(fn,v,self,a,nargs,out)
  LIST("size","()I",list_size);LIST("isEmpty","()Z",list_isEmpty);LIST("clear","()V",list_clear);
  LIST("add","(Ljava/lang/Object;)Z",list_add);LIST("add","(ILjava/lang/Object;)V",list_add_at);
  LIST("get","(I)Ljava/lang/Object;",list_get);LIST("set","(ILjava/lang/Object;)Ljava/lang/Object;",list_set);
  LIST("remove","(I)Ljava/lang/Object;",list_remove);LIST("remove","(Ljava/lang/Object;)Z",list_remove_obj);LIST("contains","(Ljava/lang/Object;)Z",list_contains);
  LIST("removeAll","(Ljava/util/Collection;)Z",list_remove_all);
  LIST("toArray","()[Ljava/lang/Object;",list_toArray);LIST("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",list_toArray);
  return false;
 }
 bool ctor=IS("<init>","()V")||IS("<init>","(I)V")||IS("<init>","(IF)V")||IS("<init>","(Ljava/util/Map;)V");
 if(ctor){if((IS("<init>","(I)V")||IS("<init>","(IF)V"))&&a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative HashMap capacity");return true;}if(IS("<init>","(IF)V")&&(!(a[1].f>0)||isnan(a[1].f))){dvm__throw(v,"java/lang/IllegalArgumentException","invalid HashMap load factor");return true;}if(!map_init(v,self,NULL,0,out))return true;if(!IS("<init>","(Ljava/util/Map;)V"))return true;}
 if(!dvm__obj(v,gc_ref_o_88)->data){dvm__throw(v,"java/lang/IllegalStateException","HashMap constructor not called");return true;}
 map_expunge_stale(v,self);
 if(IS("entrySet","()Ljava/util/Set;")){
  /* Backed view, not a snapshot: size/contains/remove/clear/iterator all read
   * and mutate this map, and Entry.getValue() re-reads the live mapping. */
  struct dvm_class *cls=dvm_find_class_or_external(v,"sdktrace/collections/EntrySet");
  entry_set_view *s=calloc(1,sizeof *s);
  if(!cls||!s){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","entrySet view");return true;}
  dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_so_92=(ref); if(!gc_operation_admit(v,gc_ref_so_92))return true;
  if(!dvm__obj(v,gc_ref_so_92)){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","entrySet view object");return true;}
  s->vm=v;s->map=self;dvm_pin(v,self);
  dvm__obj(v,gc_ref_so_92)->data=s;dvm__obj(v,gc_ref_so_92)->data_destroy=entry_set_destroy;
  out->l=ref;return true;
 }
 if(IS("keySet","()Ljava/util/Set;")){
  /* Backed view, not a snapshot: size/contains/remove/clear/iterator all read
   * and mutate this map, so later put/remove calls are immediately visible. */
  struct dvm_class *cls=dvm_find_class_or_external(v,"sdktrace/collections/KeySet");
  key_set_view *s=calloc(1,sizeof *s);
  if(!cls||!s){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","keySet view");return true;}
  dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_so_93=(ref); if(!gc_operation_admit(v,gc_ref_so_93))return true;
  if(!dvm__obj(v,gc_ref_so_93)){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","keySet view object");return true;}
  s->vm=v;s->map=self;dvm_pin(v,self);
  dvm__obj(v,gc_ref_so_93)->data=s;dvm__obj(v,gc_ref_so_93)->data_destroy=key_set_destroy;
  out->l=ref;return true;
 }
 if(IS("values","()Ljava/util/Collection;")){
  /* Backed Collection view, counterpart of keySet(): reads and mutates the
   * same rt_map.e[].v slots.  Collections.synchronizedMap() returns this view
   * through delegation instead of a null/empty stand-in. */
  struct dvm_class *cls=dvm_find_class_or_external(v,"sdktrace/collections/Values");
  values_view *s=calloc(1,sizeof *s);
  if(!cls||!s){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","values view");return true;}
  dvm_ref ref=dvm_new_object(v,cls);const dvm_ref gc_ref_so_94=(ref); if(!gc_operation_admit(v,gc_ref_so_94))return true;
  if(!dvm__obj(v,gc_ref_so_94)){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","values view object");return true;}
  s->vm=v;s->map=self;dvm_pin(v,self);
  dvm__obj(v,gc_ref_so_94)->data=s;dvm__obj(v,gc_ref_so_94)->data_destroy=values_view_destroy;
  out->l=ref;return true;
 }
 if(IS("putAll","(Ljava/util/Map;)V")||IS("<init>","(Ljava/util/Map;)V")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","source Map");return true;}const dvm_ref gc_ref_src_95=(a[0].l); if(!gc_operation_admit(v,gc_ref_src_95))return true;
  if(dvm__obj(v,gc_ref_src_95)&&dvm__obj(v,gc_ref_src_95)->data_destroy==map_destroy&&dvm__obj(v,gc_ref_src_95)->data){
   map_expunge_stale(v,a[0].l);
   struct rt_map *mapdata=dvm__obj(v,gc_ref_src_95)->data;size_t bytes=sizeof *mapdata+(size_t)mapdata->cap*sizeof mapdata->e[0];struct rt_map *copy=malloc(bytes);if(!copy){dvm__throw(v,"java/lang/OutOfMemoryError","Map snapshot");return true;}memcpy(copy,mapdata,bytes);
   for(uint32_t i=0;i<copy->size;++i){union dvm_value args[]={{.l=copy->e[i].k},{.l=copy->e[i].v}},ignored={0};if(!map_put(v,self,args,2,&ignored))break;}free(copy);return true;
  }
  return map_copy_from_source(v,self,a[0].l,out);
 }
 if(IS("containsValue","(Ljava/lang/Object;)Z")){struct rt_map *mp=dvm__obj(v,gc_ref_o_88)->data;for(uint32_t i=0;i<mp->size;++i){bool hit=java_basics_equal(v,a[0].l,mp->e[i].v);if(dvm_exception(v))return true;if(hit){out->i=1;break;}}return true;}
 if(IS("getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")){if(!map_get(v,self,a,1,out))return true;if(!out->l){union dvm_value present={0};if(!map_containsKey(v,self,a,1,&present))return true;if(!present.i)out->l=a[1].l;}return true;}
 /* AbstractMap contracts: Collections.synchronizedMap() delegates equals(),
  * hashCode() and toString() to the backing map, so a HashMap backing must
  * answer them instead of failing the wrapper dispatch. */
 if(IS("equals","(Ljava/lang/Object;)Z"))return map_equals(v,self,a[0].l,out);
 if(IS("hashCode","()I"))return map_hashcode(v,self,out);
 if(IS("toString","()Ljava/lang/String;"))return map_tostring(v,self,out);
#define MAP(name,signature,fn) if(IS(name,signature))return invoke(fn,v,self,a,nargs,out)
 MAP("put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map_put);MAP("get","(Ljava/lang/Object;)Ljava/lang/Object;",map_get);MAP("remove","(Ljava/lang/Object;)Ljava/lang/Object;",map_remove);MAP("containsKey","(Ljava/lang/Object;)Z",map_containsKey);MAP("size","()I",map_size);MAP("isEmpty","()Z",map_isEmpty);MAP("clear","()V",map_clear);
 return false;
}
bool collections_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return true;
 bool result=collections_call_operation(v,c,m,sig,self,a,nargs,out);
 gc_operation_result(&operation,sig,result&&out?out->l:0);
 return result;
}


/* Weak-key pin ownership and real temporary-release checks. This deliberately
 * does not claim collector/reference-processing coverage. */
static bool collections_weak_map_selftest_operation(struct dvm *v){
 const char *wm="java/util/WeakHashMap",*map="java/util/Map";
 union dvm_value out={0},a[2]={{0},{0}};
 unsigned checks=0;
#define WEAK_CHECK(x) do{++checks;if(!(x)||dvm_exception(v)){sdk_trace_fprintf(stderr,"WEAK_HASH_MAP_SELFTEST FAIL line=%d check=%s\n",__LINE__,#x);return false;}}while(0)
 dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,wm));
 WEAK_CHECK(r&&collections_call(v,wm,"<init>","()V",r,NULL,0,&out));
 WEAK_CHECK(dvm__class_assignable(v,dvm_object_class(v,r),dvm_find_class_or_external(v,map)));
 WEAK_CHECK(!is_type(dvm_object_class(v,r),"java/util/HashMap"));
 WEAK_CHECK(dvm_find_method(v,dvm_object_class(v,r),"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")!=NULL);
 dvm_ref key=dvm_new_string(v,"weak-key"),equal=dvm_new_string(v,"weak-key");
 dvm_ref first=dvm_new_string(v,"weak-value-old"),second=dvm_new_string(v,"weak-value-new");
 WEAK_CHECK(key&&equal&&first&&second);
 uint32_t keypins=dvm__obj(v,key)->pins,firstpins=dvm__obj(v,first)->pins,secondpins=dvm__obj(v,second)->pins;
 a[0].l=key;a[1].l=first;
 WEAK_CHECK(collections_call(v,map,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out)&&out.l==0);
 WEAK_CHECK(dvm__obj(v,key)->pins==keypins&&dvm__obj(v,first)->pins==firstpins+1);
 a[0].l=equal;a[1].l=second;
 WEAK_CHECK(collections_call(v,map,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out)&&out.l==first);
 WEAK_CHECK(dvm__obj(v,key)->pins==keypins&&dvm__obj(v,equal)->pins==0);
 WEAK_CHECK(dvm__obj(v,first)->pins==firstpins&&dvm__obj(v,second)->pins==secondpins+1);
 WEAK_CHECK(collections_call(v,map,"size","()I",r,NULL,0,&out)&&out.i==1);
 WEAK_CHECK(collections_call(v,map,"get","(Ljava/lang/Object;)Ljava/lang/Object;",r,a,1,&out)&&out.l==second);
 WEAK_CHECK(collections_call(v,map,"keySet","()Ljava/util/Set;",r,NULL,0,&out)&&out.l);
 dvm_ref keys=out.l;
 /* This exercises real VM temporary destruction and slot reuse, not GC. */
 uint64_t identity=dvm__obj(v,key)->identity_generation;
 dvm__discard_temporary(v,key);
 WEAK_CHECK(dvm__obj(v,key)==NULL);
 dvm_ref reused=dvm_new_string(v,"replacement-is-not-a-key");
 WEAK_CHECK(reused==key&&dvm__obj(v,reused)->identity_generation!=identity);
 WEAK_CHECK(collections_call(v,"java/util/Set","size","()I",keys,NULL,0,&out)&&out.i==0);
 WEAK_CHECK(dvm__obj(v,second)->pins==secondpins);
 a[0].l=0;a[1].l=first;
 WEAK_CHECK(collections_call(v,map,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out));
 WEAK_CHECK(collections_call(v,map,"containsKey","(Ljava/lang/Object;)Z",r,a,1,&out)&&out.i);
 WEAK_CHECK(collections_call(v,map,"get","(Ljava/lang/Object;)Ljava/lang/Object;",r,a,1,&out)&&out.l==first);
 WEAK_CHECK(collections_call(v,map,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",r,a,1,&out)&&out.l==first);
 WEAK_CHECK(dvm__obj(v,first)->pins==firstpins);
 a[0].l=equal;a[1].l=0;
 WEAK_CHECK(collections_call(v,map,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out));
 a[1].l=first;
 WEAK_CHECK(collections_call(v,map,"getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",r,a,2,&out)&&out.l==0);
 WEAK_CHECK(collections_call(v,map,"clear","()V",r,NULL,0,&out));
 WEAK_CHECK(collections_call(v,map,"isEmpty","()Z",r,NULL,0,&out)&&out.i);
 /* A weak map owns strong value pins, including destructor cleanup. */
 dvm_ref doomed=dvm_new_object(v,dvm_find_class_or_external(v,wm));
 WEAK_CHECK(doomed&&collections_call(v,wm,"<init>","(I)V",doomed,(union dvm_value[]){{.i=1}},1,&out));
 a[0].l=equal;a[1].l=first;
 WEAK_CHECK(collections_call(v,map,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",doomed,a,2,&out));
 WEAK_CHECK(dvm__obj(v,first)->pins==firstpins+1&&dvm__obj(v,equal)->pins==0);
 dvm__discard_temporary(v,doomed);
 WEAK_CHECK(!dvm__obj(v,doomed)&&dvm__obj(v,first)->pins==firstpins);
 sdk_trace_fprintf(stderr,"WEAK_HASH_MAP_SELFTEST PASS checks=%u java_equals=1 null_key=1 null_value=1 weak_key_no_pin=1 strong_value_pin=1 replace_release=1 remove_release=1 destructor_release=1 live_view=1 discarded_key_slot_reuse=1 gc_tested=0\n",checks);
#undef WEAK_CHECK
 return true;
}
bool collections_weak_map_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=collections_weak_map_selftest_operation(v);
 return result;
}


/* Native collection payload bytes, separate from guest object/slot/array
 * storage already charged to the arena. Count each owning payload once;
 * backing objects are visited by the heap scan, never recursively added. */
size_t collections_storage_bytes(struct dvm *v){
 extern size_t set_runtime_storage_bytes(struct dvm *),java_basics_collection_storage_bytes(struct dvm *),list_views_storage_bytes(struct dvm *);
 size_t bytes=0;
 for(dvm_ref r=1;r<=v->heap_size;r++){
  struct dvm_object *o=dvm__obj(v,r);if(!o||!o->data||!o->cls)continue;
  if(o->data_destroy==map_destroy){struct rt_map *p=o->data;bytes+=sizeof *p+(size_t)p->cap*sizeof p->e[0];}
  else if(!o->data_destroy&&(is_type(o->cls,"java/util/ArrayList")||is_type(o->cls,"java/util/LinkedList")||!strcmp(o->cls->name,"java/util/ArrayDeque"))&&strcmp(o->cls->name,"java/util/ArrayList$SubList")){struct rt_list *p=o->data;bytes+=sizeof *p+(size_t)p->cap*sizeof p->items[0];}
  else if(!o->data_destroy&&!strcmp(o->cls->name,"java/util/ArrayList$Itr"))bytes+=sizeof(struct iterator);
#define PAYLOAD(dtor,type) else if(o->data_destroy==dtor)bytes+=sizeof(type)
  PAYLOAD(entry_view_destroy,entry_view);
  PAYLOAD(entry_set_destroy,entry_set_view);
  PAYLOAD(entry_iter_destroy,entry_iter_view);
  PAYLOAD(key_set_destroy,key_set_view);
  PAYLOAD(key_iter_destroy,key_iter_view);
  PAYLOAD(values_view_destroy,values_view);
  PAYLOAD(value_iter_destroy,value_iter_view);
  PAYLOAD(synchronized_map_destroy,synchronized_map_view);
  PAYLOAD(readonly_destroy,readonly_view);
  PAYLOAD(synchronized_list_destroy,synchronized_list_view);
  PAYLOAD(synchronized_list_iterator_destroy,synchronized_list_iterator_view);
  PAYLOAD(singleton_list_destroy,singleton_list_view);
  PAYLOAD(singleton_list_iterator_destroy,singleton_list_iterator_view);
#undef PAYLOAD
 }
 return bytes+set_runtime_storage_bytes(v)+java_basics_collection_storage_bytes(v)+list_views_storage_bytes(v);
}

int collections_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 struct gc_operation_pin pin __attribute__((cleanup(gc_operation_pin_end)));
 if(!gc_operation_pin_begin(v,&pin,o))return DVM_GC_PAYLOAD_INVALID;
 o=dvm__obj(v,pin.ref);

 (void)v;void *p=o->data;
 if(o->data_destroy==list_gc_destroy){struct rt_list *l=p;if(l->size>l->cap||l->cap>1000000)return DVM_GC_PAYLOAD_INVALID;
  if(visit)for(uint32_t i=0;i<l->size;i++)visit(&l->items[i],user);return DVM_GC_PAYLOAD_REFS;}
 if(o->data_destroy==map_destroy){struct rt_map *m=p;if(m->size>m->cap||m->cap>1000000)return DVM_GC_PAYLOAD_INVALID;
  /* Weak keys are retained, not cleared, in the E2 nonmoving phase. */
  if(visit){for(uint32_t i=0;i<m->size;i++){visit(&m->e[i].k,user);visit(&m->e[i].v,user);visit(&m->e[i].node,user);}for(int i=0;i<3;i++)visit(&m->views[i],user);}return DVM_GC_PAYLOAD_REFS;}
 if(o->data_destroy==iterator_gc_destroy){if(visit)visit(&((struct iterator*)p)->target,user);return DVM_GC_PAYLOAD_REFS;}
#define GC_EDGE(destroy,type,field) if(o->data_destroy==destroy){if(visit)visit(&((type*)p)->field,user);return DVM_GC_PAYLOAD_REFS;}
 if(o->data_destroy==entry_view_destroy){entry_view *e=p;if(visit){visit(&e->map,user);visit(&e->key,user);visit(&e->value,user);}return DVM_GC_PAYLOAD_REFS;}
 GC_EDGE(entry_set_destroy,entry_set_view,map)
 GC_EDGE(entry_iter_destroy,entry_iter_view,owner)
 GC_EDGE(key_set_destroy,key_set_view,map)
 GC_EDGE(key_iter_destroy,key_iter_view,owner)
 GC_EDGE(values_view_destroy,values_view,map)
 GC_EDGE(value_iter_destroy,value_iter_view,owner)
 GC_EDGE(synchronized_map_destroy,synchronized_map_view,backing)
 GC_EDGE(readonly_destroy,readonly_view,backing)
 GC_EDGE(synchronized_list_destroy,synchronized_list_view,backing)
 GC_EDGE(synchronized_list_iterator_destroy,synchronized_list_iterator_view,owner)
 GC_EDGE(singleton_list_destroy,singleton_list_view,element)
 GC_EDGE(singleton_list_iterator_destroy,singleton_list_iterator_view,owner)
#undef GC_EDGE
 return DVM_GC_PAYLOAD_UNKNOWN;
}
