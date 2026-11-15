/* android.util.Pair and android.content.ContentValues.
 *
 * DEX evidence (BanGDream APK):
 *   Pair.<init>(Object,Object)V       34 call sites
 *   Pair.create(Object,Object)Pair     5 call sites
 *   ContentValues.<init>()V           48 call sites
 *   ContentValues.<init>(I)V           1 call site
 *   ContentValues.put(String,String/Integer/Long/Boolean/byte[]) heavy use
 *   ContentValues.getAsString/containsKey/size call sites
 * The remaining ContentValues accessors are the class's own contract and are
 * implemented over the same real HashMap so a later DEX path cannot silently
 * observe a missing method.
 *
 * Storage is never synthesized: Pair keeps both references in its instance
 * slots (visible to guest iget/iput) and pins them for the payload's lifetime;
 * ContentValues owns a real java.util.HashMap object from collections.c and
 * delegates every accessor, so keySet()/values() are backed views. */
#include "android_util.h"
#include "collections.h"
#include "java_basics.h"
#include "string_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

static struct dvm *active;
static struct dvm_class *pair_class;
static struct dvm_class *values_class;

static bool fail(struct dvm *v,const char *type,const char *message){
 dvm__throw(v,type,"%s",message);
 return true;
}
static void install_method(struct dvm_class *c,int index,const char *name,const char *sig,uint32_t access,dvm_builtin_fn fn,char ret_kind){
 struct dvm_method *m=&c->methods[index];
 m->cls=c;m->name=name;m->sig=strdup(sig);m->access=access;m->ret_kind=ret_kind;m->builtin=fn;
}
static void install_field(struct dvm_field *f,struct dvm_class *owner,const char *name,const char *type,uint32_t access,uint16_t slot){
 f->cls=owner;f->name=name;f->type=type;f->access=access;
 f->kind=dvm__kind_of(type);f->width=1;f->slot=slot;
}
static bool is_class(struct dvm_object *o,const char *name){
 return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,name);
}
/* The receiver is one of the classes this module installed.  Compare the
 * class identity first: a guest new-instance resolves the descriptor through
 * dvm__class_by_desc(), which may return a dex definition that another module
 * registered after us.  The name comparison stays as the descriptor-level
 * fallback and keeps a dex-defined Pair/ContentValues usable. */
static bool pair_receiver(struct dvm_object *o){
 if(!o||!o->cls)return false;
 return o->cls==pair_class||is_class(o,"android/util/Pair");
}
static bool values_receiver(struct dvm_object *o){
 if(!o||!o->cls)return false;
 return o->cls==values_class||is_class(o,"android/content/ContentValues");
}
/* One line that names the concrete object state at a failure, so a device
 * report distinguishes a bad receiver, a foreign payload and a missing class
 * without a rebuild. */
static void payload_diag(const char *stage,const char *why,struct dvm_object *o,struct dvm_class *expect){
 sdk_trace_fprintf(stderr,"ANDROID_UTIL_DIAG stage=%s why=%s self=%p cls=%p expect=%p cls_name=%s expect_name=%s data=%p destroy=%p slots=%p islots=%d\n",
  stage,why,(void*)o,(void*)(o?o->cls:NULL),(void*)expect,
  o&&o->cls&&o->cls->name?o->cls->name:"(none)",
  expect&&expect->name?expect->name:"(none)",
  o?o->data:NULL,o?(void*)o->data_destroy:NULL,o?(void*)o->slots:NULL,
  o&&o->cls?o->cls->islots:0);
}

/* ---- android.util.Pair ------------------------------------------------- */
struct pair_box { struct dvm *vm; dvm_ref first,second; };
static void pair_box_destroy(void *p){
 struct pair_box *b=p;
 if(!b)return;
 if(b->vm){
  if(b->first)dvm_unpin(b->vm,b->first);
  if(b->second)dvm_unpin(b->vm,b->second);
 }
 free(b);
}
static struct pair_box *pair_box_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!is_class(o,"android/util/Pair")||o->data_destroy!=pair_box_destroy||!o->data)return NULL;
 return o->data;
}
static bool pair_slots(struct dvm *v,dvm_ref self,dvm_ref *first,dvm_ref *second){
 struct pair_box *b=pair_box_of(v,self);
 if(!b){payload_diag("pair_slots","missing payload",dvm__obj(v,self),pair_class);return fail(v,self?"java/lang/IllegalStateException":"java/lang/NullPointerException","Pair receiver");}
 struct dvm_object *o=dvm__obj(v,self);
 *first=o->slots?o->slots[0].l:b->first;
 *second=o->slots?o->slots[1].l:b->second;
 return true;
}
static bool pair_init_values(struct dvm *v,dvm_ref self,const union dvm_value *a,int n){
 if(n!=2)return fail(v,"java/lang/IllegalArgumentException","Pair constructor arguments");
 struct dvm_object *o=dvm__obj(v,self);
 if(!pair_receiver(o)){payload_diag("pair_init_receiver","receiver type",o,pair_class);return fail(v,self?"java/lang/IllegalStateException":"java/lang/NullPointerException","Pair constructor receiver");}
 if(o->data&&o->data_destroy!=pair_box_destroy){payload_diag("pair_init_payload","foreign payload",o,pair_class);return fail(v,"java/lang/IllegalStateException","foreign Pair payload");}
 if(o->data)pair_box_destroy(o->data);
 o->data=NULL;o->data_destroy=NULL;
 if(!o->slots||o->cls->islots<2)return fail(v,"java/lang/InternalError","Pair instance slots");
 /* Pair's two slots are (dvm_ref, dvm_ref) — exactly the 8-byte
    {ref, 1} pattern found in corrupted class name buffers.  If the receiver's
    slots pointer landed in a class name/desc buffer, the two writes below
    would overwrite that class record; report the guest caller instead. */
 if(dvm__name_range_hit(v,o->slots,2*sizeof(union dvm_value)))
  sdk_trace_fprintf(stderr,"PAIR_BAD_SLOTS vm=%p self=%x cls=%s slots=%p islots=%d caller=%s.%s\n",
   (void*)v,self,o->cls->name,(void*)o->slots,o->cls->islots,
   v->cur_method&&v->cur_method->cls?v->cur_method->cls->name:"?",
   v->cur_method?v->cur_method->name:"?");
 struct pair_box *b=calloc(1,sizeof *b);
 if(!b)return fail(v,"java/lang/OutOfMemoryError","Pair payload");
 b->vm=v;b->first=a[0].l;b->second=a[1].l;
 if(b->first)dvm_pin(v,b->first);
 if(b->second)dvm_pin(v,b->second);
 o->data=b;o->data_destroy=pair_box_destroy;
 o->slots[0].l=a[0].l;o->slots[1].l=a[1].l;
 return true;
}
static bool pair_ctor_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 return pair_init_values(v,self,a,n);
}
static bool pair_create_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;
 if(!pair_class)return fail(v,"java/lang/IllegalStateException","Pair class not installed");
 if(n!=2)return fail(v,"java/lang/IllegalArgumentException","Pair.create arguments");
 dvm_ref ref=dvm_new_object(v,pair_class);
 if(!ref)return fail(v,"java/lang/OutOfMemoryError","Pair.create");
 if(!pair_init_values(v,ref,a,n))return true;
 if(dvm_exception(v))return true;
 out->l=ref;return true;
}
/* Objects.equals for one Pair field; an exception raised by a guest equals()
 * propagates instead of being converted into false. */
static bool pair_field_equals(struct dvm *v,dvm_ref x,dvm_ref y,bool *equal){
 if(x==y){*equal=true;return true;}
 if(!x||!y){*equal=false;return true;}
 *equal=java_basics_equal(v,x,y);
 return !dvm_exception(v);
}
static bool pair_equals_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=1)return fail(v,"java/lang/IllegalArgumentException","Pair.equals arguments");
 dvm_ref first=0,second=0;
 if(!pair_slots(v,self,&first,&second))return true;
 dvm_ref other=a[0].l;
 if(other==self){out->i=1;return true;}
 if(!other){out->i=0;return true;}
 struct dvm_object *oo=dvm__obj(v,other);
 if(!is_class(oo,"android/util/Pair")){out->i=0;return true;}
 struct pair_box *ob=pair_box_of(v,other);
 if(!ob)return fail(v,"java/lang/IllegalStateException","Pair.equals foreign payload");
 dvm_ref ofirst=oo->slots?oo->slots[0].l:ob->first;
 dvm_ref osecond=oo->slots?oo->slots[1].l:ob->second;
 bool eq1=false,eq2=false;
 if(!pair_field_equals(v,first,ofirst,&eq1))return true;
 if(!pair_field_equals(v,second,osecond,&eq2))return true;
 out->i=eq1&&eq2;return true;
}
/* Hash of one Pair field.  The runtime's String/boxed implementations answer
 * first; a guest class uses its real hashCode method. */
static bool pair_field_hash(struct dvm *v,dvm_ref ref,int32_t *hash){
 if(!ref){*hash=0;return true;}
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls)return fail(v,"java/lang/IllegalStateException","Pair element handle");
 union dvm_value r={0};
 bool handled=false;
 if(o->kind==DVM_OBJ_STRING)handled=string_runtime_call(v,"java/lang/String","hashCode","()I",ref,NULL,0,&r);
 else handled=java_basics_call(v,o->cls->name,"hashCode","()I",ref,NULL,0,&r);
 if(dvm_exception(v))return true;
 if(!handled){
  struct dvm_method *m=dvm_find_method(v,o->cls,"hashCode","()I");
  if(m&&(m->has_code||m->builtin)){if(!dvm_call(v,m,ref,NULL,0,&r))return true;handled=true;}
 }
 if(dvm_exception(v))return true;
 if(!handled)return fail(v,"java/lang/UnsupportedOperationException","Pair element has no hashCode");
 *hash=r.i;return true;
}
static bool pair_hashcode_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;
 if(n!=0)return fail(v,"java/lang/IllegalArgumentException","Pair.hashCode arguments");
 dvm_ref first=0,second=0;
 if(!pair_slots(v,self,&first,&second))return true;
 int32_t h1=0,h2=0;
 if(!pair_field_hash(v,first,&h1))return true;
 if(!pair_field_hash(v,second,&h2))return true;
 /* AOSP Pair.hashCode(): Objects.hashCode(first) ^ Objects.hashCode(second). */
 out->i=h1^h2;return true;
}
/* String.valueOf for one Pair field. */
static bool pair_field_text(struct dvm *v,dvm_ref ref,const char **text,size_t *len){
 if(!ref){*text="null";*len=4;return true;}
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls)return fail(v,"java/lang/IllegalStateException","Pair element handle");
 if(o->kind==DVM_OBJ_STRING){
  if(!o->utf8)return fail(v,"java/lang/IllegalStateException","Pair String element");
  *text=o->utf8;*len=o->utf8_len;return true;
 }
 union dvm_value r={0};
 bool handled=java_basics_call(v,o->cls->name,"toString","()Ljava/lang/String;",ref,NULL,0,&r);
 if(dvm_exception(v))return true;
 if(!handled){
  struct dvm_method *m=dvm_find_method(v,o->cls,"toString","()Ljava/lang/String;");
  if(m&&(m->has_code||m->builtin)){if(!dvm_call(v,m,ref,NULL,0,&r))return true;handled=true;}
 }
 if(dvm_exception(v))return true;
 if(!handled)return fail(v,"java/lang/UnsupportedOperationException","Pair element has no toString");
 if(!r.l){*text="null";*len=4;return true;}
 struct dvm_object *s=dvm__obj(v,r.l);
 if(!s||s->kind!=DVM_OBJ_STRING||!s->utf8)return fail(v,"java/lang/ClassCastException","Pair element toString did not return a String");
 *text=s->utf8;*len=s->utf8_len;return true;
}
static bool pair_tostring_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;
 if(n!=0)return fail(v,"java/lang/IllegalArgumentException","Pair.toString arguments");
 dvm_ref first=0,second=0;
 if(!pair_slots(v,self,&first,&second))return true;
 const char *t1=NULL,*t2=NULL;size_t l1=0,l2=0;
 if(!pair_field_text(v,first,&t1,&l1))return true;
 if(!pair_field_text(v,second,&t2,&l2))return true;
 /* AOSP format: "Pair{" + first + " " + second + "}" (space separated). */
 size_t need=5+1+l1+1+l2+1;
 char *buf=malloc(need);
 if(!buf)return fail(v,"java/lang/OutOfMemoryError","Pair.toString");
 size_t at=0;
 memcpy(buf+at,"Pair{",5);at+=5;
 memcpy(buf+at,t1,l1);at+=l1;
 buf[at++]=' ';
 memcpy(buf+at,t2,l2);at+=l2;
 buf[at++]='}';buf[at]=0;
 out->l=dvm_new_string(v,buf);
 free(buf);
 if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Pair.toString string");
 return true;
}
static void install_pair_class(struct dvm *v){
 struct dvm_class *c=dvm__register_builtin(v,"Landroid/util/Pair;");
 if(!c)return;
 c->super=dvm_find_class_or_external(v,"java/lang/Object");
 c->access=DEX_ACC_PUBLIC;
 c->ifields=calloc(2,sizeof *c->ifields);
 if(!c->ifields)return;
 c->nifields=2;
 install_field(&c->ifields[0],c,"first","Ljava/lang/Object;",DEX_ACC_PUBLIC|DEX_ACC_FINAL,0);
 install_field(&c->ifields[1],c,"second","Ljava/lang/Object;",DEX_ACC_PUBLIC|DEX_ACC_FINAL,1);
 c->islots=2;
 c->methods=calloc(5,sizeof *c->methods);
 if(!c->methods)return;
 c->nmethods=5;
 install_method(c,0,"<init>","(Ljava/lang/Object;Ljava/lang/Object;)V",DEX_ACC_PUBLIC,pair_ctor_builtin,'V');
 install_method(c,1,"create","(Ljava/lang/Object;Ljava/lang/Object;)Landroid/util/Pair;",DEX_ACC_PUBLIC|DEX_ACC_STATIC,pair_create_builtin,'L');
 install_method(c,2,"equals","(Ljava/lang/Object;)Z",DEX_ACC_PUBLIC,pair_equals_builtin,'Z');
 install_method(c,3,"hashCode","()I",DEX_ACC_PUBLIC,pair_hashcode_builtin,'I');
 install_method(c,4,"toString","()Ljava/lang/String;",DEX_ACC_PUBLIC,pair_tostring_builtin,'L');
 pair_class=c;
}

/* ---- android.content.ContentValues ------------------------------------ */
struct content_values { struct dvm *vm; dvm_ref map; };
static void values_destroy(void *p){
 struct content_values *c=p;
 if(!c)return;
 if(c->vm&&c->map)dvm_unpin(c->vm,c->map);
 free(c);
}
static bool values_map(struct dvm *v,dvm_ref self,dvm_ref *map){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o)return fail(v,"java/lang/NullPointerException","ContentValues receiver");
 if(!values_receiver(o)){payload_diag("values_map_receiver","receiver type",o,values_class);return fail(v,"java/lang/IllegalArgumentException","ContentValues receiver type");}
 if(o->data&&o->data_destroy!=values_destroy){payload_diag("values_map_payload","foreign payload",o,values_class);return fail(v,"java/lang/IllegalStateException","foreign ContentValues payload");}
 struct content_values *c=o->data;
 if(!c||!c->map)return fail(v,"java/lang/IllegalStateException","ContentValues constructor not called");
 *map=c->map;
 return true;
}
static bool values_call_map(struct dvm *v,dvm_ref map,const char *method,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 bool handled=collections_call(v,"java/util/HashMap",method,sig,map,a,n,out);
 if(!handled&&!dvm_exception(v))return fail(v,"java/lang/UnsupportedOperationException","ContentValues backing HashMap method");
 return true;
}
static bool values_init(struct dvm *v,dvm_ref self,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o)return fail(v,"java/lang/NullPointerException","ContentValues constructor receiver");
 if(!values_receiver(o)){payload_diag("values_init_receiver","receiver type",o,values_class);return fail(v,"java/lang/IllegalArgumentException","ContentValues constructor receiver type");}
 if(o->data&&o->data_destroy!=values_destroy){payload_diag("values_init_payload","foreign payload",o,values_class);return fail(v,"java/lang/IllegalStateException","foreign ContentValues payload");}
 if(o->data)values_destroy(o->data);
 o->data=NULL;o->data_destroy=NULL;
 struct dvm_class *map_class=dvm_find_class_or_external(v,"java/util/HashMap");
 if(!map_class)return fail(v,"java/lang/IllegalStateException","HashMap class");
 dvm_ref map=dvm_new_object(v,map_class);
 if(!map)return fail(v,"java/lang/OutOfMemoryError","ContentValues HashMap");
 union dvm_value mout={0};
 if(!strcmp(sig,"()V")){
  if(!values_call_map(v,map,"<init>","()V",NULL,0,&mout))return true;
 }else if(!strcmp(sig,"(I)V")&&n==1){
  if(!values_call_map(v,map,"<init>","(I)V",a,1,&mout))return true;
 }else return fail(v,"java/lang/IllegalArgumentException","ContentValues constructor signature");
 if(dvm_exception(v))return true;
 struct content_values *c=calloc(1,sizeof *c);
 if(!c)return fail(v,"java/lang/OutOfMemoryError","ContentValues payload");
 c->vm=v;c->map=map;dvm_pin(v,map);
 o->data=c;o->data_destroy=values_destroy;
 return true;
}
static bool values_ctor_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 if(!m)return fail(v,"java/lang/IllegalStateException","ContentValues constructor method");
 return values_init(v,self,m->sig,a,n,out);
}
/* AOSP rejects any value outside String/Byte/Short/Integer/Long/Float/Double/
 * Boolean/byte[].  null is not rejected here: it removes the key. */
static bool values_type_allowed(struct dvm *v,dvm_ref value){
 if(!value)return true;
 struct dvm_object *o=dvm__obj(v,value);
 if(!o||!o->cls)return false;
 if(o->kind==DVM_OBJ_STRING)return true;
 if(o->kind==DVM_OBJ_ARRAY){
  /* byte[] is the only accepted array; any other component is rejected. */
  return o->cls->is_array&&o->cls->elem_kind=='B';
 }
 const char *n=o->cls->name;
 return n&&(!strcmp(n,"java/lang/Byte")||!strcmp(n,"java/lang/Short")||
            !strcmp(n,"java/lang/Integer")||!strcmp(n,"java/lang/Long")||
            !strcmp(n,"java/lang/Float")||!strcmp(n,"java/lang/Double")||
            !strcmp(n,"java/lang/Boolean"));
}
static bool values_put_common(struct dvm *v,dvm_ref self,const union dvm_value *a,int n){
 if(n!=2)return fail(v,"java/lang/IllegalArgumentException","ContentValues.put arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 if(a[1].l&&!values_type_allowed(v,a[1].l))return fail(v,"java/lang/IllegalArgumentException","ContentValues value type");
 if(!a[1].l){
  /* AOSP put(key, null) removes the key; the backing map stores no null. */
  union dvm_value args[1]={{.l=a[0].l}},ignored={0};
  return values_call_map(v,map,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",args,1,&ignored);
 }
 union dvm_value ignored={0};
 return values_call_map(v,map,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",a,2,&ignored);
}
static bool values_put_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 return values_put_common(v,self,a,n);
}
static bool values_get_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=1)return fail(v,"java/lang/IllegalArgumentException","ContentValues.get arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 return values_call_map(v,map,"get","(Ljava/lang/Object;)Ljava/lang/Object;",a,1,out);
}
static bool values_get_string_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=1)return fail(v,"java/lang/IllegalArgumentException","ContentValues.getAsString arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 union dvm_value got={0};
 if(!values_call_map(v,map,"get","(Ljava/lang/Object;)Ljava/lang/Object;",a,1,&got))return true;
 if(dvm_exception(v))return true;
 if(!got.l){out->l=0;return true;}
 const char *text=NULL;size_t len=0;
 if(!pair_field_text(v,got.l,&text,&len))return true;
 out->l=dvm_new_string_n(v,text,len);
 if(!out->l)return fail(v,"java/lang/OutOfMemoryError","ContentValues.getAsString");
 return true;
}
/* AOSP getAsX casts the stored Object; a wrong wrapper is a ClassCastException. */
static bool values_get_typed(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,const char *type,union dvm_value *out){
 if(n!=1)return fail(v,"java/lang/IllegalArgumentException","ContentValues.getAs arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 union dvm_value got={0};
 if(!values_call_map(v,map,"get","(Ljava/lang/Object;)Ljava/lang/Object;",a,1,&got))return true;
 if(dvm_exception(v))return true;
 if(got.l){
  struct dvm_object *o=dvm__obj(v,got.l);
  if(!is_class(o,type))return fail(v,"java/lang/ClassCastException","ContentValues.getAs type");
 }
 out->l=got.l;return true;
}
static bool values_get_integer_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return values_get_typed(v,self,a,n,"java/lang/Integer",out);}
static bool values_get_long_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return values_get_typed(v,self,a,n,"java/lang/Long",out);}
static bool values_get_boolean_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return values_get_typed(v,self,a,n,"java/lang/Boolean",out);}
static bool values_get_byte_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return values_get_typed(v,self,a,n,"java/lang/Byte",out);}
static bool values_get_short_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return values_get_typed(v,self,a,n,"java/lang/Short",out);}
static bool values_get_float_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return values_get_typed(v,self,a,n,"java/lang/Float",out);}
static bool values_get_double_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){return values_get_typed(v,self,a,n,"java/lang/Double",out);}
static bool values_contains_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=1)return fail(v,"java/lang/IllegalArgumentException","ContentValues.containsKey arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 return values_call_map(v,map,"containsKey","(Ljava/lang/Object;)Z",a,1,out);
}
static bool values_remove_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=1)return fail(v,"java/lang/IllegalArgumentException","ContentValues.remove arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 return values_call_map(v,map,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",a,1,out);
}
static bool values_size_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;
 if(n!=0)return fail(v,"java/lang/IllegalArgumentException","ContentValues.size arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 return values_call_map(v,map,"size","()I",NULL,0,out);
}
static bool values_clear_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)out;
 if(n!=0)return fail(v,"java/lang/IllegalArgumentException","ContentValues.clear arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 return values_call_map(v,map,"clear","()V",NULL,0,out);
}
static bool values_keyset_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;
 if(n!=0)return fail(v,"java/lang/IllegalArgumentException","ContentValues.keySet arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 return values_call_map(v,map,"keySet","()Ljava/util/Set;",NULL,0,out);
}
static bool values_values_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;
 if(n!=0)return fail(v,"java/lang/IllegalArgumentException","ContentValues.values arguments");
 dvm_ref map=0;
 if(!values_map(v,self,&map))return true;
 return values_call_map(v,map,"values","()Ljava/util/Collection;",NULL,0,out);
}
static void install_values_class(struct dvm *v){
 struct dvm_class *c=dvm__register_builtin(v,"Landroid/content/ContentValues;");
 if(!c)return;
 c->super=dvm_find_class_or_external(v,"java/lang/Object");
 c->access=DEX_ACC_PUBLIC;
 c->methods=calloc(26,sizeof *c->methods);
 if(!c->methods)return;
 c->nmethods=26;
 int i=0;
 install_method(c,i++,"<init>","()V",DEX_ACC_PUBLIC,values_ctor_builtin,'V');
 install_method(c,i++,"<init>","(I)V",DEX_ACC_PUBLIC,values_ctor_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/Object;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/String;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/Byte;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/Short;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/Integer;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/Long;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/Float;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/Double;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;Ljava/lang/Boolean;)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"put","(Ljava/lang/String;[B)V",DEX_ACC_PUBLIC,values_put_builtin,'V');
 install_method(c,i++,"get","(Ljava/lang/String;)Ljava/lang/Object;",DEX_ACC_PUBLIC,values_get_builtin,'L');
 install_method(c,i++,"getAsString","(Ljava/lang/String;)Ljava/lang/String;",DEX_ACC_PUBLIC,values_get_string_builtin,'L');
 install_method(c,i++,"getAsInteger","(Ljava/lang/String;)Ljava/lang/Integer;",DEX_ACC_PUBLIC,values_get_integer_builtin,'L');
 install_method(c,i++,"getAsLong","(Ljava/lang/String;)Ljava/lang/Long;",DEX_ACC_PUBLIC,values_get_long_builtin,'L');
 install_method(c,i++,"getAsBoolean","(Ljava/lang/String;)Ljava/lang/Boolean;",DEX_ACC_PUBLIC,values_get_boolean_builtin,'L');
 install_method(c,i++,"getAsByte","(Ljava/lang/String;)Ljava/lang/Byte;",DEX_ACC_PUBLIC,values_get_byte_builtin,'L');
 install_method(c,i++,"getAsShort","(Ljava/lang/String;)Ljava/lang/Short;",DEX_ACC_PUBLIC,values_get_short_builtin,'L');
 install_method(c,i++,"getAsFloat","(Ljava/lang/String;)Ljava/lang/Float;",DEX_ACC_PUBLIC,values_get_float_builtin,'L');
 install_method(c,i++,"getAsDouble","(Ljava/lang/String;)Ljava/lang/Double;",DEX_ACC_PUBLIC,values_get_double_builtin,'L');
 install_method(c,i++,"containsKey","(Ljava/lang/String;)Z",DEX_ACC_PUBLIC,values_contains_builtin,'Z');
 install_method(c,i++,"remove","(Ljava/lang/String;)V",DEX_ACC_PUBLIC,values_remove_builtin,'V');
 install_method(c,i++,"size","()I",DEX_ACC_PUBLIC,values_size_builtin,'I');
 install_method(c,i++,"keySet","()Ljava/util/Set;",DEX_ACC_PUBLIC,values_keyset_builtin,'L');
 install_method(c,i++,"values","()Ljava/util/Collection;",DEX_ACC_PUBLIC,values_values_builtin,'L');
 install_method(c,i++,"clear","()V",DEX_ACC_PUBLIC,values_clear_builtin,'V');
 values_class=c;
}

/* ---- public interface -------------------------------------------------- */
/* dvm_runtime_install() runs once per VM.  The diagnostic battery VM is
 * destroyed before the main trace VM, and the module had no vm_end call in
 * that path: `active==v` alone is not proof the cached classes belong to the
 * new VM, because malloc can hand the new VM the old address.  The stale
 * pair_class then answered a strcmp on freed memory (72bb: IllegalState) or a
 * garbage islots (72bd: InternalError).  Only pointer values are compared
 * here; the cached class is dereferenced just when it is in this VM's own
 * teardown table. */
static bool class_in_vm(struct dvm *v,struct dvm_class *c){
 if(!v||!c)return false;
 for(int i=0;i<v->nclasses;++i)if(v->classes[i]==c)return true;
 return false;
}
void android_util_vm_start(struct dvm *v){
 if(active==v&&pair_class&&values_class&&
    class_in_vm(v,pair_class)&&class_in_vm(v,values_class)){
  sdk_trace_fprintf(stderr,"ANDROID_UTIL_VM start vm=%p reused=1 pair=%p pair_islots=%d values=%p\n",(void*)v,(void*)pair_class,pair_class->islots,(void*)values_class);
  return;
 }
 sdk_trace_fprintf(stderr,"ANDROID_UTIL_VM start vm=%p reused=0 previous=%p\n",(void*)v,(void*)active);
 active=v;
 install_pair_class(v);
 install_values_class(v);
 sdk_trace_fprintf(stderr,"ANDROID_UTIL_VM installed vm=%p pair=%p pair_islots=%d values=%p\n",(void*)v,(void*)pair_class,pair_class?pair_class->islots:-1,(void*)values_class);
}
void android_util_vm_end(struct dvm *v){
 if(v!=active){
  sdk_trace_fprintf(stderr,"ANDROID_UTIL_VM end vm=%p skipped=1 active=%p\n",(void*)v,(void*)active);
  return;
 }
 sdk_trace_fprintf(stderr,"ANDROID_UTIL_VM end vm=%p cleared=1 pair=%p values=%p\n",(void*)v,(void*)pair_class,(void*)values_class);
 active=NULL;pair_class=NULL;values_class=NULL;
}
/* Direct entry point used by the runtime installation and selftest.  The
 * builtin method table is the primary path inside the DVM; this dispatcher
 * carries the same implementations for callers that reach the module through
 * the external hook instead. */
bool android_util_call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active)return false;
 if(!strcmp(c,"android/util/Pair")){
  if(!strcmp(m,"<init>")&&!strcmp(s,"(Ljava/lang/Object;Ljava/lang/Object;)V"))return pair_init_values(v,self,a,n);
  if(!strcmp(m,"create")&&!strcmp(s,"(Ljava/lang/Object;Ljava/lang/Object;)Landroid/util/Pair;"))return pair_create_builtin(v,self,a,n,out);
  if(!strcmp(m,"equals")&&!strcmp(s,"(Ljava/lang/Object;)Z"))return pair_equals_builtin(v,self,a,n,out);
  if(!strcmp(m,"hashCode")&&!strcmp(s,"()I"))return pair_hashcode_builtin(v,self,a,n,out);
  if(!strcmp(m,"toString")&&!strcmp(s,"()Ljava/lang/String;"))return pair_tostring_builtin(v,self,a,n,out);
  return false;
 }
 if(!strcmp(c,"android/content/ContentValues")){
  if(!strcmp(m,"<init>")&&(!strcmp(s,"()V")||!strcmp(s,"(I)V")))return values_init(v,self,s,a,n,out);
  if(!strcmp(m,"put")&&
     (!strcmp(s,"(Ljava/lang/String;Ljava/lang/Object;)V")||!strcmp(s,"(Ljava/lang/String;Ljava/lang/String;)V")||
      !strcmp(s,"(Ljava/lang/String;Ljava/lang/Byte;)V")||!strcmp(s,"(Ljava/lang/String;Ljava/lang/Short;)V")||
      !strcmp(s,"(Ljava/lang/String;Ljava/lang/Integer;)V")||!strcmp(s,"(Ljava/lang/String;Ljava/lang/Long;)V")||
      !strcmp(s,"(Ljava/lang/String;Ljava/lang/Float;)V")||!strcmp(s,"(Ljava/lang/String;Ljava/lang/Double;)V")||
      !strcmp(s,"(Ljava/lang/String;Ljava/lang/Boolean;)V")||!strcmp(s,"(Ljava/lang/String;[B)V")))return values_put_common(v,self,a,n);
  if(!strcmp(m,"get")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/Object;"))return values_get_builtin(v,self,a,n,out);
  if(!strcmp(m,"getAsString")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/String;"))return values_get_string_builtin(v,self,a,n,out);
  if(!strcmp(m,"getAsInteger")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/Integer;"))return values_get_integer_builtin(v,self,a,n,out);
  if(!strcmp(m,"getAsLong")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/Long;"))return values_get_long_builtin(v,self,a,n,out);
  if(!strcmp(m,"getAsBoolean")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/Boolean;"))return values_get_boolean_builtin(v,self,a,n,out);
  if(!strcmp(m,"getAsByte")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/Byte;"))return values_get_byte_builtin(v,self,a,n,out);
  if(!strcmp(m,"getAsShort")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/Short;"))return values_get_short_builtin(v,self,a,n,out);
  if(!strcmp(m,"getAsFloat")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/Float;"))return values_get_float_builtin(v,self,a,n,out);
  if(!strcmp(m,"getAsDouble")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/Double;"))return values_get_double_builtin(v,self,a,n,out);
  if(!strcmp(m,"containsKey")&&!strcmp(s,"(Ljava/lang/String;)Z"))return values_contains_builtin(v,self,a,n,out);
  if(!strcmp(m,"remove")&&!strcmp(s,"(Ljava/lang/String;)V"))return values_remove_builtin(v,self,a,n,out);
  if(!strcmp(m,"size")&&!strcmp(s,"()I"))return values_size_builtin(v,self,a,n,out);
  if(!strcmp(m,"keySet")&&!strcmp(s,"()Ljava/util/Set;"))return values_keyset_builtin(v,self,a,n,out);
  if(!strcmp(m,"values")&&!strcmp(s,"()Ljava/util/Collection;"))return values_values_builtin(v,self,a,n,out);
  if(!strcmp(m,"clear")&&!strcmp(s,"()V"))return values_clear_builtin(v,self,a,n,out);
  return false;
 }
 return false;
}

/* ---- selftest ---------------------------------------------------------- */
static bool text_is(struct dvm *v,dvm_ref ref,const char *expect){
 const char *text=dvm_string_utf8(v,ref);
 return text&&!strcmp(text,expect);
}
static bool new_object_of(struct dvm *v,struct dvm_class *c,dvm_ref *out){
 if(!c)return false;
 *out=dvm_new_object(v,c);
 return *out!=0;
}
/* Name the pending exception so a device failure identifies the class and
 * not only the boolean aggregate. */
static const char *selftest_exception_name(struct dvm *v){
 struct dvm_object *e=v?dvm__obj(v,dvm_exception(v)):NULL;
 return e&&e->cls&&e->cls->name?e->cls->name:"none";
}
#define CHECK(expr,label) do{++checks;if(!(expr)){sdk_trace_fprintf(stderr,"ANDROID_UTIL_SELFTEST FAIL stage=%s line=%d EXCEPTION=%s\n",label,__LINE__,selftest_exception_name(v));ok=false;goto done;}}while(0)
bool android_util_selftest(struct dvm *v){
 unsigned checks=0;
 if(!v)return false;
 /* A pending exception from an earlier module must not be attributed to
  * Pair.  Record it first, then start from a clean slate; a real failure
  * below still leaves its own exception in place. */
 if(dvm_exception(v)){
  struct dvm_object *pe=dvm__obj(v,dvm_exception(v));
  sdk_trace_fprintf(stderr,"ANDROID_UTIL_SELFTEST PRESTAGE residual_exception=%s\n",pe&&pe->cls&&pe->cls->name?pe->cls->name:"none");
  dvm_clear_exception(v);
 }
 android_util_vm_start(v);
 if(v!=active||!pair_class||!values_class){
  sdk_trace_fprintf(stderr,"ANDROID_UTIL_SELFTEST FAIL stage=install line=%d EXCEPTION=%s\n",__LINE__,selftest_exception_name(v));
  return false;
 }
 bool ok=true;
 /* Pair: constructor, fields, equals, hashCode, toString, create. */
 dvm_ref s1=dvm_new_string(v,"alpha"),s2=dvm_new_string(v,"beta"),s1b=dvm_new_string(v,"alpha");
 dvm_ref s3=dvm_new_string(v,"gamma");
 CHECK(s1&&s2&&s1b&&s3,"pair_strings");
 dvm_ref p1=0,p2=0,p3=0,pnull=0;
 CHECK(new_object_of(v,pair_class,&p1)&&new_object_of(v,pair_class,&p2)&&new_object_of(v,pair_class,&p3)&&new_object_of(v,pair_class,&pnull),"pair_objects");
 union dvm_value a[2]={{.l=s1},{.l=s2}},a2[2]={{.l=s1b},{.l=s2}},ag[2]={{.l=s3},{.l=s2}},an[2]={{0},{0}},r={0};
 CHECK(android_util_call(v,"android/util/Pair","<init>","(Ljava/lang/Object;Ljava/lang/Object;)V",p1,a,2,&r)&&!dvm_exception(v),"pair_ctor_1");
 CHECK(android_util_call(v,"android/util/Pair","<init>","(Ljava/lang/Object;Ljava/lang/Object;)V",p2,a2,2,&r)&&!dvm_exception(v),"pair_ctor_2");
 CHECK(android_util_call(v,"android/util/Pair","<init>","(Ljava/lang/Object;Ljava/lang/Object;)V",p3,ag,2,&r)&&!dvm_exception(v),"pair_ctor_3");
 CHECK(android_util_call(v,"android/util/Pair","<init>","(Ljava/lang/Object;Ljava/lang/Object;)V",pnull,an,2,&r)&&!dvm_exception(v),"pair_ctor_null");
 /* first/second are real instance fields visible to guest iget. */
 struct dvm_object *p1o=dvm__obj(v,p1);
 CHECK(p1o&&p1o->slots&&p1o->cls->islots>=2&&p1o->slots[0].l==s1&&p1o->slots[1].l==s2,"pair_fields_slots");
 union dvm_value eq={0};
 union dvm_value other={.l=p2};
 CHECK(android_util_call(v,"android/util/Pair","equals","(Ljava/lang/Object;)Z",p1,&other,1,&eq)&&eq.i==1&&!dvm_exception(v),"pair_equals_content");
 other.l=p1;
 CHECK(android_util_call(v,"android/util/Pair","equals","(Ljava/lang/Object;)Z",p1,&other,1,&eq)&&eq.i==1&&!dvm_exception(v),"pair_equals_same");
 other.l=p3;
 CHECK(android_util_call(v,"android/util/Pair","equals","(Ljava/lang/Object;)Z",p1,&other,1,&eq)&&eq.i==0&&!dvm_exception(v),"pair_equals_different");
 other.l=s1;
 CHECK(android_util_call(v,"android/util/Pair","equals","(Ljava/lang/Object;)Z",p1,&other,1,&eq)&&eq.i==0&&!dvm_exception(v),"pair_equals_non_pair");
 other.l=0;
 CHECK(android_util_call(v,"android/util/Pair","equals","(Ljava/lang/Object;)Z",p1,&other,1,&eq)&&eq.i==0&&!dvm_exception(v),"pair_equals_null");
 union dvm_value h1={0},h2={0},hn={0},t1={0},tn={0};
 CHECK(android_util_call(v,"android/util/Pair","hashCode","()I",p1,NULL,0,&h1)&&!dvm_exception(v),"pair_hash_p1");
 CHECK(android_util_call(v,"android/util/Pair","hashCode","()I",p2,NULL,0,&h2)&&!dvm_exception(v),"pair_hash_p2");
 CHECK(android_util_call(v,"android/util/Pair","hashCode","()I",pnull,NULL,0,&hn)&&!dvm_exception(v),"pair_hash_null");
 CHECK(h1.i==h2.i&&hn.i==0,"pair_hash_consistent");
 CHECK(android_util_call(v,"android/util/Pair","toString","()Ljava/lang/String;",p1,NULL,0,&t1)&&!dvm_exception(v)&&t1.l,"pair_tostring_p1");
 CHECK(text_is(v,t1.l,"Pair{alpha beta}"),"pair_tostring_text");
 CHECK(android_util_call(v,"android/util/Pair","toString","()Ljava/lang/String;",pnull,NULL,0,&tn)&&!dvm_exception(v)&&tn.l,"pair_tostring_null");
 CHECK(text_is(v,tn.l,"Pair{null null}"),"pair_tostring_null_text");
 union dvm_value created={0};
 CHECK(android_util_call(v,"android/util/Pair","create","(Ljava/lang/Object;Ljava/lang/Object;)Landroid/util/Pair;",0,a,2,&created)&&!dvm_exception(v)&&created.l,"pair_create");
 other.l=created.l;
 CHECK(android_util_call(v,"android/util/Pair","equals","(Ljava/lang/Object;)Z",p1,&other,1,&eq)&&eq.i==1&&!dvm_exception(v),"pair_create_equals");
 /* A non-Pair receiver for a method is not silently accepted. */
 CHECK(android_util_call(v,"android/util/Pair","hashCode","()I",0,NULL,0,&h1)&&dvm_exception(v),"pair_null_receiver");
 dvm_clear_exception(v);
 /* ContentValues: real HashMap storage, null deletes, type validation. */
 dvm_ref cv=0;
 CHECK(new_object_of(v,values_class,&cv),"content_values_object");
 CHECK(android_util_call(v,"android/content/ContentValues","<init>","()V",cv,NULL,0,&r)&&!dvm_exception(v),"content_values_ctor");
 union dvm_value key={.l=dvm_new_string(v,"name")},outv={0};
 CHECK(key.l,"content_values_key");
 CHECK(android_util_call(v,"android/content/ContentValues","put","(Ljava/lang/String;Ljava/lang/String;)V",cv,(union dvm_value[]){{.l=key.l},{.l=s1}},2,&r)&&!dvm_exception(v),"content_values_put_string");
 CHECK(android_util_call(v,"android/content/ContentValues","size","()I",cv,NULL,0,&outv)&&outv.i==1&&!dvm_exception(v),"content_values_size_1");
 CHECK(android_util_call(v,"android/content/ContentValues","containsKey","(Ljava/lang/String;)Z",cv,&key,1,&outv)&&outv.i==1&&!dvm_exception(v),"content_values_contains");
 CHECK(android_util_call(v,"android/content/ContentValues","getAsString","(Ljava/lang/String;)Ljava/lang/String;",cv,&key,1,&outv)&&outv.l&&!dvm_exception(v)&&!strcmp(dvm_string_utf8(v,outv.l),"alpha"),"content_values_get_string");
 /* Null value removes the key. */
 CHECK(android_util_call(v,"android/content/ContentValues","put","(Ljava/lang/String;Ljava/lang/String;)V",cv,(union dvm_value[]){{.l=key.l},{0}},2,&r)&&!dvm_exception(v),"content_values_put_null");
 CHECK(android_util_call(v,"android/content/ContentValues","size","()I",cv,NULL,0,&outv)&&outv.i==0&&!dvm_exception(v),"content_values_size_after_null");
 CHECK(android_util_call(v,"android/content/ContentValues","containsKey","(Ljava/lang/String;)Z",cv,&key,1,&outv)&&outv.i==0&&!dvm_exception(v),"content_values_contains_after_null");
 /* Unsupported Object value is rejected with IllegalArgumentException. */
 struct dvm_class *list_class=dvm_find_class_or_external(v,"java/util/ArrayList");
 dvm_ref list=list_class?dvm_new_object(v,list_class):0;
 CHECK(list,"content_values_bad_list_object");
 union dvm_value bad[2]={{.l=key.l},{.l=list}};
 CHECK(android_util_call(v,"android/content/ContentValues","put","(Ljava/lang/String;Ljava/lang/Object;)V",cv,bad,2,&r)&&dvm_exception(v),"content_values_put_bad_type");
 dvm_clear_exception(v);
 /* Integer/Long/Boolean/byte[] values and typed getters. */
 union dvm_value iv={.i=42},lv={.j=9000000000LL},bv={.i=1};
 union dvm_value intbox={0},longbox={0},boolbox={0},bytes={0};
 CHECK(java_basics_call(v,"java/lang/Integer","valueOf","(I)Ljava/lang/Integer;",0,&iv,1,&intbox)&&intbox.l,"content_values_integer_box");
 CHECK(java_basics_call(v,"java/lang/Long","valueOf","(J)Ljava/lang/Long;",0,&lv,1,&longbox)&&longbox.l,"content_values_long_box");
 CHECK(java_basics_call(v,"java/lang/Boolean","valueOf","(Z)Ljava/lang/Boolean;",0,&bv,1,&boolbox)&&boolbox.l,"content_values_boolean_box");
 bytes.l=dvm_new_array(v,'B',"B",4);
 CHECK(bytes.l,"content_values_byte_array");
 union dvm_value k2={.l=dvm_new_string(v,"count")},k3={.l=dvm_new_string(v,"big")},k4={.l=dvm_new_string(v,"flag")},k5={.l=dvm_new_string(v,"blob")};
 CHECK(k2.l&&k3.l&&k4.l&&k5.l,"content_values_keys");
 CHECK(android_util_call(v,"android/content/ContentValues","put","(Ljava/lang/String;Ljava/lang/Integer;)V",cv,(union dvm_value[]){{.l=k2.l},{.l=intbox.l}},2,&r)&&!dvm_exception(v),"content_values_put_integer");
 CHECK(android_util_call(v,"android/content/ContentValues","put","(Ljava/lang/String;Ljava/lang/Long;)V",cv,(union dvm_value[]){{.l=k3.l},{.l=longbox.l}},2,&r)&&!dvm_exception(v),"content_values_put_long");
 CHECK(android_util_call(v,"android/content/ContentValues","put","(Ljava/lang/String;Ljava/lang/Boolean;)V",cv,(union dvm_value[]){{.l=k4.l},{.l=boolbox.l}},2,&r)&&!dvm_exception(v),"content_values_put_boolean");
 CHECK(android_util_call(v,"android/content/ContentValues","put","(Ljava/lang/String;[B)V",cv,(union dvm_value[]){{.l=k5.l},{.l=bytes.l}},2,&r)&&!dvm_exception(v),"content_values_put_bytes");
 CHECK(android_util_call(v,"android/content/ContentValues","getAsInteger","(Ljava/lang/String;)Ljava/lang/Integer;",cv,&k2,1,&outv)&&outv.l==intbox.l&&!dvm_exception(v),"content_values_get_integer");
 CHECK(android_util_call(v,"android/content/ContentValues","getAsLong","(Ljava/lang/String;)Ljava/lang/Long;",cv,&k3,1,&outv)&&outv.l==longbox.l&&!dvm_exception(v),"content_values_get_long");
 CHECK(android_util_call(v,"android/content/ContentValues","getAsBoolean","(Ljava/lang/String;)Ljava/lang/Boolean;",cv,&k4,1,&outv)&&outv.l==boolbox.l&&!dvm_exception(v),"content_values_get_boolean");
 CHECK(android_util_call(v,"android/content/ContentValues","getAsString","(Ljava/lang/String;)Ljava/lang/String;",cv,&k2,1,&outv)&&outv.l&&!dvm_exception(v)&&!strcmp(dvm_string_utf8(v,outv.l),"42"),"content_values_get_string_integer");
 CHECK(android_util_call(v,"android/content/ContentValues","getAsLong","(Ljava/lang/String;)Ljava/lang/Long;",cv,&k2,1,&outv)&&dvm_exception(v),"content_values_get_long_ccr");
 dvm_clear_exception(v);
 /* keySet()/values() are the real backed views of the backing map. */
 CHECK(android_util_call(v,"android/content/ContentValues","keySet","()Ljava/util/Set;",cv,NULL,0,&outv)&&outv.l&&!dvm_exception(v),"content_values_keyset");
 union dvm_value ksv={0};
 CHECK(collections_call(v,"java/util/Set","size","()I",outv.l,NULL,0,&ksv)&&ksv.i==4&&!dvm_exception(v),"content_values_keyset_size");
 CHECK(android_util_call(v,"android/content/ContentValues","values","()Ljava/util/Collection;",cv,NULL,0,&outv)&&outv.l&&!dvm_exception(v),"content_values_values");
 CHECK(collections_call(v,"java/util/Collection","size","()I",outv.l,NULL,0,&ksv)&&ksv.i==4&&!dvm_exception(v),"content_values_values_size");
 CHECK(android_util_call(v,"android/content/ContentValues","remove","(Ljava/lang/String;)V",cv,&k2,1,&r)&&!dvm_exception(v),"content_values_remove");
 CHECK(android_util_call(v,"android/content/ContentValues","size","()I",cv,NULL,0,&outv)&&outv.i==3&&!dvm_exception(v),"content_values_size_after_remove");
 CHECK(android_util_call(v,"android/content/ContentValues","clear","()V",cv,NULL,0,&r)&&!dvm_exception(v),"content_values_clear");
 CHECK(android_util_call(v,"android/content/ContentValues","size","()I",cv,NULL,0,&outv)&&outv.i==0&&!dvm_exception(v),"content_values_size_after_clear");
 CHECK(android_util_call(v,"android/content/ContentValues","size","()I",0,NULL,0,&outv)&&dvm_exception(v),"content_values_null_receiver");
 dvm_clear_exception(v);
 /* The same implementations are reachable through the DVM builtin method
  * table used by guest invokes, not only through the external hook above. */
 struct dvm_method *bm=dvm_find_method(v,pair_class,"toString","()Ljava/lang/String;");
 CHECK(bm,"pair_builtin_method");
 union dvm_value bt={0};
 CHECK(dvm_call(v,bm,p1,NULL,0,&bt)&&!dvm_exception(v)&&text_is(v,bt.l,"Pair{alpha beta}"),"pair_builtin_dispatch_tostring");
 struct dvm_method *cm=dvm_find_method(v,values_class,"<init>","()V");
 CHECK(cm,"content_values_builtin_method");
 dvm_ref cv2=dvm_new_object(v,values_class);
 union dvm_value cr={0};
 CHECK(cv2&&dvm_call(v,cm,cv2,NULL,0,&cr)&&!dvm_exception(v),"content_values_builtin_ctor");
done:
 if(!ok)dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"ANDROID_UTIL_SELFTEST %s pair=1 content_values=1 checks=%u\n",ok?"PASS":"FAIL",checks);
 return ok;
}
#undef CHECK
