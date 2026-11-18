/* In-memory android.os.Bundle key/value semantics.
 *
 * The PackageManager metadata Bundle is a separate fixed view owned by
 * package_metadata.c.  This module only claims objects whose data_destroy is
 * bundle_destroy and whose payload carries BUNDLE_MAGIC, so a metadata Bundle
 * (data_destroy == package_metadata's bundle_free/box_free) is never touched:
 * bundle_runtime_call returns false for it and the dispatcher keeps looking.
 *
 * Values are stored by reference for every object/array type.  No Parcel
 * serialisation or copying of array contents. An operation-local native scope
 * borrows touched bodies/payloads across reentrant calls. Payload visitors pin
 * their owner only for the visitation interval. Production moving stays off.
 * Every entry point runs under the interpreter GIL and performs no locking.
 *
 * Object boxing in get(String)Object is delegated to java_basics_call.  It
 * implements Integer/Long/Boolean valueOf only.  Byte/Short/Character/Float/
 * Double boxing therefore returns null instead of fabricating a boxed object;
 * the typed getters still return their primitive values.  Type mismatches on
 * typed getters return the Android default value (or null) instead of
 * pretending success. */
#include "bundle_runtime.h"
#include "java_basics.h"
#include "gc_operation.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE*,const char*,...);
/* set_runtime.c owns HashSet.  Declared like list_views_call in set_runtime.c
 * so this module does not add a header dependency on that component. */
extern bool set_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);

#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
#define BUNDLE_MAGIC 0x424e444cu /* "BNDL" */

enum bundle_type {
 BT_STRING=1,BT_INT,BT_LONG,BT_BOOLEAN,BT_DOUBLE,BT_FLOAT,BT_SHORT,BT_BYTE,BT_CHAR,
 BT_STRING_ARRAY,BT_INT_ARRAY,BT_BYTE_ARRAY,BT_CHAR_ARRAY,
 BT_CHAR_SEQUENCE,BT_CHAR_SEQUENCE_ARRAY,BT_BUNDLE,
 BT_PARCELABLE,BT_SERIALIZABLE,BT_ARRAY_LIST
};

typedef union bundle_value {dvm_ref l;int32_t i;int64_t j;double d;float f;} bundle_value;
typedef struct bundle_entry {char *key;uint32_t key_len;int type;bundle_value value;} bundle_entry;
typedef struct bundle_data {uint32_t magic;bundle_entry *entries;size_t count,capacity;} bundle_data;

static void bundle_destroy(void *p){
 bundle_data *b=p;if(!b)return;
 for(size_t i=0;i<b->count;i++)free(b->entries[i].key);
 free(b->entries);
 b->magic=0;
 free(b);
}

static bool bundle_ref_type(int type){
 switch(type){
  case BT_STRING:case BT_STRING_ARRAY:case BT_INT_ARRAY:case BT_BYTE_ARRAY:case BT_CHAR_ARRAY:
  case BT_CHAR_SEQUENCE:case BT_CHAR_SEQUENCE_ARRAY:case BT_BUNDLE:
  case BT_PARCELABLE:case BT_SERIALIZABLE:case BT_ARRAY_LIST:return true;
  default:return false;
 }
}

static bool bundle_runtime_trace_keys_operation(struct dvm *v,dvm_ref ref,int fd){
 const dvm_ref gc_ref_o_0=(ref); if(!gc_operation_admit(v,gc_ref_o_0))return 0;
 bundle_data *b=dvm__obj(v,gc_ref_o_0)&&dvm__obj(v,gc_ref_o_0)->data_destroy==bundle_destroy?dvm__obj(v,gc_ref_o_0)->data:NULL;
 if(!b||b->magic!=BUNDLE_MAGIC){dprintf(fd,"BUNDLE_KEYS ref=%u unavailable=1\n",ref);return false;}
 dprintf(fd,"BUNDLE_KEYS ref=%u count=%zu\n",ref,b->count);
 for(size_t i=0;i<b->count;i++){
  bundle_entry *e=&b->entries[i];char key[256];size_t n=e->key_len<sizeof key-1?e->key_len:sizeof key-1;
  for(size_t j=0;j<n;j++)key[j]=(unsigned char)e->key[j]<32?'?':e->key[j];key[n]=0;
  if(e->type==BT_STRING||e->type==BT_CHAR_SEQUENCE){
   const char *text=dvm_string_utf8(v,e->value.l);size_t len=text?dvm_string_utf8_length(v,e->value.l):0;
   bool secret=strstr(key,"token")||strstr(key,"access_key");
   char prefix[5]={0};if(secret&&text)for(size_t k=0;k<len&&k<4;k++)prefix[k]=(unsigned char)text[k]>=32&&text[k]!='"'&&text[k]!='\\'?text[k]:'?';
   dprintf(fd,"BUNDLE_KEY key=\"%s\" type=%d string_length=%zu null=%d%s%s\n",key,e->type,len,!e->value.l,secret?" prefix4=":"",secret?prefix:"");
  }else if(bundle_ref_type(e->type))dprintf(fd,"BUNDLE_KEY key=\"%s\" type=%d nonnull=%d\n",key,e->type,e->value.l!=0);
  else dprintf(fd,"BUNDLE_KEY key=\"%s\" type=%d primitive=1\n",key,e->type);
 }
 return true;
}
bool bundle_runtime_trace_keys(struct dvm *v,dvm_ref ref,int fd){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=bundle_runtime_trace_keys_operation(v,ref,fd);
 return result;
}


static bundle_data *bundle_attach(struct dvm *v,dvm_ref ref,size_t capacity){
 if(!gc_operation_admit(v,ref))return 0;
 if(capacity>SIZE_MAX/sizeof(bundle_entry)){dvm__throw(v,"java/lang/OutOfMemoryError","Bundle capacity");return NULL;}
 bundle_data *b=calloc(1,sizeof *b);
 if(!b){dvm__throw(v,"java/lang/OutOfMemoryError","Bundle state");return NULL;}
 b->magic=BUNDLE_MAGIC;
 if(capacity){
  b->entries=calloc(capacity,sizeof *b->entries);
  if(!b->entries){free(b);dvm__throw(v,"java/lang/OutOfMemoryError","Bundle entries");return NULL;}
  b->capacity=capacity;
 }
 const dvm_ref gc_ref_o_1=(ref); if(!gc_operation_admit(v,gc_ref_o_1))return 0;
 if(!dvm__obj(v,gc_ref_o_1)){bundle_destroy(b);dvm__throw(v,"java/lang/IllegalStateException","Bundle allocation");return NULL;}
 dvm__obj(v,gc_ref_o_1)->data=b;dvm__obj(v,gc_ref_o_1)->data_destroy=bundle_destroy;return b;
}

static bool bundle_reserve(struct dvm *v,bundle_data *b,size_t need){
 if(need<=b->capacity)return true;
 if(need>SIZE_MAX/sizeof(bundle_entry)){dvm__throw(v,"java/lang/OutOfMemoryError","Bundle entries");return false;}
 size_t cap=b->capacity?b->capacity:8;
 while(cap<need&&cap<=SIZE_MAX/2)cap*=2;
 if(cap<need)cap=need;
 bundle_entry *p=realloc(b->entries,cap*sizeof *p);
 if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","Bundle entries");return false;}
 b->entries=p;b->capacity=cap;return true;
}

static bundle_entry *bundle_find(bundle_data *b,const char *key,uint32_t len){
 for(size_t i=0;i<b->count;i++)
  if(b->entries[i].key_len==len&&!memcmp(b->entries[i].key,key,len))return &b->entries[i];
 return NULL;
}

static bool bundle_put(struct dvm *v,bundle_data *b,const char *key,uint32_t len,int type,bundle_value value){
 bundle_entry *e=bundle_find(b,key,len);
 if(e){e->type=type;e->value=value;return true;}
 if(!bundle_reserve(v,b,b->count+1))return false;
 char *copy=malloc((size_t)len+1);
 if(!copy){dvm__throw(v,"java/lang/OutOfMemoryError","Bundle key");return false;}
 if(len)memcpy(copy,key,len);
 copy[len]='\0';
 e=&b->entries[b->count++];
 e->key=copy;e->key_len=len;e->type=type;e->value=value;
 return true;
}

static void bundle_remove_at(bundle_data *b,size_t i){
 free(b->entries[i].key);
 memmove(&b->entries[i],&b->entries[i+1],(b->count-i-1)*sizeof *b->entries);
 --b->count;
}

static void bundle_clear_entries(bundle_data *b){
 for(size_t i=0;i<b->count;i++)free(b->entries[i].key);
 b->count=0;
}

static bool bundle_clone_entries(struct dvm *v,bundle_data *dst,bundle_data *src){
 if(!bundle_reserve(v,dst,src->count))return false;
 for(size_t i=0;i<src->count;i++){
  bundle_entry *e=&src->entries[i];
  if(!bundle_put(v,dst,e->key,e->key_len,e->type,e->value))return false;
 }
 return true;
}

/* The key is a Java String; UTF-8 bytes may contain NUL, so the length from
 * the VM is kept and comparison is length + memcmp, never strcmp. */
static bool bundle_key_arg(struct dvm *v,const union dvm_value *a,const char **key,uint32_t *len){
 if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Bundle key");return false;}
 const char *k=dvm_string_utf8(v,a[0].l);
 if(!k){dvm__throw(v,"java/lang/ClassCastException","Bundle key is not java.lang.String");return false;}
 *key=k;*len=dvm_string_utf8_length(v,a[0].l);return true;
}

static bool bundle_is_string_obj(struct dvm *v,dvm_ref ref){
 const dvm_ref gc_ref_o_2=(ref); if(!gc_operation_admit(v,gc_ref_o_2))return 0;
 return dvm__obj(v,gc_ref_o_2)&&dvm__obj(v,gc_ref_o_2)->kind==DVM_OBJ_STRING;
}

static bool bundle_prim_array_is(struct dvm *v,dvm_ref ref,char kind){
 const dvm_ref gc_ref_o_3=(ref); if(!gc_operation_admit(v,gc_ref_o_3))return 0;
 return dvm__obj(v,gc_ref_o_3)&&dvm__obj(v,gc_ref_o_3)->kind==DVM_OBJ_ARRAY&&dvm__obj(v,gc_ref_o_3)->elem_kind==kind;
}

static bool bundle_object_array_is(struct dvm *v,dvm_ref ref,const char *elem_a,const char *elem_b){
 const dvm_ref gc_ref_o_4=(ref); if(!gc_operation_admit(v,gc_ref_o_4))return 0;
 if(!dvm__obj(v,gc_ref_o_4)||dvm__obj(v,gc_ref_o_4)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc_ref_o_4)->elem_kind!='L')return false;
 const char *name=dvm__obj(v,gc_ref_o_4)->cls&&dvm__obj(v,gc_ref_o_4)->cls->elem?dvm__obj(v,gc_ref_o_4)->cls->elem->name:NULL;
 if(!name)return true; /* unknown component metadata must not block the app */
 return !strcmp(name,elem_a)||(elem_b&&!strcmp(name,elem_b));
}

static bool bundle_is_bundle_obj(struct dvm *v,dvm_ref ref){
 const dvm_ref gc_ref_o_5=(ref); if(!gc_operation_admit(v,gc_ref_o_5))return 0;
 if(!dvm__obj(v,gc_ref_o_5)||!dvm__obj(v,gc_ref_o_5)->cls)return false;
 for(struct dvm_class *k=dvm__obj(v,gc_ref_o_5)->cls;k;k=k->super)
  if(k->name&&!strcmp(k->name,"android/os/Bundle"))return true;
 return false;
}

/* Integer/Long/Boolean are provided by java_basics.  The remaining boxes are
 * attempted too so a future java_basics implementation starts working without
 * changes here, but they currently leave out->l == 0 (no fabricated object). */
static void bundle_box(struct dvm *v,const bundle_entry *e,union dvm_value *out){
 const char *cls=NULL,*sig=NULL;union dvm_value arg={0},boxed={0};
 switch(e->type){
  case BT_INT:cls="java/lang/Integer";sig="(I)Ljava/lang/Integer;";arg.i=e->value.i;break;
  case BT_LONG:cls="java/lang/Long";sig="(J)Ljava/lang/Long;";arg.j=e->value.j;break;
  case BT_BOOLEAN:cls="java/lang/Boolean";sig="(Z)Ljava/lang/Boolean;";arg.i=e->value.i?1:0;break;
  case BT_BYTE:cls="java/lang/Byte";sig="(B)Ljava/lang/Byte;";arg.i=(int8_t)e->value.i;break;
  case BT_SHORT:cls="java/lang/Short";sig="(S)Ljava/lang/Short;";arg.i=(int16_t)e->value.i;break;
  case BT_CHAR:cls="java/lang/Character";sig="(C)Ljava/lang/Character;";arg.i=(uint16_t)e->value.i;break;
  case BT_FLOAT:cls="java/lang/Float";sig="(F)Ljava/lang/Float;";arg.f=e->value.f;break;
  case BT_DOUBLE:cls="java/lang/Double";sig="(D)Ljava/lang/Double;";arg.d=e->value.d;break;
  default:out->l=0;return;
 }
 if(!java_basics_call(v,cls,"valueOf",sig,0,&arg,1,&boxed)){out->l=0;return;}
 out->l=boxed.l;
}

static bool bundle_get_scalar(struct dvm *v,bundle_data *b,const union dvm_value *a,int type,const union dvm_value *fallback,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 memset(out,0,sizeof *out);
 if(!e||e->type!=type){if(fallback)*out=*fallback;return true;}
 switch(type){
  case BT_INT:out->i=e->value.i;break;
  case BT_LONG:out->j=e->value.j;break;
  case BT_BOOLEAN:out->i=e->value.i?1:0;break;
  case BT_BYTE:out->i=(int8_t)e->value.i;break;
  case BT_SHORT:out->i=(int16_t)e->value.i;break;
  case BT_CHAR:out->i=(uint16_t)e->value.i;break;
  case BT_FLOAT:out->f=e->value.f;break;
  case BT_DOUBLE:out->d=e->value.d;break;
 }
 return true;
}

static bool bundle_get_object(struct dvm *v,bundle_data *b,const union dvm_value *a,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 memset(out,0,sizeof *out);
 if(!e)return true;
 if(bundle_ref_type(e->type)){out->l=e->value.l;return true;}
 bundle_box(v,e,out);
 return true;
}

static bool bundle_get_string(struct dvm *v,bundle_data *b,const union dvm_value *a,const union dvm_value *fallback,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 if(e&&bundle_ref_type(e->type)&&e->value.l&&bundle_is_string_obj(v,e->value.l)){out->l=e->value.l;return true;}
 if(fallback)*out=*fallback;else out->l=0;
 return true;
}

static bool bundle_get_char_sequence(struct dvm *v,bundle_data *b,const union dvm_value *a,const union dvm_value *fallback,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 if(e&&bundle_ref_type(e->type)&&e->value.l&&
    (e->type==BT_STRING||e->type==BT_CHAR_SEQUENCE||bundle_is_string_obj(v,e->value.l))){
  out->l=e->value.l;return true;
 }
 if(fallback)*out=*fallback;else out->l=0;
 return true;
}

static bool bundle_get_string_array(struct dvm *v,bundle_data *b,const union dvm_value *a,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 if(e&&(e->type==BT_STRING_ARRAY||e->type==BT_CHAR_SEQUENCE_ARRAY)&&
    bundle_object_array_is(v,e->value.l,"java/lang/String",NULL))out->l=e->value.l;
 else out->l=0;
 return true;
}

static bool bundle_get_char_sequence_array(struct dvm *v,bundle_data *b,const union dvm_value *a,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 if(e&&(e->type==BT_STRING_ARRAY||e->type==BT_CHAR_SEQUENCE_ARRAY)&&
    bundle_object_array_is(v,e->value.l,"java/lang/String","java/lang/CharSequence"))out->l=e->value.l;
 else out->l=0;
 return true;
}

static bool bundle_get_prim_array(struct dvm *v,bundle_data *b,const union dvm_value *a,int type,char kind,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 if(e&&e->type==type&&bundle_prim_array_is(v,e->value.l,kind))out->l=e->value.l;
 else out->l=0;
 return true;
}

static bool bundle_get_bundle(struct dvm *v,bundle_data *b,const union dvm_value *a,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 if(e&&bundle_ref_type(e->type)&&e->value.l&&bundle_is_bundle_obj(v,e->value.l))out->l=e->value.l;
 else out->l=0;
 return true;
}

static bool bundle_get_parcelable(struct dvm *v,bundle_data *b,const union dvm_value *a,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 out->l=e&&e->type==BT_PARCELABLE?e->value.l:0;
 return true;
}

static bool bundle_get_serializable(struct dvm *v,bundle_data *b,const union dvm_value *a,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 if(e&&e->type==BT_SERIALIZABLE)out->l=e->value.l;
 else if(e&&bundle_ref_type(e->type)&&e->value.l&&bundle_is_string_obj(v,e->value.l))out->l=e->value.l; /* String implements Serializable */
 else out->l=0;
 return true;
}

static bool bundle_get_array_list(struct dvm *v,bundle_data *b,const union dvm_value *a,union dvm_value *out){
 const char *key;uint32_t len;
 if(!bundle_key_arg(v,a,&key,&len))return true;
 bundle_entry *e=bundle_find(b,key,len);
 out->l=e&&e->type==BT_ARRAY_LIST?e->value.l:0;
 return true;
}

/* HashSet construction and add are implemented by set_runtime.c.  If that
 * component ever stops answering, fail closed with UnsupportedOperationException
 * rather than returning an object of the wrong type. */
static bool bundle_key_set(struct dvm *v,bundle_data *b,union dvm_value *out){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/HashSet");
 if(!cls){dvm__throw(v,"java/lang/UnsupportedOperationException","keySet requires java/util/HashSet");return true;}
 dvm_ref set=dvm_new_object(v,cls);
 if(!set){dvm__throw(v,"java/lang/OutOfMemoryError","Bundle.keySet");return true;}
 union dvm_value ignored={0};
 if(!set_runtime_call(v,"java/util/HashSet","<init>","()V",set,NULL,0,&ignored)){
  dvm__throw(v,"java/lang/UnsupportedOperationException","keySet requires HashSet construction");return true;
 }
 if(dvm_exception(v))return true;
 for(size_t i=0;i<b->count;i++){
  dvm_ref key=dvm_new_string_n(v,b->entries[i].key,b->entries[i].key_len);
  if(!key){dvm__throw(v,"java/lang/OutOfMemoryError","Bundle.keySet");return true;}
  union dvm_value arg={.l=key};
  if(!set_runtime_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",set,&arg,1,&ignored)){
   dvm__throw(v,"java/lang/UnsupportedOperationException","keySet requires Set.add");return true;
  }
  if(dvm_exception(v))return true;
 }
 out->l=set;return true;
}

static dvm_ref bundle_runtime_new_operation(struct dvm *v){
 struct dvm_class *cls=dvm_find_class_or_external(v,"android/os/Bundle");
 if(!cls){dvm__throw(v,"java/lang/NoClassDefFoundError","android/os/Bundle");return 0;}
 dvm_ref ref=dvm_new_object(v,cls);
 if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Bundle");return 0;}
 const dvm_ref gc_ref_o_6=(ref); if(!gc_operation_admit(v,gc_ref_o_6))return 0;
 if(!dvm__obj(v,gc_ref_o_6)){dvm__throw(v,"java/lang/IllegalStateException","Bundle allocation");return 0;}
 if(!bundle_attach(v,ref,0))return 0;
 return ref;
}
dvm_ref bundle_runtime_new(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return 0;
 dvm_ref result=bundle_runtime_new_operation(v);
 gc_operation_result(&operation,"()Ljava/lang/Object;",result);
 return result;
}


static bool bundle_runtime_call_operation(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;
 if(strcmp(c,"android/os/Bundle"))return false;
 const dvm_ref gc_ref_o_7=(self); if(!gc_operation_admit(v,gc_ref_o_7))return true;
 if(!dvm__obj(v,gc_ref_o_7)||!dvm__obj(v,gc_ref_o_7)->cls||!dvm__obj(v,gc_ref_o_7)->cls->name||strcmp(dvm__obj(v,gc_ref_o_7)->cls->name,"android/os/Bundle")){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Bundle receiver");return true;}
  return false;
 }
 bool init_empty=IS("<init>","()V");
 bool init_copy=IS("<init>","(Landroid/os/Bundle;)V");
 bool init_capacity=IS("<init>","(I)V");
 if(init_empty||init_copy||init_capacity){
  if(dvm__obj(v,gc_ref_o_7)->data||dvm__obj(v,gc_ref_o_7)->data_destroy){dvm__throw(v,"java/lang/IllegalStateException","Bundle already initialized");return true;}
  if(init_capacity){
   if(a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative Bundle capacity");return true;}
   if(!bundle_attach(v,self,(size_t)a[0].i))return true;
   return true;
  }
  bundle_data *dst=bundle_attach(v,self,0);
  if(!dst)return true;
  if(init_copy&&a[0].l){
   if(a[0].l==self){dvm__throw(v,"java/lang/IllegalStateException","Bundle copy from itself");return true;}
   if(!bundle_is_bundle_obj(v,a[0].l)){dvm__throw(v,"java/lang/ClassCastException","Bundle copy source");return true;}
   const dvm_ref gc_ref_src_obj_8=(a[0].l); if(!gc_operation_admit(v,gc_ref_src_obj_8))return true;
   if(!dvm__obj(v,gc_ref_src_obj_8)||!dvm__obj(v,gc_ref_src_obj_8)->data_destroy){dvm__throw(v,"java/lang/IllegalStateException","source Bundle constructor not called");return true;}
   if(dvm__obj(v,gc_ref_src_obj_8)->data_destroy!=bundle_destroy||!dvm__obj(v,gc_ref_src_obj_8)->data||((bundle_data*)dvm__obj(v,gc_ref_src_obj_8)->data)->magic!=BUNDLE_MAGIC){
    /* A PackageManager metadata Bundle has no enumerable in-memory map.  Do
     * not invent an empty copy for it. */
    dvm__throw(v,"java/lang/UnsupportedOperationException","Bundle copy source is not an in-memory Bundle");return true;
   }
   if(!bundle_clone_entries(v,dst,dvm__obj(v,gc_ref_src_obj_8)->data))return true;
  }
  return true;
 }
 if(dvm__obj(v,gc_ref_o_7)->data_destroy!=bundle_destroy){
  /* package_metadata.c owns the fixed metadata view; leave it untouched. */
  if(!dvm__obj(v,gc_ref_o_7)->data_destroy){dvm__throw(v,"java/lang/IllegalStateException","Bundle constructor not called");return true;}
  return false;
 }
 bundle_data *b=dvm__obj(v,gc_ref_o_7)->data;
 if(!b||b->magic!=BUNDLE_MAGIC)return false;
 if(IS("putAll","(Landroid/os/Bundle;)V")&&n==1){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Bundle putAll source");return true;}
  if(a[0].l==self)return true;
  const dvm_ref gc_ref_src_9=(a[0].l); if(!gc_operation_admit(v,gc_ref_src_9))return true;
  if(!bundle_is_bundle_obj(v,a[0].l)){dvm__throw(v,"java/lang/ClassCastException","Bundle putAll source");return true;}
  if(!dvm__obj(v,gc_ref_src_9)||dvm__obj(v,gc_ref_src_9)->data_destroy!=bundle_destroy||!dvm__obj(v,gc_ref_src_9)->data||((bundle_data*)dvm__obj(v,gc_ref_src_9)->data)->magic!=BUNDLE_MAGIC){dvm__throw(v,"java/lang/UnsupportedOperationException","Bundle putAll source is not an in-memory Bundle");return true;}
  (void)bundle_clone_entries(v,b,dvm__obj(v,gc_ref_src_9)->data);return true;
 }

 if(IS("size","()I")){out->i=b->count>(size_t)INT_MAX?INT_MAX:(int)b->count;return true;}
 if(IS("isEmpty","()Z")){out->i=b->count==0;return true;}
 if(IS("containsKey","(Ljava/lang/String;)Z")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  out->i=bundle_find(b,key,len)!=NULL;return true;
 }
 if(IS("get","(Ljava/lang/String;)Ljava/lang/Object;"))return bundle_get_object(v,b,a,out);
 if(IS("remove","(Ljava/lang/String;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_entry *e=bundle_find(b,key,len);if(e)bundle_remove_at(b,(size_t)(e-b->entries));
  return true;
 }
 if(IS("clear","()V")){bundle_clear_entries(b);return true;}
 if(IS("keySet","()Ljava/util/Set;"))return bundle_key_set(v,b,out);

 if(IS("getString","(Ljava/lang/String;)Ljava/lang/String;"))return bundle_get_string(v,b,a,NULL,out);
 if(IS("getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"))return bundle_get_string(v,b,a,&a[1],out);
 if(IS("getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;"))return bundle_get_char_sequence(v,b,a,NULL,out);
 if(IS("getCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;"))return bundle_get_char_sequence(v,b,a,&a[1],out);

 if(IS("getInt","(Ljava/lang/String;)I"))return bundle_get_scalar(v,b,a,BT_INT,NULL,out);
 if(IS("getInt","(Ljava/lang/String;I)I"))return bundle_get_scalar(v,b,a,BT_INT,&a[1],out);
 if(IS("getLong","(Ljava/lang/String;)J"))return bundle_get_scalar(v,b,a,BT_LONG,NULL,out);
 if(IS("getLong","(Ljava/lang/String;J)J"))return bundle_get_scalar(v,b,a,BT_LONG,&a[1],out);
 if(IS("getBoolean","(Ljava/lang/String;)Z"))return bundle_get_scalar(v,b,a,BT_BOOLEAN,NULL,out);
 if(IS("getBoolean","(Ljava/lang/String;Z)Z"))return bundle_get_scalar(v,b,a,BT_BOOLEAN,&a[1],out);
 if(IS("getDouble","(Ljava/lang/String;)D"))return bundle_get_scalar(v,b,a,BT_DOUBLE,NULL,out);
 if(IS("getDouble","(Ljava/lang/String;D)D"))return bundle_get_scalar(v,b,a,BT_DOUBLE,&a[1],out);
 if(IS("getFloat","(Ljava/lang/String;)F"))return bundle_get_scalar(v,b,a,BT_FLOAT,NULL,out);
 if(IS("getFloat","(Ljava/lang/String;F)F"))return bundle_get_scalar(v,b,a,BT_FLOAT,&a[1],out);
 if(IS("getShort","(Ljava/lang/String;)S"))return bundle_get_scalar(v,b,a,BT_SHORT,NULL,out);
 if(IS("getShort","(Ljava/lang/String;S)S"))return bundle_get_scalar(v,b,a,BT_SHORT,&a[1],out);
 if(IS("getByte","(Ljava/lang/String;)B"))return bundle_get_scalar(v,b,a,BT_BYTE,NULL,out);
 if(IS("getByte","(Ljava/lang/String;B)B"))return bundle_get_scalar(v,b,a,BT_BYTE,&a[1],out);
 if(IS("getChar","(Ljava/lang/String;)C"))return bundle_get_scalar(v,b,a,BT_CHAR,NULL,out);
 if(IS("getChar","(Ljava/lang/String;C)C"))return bundle_get_scalar(v,b,a,BT_CHAR,&a[1],out);

 if(IS("getStringArray","(Ljava/lang/String;)[Ljava/lang/String;"))return bundle_get_string_array(v,b,a,out);
 if(IS("getIntArray","(Ljava/lang/String;)[I"))return bundle_get_prim_array(v,b,a,BT_INT_ARRAY,'I',out);
 if(IS("getByteArray","(Ljava/lang/String;)[B"))return bundle_get_prim_array(v,b,a,BT_BYTE_ARRAY,'B',out);
 if(IS("getCharArray","(Ljava/lang/String;)[C"))return bundle_get_prim_array(v,b,a,BT_CHAR_ARRAY,'C',out);
 if(IS("getCharSequenceArray","(Ljava/lang/String;)[Ljava/lang/CharSequence;"))return bundle_get_char_sequence_array(v,b,a,out);
 if(IS("getBundle","(Ljava/lang/String;)Landroid/os/Bundle;"))return bundle_get_bundle(v,b,a,out);
 if(IS("getParcelable","(Ljava/lang/String;)Landroid/os/Parcelable;"))return bundle_get_parcelable(v,b,a,out);
 if(IS("getSerializable","(Ljava/lang/String;)Ljava/io/Serializable;"))return bundle_get_serializable(v,b,a,out);
 if(IS("getStringArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;")||
    IS("getIntegerArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;")||
    IS("getParcelableArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;"))return bundle_get_array_list(v,b,a,out);

 if(IS("putString","(Ljava/lang/String;Ljava/lang/String;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  if(a[1].l&&!bundle_is_string_obj(v,a[1].l)){dvm__throw(v,"java/lang/ClassCastException","putString value");return true;}
  bundle_put(v,b,key,len,BT_STRING,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putInt","(Ljava/lang/String;I)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_INT,(bundle_value){.i=a[1].i});return true;
 }
 if(IS("putLong","(Ljava/lang/String;J)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_LONG,(bundle_value){.j=a[1].j});return true;
 }
 if(IS("putBoolean","(Ljava/lang/String;Z)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_BOOLEAN,(bundle_value){.i=a[1].i?1:0});return true;
 }
 if(IS("putDouble","(Ljava/lang/String;D)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_DOUBLE,(bundle_value){.d=a[1].d});return true;
 }
 if(IS("putFloat","(Ljava/lang/String;F)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_FLOAT,(bundle_value){.f=a[1].f});return true;
 }
 if(IS("putShort","(Ljava/lang/String;S)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_SHORT,(bundle_value){.i=(int16_t)a[1].i});return true;
 }
 if(IS("putByte","(Ljava/lang/String;B)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_BYTE,(bundle_value){.i=(int8_t)a[1].i});return true;
 }
 if(IS("putChar","(Ljava/lang/String;C)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_CHAR,(bundle_value){.i=(uint16_t)a[1].i});return true;
 }
 if(IS("putCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_CHAR_SEQUENCE,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putStringArray","(Ljava/lang/String;[Ljava/lang/String;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  if(a[1].l&&!bundle_object_array_is(v,a[1].l,"java/lang/String",NULL)){dvm__throw(v,"java/lang/ClassCastException","putStringArray value");return true;}
  bundle_put(v,b,key,len,BT_STRING_ARRAY,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putIntArray","(Ljava/lang/String;[I)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  if(a[1].l&&!bundle_prim_array_is(v,a[1].l,'I')){dvm__throw(v,"java/lang/ClassCastException","putIntArray value");return true;}
  bundle_put(v,b,key,len,BT_INT_ARRAY,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putByteArray","(Ljava/lang/String;[B)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  if(a[1].l&&!bundle_prim_array_is(v,a[1].l,'B')){dvm__throw(v,"java/lang/ClassCastException","putByteArray value");return true;}
  bundle_put(v,b,key,len,BT_BYTE_ARRAY,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putCharArray","(Ljava/lang/String;[C)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  if(a[1].l&&!bundle_prim_array_is(v,a[1].l,'C')){dvm__throw(v,"java/lang/ClassCastException","putCharArray value");return true;}
  bundle_put(v,b,key,len,BT_CHAR_ARRAY,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putCharSequenceArray","(Ljava/lang/String;[Ljava/lang/CharSequence;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  if(a[1].l&&!bundle_object_array_is(v,a[1].l,"java/lang/String","java/lang/CharSequence")){dvm__throw(v,"java/lang/ClassCastException","putCharSequenceArray value");return true;}
  bundle_put(v,b,key,len,BT_CHAR_SEQUENCE_ARRAY,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putBundle","(Ljava/lang/String;Landroid/os/Bundle;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  if(a[1].l&&!bundle_is_bundle_obj(v,a[1].l)){dvm__throw(v,"java/lang/ClassCastException","putBundle value");return true;}
  bundle_put(v,b,key,len,BT_BUNDLE,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putParcelable","(Ljava/lang/String;Landroid/os/Parcelable;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_PARCELABLE,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putSerializable","(Ljava/lang/String;Ljava/io/Serializable;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_SERIALIZABLE,(bundle_value){.l=a[1].l});return true;
 }
 if(IS("putStringArrayList","(Ljava/lang/String;Ljava/util/ArrayList;)V")||
    IS("putIntegerArrayList","(Ljava/lang/String;Ljava/util/ArrayList;)V")||
    IS("putParcelableArrayList","(Ljava/lang/String;Ljava/util/ArrayList;)V")){
  const char *key;uint32_t len;if(!bundle_key_arg(v,a,&key,&len))return true;
  bundle_put(v,b,key,len,BT_ARRAY_LIST,(bundle_value){.l=a[1].l});return true;
 }
 return false;
}
bool bundle_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return true;
 bool result=bundle_runtime_call_operation(v,c,m,sig,self,a,n,out);
 gc_operation_result(&operation,sig,result&&out?out->l:0);
 return result;
}


/* A different destroy function deliberately has a different body/address from
 * package_metadata.c's bundle_free, and the magic check makes recognition
 * independent of any linker folding. */
static void bundle_test_foreign_destroy(void *p){free(p);}

static bool bundle_test_call(struct dvm *v,dvm_ref self,const char *m,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 return bundle_runtime_call(v,"android/os/Bundle",m,sig,self,a,n,out);
}

static dvm_ref bundle_test_raw(struct dvm *v){
 return dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Bundle"));
}

static bool bundle_runtime_selftest_operation(struct dvm *v){
 union dvm_value out={0},a[2]={{0},{0}},sout={0};
 bool ok=true;const char *stage="init",*first_fail=NULL;
#define MARK_FAIL() do{ok=false;if(!first_fail)first_fail=stage;}while(0)
 dvm_ref b=bundle_runtime_new(v);
 if(!b||dvm_exception(v))return false;

 /* Explicit <init>()V, distinct from bundle_runtime_new's host-side path. */
 dvm_ref raw=bundle_test_raw(v);
 if(!raw||!bundle_test_call(v,raw,"<init>","()V",NULL,0,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,raw,"size","()I",NULL,0,&out)||out.i)MARK_FAIL();
 if(!bundle_test_call(v,raw,"isEmpty","()Z",NULL,0,&out)||!out.i)MARK_FAIL();

 if(!bundle_test_call(v,b,"size","()I",NULL,0,&out)||out.i)MARK_FAIL();
 if(!bundle_test_call(v,b,"isEmpty","()Z",NULL,0,&out)||!out.i)MARK_FAIL();
 stage="putAll";
 dvm_ref merge=bundle_runtime_new(v),merge_key=dvm_new_string(v,"merged"),merge_value=dvm_new_string(v,"value");
 if(!merge||!merge_key||!merge_value)return false;
 a[0].l=merge_key;a[1].i=7;
 if(!bundle_test_call(v,raw,"putInt","(Ljava/lang/String;I)V",a,2,&out))MARK_FAIL();
 a[1].l=merge_value;
 if(!bundle_test_call(v,merge,"putString","(Ljava/lang/String;Ljava/lang/String;)V",a,2,&out))MARK_FAIL();
 a[0].l=merge;
 if(!bundle_test_call(v,raw,"putAll","(Landroid/os/Bundle;)V",a,1,&out)||dvm_exception(v))MARK_FAIL();
 a[0].l=merge_key;
 if(!bundle_test_call(v,raw,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l!=merge_value)MARK_FAIL();
 if(!bundle_test_call(v,merge,"clear","()V",NULL,0,&out))MARK_FAIL();
 if(!bundle_test_call(v,raw,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l!=merge_value)MARK_FAIL();
 a[0].l=raw;if(!bundle_test_call(v,raw,"putAll","(Landroid/os/Bundle;)V",a,1,&out)||dvm_exception(v))MARK_FAIL();
 a[0].l=0;if(!bundle_test_call(v,raw,"putAll","(Landroid/os/Bundle;)V",a,1,&out)||!dvm_exception(v))MARK_FAIL();dvm_clear_exception(v);
 if(!bundle_test_call(v,raw,"clear","()V",NULL,0,&out))MARK_FAIL();

 stage="string";
 /* String and null value semantics. */
 dvm_ref name=dvm_new_string(v,"name"),value=dvm_new_string(v,"value");
 if(!name||!value)return false;
 a[0].l=name;a[1].l=value;
 if(!bundle_test_call(v,b,"putString","(Ljava/lang/String;Ljava/lang/String;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"containsKey","(Ljava/lang/String;)Z",a,1,&out)||!out.i)MARK_FAIL();
 if(!bundle_test_call(v,b,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l!=value)MARK_FAIL();
 if(!bundle_test_call(v,b,"get","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)||out.l!=value)MARK_FAIL();
 dvm_ref fallback=dvm_new_string(v,"fallback"),missing=dvm_new_string(v,"missing");
 if(!fallback||!missing)return false;
 a[0].l=missing;a[1].l=fallback;
 if(!bundle_test_call(v,b,"getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",a,2,&out)||out.l!=fallback)MARK_FAIL();
 if(!bundle_test_call(v,b,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l)MARK_FAIL();
 dvm_ref null_key=dvm_new_string(v,"nullValue");
 a[0].l=null_key;a[1].l=0;
 if(!bundle_test_call(v,b,"putString","(Ljava/lang/String;Ljava/lang/String;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"containsKey","(Ljava/lang/String;)Z",a,1,&out)||!out.i)MARK_FAIL();
 if(!bundle_test_call(v,b,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l)MARK_FAIL();

 stage="scalars";
 /* Scalar put/get, defaults and Object boxing. */
 dvm_ref ki=dvm_new_string(v,"int"),kj=dvm_new_string(v,"long"),kz=dvm_new_string(v,"bool");
 dvm_ref kf=dvm_new_string(v,"float"),kd=dvm_new_string(v,"double"),ks=dvm_new_string(v,"short");
 dvm_ref kb=dvm_new_string(v,"byte"),kc=dvm_new_string(v,"char");
 if(!ki||!kj||!kz||!kf||!kd||!ks||!kb||!kc)return false;
 a[0].l=ki;a[1].i=42;
 if(!bundle_test_call(v,b,"putInt","(Ljava/lang/String;I)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getInt","(Ljava/lang/String;)I",a,1,&out)||out.i!=42)MARK_FAIL();
 a[0].l=missing;a[1].i=7;
 if(!bundle_test_call(v,b,"getInt","(Ljava/lang/String;I)I",a,2,&out)||out.i!=7)MARK_FAIL();
 a[0].l=name;a[1].i=9;
 if(!bundle_test_call(v,b,"getInt","(Ljava/lang/String;I)I",a,2,&out)||out.i!=9)MARK_FAIL();
 a[0].l=ki; /* an int entry must not satisfy getString: expect null, not the name key's String */
 if(!bundle_test_call(v,b,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l)MARK_FAIL();
 a[0].l=ki;
 if(!bundle_test_call(v,b,"get","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)||!out.l)MARK_FAIL();
 else if(!java_basics_call(v,"java/lang/Integer","intValue","()I",out.l,NULL,0,&sout)||sout.i!=42)MARK_FAIL();

 a[0].l=kj;a[1].j=1234567890123LL;
 if(!bundle_test_call(v,b,"putLong","(Ljava/lang/String;J)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getLong","(Ljava/lang/String;)J",a,1,&out)||out.j!=1234567890123LL)MARK_FAIL();
 a[0].l=missing;a[1].j=5;
 if(!bundle_test_call(v,b,"getLong","(Ljava/lang/String;J)J",a,2,&out)||out.j!=5)MARK_FAIL();
 a[0].l=kj;
 if(!bundle_test_call(v,b,"get","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)||!out.l)MARK_FAIL();
 else if(!java_basics_call(v,"java/lang/Long","longValue","()J",out.l,NULL,0,&sout)||sout.j!=1234567890123LL)MARK_FAIL();

 a[0].l=kz;a[1].i=1;
 if(!bundle_test_call(v,b,"putBoolean","(Ljava/lang/String;Z)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getBoolean","(Ljava/lang/String;)Z",a,1,&out)||!out.i)MARK_FAIL();
 a[0].l=missing;a[1].i=1;
 if(!bundle_test_call(v,b,"getBoolean","(Ljava/lang/String;Z)Z",a,2,&out)||!out.i)MARK_FAIL();
 a[0].l=kz;
 if(!bundle_test_call(v,b,"get","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)||!out.l)MARK_FAIL();
 else if(!java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",out.l,NULL,0,&sout)||!sout.i)MARK_FAIL();

 a[0].l=ks;a[1].i=300;
 if(!bundle_test_call(v,b,"putShort","(Ljava/lang/String;S)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getShort","(Ljava/lang/String;)S",a,1,&out)||out.i!=300)MARK_FAIL();
 a[0].l=missing;a[1].i=3;
 if(!bundle_test_call(v,b,"getShort","(Ljava/lang/String;S)S",a,2,&out)||out.i!=3)MARK_FAIL();

 a[0].l=kb;a[1].i=-5;
 if(!bundle_test_call(v,b,"putByte","(Ljava/lang/String;B)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getByte","(Ljava/lang/String;)B",a,1,&out)||out.i!=-5)MARK_FAIL();
 a[0].l=missing;a[1].i=2;
 if(!bundle_test_call(v,b,"getByte","(Ljava/lang/String;B)B",a,2,&out)||out.i!=2)MARK_FAIL();

 a[0].l=kc;a[1].i=0x4e2d;
 if(!bundle_test_call(v,b,"putChar","(Ljava/lang/String;C)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getChar","(Ljava/lang/String;)C",a,1,&out)||out.i!=0x4e2d)MARK_FAIL();
 a[0].l=missing;a[1].i=1;
 if(!bundle_test_call(v,b,"getChar","(Ljava/lang/String;C)C",a,2,&out)||out.i!=1)MARK_FAIL();

 a[0].l=kf;a[1].f=1.5f;
 if(!bundle_test_call(v,b,"putFloat","(Ljava/lang/String;F)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getFloat","(Ljava/lang/String;)F",a,1,&out)||out.f!=1.5f)MARK_FAIL();
 a[0].l=missing;a[1].f=2.5f;
 if(!bundle_test_call(v,b,"getFloat","(Ljava/lang/String;F)F",a,2,&out)||out.f!=2.5f)MARK_FAIL();
 a[0].l=kf;
 if(!bundle_test_call(v,b,"get","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)||!out.l)MARK_FAIL();
 else if(!java_basics_call(v,"java/lang/Float","floatValue","()F",out.l,NULL,0,&sout)||sout.f!=1.5f)MARK_FAIL();

 a[0].l=kd;a[1].d=2.25;
 if(!bundle_test_call(v,b,"putDouble","(Ljava/lang/String;D)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getDouble","(Ljava/lang/String;)D",a,1,&out)||out.d!=2.25)MARK_FAIL();
 a[0].l=missing;a[1].d=3.5;
 if(!bundle_test_call(v,b,"getDouble","(Ljava/lang/String;D)D",a,2,&out)||out.d!=3.5)MARK_FAIL();
 a[0].l=kd;
 if(!bundle_test_call(v,b,"get","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)||!out.l)MARK_FAIL();
 else if(!java_basics_call(v,"java/lang/Double","doubleValue","()D",out.l,NULL,0,&sout)||sout.d!=2.25)MARK_FAIL();

 stage="arrays";
 /* Arrays are stored by reference. */
 dvm_ref strarr=dvm_new_array(v,'L',"Ljava/lang/String;",2);
 dvm_ref intarr=dvm_new_array(v,'I',"I",2),bytearr=dvm_new_array(v,'B',"B",3),chararr=dvm_new_array(v,'C',"C",2);
 dvm_ref csarr=dvm_new_array(v,'L',"Ljava/lang/CharSequence;",1);
 if(!strarr||!intarr||!bytearr||!chararr||!csarr)return false;
 ((dvm_ref*)dvm_array_data(v,strarr))[0]=value;
 ((int32_t*)dvm_array_data(v,intarr))[0]=3;
 ((uint8_t*)dvm_array_data(v,bytearr))[0]=0x7f;
 ((uint16_t*)dvm_array_data(v,chararr))[0]=0x4e2d;
 ((dvm_ref*)dvm_array_data(v,csarr))[0]=value;
 dvm_ref ksa=dvm_new_string(v,"strArr"),kia=dvm_new_string(v,"intArr"),kba=dvm_new_string(v,"byteArr");
 dvm_ref kca=dvm_new_string(v,"charArr"),kcsa=dvm_new_string(v,"csArr");
 if(!ksa||!kia||!kba||!kca||!kcsa)return false;
 a[0].l=ksa;a[1].l=strarr;
 if(!bundle_test_call(v,b,"putStringArray","(Ljava/lang/String;[Ljava/lang/String;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getStringArray","(Ljava/lang/String;)[Ljava/lang/String;",a,1,&out)||out.l!=strarr)MARK_FAIL();
 if(!bundle_test_call(v,b,"getCharSequenceArray","(Ljava/lang/String;)[Ljava/lang/CharSequence;",a,1,&out)||out.l!=strarr)MARK_FAIL();
 if(!bundle_test_call(v,b,"get","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)||out.l!=strarr)MARK_FAIL();
 a[0].l=kia;a[1].l=intarr;
 if(!bundle_test_call(v,b,"putIntArray","(Ljava/lang/String;[I)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getIntArray","(Ljava/lang/String;)[I",a,1,&out)||out.l!=intarr)MARK_FAIL();
 if(!bundle_test_call(v,b,"getByteArray","(Ljava/lang/String;)[B",a,1,&out)||out.l)MARK_FAIL();
 a[0].l=kba;a[1].l=bytearr;
 if(!bundle_test_call(v,b,"putByteArray","(Ljava/lang/String;[B)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getByteArray","(Ljava/lang/String;)[B",a,1,&out)||out.l!=bytearr)MARK_FAIL();
 a[0].l=kca;a[1].l=chararr;
 if(!bundle_test_call(v,b,"putCharArray","(Ljava/lang/String;[C)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getCharArray","(Ljava/lang/String;)[C",a,1,&out)||out.l!=chararr)MARK_FAIL();
 a[0].l=kcsa;a[1].l=csarr;
 if(!bundle_test_call(v,b,"putCharSequenceArray","(Ljava/lang/String;[Ljava/lang/CharSequence;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getCharSequenceArray","(Ljava/lang/String;)[Ljava/lang/CharSequence;",a,1,&out)||out.l!=csarr)MARK_FAIL();
 if(!bundle_test_call(v,b,"getStringArray","(Ljava/lang/String;)[Ljava/lang/String;",a,1,&out)||out.l)MARK_FAIL();

 stage="charsequence";
 /* CharSequence scalar: a String is both String and CharSequence. */
 dvm_ref kcs=dvm_new_string(v,"cs");
 a[0].l=kcs;a[1].l=value;
 if(!bundle_test_call(v,b,"putCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;",a,1,&out)||out.l!=value)MARK_FAIL();
 if(!bundle_test_call(v,b,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l!=value)MARK_FAIL();

 stage="bundle_copy";
 /* Nested Bundle and map-copy constructor. */
 dvm_ref child=bundle_runtime_new(v);
 dvm_ref child_key=dvm_new_string(v,"childKey"),child_value=dvm_new_string(v,"childValue");
 if(!child||!child_key||!child_value||dvm_exception(v))return false;
 a[0].l=child_key;a[1].l=child_value;
 if(!bundle_test_call(v,child,"putString","(Ljava/lang/String;Ljava/lang/String;)V",a,2,&out)||dvm_exception(v))return false;
 dvm_ref kbundle=dvm_new_string(v,"bundle");
 if(!kbundle)return false;
 a[0].l=kbundle;a[1].l=child;
 if(!bundle_test_call(v,b,"putBundle","(Ljava/lang/String;Landroid/os/Bundle;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getBundle","(Ljava/lang/String;)Landroid/os/Bundle;",a,1,&out)||out.l!=child)MARK_FAIL();
 dvm_ref copy=bundle_test_raw(v);
 a[0].l=child;
 if(!copy||!bundle_test_call(v,copy,"<init>","(Landroid/os/Bundle;)V",a,1,&out)||dvm_exception(v))return false;
 if(copy==child)MARK_FAIL();
 if(!bundle_test_call(v,copy,"size","()I",NULL,0,&out)||out.i!=1)MARK_FAIL();
 a[0].l=child_key;
 if(!bundle_test_call(v,copy,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l!=child_value)MARK_FAIL();
 dvm_ref after=dvm_new_string(v,"after");
 a[0].l=after;a[1].i=7;
 if(!bundle_test_call(v,child,"putInt","(Ljava/lang/String;I)V",a,2,&out)||dvm_exception(v))return false;
 a[0].l=after;a[1].i=0;
 if(!bundle_test_call(v,copy,"getInt","(Ljava/lang/String;I)I",a,2,&out)||out.i)MARK_FAIL(); /* map copied, later puts do not leak */
 dvm_ref copy_null=bundle_test_raw(v);
 a[0].l=0;
 if(!copy_null||!bundle_test_call(v,copy_null,"<init>","(Landroid/os/Bundle;)V",a,1,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,copy_null,"isEmpty","()Z",NULL,0,&out)||!out.i)MARK_FAIL();

 stage="references";
 /* Parcelable/Serializable/ArrayList are references, not serialised copies. */
 dvm_ref dummy=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Object"));
 dvm_ref list=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/ArrayList"));
 dvm_ref kp=dvm_new_string(v,"parcel"),ksr=dvm_new_string(v,"serial"),kl=dvm_new_string(v,"list");
 if(!dummy||!list||!kp||!ksr||!kl)return false;
 a[0].l=kp;a[1].l=dummy;
 if(!bundle_test_call(v,b,"putParcelable","(Ljava/lang/String;Landroid/os/Parcelable;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getParcelable","(Ljava/lang/String;)Landroid/os/Parcelable;",a,1,&out)||out.l!=dummy)MARK_FAIL();
 if(!bundle_test_call(v,b,"getSerializable","(Ljava/lang/String;)Ljava/io/Serializable;",a,1,&out)||out.l)MARK_FAIL();
 a[0].l=ksr;a[1].l=dummy;
 if(!bundle_test_call(v,b,"putSerializable","(Ljava/lang/String;Ljava/io/Serializable;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getSerializable","(Ljava/lang/String;)Ljava/io/Serializable;",a,1,&out)||out.l!=dummy)MARK_FAIL();
 if(!bundle_test_call(v,b,"getParcelable","(Ljava/lang/String;)Landroid/os/Parcelable;",a,1,&out)||out.l)MARK_FAIL();
 a[0].l=kl;a[1].l=list;
 if(!bundle_test_call(v,b,"putStringArrayList","(Ljava/lang/String;Ljava/util/ArrayList;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"getStringArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;",a,1,&out)||out.l!=list)MARK_FAIL();
 if(!bundle_test_call(v,b,"getIntegerArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;",a,1,&out)||out.l!=list)MARK_FAIL();
 if(!bundle_test_call(v,b,"getParcelableArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;",a,1,&out)||out.l!=list)MARK_FAIL();
 a[0].l=kl;
 if(!bundle_test_call(v,b,"get","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)||out.l!=list)MARK_FAIL();

 stage="keyset";
 /* keySet is backed by set_runtime's HashSet. */
 if(!bundle_test_call(v,b,"size","()I",NULL,0,&out))MARK_FAIL();
 int bundle_size=out.i;
 if(!bundle_test_call(v,b,"keySet","()Ljava/util/Set;",NULL,0,&out)||!out.l||dvm_exception(v))MARK_FAIL();
 else{
  dvm_ref set=out.l;
  if(!set_runtime_call(v,"java/util/Set","size","()I",set,NULL,0,&sout)||sout.i!=bundle_size)MARK_FAIL();
  a[0].l=name;
  if(!set_runtime_call(v,"java/util/Set","contains","(Ljava/lang/Object;)Z",set,a,1,&sout)||!sout.i)MARK_FAIL();
 }

 stage="remove_clear";
 /* remove/clear. */
 a[0].l=name;
 if(!bundle_test_call(v,b,"remove","(Ljava/lang/String;)V",a,1,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"containsKey","(Ljava/lang/String;)Z",a,1,&out)||out.i)MARK_FAIL();
 if(!bundle_test_call(v,b,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l)MARK_FAIL();
 a[0].l=missing;
 if(!bundle_test_call(v,b,"remove","(Ljava/lang/String;)V",a,1,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"clear","()V",NULL,0,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,b,"size","()I",NULL,0,&out)||out.i)MARK_FAIL();
 if(!bundle_test_call(v,b,"isEmpty","()Z",NULL,0,&out)||!out.i)MARK_FAIL();

 stage="capacity";
 /* Capacity constructor. */
 dvm_ref cap=bundle_test_raw(v);
 a[0].i=4;
 if(!cap||!bundle_test_call(v,cap,"<init>","(I)V",a,1,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,cap,"size","()I",NULL,0,&out)||out.i)MARK_FAIL();
 a[0].l=name;a[1].l=value;
 if(!bundle_test_call(v,cap,"putString","(Ljava/lang/String;Ljava/lang/String;)V",a,2,&out)||dvm_exception(v))return false;
 if(!bundle_test_call(v,cap,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||out.l!=value)MARK_FAIL();

 stage="errors";
 /* Error contracts. */
 a[0].l=0;a[1].i=1;
 if(!bundle_test_call(v,b,"putInt","(Ljava/lang/String;I)V",a,2,&out)||!dvm_exception(v))MARK_FAIL();
 dvm_clear_exception(v);
 a[0].l=0;
 if(!bundle_test_call(v,b,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&out)||!dvm_exception(v))MARK_FAIL();
 dvm_clear_exception(v);
 dvm_ref uninit=bundle_test_raw(v);
 if(!uninit||!bundle_test_call(v,uninit,"size","()I",NULL,0,&out)||!dvm_exception(v))MARK_FAIL();
 dvm_clear_exception(v);
 if(!bundle_test_call(v,cap,"<init>","()V",NULL,0,&out)||!dvm_exception(v))MARK_FAIL();
 dvm_clear_exception(v);

 stage="foreign";
 /* A foreign Bundle (package_metadata-style data_destroy) must not be claimed. */
 dvm_ref foreign=bundle_test_raw(v);
 const dvm_ref gc_ref_fo_10=(foreign); if(!gc_operation_admit(v,gc_ref_fo_10))return 0;
 if(!dvm__obj(v,gc_ref_fo_10))MARK_FAIL();
 else{
  dvm__obj(v,gc_ref_fo_10)->data=calloc(1,1);dvm__obj(v,gc_ref_fo_10)->data_destroy=bundle_test_foreign_destroy;
  bool claimed=bundle_test_call(v,foreign,"size","()I",NULL,0,&out);
  if(claimed||dvm_exception(v))MARK_FAIL();
  free(dvm__obj(v,gc_ref_fo_10)->data);dvm__obj(v,gc_ref_fo_10)->data=NULL;dvm__obj(v,gc_ref_fo_10)->data_destroy=NULL;
 }

 if(dvm_exception(v))MARK_FAIL();
 sdk_trace_fprintf(stderr,"BUNDLE_RUNTIME_SELFTEST %s failed_stage=%s constructors=1 scalars=1 arrays=1 references=1 bundle_copy=1 keyset=1 null_key_npe=1 uninitialized_ise=1 foreign_bundle_left=1 object_boxing=Integer,Long,Boolean,Float,Double,Character object_boxing_unavailable=Byte,Short\n",ok?"PASS":"FAIL",first_fail?first_fail:"none");
#undef MARK_FAIL
 return ok;
}
bool bundle_runtime_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=bundle_runtime_selftest_operation(v);
 return result;
}


/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int bundle_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 struct gc_operation_pin pin __attribute__((cleanup(gc_operation_pin_end)));
 if(!gc_operation_pin_begin(v,&pin,o))return DVM_GC_PAYLOAD_INVALID;
 o=dvm__obj(v,pin.ref);

 (void)v;if(o->data_destroy!=bundle_destroy)return DVM_GC_PAYLOAD_UNKNOWN;bundle_data *p=o->data;
 if(p->magic!=BUNDLE_MAGIC||p->count>p->capacity||(!p->entries&&p->count))return DVM_GC_PAYLOAD_INVALID;
 if(visit)for(size_t i=0;i<p->count;i++)if(bundle_ref_type(p->entries[i].type))visit(&p->entries[i].value.l,user);return DVM_GC_PAYLOAD_REFS;
}
