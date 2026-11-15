/* Standard-library adapter. Covered extracted bodies retain MPL-2.0 notices. */
#include "runtime_extra.h"
#include "portable.h"
#include <errno.h>
#include <inttypes.h>
#include <sys/random.h>
static int runtime_random_bytes(unsigned char *out,size_t n){
 size_t done=0;while(done<n){ssize_t got=getrandom(out+done,n-done,0);if(got<0&&errno==EINTR)continue;if(got<=0)return 0;done+=(size_t)got;}return 1;
}
#define ARG(i) args[i]
#define RETI(x) do{out->i=(x);return true;}while(0)
#define RETL(x) do{out->l=(x);return true;}while(0)
#define RETJ(x) do{out->j=(x);return true;}while(0)
#define RETV() return true
#include "upstream_standard.inc"
void runtime_extra_vm_start(struct dvm *v){
 struct dvm_class *c=dvm__class_by_desc(v,"Ljava/util/UUID;");if(!c)return;
 c->ifields=calloc(2,sizeof *c->ifields);if(!c->ifields){dvm__throw(v,"java/lang/OutOfMemoryError","UUID fields");return;}c->nifields=2;c->islots=2;
 for(int i=0;i<2;++i){c->ifields[i].cls=c;c->ifields[i].name=i?"lsb":"msb";c->ifields[i].type="J";c->ifields[i].kind='J';c->ifields[i].width=1;c->ifields[i].slot=(uint16_t)i;}
}
static bool extra_invoke(dvm_builtin_fn fn,struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){bool ok=fn(v,self,a,n,out);if(!ok&&!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","standard operation unavailable");return true;}
bool runtime_extra_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 memset(out,0,sizeof *out);
 if(!strcmp(c,"java/lang/Runtime")){
  if(IS("getRuntime","()Ljava/lang/Runtime;"))return extra_invoke(runtime_getRuntime,v,self,a,nargs,out);
  if(IS("availableProcessors","()I")){long n=sysconf(_SC_NPROCESSORS_ONLN);if(n<1||n>INT32_MAX)dvm__throw(v,"java/lang/IllegalStateException","CPU topology unavailable");else out->i=(int)n;return true;}
  if(IS("maxMemory","()J"))sdk_trace_fprintf(stderr,"RUNTIME_BOUNDARY maxMemory requires an enforced managed-heap budget; host RAM and arbitrary constants are not Java heap limits\n");
  return false;
 }
 if(!strcmp(c,"java/util/UUID")){
#define UUID(name,signature,fn) if(IS(name,signature))return extra_invoke(fn,v,self,a,nargs,out)
  UUID("randomUUID","()Ljava/util/UUID;",uuid_randomUUID);UUID("<init>","(JJ)V",uuid_init);UUID("toString","()Ljava/lang/String;",uuid_toString);UUID("getMostSignificantBits","()J",uuid_msb);UUID("getLeastSignificantBits","()J",uuid_lsb);UUID("equals","(Ljava/lang/Object;)Z",uuid_equals);
  if(IS("version","()I")||IS("variant","()I")){uint64_t hi,lo;uuid_bits(v,self,&hi,&lo);out->i=IS("version","()I")?(int)((hi>>12)&15):!(lo>>63)?0:((lo>>62)==2)?2:((lo>>61)==6)?6:7;return true;}
  return false;
 }
 if(!strcmp(c,"java/lang/Long")&&IS("toHexString","(J)Ljava/lang/String;")){char text[17];snprintf(text,sizeof text,"%"PRIx64,a[0].ju);out->l=dvm_new_string(v,text);return true;}
 if(strcmp(c,"java/lang/StringBuilder"))return false;
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","StringBuilder");return true;}
 if(IS("<init>","()V")||IS("<init>","(I)V")||IS("<init>","(Ljava/lang/String;)V")){
  if(IS("<init>","(I)V")&&a[0].i<0){dvm__throw(v,"java/lang/NegativeArraySizeException","builder capacity");return true;}
  if(!sb_init(v,self,NULL,0,out))return true;
  if(IS("<init>","(I)V")&&a[0].i>0)sb_reserve(v,o,(size_t)a[0].i);
  if(IS("<init>","(Ljava/lang/String;)V")){const char *text=dvm_string_utf8(v,a[0].l);if(!a[0].l)dvm__throw(v,"java/lang/NullPointerException","builder initial String");else if(!text){sdk_trace_missing_api("java/lang/String","toString","()Ljava/lang/String;");dvm__throw(v,"java/lang/UnsupportedOperationException","builder initial String type");}else sb_append(v,self,text,dvm_string_utf8_length(v,a[0].l));}
  return true;
 }
 if(IS("toString","()Ljava/lang/String;"))return extra_invoke(sb_toString,v,self,a,nargs,out);
 if(!strncmp(m,"append",7)&&!strncmp(sig,"(",1)){
  char text[64];const char *bytes=NULL;size_t n=0;
  if(IS("append","(Ljava/lang/String;)Ljava/lang/StringBuilder;")){bytes=a[0].l?dvm_string_utf8(v,a[0].l):"null";if(!bytes)return false;n=a[0].l?dvm_string_utf8_length(v,a[0].l):4;}
  else if(IS("append","(I)Ljava/lang/StringBuilder;")){snprintf(text,sizeof text,"%d",a[0].i);bytes=text;n=strlen(text);}
  else if(IS("append","(J)Ljava/lang/StringBuilder;")){snprintf(text,sizeof text,"%"PRId64,a[0].j);bytes=text;n=strlen(text);}
  else if(IS("append","(Z)Ljava/lang/StringBuilder;")){bytes=a[0].i?"true":"false";n=strlen(bytes);}
  else return false;
  sb_append(v,self,bytes,n);out->l=self;return true;
 }
 return false;
}
