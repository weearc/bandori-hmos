/* Stateful Java base-class contracts. No SDK/business code is substituted.
 * Java SE contract references are recorded in README.md. All calls occur under
 * the interpreter GIL. AtomicBoolean additionally uses C11 seq_cst atomics. */
#include "java_basics.h"
#include "uri_runtime.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>

extern int sdk_trace_fprintf(FILE*,const char*,...);
dvm_ref java_basics_boolean_box(struct dvm *v,int value);
enum {PAY_LIST=1,PAY_ITER,PAY_ATOMIC_BOOL,PAY_BOOLEAN,PAY_TLS,PAY_TIMEUNIT,PAY_ATOMIC_INT,PAY_FLOAT,PAY_DOUBLE,PAY_CHARACTER};
struct boxed_integer {int32_t value;};
struct boxed_long {int64_t value;};
static struct boxed_integer *boxed_integer_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,"java/lang/Integer")?(struct boxed_integer *)o->data:NULL;
}
/* Java Integer.toString(int[,int]) 的静态重载。
 * radix 2..36；负数输出前导 '-'（与 Integer.toString(-1,16)=="-1" 一致），
 * 不是补码无符号十六进制。INT32_MIN 用 int64_t 中间量避免取负溢出。 */
static bool integer_to_string_radix(struct dvm *v,int32_t value,int radix,union dvm_value *out){
 static const char digits[]="0123456789abcdefghijklmnopqrstuvwxyz";
 if(radix<2||radix>36){dvm__throw(v,"java/lang/IllegalArgumentException","radix out of range");return true;}
 char buf[35];char *p=buf+sizeof buf;
 *--p='\0';
 int64_t n=value;bool negative=n<0;uint64_t magnitude=negative?(uint64_t)(-n):(uint64_t)n;
 do{*--p=digits[magnitude%(uint32_t)radix];magnitude/=(uint32_t)radix;}while(magnitude);
 if(negative)*--p='-';
 size_t len=(size_t)(buf+sizeof buf-p-1);dvm_ref text=dvm_new_string_n(v,p,len);
 if(!text){dvm__throw(v,"java/lang/OutOfMemoryError","Integer.toString");return true;}
 out->l=text;return true;
}
static struct boxed_long *boxed_long_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,"java/lang/Long")?(struct boxed_long *)o->data:NULL;
}
/* Float/Double/Character use their own payload kinds and a dedicated
 * destructor.  The old shared struct payload is never a valid wrapper and the
 * kind check in each accessor keeps Boolean/AtomicBoolean payloads out. */
struct boxed_float {int kind;float value;};
struct boxed_double {int kind;double value;};
struct boxed_character {int kind;uint32_t value;};
static void boxed_destroy(void *data){free(data);}
static uint32_t float_bits(float value){uint32_t bits;memcpy(&bits,&value,sizeof bits);return bits;}
static uint64_t double_bits(double value){uint64_t bits;memcpy(&bits,&value,sizeof bits);return bits;}
/* Java floatToIntBits/doubleToLongBits canonicalise every NaN. */
static uint32_t float_to_int_bits(float value){
 uint32_t bits=float_bits(value);
 return (bits&0x7f800000u)==0x7f800000u&&(bits&0x007fffffu)?0x7fc00000u:bits;
}
static uint64_t double_to_long_bits(double value){
 uint64_t bits=double_bits(value);
 return (bits&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000)&&(bits&UINT64_C(0x000fffffffffffff))?UINT64_C(0x7ff8000000000000):bits;
}
static bool float_is_nan(float value){
 uint32_t bits=float_bits(value);return (bits&0x7f800000u)==0x7f800000u&&(bits&0x007fffffu)!=0;
}
static bool double_is_nan(double value){
 uint64_t bits=double_bits(value);return (bits&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000)&&(bits&UINT64_C(0x000fffffffffffff))!=0;
}
/* JLS 5.1.3: NaN becomes zero and out-of-range values saturate.  The C cast
 * alone is undefined outside the int range, so compare first. */
static int32_t java_float_to_int(float value){
 if(float_is_nan(value))return 0;
 if(value>=2147483648.0f)return INT32_MAX;
 if(value<=-2147483648.0f)return INT32_MIN;
 return (int32_t)value;
}
static int64_t java_float_to_long(float value){
 if(float_is_nan(value))return 0;
 if(value>=9223372036854775808.0f)return INT64_MAX;
 if(value<=-9223372036854775808.0f)return INT64_MIN;
 return (int64_t)value;
}
static int32_t java_double_to_int(double value){
 if(double_is_nan(value))return 0;
 if(value>=2147483648.0)return INT32_MAX;
 if(value<=-2147483649.0)return INT32_MIN;
 return (int32_t)value;
}
static int64_t java_double_to_long(double value){
 if(double_is_nan(value))return 0;
 if(value>=9223372036854775808.0)return INT64_MAX;
 if(value<=-9223372036854775808.0)return INT64_MIN;
 return (int64_t)value;
}
/* Java's shortest round-trip decimal, then its plain/scientific layout:
 * plain for 10^-3 <= |v| < 10^7 with a mandatory fractional digit, otherwise
 * one digit before the point and an unsigned exponent ("1.0E10"). */
static int java_decimal_text(double value,bool single,char *out,size_t cap){
 int limit=single?9:17,precision=limit;char sci[64];
 for(int p=1;p<=limit;++p){
  snprintf(sci,sizeof sci,"%.*e",p-1,value);
  bool same=single?float_bits(strtof(sci,NULL))==float_bits((float)value):double_bits(strtod(sci,NULL))==double_bits(value);
  if(same){precision=p;break;}
 }
 snprintf(sci,sizeof sci,"%.*e",precision-1,value);
 const char *s=sci;bool negative=false;
 if(*s=='-'){negative=true;++s;}
 char digits[24];size_t nd=0;
 while(*s&&*s!='e'&&*s!='E'){if(*s>='0'&&*s<='9'&&nd<sizeof digits)digits[nd++]=*s;++s;}
 while(nd>1&&digits[nd-1]=='0')--nd;
 int exp=0;
 if(*s=='e'||*s=='E'){++s;bool eneg=false;if(*s=='+')++s;else if(*s=='-'){eneg=true;++s;}while(*s>='0'&&*s<='9'){exp=exp*10+(*s-'0');++s;}if(eneg)exp=-exp;}
 size_t at=0;
 if(negative&&at<cap)out[at++]='-';
 if(exp>=-3&&exp<7){
  if(exp>=0){
   size_t before=(size_t)exp+1;
   if(before>=nd){
    for(size_t i=0;i<nd&&at<cap;++i)out[at++]=digits[i];
    for(size_t i=nd;i<before&&at<cap;++i)out[at++]='0';
    if(at<cap)out[at++]='.';
    if(at<cap)out[at++]='0';
   }else{
    for(size_t i=0;i<before&&at<cap;++i)out[at++]=digits[i];
    if(at<cap)out[at++]='.';
    for(size_t i=before;i<nd&&at<cap;++i)out[at++]=digits[i];
   }
  }else{
   if(at<cap)out[at++]='0';
   if(at<cap)out[at++]='.';
   for(int i=0;i<-exp-1&&at<cap;++i)out[at++]='0';
   for(size_t i=0;i<nd&&at<cap;++i)out[at++]=digits[i];
  }
 }else{
  if(at<cap)out[at++]=digits[0];
  if(at<cap)out[at++]='.';
  if(nd>1){for(size_t i=1;i<nd&&at<cap;++i)out[at++]=digits[i];}
  else if(at<cap)out[at++]='0';
  if(at<cap)out[at++]='E';
  if(exp<0){if(at<cap)out[at++]='-';exp=-exp;}
  char ebuf[8];int en=snprintf(ebuf,sizeof ebuf,"%d",exp);
  for(int i=0;i<en&&at<cap;++i)out[at++]=ebuf[i];
 }
 if(cap)out[at<cap?at:cap-1]='\0';
 return (int)at;
}
static int java_float_text(float value,char *out,size_t cap){
 uint32_t bits=float_bits(value);
 if((bits&0x7f800000u)==0x7f800000u)return snprintf(out,cap,"%s",(bits&0x007fffffu)?"NaN":(bits>>31?"-Infinity":"Infinity"));
 if((bits&0x7fffffffu)==0)return snprintf(out,cap,"%s",(bits>>31)?"-0.0":"0.0");
 return java_decimal_text((double)value,true,out,cap);
}
static int java_double_text(double value,char *out,size_t cap){
 uint64_t bits=double_bits(value);
 if((bits&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000))return snprintf(out,cap,"%s",(bits&UINT64_C(0x000fffffffffffff))?"NaN":((bits>>63)?"-Infinity":"Infinity"));
 if((bits&UINT64_C(0x7fffffffffffffff))==0)return snprintf(out,cap,"%s",(bits>>63)?"-0.0":"0.0");
 return java_decimal_text(value,false,out,cap);
}
static int java_character_text(uint32_t value,char *out,size_t cap){
 uint32_t cp=value&0xffffu;size_t n=0;
 if(cp<0x80){if(n<cap)out[n++]=(char)cp;}
 else if(cp<0x800){if(n<cap)out[n++]=(char)(0xc0|(cp>>6));if(n<cap)out[n++]=(char)(0x80|(cp&0x3f));}
 else{if(n<cap)out[n++]=(char)(0xe0|(cp>>12));if(n<cap)out[n++]=(char)(0x80|((cp>>6)&0x3f));if(n<cap)out[n++]=(char)(0x80|(cp&0x3f));}
 if(cap)out[n<cap?n:cap-1]='\0';return (int)n;
}
/* Java Character.isWhitespace(char)：Unicode 空白类 Zs/Zl/Zp 中排除三个
 * 非断行空格（U+00A0、U+2007、U+202F），再加上 U+0009..U+000D 和
 * U+001C..U+001F。char 参数只有 16 位，故这里按 BMP 值判断。 */
static bool java_character_is_whitespace(uint32_t ch){
 return (ch>=0x0009&&ch<=0x000d)||(ch>=0x001c&&ch<=0x001f)||ch==0x0020||
        ch==0x1680||(ch>=0x2000&&ch<=0x2006)||(ch>=0x2008&&ch<=0x200a)||
        ch==0x2028||ch==0x2029||ch==0x205f||ch==0x3000;
}
/* Java Character.isSpaceChar(char)：Unicode 类别 Zs/Zl/Zp，包含
 * 非断行空格 U+00A0、U+2007、U+202F；U+200B（Cf）不属于空白类。 */
static bool java_character_is_space_char(uint32_t ch){
 return ch==0x0020||ch==0x00a0||ch==0x1680||(ch>=0x2000&&ch<=0x200a)||
        ch==0x2028||ch==0x2029||ch==0x202f||ch==0x205f||ch==0x3000;
}
/* Java Character.isISOControl(int)：仅 U+0000..U+001F 与 U+007F..U+009F。
 * int 重载接收完整 code point，不能截断成低 16 位（0x10020 必须为 false）；
 * 负数按 uint32_t 解释后远大于 U+009F，同样为 false，与 Java 实测一致。
 * char 重载由调用方先截成 16 位 char 再传入。 */
static bool java_character_is_iso_control(uint32_t cp){
 return (cp>=0x0000u&&cp<=0x001fu)||(cp>=0x007fu&&cp<=0x009fu);
}
static struct boxed_float *boxed_float_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Float")||o->data_destroy!=boxed_destroy)return NULL;
 struct boxed_float *p=o->data;return p&&p->kind==PAY_FLOAT?p:NULL;
}
static struct boxed_double *boxed_double_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Double")||o->data_destroy!=boxed_destroy)return NULL;
 struct boxed_double *p=o->data;return p&&p->kind==PAY_DOUBLE?p:NULL;
}
static struct boxed_character *boxed_character_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Character")||o->data_destroy!=boxed_destroy)return NULL;
 struct boxed_character *p=o->data;return p&&p->kind==PAY_CHARACTER?p:NULL;
}
static struct boxed_float *require_boxed_float(struct dvm *v,dvm_ref ref,const char *where){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","%s receiver",where);return NULL;}
 struct boxed_float *p=boxed_float_of(v,ref);
 if(!p)dvm__throw(v,"java/lang/IllegalStateException","%s receiver is not an initialized %s",where,where);
 return p;
}
static struct boxed_double *require_boxed_double(struct dvm *v,dvm_ref ref,const char *where){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","%s receiver",where);return NULL;}
 struct boxed_double *p=boxed_double_of(v,ref);
 if(!p)dvm__throw(v,"java/lang/IllegalStateException","%s receiver is not an initialized %s",where,where);
 return p;
}
static struct boxed_character *require_boxed_character(struct dvm *v,dvm_ref ref,const char *where){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","%s receiver",where);return NULL;}
 struct boxed_character *p=boxed_character_of(v,ref);
 if(!p)dvm__throw(v,"java/lang/IllegalStateException","%s receiver is not an initialized %s",where,where);
 return p;
}
static bool install_boxed(struct dvm *v,dvm_ref ref,const char *cls_name,const char *where,void *data){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","%s receiver",where);free(data);return false;}
 if(!o->cls||!o->cls->name||strcmp(o->cls->name,cls_name)){dvm__throw(v,"java/lang/IllegalStateException","%s receiver type mismatch",where);free(data);return false;}
 if(o->data){if(o->data_destroy)o->data_destroy(o->data);else free(o->data);}
 o->data=data;o->data_destroy=boxed_destroy;return true;
}
static dvm_ref new_boxed_float(struct dvm *v,float value){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Float");
 if(!cls){dvm__throw(v,"java/lang/NoClassDefFoundError","java/lang/Float");return 0;}
 dvm_ref ref=dvm_new_object(v,cls);
 if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Float");return 0;}
 struct boxed_float *box=calloc(1,sizeof *box);
 if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Float payload");return 0;}
 box->kind=PAY_FLOAT;box->value=value;
 struct dvm_object *o=dvm__obj(v,ref);o->data=box;o->data_destroy=boxed_destroy;return ref;
}
static dvm_ref new_boxed_double(struct dvm *v,double value){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Double");
 if(!cls){dvm__throw(v,"java/lang/NoClassDefFoundError","java/lang/Double");return 0;}
 dvm_ref ref=dvm_new_object(v,cls);
 if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Double");return 0;}
 struct boxed_double *box=calloc(1,sizeof *box);
 if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Double payload");return 0;}
 box->kind=PAY_DOUBLE;box->value=value;
 struct dvm_object *o=dvm__obj(v,ref);o->data=box;o->data_destroy=boxed_destroy;return ref;
}
static dvm_ref new_boxed_character(struct dvm *v,uint32_t value){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Character");
 if(!cls){dvm__throw(v,"java/lang/NoClassDefFoundError","java/lang/Character");return 0;}
 dvm_ref ref=dvm_new_object(v,cls);
 if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Character");return 0;}
 struct boxed_character *box=calloc(1,sizeof *box);
 if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Character payload");return 0;}
 box->kind=PAY_CHARACTER;box->value=value&0xffffu;
 struct dvm_object *o=dvm__obj(v,ref);o->data=box;o->data_destroy=boxed_destroy;return ref;
}
struct snapshot {size_t owners,n;dvm_ref values[];};
struct tls_entry {pthread_t thread;dvm_ref value;struct tls_entry *next;};
struct payload {int kind;struct snapshot *s;size_t pos;_Atomic bool flag;_Atomic int32_t number;struct tls_entry *tls;uint64_t unit_nanos;};
static struct snapshot *snapshot_new(size_t n) {
 if(n>1000000)return NULL;
 struct snapshot *s=calloc(1,sizeof(*s)+n*sizeof(dvm_ref));
 if(s){s->owners=1;s->n=n;}return s;
}
static void snapshot_release(struct snapshot *s){if(s&&!--s->owners)free(s);}
static void destroy(void *data){
 struct payload *p=data;snapshot_release(p->s);
 while(p->tls){struct tls_entry *e=p->tls;p->tls=e->next;free(e);}free(p);
}
static struct payload *payload(struct dvm *v,dvm_ref ref,int kind,bool create){
 struct dvm_object *o=dvm__obj(v,ref);if(!o){dvm__throw(v,"java/lang/NullPointerException","base class receiver");return NULL;}
 struct payload *p=o->data;
 if(!p&&create){p=calloc(1,sizeof *p);if(p){p->kind=kind;atomic_init(&p->flag,false);atomic_init(&p->number,0);o->data=p;o->data_destroy=destroy;}}
 if(!p){dvm__throw(v,"java/lang/IllegalStateException","base class constructor not called");return NULL;}
 if(p->kind!=kind){dvm__throw(v,"java/lang/IllegalStateException","base class payload kind mismatch");return NULL;}
 return p;
}
/* Boolean and AtomicBoolean share a mutable flag but are unrelated Java types. */
static struct payload *boolean_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Boolean")||o->data_destroy!=destroy)return NULL;
 struct payload *p=o->data;return p&&p->kind==PAY_BOOLEAN?p:NULL;
}
bool java_basics_equal(struct dvm *v,dvm_ref a,dvm_ref b){
 if(a==b)return true;if(!a||!b)return false;
 struct dvm_object *x=dvm__obj(v,a),*y=dvm__obj(v,b);
 if(x&&y&&x->kind==DVM_OBJ_STRING&&y->kind==DVM_OBJ_STRING)return x->utf8_len==y->utf8_len&&!memcmp(x->utf8,y->utf8,x->utf8_len);
 /* java.lang.Class has identity equality.  The VM may materialise the same
  * class through separate Class objects, so compare the underlying class. */
 if(x&&y&&x->kind==DVM_OBJ_CLASS&&y->kind==DVM_OBJ_CLASS)
  return x->klass&&y->klass&&(x->klass==y->klass||!strcmp(x->klass->desc,y->klass->desc));
 struct boxed_integer *ix=boxed_integer_of(v,a),*iy=boxed_integer_of(v,b);
 if(ix||iy)return ix&&iy&&ix->value==iy->value;
 struct payload *bx=boolean_of(v,a),*by=boolean_of(v,b);
 if(bx||by)return bx&&by&&(atomic_load(&bx->flag)!=0)==(atomic_load(&by->flag)!=0);
 struct boxed_float *fx=boxed_float_of(v,a),*fy=boxed_float_of(v,b);
 if(fx||fy)return fx&&fy&&float_to_int_bits(fx->value)==float_to_int_bits(fy->value);
 struct boxed_double *dx=boxed_double_of(v,a),*dy=boxed_double_of(v,b);
 if(dx||dy)return dx&&dy&&double_to_long_bits(dx->value)==double_to_long_bits(dy->value);
 struct boxed_character *cx=boxed_character_of(v,a),*cy=boxed_character_of(v,b);
 if(cx||cy)return cx&&cy&&cx->value==cy->value;
 struct dvm_method *m=x?dvm_find_method(v,x->cls,"equals","(Ljava/lang/Object;)Z"):NULL;
 if(m){union dvm_value arg={.l=b},r={0};return dvm_call(v,m,a,&arg,1,&r)&&r.i;}
 if(x&&x->cls&&x->cls->external&&strcmp(x->cls->name,"java/lang/Object")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","equals contract not implemented for external class %s",x->cls->name);
 }
 /* java.lang.Object.equals defaults to reference identity, already tested. */
 return false;
}
/* java.util.Locale.US / Locale.ROOT / Locale.ENGLISH are external static
 * fields: the DEX uses sget-object and the VM field resolver has no field
 * table for the external class, so it reaches hooks.get_external_static ->
 * get_static -> java_basics_static.  Each constant is a genuine
 * java/util/Locale object with language/country data, cached and pinned like
 * Boolean.TRUE/FALSE.  These are deliberately distinct from
 * platform_runtime.c's Locale.getDefault singleton, whose object carries no
 * locale_value payload. */
struct locale_value {char language[16];char country[16];};
static void locale_destroy(void *data){free(data);}
static struct dvm *locale_statics_vm;
static dvm_ref locale_statics[3]; /* 0 = US, 1 = ROOT, 2 = ENGLISH */
static struct locale_value *locale_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/util/Locale")||o->data_destroy!=locale_destroy)return NULL;
 return o->data;
}
static struct dvm *boolean_statics_vm;
static dvm_ref boolean_statics[2];
/* java/io/File 的静态常量。Android/Linux 上 separator="/"、pathSeparator=":"；
 * separatorChar/pathSeparatorChar 是 char 字段，直接返回字符值，不经过对象。
 * 字符串常量按 VM 缓存并 pin，理由与 Boolean.TRUE/FALSE 相同。 */
static struct dvm *file_statics_vm;
static dvm_ref file_statics[2]; /* 0 = separator, 1 = pathSeparator */
/* java.net.Proxy.NO_PROXY and Proxy.Type.DIRECT are real cached objects; the
 * NO_PROXY instance answers type() with the same DIRECT object that the DEX
 * reads through the static field. */
static struct dvm *proxy_statics_vm;
static dvm_ref proxy_none,proxy_direct,proxy_http,proxy_socks;
/* Called from sdk_init_vm_start on every new VM so cached singletons from a
 * destroyed VM can never be observed by a later VM that reuses the address. */
void java_basics_reset(struct dvm *v){
 (void)v;boolean_statics_vm=NULL;boolean_statics[0]=boolean_statics[1]=0;
 locale_statics_vm=NULL;locale_statics[0]=locale_statics[1]=locale_statics[2]=0;
 file_statics_vm=NULL;file_statics[0]=file_statics[1]=0;
 proxy_statics_vm=NULL;proxy_none=proxy_direct=proxy_http=proxy_socks=0;
}
bool java_basics_static(struct dvm *v,const char *c,const char *f,const char *t,union dvm_value *out){
 if(!strcmp(c,"java/lang/Boolean")&&!strcmp(t,"Ljava/lang/Boolean;")&&(!strcmp(f,"TRUE")||!strcmp(f,"FALSE"))){
  if(boolean_statics_vm!=v){boolean_statics_vm=v;boolean_statics[0]=boolean_statics[1]=0;}
  dvm_ref *slot=&boolean_statics[!strcmp(f,"TRUE")?0:1];
  if(!*slot){
   struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Boolean");
   if(!cls){dvm__throw(v,"java/lang/NoClassDefFoundError","java/lang/Boolean");return true;}
   dvm_ref ref=dvm_new_object(v,cls);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Boolean static constant");return true;}
   struct payload *p=payload(v,ref,PAY_BOOLEAN,true);if(!p){dvm__discard_temporary(v,ref);return true;}
   atomic_store(&p->flag,!strcmp(f,"TRUE"));
   dvm_pin(v,ref);*slot=ref;
  }
  out->l=*slot;return true;
 }
 if(!strcmp(c,"java/util/concurrent/TimeUnit")&&!strcmp(t,"Ljava/util/concurrent/TimeUnit;")){
  static const char *names[]={"NANOSECONDS","MICROSECONDS","MILLISECONDS","SECONDS","MINUTES","HOURS","DAYS"};
  static const uint64_t scales[]={1,1000,1000000,1000000000,60000000000ULL,3600000000000ULL,86400000000000ULL};
  int index=-1;for(int i=0;i<7;i++)if(!strcmp(f,names[i]))index=i;if(index<0)return false;
  struct dvm_class *unit=dvm_find_class_or_external(v,"java/util/concurrent/TimeUnit");
  if(!unit){dvm__throw(v,"java/lang/NoClassDefFoundError","java/util/concurrent/TimeUnit");return true;}
  out->l=dvm_new_object(v,unit);if(!out->l){dvm__throw(v,"java/lang/OutOfMemoryError","TimeUnit.SECONDS");return true;}
  struct payload *p=payload(v,out->l,PAY_TIMEUNIT,true);if(!p)return true;p->unit_nanos=scales[index];
  return true;
 }
 if(!strcmp(c,"java/util/Locale")&&!strcmp(t,"Ljava/util/Locale;")&&(!strcmp(f,"US")||!strcmp(f,"ROOT")||!strcmp(f,"ENGLISH"))){
  if(locale_statics_vm!=v){locale_statics_vm=v;locale_statics[0]=locale_statics[1]=locale_statics[2]=0;}
  int index=!strcmp(f,"US")?0:!strcmp(f,"ROOT")?1:2;
  dvm_ref *slot=&locale_statics[index];
  if(!*slot){
   struct dvm_class *cls=dvm_find_class_or_external(v,"java/util/Locale");
   if(!cls){dvm__throw(v,"java/lang/NoClassDefFoundError","java/util/Locale");return true;}
   dvm_ref ref=dvm_new_object(v,cls);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Locale static constant");return true;}
   struct locale_value *value=calloc(1,sizeof *value);
   if(!value){dvm__discard_temporary(v,ref);dvm__throw(v,"java/lang/OutOfMemoryError","Locale static constant");return true;}
   if(index!=1)memcpy(value->language,"en",3); /* US and ENGLISH are both en */
   if(!index)memcpy(value->country,"US",3);
   struct dvm_object *object=dvm__obj(v,ref);
   object->data=value;object->data_destroy=locale_destroy;
   dvm_pin(v,ref);*slot=ref;
  }
  out->l=*slot;return true;
 }
 if(!strcmp(c,"java/net/Proxy")||!strcmp(c,"java/net/Proxy$Type")){
  if(proxy_statics_vm!=v){proxy_statics_vm=v;proxy_none=proxy_direct=0;}
  const char *cls_name=NULL;
  if(!strcmp(c,"java/net/Proxy")&&!strcmp(f,"NO_PROXY")&&!strcmp(t,"Ljava/net/Proxy;")){cls_name="java/net/Proxy";if(!proxy_none){proxy_none=dvm_new_object(v,dvm_find_class_or_external(v,cls_name));if(!proxy_none){dvm__throw(v,"java/lang/OutOfMemoryError","Proxy.NO_PROXY");return true;}}out->l=proxy_none;return true;}
  if(!strcmp(c,"java/net/Proxy$Type")&&!strcmp(t,"Ljava/net/Proxy$Type;")){
   dvm_ref *slot=NULL;const char *label=NULL;
   if(!strcmp(f,"DIRECT")){slot=&proxy_direct;label="Proxy.Type.DIRECT";}
   else if(!strcmp(f,"HTTP")){slot=&proxy_http;label="Proxy.Type.HTTP";}
   else if(!strcmp(f,"SOCKS")){slot=&proxy_socks;label="Proxy.Type.SOCKS";}
   if(!slot)return false;
   if(!*slot){*slot=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Proxy$Type"));if(!*slot){dvm__throw(v,"java/lang/OutOfMemoryError",label);return true;}}
   out->l=*slot;return true;
  }
  return false;
 }
 /* java/io/File 的静态字段走同一条 external-static 钩子。只接受确切的字段名
  * 与类型；其它 File 字段返回 false，由调用方按缺失静态字段处理（fail
  * closed），不能凭空造值。字符串字段返回真实 java/lang/String 对象，
  * 字符字段返回 Java char 值（union 的 .i 槽）。 */
 if(!strcmp(c,"java/io/File")){
  const char *separator=NULL;
  if(!strcmp(t,"Ljava/lang/String;")&&(!strcmp(f,"separator")||!strcmp(f,"pathSeparator")))
   separator=!strcmp(f,"separator")?"/":":";
  else if(!strcmp(t,"C")&&(!strcmp(f,"separatorChar")||!strcmp(f,"pathSeparatorChar"))){
   out->i=!strcmp(f,"separatorChar")?'/':':';return true;
  } else return false;
  if(file_statics_vm!=v){file_statics_vm=v;file_statics[0]=file_statics[1]=0;}
  int index=!strcmp(f,"pathSeparator")?1:0;
  if(!file_statics[index]){
   dvm_ref ref=dvm_new_string(v,separator);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","File static string");return true;}
   dvm_pin(v,ref);file_statics[index]=ref;
  }
  out->l=file_statics[index];return true;
 }
 /* android.content.Context.MODE_PRIVATE is a real Java static int (0), the
  * mode passed to getSharedPreferences.  Unity's native PlayerPrefs upgrade
  * reads it through JNI (handled by the bridge's external-constant path);
  * answering the DEX sget path here keeps both views identical.  Other
  * Context statics remain missing and fail closed. */
 if(!strcmp(c,"android/content/Context")&&!strcmp(f,"MODE_PRIVATE")&&!strcmp(t,"I")){
  out->i=0;return true;
 }
 /* Settings.System/Global/Secure.CONTENT_URI are real content://settings/<domain>
  * Uri objects.  UnityPlayer's audio/observer handlers register observers on
  * these constants.  Any other Settings static stays a missing field. */
 if(!strcmp(c,"android/provider/Settings$System")||!strcmp(c,"android/provider/Settings$Global")||!strcmp(c,"android/provider/Settings$Secure")){
  static struct dvm *settings_uri_vm;static dvm_ref settings_uri[3];
  if(strcmp(f,"CONTENT_URI")||strcmp(t,"Landroid/net/Uri;"))return false;
  int index=!strcmp(c,"android/provider/Settings$System")?0:!strcmp(c,"android/provider/Settings$Global")?1:2;
  if(settings_uri_vm!=v){settings_uri_vm=v;settings_uri[0]=settings_uri[1]=settings_uri[2]=0;}
  if(!settings_uri[index]){
   const char *domain=index==0?"system":index==1?"global":"secure";
   char text[64];int length=snprintf(text,sizeof text,"content://settings/%s",domain);
   if(length<0||(size_t)length>=sizeof text)return false;
   dvm_ref text_ref=dvm_new_string(v,text);
   if(!text_ref){dvm__throw(v,"java/lang/OutOfMemoryError","Settings.CONTENT_URI");return true;}
   union dvm_value arg={.l=text_ref},parsed={0};
   if(!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&parsed)){
    if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Uri runtime unavailable");
    return true;
   }
   if(!parsed.l){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Uri.parse returned null");return true;}
   dvm_pin(v,parsed.l);settings_uri[index]=parsed.l;
  }
  out->l=settings_uri[index];return true;
 }
 if(strcmp(f,"TYPE")||strcmp(t,"Ljava/lang/Class;"))return false;
 static const struct {const char *name,*desc;} types[]={
  {"java/lang/Void","V"},{"java/lang/Boolean","Z"},{"java/lang/Byte","B"},{"java/lang/Character","C"},{"java/lang/Short","S"},{"java/lang/Integer","I"},{"java/lang/Long","J"},{"java/lang/Float","F"},{"java/lang/Double","D"}};
 for(size_t i=0;i<sizeof(types)/sizeof(types[0]);++i)if(!strcmp(c,types[i].name)){
  out->l=dvm_class_object(v,dvm__define_primitive(v,types[i].desc));return true;
 }
 return false;
}
/* Shared factory for Boolean.TRUE/FALSE and Boolean.valueOf(Z). */
dvm_ref java_basics_boolean_box(struct dvm *v,int value){
 union dvm_value out={0};
 if(!java_basics_static(v,"java/lang/Boolean",value?"TRUE":"FALSE","Ljava/lang/Boolean;",&out)||!out.l){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Boolean static constant");
  return 0;
 }
 return out.l;
}
bool java_basics_timeunit_nanos(struct dvm *v,dvm_ref unit,uint64_t *nanos){
 struct dvm_object *o=dvm__obj(v,unit);struct payload *p=o?o->data:NULL;
 if(!o||o->data_destroy!=destroy||!p||p->kind!=PAY_TIMEUNIT)return false;
 *nanos=p->unit_nanos;return *nanos!=0;
}
/* java.util.Arrays.toString(Object[]) needs the same element text Java's
 * StringBuilder.append(Object) produces: null -> "null", a String's contents,
 * otherwise the element's real toString().  A failure inside that toString is
 * left pending, and an element whose class has no callable toString() fails
 * closed instead of substituting a class name. */
struct arrays_text {
 char *data;
 size_t len;
 size_t capacity;
};
static bool arrays_text_reserve(struct dvm *v,struct arrays_text *t,size_t extra){
 if(extra>UINT32_MAX||t->len>UINT32_MAX-extra){
  dvm__throw(v,"java/lang/OutOfMemoryError","Arrays.toString result");
  return false;
 }
 size_t need=t->len+extra;
 if(need<=t->capacity)return true;
 size_t capacity=t->capacity?t->capacity:64;
 while(capacity<need){
  size_t next=capacity*2;
  if(next<capacity||next>UINT32_MAX){capacity=need;break;}
  capacity=next;
 }
 if(capacity>UINT32_MAX)capacity=UINT32_MAX;
 char *data=realloc(t->data,capacity);
 if(!data){dvm__throw(v,"java/lang/OutOfMemoryError","Arrays.toString buffer");return false;}
 t->data=data;t->capacity=capacity;
 return true;
}
static bool arrays_text_append(struct dvm *v,struct arrays_text *t,const char *text,size_t length){
 if(!arrays_text_reserve(v,t,length))return false;
 if(length)memcpy(t->data+t->len,text,length);
 t->len+=length;
 return true;
}
static bool arrays_element_text(struct dvm *v,dvm_ref ref,const char **text,size_t *length){
 if(!ref){*text="null";*length=4;return true;}
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o){dvm__throw(v,"java/lang/IllegalStateException","Arrays.toString element handle");return false;}
 if(o->kind==DVM_OBJ_STRING){
  if(!o->utf8){dvm__throw(v,"java/lang/IllegalStateException","Arrays.toString String element");return false;}
  *text=o->utf8;*length=o->utf8_len;return true;
 }
 const char *cls=o->cls&&o->cls->name?o->cls->name:NULL;
 if(!cls){dvm__throw(v,"java/lang/IllegalStateException","Arrays.toString element class");return false;}
 union dvm_value result={0};
 bool handled=java_basics_call(v,cls,"toString","()Ljava/lang/String;",ref,NULL,0,&result);
 if(dvm_exception(v))return false;
 if(!handled){
  struct dvm_method *method=dvm_find_method(v,o->cls,"toString","()Ljava/lang/String;");
  if(method&&(method->has_code||method->builtin)){
   if(!dvm_call(v,method,ref,NULL,0,&result))return false;
   handled=true;
  }
 }
 if(dvm_exception(v))return false;
 if(!handled){
  dvm__throw(v,"java/lang/UnsupportedOperationException","Arrays.toString element %s has no toString",cls);
  return false;
 }
 if(!result.l){*text="null";*length=4;return true;}
 struct dvm_object *rendered=dvm__obj(v,result.l);
 if(!rendered||rendered->kind!=DVM_OBJ_STRING||!rendered->utf8){
  dvm__throw(v,"java/lang/ClassCastException","Arrays.toString element toString did not return a String");
  return false;
 }
 *text=rendered->utf8;*length=rendered->utf8_len;
 return true;
}
static bool arrays_to_string(struct dvm *v,dvm_ref array,union dvm_value *out){
 if(!array){
  out->l=dvm_new_string(v,"null");
  if(!out->l){dvm__throw(v,"java/lang/OutOfMemoryError","Arrays.toString null");return true;}
  return true;
 }
 struct dvm_object *arr=dvm__obj(v,array);
 if(!arr||arr->kind!=DVM_OBJ_ARRAY||(arr->elem_kind!='L'&&arr->elem_kind!='[')){
  dvm__throw(v,"java/lang/ClassCastException","Arrays.toString requires an Object array");
  return true;
 }
 uint32_t count=arr->length;
 dvm_ref *items=count?(dvm_ref*)dvm_array_data(v,array):NULL;
 if(count&&!items){dvm__throw(v,"java/lang/IllegalStateException","Arrays.toString array data");return true;}
 struct arrays_text text={0};
 bool ok=true;
 if(!arrays_text_append(v,&text,"[",1))ok=false;
 for(uint32_t i=0;ok&&i<count;i++){
  if(i&&!arrays_text_append(v,&text,", ",2)){ok=false;break;}
  const char *element=NULL;size_t length=0;
  if(!arrays_element_text(v,items[i],&element,&length)){ok=false;break;}
  if(!arrays_text_append(v,&text,element,length)){ok=false;break;}
 }
 if(ok&&!arrays_text_append(v,&text,"]",1))ok=false;
 if(ok){
  dvm_ref rendered=dvm_new_string_n(v,text.data,text.len);
  if(!rendered)dvm__throw(v,"java/lang/OutOfMemoryError","Arrays.toString result");
  else out->l=rendered;
 }
 free(text.data);
 return true;
}
#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
bool java_basics_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;memset(out,0,sizeof *out);
 /* Virtual calls declared on Number/Object/Comparable carry no method table
  * for an external concrete class.  Re-dispatch the wrapper methods through
  * the receiver's actual wrapper class; static entry points keep the declared
  * class and are not affected.  Byte/Short are handled by small_boxes_call,
  * which runs before this module.  Integer/Long/AtomicInteger are Number
  * subclasses, so Number.intValue()/longValue()/doubleValue() declared calls
  * reach the concrete implementation only through this re-dispatch. */
 if(self&&(!strcmp(c,"java/lang/Number")||!strcmp(c,"java/lang/Object")||!strcmp(c,"java/lang/Comparable"))&&strcmp(m,"<init>")&&strcmp(m,"valueOf")){
  struct dvm_object *receiver=dvm__obj(v,self);
  const char *actual=receiver&&receiver->cls?receiver->cls->name:NULL;
  if(actual&&(!strcmp(actual,"java/lang/Integer")||!strcmp(actual,"java/lang/Long")||
              !strcmp(actual,"java/lang/Float")||!strcmp(actual,"java/lang/Double")||
              !strcmp(actual,"java/lang/Character")||!strcmp(actual,"java/lang/Boolean")||
              !strcmp(actual,"java/util/concurrent/atomic/AtomicInteger")))
   return java_basics_call(v,actual,m,sig,self,a,nargs,out);
 }
 if(!strcmp(c,"java/util/Arrays")&&!self&&IS("toString","([Ljava/lang/Object;)Ljava/lang/String;")&&nargs==1){
  if(!a)return false;
  return arrays_to_string(v,a[0].l,out);
 }
 /* Locale constants created by java_basics_static.  A Locale object from
  * platform_runtime.c's getDefault singleton has no locale_value payload and
  * is left to platform_runtime_call (which runs earlier in strict_runtime.c);
  * this branch returns false for it rather than guessing a locale. */
 if(!strcmp(c,"java/util/Locale")&&(IS("getLanguage","()Ljava/lang/String;")||IS("getCountry","()Ljava/lang/String;")||IS("toString","()Ljava/lang/String;"))){
  struct locale_value *value=locale_of(v,self);
  if(value){
   if(IS("getLanguage","()Ljava/lang/String;"))out->l=dvm_new_string(v,value->language);
   else if(IS("getCountry","()Ljava/lang/String;"))out->l=dvm_new_string(v,value->country);
   else if(!value->language[0])out->l=dvm_new_string(v,"");
   else {char text[sizeof value->language+sizeof value->country+2];snprintf(text,sizeof text,"%s_%s",value->language,value->country);out->l=dvm_new_string(v,text);}
   if(out->l)return true;
   dvm__throw(v,"java/lang/OutOfMemoryError","Locale result");return true;
  }
 }
 if(!strcmp(c,"java/util/concurrent/TimeUnit")&&nargs==1&&!strcmp(sig,"(J)J")){
  static const char *methods[]={"toNanos","toMicros","toMillis","toSeconds","toMinutes","toHours","toDays"};
  static const uint64_t divisors[]={1,1000,1000000,1000000000,60000000000ULL,3600000000000ULL,86400000000000ULL};
  int index=-1;for(int i=0;i<7;i++)if(!strcmp(m,methods[i]))index=i;
  if(index<0)return false;uint64_t scale;if(!java_basics_timeunit_nanos(v,self,&scale))return false;
  __int128 result=(__int128)a[0].j*scale/divisors[index];out->j=result>INT64_MAX?INT64_MAX:result<INT64_MIN?INT64_MIN:(int64_t)result;return true;
 }
 if(!strcmp(c,"java/util/concurrent/atomic/AtomicInteger")){
  bool ctor=IS("<init>","()V")||IS("<init>","(I)V");
  bool get=IS("get","()I")||IS("intValue","()I");
  bool set=IS("set","(I)V")||IS("lazySet","(I)V");
  bool cas=IS("compareAndSet","(II)Z")||IS("weakCompareAndSet","(II)Z");
  bool exchange=IS("getAndSet","(I)I");
  bool inc=IS("getAndIncrement","()I")||IS("incrementAndGet","()I");
  bool dec=IS("getAndDecrement","()I")||IS("decrementAndGet","()I");
  bool add=IS("getAndAdd","(I)I")||IS("addAndGet","(I)I");
  bool text=IS("toString","()Ljava/lang/String;");
  if(!ctor&&!get&&!set&&!cas&&!exchange&&!inc&&!dec&&!add&&!text)return false;
  struct payload *p=payload(v,self,PAY_ATOMIC_INT,ctor);if(!p)return true;
  if(ctor){atomic_store(&p->number,IS("<init>","(I)V")?a[0].i:0);return true;}
  if(get)out->i=atomic_load(&p->number);
  else if(set)atomic_store(&p->number,a[0].i);
  else if(cas){int32_t expected=a[0].i;out->i=atomic_compare_exchange_strong(&p->number,&expected,a[1].i);}
  else if(exchange)out->i=atomic_exchange(&p->number,a[0].i);
  else if(inc||dec||add){
   int32_t delta=inc?1:dec?-1:a[0].i;int32_t old=atomic_fetch_add(&p->number,delta);
   bool after=IS("incrementAndGet","()I")||IS("decrementAndGet","()I")||IS("addAndGet","(I)I");
   out->i=after?(int32_t)((uint32_t)old+(uint32_t)delta):old;
  }else{char buf[24];snprintf(buf,sizeof buf,"%"PRId32,atomic_load(&p->number));out->l=dvm_new_string(v,buf);}
  return true;
 }
 if(!strcmp(c,"java/lang/String")&&!strcmp(m,"valueOf")&&nargs==1){
  char text[32];
  if(!strcmp(sig,"(I)Ljava/lang/String;"))snprintf(text,sizeof text,"%"PRId32,a[0].i);
  else if(!strcmp(sig,"(J)Ljava/lang/String;"))snprintf(text,sizeof text,"%"PRId64,a[0].j);
  else if(!strcmp(sig,"(Z)Ljava/lang/String;"))snprintf(text,sizeof text,"%s",a[0].i?"true":"false");
  else return false;
  out->l=dvm_new_string(v,text);return true;
 }
 if(!strcmp(c,"java/lang/Boolean")){
  bool value_of=IS("valueOf","(Z)Ljava/lang/Boolean;");
  bool ctor=IS("<init>","(Z)V");
  bool value=IS("booleanValue","()Z");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool hash=IS("hashCode","()I");
  bool tostring=IS("toString","()Ljava/lang/String;");
  if(!value_of&&!ctor&&!value&&!equals&&!hash&&!tostring)return false;
  if(value_of){out->l=java_basics_boolean_box(v,a[0].i!=0);return true;}
  struct payload *p=payload(v,self,PAY_BOOLEAN,ctor);if(!p)return true;
  if(ctor){atomic_store(&p->flag,a[0].i!=0);return true;}
  if(value){out->i=atomic_load(&p->flag);return true;}
  if(hash){out->i=atomic_load(&p->flag)?1231:1237;return true;}
  if(equals){
   struct payload *q=boolean_of(v,a[0].l);
   out->i=q&&(atomic_load(&q->flag)!=0)==(atomic_load(&p->flag)!=0);return true;
  }
  out->l=dvm_new_string(v,atomic_load(&p->flag)?"true":"false");return true;
 }
 if(!strcmp(c,"java/net/Proxy")&&(IS("equals","(Ljava/lang/Object;)Z")||IS("hashCode","()I"))){
  if(IS("equals","(Ljava/lang/Object;)Z")){if(!self){dvm__throw(v,"java/lang/NullPointerException","Proxy.equals receiver");return true;}out->i=a&&a[0].l==self;return true;}
  out->i=(int32_t)self;return true;
 }
 if(!strcmp(c,"java/net/Proxy")&&IS("type","()Ljava/net/Proxy$Type;")){
  if(proxy_statics_vm!=v||!proxy_direct){
   proxy_statics_vm=v;proxy_direct=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Proxy$Type"));
   if(!proxy_direct){dvm__throw(v,"java/lang/OutOfMemoryError","Proxy.Type.DIRECT");return true;}
  }
  out->l=proxy_direct;return true;
 }
 if(!strcmp(c,"java/lang/Integer")){
  bool value_of=IS("valueOf","(I)Ljava/lang/Integer;");
  bool ctor=IS("<init>","(I)V");
  bool value=IS("intValue","()I")||IS("byteValue","()B")||IS("shortValue","()S")||IS("longValue","()J")||IS("floatValue","()F")||IS("doubleValue","()D")||IS("hashCode","()I");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool compare=IS("compareTo","(Ljava/lang/Integer;)I");
  bool tostring=IS("toString","()Ljava/lang/String;");
  bool tostring_i=IS("toString","(I)Ljava/lang/String;");
  bool tostring_ii=IS("toString","(II)Ljava/lang/String;");
  if(!value_of&&!ctor&&!value&&!equals&&!compare&&!tostring&&!tostring_i&&!tostring_ii)return false;
  if(value_of){
   struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Integer");dvm_ref ref=dvm_new_object(v,cls);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Integer.valueOf");return true;}
   struct boxed_integer *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Integer.valueOf payload");return true;}
   box->value=a[0].i;dvm__obj(v,ref)->data=box;out->l=ref;return true;
  }
  /* 静态调用 self==0，必须在这里返回，不能落入下面的 boxed_integer_of(self)。 */
  if(tostring_i){if(nargs!=1)return false;return integer_to_string_radix(v,a[0].i,10,out);}
  if(tostring_ii){if(nargs!=2)return false;return integer_to_string_radix(v,a[0].i,a[1].i,out);}
  if(ctor){struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","Integer receiver");return true;}free(o->data);struct boxed_integer *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Integer payload");return true;}box->value=a[0].i;o->data=box;return true;}
  struct boxed_integer *box=boxed_integer_of(v,self);if(!box){dvm__throw(v,"java/lang/IllegalStateException","Integer not initialized");return true;}
  if(value){if(IS("intValue","()I")||IS("hashCode","()I"))out->i=box->value;else if(IS("byteValue","()B"))out->i=(int8_t)box->value;else if(IS("shortValue","()S"))out->i=(int16_t)box->value;else if(IS("longValue","()J"))out->j=box->value;else if(IS("floatValue","()F"))out->f=(float)box->value;else out->d=(double)box->value;return true;}
  if(equals){struct boxed_integer *other=boxed_integer_of(v,a[0].l);out->i=other&&other->value==box->value;return true;}
  if(compare){struct boxed_integer *other=boxed_integer_of(v,a[0].l);if(!other){dvm__throw(v,"java/lang/NullPointerException","Integer.compareTo");return true;}out->i=box->value<other->value?-1:box->value>other->value?1:0;return true;}
  char text[32];int n=snprintf(text,sizeof text,"%d",box->value);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/Long")){
  bool value_of=IS("valueOf","(J)Ljava/lang/Long;");
  bool ctor=IS("<init>","(J)V");
  bool value=IS("longValue","()J")||IS("intValue","()I")||IS("shortValue","()S")||IS("byteValue","()B")||IS("floatValue","()F")||IS("doubleValue","()D")||IS("hashCode","()I");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool compare=IS("compareTo","(Ljava/lang/Long;)I");
  bool tostring=IS("toString","()Ljava/lang/String;");
  bool tostring_j=IS("toString","(J)Ljava/lang/String;");
  if(!value_of&&!ctor&&!value&&!equals&&!compare&&!tostring&&!tostring_j)return false;
  if(tostring_j){if(nargs!=1)return false;char jt[32];int jn=snprintf(jt,sizeof jt,"%" PRId64 "",a[0].j);out->l=dvm_new_string_n(v,jt,(size_t)jn);return true;}
  if(value_of){
   struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Long");dvm_ref ref=dvm_new_object(v,cls);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Long.valueOf");return true;}
   struct boxed_long *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Long.valueOf payload");return true;}
   box->value=a[0].j;dvm__obj(v,ref)->data=box;out->l=ref;return true;
  }
  if(ctor){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","Long receiver");return true;}
   free(o->data);struct boxed_long *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Long payload");return true;}box->value=a[0].j;o->data=box;return true;
  }
  struct boxed_long *box=boxed_long_of(v,self);if(!box){dvm__throw(v,"java/lang/IllegalStateException","Long not initialized");return true;}
  if(value){
   if(IS("longValue","()J"))out->j=box->value;
   else if(IS("intValue","()I"))out->i=(int32_t)box->value;
   else if(IS("hashCode","()I"))out->i=(int32_t)(box->value^(box->value>>32));
   else if(IS("shortValue","()S"))out->i=(int16_t)box->value;
   else if(IS("byteValue","()B"))out->i=(int8_t)box->value;
   else if(IS("floatValue","()F"))out->f=(float)box->value;
   else out->d=(double)box->value;
   return true;
  }
  if(equals){struct boxed_long *other=boxed_long_of(v,a[0].l);out->i=other&&other->value==box->value;return true;}
  if(compare){struct boxed_long *other=boxed_long_of(v,a[0].l);if(!other){dvm__throw(v,"java/lang/NullPointerException","Long.compareTo");return true;}out->i=box->value<other->value?-1:box->value>other->value?1:0;return true;}
  char text[32];int n=snprintf(text,sizeof text,"%" PRId64,box->value);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/Float")){
  bool value_of=IS("valueOf","(F)Ljava/lang/Float;");
  bool ctor=IS("<init>","(F)V");
  bool value=IS("floatValue","()F")||IS("doubleValue","()D")||IS("intValue","()I")||IS("longValue","()J");
  bool hash=IS("hashCode","()I");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool tostring=IS("toString","()Ljava/lang/String;");
  if(!value_of&&!ctor&&!value&&!hash&&!equals&&!tostring)return false;
  if(value_of){out->l=new_boxed_float(v,a[0].f);return true;}
  if(ctor){
   struct boxed_float *box=calloc(1,sizeof *box);
   if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Float payload");return true;}
   box->kind=PAY_FLOAT;box->value=a[0].f;install_boxed(v,self,"java/lang/Float","Float",box);return true;
  }
  struct boxed_float *box=require_boxed_float(v,self,"Float");if(!box)return true;
  if(IS("floatValue","()F")){out->f=box->value;return true;}
  if(IS("doubleValue","()D")){out->d=(double)box->value;return true;}
  if(IS("intValue","()I")){out->i=java_float_to_int(box->value);return true;}
  if(IS("longValue","()J")){out->j=java_float_to_long(box->value);return true;}
  if(hash){out->i=(int32_t)float_to_int_bits(box->value);return true;}
  if(equals){struct boxed_float *other=boxed_float_of(v,a[0].l);out->i=other&&float_to_int_bits(other->value)==float_to_int_bits(box->value);return true;}
  char text[64];int n=java_float_text(box->value,text,sizeof text);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/Double")){
  bool value_of=IS("valueOf","(D)Ljava/lang/Double;");
  bool ctor=IS("<init>","(D)V");
  bool value=IS("doubleValue","()D")||IS("floatValue","()F")||IS("intValue","()I")||IS("longValue","()J");
  bool hash=IS("hashCode","()I");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool tostring=IS("toString","()Ljava/lang/String;");
  if(!value_of&&!ctor&&!value&&!hash&&!equals&&!tostring)return false;
  if(value_of){out->l=new_boxed_double(v,a[0].d);return true;}
  if(ctor){
   struct boxed_double *box=calloc(1,sizeof *box);
   if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Double payload");return true;}
   box->kind=PAY_DOUBLE;box->value=a[0].d;install_boxed(v,self,"java/lang/Double","Double",box);return true;
  }
  struct boxed_double *box=require_boxed_double(v,self,"Double");if(!box)return true;
  if(IS("doubleValue","()D")){out->d=box->value;return true;}
  if(IS("floatValue","()F")){out->f=(float)box->value;return true;}
  if(IS("intValue","()I")){out->i=java_double_to_int(box->value);return true;}
  if(IS("longValue","()J")){out->j=java_double_to_long(box->value);return true;}
  if(hash){uint64_t bits=double_to_long_bits(box->value);out->i=(int32_t)(uint32_t)(bits^(bits>>32));return true;}
  if(equals){struct boxed_double *other=boxed_double_of(v,a[0].l);out->i=other&&double_to_long_bits(other->value)==double_to_long_bits(box->value);return true;}
  char text[64];int n=java_double_text(box->value,text,sizeof text);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/Character")){
  bool value_of=IS("valueOf","(C)Ljava/lang/Character;");
  bool ctor=IS("<init>","(C)V");
  bool value=IS("charValue","()C")||IS("intValue","()I")||IS("longValue","()J")||IS("floatValue","()F")||IS("doubleValue","()D");
  bool hash=IS("hashCode","()I");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool tostring=IS("toString","()Ljava/lang/String;");
  /* Character.charCount(int) is static and has no wrapper receiver.  Java
   * returns 2 only for code points at or above U+10000 and 1 for everything
   * else, including negative values and unpaired surrogates. */
  bool char_count=IS("charCount","(I)I")&&nargs==1;
  /* Character.isWhitespace(char)/isSpaceChar(char) 是静态方法，DEX 的
   * invoke-static 不携带 receiver（self==0）。精确匹配 (C)Z，避免把装箱
   * 实例方法或其他重载（如 isWhitespace(int)）吞进这个分支。 */
  bool is_whitespace=!self&&IS("isWhitespace","(C)Z")&&nargs==1;
  bool is_space_char=!self&&IS("isSpaceChar","(C)Z")&&nargs==1;
  /* int 重载接收 code point：不截断为 16 位，补充平面当前没有 Zs/Zl/Zp，
   * 非法 code point 也一律 false。 */
  bool is_whitespace_i=!self&&IS("isWhitespace","(I)Z")&&nargs==1;
  bool is_space_char_i=!self&&IS("isSpaceChar","(I)Z")&&nargs==1;
  /* OkHttp HttpLoggingInterceptor.isPlaintext 在 DEX unit_offset 46/52 以
   * invoke-static 调用 isISOControl(I)Z / isWhitespace(I)Z；同时保留 char
   * 重载。两个重载都精确匹配签名且要求无 receiver。 */
  bool is_iso_control=!self&&IS("isISOControl","(C)Z")&&nargs==1;
  bool is_iso_control_i=!self&&IS("isISOControl","(I)Z")&&nargs==1;
  if(!value_of&&!ctor&&!value&&!hash&&!equals&&!tostring&&!char_count&&
     !is_whitespace&&!is_space_char&&!is_whitespace_i&&!is_space_char_i&&
     !is_iso_control&&!is_iso_control_i)return false;
  if(char_count){out->i=a[0].i>=0x10000?2:1;return true;}
  if(is_whitespace||is_space_char){
   uint32_t ch=(uint32_t)(uint16_t)a[0].i;
   out->i=is_whitespace?java_character_is_whitespace(ch):java_character_is_space_char(ch);
   return true;
  }
  if(is_whitespace_i||is_space_char_i){
   int32_t cp=a[0].i;
   bool valid=cp>=0&&(uint32_t)cp<=0x10FFFF;
   out->i=valid&&(is_whitespace_i?java_character_is_whitespace((uint32_t)cp):java_character_is_space_char((uint32_t)cp));
   return true;
  }
  if(is_iso_control||is_iso_control_i){
   /* char 重载只取低 16 位；int 重载原样传递，负数转 uint32_t 后自然不命中。 */
   uint32_t cp=is_iso_control?(uint32_t)(uint16_t)a[0].i:(uint32_t)a[0].i;
   out->i=java_character_is_iso_control(cp);
   return true;
  }
  if(value_of){out->l=new_boxed_character(v,(uint32_t)(uint16_t)a[0].i);return true;}
  if(ctor){
   struct boxed_character *box=calloc(1,sizeof *box);
   if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Character payload");return true;}
   box->kind=PAY_CHARACTER;box->value=(uint32_t)(uint16_t)a[0].i;install_boxed(v,self,"java/lang/Character","Character",box);return true;
  }
  struct boxed_character *box=require_boxed_character(v,self,"Character");if(!box)return true;
  if(IS("charValue","()C")||IS("intValue","()I")){out->i=(int32_t)box->value;return true;}
  if(IS("longValue","()J")){out->j=(int64_t)box->value;return true;}
  if(IS("floatValue","()F")){out->f=(float)box->value;return true;}
  if(IS("doubleValue","()D")){out->d=(double)box->value;return true;}
  if(hash){out->i=(int32_t)box->value;return true;}
  if(equals){struct boxed_character *other=boxed_character_of(v,a[0].l);out->i=other&&other->value==box->value;return true;}
  char text[8];int n=java_character_text(box->value,text,sizeof text);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/String")&&IS("equals","(Ljava/lang/Object;)Z")){
  struct dvm_object *left=dvm__obj(v,self),*right=dvm__obj(v,a[0].l);
  out->i=left&&right&&left->kind==DVM_OBJ_STRING&&right->kind==DVM_OBJ_STRING&&
         left->utf8_len==right->utf8_len&&!memcmp(left->utf8,right->utf8,left->utf8_len);
  return true;
 }
 if(!strcmp(c,"java/util/concurrent/atomic/AtomicBoolean")){
  bool ctor=IS("<init>","()V")||IS("<init>","(Z)V");
  if(!ctor&&!IS("get","()Z")&&!IS("set","(Z)V")&&!IS("lazySet","(Z)V")&&!IS("getAndSet","(Z)Z")&&!IS("compareAndSet","(ZZ)Z")&&!IS("weakCompareAndSet","(ZZ)Z"))return false;
  struct payload *p=payload(v,self,PAY_ATOMIC_BOOL,ctor);if(!p)return true;
  if(ctor){atomic_store(&p->flag,IS("<init>","(Z)V")&&a[0].i);}
  else if(IS("get","()Z"))out->i=atomic_load(&p->flag);
  else if(IS("getAndSet","(Z)Z"))out->i=atomic_exchange(&p->flag,!!a[0].i);
  else if(IS("compareAndSet","(ZZ)Z")||IS("weakCompareAndSet","(ZZ)Z")){bool expected=!!a[0].i;out->i=atomic_compare_exchange_strong(&p->flag,&expected,!!a[1].i);}
  else atomic_store(&p->flag,!!a[0].i);
  return true;
 }
 if(!strcmp(c,"java/lang/ThreadLocal")){
  bool ctor=IS("<init>","()V");
  if(IS("initialValue","()Ljava/lang/Object;"))return true; /* default null */
  if(!ctor&&!IS("get","()Ljava/lang/Object;")&&!IS("set","(Ljava/lang/Object;)V")&&!IS("remove","()V"))return false;
  struct payload *p=payload(v,self,PAY_TLS,ctor);if(!p)return true;
  if(ctor)return true;
  /* Subclasses may override initialValue: an absent entry must execute it. */
  struct tls_entry **slot=&p->tls;
  while(*slot&&!pthread_equal((*slot)->thread,pthread_self()))slot=&(*slot)->next;
  if(IS("remove","()V")){if(*slot){struct tls_entry *e=*slot;*slot=e->next;free(e);}return true;}
  if(!*slot){*slot=calloc(1,sizeof **slot);if(!*slot){dvm__throw(v,"java/lang/OutOfMemoryError","ThreadLocal");return true;}(*slot)->thread=pthread_self();
   if(IS("get","()Ljava/lang/Object;")){struct dvm_object *o=dvm__obj(v,self);struct dvm_method *init=dvm_find_method(v,o->cls,"initialValue","()Ljava/lang/Object;");if(init&&(init->has_code||init->builtin)){union dvm_value r={0};if(dvm_call(v,init,self,NULL,0,&r))(*slot)->value=r.l;}}}
  if(IS("set","(Ljava/lang/Object;)V"))(*slot)->value=a[0].l;else out->l=(*slot)->value;
  return true;
 }
 struct dvm_object *receiver=self?dvm__obj(v,self):NULL;
 bool list=!strcmp(c,"java/util/concurrent/CopyOnWriteArrayList")||(receiver&&receiver->cls&&!strcmp(receiver->cls->name,"java/util/concurrent/CopyOnWriteArrayList"));
 bool iterator=receiver&&receiver->cls&&!strcmp(receiver->cls->name,"java/util/concurrent/CopyOnWriteArrayList$COWIterator");
 /* CopyOnWriteArraySet is the Set facade over the same copy-on-write array:
  * add() appends only when java_basics_equal() says the element is absent,
  * and every mutation replaces the snapshot so live iterators keep the old
  * one (Laser TaskManager's <clinit> reached this gap in 72bq/72br). */
 bool cow_set=!strcmp(c,"java/util/concurrent/CopyOnWriteArraySet")||(receiver&&receiver->cls&&!strcmp(receiver->cls->name,"java/util/concurrent/CopyOnWriteArraySet"));
 if(cow_set){
  bool ctor=IS("<init>","()V");
  if(!ctor&&!IS("size","()I")&&!IS("isEmpty","()Z")&&!IS("add","(Ljava/lang/Object;)Z")&&!IS("contains","(Ljava/lang/Object;)Z")&&!IS("remove","(Ljava/lang/Object;)Z")&&!IS("clear","()V")&&!IS("iterator","()Ljava/util/Iterator;")&&!IS("toArray","()[Ljava/lang/Object;"))return false;
  struct payload *p=payload(v,self,PAY_LIST,ctor);if(!p)return true;
  if(ctor){struct snapshot *s=snapshot_new(0);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","set payload");return true;}snapshot_release(p->s);p->s=s;return true;}
  if(!p->s){dvm__throw(v,"java/lang/IllegalStateException","set uninitialized");return true;}
  size_t n=p->s->n;struct snapshot *next=NULL;
  if(IS("size","()I")){out->i=(int)n;return true;}
  if(IS("isEmpty","()Z")){out->i=n==0;return true;}
  if(IS("iterator","()Ljava/util/Iterator;")){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/CopyOnWriteArrayList$COWIterator"));struct payload *it=payload(v,out->l,PAY_ITER,true);if(!it)return true;it->s=p->s;++p->s->owners;return true;}
  if(IS("toArray","()[Ljava/lang/Object;")){out->l=dvm_new_array(v,'L',"Ljava/lang/Object;",(uint32_t)n);if(n)memcpy(dvm_array_data(v,out->l),p->s->values,n*sizeof(dvm_ref));return true;}
  if(IS("clear","()V")){next=snapshot_new(0);if(!next){dvm__throw(v,"java/lang/OutOfMemoryError","set payload");return true;}snapshot_release(p->s);p->s=next;return true;}
  size_t i=0;for(;i<n;++i){if(java_basics_equal(v,a?a[0].l:0,p->s->values[i]))break;if(dvm_exception(v))return true;}
  if(IS("contains","(Ljava/lang/Object;)Z")){out->i=i<n;return true;}
  if(IS("remove","(Ljava/lang/Object;)Z")){
   if(i==n){out->i=0;return true;}
   next=snapshot_new(n-1);if(!next){dvm__throw(v,"java/lang/OutOfMemoryError","set payload");return true;}
   memcpy(next->values,p->s->values,i*sizeof(dvm_ref));memcpy(next->values+i,p->s->values+i+1,(n-i-1)*sizeof(dvm_ref));out->i=1;
  }else if(IS("add","(Ljava/lang/Object;)Z")){
   if(i<n){out->i=0;return true;}
   next=snapshot_new(n+1);if(!next){dvm__throw(v,"java/lang/OutOfMemoryError","set payload");return true;}
   memcpy(next->values,p->s->values,n*sizeof(dvm_ref));next->values[n]=a[0].l;out->i=1;
  }else return false;
  snapshot_release(p->s);p->s=next;return true;
 }
 if(iterator){
  if(!IS("hasNext","()Z")&&!IS("next","()Ljava/lang/Object;")&&!IS("remove","()V"))return false;
  struct payload *p=payload(v,self,PAY_ITER,false);if(!p)return true;
  if(IS("hasNext","()Z"))out->i=p->pos<p->s->n;
  else if(IS("remove","()V"))dvm__throw(v,"java/lang/UnsupportedOperationException","CopyOnWrite iterator is immutable");
  else if(p->pos>=p->s->n)dvm__throw(v,"java/util/NoSuchElementException","iterator exhausted");
  else out->l=p->s->values[p->pos++];return true;
 }
 if(!list)return false;
 bool ctor=IS("<init>","()V")||IS("<init>","([Ljava/lang/Object;)V");
 if(!ctor&&!IS("size","()I")&&!IS("isEmpty","()Z")&&!IS("add","(Ljava/lang/Object;)Z")&&!IS("add","(ILjava/lang/Object;)V")&&!IS("get","(I)Ljava/lang/Object;")&&!IS("set","(ILjava/lang/Object;)Ljava/lang/Object;")&&!IS("remove","(I)Ljava/lang/Object;")&&!IS("remove","(Ljava/lang/Object;)Z")&&!IS("clear","()V")&&!IS("iterator","()Ljava/util/Iterator;")&&!IS("contains","(Ljava/lang/Object;)Z")&&!IS("indexOf","(Ljava/lang/Object;)I")&&!IS("addIfAbsent","(Ljava/lang/Object;)Z")&&!IS("toArray","()[Ljava/lang/Object;"))return false;
 struct payload *p=payload(v,self,PAY_LIST,ctor);if(!p)return true;
 if(ctor){size_t n=IS("<init>","()V")?0:dvm_array_length(v,a[0].l);if(IS("<init>","([Ljava/lang/Object;)V")&&!a[0].l){dvm__throw(v,"java/lang/NullPointerException","source array");return true;}struct snapshot *s=snapshot_new(n);if(!s)goto oom;if(n)memcpy(s->values,dvm_array_data(v,a[0].l),n*sizeof(dvm_ref));snapshot_release(p->s);p->s=s;return true;}
 if(!p->s){dvm__throw(v,"java/lang/IllegalStateException","list uninitialized");return true;}
 size_t n=p->s->n;struct snapshot *next=NULL;
 if(IS("size","()I")){out->i=(int)n;return true;}
 if(IS("isEmpty","()Z")){out->i=n==0;return true;}
 if(IS("iterator","()Ljava/util/Iterator;")){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/CopyOnWriteArrayList$COWIterator"));struct payload *it=payload(v,out->l,PAY_ITER,true);if(!it)return true;it->s=p->s;++p->s->owners;return true;}
 if(IS("toArray","()[Ljava/lang/Object;")){out->l=dvm_new_array(v,'L',"Ljava/lang/Object;",(uint32_t)n);if(n)memcpy(dvm_array_data(v,out->l),p->s->values,n*sizeof(dvm_ref));return true;}
 if(IS("contains","(Ljava/lang/Object;)Z")||IS("indexOf","(Ljava/lang/Object;)I")||IS("addIfAbsent","(Ljava/lang/Object;)Z")||IS("remove","(Ljava/lang/Object;)Z")){
  size_t i=0;for(;i<n;++i){if(java_basics_equal(v,a[0].l,p->s->values[i]))break;if(dvm_exception(v))return true;}
  if(IS("contains","(Ljava/lang/Object;)Z")){out->i=i<n;return true;}
  if(IS("indexOf","(Ljava/lang/Object;)I")){out->i=i<n?(int)i:-1;return true;}
  if(IS("addIfAbsent","(Ljava/lang/Object;)Z")){if(i<n)return true;next=snapshot_new(n+1);if(!next)goto oom;memcpy(next->values,p->s->values,n*sizeof(dvm_ref));next->values[n]=a[0].l;out->i=1;}
  else {if(i==n)return true;next=snapshot_new(n-1);if(!next)goto oom;memcpy(next->values,p->s->values,i*sizeof(dvm_ref));memcpy(next->values+i,p->s->values+i+1,(n-i-1)*sizeof(dvm_ref));out->i=1;}
 }else if(IS("add","(Ljava/lang/Object;)Z")||IS("add","(ILjava/lang/Object;)V")){
  int at=IS("add","(Ljava/lang/Object;)Z")?(int)n:a[0].i;if(at<0||(size_t)at>n)goto bounds;
  next=snapshot_new(n+1);if(!next)goto oom;memcpy(next->values,p->s->values,(size_t)at*sizeof(dvm_ref));next->values[at]=a[IS("add","(Ljava/lang/Object;)Z")?0:1].l;memcpy(next->values+at+1,p->s->values+at,(n-at)*sizeof(dvm_ref));out->i=1;
 }else if(IS("clear","()V")){next=snapshot_new(0);if(!next)goto oom;}
 else {int at=a[0].i;if(at<0||(size_t)at>=n)goto bounds;out->l=p->s->values[at];if(IS("get","(I)Ljava/lang/Object;"))return true;
  bool remove=IS("remove","(I)Ljava/lang/Object;");next=snapshot_new(n-remove);if(!next)goto oom;
  if(remove){memcpy(next->values,p->s->values,(size_t)at*sizeof(dvm_ref));memcpy(next->values+at,p->s->values+at+1,(n-at-1)*sizeof(dvm_ref));}else{memcpy(next->values,p->s->values,n*sizeof(dvm_ref));next->values[at]=a[1].l;}}
 snapshot_release(p->s);p->s=next;return true;
bounds:dvm__throw(v,"java/lang/IndexOutOfBoundsException","list index");return true;
oom:dvm__throw(v,"java/lang/OutOfMemoryError","base class payload");return true;
}
static bool wrapper_exception_is(struct dvm *v,const char *name){
 struct dvm_object *e=dvm__obj(v,dvm_exception(v));
 return e&&e->cls&&e->cls->name&&!strcmp(e->cls->name,name);
}
/* Contract tests for the Float/Double/Character wrappers.  Call this from
 * sdk_init_selftest with a live VM: no state outside the VM is touched. */
bool java_basics_wrapper_selftest(struct dvm *v){
 bool okay=true;unsigned checks=0;
 union dvm_value arg={0},out={0},eq={0};
 dvm_ref f1=0,f2=0,f3=0,d1=0,d2=0,c1=0,c2=0,c3=0,vf=0,vd=0,vc=0,bad_bool=0,uninit=0;
 dvm_ref arr_empty=0,arr_mixed=0,arr_throwing=0,alpha=0,boxed7=0;
 dvm_ref boxedLong=0,boxedNan=0,boxedPosInf=0,boxedNegInf=0,boxedFloatNan=0,atomicInt=0;
 const char *text=NULL;
 if(!v)return false;
 struct dvm_class *float_cls=dvm_find_class_or_external(v,"java/lang/Float");
 struct dvm_class *double_cls=dvm_find_class_or_external(v,"java/lang/Double");
 struct dvm_class *char_cls=dvm_find_class_or_external(v,"java/lang/Character");
 struct dvm_class *bool_cls=dvm_find_class_or_external(v,"java/lang/Boolean");
 struct dvm_class *atomic_cls=dvm_find_class_or_external(v,"java/util/concurrent/atomic/AtomicInteger");
 if(!float_cls||!double_cls||!char_cls||!bool_cls||!atomic_cls)return false;
#define WRAP_CHECK(expr,stage) do{++checks;if(!(expr)){sdk_trace_fprintf(stderr,"JAVA_BASICS_WRAPPER_SELFTEST FAIL stage=%s line=%d\n",stage,__LINE__);okay=false;goto done;}}while(0)
 /* Float(1.5f) and Java value/comparison semantics. */
 f1=dvm_new_object(v,float_cls);arg.f=1.5f;
 WRAP_CHECK(f1&&java_basics_call(v,"java/lang/Float","<init>","(F)V",f1,&arg,1,&out),"float_ctor");
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","floatValue","()F",f1,NULL,0,&out)&&out.f==1.5f,"float_floatValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","doubleValue","()D",f1,NULL,0,&out)&&out.d==1.5,"float_doubleValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","intValue","()I",f1,NULL,0,&out)&&out.i==1,"float_intValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","longValue","()J",f1,NULL,0,&out)&&out.j==1,"float_longValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","hashCode","()I",f1,NULL,0,&out)&&(uint32_t)out.i==0x3fc00000u,"float_hashCode");
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","toString","()Ljava/lang/String;",f1,NULL,0,&out)&&out.l&&(text=dvm_string_utf8(v,out.l))&&!strcmp(text,"1.5"),"float_toString");
 f2=dvm_new_object(v,float_cls);arg.f=1.5f;
 WRAP_CHECK(f2&&java_basics_call(v,"java/lang/Float","<init>","(F)V",f2,&arg,1,&out),"float_ctor_same");
 eq.l=f2;
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","equals","(Ljava/lang/Object;)Z",f1,&eq,1,&out)&&out.i,"float_equals_same");
 WRAP_CHECK(java_basics_equal(v,f1,f2),"float_equal_same");
 f3=dvm_new_object(v,float_cls);arg.f=2.5f;
 WRAP_CHECK(f3&&java_basics_call(v,"java/lang/Float","<init>","(F)V",f3,&arg,1,&out),"float_ctor_other");
 eq.l=f3;
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","equals","(Ljava/lang/Object;)Z",f1,&eq,1,&out)&&!out.i,"float_equals_other");
 eq.l=0;
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","equals","(Ljava/lang/Object;)Z",f1,&eq,1,&out)&&!out.i,"float_equals_null");
 /* Double(2.25). */
 d1=dvm_new_object(v,double_cls);arg.d=2.25;
 WRAP_CHECK(d1&&java_basics_call(v,"java/lang/Double","<init>","(D)V",d1,&arg,1,&out),"double_ctor");
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","doubleValue","()D",d1,NULL,0,&out)&&out.d==2.25,"double_doubleValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","floatValue","()F",d1,NULL,0,&out)&&out.f==2.25f,"double_floatValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","intValue","()I",d1,NULL,0,&out)&&out.i==2,"double_intValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","longValue","()J",d1,NULL,0,&out)&&out.j==2,"double_longValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","hashCode","()I",d1,NULL,0,&out)&&out.i==0x40020000,"double_hashCode");
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","toString","()Ljava/lang/String;",d1,NULL,0,&out)&&out.l&&(text=dvm_string_utf8(v,out.l))&&!strcmp(text,"2.25"),"double_toString");
 d2=dvm_new_object(v,double_cls);arg.d=2.25;
 WRAP_CHECK(d2&&java_basics_call(v,"java/lang/Double","<init>","(D)V",d2,&arg,1,&out),"double_ctor_same");
 eq.l=d2;
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","equals","(Ljava/lang/Object;)Z",d1,&eq,1,&out)&&out.i,"double_equals_same");
 /* Character('中'), including the UTF-16 char and UTF-8 toString. */
 c1=dvm_new_object(v,char_cls);arg.i=0x4e2d;
 WRAP_CHECK(c1&&java_basics_call(v,"java/lang/Character","<init>","(C)V",c1,&arg,1,&out),"character_ctor");
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","charValue","()C",c1,NULL,0,&out)&&out.i==0x4e2d,"character_charValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","intValue","()I",c1,NULL,0,&out)&&out.i==0x4e2d,"character_intValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","hashCode","()I",c1,NULL,0,&out)&&out.i==0x4e2d,"character_hashCode");
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","toString","()Ljava/lang/String;",c1,NULL,0,&out)&&out.l&&dvm_string_utf8_length(v,out.l)==3&&(text=dvm_string_utf8(v,out.l))&&!memcmp(text,"\xe4\xb8\xad",3),"character_toString");
 c2=dvm_new_object(v,char_cls);arg.i=0x4e2d;
 WRAP_CHECK(c2&&java_basics_call(v,"java/lang/Character","<init>","(C)V",c2,&arg,1,&out),"character_ctor_same");
 eq.l=c2;
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","equals","(Ljava/lang/Object;)Z",c1,&eq,1,&out)&&out.i,"character_equals_same");
 c3=dvm_new_object(v,char_cls);arg.i=0x6587;
 WRAP_CHECK(c3&&java_basics_call(v,"java/lang/Character","<init>","(C)V",c3,&arg,1,&out),"character_ctor_other");
 eq.l=c3;
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","equals","(Ljava/lang/Object;)Z",c1,&eq,1,&out)&&!out.i,"character_equals_other");
 /* Character.charCount(int) is static and code-point based.  Java returns 2
  * exactly for code points >= U+10000; negative input and BMP values,
  * including unpaired surrogates, return 1.  A different signature must stay
  * unhandled and must not leave an exception behind. */
 union dvm_value cc={0};
 cc.i=0x41;WRAP_CHECK(java_basics_call(v,"java/lang/Character","charCount","(I)I",0,&cc,1,&out)&&out.i==1,"character_charCount_ascii");
 cc.i=0xFFFF;WRAP_CHECK(java_basics_call(v,"java/lang/Character","charCount","(I)I",0,&cc,1,&out)&&out.i==1,"character_charCount_bmp_max");
 cc.i=0x10000;WRAP_CHECK(java_basics_call(v,"java/lang/Character","charCount","(I)I",0,&cc,1,&out)&&out.i==2,"character_charCount_boundary");
 cc.i=0x1F600;WRAP_CHECK(java_basics_call(v,"java/lang/Character","charCount","(I)I",0,&cc,1,&out)&&out.i==2,"character_charCount_emoji");
 cc.i=0x10FFFF;WRAP_CHECK(java_basics_call(v,"java/lang/Character","charCount","(I)I",0,&cc,1,&out)&&out.i==2,"character_charCount_max");
 cc.i=-1;WRAP_CHECK(java_basics_call(v,"java/lang/Character","charCount","(I)I",0,&cc,1,&out)&&out.i==1,"character_charCount_negative");
 WRAP_CHECK(!java_basics_call(v,"java/lang/Character","charCount","(J)I",0,&cc,1,&out),"character_charCount_signature");
 WRAP_CHECK(!dvm_exception(v),"character_charCount_clean_state");
 /* Character.isWhitespace(char)/isSpaceChar(char) 静态谓词。Java 空白
  * 语义排除 NBSP/U+2007/U+202F，而 isSpaceChar 接受全部 Zs/Zl/Zp。 */
 cc.i=0x20;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_space");
 cc.i=0x09;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_tab");
 cc.i=0x0a;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_newline");
 cc.i=0x0d;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_cr");
 cc.i=0x1c;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_fs");
 cc.i=0x1680;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_ogham");
 cc.i=0x2000;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_en_quad");
 cc.i=0x2006;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_six_per_em");
 cc.i=0x2008;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_punctuation_space");
 cc.i=0x200a;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_hair_space");
 cc.i=0x205f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_medium_mathematical_space");
 cc.i=0x3000;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_ideographic_space");
 cc.i=0x00a0;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_nbsp_false");
 cc.i=0x2007;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_figure_space_false");
 cc.i=0x202f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_narrow_nbsp_false");
 cc.i=0x200b;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_zero_width_false");
 cc.i=0x41;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_letter_false");
 cc.i=0x00a0;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&out.i,"character_isSpaceChar_nbsp");
 cc.i=0x0020;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&out.i,"character_isSpaceChar_space");
 cc.i=0x2007;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&out.i,"character_isSpaceChar_figure_space");
 cc.i=0x2028;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&out.i,"character_isSpaceChar_line_separator");
 cc.i=0x2029;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&out.i,"character_isSpaceChar_paragraph_separator");
 cc.i=0x202f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&out.i,"character_isSpaceChar_narrow_nbsp");
 cc.i=0x3000;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&out.i,"character_isSpaceChar_ideographic_space");
 cc.i=0x200b;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&!out.i,"character_isSpaceChar_zero_width_false");
 cc.i=0x09;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&!out.i,"character_isSpaceChar_tab_false");
 cc.i=0x41;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(C)Z",0,&cc,1,&out)&&!out.i,"character_isSpaceChar_letter_false");
 /* int 重载接收完整 code point；低 16 位恰为空白类的补充平面字符必须
  * 返回 false，防止实现错误地截断成 char。 */
 cc.i=0x20;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(I)Z",0,&cc,1,&out)&&out.i,"character_isWhitespace_int_space");
 cc.i=0x00a0;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(I)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_int_nbsp_false");
 cc.i=0x10020;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(I)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_int_supplementary_false");
 cc.i=-1;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(I)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_int_negative_false");
 cc.i=0x110000;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isWhitespace","(I)Z",0,&cc,1,&out)&&!out.i,"character_isWhitespace_int_invalid_codepoint_false");
 cc.i=0x00a0;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(I)Z",0,&cc,1,&out)&&out.i,"character_isSpaceChar_int_nbsp");
 cc.i=0x10020;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(I)Z",0,&cc,1,&out)&&!out.i,"character_isSpaceChar_int_supplementary_false");
 cc.i=0x200b;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isSpaceChar","(I)Z",0,&cc,1,&out)&&!out.i,"character_isSpaceChar_int_zero_width_false");
 /* Character.isISOControl(char)：C0 控制符、DEL 与 C1 控制符为 true，
  * 相邻的 U+0020/U+007E/U+00A0 为 false。 */
 cc.i=0x00;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(C)Z",0,&cc,1,&out)&&out.i,"character_isISOControl_nul");
 cc.i=0x1f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(C)Z",0,&cc,1,&out)&&out.i,"character_isISOControl_unit_separator");
 cc.i=0x7f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(C)Z",0,&cc,1,&out)&&out.i,"character_isISOControl_del");
 cc.i=0x9f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(C)Z",0,&cc,1,&out)&&out.i,"character_isISOControl_c1_max");
 cc.i=0x20;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(C)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_space_false");
 cc.i=0x7e;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(C)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_tilde_false");
 cc.i=0xa0;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(C)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_nbsp_false");
 /* int 重载是 code point：与 Java oracle 一致，0x10020/负值/0x110000 均 false，
  * 不能把 0x10020 截断成 0x0020。 */
 cc.i=0x00;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&out.i,"character_isISOControl_int_nul");
 cc.i=0x1f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&out.i,"character_isISOControl_int_unit_separator");
 cc.i=0x7f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&out.i,"character_isISOControl_int_del");
 cc.i=0x9f;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&out.i,"character_isISOControl_int_c1_max");
 cc.i=0x20;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_int_space_false");
 cc.i=0x7e;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_int_tilde_false");
 cc.i=0xa0;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_int_nbsp_false");
 cc.i=0x10020;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_int_supplementary_false");
 cc.i=-1;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_int_negative_false");
 cc.i=0x110000;WRAP_CHECK(java_basics_call(v,"java/lang/Character","isISOControl","(I)Z",0,&cc,1,&out)&&!out.i,"character_isISOControl_int_invalid_codepoint_false");
 WRAP_CHECK(!java_basics_call(v,"java/lang/Character","isWhitespace","(J)Z",0,&cc,1,&out)&&!dvm_exception(v),"character_isWhitespace_signature");
 WRAP_CHECK(!java_basics_call(v,"java/lang/Character","isISOControl","(J)Z",0,&cc,1,&out)&&!dvm_exception(v),"character_isISOControl_signature");
 WRAP_CHECK(!java_basics_call(v,"java/lang/Character","isISOControl","(C)Z",c1,&cc,1,&out)&&!dvm_exception(v),"character_isISOControl_receiver_rejected");
 WRAP_CHECK(!java_basics_call(v,"java/lang/Character","isSpaceChar","(J)Z",0,&cc,1,&out)&&!dvm_exception(v),"character_isSpaceChar_signature");
 WRAP_CHECK(!java_basics_call(v,"java/lang/Character","isWhitespace","(C)Z",c1,&cc,1,&out)&&!dvm_exception(v),"character_isWhitespace_receiver_rejected");
 /* valueOf round-trips through the same payload accessors. */
 arg.f=-1.5f;
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","valueOf","(F)Ljava/lang/Float;",0,&arg,1,&out)&&(vf=out.l)!=0,"float_valueOf");
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","floatValue","()F",vf,NULL,0,&out)&&out.f==-1.5f,"float_valueOf_roundtrip");
 arg.d=-2.25;
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","valueOf","(D)Ljava/lang/Double;",0,&arg,1,&out)&&(vd=out.l)!=0,"double_valueOf");
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","doubleValue","()D",vd,NULL,0,&out)&&out.d==-2.25,"double_valueOf_roundtrip");
 arg.i=0x4e2d;
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","valueOf","(C)Ljava/lang/Character;",0,&arg,1,&out)&&(vc=out.l)!=0,"character_valueOf");
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","charValue","()C",vc,NULL,0,&out)&&out.i==0x4e2d,"character_valueOf_roundtrip");
 /* Virtual calls declared on Number/Object re-dispatch to the wrapper class. */
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","doubleValue","()D",f1,NULL,0,&out)&&out.d==1.5,"number_declared_doubleValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","intValue","()I",d1,NULL,0,&out)&&out.i==2,"number_declared_intValue");
 /* Number-declared calls must reach the Integer/Long implementations.  The
  * DEX invokes Number.intValue()/longValue() on boxed values (Gson,
  * preferences, BLConfig), and Integer/Long are external classes without a
  * populated method table for Number.  Box every receiver through the real
  * valueOf paths before any assertion below reads it, so the Number
  * declarations never observe a zero handle. */
 arg.i=7;
 WRAP_CHECK(java_basics_call(v,"java/lang/Integer","valueOf","(I)Ljava/lang/Integer;",0,&arg,1,&out)&&(boxed7=out.l)!=0,"number_integer_box");
 arg.j=INT64_C(0x100000007);
 WRAP_CHECK(java_basics_call(v,"java/lang/Long","valueOf","(J)Ljava/lang/Long;",0,&arg,1,&out)&&(boxedLong=out.l)!=0,"number_long_box");
 {
  uint64_t nan_bits=UINT64_C(0x7ff8000000000000),pos_inf_bits=UINT64_C(0x7ff0000000000000),neg_inf_bits=UINT64_C(0xfff0000000000000);
  memcpy(&arg.d,&nan_bits,sizeof arg.d);
  WRAP_CHECK(java_basics_call(v,"java/lang/Double","valueOf","(D)Ljava/lang/Double;",0,&arg,1,&out)&&(boxedNan=out.l)!=0,"number_double_nan_box");
  memcpy(&arg.d,&pos_inf_bits,sizeof arg.d);
  WRAP_CHECK(java_basics_call(v,"java/lang/Double","valueOf","(D)Ljava/lang/Double;",0,&arg,1,&out)&&(boxedPosInf=out.l)!=0,"number_double_pos_inf_box");
  memcpy(&arg.d,&neg_inf_bits,sizeof arg.d);
  WRAP_CHECK(java_basics_call(v,"java/lang/Double","valueOf","(D)Ljava/lang/Double;",0,&arg,1,&out)&&(boxedNegInf=out.l)!=0,"number_double_neg_inf_box");
 }
 {
  uint32_t float_nan_bits=UINT32_C(0x7fc00000);
  memcpy(&arg.f,&float_nan_bits,sizeof arg.f);
  WRAP_CHECK(java_basics_call(v,"java/lang/Float","valueOf","(F)Ljava/lang/Float;",0,&arg,1,&out)&&(boxedFloatNan=out.l)!=0,"number_float_nan_box");
 }
 /* AtomicInteger is another Number subclass implemented by java_basics. */
 atomicInt=dvm_new_object(v,atomic_cls);arg.i=41;
 WRAP_CHECK(atomicInt&&java_basics_call(v,"java/util/concurrent/atomic/AtomicInteger","<init>","(I)V",atomicInt,&arg,1,&out),"number_atomic_integer_ctor");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","intValue","()I",boxed7,NULL,0,&out)&&out.i==7,"number_declared_integer_intValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","longValue","()J",boxed7,NULL,0,&out)&&out.j==7,"number_declared_integer_longValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","doubleValue","()D",boxed7,NULL,0,&out)&&out.d==7.0,"number_declared_integer_doubleValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Object","toString","()Ljava/lang/String;",boxed7,NULL,0,&out)&&out.l&&(text=dvm_string_utf8(v,out.l))&&!strcmp(text,"7"),"object_declared_integer_toString");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","intValue","()I",boxedLong,NULL,0,&out)&&out.i==7,"number_declared_long_intValue_truncates");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","longValue","()J",boxedLong,NULL,0,&out)&&out.j==INT64_C(0x100000007),"number_declared_long_longValue");
 /* JLS 5.1.3: NaN converts to zero and out-of-range values saturate. */
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","intValue","()I",boxedNan,NULL,0,&out)&&out.i==0,"number_declared_double_nan_intValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","intValue","()I",boxedPosInf,NULL,0,&out)&&out.i==INT32_MAX,"number_declared_double_pos_inf_saturates");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","intValue","()I",boxedNegInf,NULL,0,&out)&&out.i==INT32_MIN,"number_declared_double_neg_inf_saturates");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","intValue","()I",boxedFloatNan,NULL,0,&out)&&out.i==0,"number_declared_float_nan_intValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Number","intValue","()I",atomicInt,NULL,0,&out)&&out.i==41,"number_declared_atomic_integer_intValue");
 WRAP_CHECK(java_basics_call(v,"java/lang/Object","toString","()Ljava/lang/String;",atomicInt,NULL,0,&out)&&out.l&&(text=dvm_string_utf8(v,out.l))&&!strcmp(text,"41"),"object_declared_atomic_integer_toString");
 WRAP_CHECK(java_basics_call(v,"java/lang/Object","toString","()Ljava/lang/String;",c1,NULL,0,&out)&&out.l&&(text=dvm_string_utf8(v,out.l))&&!strcmp(text,"\xe4\xb8\xad"),"object_declared_toString");
 eq.l=f2;
 WRAP_CHECK(java_basics_call(v,"java/lang/Object","equals","(Ljava/lang/Object;)Z",f1,&eq,1,&out)&&out.i,"object_declared_equals");
 /* Wrong receivers fail closed: null, uninitialized, and another wrapper. */
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","floatValue","()F",0,NULL,0,&out)&&wrapper_exception_is(v,"java/lang/NullPointerException"),"float_null_receiver");
 dvm_clear_exception(v);
 bad_bool=dvm_new_object(v,bool_cls);arg.i=1;
 WRAP_CHECK(bad_bool&&java_basics_call(v,"java/lang/Boolean","<init>","(Z)V",bad_bool,&arg,1,&out),"boolean_ctor_for_wrong_receiver");
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","floatValue","()F",bad_bool,NULL,0,&out)&&wrapper_exception_is(v,"java/lang/IllegalStateException"),"float_wrong_class_receiver");
 dvm_clear_exception(v);
 uninit=dvm_new_object(v,float_cls);
 WRAP_CHECK(java_basics_call(v,"java/lang/Float","doubleValue","()D",uninit,NULL,0,&out)&&wrapper_exception_is(v,"java/lang/IllegalStateException"),"float_uninitialized_receiver");
 dvm_clear_exception(v);
 WRAP_CHECK(java_basics_call(v,"java/lang/Double","doubleValue","()D",f1,NULL,0,&out)&&wrapper_exception_is(v,"java/lang/IllegalStateException"),"double_wrong_class_receiver");
 dvm_clear_exception(v);
 WRAP_CHECK(java_basics_call(v,"java/lang/Character","charValue","()C",d1,NULL,0,&out)&&wrapper_exception_is(v,"java/lang/IllegalStateException"),"character_wrong_class_receiver");
 dvm_clear_exception(v);
 WRAP_CHECK(java_basics_equal(v,f1,bad_bool)==false,"float_not_boolean");
 /* Arrays.toString(Object[]): null, empty, mixed, a real element toString,
  * a throwing element, and fail-closed signature/receiver checks. */
 arg.l=0;
 WRAP_CHECK(java_basics_call(v,"java/util/Arrays","toString","([Ljava/lang/Object;)Ljava/lang/String;",0,&arg,1,&out)&&
  out.l&&(text=dvm_string_utf8(v,out.l))&&!strcmp(text,"null"),"arrays_null");
 arr_empty=dvm_new_array(v,'L',"Ljava/lang/Object;",0);
 WRAP_CHECK(arr_empty!=0,"arrays_empty_array");
 arg.l=arr_empty;
 WRAP_CHECK(java_basics_call(v,"java/util/Arrays","toString","([Ljava/lang/Object;)Ljava/lang/String;",0,&arg,1,&out)&&
  out.l&&(text=dvm_string_utf8(v,out.l))&&!strcmp(text,"[]"),"arrays_empty");
 alpha=dvm_new_string(v,"alpha");
 arg.i=7;
 WRAP_CHECK(java_basics_call(v,"java/lang/Integer","valueOf","(I)Ljava/lang/Integer;",0,&arg,1,&out)&&(boxed7=out.l)!=0,"arrays_integer");
 arr_mixed=dvm_new_array(v,'L',"Ljava/lang/Object;",3);
 WRAP_CHECK(arr_mixed!=0&&alpha!=0,"arrays_mixed_array");
 {
  dvm_ref *slots=arr_mixed?(dvm_ref*)dvm_array_data(v,arr_mixed):NULL;
  WRAP_CHECK(slots!=NULL,"arrays_mixed_data");
  slots[0]=alpha;slots[1]=0;slots[2]=boxed7;
 }
 arg.l=arr_mixed;
 WRAP_CHECK(java_basics_call(v,"java/util/Arrays","toString","([Ljava/lang/Object;)Ljava/lang/String;",0,&arg,1,&out)&&
  out.l&&(text=dvm_string_utf8(v,out.l))&&!strcmp(text,"[alpha, null, 7]"),"arrays_mixed");
 arr_throwing=dvm_new_array(v,'L',"Ljava/lang/Object;",1);
 WRAP_CHECK(arr_throwing!=0,"arrays_throwing_array");
 {
  dvm_ref *slots=(dvm_ref*)dvm_array_data(v,arr_throwing);
  WRAP_CHECK(slots!=NULL,"arrays_throwing_data");
  slots[0]=uninit;
 }
 arg.l=arr_throwing;
 WRAP_CHECK(java_basics_call(v,"java/util/Arrays","toString","([Ljava/lang/Object;)Ljava/lang/String;",0,&arg,1,&out)&&
  wrapper_exception_is(v,"java/lang/IllegalStateException"),"arrays_element_exception");
 dvm_clear_exception(v);
 arg.l=bad_bool;
 WRAP_CHECK(java_basics_call(v,"java/util/Arrays","toString","([Ljava/lang/Object;)Ljava/lang/String;",0,&arg,1,&out)&&
  wrapper_exception_is(v,"java/lang/ClassCastException"),"arrays_non_array");
 dvm_clear_exception(v);
 arg.l=arr_empty;
 WRAP_CHECK(!java_basics_call(v,"java/util/Arrays","toString","([Ljava/lang/String;)Ljava/lang/String;",0,&arg,1,&out)&&
  !dvm_exception(v),"arrays_wrong_signature");
 WRAP_CHECK(!java_basics_call(v,"java/util/Arrays","toString","([Ljava/lang/Object;)Ljava/lang/String;",c1,&arg,1,&out)&&
  !dvm_exception(v),"arrays_receiver_rejected");
 WRAP_CHECK(!dvm_exception(v),"clean_state");
done:
 sdk_trace_fprintf(stderr,"JAVA_BASICS_WRAPPER_SELFTEST %s checks=%u float=1 double=1 character=1 valueof=1 wrong_receiver=1\n",okay?"PASS":"FAIL",checks);
#undef WRAP_CHECK
 return okay;
}
#include "runtime_extra.inc"
