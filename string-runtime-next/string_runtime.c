#include "string_runtime.h"
#include "enum_runtime.h"
#include "java_basics.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
static struct dvm_object* text(struct dvm* v,dvm_ref ref){struct dvm_object* o=dvm__obj(v,ref);return o&&o->cls&&!strcmp(o->cls->name,"java/lang/String")?o:NULL;}
/* Internal VM strings may encode isolated UTF16 surrogates; preserve them. */
static bool index_text(struct dvm* v,struct dvm_object* o){
 if(o->string_index_ready&&o->string_units)return true;
 size_t length=o->utf8_len;const unsigned char* bytes=(const unsigned char*)o->utf8;
 uint16_t* units=malloc((length+1)*sizeof *units);if(!units){dvm__throw(v,"java/lang/OutOfMemoryError","UTF16 index");return false;}
 uint32_t count=0;
 for(size_t i=0;i<length;){uint32_t cp=bytes[i++];int extra=0;
  if(cp>=0xC0&&cp<=0xDF){cp&=31;extra=1;}else if(cp>=0xE0&&cp<=0xEF){cp&=15;extra=2;}else if(cp>=0xF0&&cp<=0xF4){cp&=7;extra=3;}else if(cp>=128){free(units);dvm__throw(v,"java/lang/UnsupportedOperationException","invalid internal UTF8");return false;}
  if((size_t)extra>length-i){free(units);dvm__throw(v,"java/lang/UnsupportedOperationException","truncated internal UTF8");return false;}
  for(int j=0;j<extra;j++){if((bytes[i]&0xC0)!=0x80){free(units);dvm__throw(v,"java/lang/UnsupportedOperationException","invalid internal UTF8");return false;}cp=(cp<<6)|(bytes[i++]&63);}
  if(cp>0x10FFFF||(extra==1&&cp<0x80)||(extra==2&&cp<0x800)||(extra==3&&cp<0x10000)){free(units);dvm__throw(v,"java/lang/UnsupportedOperationException","invalid code point");return false;}
  if(cp>0xFFFF){cp-=0x10000;units[count++]=(uint16_t)(0xD800+(cp>>10));units[count++]=(uint16_t)(0xDC00+(cp&1023));}else units[count++]=(uint16_t)cp;
 }
 free(o->string_units);o->string_units=units;o->string_length=count;o->string_index_ready=true;return true;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
bool string_runtime_call(struct dvm* v,const char* c,const char* m,const char* sig,dvm_ref self,const union dvm_value* a,int n,union dvm_value* out){
 if(strcmp(c,"java/lang/String"))return false;
 if(IS("valueOf","(Ljava/lang/Object;)Ljava/lang/String;")&&n==1){
  if(!a[0].l){out->l=dvm_new_string(v,"null");return true;}
  if(text(v,a[0].l)){out->l=a[0].l;return true;}
  struct dvm_object* o=dvm__obj(v,a[0].l);if(!o)return false;
  struct dvm_method* method=dvm_find_method(v,o->cls,"toString","()Ljava/lang/String;");
  if(method&&(method->has_code||method->builtin)){dvm_call(v,method,a[0].l,NULL,0,out);return true;}
  if(enum_runtime_call(v,"java/lang/Enum","toString","()Ljava/lang/String;",a[0].l,NULL,0,out))return true;
  return java_basics_call(v,o->cls->name,"toString","()Ljava/lang/String;",a[0].l,NULL,0,out);
 }
 bool length=IS("length","()I"),empty=IS("isEmpty","()Z"),hash=IS("hashCode","()I");
 bool starts=IS("startsWith","(Ljava/lang/String;)Z")||IS("startsWith","(Ljava/lang/String;I)Z");
 bool ends=IS("endsWith","(Ljava/lang/String;)Z"),contains=IS("contains","(Ljava/lang/CharSequence;)Z");
 if(!length&&!empty&&!hash&&!starts&&!ends&&!contains)return false;
 struct dvm_object* source=text(v,self);if(!source)return false;
 struct dvm_object* needle=NULL;
 if(starts||ends||contains){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","String argument");return true;}needle=text(v,a[0].l);if(!needle)return false;}
 if(!index_text(v,source))return true;
 uint32_t size=source->string_length;
 if(length){out->i=(int32_t)size;return true;}if(empty){out->i=size==0;return true;}
 if(hash){uint32_t h=0;for(uint32_t i=0;i<size;i++)h=h*31+source->string_units[i];out->i=(int32_t)h;return true;}
 if(!index_text(v,needle))return true;
 uint32_t count=needle->string_length;out->i=0;if(count>size)return true;
 if(contains){for(uint32_t i=0;i<=size-count;i++)if(!memcmp(source->string_units+i,needle->string_units,(size_t)count*2)){out->i=1;break;}return true;}
 int64_t offset=ends?(int64_t)size-count:(n==2?a[1].i:0);
 if(offset<0||(uint64_t)offset>size-count)return true;
 out->i=!memcmp(source->string_units+(size_t)offset,needle->string_units,(size_t)count*2);return true;
}
bool string_runtime_selftest(struct dvm* v){
 static const char bytes[]={'a',0,(char)0xF0,(char)0x9F,(char)0x98,(char)0x80,'b'};
 dvm_ref value=dvm_new_string_n(v,bytes,sizeof bytes);union dvm_value out={0},a[2]={{.l=dvm_new_string(v,"b")},{.i=4}};
 if(!string_runtime_call(v,"java/lang/String","length","()I",value,NULL,0,&out)||out.i!=5)return false;
 if(!string_runtime_call(v,"java/lang/String","startsWith","(Ljava/lang/String;I)Z",value,a,2,&out)||!out.i)return false;
 a[1].i=-1;if(!string_runtime_call(v,"java/lang/String","startsWith","(Ljava/lang/String;I)Z",value,a,2,&out)||out.i)return false;
 if(!string_runtime_call(v,"java/lang/String","hashCode","()I",value,NULL,0,&out))return false;
 uint32_t h=0;const uint16_t units[]={'a',0,0xD83D,0xDE00,'b'};for(size_t i=0;i<5;i++)h=h*31+units[i];
 if(out.i!=(int32_t)h||dvm_exception(v))return false;
 a[0].l=dvm_new_string(v,"");a[1].i=5;
 if(!string_runtime_call(v,"java/lang/String","startsWith","(Ljava/lang/String;I)Z",value,a,2,&out)||!out.i)return false;
 a[0].l=dvm_new_string(v,"b");
 if(!string_runtime_call(v,"java/lang/String","endsWith","(Ljava/lang/String;)Z",value,a,1,&out)||!out.i)return false;
 if(string_runtime_call(v,"java/lang/String","unsupportedMethod","()V",value,NULL,0,&out))return false;
 sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST PASS UTF16_nonBMP_NUL=1 prefix_offset=1 hash=1 strict_unknown=1\n");
 return !dvm_exception(v);
}
