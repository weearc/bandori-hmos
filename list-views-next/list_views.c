#include "list_views.h"
#include "collections.h"
#include "java_basics.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);

/* One pinned source per view. A view keeps its source alive even when the
 * interpreted stack no longer refers to that source directly. */
struct view { struct dvm *vm; dvm_ref source; int readonly; };
struct cursor { struct dvm *vm; dvm_ref view; uint32_t index; };
static void release_view(void *p){struct view *x=p;if(x){dvm_unpin(x->vm,x->source);free(x);}}
static void release_cursor(void *p){struct cursor *x=p;if(x){dvm_unpin(x->vm,x->view);free(x);}}
size_t list_views_storage_bytes(struct dvm *v){
 size_t bytes=0;for(dvm_ref r=1;r<=v->heap_size;r++){struct dvm_object *o=dvm__obj(v,r);if(!o||!o->data)continue;
  if(o->data_destroy==release_view)bytes+=sizeof(struct view);
  else if(o->data_destroy==release_cursor)bytes+=sizeof(struct cursor);
 }return bytes;
}
static bool eq(const char *a,const char *b){return !strcmp(a,b);}
static bool method(const char *m,const char *s,const char *name,const char *sig){return eq(m,name)&&eq(s,sig);}
static struct dvm_object *obj(struct dvm *v,dvm_ref r){return r?dvm__obj(v,r):NULL;}
static bool array_source(struct dvm *v,dvm_ref r){struct dvm_object *o=obj(v,r);return o&&o->kind==DVM_OBJ_ARRAY&&o->elem_kind=='L';}
static struct view *view_of(struct dvm *v,dvm_ref r){struct dvm_object *o=obj(v,r);return o&&o->data_destroy==release_view?(struct view *)o->data:NULL;}
static bool raise(struct dvm *v,const char *type,const char *detail){dvm__throw(v,type,"%s",detail);return true;}
static bool size_of(struct dvm *v,dvm_ref r,uint32_t *n){
 if(array_source(v,r)){*n=dvm_array_length(v,r);return true;}
 struct dvm_object *o=obj(v,r);if(o&&o->cls&&(eq(o->cls->name,"java/util/Arrays$ArrayList")||eq(o->cls->name,"java/util/Collections$UnmodifiableList"))){struct view *x=view_of(v,r);return x&&size_of(v,x->source,n);}
 union dvm_value out={0};if(o&&o->cls&&collections_call(v,o->cls->name,"size","()I",r,NULL,0,&out)&&!dvm_exception(v)){*n=(uint32_t)out.i;return true;}
 return false;
}
static bool get_at(struct dvm *v,dvm_ref r,uint32_t i,dvm_ref *value){
 if(array_source(v,r)){*value=((dvm_ref *)dvm_array_data(v,r))[i];return true;}
 struct dvm_object *o=obj(v,r);if(o&&o->cls&&(eq(o->cls->name,"java/util/Arrays$ArrayList")||eq(o->cls->name,"java/util/Collections$UnmodifiableList"))){struct view *x=view_of(v,r);return x&&get_at(v,x->source,i,value);}
 union dvm_value a={.i=(int)i},out={0};if(o&&o->cls&&collections_call(v,o->cls->name,"get","(I)Ljava/lang/Object;",r,&a,1,&out)&&!dvm_exception(v)){*value=out.l;return true;}
 return false;
}
static dvm_ref make_view(struct dvm *v,const char *name,dvm_ref source,int readonly){
 struct dvm_class *cls=dvm_find_class_or_external(v,name);dvm_ref r=cls?dvm_new_object(v,cls):0;struct dvm_object *o=obj(v,r);struct view *x=calloc(1,sizeof *x);
 if(!o||!x){free(x);dvm__throw(v,"java/lang/OutOfMemoryError","list view");return 0;}
 x->vm=v;x->source=source;x->readonly=readonly;dvm_pin(v,source);o->data=x;o->data_destroy=release_view;return r;
}
static bool exception_named(struct dvm *v,const char *name){struct dvm_object *e=obj(v,dvm_exception(v));return e&&e->cls&&e->cls->name&&eq(e->cls->name,name);}
/* Java List.toArray contract over the view's backing list: shallow-copy the
 * element references, never return the backing array itself, and for the
 * array overload reuse a big enough caller array (null-terminating at size)
 * or allocate a new one with the caller array's runtime element type. */
static dvm_ref to_array_copy(struct dvm *v,struct view *x,uint32_t len,dvm_ref target){
 struct dvm_object *array=target?obj(v,target):NULL;uint32_t capacity=target?dvm_array_length(v,target):0;
 dvm_ref result=target;
 if(!target||capacity<len){const char *elem="Ljava/lang/Object;";if(array&&array->cls&&array->cls->elem&&array->cls->elem->desc)elem=array->cls->elem->desc;result=dvm_new_array(v,'L',elem,len);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","list toArray array");return 0;}}
 dvm_ref *items=dvm_array_data(v,result);
 for(uint32_t i=0;i<len;i++){dvm_ref item=0;if(!get_at(v,x->source,i,&item))return 0;if(dvm_exception(v))return 0;items[i]=item;}
 if(target&&capacity>len)items[len]=0;
 return result;
}
static bool list_views_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){struct dvm_method *m=dvm__builtin_method();return m&&m->cls&&m->name&&m->sig?list_views_call(v,m->cls->name,m->name,m->sig,self,a,n,out):false;}
/* collections.c snapshots bulk sources through dvm_find_method(); install the
 * two List.toArray overloads so UnmodifiableList/Arrays$ArrayList sources are
 * found there instead of being reported as MISSING_API. */
static void install_to_array_methods(struct dvm *v,struct dvm_class *c){
 if(!c||c->nmethods)return;
 c->methods=calloc(2,sizeof *c->methods);if(!c->methods){dvm__throw(v,"java/lang/OutOfMemoryError","list view toArray methods");return;}
 c->nmethods=2;struct dvm_method *m=c->methods;
 m[0].cls=c;m[0].name="toArray";m[0].sig=strdup("()[Ljava/lang/Object;");m[0].builtin=list_views_builtin;m[0].ret_kind='L';
 m[1].cls=c;m[1].name="toArray";m[1].sig=strdup("([Ljava/lang/Object;)[Ljava/lang/Object;");m[1].builtin=list_views_builtin;m[1].ret_kind='L';m[1].arg_count=1;m[1].arg_slots=1;
 if(!m[0].sig||!m[1].sig)dvm__throw(v,"java/lang/OutOfMemoryError","list view toArray signatures");
}
void list_views_vm_start(struct dvm *v){
 const char *names[]={"Ljava/util/Arrays$ArrayList;","Ljava/util/Collections$UnmodifiableList;","Ljava/util/Arrays$ArrayList$Itr;","Ljava/util/Collections$UnmodifiableList$Itr;"};
 for(int i=0;i<4;i++){struct dvm_class *c=dvm__class_by_desc(v,names[i]);if(!c)continue;c->super=dvm__class_by_desc(v,"Ljava/lang/Object;");if(i<2)install_to_array_methods(v,c);if(c->ifaces)continue;if(i<2){const char *interfaces[]={"Ljava/util/List;","Ljava/util/RandomAccess;","Ljava/lang/Iterable;"};c->ifaces=calloc(3,sizeof *c->ifaces);if(!c->ifaces){dvm__throw(v,"java/lang/OutOfMemoryError","list view interfaces");return;}c->nifaces=3;for(int j=0;j<3;j++)c->ifaces[j]=dvm__class_by_desc(v,interfaces[j]);}else{c->ifaces=calloc(1,sizeof *c->ifaces);if(!c->ifaces){dvm__throw(v,"java/lang/OutOfMemoryError","iterator interface");return;}c->nifaces=1;c->ifaces[0]=dvm__class_by_desc(v,"Ljava/util/Iterator;");}}
}
bool list_views_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(eq(c,"java/util/Arrays")&&method(m,sig,"asList","([Ljava/lang/Object;)Ljava/util/List;")&&n==1){if(!a[0].l)return raise(v,"java/lang/NullPointerException","Arrays.asList array");if(!array_source(v,a[0].l))return false;out->l=make_view(v,"java/util/Arrays$ArrayList",a[0].l,0);return true;}
 if(eq(c,"java/util/Collections")&&method(m,sig,"unmodifiableList","(Ljava/util/List;)Ljava/util/List;")&&n==1){if(!a[0].l)return raise(v,"java/lang/NullPointerException","unmodifiableList source");uint32_t len;if(!size_of(v,a[0].l,&len))return false;out->l=make_view(v,"java/util/Collections$UnmodifiableList",a[0].l,1);return true;}
 struct dvm_object *o=obj(v,self);if(!o||!o->cls)return false;
 bool iter=eq(o->cls->name,"java/util/Arrays$ArrayList$Itr")||eq(o->cls->name,"java/util/Collections$UnmodifiableList$Itr");
 if(iter){struct cursor *it=o->data_destroy==release_cursor?o->data:NULL;if(!it)return raise(v,"java/lang/IllegalStateException","uninitialized list iterator");uint32_t len;if(!size_of(v,it->view,&len))return false;
  if(method(m,sig,"hasNext","()Z")){out->i=it->index<len;return true;}
  if(method(m,sig,"next","()Ljava/lang/Object;")){if(it->index>=len)return raise(v,"java/util/NoSuchElementException","list iterator exhausted");return get_at(v,it->view,it->index++,&out->l);}
  if(method(m,sig,"remove","()V"))return raise(v,"java/lang/UnsupportedOperationException","list iterator remove");return false;}
 bool fixed=eq(o->cls->name,"java/util/Arrays$ArrayList"),readonly=eq(o->cls->name,"java/util/Collections$UnmodifiableList");if(!fixed&&!readonly)return false;
 struct view *x=view_of(v,self);if(!x)return raise(v,"java/lang/IllegalStateException","uninitialized list view");uint32_t len;if(!size_of(v,x->source,&len))return false;
 if(method(m,sig,"size","()I")){out->i=(int)len;return true;}
 if(method(m,sig,"isEmpty","()Z")){out->i=!len;return true;}
 if(method(m,sig,"get","(I)Ljava/lang/Object;")){if(a[0].i<0||(uint32_t)a[0].i>=len)return raise(v,"java/lang/IndexOutOfBoundsException","list get");return get_at(v,x->source,(uint32_t)a[0].i,&out->l);}
 if(method(m,sig,"toArray","()[Ljava/lang/Object;")){dvm_ref result=to_array_copy(v,x,len,0);if(!result)return dvm_exception(v);out->l=result;return true;}
 if(method(m,sig,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;")){if(n!=1||!a[0].l)return raise(v,"java/lang/NullPointerException","list toArray array");struct dvm_object *array=obj(v,a[0].l);if(!array||array->kind!=DVM_OBJ_ARRAY||array->elem_kind!='L')return raise(v,"java/lang/ArrayStoreException","list toArray requires an object array");dvm_ref result=to_array_copy(v,x,len,a[0].l);if(!result)return dvm_exception(v);out->l=result;return true;}
 if(method(m,sig,"contains","(Ljava/lang/Object;)Z")){for(uint32_t i=0;i<len;i++){dvm_ref item=0;if(!get_at(v,x->source,i,&item))return false;if(java_basics_equal(v,item,a[0].l)){out->i=1;return true;}if(dvm_exception(v))return true;}out->i=0;return true;}
 if(method(m,sig,"iterator","()Ljava/util/Iterator;")){const char *name=fixed?"java/util/Arrays$ArrayList$Itr":"java/util/Collections$UnmodifiableList$Itr";out->l=dvm_new_object(v,dvm_find_class_or_external(v,name));struct dvm_object *io=obj(v,out->l);struct cursor *it=calloc(1,sizeof *it);if(!io||!it){free(it);return raise(v,"java/lang/OutOfMemoryError","list iterator");}it->vm=v;it->view=self;dvm_pin(v,self);io->data=it;io->data_destroy=release_cursor;return true;}
 if(method(m,sig,"set","(ILjava/lang/Object;)Ljava/lang/Object;")){if(readonly)return raise(v,"java/lang/UnsupportedOperationException","unmodifiable list set");if(a[0].i<0||(uint32_t)a[0].i>=len)return raise(v,"java/lang/IndexOutOfBoundsException","list set");dvm_ref *items=dvm_array_data(v,x->source);out->l=items[a[0].i];items[a[0].i]=a[1].l;return true;}
 if(method(m,sig,"add","(Ljava/lang/Object;)Z")||method(m,sig,"add","(ILjava/lang/Object;)V")||method(m,sig,"remove","(I)Ljava/lang/Object;")||method(m,sig,"remove","(Ljava/lang/Object;)Z")||method(m,sig,"clear","()V"))return raise(v,"java/lang/UnsupportedOperationException",readonly?"unmodifiable list":"fixed-size list");
 return false;
}

bool list_views_selftest(struct dvm *v){
 dvm_ref array=dvm_new_array(v,'L',"Ljava/lang/Object;",2);if(!array)return false;
 dvm_ref first=dvm_new_string(v,"first"),second=dvm_new_string(v,"second"),third=dvm_new_string(v,"third");
 dvm_ref *elements=dvm_array_data(v,array);elements[0]=first;elements[1]=second;
 union dvm_value arg={.l=array},out={0};
 if(!list_views_call(v,"java/util/Arrays","asList","([Ljava/lang/Object;)Ljava/util/List;",0,&arg,1,&out)||!out.l)return false;
 dvm_ref fixed=out.l;arg.l=fixed;out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/Collections","unmodifiableList","(Ljava/util/List;)Ljava/util/List;",0,&arg,1,&out)||!out.l)return false;
 dvm_ref ro=out.l;elements[0]=third;union dvm_value index={.i=0};out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/List","get","(I)Ljava/lang/Object;",ro,&index,1,&out)||out.l!=third)return false;
 union dvm_value setargs[2]={{.i=1},{.l=first}};out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/List","set","(ILjava/lang/Object;)Ljava/lang/Object;",fixed,setargs,2,&out)||out.l!=second||elements[1]!=first)return false;
 if(!list_views_call(v,"java/util/List","iterator","()Ljava/util/Iterator;",ro,NULL,0,&out)||!out.l)return false;
 dvm_ref iterator=out.l;out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/Iterator","hasNext","()Z",iterator,NULL,0,&out)||!out.i)return false;
 if(!list_views_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",iterator,NULL,0,&out)||out.l!=third)return false;
 if(!list_views_call(v,"java/util/List","set","(ILjava/lang/Object;)Ljava/lang/Object;",ro,setargs,2,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 arg.l=first;if(!list_views_call(v,"java/util/List","add","(Ljava/lang/Object;)Z",ro,&arg,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 index.i=0;if(!list_views_call(v,"java/util/List","remove","(I)Ljava/lang/Object;",ro,&index,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 /* toArray must copy elements into a fresh array and must not expose or
  * mutate the backing array; the array overload follows the JDK rules. */
 dvm_ref abc_array=dvm_new_array(v,'L',"Ljava/lang/Object;",3);if(!abc_array)return false;
 dvm_ref *abc=dvm_array_data(v,abc_array);abc[0]=first;abc[1]=second;abc[2]=third;
 union dvm_value abc_arg={.l=abc_array};out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/Arrays","asList","([Ljava/lang/Object;)Ljava/util/List;",0,&abc_arg,1,&out)||!out.l)return false;
 dvm_ref abc_fixed=out.l;abc_arg.l=abc_fixed;out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/Collections","unmodifiableList","(Ljava/util/List;)Ljava/util/List;",0,&abc_arg,1,&out)||!out.l)return false;
 dvm_ref abc_ro=out.l;out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/List","toArray","()[Ljava/lang/Object;",abc_ro,NULL,0,&out)||!out.l||dvm_array_length(v,out.l)!=3)return false;
 dvm_ref *snapshot=dvm_array_data(v,out.l);
 if(snapshot==abc||snapshot[0]!=first||snapshot[1]!=second||snapshot[2]!=third)return false;
 snapshot[0]=second;if(abc[0]!=first)return false;
 dvm_ref exact=dvm_new_array(v,'L',"Ljava/lang/Object;",3);if(!exact)return false;
 union dvm_value exact_arg={.l=exact};out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/List","toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",abc_ro,&exact_arg,1,&out)||out.l!=exact)return false;
 dvm_ref *exact_items=dvm_array_data(v,exact);
 if(exact_items[0]!=first||exact_items[1]!=second||exact_items[2]!=third)return false;
 dvm_ref big=dvm_new_array(v,'L',"Ljava/lang/Object;",5);if(!big)return false;
 dvm_ref *big_items=dvm_array_data(v,big);big_items[3]=first;big_items[4]=first;
 union dvm_value big_arg={.l=big};out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/List","toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",abc_ro,&big_arg,1,&out)||out.l!=big)return false;
 if(big_items[0]!=first||big_items[1]!=second||big_items[2]!=third||big_items[3]!=0||big_items[4]!=first)return false;
 dvm_ref zero=dvm_new_array(v,'L',"Ljava/lang/Object;",0);if(!zero)return false;
 union dvm_value zero_arg={.l=zero};out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/List","toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",abc_ro,&zero_arg,1,&out)||!out.l||out.l==zero||dvm_array_length(v,out.l)!=3)return false;
 dvm_ref *zero_items=dvm_array_data(v,out.l);
 if(zero_items[0]!=first||zero_items[1]!=second||zero_items[2]!=third)return false;
 dvm_ref narrow=dvm_new_array(v,'L',"Ljava/lang/String;",1);if(!narrow)return false;
 union dvm_value narrow_arg={.l=narrow};out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/List","toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",abc_ro,&narrow_arg,1,&out)||!out.l||out.l==narrow||dvm_array_length(v,out.l)!=3)return false;
 struct dvm_class *narrow_cls=dvm_object_class(v,out.l);if(!narrow_cls||!narrow_cls->elem||!eq(narrow_cls->elem->name,"java/lang/String"))return false;
 union dvm_value null_array={.l=0};out=(union dvm_value){0};
 if(!list_views_call(v,"java/util/List","toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",abc_ro,&null_array,1,&out))return false;
 if(!exception_named(v,"java/lang/NullPointerException"))return false;dvm_clear_exception(v);
 out=(union dvm_value){0};
 if(list_views_call(v,"java/util/List","toArray","(I)Ljava/lang/Object;",abc_ro,NULL,0,&out))return false;
 if(dvm_exception(v))return false;
 sdk_trace_fprintf(stderr,"LIST_VIEWS_SELFTEST PASS backed_array=1 fixed_size=1 readonly=1 iterator=1 toarray_copy=1 toarray_exact=1 toarray_oversize_null=1 toarray_zero_alloc=1 toarray_runtime_type=1 toarray_npe=1 toarray_signature=1\n");return true;
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int list_views_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;if(o->data_destroy==release_view){if(visit)visit(&((struct view*)o->data)->source,user);return DVM_GC_PAYLOAD_REFS;}
 if(o->data_destroy==release_cursor){if(visit)visit(&((struct cursor*)o->data)->view,user);return DVM_GC_PAYLOAD_REFS;}return DVM_GC_PAYLOAD_UNKNOWN;
}
