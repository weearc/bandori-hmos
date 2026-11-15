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
/* StringBuilder stores UTF-8 bytes, but Java reports length and indexes in
 * UTF-16 code units.  These helpers decode the internal representation one
 * code point at a time.  A 4-byte sequence is one supplementary code point
 * and counts as two units; a 3-byte surrogate sequence is one unit, because
 * append(char) writes unpaired surrogates that way and deleteCharAt must be
 * able to leave the surviving half of a pair in place.  Malformed UTF-8 is
 * reported and never silently miscounted. */
static bool sb_unit_next(const unsigned char *s,size_t n,size_t at,uint32_t *cp,size_t *width){
 if(at>=n)return false;
 unsigned c=s[at];size_t extra;uint32_t value;
 if(c<0x80){*cp=c;*width=1;return true;}
 if(c>=0xc2&&c<=0xdf){extra=1;value=c&0x1f;}
 else if(c>=0xe0&&c<=0xef){extra=2;value=c&0x0f;}
 else if(c>=0xf0&&c<=0xf4){extra=3;value=c&0x07;}
 else return false;
 if(extra>n-at-1)return false;
 for(size_t i=1;i<=extra;++i){
  if((s[at+i]&0xc0)!=0x80)return false;
  value=(value<<6)|(s[at+i]&0x3f);
 }
 if((extra==1&&value<0x80)||(extra==2&&value<0x800)||(extra==3&&value<0x10000)||value>0x10ffff)return false;
 *cp=value;*width=extra+1;return true;
}
static bool sb_utf16_units(const char *text,size_t n,uint32_t *units){
 uint32_t count=0;size_t at=0;
 while(at<n){
  uint32_t cp;size_t width;
  if(!sb_unit_next((const unsigned char*)text,n,at,&cp,&width))return false;
  at+=width;
  if(cp>0xffff){if(count>INT32_MAX-2)return false;count+=2;}
  else{if(count==INT32_MAX)return false;++count;}
 }
 *units=count;return true;
}
/* Locate the UTF-8 sequence holding UTF-16 code unit `index`.  `trail` is
 * true when the index names the low half of a supplementary code point. */
static bool sb_unit_at(const char *text,size_t n,uint32_t index,size_t *start,size_t *width,uint32_t *cp,bool *trail){
 uint32_t unit=0;size_t at=0;
 while(at<n){
  uint32_t value;size_t w;
  if(!sb_unit_next((const unsigned char*)text,n,at,&value,&w))return false;
  uint32_t span=value>0xffff?2:1;
  if(index<unit+span){*start=at;*width=w;*cp=value;*trail=index!=unit;return true;}
  unit+=span;at+=w;
 }
 return false;
}
static void sb_encode_unit(uint32_t unit,char out[3]){
 out[0]=(char)(0xe0|(unit>>12));
 out[1]=(char)(0x80|((unit>>6)&63));
 out[2]=(char)(0x80|(unit&63));
}
/* Java StringBuilder.append(Object) is append(String.valueOf(object)): a null
 * object appends "null", and any other object contributes its toString()
 * result.  Resolve toString() on the receiver first so a dex override runs in
 * the interpreter; when the declaring class is a host stub (Throwable, an
 * enum, a boxed value) fall through to dvm__call_out(), the VM's external
 * boundary.  A missing method, a thrown exception or a non-String result is
 * never replaced by an empty string: the operation fails closed. */
static bool sb_object_text(struct dvm *v,dvm_ref value,const char **text,size_t *length){
 struct dvm_object *o=dvm__obj(v,value);if(!o)return false;
 const char *plain=dvm_string_utf8(v,value);
 if(plain){*text=plain;*length=dvm_string_utf8_length(v,value);return true;}
 struct dvm_method *method=dvm_find_method(v,o->cls,"toString","()Ljava/lang/String;");
 union dvm_value result={0};bool ok;
 if(method&&(method->has_code||method->builtin))ok=dvm_call(v,method,value,NULL,0,&result);
 else{
  /* Enum.toString is inherited from java/lang/Enum, which is the class the
   * host enum runtime answers for; every other receiver reports its own
   * class so a dex subclass of a host type resolves against its own chain. */
  struct dvm_class *decl=o->cls;
  for(struct dvm_class *c=o->cls;c;c=c->super)if(c->name&&!strcmp(c->name,"java/lang/Enum")){decl=c;break;}
  ok=dvm__call_out(v,decl,"toString","()Ljava/lang/String;",false,method&&(method->access&DEX_ACC_NATIVE),value,NULL,0,&result);
 }
 if(!ok||dvm_exception(v)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","StringBuilder.append(Object): toString unavailable");
  return false;
 }
 const char *resolved=dvm_string_utf8(v,result.l);
 if(!resolved){dvm__throw(v,"java/lang/ClassCastException","StringBuilder.append(Object): toString did not return java/lang/String");return false;}
 *text=resolved;*length=dvm_string_utf8_length(v,result.l);return true;
}
/* Decode the builder's UTF-8 into Java UTF-16 code units.  Supplementary
 * code points become a surrogate pair; unpaired 3-byte surrogates stay single
 * units.  Malformed storage is an internal error, never a partial result. */
static bool sb_decode_units(struct dvm *v,struct dvm_object *o,uint16_t **out,uint32_t *out_count){
 size_t n=o->utf8_len;const unsigned char *s=(const unsigned char*)o->utf8;
 uint16_t *units=malloc((n+1)*sizeof *units);if(!units){dvm__throw(v,"java/lang/OutOfMemoryError","StringBuilder units");return false;}
 uint32_t count=0;size_t at=0;
 while(at<n){
  uint32_t cp;size_t width;
  if(!sb_unit_next(s,n,at,&cp,&width)){free(units);dvm__throw(v,"java/lang/InternalError","StringBuilder internal UTF-8 is malformed");return false;}
  at+=width;
  if(cp>0xffff){
   if(count>UINT32_MAX-2){free(units);dvm__throw(v,"java/lang/OutOfMemoryError","StringBuilder units");return false;}
   uint32_t base=cp-0x10000;
   units[count++]=(uint16_t)(0xd800+(base>>10));
   units[count++]=(uint16_t)(0xdc00+(base&0x3ff));
  }else units[count++]=(uint16_t)cp;
 }
 *out=units;*out_count=count;return true;
}
/* Encode a UTF-16 slice as the VM's UTF-8 string storage.  A complete
 * surrogate pair becomes one 4-byte sequence; a lone surrogate (possible when
 * substring splits a pair) becomes its 3-byte form, matching append(char). */
static dvm_ref sb_units_to_string(struct dvm *v,const uint16_t *units,uint32_t count){
 size_t capacity=(size_t)count*4u+1u;
 if(capacity<count){dvm__throw(v,"java/lang/OutOfMemoryError","StringBuilder.substring");return 0;}
 char *buf=malloc(capacity);
 if(!buf){dvm__throw(v,"java/lang/OutOfMemoryError","StringBuilder.substring");return 0;}
 size_t at=0;
 for(uint32_t i=0;i<count;++i){
  uint32_t u=units[i];
  if(u>=0xd800&&u<=0xdbff&&i+1<count&&units[i+1]>=0xdc00&&units[i+1]<=0xdfff){
   uint32_t cp=0x10000+((u-0xd800)<<10)+(units[i+1]-0xdc00);++i;
   buf[at++]=(char)(0xf0|(cp>>18));
   buf[at++]=(char)(0x80|((cp>>12)&63));
   buf[at++]=(char)(0x80|((cp>>6)&63));
   buf[at++]=(char)(0x80|(cp&63));
  }else if(u<0x80)buf[at++]=(char)u;
  else if(u<0x800){buf[at++]=(char)(0xc0|(u>>6));buf[at++]=(char)(0x80|(u&63));}
  else{buf[at++]=(char)(0xe0|(u>>12));buf[at++]=(char)(0x80|((u>>6)&63));buf[at++]=(char)(0x80|(u&63));}
 }
 dvm_ref ref=dvm_new_string_n(v,buf,at);
 free(buf);
 if(!ref)dvm__throw(v,"java/lang/OutOfMemoryError","StringBuilder.substring");
 return ref;
}
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
  if(IS("maxMemory","()J")){out->j=(int64_t)vm_arena_limit();return true;}
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
 if(IS("length","()I")){
  uint32_t units=0;
  if(o->utf8_len&&(!o->utf8||!sb_utf16_units(o->utf8,o->utf8_len,&units))){
   sdk_trace_fprintf(stderr,"STANDARD_BUILDER_INVALID_UTF8 method=length bytes=%u\n",o->utf8_len);
   dvm__throw(v,"java/lang/InternalError","StringBuilder internal UTF-8 is malformed");return true;
  }
  out->i=(int32_t)units;return true;
 }
 if(IS("deleteCharAt","(I)Ljava/lang/StringBuilder;")){
  uint32_t units=0;
  if(o->utf8_len&&(!o->utf8||!sb_utf16_units(o->utf8,o->utf8_len,&units))){
   sdk_trace_fprintf(stderr,"STANDARD_BUILDER_INVALID_UTF8 method=deleteCharAt bytes=%u\n",o->utf8_len);
   dvm__throw(v,"java/lang/InternalError","StringBuilder internal UTF-8 is malformed");return true;
  }
  int32_t index=a[0].i;
  if(index<0||(uint32_t)index>=units){dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","deleteCharAt index %d length %u",index,units);return true;}
  size_t start=0,width=0;uint32_t cp=0;bool trail=false;
  if(!o->utf8||!sb_unit_at(o->utf8,o->utf8_len,(uint32_t)index,&start,&width,&cp,&trail)){
   sdk_trace_fprintf(stderr,"STANDARD_BUILDER_INVALID_UTF8 method=deleteCharAt index=%d bytes=%u\n",index,o->utf8_len);
   dvm__throw(v,"java/lang/InternalError","StringBuilder internal UTF-8 is malformed");return true;
  }
  char encoded[3];const char *insert=NULL;size_t insert_len=0;
  if(cp>0xffff){
   /* Removing one code unit splits the pair: keep the surviving surrogate in
    * the same three-byte form append(char) uses for unpaired surrogates. */
   uint32_t base=cp-0x10000;
   uint32_t keep=trail?(0xd800u+(base>>10)):(0xdc00u+(base&0x3ffu));
   sb_encode_unit(keep,encoded);insert=encoded;insert_len=3;
  }
  size_t have=o->utf8_len;
  size_t new_len=have-width+insert_len;
  memmove(o->utf8+start+insert_len,o->utf8+start+width,have-(start+width));
  if(insert_len)memcpy(o->utf8+start,insert,insert_len);
  o->utf8[new_len]='\0';o->utf8_len=(uint32_t)new_len;
  out->l=self;return true;
 }
 if(IS("substring","(II)Ljava/lang/String;")||IS("substring","(I)Ljava/lang/String;")){
  uint16_t *units=NULL;uint32_t count=0;
  if(!sb_decode_units(v,o,&units,&count))return true;
  int32_t start=a[0].i;
  int32_t end=IS("substring","(II)Ljava/lang/String;")?a[1].i:(int32_t)count;
  if(start<0||end<start||(uint32_t)end>count){
   free(units);
   dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","StringBuilder.substring start=%d end=%d length=%u",start,end,count);
   return true;
  }
  dvm_ref ref=sb_units_to_string(v,units+(uint32_t)start,(uint32_t)(end-start));
  free(units);
  if(!ref)return true;
  out->l=ref;return true;
 }
 if(!strncmp(m,"append",7)&&!strncmp(sig,"(",1)){
  char text[64];const char *bytes=NULL;size_t n=0;
  if(IS("append","(Ljava/lang/String;)Ljava/lang/StringBuilder;")){bytes=a[0].l?dvm_string_utf8(v,a[0].l):"null";if(!bytes)return false;n=a[0].l?dvm_string_utf8_length(v,a[0].l):4;}
  else if(IS("append","(I)Ljava/lang/StringBuilder;")){snprintf(text,sizeof text,"%d",a[0].i);bytes=text;n=strlen(text);}
  else if(IS("append","(J)Ljava/lang/StringBuilder;")){snprintf(text,sizeof text,"%"PRId64,a[0].j);bytes=text;n=strlen(text);}
  else if(IS("append","(Z)Ljava/lang/StringBuilder;")){bytes=a[0].i?"true":"false";n=strlen(bytes);}
  else if(IS("append","(C)Ljava/lang/StringBuilder;")){
   /* Java StringBuilder stores UTF-16 code units; this builder stores UTF-8,
    * so append(char) encodes the unit.  Unpaired surrogates are encoded as
    * their three-byte form and remain a documented string-storage boundary. */
   unsigned cp=(unsigned)(a[0].i&0xffff);char enc[3];size_t en=0;
   if(cp<0x80){enc[0]=(char)cp;en=1;}
   else if(cp<0x800){enc[0]=(char)(0xc0|(cp>>6));enc[1]=(char)(0x80|(cp&63));en=2;}
   else{enc[0]=(char)(0xe0|(cp>>12));enc[1]=(char)(0x80|((cp>>6)&63));enc[2]=(char)(0x80|(cp&63));en=3;}
   bytes=enc;n=en;
  }
  else if(IS("append","(Ljava/lang/Object;)Ljava/lang/StringBuilder;")){
   if(!a[0].l){bytes="null";n=4;}
   else if(!sb_object_text(v,a[0].l,&bytes,&n)){if(!dvm_exception(v))dvm__throw(v,"java/lang/InternalError","StringBuilder.append(Object) receiver");return true;}
  }
  else return false;
  sb_append(v,self,bytes,n);out->l=self;return true;
 }
 return false;
}
