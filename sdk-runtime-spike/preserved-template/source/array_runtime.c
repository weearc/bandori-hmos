#include "array_runtime.h"
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
bool array_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *args,int n,union dvm_value *out){
 (void)args;(void)c;
 if(strcmp(m,"clone")||strcmp(sig,"()Ljava/lang/Object;")||n)return false;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o||o->kind!=DVM_OBJ_ARRAY||!o->cls||!o->cls->desc||o->cls->desc[0]!='[')return false;
 const char *desc=o->cls->desc+1;size_t width=(size_t)dvm__elem_width(o->elem_kind);
 if(o->length>SIZE_MAX/width){dvm__throw(v,"java/lang/OutOfMemoryError","array clone size");return true;}
 size_t bytes=(size_t)o->length*width;
 out->l=dvm_new_array(v,o->elem_kind,desc,o->length);
 if(!out->l){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","array clone");return true;}
 if(bytes)memcpy(dvm_array_data(v,out->l),o->data,bytes);
 return true;
}
bool array_runtime_selftest(struct dvm *v){
 const char *types[]={"Z","B","C","S","I","J","F","D","Ljava/lang/Object;","[B"};
 for(unsigned i=0;i<sizeof types/sizeof types[0];i++){
  const char *desc=types[i];char kind=desc[0]=='L'||desc[0]=='['?'L':desc[0];
  dvm_ref source=dvm_new_array(v,kind,desc,3);if(!source)return false;
  size_t bytes=3*(size_t)dvm__elem_width(kind);
  if(kind=='L'){dvm_ref *values=dvm_array_data(v,source);values[0]=dvm_new_string(v,"shared-child");values[2]=values[0];}
  else memset(dvm_array_data(v,source),0x5a,bytes);
  union dvm_value out={0};
  if(!array_runtime_call(v,dvm_object_class(v,source)->name,"clone","()Ljava/lang/Object;",source,NULL,0,&out)||!out.l||out.l==source)return false;
  if(dvm_object_class(v,out.l)!=dvm_object_class(v,source)||memcmp(dvm_array_data(v,source),dvm_array_data(v,out.l),bytes))return false;
  if(kind=='L')((dvm_ref*)dvm_array_data(v,out.l))[0]=0;
  else ((unsigned char*)dvm_array_data(v,out.l))[0]^=1;
  if(!memcmp(dvm_array_data(v,source),dvm_array_data(v,out.l),bytes))return false;
 }
 dvm_ref empty=dvm_new_array(v,'B',"B",0);union dvm_value out={0};
 if(!array_runtime_call(v,"[B","clone","()Ljava/lang/Object;",empty,NULL,0,&out)||!out.l||out.l==empty||dvm_array_length(v,out.l))return false;
 sdk_trace_fprintf(stderr,"ARRAY_RUNTIME_SELFTEST PASS primitive_types=8 shallow_reference=1 runtime_type=1 independent_storage=1 empty=1\n");
 return !dvm_exception(v);
}
