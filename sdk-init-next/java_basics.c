#include "gc_runtime.h"
/* Stateful Java base-class contracts. No SDK/business code is substituted.
 * Java SE contract references are recorded in README.md. All calls occur under
 * the interpreter GIL. AtomicBoolean additionally uses C11 seq_cst atomics. */
#include "java_basics.h"
#include "gc_operation.h"
#include "uri_runtime.h"
#include "numeric_runtime.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>
#include <zlib.h>
#include <unistd.h>

extern int sdk_trace_fprintf(FILE*,const char*,...);
dvm_ref java_basics_boolean_box(struct dvm *v,int value);
enum {PAY_LIST=1,PAY_ITER,PAY_ATOMIC_BOOL,PAY_BOOLEAN,PAY_TLS,PAY_TIMEUNIT,PAY_ATOMIC_INT,PAY_FLOAT,PAY_DOUBLE,PAY_CHARACTER};
struct boxed_integer {int32_t value;};
struct boxed_long {int64_t value;};
/* One pinned cache per VM; never reuse a numeric guest ref in another VM. */
struct integer_cache {struct dvm *vm;dvm_ref refs[256],long_refs[256];struct integer_cache *next;};
static struct integer_cache *integer_caches;
static pthread_mutex_t integer_cache_lock=PTHREAD_MUTEX_INITIALIZER;
static void java_basics_gc_end(struct dvm *);
static bool integer_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 struct integer_cache *p=context;if(!dvm_gil_held(v)||!p||p->vm!=v)return false;
 for(unsigned i=0;i<256;i++){if(p->refs[i])visit(&p->refs[i],user);if(p->long_refs[i])visit(&p->long_refs[i],user);}return true;
}

static struct integer_cache *integer_cache_for(struct dvm *v){
 pthread_mutex_lock(&integer_cache_lock);
 struct integer_cache *p=integer_caches;while(p&&p->vm!=v)p=p->next;
 if(!p){p=calloc(1,sizeof *p);if(p){p->vm=v;
  if(!dvm_gc_root_provider_add(v,"java-basics/Integer-cache",DVM_GC_ROOT_STRONG,integer_gc_roots,p)){free(p);p=NULL;}
  else{p->next=integer_caches;integer_caches=p;}}}
 pthread_mutex_unlock(&integer_cache_lock);return p;
}
void java_basics_integer_vm_end(struct dvm *v){
 java_basics_gc_end(v);
 pthread_mutex_lock(&integer_cache_lock);struct integer_cache **at=&integer_caches;
 while(*at&&(*at)->vm!=v)at=&(*at)->next;
 struct integer_cache *p=*at;if(p)*at=p->next;
 pthread_mutex_unlock(&integer_cache_lock);
 if(p){dvm_gc_root_provider_remove(v,"java-basics/Integer-cache",p);for(unsigned i=0;i<256;i++){if(p->refs[i])dvm_unpin(v,p->refs[i]);if(p->long_refs[i])dvm_unpin(v,p->long_refs[i]);}free(p);}
}
bool integer_runtime_selftest(struct dvm *v);
static bool boxed_integer_copy(struct dvm *v,dvm_ref ref,struct boxed_integer *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,ref))return false;
 struct dvm_object *o=dvm__obj(v,ref);if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Integer")||!o->data)return false;
 *out=*(struct boxed_integer *)o->data;return true;
}
static bool boxed_integer_value_is(struct dvm *v,dvm_ref ref,int32_t expected){struct boxed_integer value;return boxed_integer_copy(v,ref,&value)&&value.value==expected;}

/* Exercise public method lookup/call, not the cache helper itself. */
bool integer_runtime_selftest(struct dvm *v){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/integer-cache-selftest.txt","w");
 bool ok=true;unsigned checks=0;dvm_ref saved=dvm_exception(v);uint64_t steps=v->call_steps,limit=v->step_limit;bool parked=v->parked;
 dvm_clear_exception(v);v->call_steps=0;v->step_limit=2000000;v->parked=false;
 struct dvm_class *k=dvm_find_class_or_external(v,"java/lang/Integer");
 struct dvm_method *factory=k?dvm_find_method(v,k,"valueOf","(I)Ljava/lang/Integer;"):NULL;
 struct dvm_method *ctor=k?dvm_find_method(v,k,"<init>","(I)V"):NULL;
 union dvm_value arg={0},q={0};dvm_ref first[256]={0};uint32_t heap_before=0;size_t used_before=0;
#define ICHECK(label,expr) do{if(!(expr)){if(f)fprintf(f,"FAIL %s line=%d exception=%u\n",label,__LINE__,dvm_exception(v));ok=false;goto done;}checks++;if(f)fprintf(f,"PASS %s\n",label);}while(0)
 ICHECK("method lookup",factory&&ctor);
 for(int i=-128;i<=127;i++){
  arg.i=i;ICHECK("warm cache actual VM call",dvm_call(v,factory,0,&arg,1,&q)&&!dvm_exception(v)&&q.l);
  first[i+128]=q.l;ICHECK("cached payload value",boxed_integer_value_is(v,q.l,i));
 }
 /* Resolve dispatch metadata before measuring allocation-free cached calls. */
 struct dvm_method *intvalue=dvm_find_method(v,k,"intValue","()I");
 struct dvm_method *equals=dvm_find_method(v,k,"equals","(Ljava/lang/Object;)Z");
 ICHECK("Integer declared intValue",intvalue&&dvm_call(v,intvalue,first[135],NULL,0,&q)&&q.i==7&&!dvm_exception(v));
 arg.l=first[135];ICHECK("Integer declared equals",equals&&dvm_call(v,equals,first[135],&arg,1,&q)&&q.i&&!dvm_exception(v));
 if(f)fprintf(f,"NOT_COVERED Number/Object declared metadata dispatch; cache test uses actual Integer declarations\n");
 for(int i=-129;i<=128;i+=257){arg.i=i;ICHECK("outside cache bounds value",dvm_call(v,factory,0,&arg,1,&q)&&q.l&&boxed_integer_value_is(v,q.l,i));}
 arg.i=7;dvm_ref fresh=dvm_new_object(v,k);
 ICHECK("constructor creates distinct identity",fresh&&dvm_call(v,ctor,fresh,&arg,1,&q)&&!dvm_exception(v)&&fresh!=first[135]&&boxed_integer_value_is(v,fresh,7));
 heap_before=v->heap_size;
#ifdef LUNARIA_TASK7_LOGIN_PANEL
 used_before=vm_arena_used();
#endif
 for(unsigned i=0;i<100000;i++){
  arg.i=(int)(i&255)-128;
  if(!dvm_call(v,factory,0,&arg,1,&q)||dvm_exception(v)||q.l!=first[arg.i+128]){ok=false;if(f)fprintf(f,"FAIL repeated cached identity iteration=%u\n",i);goto done;}
 }
 ICHECK("100000 cache calls no new guest objects",v->heap_size==heap_before);
#ifdef LUNARIA_TASK7_LOGIN_PANEL
 ICHECK("100000 cache calls no arena growth",vm_arena_used()==used_before);
#else
 (void)used_before;
#endif
done:
 if(f){fprintf(f,"RESULT ok=%d checks=%u repetitions=100000 heap_before=%u heap_after=%u\n",ok?1:0,checks,heap_before,v->heap_size);fclose(f);}
 sdk_trace_fprintf(stderr,"INTEGER_CACHE_SELFTEST %s checks=%u repetitions=100000\n",ok?"PASS":"FAIL",checks);
 dvm_clear_exception(v);v->exception=saved;v->call_steps=steps;v->step_limit=limit;v->parked=parked;
#undef ICHECK
 return ok;
}
/* Java Integer/Long.toString: invalid radix defaults to ten. Use unsigned
 * subtraction for magnitude so even Long.MIN_VALUE never overflows C signed
 * arithmetic. The longest result is a sign plus 64 binary digits. */
static bool signed_to_string_radix(struct dvm *v,int64_t value,int radix,union dvm_value *out){
 static const char digits[]="0123456789abcdefghijklmnopqrstuvwxyz";
 if(radix<2||radix>36)radix=10;
 char buf[66];char *p=buf+sizeof buf;
 *--p='\0';
 bool negative=value<0;uint64_t magnitude=negative?UINT64_C(0)-(uint64_t)value:(uint64_t)value;
 do{*--p=digits[magnitude%(uint32_t)radix];magnitude/=(uint32_t)radix;}while(magnitude);
 if(negative)*--p='-';
 size_t len=(size_t)(buf+sizeof buf-p-1);dvm_ref text=dvm_new_string_n(v,p,len);
 if(!text){dvm__throw(v,"java/lang/OutOfMemoryError","signed toString");return true;}
 out->l=text;return true;
}
/* Integer/Long.toHexString use the unsigned two's-complement bit pattern,
 * unlike toString(value,16), which prefixes negative values with '-'. */
static bool unsigned_to_hex_string(struct dvm *v,uint64_t bits,union dvm_value *out){
 static const char digits[]="0123456789abcdef";
 char buf[16];size_t start=sizeof buf;
 do{buf[--start]=digits[bits&15];bits>>=4;}while(bits);
 out->l=dvm_new_string_n(v,buf+start,sizeof buf-start);
 if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","toHexString");
 return true;
}
static bool boxed_long_copy(struct dvm *v,dvm_ref ref,struct boxed_long *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,ref))return false;
 struct dvm_object *o=dvm__obj(v,ref);if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Long")||!o->data)return false;
 *out=*(struct boxed_long *)o->data;return true;
}
/* Startup-only HAP test. No render/vsync hook and no guest state mutation. */
bool mem_long_cache_selftest(struct dvm *v){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/mem-long-selftest.txt","w");
 bool ok=true;unsigned checks=0;union dvm_value a={0},x={0},y={0},z={0};
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Long");
 struct dvm_method *factory=cls?dvm_find_method(v,cls,"valueOf","(J)Ljava/lang/Long;"):NULL;
#define LCHECK(expr) do{checks++;if(!(expr)){ok=false;if(f)fprintf(f,"FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));goto end;}}while(0)
 LCHECK(factory);
 for(int i=-128;i<=127;i++){
  a.j=i;LCHECK(dvm_call(v,factory,0,&a,1,&x)&&!dvm_exception(v)&&x.l);
  LCHECK(dvm_call(v,factory,0,&a,1,&y)&&!dvm_exception(v)&&y.l==x.l);
  LCHECK(java_basics_call(v,"java/lang/Long","longValue","()J",x.l,NULL,0,&z)&&z.j==i);
  LCHECK(java_basics_call(v,"java/lang/Long","equals","(Ljava/lang/Object;)Z",x.l,&y,1,&z)&&z.i);
  LCHECK(java_basics_call(v,"java/lang/Long","hashCode","()I",x.l,NULL,0,&z)&&z.i==(int32_t)((uint64_t)a.j^((uint64_t)a.j>>32)));
  LCHECK(java_basics_call(v,"java/lang/Long","toString","()Ljava/lang/String;",x.l,NULL,0,&z));
  char expected[32];snprintf(expected,sizeof expected,"%d",i);LCHECK(dvm_string_utf8(v,z.l)&&!strcmp(dvm_string_utf8(v,z.l),expected));
 }
 {const int64_t outside[]={-129,128,INT64_MIN,INT64_MAX,INT64_C(1000000000000)};
 for(unsigned i=0;i<sizeof outside/sizeof *outside;i++){
  a.j=outside[i];LCHECK(dvm_call(v,factory,0,&a,1,&x)&&!dvm_exception(v)&&x.l);
  LCHECK(dvm_call(v,factory,0,&a,1,&y)&&!dvm_exception(v)&&y.l&&x.l!=y.l);
  LCHECK(java_basics_call(v,"java/lang/Long","longValue","()J",x.l,NULL,0,&z)&&z.j==a.j);
  LCHECK(java_basics_call(v,"java/lang/Long","equals","(Ljava/lang/Object;)Z",x.l,&y,1,&z)&&z.i);
  LCHECK(java_basics_call(v,"java/lang/Long","hashCode","()I",x.l,NULL,0,&z)&&z.i==(int32_t)((uint64_t)a.j^((uint64_t)a.j>>32)));
 }}
 /* new Long(7) remains distinct from valueOf(7). */
 a.j=7;LCHECK(dvm_call(v,factory,0,&a,1,&x)&&x.l);y.l=dvm_new_object(v,cls);LCHECK(y.l&&y.l!=x.l);
 LCHECK(java_basics_call(v,"java/lang/Long","<init>","(J)V",y.l,&a,1,&z));
 LCHECK(java_basics_call(v,"java/lang/Long","longValue","()J",y.l,NULL,0,&z)&&z.j==7);
end:
 if(f){fprintf(f,"LONG_CACHE_SELFTEST %s pid=%d checks=%u cache=-128..127 eager_warm=256 outside_distinct=1 constructor_distinct=1 strong_roots=1 moving=0\n",ok?"PASS":"FAIL",getpid(),checks);fclose(f);}
 return ok&&!dvm_exception(v);
#undef LCHECK
}
/* Called only by sdk_dispatch_selftest while the production external hook is
 * installed. JDK-generated expected text is independent of the C algorithm. */
bool java_basics_hotpath_selftest(struct dvm *v){
 static const struct {bool wide;int64_t value;int32_t radix;const char *expected;} cases[]={
#include "numeric_radix_cases.inc"
 };
 if(!v->hooks.call_external)return false;
 if(!integer_runtime_selftest(v))return false;
 if(!mem_long_cache_selftest(v))return false;
 unsigned checks=0;
 for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
  const char *owner=cases[i].wide?"java/lang/Long":"java/lang/Integer";
  struct dvm_class *cls=dvm_find_class_or_external(v,owner);
  uint64_t bits=(uint64_t)cases[i].value;
  uint32_t slots[3]={(uint32_t)bits,(uint32_t)(bits>>32),(uint32_t)cases[i].radix};
  if(!cases[i].wide)slots[1]=(uint32_t)cases[i].radix;
  union dvm_value out={0};
  bool handled=dvm__call_out(v,cls,"toString",cases[i].wide?"(JI)Ljava/lang/String;":"(II)Ljava/lang/String;",true,false,0,slots,cases[i].wide?3:2,&out);
  const char *got=dvm_string_utf8(v,out.l);size_t length=strlen(cases[i].expected);
  if(!handled||dvm_exception(v)||!got||dvm_string_utf8_length(v,out.l)!=length||memcmp(got,cases[i].expected,length)){
   sdk_trace_fprintf(stderr,"TASK2_RADIX_SELFTEST FAIL case=%zu owner=%s radix=%d got=%s expected=%s exception=%d\n",i,owner,cases[i].radix,got?got:"null",cases[i].expected,dvm_exception(v)!=0);return false;
  }
  checks++;
  /* Decimal overloads must agree with the same JDK oracle. */
  if(cases[i].radix==10){
   if(!dvm__call_out(v,cls,"toString",cases[i].wide?"(J)Ljava/lang/String;":"(I)Ljava/lang/String;",true,false,0,slots,cases[i].wide?2:1,&out)||dvm_exception(v)||!dvm_string_utf8(v,out.l)||strcmp(dvm_string_utf8(v,out.l),cases[i].expected))return false;
   checks++;
  }
 }
 sdk_trace_fprintf(stderr,"TASK2_RADIX_SELFTEST PASS checks=%u oracle=JDK radix=2..36 invalid_radix=decimal Integer=1 Long=1 MIN_MAX=1 actual_external=1\n",checks);
 static const struct{int64_t left,right,minimum,maximum;} math[]={
  {INT64_MIN,INT64_MAX,INT64_MIN,INT64_MAX},{INT64_MAX,INT64_MIN,INT64_MIN,INT64_MAX},
  {-1,INT64_MIN,INT64_MIN,-1},{0,0,0,0},{INT64_MIN,INT64_MIN,INT64_MIN,INT64_MIN},
  {INT64_MAX,INT64_MAX,INT64_MAX,INT64_MAX},{-1,0,-1,0},{1,-1,-1,1}
 };
 for(size_t i=0;i<sizeof math/sizeof math[0];i++){
  uint64_t left=(uint64_t)math[i].left,right=(uint64_t)math[i].right;
  uint32_t slots[]={(uint32_t)left,(uint32_t)(left>>32),(uint32_t)right,(uint32_t)(right>>32)};
  struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Math");union dvm_value out={0};
  if(!dvm__call_out(v,cls,"min","(JJ)J",true,false,0,slots,4,&out)||dvm_exception(v)||out.j!=math[i].minimum)return false;
  if(!dvm__call_out(v,cls,"max","(JJ)J",true,false,0,slots,4,&out)||dvm_exception(v)||out.j!=math[i].maximum)return false;
 }
 sdk_trace_fprintf(stderr,"TASK2_MATH_LONG_SELFTEST PASS checks=16 min=1 max=1 MIN_MAX=1 equal=1 negative=1 actual_external=1\n");
 return true;
}
/* Float/Double/Character use their own payload kinds and a dedicated
 * destructor.  The old shared struct payload is never a valid wrapper and the
 * kind check in each accessor keeps Boolean/AtomicBoolean payloads out. */
struct boxed_float {int kind;float value;};
struct boxed_double {int kind;double value;};
struct boxed_character {int kind;uint32_t value;};
static void boxed_destroy(void *data){free(data);}
/* Explicit ownership certificate for a newly boxed, non-cached timestamp.
 * No subclass/foreign payload/finalizer is eligible for manual retirement. */
bool leak_scope_owned_long(struct dvm *v,uint32_t ref,int64_t value){
 struct dvm_object *o=dvm__obj(v,ref);
 return o&&o->cls&&!o->cls->dex&&o->cls->name&&!strcmp(o->cls->name,"java/lang/Long")&&
  o->kind==DVM_OBJ_PLAIN&&o->data&&o->data_destroy==boxed_destroy&&!o->data_owner&&
  !o->slots&&((struct boxed_long*)o->data)->value==value;
}
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
static bool boxed_float_copy(struct dvm *v,dvm_ref ref,struct boxed_float *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,ref))return false;
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Float")||o->data_destroy!=boxed_destroy)return false;
 struct boxed_float *p=o->data;if(!p||p->kind!=PAY_FLOAT)return false;
 *out=*p;return true;
}
static bool boxed_double_copy(struct dvm *v,dvm_ref ref,struct boxed_double *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,ref))return false;
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Double")||o->data_destroy!=boxed_destroy)return false;
 struct boxed_double *p=o->data;if(!p||p->kind!=PAY_DOUBLE)return false;
 *out=*p;return true;
}
static bool boxed_character_copy(struct dvm *v,dvm_ref ref,struct boxed_character *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,ref))return false;
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/Character")||o->data_destroy!=boxed_destroy)return false;
 struct boxed_character *p=o->data;if(!p||p->kind!=PAY_CHARACTER)return false;
 *out=*p;return true;
}
static bool require_boxed_float_copy(struct dvm *v,dvm_ref ref,const char *where,struct boxed_float *out){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","%s receiver",where);return false;}
 bool ok=boxed_float_copy(v,ref,out);
 if(!ok)dvm__throw(v,"java/lang/IllegalStateException","%s receiver is not an initialized %s",where,where);
 return ok;
}
static bool require_boxed_double_copy(struct dvm *v,dvm_ref ref,const char *where,struct boxed_double *out){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","%s receiver",where);return false;}
 bool ok=boxed_double_copy(v,ref,out);
 if(!ok)dvm__throw(v,"java/lang/IllegalStateException","%s receiver is not an initialized %s",where,where);
 return ok;
}
static bool require_boxed_character_copy(struct dvm *v,dvm_ref ref,const char *where,struct boxed_character *out){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","%s receiver",where);return false;}
 bool ok=boxed_character_copy(v,ref,out);
 if(!ok)dvm__throw(v,"java/lang/IllegalStateException","%s receiver is not an initialized %s",where,where);
 return ok;
}
static bool install_boxed(struct dvm *v,dvm_ref ref,const char *cls_name,const char *where,void *data){
 const dvm_ref gc4_ref_o=(ref); if(!gc_operation_admit(v,gc4_ref_o))return false;
 if(!dvm__obj(v,gc4_ref_o)){dvm__throw(v,"java/lang/NullPointerException","%s receiver",where);free(data);return false;}
 if(!dvm__obj(v,gc4_ref_o)->cls||!dvm__obj(v,gc4_ref_o)->cls->name||strcmp(dvm__obj(v,gc4_ref_o)->cls->name,cls_name)){dvm__throw(v,"java/lang/IllegalStateException","%s receiver type mismatch",where);free(data);return false;}
 if(dvm__obj(v,gc4_ref_o)->data){if(dvm__obj(v,gc4_ref_o)->data_destroy)dvm__obj(v,gc4_ref_o)->data_destroy(dvm__obj(v,gc4_ref_o)->data);else free(dvm__obj(v,gc4_ref_o)->data);}
 dvm__obj(v,gc4_ref_o)->data=data;dvm__obj(v,gc4_ref_o)->data_destroy=boxed_destroy;return true;
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
struct snapshot {size_t owners,n;uint64_t census_generation;dvm_ref values[];};
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
/* GIL-owned diagnostic generation deduplicates a snapshot shared by a COW
 * collection and its independent iterators. No temporary set/allocation. */
size_t java_basics_collection_storage_bytes(struct dvm *v){
 static uint64_t generation;uint64_t current=++generation;size_t bytes=0;
 for(dvm_ref r=1;r<=v->heap_size;r++){struct dvm_object *o=dvm__obj(v,r);if(!o||!o->data||o->data_destroy!=destroy)continue;
  struct payload *p=o->data;if(p->kind!=PAY_LIST&&p->kind!=PAY_ITER)continue;
  bytes+=sizeof *p;if(p->s&&p->s->census_generation!=current){p->s->census_generation=current;bytes+=sizeof *p->s+p->s->n*sizeof *p->s->values;}
 }return bytes;
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
static bool java_basics_equal_gc4_operation(struct dvm *v,dvm_ref a,dvm_ref b){
 if(a==b)return true;if(!a||!b)return false;
 const dvm_ref gc4_ref_x=(a); if(!gc_operation_admit(v,gc4_ref_x))return false;const dvm_ref gc4_ref_y=(b); if(!gc_operation_admit(v,gc4_ref_y))return false;
 if(dvm__obj(v,gc4_ref_x)&&dvm__obj(v,gc4_ref_y)&&dvm__obj(v,gc4_ref_x)->kind==DVM_OBJ_STRING&&dvm__obj(v,gc4_ref_y)->kind==DVM_OBJ_STRING)return dvm__obj(v,gc4_ref_x)->utf8_len==dvm__obj(v,gc4_ref_y)->utf8_len&&!memcmp(dvm__obj(v,gc4_ref_x)->utf8,dvm__obj(v,gc4_ref_y)->utf8,dvm__obj(v,gc4_ref_x)->utf8_len);
 /* java.lang.Class has identity equality.  The VM may materialise the same
  * class through separate Class objects, so compare the underlying class. */
 if(dvm__obj(v,gc4_ref_x)&&dvm__obj(v,gc4_ref_y)&&dvm__obj(v,gc4_ref_x)->kind==DVM_OBJ_CLASS&&dvm__obj(v,gc4_ref_y)->kind==DVM_OBJ_CLASS)
  return dvm__obj(v,gc4_ref_x)->klass&&dvm__obj(v,gc4_ref_y)->klass&&(dvm__obj(v,gc4_ref_x)->klass==dvm__obj(v,gc4_ref_y)->klass||!strcmp(dvm__obj(v,gc4_ref_x)->klass->desc,dvm__obj(v,gc4_ref_y)->klass->desc));
 struct boxed_integer ix_snapshot;struct boxed_integer *ix=boxed_integer_copy(v,a,&ix_snapshot)?&ix_snapshot:NULL;struct boxed_integer iy_snapshot;struct boxed_integer *iy=boxed_integer_copy(v,b,&iy_snapshot)?&iy_snapshot:NULL;
 if(ix||iy)return ix&&iy&&ix->value==iy->value;
 struct payload *bx=boolean_of(v,a),*by=boolean_of(v,b);
 if(bx||by)return bx&&by&&(atomic_load(&bx->flag)!=0)==(atomic_load(&by->flag)!=0);
 struct boxed_float fx_snapshot;struct boxed_float *fx=boxed_float_copy(v,a,&fx_snapshot)?&fx_snapshot:NULL;struct boxed_float fy_snapshot;struct boxed_float *fy=boxed_float_copy(v,b,&fy_snapshot)?&fy_snapshot:NULL;
 if(fx||fy)return fx&&fy&&float_to_int_bits(fx->value)==float_to_int_bits(fy->value);
 struct boxed_double dx_snapshot;struct boxed_double *dx=boxed_double_copy(v,a,&dx_snapshot)?&dx_snapshot:NULL;struct boxed_double dy_snapshot;struct boxed_double *dy=boxed_double_copy(v,b,&dy_snapshot)?&dy_snapshot:NULL;
 if(dx||dy)return dx&&dy&&double_to_long_bits(dx->value)==double_to_long_bits(dy->value);
 struct boxed_character cx_snapshot;struct boxed_character *cx=boxed_character_copy(v,a,&cx_snapshot)?&cx_snapshot:NULL;struct boxed_character cy_snapshot;struct boxed_character *cy=boxed_character_copy(v,b,&cy_snapshot)?&cy_snapshot:NULL;
 if(cx||cy)return cx&&cy&&cx->value==cy->value;
 struct dvm_method *m=dvm__obj(v,gc4_ref_x)?dvm_find_method(v,dvm__obj(v,gc4_ref_x)->cls,"equals","(Ljava/lang/Object;)Z"):NULL;
 if(m){union dvm_value arg={.l=b},r={0};return dvm_call(v,m,a,&arg,1,&r)&&r.i;}
 if(dvm__obj(v,gc4_ref_x)&&dvm__obj(v,gc4_ref_x)->cls&&dvm__obj(v,gc4_ref_x)->cls->external&&strcmp(dvm__obj(v,gc4_ref_x)->cls->name,"java/lang/Object")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","equals contract not implemented for external class %s",dvm__obj(v,gc4_ref_x)->cls->name);
 }
 /* java.lang.Object.equals defaults to reference identity, already tested. */
 return false;
}
bool java_basics_equal(struct dvm *v,dvm_ref a,dvm_ref b){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool ok=java_basics_equal_gc4_operation(v,a,b);
 return ok;
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
static bool locale_copy(struct dvm *v,dvm_ref ref,struct locale_value *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,ref))return false;
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/util/Locale")||o->data_destroy!=locale_destroy)return false;
 if(!o->data)return false;
 *out=*(struct locale_value *)o->data;return true;
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
/* Hoisted from function-local statics so moving-root visitors update the actual holder. */
static uint64_t websettings_serial;
static dvm_ref websettings_values[9];
static struct dvm *settings_uri_vm,*java_basics_gc_vm;
static dvm_ref settings_uri[3];
static bool java_basics_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 (void)context;if(!dvm_gil_held(v)||java_basics_gc_vm!=v)return false;
 if(locale_statics_vm==v)for(unsigned i=0;i<3;i++)if(locale_statics[i])visit(&locale_statics[i],user);
 if(boolean_statics_vm==v)for(unsigned i=0;i<2;i++)if(boolean_statics[i])visit(&boolean_statics[i],user);
 if(file_statics_vm==v)for(unsigned i=0;i<2;i++)if(file_statics[i])visit(&file_statics[i],user);
 if(proxy_statics_vm==v){dvm_ref *a[]={&proxy_none,&proxy_direct,&proxy_http,&proxy_socks};for(unsigned i=0;i<4;i++)if(*a[i])visit(a[i],user);}
 if(websettings_serial==v->serial)for(unsigned i=0;i<9;i++)if(websettings_values[i])visit(&websettings_values[i],user);
 if(settings_uri_vm==v)for(unsigned i=0;i<3;i++)if(settings_uri[i])visit(&settings_uri[i],user);
 return true;
}
static void java_basics_gc_end(struct dvm *v){
 if(java_basics_gc_vm!=v)return;
 dvm_gc_root_provider_remove(v,"java-basics/constants",&java_basics_gc_vm);
 java_basics_gc_vm=NULL;locale_statics_vm=boolean_statics_vm=file_statics_vm=proxy_statics_vm=settings_uri_vm=NULL;
 memset(locale_statics,0,sizeof locale_statics);memset(boolean_statics,0,sizeof boolean_statics);memset(file_statics,0,sizeof file_statics);
 proxy_none=proxy_direct=proxy_http=proxy_socks=0;websettings_serial=0;
 memset(websettings_values,0,sizeof websettings_values);memset(settings_uri,0,sizeof settings_uri);
}

/* Called from sdk_init_vm_start on every new VM so cached singletons from a
 * destroyed VM can never be observed by a later VM that reuses the address. */
void java_basics_reset(struct dvm *v){
 java_basics_gc_vm=v;
 if(!dvm_gc_root_provider_add(v,"java-basics/constants",DVM_GC_ROOT_STRONG,java_basics_gc_roots,&java_basics_gc_vm)){dvm__throw(v,"java/lang/OutOfMemoryError","Java constant GC roots");return;}

 (void)v;boolean_statics_vm=NULL;boolean_statics[0]=boolean_statics[1]=0;
 locale_statics_vm=NULL;locale_statics[0]=locale_statics[1]=locale_statics[2]=0;
 file_statics_vm=NULL;file_statics[0]=file_statics[1]=0;
 proxy_statics_vm=NULL;proxy_none=proxy_direct=proxy_http=proxy_socks=0;
}
static bool java_basics_static_gc4_operation(struct dvm *v,const char *c,const char *f,const char *t,union dvm_value *out){
 if((!strcmp(c,"android/webkit/WebSettings$ZoomDensity")&&(!strcmp(f,"FAR")||!strcmp(f,"MEDIUM")||!strcmp(f,"CLOSE")))||(!strcmp(c,"android/webkit/WebSettings$LayoutAlgorithm")&&(!strcmp(f,"NARROW_COLUMNS")||!strcmp(f,"NORMAL")||!strcmp(f,"SINGLE_COLUMN")))||(!strcmp(c,"android/webkit/WebSettings$PluginState")&&(!strcmp(f,"OFF")||!strcmp(f,"ON")||!strcmp(f,"ON_DEMAND")))){
  if(websettings_serial!=v->serial){websettings_serial=v->serial;memset(websettings_values,0,sizeof websettings_values);}
  const char *names[]={"FAR","MEDIUM","CLOSE","NARROW_COLUMNS","NORMAL","SINGLE_COLUMN","OFF","ON","ON_DEMAND"};int i=0;while(i<9&&strcmp(f,names[i]))i++;
  if(!websettings_values[i]){websettings_values[i]=dvm_new_object(v,dvm_find_class_or_external(v,c));dvm_pin(v,websettings_values[i]);}out->l=websettings_values[i];return true;
 }

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
   const dvm_ref gc4_ref_object=(ref); if(!gc_operation_admit(v,gc4_ref_object))return true;
   dvm__obj(v,gc4_ref_object)->data=value;dvm__obj(v,gc4_ref_object)->data_destroy=locale_destroy;
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
 extern bool content_runtime_constant(struct dvm *,const char *,const char *,const char *,union dvm_value *) __attribute__((weak));
 if(content_runtime_constant&&content_runtime_constant(v,c,f,t,out))return true;
 if(!strcmp(c,"android/provider/Settings$System")||!strcmp(c,"android/provider/Settings$Global")||!strcmp(c,"android/provider/Settings$Secure")){
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
bool java_basics_static(struct dvm *v,const char *c,const char *f,const char *t,union dvm_value *out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return true;
 bool ok=java_basics_static_gc4_operation(v,c,f,t,out);
 gc_operation_result(&operation,"()Ljava/lang/Object;",ok&&out&&t&&(t[0]=='L'||t[0]=='[')?out->l:0);
 return ok;
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
static bool java_basics_timeunit_nanos_gc4_operation(struct dvm *v,dvm_ref unit,uint64_t *nanos){
 const dvm_ref gc4_ref_o=(unit); if(!gc_operation_admit(v,gc4_ref_o))return false;struct payload *p=dvm__obj(v,gc4_ref_o)?dvm__obj(v,gc4_ref_o)->data:NULL;
 if(!dvm__obj(v,gc4_ref_o)||dvm__obj(v,gc4_ref_o)->data_destroy!=destroy||!p||p->kind!=PAY_TIMEUNIT)return false;
 *nanos=p->unit_nanos;return *nanos!=0;
}
bool java_basics_timeunit_nanos(struct dvm *v,dvm_ref unit,uint64_t *nanos){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool ok=java_basics_timeunit_nanos_gc4_operation(v,unit,nanos);
 return ok;
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
 const dvm_ref gc4_ref_o=(ref); if(!gc_operation_admit(v,gc4_ref_o))return false;
 if(!dvm__obj(v,gc4_ref_o)){dvm__throw(v,"java/lang/IllegalStateException","Arrays.toString element handle");return false;}
 if(dvm__obj(v,gc4_ref_o)->kind==DVM_OBJ_STRING){
  if(!dvm__obj(v,gc4_ref_o)->utf8){dvm__throw(v,"java/lang/IllegalStateException","Arrays.toString String element");return false;}
  *text=dvm__obj(v,gc4_ref_o)->utf8;*length=dvm__obj(v,gc4_ref_o)->utf8_len;return true;
 }
 const char *cls=dvm__obj(v,gc4_ref_o)->cls&&dvm__obj(v,gc4_ref_o)->cls->name?dvm__obj(v,gc4_ref_o)->cls->name:NULL;
 if(!cls){dvm__throw(v,"java/lang/IllegalStateException","Arrays.toString element class");return false;}
 union dvm_value result={0};
 bool handled=java_basics_call(v,cls,"toString","()Ljava/lang/String;",ref,NULL,0,&result);
 if(dvm_exception(v))return false;
 if(!handled){
  struct dvm_method *method=dvm_find_method(v,dvm__obj(v,gc4_ref_o)->cls,"toString","()Ljava/lang/String;");
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
 const dvm_ref gc4_ref_rendered=(result.l); if(!gc_operation_admit(v,gc4_ref_rendered))return false;
 if(!dvm__obj(v,gc4_ref_rendered)||dvm__obj(v,gc4_ref_rendered)->kind!=DVM_OBJ_STRING||!dvm__obj(v,gc4_ref_rendered)->utf8){
  dvm__throw(v,"java/lang/ClassCastException","Arrays.toString element toString did not return a String");
  return false;
 }
 *text=dvm__obj(v,gc4_ref_rendered)->utf8;*length=dvm__obj(v,gc4_ref_rendered)->utf8_len;
 return true;
}
static bool arrays_to_string(struct dvm *v,dvm_ref array,union dvm_value *out){
 if(!array){
  out->l=dvm_new_string(v,"null");
  if(!out->l){dvm__throw(v,"java/lang/OutOfMemoryError","Arrays.toString null");return true;}
  return true;
 }
 const dvm_ref gc4_ref_arr=(array); if(!gc_operation_admit(v,gc4_ref_arr))return true;
 if(!dvm__obj(v,gc4_ref_arr)||dvm__obj(v,gc4_ref_arr)->kind!=DVM_OBJ_ARRAY||(dvm__obj(v,gc4_ref_arr)->elem_kind!='L'&&dvm__obj(v,gc4_ref_arr)->elem_kind!='[')){
  dvm__throw(v,"java/lang/ClassCastException","Arrays.toString requires an Object array");
  return true;
 }
 uint32_t count=dvm__obj(v,gc4_ref_arr)->length;
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
struct java_inflater {z_stream stream;dvm_ref input;uint32_t offset,remaining;bool ended,finished,dictionary;};
static void inflater_destroy(void *data){struct java_inflater *p=data;if(p){if(!p->ended)inflateEnd(&p->stream);free(p);}}
static bool inflater_bytes(struct dvm *v,dvm_ref ref,int64_t offset,int64_t length,uint8_t **bytes){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","Inflater bytes");return false;}
 const dvm_ref gc4_ref_o=(ref); if(!gc_operation_admit(v,gc4_ref_o))return false;
 if(!dvm__obj(v,gc4_ref_o)||dvm__obj(v,gc4_ref_o)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc4_ref_o)->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","Inflater byte array");return false;}
 if(offset<0||length<0||offset+length>dvm__obj(v,gc4_ref_o)->length){dvm__throw(v,"java/lang/ArrayIndexOutOfBoundsException","Inflater range");return false;}
 *bytes=dvm_array_data(v,ref);if(length)*bytes+=offset;return true;
}
static bool inflater_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 bool ctor=IS("<init>","()V")||IS("<init>","(Z)V"),end=IS("end","()V"),reset=IS("reset","()V");
 bool input=IS("setInput","([B)V")||IS("setInput","([BII)V"),dict=IS("setDictionary","([B)V")||IS("setDictionary","([BII)V");
 bool decode=IS("inflate","([B)I")||IS("inflate","([BII)I");
 bool query=IS("needsInput","()Z")||IS("needsDictionary","()Z")||IS("finished","()Z")||IS("getRemaining","()I")||IS("getBytesRead","()J")||IS("getBytesWritten","()J")||IS("getTotalIn","()I")||IS("getTotalOut","()I")||IS("getAdler","()I");
 if(!ctor&&!end&&!reset&&!input&&!dict&&!decode&&!query)return false;
 const dvm_ref gc4_ref_o=(self); if(!gc_operation_admit(v,gc4_ref_o))return true;if(!dvm__obj(v,gc4_ref_o)){dvm__throw(v,"java/lang/NullPointerException","Inflater receiver");return true;}
 if(ctor){if(dvm__obj(v,gc4_ref_o)->data){dvm__throw(v,"java/lang/IllegalStateException","Inflater initialized");return true;}struct java_inflater *p=calloc(1,sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","Inflater state");return true;}int rc=inflateInit2(&p->stream,nargs&&a[0].i?-MAX_WBITS:MAX_WBITS);if(rc!=Z_OK){free(p);dvm__throw(v,"java/lang/OutOfMemoryError","Inflater initialization");return true;}dvm__obj(v,gc4_ref_o)->data=p;dvm__obj(v,gc4_ref_o)->data_destroy=inflater_destroy;return true;}
 struct java_inflater *p=dvm__obj(v,gc4_ref_o)->data;if(!p){dvm__throw(v,"java/lang/IllegalStateException","Inflater uninitialized");return true;}
 if(end){if(!p->ended){inflateEnd(&p->stream);p->ended=true;p->input=0;p->remaining=0;}return true;}
 if(IS("needsInput","()Z")){out->i=p->remaining==0;return true;}if(IS("finished","()Z")){out->i=p->finished;return true;}if(IS("needsDictionary","()Z")){out->i=p->dictionary;return true;}if(IS("getRemaining","()I")){out->i=p->remaining;return true;}
 if(p->ended){dvm__throw(v,"java/lang/NullPointerException","Inflater closed");return true;}
 if(reset){int rc=inflateReset(&p->stream);if(rc!=Z_OK){dvm__throw(v,"java/lang/IllegalStateException","Inflater reset");return true;}p->input=0;p->offset=p->remaining=0;p->finished=p->dictionary=false;return true;}
 if(IS("getBytesRead","()J")){out->j=p->stream.total_in;return true;}if(IS("getBytesWritten","()J")){out->j=p->stream.total_out;return true;}if(IS("getTotalIn","()I")){out->i=(int32_t)p->stream.total_in;return true;}if(IS("getTotalOut","()I")){out->i=(int32_t)p->stream.total_out;return true;}if(IS("getAdler","()I")){out->i=(int32_t)p->stream.adler;return true;}
 int64_t offset=nargs==3?a[1].i:0,length=nargs==3?a[2].i:dvm_array_length(v,a[0].l);uint8_t *bytes=NULL;
 if(!inflater_bytes(v,a[0].l,offset,length,&bytes))return true;
 if(input){p->input=a[0].l;p->offset=offset;p->remaining=length;return true;}
 if(dict){int rc=inflateSetDictionary(&p->stream,bytes,(uInt)length);if(rc!=Z_OK)dvm__throw(v,"java/lang/IllegalArgumentException","Inflater dictionary mismatch");else p->dictionary=false;return true;}
 if(!length)return true;
 uint8_t *source=NULL;if(p->remaining&&!inflater_bytes(v,p->input,p->offset,p->remaining,&source))return true;
 p->stream.next_in=source;p->stream.avail_in=p->remaining;p->stream.next_out=bytes;p->stream.avail_out=(uInt)length;
 int rc=inflate(&p->stream,Z_NO_FLUSH);uint32_t consumed=p->remaining-p->stream.avail_in;p->offset+=consumed;p->remaining=p->stream.avail_in;out->i=(int)(length-p->stream.avail_out);p->stream.next_in=p->stream.next_out=NULL;
 if(rc==Z_STREAM_END)p->finished=true;else if(rc==Z_NEED_DICT)p->dictionary=true;else if(rc==Z_DATA_ERROR)dvm__throw(v,"java/util/zip/DataFormatException",p->stream.msg?p->stream.msg:"invalid compressed data");else if(rc==Z_MEM_ERROR)dvm__throw(v,"java/lang/OutOfMemoryError","Inflater decode");else if(rc!=Z_OK&&rc!=Z_BUF_ERROR)dvm__throw(v,"java/lang/IllegalStateException","Inflater decode state");return true;
}
/* java.util.zip CRC32 uses the standard reflected IEEE polynomial. Keep the
 * pre-complement state per receiver, including across multiple update calls. */
static bool crc32_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 bool ctor=IS("<init>","()V"),reset=IS("reset","()V"),get=IS("getValue","()J");
 bool one=IS("update","(I)V"),all=IS("update","([B)V"),slice=IS("update","([BII)V");
 if(!ctor&&!reset&&!get&&!one&&!all&&!slice)return false;
 const dvm_ref gc4_ref_o=(self); if(!gc_operation_admit(v,gc4_ref_o))return true;
 if(!dvm__obj(v,gc4_ref_o)){dvm__throw(v,"java/lang/NullPointerException","CRC32 receiver");return true;}
 if(ctor){if(nargs)return false;if(dvm__obj(v,gc4_ref_o)->data){dvm__throw(v,"java/lang/IllegalStateException","CRC32 initialized");return true;}uint32_t *state=malloc(sizeof *state);if(!state){dvm__throw(v,"java/lang/OutOfMemoryError","CRC32 state");return true;}*state=UINT32_MAX;dvm__obj(v,gc4_ref_o)->data=state;dvm__obj(v,gc4_ref_o)->data_destroy=boxed_destroy;return true;}
 uint32_t *state=dvm__obj(v,gc4_ref_o)->data;if(!state){dvm__throw(v,"java/lang/IllegalStateException","CRC32 uninitialized");return true;}
 if(reset){*state=UINT32_MAX;return true;}if(get){out->j=(uint32_t)(*state^UINT32_MAX);return true;}
 const uint8_t *bytes=NULL;uint8_t byte;size_t count=0;
 if(one){byte=(uint8_t)a[0].i;bytes=&byte;count=1;}
 else{
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","CRC32 bytes");return true;}
  const dvm_ref gc4_ref_arr=(a[0].l); if(!gc_operation_admit(v,gc4_ref_arr))return true;
  if(!dvm__obj(v,gc4_ref_arr)||dvm__obj(v,gc4_ref_arr)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc4_ref_arr)->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","CRC32 byte array");return true;}
  int64_t offset=slice?a[1].i:0,length=slice?a[2].i:dvm__obj(v,gc4_ref_arr)->length;
  if(offset<0||length<0||offset+length>dvm__obj(v,gc4_ref_arr)->length){dvm__throw(v,"java/lang/ArrayIndexOutOfBoundsException","CRC32 range");return true;}
  count=(size_t)length;bytes=(const uint8_t*)dvm_array_data(v,a[0].l);if(count)bytes+=offset;
 }
 uint32_t crc=*state;for(size_t i=0;i<count;i++){crc^=bytes[i];for(int bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?UINT32_C(0xedb88320):0);}*state=crc;return true;
}
static bool java_basics_call_gc4_operation(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;memset(out,0,sizeof *out);
 /* Number's base constructor has no fields or side effects. */
 if(!strcmp(c,"java/lang/Number")&&IS("<init>","()V")&&nargs==0){
  const dvm_ref gc4_ref_receiver=(self); if(!gc_operation_admit(v,gc4_ref_receiver))return true;
  if(!dvm__obj(v,gc4_ref_receiver))dvm__throw(v,"java/lang/NullPointerException","Number constructor receiver");
  else if(!dvm__class_assignable(v,dvm__obj(v,gc4_ref_receiver)->cls,dvm_find_class_or_external(v,"java/lang/Number")))dvm__throw(v,"java/lang/IllegalArgumentException","Number constructor receiver type");
  return true;
 }
 if(!strcmp(c,"java/util/zip/Inflater"))return inflater_call(v,m,sig,self,a,nargs,out);
 if(!strcmp(c,"java/util/zip/CRC32"))return crc32_call(v,m,sig,self,a,nargs,out);
 if(!strcmp(c,"java/lang/Math")&&!self&&IS("max","(II)I")&&nargs==2){out->i=a[0].i>=a[1].i?a[0].i:a[1].i;return true;}
 if(!strcmp(c,"java/lang/Math")&&!self&&IS("max","(JJ)J")&&nargs==2){out->j=a[0].j>=a[1].j?a[0].j:a[1].j;return true;}
 /* Virtual calls declared on Number/Object/Comparable carry no method table
  * for an external concrete class.  Re-dispatch the wrapper methods through
  * the receiver's actual wrapper class; static entry points keep the declared
  * class and are not affected.  Byte/Short are handled by small_boxes_call,
  * which runs before this module.  Integer/Long/AtomicInteger are Number
  * subclasses, so Number.intValue()/longValue()/doubleValue() declared calls
  * reach the concrete implementation only through this re-dispatch. */
 if(self&&(!strcmp(c,"java/lang/Number")||!strcmp(c,"java/lang/Object")||!strcmp(c,"java/lang/Comparable"))&&strcmp(m,"<init>")&&strcmp(m,"valueOf")){
  const dvm_ref gc4_ref_receiver=(self); if(!gc_operation_admit(v,gc4_ref_receiver))return true;
  const char *actual=dvm__obj(v,gc4_ref_receiver)&&dvm__obj(v,gc4_ref_receiver)->cls?dvm__obj(v,gc4_ref_receiver)->cls->name:NULL;
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
  struct locale_value value_snapshot;struct locale_value *value=locale_copy(v,self,&value_snapshot)?&value_snapshot:NULL;
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
  if(!self&&IS("toString","(Z)Ljava/lang/String;")&&nargs==1){out->l=dvm_new_string(v,a[0].i?"true":"false");return true;}
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
  if(IS("toHexString","(I)Ljava/lang/String;")&&nargs==1)return unsigned_to_hex_string(v,(uint32_t)a[0].i,out);
  if(IS("valueOf","(Ljava/lang/String;)Ljava/lang/Integer;")||IS("valueOf","(Ljava/lang/String;I)Ljava/lang/Integer;")){
   bool radix=strstr(sig,";I)")!=NULL;union dvm_value parsed={0};
   if(!numeric_runtime_call(v,c,"parseInt",radix?"(Ljava/lang/String;I)I":"(Ljava/lang/String;)I",0,a,nargs,&parsed))return false;
   if(dvm_exception(v))return true;
   return java_basics_call(v,c,"valueOf","(I)Ljava/lang/Integer;",0,&parsed,1,out);
  }
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
    struct integer_cache *cache=NULL;dvm_ref *slot=NULL;
    if(a[0].i>=-128&&a[0].i<=127){
     cache=integer_cache_for(v);if(!cache){dvm__throw(v,"java/lang/OutOfMemoryError","Integer cache");return true;}
     slot=&cache->refs[a[0].i+128];if(*slot){out->l=*slot;return true;}
    }
   struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Integer");dvm_ref ref=dvm_new_object(v,cls);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Integer.valueOf");return true;}
    struct boxed_integer *box=calloc(1,sizeof *box);if(!box){dvm__discard_temporary(v,ref);dvm__throw(v,"java/lang/OutOfMemoryError","Integer.valueOf payload");return true;}
    box->value=a[0].i;dvm__obj(v,ref)->data=box;dvm__obj(v,ref)->data_destroy=boxed_destroy;if(slot){*slot=ref;dvm_pin(v,ref);}out->l=ref;return true;
  }
  /* 静态调用 self==0，必须在这里返回，不能落入下面的 boxed_integer_of(self)。 */
  if(tostring_i){if(nargs!=1)return false;return signed_to_string_radix(v,a[0].i,10,out);}
  if(tostring_ii){if(nargs!=2)return false;return signed_to_string_radix(v,a[0].i,a[1].i,out);}
  if(ctor){const dvm_ref gc4_ref_o=(self); if(!gc_operation_admit(v,gc4_ref_o))return true;if(!dvm__obj(v,gc4_ref_o)){dvm__throw(v,"java/lang/NullPointerException","Integer receiver");return true;}struct boxed_integer *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Integer payload");return true;}box->value=a[0].i;free(dvm__obj(v,gc4_ref_o)->data);dvm__obj(v,gc4_ref_o)->data=box;dvm__obj(v,gc4_ref_o)->data_destroy=boxed_destroy;return true;}
  struct boxed_integer box_snapshot;struct boxed_integer *box=boxed_integer_copy(v,self,&box_snapshot)?&box_snapshot:NULL;if(!box){dvm__throw(v,"java/lang/IllegalStateException","Integer not initialized");return true;}
  if(value){if(IS("intValue","()I")||IS("hashCode","()I"))out->i=box->value;else if(IS("byteValue","()B"))out->i=(int8_t)box->value;else if(IS("shortValue","()S"))out->i=(int16_t)box->value;else if(IS("longValue","()J"))out->j=box->value;else if(IS("floatValue","()F"))out->f=(float)box->value;else out->d=(double)box->value;return true;}
  if(equals){struct boxed_integer other_snapshot;struct boxed_integer *other=boxed_integer_copy(v,a[0].l,&other_snapshot)?&other_snapshot:NULL;out->i=other&&other->value==box->value;return true;}
  if(compare){struct boxed_integer other_snapshot;struct boxed_integer *other=boxed_integer_copy(v,a[0].l,&other_snapshot)?&other_snapshot:NULL;if(!other){dvm__throw(v,"java/lang/NullPointerException","Integer.compareTo");return true;}out->i=box->value<other->value?-1:box->value>other->value?1:0;return true;}
  char text[32];int n=snprintf(text,sizeof text,"%d",box->value);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/Long")){
  if(IS("toHexString","(J)Ljava/lang/String;")&&nargs==1)return unsigned_to_hex_string(v,(uint64_t)a[0].j,out);
  bool value_of=IS("valueOf","(J)Ljava/lang/Long;");
  bool ctor=IS("<init>","(J)V");
  bool value=IS("longValue","()J")||IS("intValue","()I")||IS("shortValue","()S")||IS("byteValue","()B")||IS("floatValue","()F")||IS("doubleValue","()D")||IS("hashCode","()I");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool compare=IS("compareTo","(Ljava/lang/Long;)I");
  bool tostring=IS("toString","()Ljava/lang/String;");
  bool tostring_j=IS("toString","(J)Ljava/lang/String;");
  bool tostring_ji=IS("toString","(JI)Ljava/lang/String;");
  if(!value_of&&!ctor&&!value&&!equals&&!compare&&!tostring&&!tostring_j&&!tostring_ji)return false;
  if(tostring_j){if(nargs!=1)return false;return signed_to_string_radix(v,a[0].j,10,out);}
  if(tostring_ji){if(nargs!=2)return false;return signed_to_string_radix(v,a[0].j,a[1].i,out);}
  if(value_of){
   dvm_ref *slot=NULL;
   if(a[0].j>=-128&&a[0].j<=127){
    struct integer_cache *cache=integer_cache_for(v);
    if(!cache){dvm__throw(v,"java/lang/OutOfMemoryError","Long cache");return true;}
    slot=&cache->long_refs[(unsigned)(a[0].j+128)];
    if(*slot){out->l=*slot;return true;}
   }
   struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Long");dvm_ref ref=dvm_new_object(v,cls);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Long.valueOf");return true;}
   struct boxed_long *box=calloc(1,sizeof *box);if(!box){dvm__discard_temporary(v,ref);dvm__throw(v,"java/lang/OutOfMemoryError","Long.valueOf payload");return true;}
   box->value=a[0].j;dvm__obj(v,ref)->data=box;dvm__obj(v,ref)->data_destroy=boxed_destroy;
   if(slot){*slot=ref;dvm_pin(v,ref);}out->l=ref;return true;
  }
  if(ctor){
   const dvm_ref gc4_ref_o=(self); if(!gc_operation_admit(v,gc4_ref_o))return true;if(!dvm__obj(v,gc4_ref_o)){dvm__throw(v,"java/lang/NullPointerException","Long receiver");return true;}
   free(dvm__obj(v,gc4_ref_o)->data);struct boxed_long *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Long payload");return true;}box->value=a[0].j;dvm__obj(v,gc4_ref_o)->data=box;dvm__obj(v,gc4_ref_o)->data_destroy=boxed_destroy;return true;
  }
  struct boxed_long box_snapshot;struct boxed_long *box=boxed_long_copy(v,self,&box_snapshot)?&box_snapshot:NULL;if(!box){dvm__throw(v,"java/lang/IllegalStateException","Long not initialized");return true;}
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
  if(equals){struct boxed_long other_snapshot;struct boxed_long *other=boxed_long_copy(v,a[0].l,&other_snapshot)?&other_snapshot:NULL;out->i=other&&other->value==box->value;return true;}
  if(compare){struct boxed_long other_snapshot;struct boxed_long *other=boxed_long_copy(v,a[0].l,&other_snapshot)?&other_snapshot:NULL;if(!other){dvm__throw(v,"java/lang/NullPointerException","Long.compareTo");return true;}out->i=box->value<other->value?-1:box->value>other->value?1:0;return true;}
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
  struct boxed_float box_snapshot;struct boxed_float *box=require_boxed_float_copy(v,self,"Float",&box_snapshot)?&box_snapshot:NULL;if(!box)return true;
  if(IS("floatValue","()F")){out->f=box->value;return true;}
  if(IS("doubleValue","()D")){out->d=(double)box->value;return true;}
  if(IS("intValue","()I")){out->i=java_float_to_int(box->value);return true;}
  if(IS("longValue","()J")){out->j=java_float_to_long(box->value);return true;}
  if(hash){out->i=(int32_t)float_to_int_bits(box->value);return true;}
  if(equals){struct boxed_float other_snapshot;struct boxed_float *other=boxed_float_copy(v,a[0].l,&other_snapshot)?&other_snapshot:NULL;out->i=other&&float_to_int_bits(other->value)==float_to_int_bits(box->value);return true;}
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
  struct boxed_double box_snapshot;struct boxed_double *box=require_boxed_double_copy(v,self,"Double",&box_snapshot)?&box_snapshot:NULL;if(!box)return true;
  if(IS("doubleValue","()D")){out->d=box->value;return true;}
  if(IS("floatValue","()F")){out->f=(float)box->value;return true;}
  if(IS("intValue","()I")){out->i=java_double_to_int(box->value);return true;}
  if(IS("longValue","()J")){out->j=java_double_to_long(box->value);return true;}
  if(hash){uint64_t bits=double_to_long_bits(box->value);out->i=(int32_t)(uint32_t)(bits^(bits>>32));return true;}
  if(equals){struct boxed_double other_snapshot;struct boxed_double *other=boxed_double_copy(v,a[0].l,&other_snapshot)?&other_snapshot:NULL;out->i=other&&double_to_long_bits(other->value)==double_to_long_bits(box->value);return true;}
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
  struct boxed_character box_snapshot;struct boxed_character *box=require_boxed_character_copy(v,self,"Character",&box_snapshot)?&box_snapshot:NULL;if(!box)return true;
  if(IS("charValue","()C")||IS("intValue","()I")){out->i=(int32_t)box->value;return true;}
  if(IS("longValue","()J")){out->j=(int64_t)box->value;return true;}
  if(IS("floatValue","()F")){out->f=(float)box->value;return true;}
  if(IS("doubleValue","()D")){out->d=(double)box->value;return true;}
  if(hash){out->i=(int32_t)box->value;return true;}
  if(equals){struct boxed_character other_snapshot;struct boxed_character *other=boxed_character_copy(v,a[0].l,&other_snapshot)?&other_snapshot:NULL;out->i=other&&other->value==box->value;return true;}
  char text[8];int n=java_character_text(box->value,text,sizeof text);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/String")&&IS("equals","(Ljava/lang/Object;)Z")){
  const dvm_ref gc4_ref_left=(self); if(!gc_operation_admit(v,gc4_ref_left))return true;const dvm_ref gc4_ref_right=(a[0].l); if(!gc_operation_admit(v,gc4_ref_right))return true;
  out->i=dvm__obj(v,gc4_ref_left)&&dvm__obj(v,gc4_ref_right)&&dvm__obj(v,gc4_ref_left)->kind==DVM_OBJ_STRING&&dvm__obj(v,gc4_ref_right)->kind==DVM_OBJ_STRING&&
         dvm__obj(v,gc4_ref_left)->utf8_len==dvm__obj(v,gc4_ref_right)->utf8_len&&!memcmp(dvm__obj(v,gc4_ref_left)->utf8,dvm__obj(v,gc4_ref_right)->utf8,dvm__obj(v,gc4_ref_left)->utf8_len);
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
   if(IS("get","()Ljava/lang/Object;")){const dvm_ref gc4_ref_o=(self); if(!gc_operation_admit(v,gc4_ref_o))return true;struct dvm_method *init=dvm_find_method(v,dvm__obj(v,gc4_ref_o)->cls,"initialValue","()Ljava/lang/Object;");if(init&&(init->has_code||init->builtin)){union dvm_value r={0};if(dvm_call(v,init,self,NULL,0,&r))(*slot)->value=r.l;}}}
  if(IS("set","(Ljava/lang/Object;)V"))(*slot)->value=a[0].l;else out->l=(*slot)->value;
  return true;
 }
 const dvm_ref gc4_ref_receiver=(self?(self):0); if(!gc_operation_admit(v,gc4_ref_receiver))return true;
 bool list=!strcmp(c,"java/util/concurrent/CopyOnWriteArrayList")||(dvm__obj(v,gc4_ref_receiver)&&dvm__obj(v,gc4_ref_receiver)->cls&&!strcmp(dvm__obj(v,gc4_ref_receiver)->cls->name,"java/util/concurrent/CopyOnWriteArrayList"));
 bool iterator=dvm__obj(v,gc4_ref_receiver)&&dvm__obj(v,gc4_ref_receiver)->cls&&!strcmp(dvm__obj(v,gc4_ref_receiver)->cls->name,"java/util/concurrent/CopyOnWriteArrayList$COWIterator");
 /* CopyOnWriteArraySet is the Set facade over the same copy-on-write array:
  * add() appends only when java_basics_equal() says the element is absent,
  * and every mutation replaces the snapshot so live iterators keep the old
  * one (Laser TaskManager's <clinit> reached this gap in 72bq/72br). */
 bool cow_set=!strcmp(c,"java/util/concurrent/CopyOnWriteArraySet")||(dvm__obj(v,gc4_ref_receiver)&&dvm__obj(v,gc4_ref_receiver)->cls&&!strcmp(dvm__obj(v,gc4_ref_receiver)->cls->name,"java/util/concurrent/CopyOnWriteArraySet"));
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
bool java_basics_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return true;
 bool ok=java_basics_call_gc4_operation(v,c,m,sig,self,a,nargs,out);
 const char *ret=sig?strchr(sig,')'):NULL;
 gc_operation_result(&operation,sig,ok&&out&&ret&&(ret[1]=='L'||ret[1]=='[')?out->l:0);
 return ok;
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
 union dvm_value max_args[2]={{.j=INT64_MIN},{.j=INT64_MAX}};
 WRAP_CHECK(java_basics_call(v,"java/lang/Math","max","(JJ)J",0,max_args,2,&out)&&out.j==INT64_MAX,"math_max_long_extremes");
 max_args[0].j=-1;max_args[1].j=INT64_MIN;
 WRAP_CHECK(java_basics_call(v,"java/lang/Math","max","(JJ)J",0,max_args,2,&out)&&out.j==-1,"math_max_long_negative");
 struct dvm_class *crc_class=dvm_find_class_or_external(v,"java/util/zip/CRC32");
 dvm_ref crc=crc_class?dvm_new_object(v,crc_class):0;
 WRAP_CHECK(crc&&java_basics_call(v,"java/util/zip/CRC32","<init>","()V",crc,NULL,0,&out),"crc32_ctor");
 for(int ch='1';ch<='9';ch++){arg.i=ch;WRAP_CHECK(java_basics_call(v,"java/util/zip/CRC32","update","(I)V",crc,&arg,1,&out),"crc32_update");}
 WRAP_CHECK(java_basics_call(v,"java/util/zip/CRC32","getValue","()J",crc,NULL,0,&out)&&out.j==UINT32_C(0xcbf43926),"crc32_ieee_vector");
 WRAP_CHECK(java_basics_call(v,"java/util/zip/CRC32","reset","()V",crc,NULL,0,&out)&&java_basics_call(v,"java/util/zip/CRC32","getValue","()J",crc,NULL,0,&out)&&out.j==0,"crc32_reset");
 dvm_ref crc_bytes=dvm_new_array(v,'B',"B",9);WRAP_CHECK(crc_bytes,"crc32_bytes");memcpy(dvm_array_data(v,crc_bytes),"123456789",9);
 union dvm_value crc_args[3]={{.l=crc_bytes},{.i=0},{.i=4}};
 WRAP_CHECK(java_basics_call(v,"java/util/zip/CRC32","update","([BII)V",crc,crc_args,3,&out),"crc32_first_slice");crc_args[1].i=4;crc_args[2].i=5;
 WRAP_CHECK(java_basics_call(v,"java/util/zip/CRC32","update","([BII)V",crc,crc_args,3,&out)&&java_basics_call(v,"java/util/zip/CRC32","getValue","()J",crc,NULL,0,&out)&&out.j==UINT32_C(0xcbf43926),"crc32_split_vector");
 static const uint8_t compressed_vector[]={120,156,51,52,50,54,49,53,51,183,176,4,0,9,30,1,222};
 for(int raw=0;raw<2;raw++){
  struct dvm_class *ic=dvm_find_class_or_external(v,"java/util/zip/Inflater");dvm_ref ir=ic?dvm_new_object(v,ic):0;arg.i=raw;
  WRAP_CHECK(ir&&java_basics_call(v,"java/util/zip/Inflater","<init>","(Z)V",ir,&arg,1,&out),"inflater_ctor");
  dvm_ref source=dvm_new_array(v,'B',"B",sizeof compressed_vector),target=dvm_new_array(v,'B',"B",9);WRAP_CHECK(source&&target,"inflater_arrays");memcpy(dvm_array_data(v,source),compressed_vector,sizeof compressed_vector);
  union dvm_value zargs[3]={{.l=source},{.i=raw?2:0},{.i=raw?11:17}};
  WRAP_CHECK(java_basics_call(v,"java/util/zip/Inflater","setInput","([BII)V",ir,zargs,3,&out),"inflater_input");zargs[0].l=target;zargs[1].i=0;zargs[2].i=4;
  WRAP_CHECK(java_basics_call(v,"java/util/zip/Inflater","inflate","([BII)I",ir,zargs,3,&out)&&out.i==4,"inflater_first_output");zargs[1].i=4;zargs[2].i=5;
  WRAP_CHECK(java_basics_call(v,"java/util/zip/Inflater","inflate","([BII)I",ir,zargs,3,&out)&&out.i==5&&!memcmp(dvm_array_data(v,target),"123456789",9),"inflater_split_output");
  WRAP_CHECK(java_basics_call(v,"java/util/zip/Inflater","finished","()Z",ir,NULL,0,&out)&&out.i,"inflater_finished");
  WRAP_CHECK(java_basics_call(v,"java/util/zip/Inflater","getBytesWritten","()J",ir,NULL,0,&out)&&out.j==9,"inflater_count");
  WRAP_CHECK(java_basics_call(v,"java/util/zip/Inflater","reset","()V",ir,NULL,0,&out)&&java_basics_call(v,"java/util/zip/Inflater","needsInput","()Z",ir,NULL,0,&out)&&out.i,"inflater_reset");
  WRAP_CHECK(java_basics_call(v,"java/util/zip/Inflater","end","()V",ir,NULL,0,&out)&&java_basics_call(v,"java/util/zip/Inflater","end","()V",ir,NULL,0,&out),"inflater_end_idempotent");
 }
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

/* Membership is tested by holder address, so a native argument scope cannot
 * accidentally stand in for a missing constant/cache provider. No GC/move. */
struct java_roots_probe {dvm_ref *slots[300];unsigned count;bool found[300];};
static void java_roots_probe_visit(dvm_ref *slot,void *user){
 struct java_roots_probe *p=user;for(unsigned i=0;i<p->count;i++)if(slot==p->slots[i])p->found[i]=true;
}
bool java_basics_global_roots_selftest(struct dvm *v){
 if(!v||!dvm_gil_held(v)||dvm_exception(v))return false;
 static const struct {const char *c,*f,*t;} cases[]={
 {"java/lang/Boolean","TRUE","Ljava/lang/Boolean;"},{"java/lang/Boolean","FALSE","Ljava/lang/Boolean;"},
 {"java/util/Locale","US","Ljava/util/Locale;"},{"java/util/Locale","ROOT","Ljava/util/Locale;"},{"java/util/Locale","ENGLISH","Ljava/util/Locale;"},
 {"java/io/File","separator","Ljava/lang/String;"},{"java/io/File","pathSeparator","Ljava/lang/String;"},
 {"java/net/Proxy","NO_PROXY","Ljava/net/Proxy;"},
 {"java/net/Proxy$Type","DIRECT","Ljava/net/Proxy$Type;"},{"java/net/Proxy$Type","HTTP","Ljava/net/Proxy$Type;"},{"java/net/Proxy$Type","SOCKS","Ljava/net/Proxy$Type;"},
 {"android/webkit/WebSettings$ZoomDensity","FAR","Landroid/webkit/WebSettings$ZoomDensity;"},
 {"android/webkit/WebSettings$ZoomDensity","MEDIUM","Landroid/webkit/WebSettings$ZoomDensity;"},
 {"android/webkit/WebSettings$ZoomDensity","CLOSE","Landroid/webkit/WebSettings$ZoomDensity;"},
 {"android/webkit/WebSettings$LayoutAlgorithm","NARROW_COLUMNS","Landroid/webkit/WebSettings$LayoutAlgorithm;"},
 {"android/webkit/WebSettings$LayoutAlgorithm","NORMAL","Landroid/webkit/WebSettings$LayoutAlgorithm;"},
 {"android/webkit/WebSettings$LayoutAlgorithm","SINGLE_COLUMN","Landroid/webkit/WebSettings$LayoutAlgorithm;"},
 {"android/webkit/WebSettings$PluginState","OFF","Landroid/webkit/WebSettings$PluginState;"},
 {"android/webkit/WebSettings$PluginState","ON","Landroid/webkit/WebSettings$PluginState;"},
 {"android/webkit/WebSettings$PluginState","ON_DEMAND","Landroid/webkit/WebSettings$PluginState;"},
 {"android/provider/Settings$System","CONTENT_URI","Landroid/net/Uri;"},
 {"android/provider/Settings$Global","CONTENT_URI","Landroid/net/Uri;"},
 {"android/provider/Settings$Secure","CONTENT_URI","Landroid/net/Uri;"}};
 bool ok=true;unsigned assertions=0;
 for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++){
  union dvm_value a={0},b={0};
  bool pass=java_basics_static(v,cases[i].c,cases[i].f,cases[i].t,&a)&&a.l&&!dvm_exception(v)&&
   java_basics_static(v,cases[i].c,cases[i].f,cases[i].t,&b)&&a.l==b.l&&!dvm_exception(v);
  assertions++;if(!pass){sdk_trace_fprintf(stderr,"JAVA_GLOBAL_ROOTS_FAIL stage=static_get case=%u class=%s field=%s first=%u second=%u exception=%u\n",i,cases[i].c,cases[i].f,a.l,b.l,dvm_exception(v));ok=false;break;}
 }
 struct dvm_class *integer=dvm_find_class_or_external(v,"java/lang/Integer");
 struct dvm_method *box=integer?dvm_find_method(v,integer,"valueOf","(I)Ljava/lang/Integer;"):NULL;
 struct dvm_method box_storage={0};if(integer&&!box){box_storage=(struct dvm_method){.cls=integer,.name="valueOf",.sig="(I)Ljava/lang/Integer;",.ret_kind='L',.access=9,.arg_count=1,.arg_slots=1};box=&box_storage;}

 static const int box_values[]={-128,0,127};
 for(unsigned i=0;ok&&i<3;i++){union dvm_value arg={.i=box_values[i]},a={0},b={0};
  bool pass=box&&dvm_call(v,box,0,&arg,1,&a)&&a.l&&dvm_call(v,box,0,&arg,1,&b)&&a.l==b.l&&!dvm_exception(v);
  assertions++;if(!pass){sdk_trace_fprintf(stderr,"JAVA_GLOBAL_ROOTS_FAIL stage=Integer.valueOf value=%d first=%u second=%u fallback_descriptor=%d exception=%u\n",box_values[i],a.l,b.l,box==&box_storage,dvm_exception(v));ok=false;}
 }
 struct java_roots_probe p={0};
#define JAVA_ROOT_ADD(x) do{if((x))p.slots[p.count++]=&(x);}while(0)
 for(unsigned i=0;i<3;i++)JAVA_ROOT_ADD(locale_statics[i]);
 for(unsigned i=0;i<2;i++){JAVA_ROOT_ADD(boolean_statics[i]);JAVA_ROOT_ADD(file_statics[i]);}
 JAVA_ROOT_ADD(proxy_none);JAVA_ROOT_ADD(proxy_direct);JAVA_ROOT_ADD(proxy_http);JAVA_ROOT_ADD(proxy_socks);
 for(unsigned i=0;i<9;i++)JAVA_ROOT_ADD(websettings_values[i]);
 for(unsigned i=0;i<3;i++)JAVA_ROOT_ADD(settings_uri[i]);
 struct integer_cache *cache=integer_cache_for(v);if(!cache)ok=false;
 if(cache)for(unsigned i=0;i<256;i++)JAVA_ROOT_ADD(cache->refs[i]);
#undef JAVA_ROOT_ADD
 if(ok){ok=dvm_gc_visit_roots(v,DVM_GC_ROOT_STRONG,java_roots_probe_visit,&p);if(!ok)sdk_trace_fprintf(stderr,"JAVA_GLOBAL_ROOTS_FAIL stage=visit_roots exception=%u\n",dvm_exception(v));}
 for(unsigned i=0;i<p.count;i++){assertions++;if(!p.found[i]){sdk_trace_fprintf(stderr,"JAVA_GLOBAL_ROOTS_FAIL stage=holder_missing index=%u ref=%u\n",i,*p.slots[i]);ok=false;}}
 sdk_trace_fprintf(stderr,"JAVA_GLOBAL_ROOTS_SELFTEST %s assertions=%u holders=%u holder_address_membership=1 cached_identity=1 moved=0\n",ok?"PASS":"FAIL",assertions,p.count);
 return ok&&!dvm_exception(v);
}

int java_basics_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 struct gc_operation_pin gc4_pin __attribute__((cleanup(gc_operation_pin_end)));
 if(!gc_operation_pin_begin(v,&gc4_pin,o))return DVM_GC_PAYLOAD_INVALID;
 o=dvm__obj(v,gc4_pin.ref);

 (void)v;
 if(o->data_destroy==boxed_destroy||o->data_destroy==locale_destroy||o->data_destroy==java_random_destroy)return DVM_GC_PAYLOAD_ATOMIC;
 if(o->data_destroy==inflater_destroy){struct java_inflater *p=o->data;if(visit)visit(&p->input,user);return DVM_GC_PAYLOAD_REFS;}
 if(o->data_destroy!=destroy)return DVM_GC_PAYLOAD_UNKNOWN;
 struct payload *p=o->data;
 if(p->kind==PAY_LIST||p->kind==PAY_ITER){if(p->s&&p->s->n>1000000)return DVM_GC_PAYLOAD_INVALID;
  if(visit&&p->s)for(size_t i=0;i<p->s->n;i++)visit(&p->s->values[i],user);return DVM_GC_PAYLOAD_REFS;}
 if(p->kind==PAY_TLS){size_t count=0;for(struct tls_entry *e=p->tls;e;e=e->next){if(++count>1000000)return DVM_GC_PAYLOAD_INVALID;if(visit)visit(&e->value,user);}return DVM_GC_PAYLOAD_REFS;}
 if(p->kind==PAY_ATOMIC_BOOL||p->kind==PAY_BOOLEAN||p->kind==PAY_TIMEUNIT||p->kind==PAY_ATOMIC_INT)return DVM_GC_PAYLOAD_ATOMIC;
 return DVM_GC_PAYLOAD_UNKNOWN;
}
