#include "string_runtime.h"
#include "enum_runtime.h"
#include "java_basics.h"
#include "platform_runtime.h"
#include "runtime_extra.h"
#include "asset_reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
void sdk_trace_missing_api(const char*,const char*,const char*);
/* Strings follow the VM/DEX convention: U+0000 is carried as the MUTF-8
 * sequence C0 80, never as a raw 0 byte.  dex.c deliberately leaves C0 80 in
 * place because it is safe in a C string and keeps the Java character count
 * right; index_text() below decodes exactly that sequence back to U+0000. */
static dvm_ref from_units(struct dvm *v,const uint16_t *u,int n){
 char *s=malloc((size_t)n*3+1);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","String units");return 0;}size_t at=0;
 for(int i=0;i<n;++i){uint32_t cp=u[i];if(cp>=0xd800&&cp<=0xdbff&&i+1<n&&u[i+1]>=0xdc00&&u[i+1]<=0xdfff){cp=0x10000+((cp-0xd800)<<10)+(u[++i]-0xdc00);s[at++]=(char)(0xf0|(cp>>18));s[at++]=(char)(0x80|((cp>>12)&63));s[at++]=(char)(0x80|((cp>>6)&63));s[at++]=(char)(0x80|(cp&63));}else if(cp&&cp<128)s[at++]=(char)cp;else if(cp<2048){s[at++]=(char)(0xc0|(cp>>6));s[at++]=(char)(0x80|(cp&63));}else{s[at++]=(char)(0xe0|(cp>>12));s[at++]=(char)(0x80|((cp>>6)&63));s[at++]=(char)(0x80|(cp&63));}}
 dvm_ref r=dvm_new_string_n(v,s,at);free(s);return r;
}
static bool object_text(struct dvm *v,dvm_ref ref,const char **p,size_t *len){
 if(!ref){*p="null";*len=4;return true;}
 struct dvm_object *o=dvm__obj(v,ref);if(!o)return false;
 if(o->kind==DVM_OBJ_STRING){*p=o->utf8;*len=o->utf8_len;return true;}
 union dvm_value out={0};
 bool ok=runtime_extra_call(v,o->cls->name,"toString","()Ljava/lang/String;",ref,NULL,0,&out)&&out.l;
 if(!ok)ok=java_basics_call(v,o->cls->name,"toString","()Ljava/lang/String;",ref,NULL,0,&out)&&out.l;
 if(!ok)return false;
 struct dvm_object *s=dvm__obj(v,out.l);if(!s||s->kind!=DVM_OBJ_STRING)return false;*p=s->utf8;*len=s->utf8_len;return true;
}
static bool object_int(struct dvm *v,dvm_ref ref,int32_t *out){
 if(!ref)return false;struct dvm_object *o=dvm__obj(v,ref);if(!o)return false;union dvm_value r={0};
 if(java_basics_call(v,o->cls->name,"intValue","()I",ref,NULL,0,&r)){*out=r.i;return true;}
 if(java_basics_call(v,o->cls->name,"longValue","()J",ref,NULL,0,&r)){*out=(int32_t)r.j;return true;}
 return false;
}
static bool object_double(struct dvm *v,dvm_ref ref,double *out){
 if(!ref)return false;struct dvm_object *o=dvm__obj(v,ref);if(!o)return false;union dvm_value r={0};
 if(java_basics_call(v,o->cls->name,"doubleValue","()D",ref,NULL,0,&r)){*out=r.d;return true;}
 if(java_basics_call(v,o->cls->name,"floatValue","()F",ref,NULL,0,&r)){*out=r.f;return true;}
 if(java_basics_call(v,o->cls->name,"intValue","()I",ref,NULL,0,&r)){*out=r.i;return true;}
 if(java_basics_call(v,o->cls->name,"longValue","()J",ref,NULL,0,&r)){*out=(double)r.j;return true;}
 return false;
}
#define STRING_FORMAT_OUTPUT_LIMIT ((size_t)1048576)
typedef struct {char *data;size_t used,cap;bool limit_hit;} fmt_output;
static bool grow(fmt_output *o,size_t need){
 if(need>STRING_FORMAT_OUTPUT_LIMIT||o->used>STRING_FORMAT_OUTPUT_LIMIT-need){o->limit_hit=true;return false;}
 size_t want=o->used+need;
 if(want<=o->cap)return true;
 size_t cap=o->cap?o->cap:256;
 while(cap<want)cap*=2;
 char *p=realloc(o->data,cap);if(!p)return false;o->data=p;o->cap=cap;return true;
}
static bool put(fmt_output *o,const char *s,size_t n){if(!grow(o,n))return false;memcpy(o->data+o->used,s,n);o->used+=n;return true;}
enum fmt_status {FMT_STATUS_OK,FMT_STATUS_MEMORY,FMT_STATUS_THROW};
static uint64_t double_bits64(double value){uint64_t bits;memcpy(&bits,&value,sizeof bits);return bits;}
/* Java's Formatter does not print a double's exact binary expansion for %f.
 * It formats the shortest round-trip decimal digits, rounds them half-up to
 * the requested fraction digits, and pads zeros when the requested precision
 * reaches beyond them.  Thus 1e100 prints as 1 followed by 100 zeroes and
 * 5e-7 prints as 0.000001, although both doubles are slightly below those
 * decimal values.  Obtain the shortest digits with the same scan as this
 * runtime's Double wrapper: the first correctly-rounded %e rendering that
 * round-trips through strtod.  The returned dec_exp satisfies
 * value = 0.<digits> * 10^dec_exp, the decimal-point position used by
 * FormattedFloatingDecimal. */
static bool shortest_double_digits(double d,char digits[18],int *count,int *dec_exp){
 char sci[64];int precision=17;
 for(int p=1;p<=17;++p){
  int n=snprintf(sci,sizeof sci,"%.*e",p-1,d);
  if(n<0||(size_t)n>=sizeof sci)return false;
  if(double_bits64(strtod(sci,NULL))==double_bits64(d)){precision=p;break;}
 }
 int n=snprintf(sci,sizeof sci,"%.*e",precision-1,d);
 if(n<0||(size_t)n>=sizeof sci)return false;
 const char *s=sci;if(*s=='-'||*s=='+')++s;
 int nd=0;while(*s&&*s!='e'&&*s!='E'){if(*s>='0'&&*s<='9'){if(nd>=17)return false;digits[nd++]=*s;}++s;}
 while(nd>1&&digits[nd-1]=='0')--nd;
 if(nd<=0)return false;
 int e=0;if(*s=='e'||*s=='E'){++s;bool eneg=false;if(*s=='+')++s;else if(*s=='-'){eneg=true;++s;}while(*s>='0'&&*s<='9'){e=e*10+(*s-'0');++s;}if(eneg)e=-e;}
 *count=nd;*dec_exp=e+1;return true;
}
/* java.lang.String.format("%f", double): fixed notation, six fraction
 * digits, HALF_UP.  NaN and infinities are Java's literal strings.  The
 * text is built from shortest-round-trip digits; no snprintf return value is
 * ever treated as the byte count of a smaller buffer. */
static enum fmt_status append_double(struct dvm *v,double d,fmt_output *o){
 if(isnan(d))return put(o,"NaN",3)?FMT_STATUS_OK:FMT_STATUS_MEMORY;
 if(isinf(d)){const char *s=d<0?"-Infinity":"Infinity";return put(o,s,strlen(s))?FMT_STATUS_OK:FMT_STATUS_MEMORY;}
 char digits[18];int count=0,dec_exp=0;
 if(!shortest_double_digits(d,digits,&count,&dec_exp)){
  dvm__throw(v,"java/lang/IllegalArgumentException","String.format float conversion");return FMT_STATUS_THROW;
 }
 bool negative=signbit(d)!=0;int precision=6;int exp=dec_exp;
 /* FormattedFloatingDecimal.applyPrecision for Form.DECIMAL_FLOAT. */
 int prec=dec_exp+precision;
 if(prec>=0&&prec<count){
  if(prec==0){
   if(digits[0]>='5'){digits[0]='1';for(int i=1;i<count;i++)digits[i]='0';exp=dec_exp+1;}
   else for(int i=0;i<count;i++)digits[i]='0';
  }else if(digits[prec]>='5'){
   int i=prec-1;
   while(i>0&&digits[i]=='9'){digits[i]='0';--i;}
   if(digits[i]=='9'){digits[0]='1';for(int j=1;j<count;j++)digits[j]='0';exp=dec_exp+1;}
   else{digits[i]++;for(int j=i+1;j<count;j++)digits[j]='0';}
  }else for(int j=prec;j<count;j++)digits[j]='0';
 }
 char magnitude[400];size_t length=0;
 if(exp>0){
  if(exp>330){dvm__throw(v,"java/lang/IllegalArgumentException","String.format float conversion");return FMT_STATUS_THROW;}
  for(int i=0;i<exp;i++)magnitude[length++]=i<count?digits[i]:'0';
  magnitude[length++]='.';
  int t=count>exp?count-exp:0;if(t>precision)t=precision;
  for(int i=0;i<precision;i++)magnitude[length++]=i<t?digits[exp+i]:'0';
 }else{
  magnitude[length++]='0';magnitude[length++]='.';
  int zeros=-exp;if(zeros>precision)zeros=precision;
  for(int i=0;i<zeros;i++)magnitude[length++]='0';
  int t=precision+exp;if(t<0)t=0;if(t>count)t=count;
  for(int i=0;i<precision-zeros;i++)magnitude[length++]=i<t?digits[i]:'0';
 }
 if(negative&&!put(o,"-",1))return FMT_STATUS_MEMORY;
 return put(o,magnitude,length)?FMT_STATUS_OK:FMT_STATUS_MEMORY;
}
static bool java_format(struct dvm *v,dvm_ref format,dvm_ref args,union dvm_value *out){
 struct dvm_object *fo=dvm__obj(v,format);if(!fo||fo->kind!=DVM_OBJ_STRING){dvm__throw(v,"java/lang/NullPointerException","format string");return true;}
 uint32_t count=args?dvm_array_length(v,args):0;dvm_ref *items=args?(dvm_ref*)dvm_array_data(v,args):NULL;
 const char *f=fo->utf8;size_t n=fo->utf8_len,at=0,arg=0;fmt_output o={0};
 while(at<n){
  if(f[at]!='%'){if(!put(&o,f+at,1))goto memory;at++;continue;}
  if(at+1>=n){dvm__throw(v,"java/util/UnknownFormatConversionException","trailing percent");goto fail;}
  size_t spec=at++;if(f[at]=='%'){if(!put(&o,"%",1))goto memory;at++;continue;}
  if(f[at]=='n'){if(!put(&o,"\n",1))goto memory;at++;continue;}
  size_t prefix=at;
  while(at<n&&strchr("-+ 0,#",f[at]))at++;
  while(at<n&&f[at]>='0'&&f[at]<='9')at++;
  if(at<n&&f[at]=='.'){at++;while(at<n&&f[at]>='0'&&f[at]<='9')at++;}
  if(at>=n){dvm__throw(v,"java/util/UnknownFormatConversionException","truncated format");goto fail;}
  char conv=f[at++];
  if(prefix!=at-1){sdk_trace_missing_api("java/lang/String","format","(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;");dvm__throw(v,"java/lang/UnsupportedOperationException","format flags/width/precision");goto fail;}
  if(conv!='s'&&conv!='d'&&conv!='x'&&conv!='f'&&conv!='c'){dvm__throw(v,"java/util/UnknownFormatConversionException","unsupported conversion");goto fail;}
  if(arg>=count){dvm__throw(v,"java/util/MissingFormatArgumentException","format arguments");goto fail;}
  dvm_ref value=items[arg++];
  if(conv=='s'){
   const char *text;size_t len;if(!object_text(v,value,&text,&len)){dvm__throw(v,"java/lang/IllegalArgumentException","String.format string argument");goto fail;}
   if(!put(&o,text,len))goto memory;continue;
  }
  if(conv=='f'){
   double d=0;if(!object_double(v,value,&d)){dvm__throw(v,"java/lang/IllegalArgumentException","String.format floating-point argument");goto fail;}
   enum fmt_status status=append_double(v,d,&o);
   if(status==FMT_STATUS_THROW)goto fail;
   if(status==FMT_STATUS_MEMORY)goto memory;
   continue;
  }
  int32_t integer=0;if(!object_int(v,value,&integer)){dvm__throw(v,"java/lang/IllegalArgumentException","format numeric argument");goto fail;}
  char text[32];int len;
  if(conv=='d')len=snprintf(text,sizeof text,"%d",integer);
  else if(conv=='x')len=snprintf(text,sizeof text,"%x",(uint32_t)integer);
  else len=snprintf(text,sizeof text,"%c",(unsigned char)integer);
  if(len<0||!put(&o,text,(size_t)len))goto memory;
 }
 out->l=dvm_new_string_n(v,o.data?o.data:"",o.used);
 if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","String.format result string");
 free(o.data);return true;
memory:
 free(o.data);
 if(o.limit_hit)dvm__throw(v,"java/lang/OutOfMemoryError","String.format output exceeds 1048576 bytes");
 else dvm__throw(v,"java/lang/OutOfMemoryError","String.format output");
 return true;
fail:
 free(o.data);return true;
}
static struct dvm_object* text(struct dvm* v,dvm_ref ref){struct dvm_object* o=dvm__obj(v,ref);return o&&o->cls&&!strcmp(o->cls->name,"java/lang/String")?o:NULL;}
/* Internal VM strings may encode isolated UTF16 surrogates; preserve them. */
static bool index_text(struct dvm* v,struct dvm_object* o){
 if(o->string_index_ready&&o->string_units)return true;
 size_t length=o->utf8_len;const unsigned char* bytes=(const unsigned char*)o->utf8;
 uint16_t* units=malloc((length+1)*sizeof *units);if(!units){dvm__throw(v,"java/lang/OutOfMemoryError","UTF16 index");return false;}
 uint32_t count=0;
 for(size_t i=0;i<length;){/* MUTF-8 U+0000: dex_string() keeps C0 80 so binary
                          * payloads stay C-string safe; decode it as a real NUL. */
  if(bytes[i]==0xC0&&i+1<length&&bytes[i+1]==0x80){units[count++]=0;i+=2;continue;}
  uint32_t cp=bytes[i++];int extra=0;
  if(cp>=0xC0&&cp<=0xDF){cp&=31;extra=1;}else if(cp>=0xE0&&cp<=0xEF){cp&=15;extra=2;}else if(cp>=0xF0&&cp<=0xF4){cp&=7;extra=3;}else if(cp>=128){free(units);dvm__throw(v,"java/lang/UnsupportedOperationException","invalid internal UTF8");return false;}
  if((size_t)extra>length-i){free(units);dvm__throw(v,"java/lang/UnsupportedOperationException","truncated internal UTF8");return false;}
  for(int j=0;j<extra;j++){if((bytes[i]&0xC0)!=0x80){free(units);dvm__throw(v,"java/lang/UnsupportedOperationException","invalid internal UTF8");return false;}cp=(cp<<6)|(bytes[i++]&63);}
  if(cp>0x10FFFF||(extra==1&&cp<0x80)||(extra==2&&cp<0x800)||(extra==3&&cp<0x10000)){free(units);dvm__throw(v,"java/lang/UnsupportedOperationException","invalid code point");return false;}
  if(cp>0xFFFF){cp-=0x10000;units[count++]=(uint16_t)(0xD800+(cp>>10));units[count++]=(uint16_t)(0xDC00+(cp&1023));}else units[count++]=(uint16_t)cp;
 }
 free(o->string_units);o->string_units=units;o->string_length=count;o->string_index_ready=true;return true;
}
/* Java String.regionMatches([ignoreCase,] toffset, other, ooffset, len)
 * compares UTF-16 code units, not UTF-8 bytes.  An out-of-range offset, or a
 * region longer than either string, returns false; that range check runs
 * before the len<=0 case, so a negative len returns true only while both
 * offsets still satisfy (int64_t)offset + len <= length.  A null other throws
 * NullPointerException before any range check, matching Java.
 *
 * ignoreCase follows Java's two-step Character.toUpperCase/toLowerCase test
 * for ASCII ("A"-"Z" and "a"-"z" fold symmetrically).  Java applies the
 * Unicode simple case mapping to every code unit (for example U+00C9 LATIN
 * CAPITAL LETTER E WITH ACUTE matches U+00E9); this runtime deliberately
 * compares non-ASCII units exactly.  That is a conservative subset: it can
 * miss a match Java accepts but never accepts one Java rejects.  No locale is
 * consulted, so the result is host-independent. */
static uint16_t ascii_upper(uint16_t unit){
 return unit>='a'&&unit<='z'?(uint16_t)(unit-('a'-'A')):unit;
}
static uint16_t ascii_lower(uint16_t unit){
 return unit>='A'&&unit<='Z'?(uint16_t)(unit+('a'-'A')):unit;
}
/* Shared by String.toLowerCase() and String.toLowerCase(Locale).  Both
 * overloads use this locale-independent ASCII-only mapping: ASCII A-Z folds
 * to a-z, while every other UTF-16 code unit (U+00C9, non-ASCII letters,
 * surrogate pairs, embedded NUL) is copied unchanged.  This is a documented
 * conservative subset of the Unicode simple lowercase mapping; it never calls
 * the host's locale-dependent tolower(), so the output is deterministic and
 * does not inherit Turkish dotless-i or any other host locale behavior.  The
 * receiver has already been checked to be a java.lang.String.  A fresh String
 * is always produced, including for the empty input, and the receiver is
 * never mutated. */
static bool string_lower_ascii(struct dvm *v,struct dvm_object *source,union dvm_value *out){
 if(!index_text(v,source))return true;
 uint32_t size=source->string_length;
 uint16_t* lower=size?malloc((size_t)size*sizeof *lower):NULL;
 if(size&&!lower){dvm__throw(v,"java/lang/OutOfMemoryError","String.toLowerCase result");return true;}
 for(uint32_t i=0;i<size;i++)lower[i]=ascii_lower(source->string_units[i]);
 out->l=from_units(v,lower,(int)size);free(lower);
 return true;
}
/* Shared by String.toUpperCase() and String.toUpperCase(Locale).  Java folds
 * ASCII a-z to A-Z for every locale except Turkish/Azeri, where "i" maps to
 * U+0130 (and "I" stays "I").  Every other UTF-16 code unit (U+00E9, other
 * non-ASCII letters, surrogate pairs, embedded NUL) is copied unchanged: with
 * no ICU case tables this is a documented conservative subset of Java's
 * Unicode mapping, which would for example map U+00E9 to U+00C9 and U+00DF
 * to "SS".  The host locale and host toupper() are never consulted, so the
 * output is deterministic.  The receiver has already been checked to be a
 * java.lang.String.  A fresh String is always produced, including for the
 * empty input, and the receiver is never mutated.  turkic is set only when
 * the caller's Locale is recognized as Turkish/Azeri; an ASCII "i" then has
 * a locale-specific mapping this runtime cannot reproduce, so the call fails
 * closed instead of silently returning "I". */
static bool string_upper_ascii(struct dvm *v,struct dvm_object *source,bool turkic,union dvm_value *out){
 if(!index_text(v,source))return true;
 uint32_t size=source->string_length;
 if(size>0x7fffffffu){dvm__throw(v,"java/lang/OutOfMemoryError","String.toUpperCase result too long");return true;}
 uint16_t* upper=size?malloc((size_t)size*sizeof *upper):NULL;
 if(size&&!upper){dvm__throw(v,"java/lang/OutOfMemoryError","String.toUpperCase result");return true;}
 for(uint32_t i=0;i<size;i++){
  uint16_t unit=source->string_units[i];
  if(turkic&&unit=='i'){
   free(upper);
   dvm__throw(v,"java/lang/UnsupportedOperationException","String.toUpperCase Turkish/Azeri dotted-i mapping");
   return true;
  }
  upper[i]=ascii_upper(unit);
 }
 out->l=from_units(v,upper,(int)size);free(upper);
 return true;
}
/* Resolve the language of a java.util.Locale object that reached a call site.
 * java_basics owns Locale.US ("en"), Locale.ENGLISH ("en") and Locale.ROOT
 * (""), while the platform runtime owns the Locale.getDefault() singleton,
 * which platform_runtime.c fixes to en_US.  Returns 1 with the language
 * copied into lang, 0 when
 * neither runtime recognizes the receiver (the caller then fails closed), and
 * -1 after throwing when a recognition call itself failed. */
static int string_locale_language(struct dvm *v,dvm_ref locale,char lang[16]){
 union dvm_value result={0};
 bool handled=java_basics_call(v,"java/util/Locale","getLanguage","()Ljava/lang/String;",locale,NULL,0,&result);
 if(!handled&&!dvm_exception(v))handled=platform_runtime_call(v,"java/util/Locale","getLanguage","()Ljava/lang/String;",locale,NULL,0,&result);
 if(dvm_exception(v))return -1;
 if(!handled||!result.l)return 0;
 const char *text=dvm_string_utf8(v,result.l);size_t length=dvm_string_utf8_length(v,result.l);
 if(!text||strlen(text)!=length||length>=16)return 0;
 memcpy(lang,text,length);lang[length]='\0';return 1;
}
/* Apply String.toUpperCase(Locale) with the locale rules documented above.
 * The caller has already rejected a null Locale with NullPointerException.  A
 * receiver that is not a java.util.Locale fails closed with
 * ClassCastException; a Locale object that no runtime can identify fails
 * closed with UnsupportedOperationException rather than guessing an en_US
 * answer that could silently corrupt a caller's data. */
static bool string_upper_locale(struct dvm *v,struct dvm_object *source,dvm_ref locale,union dvm_value *out){
 struct dvm_object *object=dvm__obj(v,locale);
 if(!object||!object->cls||!object->cls->name||strcmp(object->cls->name,"java/util/Locale")){
  dvm__throw(v,"java/lang/ClassCastException","locale");return true;
 }
 char language[16];int known=string_locale_language(v,locale,language);
 if(known<0)return true;
 if(!known){dvm__throw(v,"java/lang/UnsupportedOperationException","String.toUpperCase unsupported Locale");return true;}
 return string_upper_ascii(v,source,!strcmp(language,"tr")||!strcmp(language,"az"),out);
}
static bool region_matches_units(struct dvm *v,struct dvm_object *source,struct dvm_object *other,bool ignore_case,int32_t toffset,int32_t ooffset,int32_t len,union dvm_value *out){
 if(!index_text(v,source)||!index_text(v,other))return true;
 uint32_t source_length=source->string_length,other_length=other->string_length;
 if(toffset<0||ooffset<0||(int64_t)toffset+(int64_t)len>(int64_t)source_length||(int64_t)ooffset+(int64_t)len>(int64_t)other_length){out->i=0;return true;}
 for(int32_t i=0;i<len;i++){
  uint16_t left=source->string_units[(size_t)toffset+i],right=other->string_units[(size_t)ooffset+i];
  if(left==right)continue;
  if(ignore_case){
   uint16_t upper_left=ascii_upper(left),upper_right=ascii_upper(right);
   if(upper_left==upper_right)continue;
   if(ascii_lower(upper_left)==ascii_lower(upper_right))continue;
  }
  out->i=0;return true;
 }
 out->i=1;return true;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
/* ------------------------------------------------------------------------
 * String(byte[], [int offset, int length,] Charset)
 *
 * okio.Buffer.readString builds Strings directly from segment storage: the
 * four-argument constructor is handed the segment byte[] plus pos/count and
 * the two-argument constructor is used when the data spans segments.  Both
 * initialize the receiver in place; returning a fresh String here would leave
 * the object allocated by new-instance uninitialized and break the caller's
 * identity assumptions.
 *
 * charset_object_name() asks the existing Charset object for name() through
 * dvm__call_out(), the VM's real external boundary.  In the strict runner that
 * reaches asset_reader_call(), which is also where Charset.forName() came
 * from, so aliases and canonical names stay consistent.  String.<init> accepts
 * only the UTF-8 names (see string_init_bytes); String.getBytes additionally
 * accepts the UTF-16 family and fails closed for every other charset instead
 * of silently transcoding.
 * ------------------------------------------------------------------------ */
static bool charset_alias(const char *name,size_t length,const char *alias){
 size_t n=strlen(alias);if(length!=n)return false;
 for(size_t i=0;i<n;i++){
  unsigned char c=(unsigned char)name[i];
  if(c>='A'&&c<='Z')c=(unsigned char)(c+('a'-'A'));
  if(c!=(unsigned char)alias[i])return false;
 }
 return true;
}
static bool is_charset_object(struct dvm_object *o){
 for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)if(k->name&&!strcmp(k->name,"java/nio/charset/Charset"))return true;
 return false;
}
static bool charset_object_name(struct dvm *v,dvm_ref charset,const char **name,size_t *length){
 if(!charset){dvm__throw(v,"java/lang/NullPointerException","charset");return false;}
 struct dvm_object *o=dvm__obj(v,charset);
 if(!is_charset_object(o)){dvm__throw(v,"java/lang/ClassCastException","charset");return false;}
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/nio/charset/Charset");
 if(!cls){dvm__throw(v,"java/lang/UnsupportedOperationException","Charset class unavailable");return false;}
 union dvm_value out={0};
 (void)dvm__call_out(v,cls,"name","()Ljava/lang/String;",false,false,charset,NULL,0,&out);
 if(dvm_exception(v))return false;
 struct dvm_object *n=out.l?dvm__obj(v,out.l):NULL;
 if(!n||n->kind!=DVM_OBJ_STRING||!n->utf8){
  dvm__throw(v,"java/lang/UnsupportedOperationException","Charset.name unavailable");return false;
 }
 *name=n->utf8;*length=n->utf8_len;return true;
}
/* The malformed-sequence rules below mirror OpenJDK's UTF_8.decodeArrayLoop:
 * a malformed sequence is replaced by one U+FFFD and only the bytes that
 * cannot begin a valid sequence are consumed.  The distinction matters:
 * "E4 B8 41" becomes U+FFFD 'A' (one replacement for the truncated prefix),
 * while "E4 B8" at end of input becomes a single U+FFFD.  Isolated surrogate
 * encodings (WTF-8/CESU-8) are malformed UTF-8 and are replaced as well. */
static bool java_utf8_continuation(unsigned char b){return (b&0xc0)==0x80;}
static bool java_utf8_malformed3_2(unsigned char b1,unsigned char b2){
 return (b1==0xe0&&(b2&0xe0)==0x80)||!java_utf8_continuation(b2);
}
static bool java_utf8_malformed4_2(unsigned char b1,unsigned char b2){
 return b1>0xf4||(b1==0xf0&&(b2<0x90||b2>0xbf))||(b1==0xf4&&(b2&0xf0)!=0x80)||!java_utf8_continuation(b2);
}
static size_t java_utf8_malformed3_length(unsigned char b1,unsigned char b2,unsigned char b3){
 if(java_utf8_malformed3_2(b1,b2))return 1;
 if(!java_utf8_continuation(b3))return 2;
 return 3;
}
static size_t java_utf8_malformed4_length(unsigned char b1,unsigned char b2,unsigned char b3,unsigned char b4){
 if(java_utf8_malformed4_2(b1,b2))return 1;
 if(!java_utf8_continuation(b3))return 2;
 if(!java_utf8_continuation(b4))return 3;
 return 3;
}
static size_t java_utf8_replacement(char *out,size_t at){
 out[at]=(char)0xef;out[at+1]=(char)0xbf;out[at+2]=(char)0xbd;return at+3;
}
static size_t java_utf8_emit(char *out,size_t at,uint32_t cp){
 if(cp<0x80)out[at++]=(char)cp;
 else if(cp<0x800){out[at++]=(char)(0xc0|(cp>>6));out[at++]=(char)(0x80|(cp&0x3f));}
 else if(cp<0x10000){out[at++]=(char)(0xe0|(cp>>12));out[at++]=(char)(0x80|((cp>>6)&0x3f));out[at++]=(char)(0x80|(cp&0x3f));}
 else{out[at++]=(char)(0xf0|(cp>>18));out[at++]=(char)(0x80|((cp>>12)&0x3f));out[at++]=(char)(0x80|((cp>>6)&0x3f));out[at++]=(char)(0x80|(cp&0x3f));}
 return at;
}
static size_t java_utf8_decode(const unsigned char *p,size_t n,char *out){
 size_t i=0,at=0;
 while(i<n){
  unsigned char b1=p[i];
  if(b1<0x80){out[at++]=(char)b1;++i;continue;}
  if(b1>=0xc2&&b1<=0xdf){
   if(i+1>=n||!java_utf8_continuation(p[i+1])){at=java_utf8_replacement(out,at);++i;continue;}
   uint32_t cp=((uint32_t)(b1&0x1f)<<6)|(p[i+1]&0x3f);
   at=java_utf8_emit(out,at,cp);i+=2;continue;
  }
  if(b1>=0xe0&&b1<=0xef){
   if(i+2>=n){
    size_t remaining=n-i;
    if(remaining>1&&java_utf8_malformed3_2(b1,p[i+1]))i+=1;
    else i=n;
    at=java_utf8_replacement(out,at);continue;
   }
   unsigned char b2=p[i+1],b3=p[i+2];
   if(java_utf8_malformed3_2(b1,b2)||!java_utf8_continuation(b3)){
    i+=java_utf8_malformed3_length(b1,b2,b3);at=java_utf8_replacement(out,at);continue;
   }
   uint32_t cp=((uint32_t)(b1&0x0f)<<12)|((uint32_t)(b2&0x3f)<<6)|(b3&0x3f);
   if(cp>=0xd800&&cp<=0xdfff){i+=3;at=java_utf8_replacement(out,at);continue;}
   at=java_utf8_emit(out,at,cp);i+=3;continue;
  }
  if(b1>=0xf0&&b1<=0xf4){
   if(i+3>=n){
    size_t remaining=n-i;
    if(b1>0xf4||(remaining>1&&java_utf8_malformed4_2(b1,p[i+1])))i+=1;
    else if(remaining>2&&!java_utf8_continuation(p[i+2]))i+=2;
    else i=n;
    at=java_utf8_replacement(out,at);continue;
   }
   unsigned char b2=p[i+1],b3=p[i+2],b4=p[i+3];
   uint32_t cp=((uint32_t)(b1&0x07)<<18)|((uint32_t)(b2&0x3f)<<12)|((uint32_t)(b3&0x3f)<<6)|(b4&0x3f);
   if(!java_utf8_continuation(b2)||!java_utf8_continuation(b3)||!java_utf8_continuation(b4)||cp<0x10000||cp>0x10ffff){
    i+=java_utf8_malformed4_length(b1,b2,b3,b4);at=java_utf8_replacement(out,at);continue;
   }
   at=java_utf8_emit(out,at,cp);i+=4;continue;
  }
  at=java_utf8_replacement(out,at);++i;
 }
 return at;
}
/* String.getBytes charset selection.  This runtime owns UTF-8 and the three
 * UTF-16 forms; every other charset fails closed at the call sites below
 * instead of producing bytes that claim an encoding they do not have.  Alias
 * matching is ASCII case-insensitive, as Charset.forName() is.  The alias sets
 * mirror the JDK charsets: UTF-8 {"UTF8", "unicode-1-1-utf-8"}, UTF-16LE
 * {"UTF_16LE", "UnicodeLittleUnmarked"}, UTF-16BE {"UTF_16BE",
 * "ISO-10646-UCS-2", "X-UTF-16BE", "UnicodeBigUnmarked"}, and UTF-16
 * {"UTF_16", "unicode"} (which writes a BOM). */
enum get_bytes_charset{GET_BYTES_UNKNOWN=0,GET_BYTES_UTF8,GET_BYTES_UTF16LE,GET_BYTES_UTF16BE,GET_BYTES_UTF16};
static enum get_bytes_charset get_bytes_charset_name(const char *name,size_t length){
 if(charset_alias(name,length,"utf-8")||charset_alias(name,length,"utf8")||charset_alias(name,length,"unicode-1-1-utf-8"))return GET_BYTES_UTF8;
 if(charset_alias(name,length,"utf-16le")||charset_alias(name,length,"utf16le")||charset_alias(name,length,"utf_16le")||charset_alias(name,length,"unicodelittleunmarked"))return GET_BYTES_UTF16LE;
 if(charset_alias(name,length,"utf-16be")||charset_alias(name,length,"utf16be")||charset_alias(name,length,"utf_16be")||charset_alias(name,length,"unicodebigunmarked")||charset_alias(name,length,"iso-10646-ucs-2")||charset_alias(name,length,"x-utf-16be"))return GET_BYTES_UTF16BE;
 if(charset_alias(name,length,"utf-16")||charset_alias(name,length,"utf16")||charset_alias(name,length,"utf_16")||charset_alias(name,length,"unicode"))return GET_BYTES_UTF16;
 return GET_BYTES_UNKNOWN;
}
static bool get_bytes_surrogate_pair(const uint16_t *units,uint32_t count,uint32_t i){
 return units[i]>=0xd800&&units[i]<=0xdbff&&i+1<count&&units[i+1]>=0xdc00&&units[i+1]<=0xdfff;
}
/* Java String.getBytes(): the receiver is a sequence of UTF-16 code units,
 * not a UTF-8 buffer.  A well-formed surrogate pair becomes one supplementary
 * code point; an isolated surrogate is malformed input and is replaced with
 * '?' (0x3F), the default CharsetEncoder replacement, never silently dropped.
 * U+0000 is emitted as a real 0x00 byte.  UTF-16 writes the big-endian BOM
 * FE FF, UTF-16LE/BE do not, and the empty string returns a fresh zero-length
 * byte[] for every charset (Java's encoder skips the BOM when there are no
 * characters).  The output array is sized exactly; the receiver's bytes are
 * never modified (index_text() only caches its immutable UTF-16 view). */
static bool string_get_bytes(struct dvm *v,struct dvm_object *source,enum get_bytes_charset charset,union dvm_value *out){
 if(!index_text(v,source))return true;
 uint32_t count=source->string_length;const uint16_t *units=source->string_units;
 uint64_t total=(charset==GET_BYTES_UTF16&&count)?2u:0u;
 for(uint32_t i=0;i<count;i++){
  bool pair=get_bytes_surrogate_pair(units,count,i);
  if(charset==GET_BYTES_UTF8){
   if(pair){total+=4;++i;}
   else if(units[i]>=0xd800&&units[i]<=0xdfff)total+=1; /* '?' */
   else if(units[i]<0x80)total+=1;
   else if(units[i]<0x800)total+=2;
   else total+=3;
  }else{total+=pair?4:2;if(pair)++i;}
  if(total>0x7fffffffu){dvm__throw(v,"java/lang/OutOfMemoryError","String.getBytes result too large");return true;}
 }
 dvm_ref array=dvm_new_array(v,'B',"B",(uint32_t)total);
 if(!array){dvm__throw(v,"java/lang/OutOfMemoryError","String.getBytes result");return true;}
 unsigned char *dst=total?(unsigned char*)dvm_array_data(v,array):NULL;
 if(total&&!dst){dvm__discard_temporary(v,array);dvm__throw(v,"java/lang/OutOfMemoryError","String.getBytes storage");return true;}
 uint64_t at=0;
 if(charset==GET_BYTES_UTF16&&count){dst[at++]=(unsigned char)0xfe;dst[at++]=(unsigned char)0xff;}
 for(uint32_t i=0;i<count;i++){
  uint16_t unit=units[i];bool pair=get_bytes_surrogate_pair(units,count,i);
  if(charset==GET_BYTES_UTF8){
   if(pair){
    uint32_t codepoint=0x10000u+(((uint32_t)(unit-0xd800u))<<10)+(uint32_t)(units[i+1]-0xdc00u);
    dst[at++]=(unsigned char)(0xf0|(codepoint>>18));
    dst[at++]=(unsigned char)(0x80|((codepoint>>12)&0x3f));
    dst[at++]=(unsigned char)(0x80|((codepoint>>6)&0x3f));
    dst[at++]=(unsigned char)(0x80|(codepoint&0x3f));
    ++i;
   }else if(unit>=0xd800&&unit<=0xdfff)dst[at++]=0x3f;
   else if(unit<0x80)dst[at++]=(unsigned char)unit;
   else if(unit<0x800){
    dst[at++]=(unsigned char)(0xc0|(unit>>6));
    dst[at++]=(unsigned char)(0x80|(unit&0x3f));
   }else{
    dst[at++]=(unsigned char)(0xe0|(unit>>12));
    dst[at++]=(unsigned char)(0x80|((unit>>6)&0x3f));
    dst[at++]=(unsigned char)(0x80|(unit&0x3f));
   }
   continue;
  }
  /* UTF-16 family: a lone surrogate is one replacement code unit '?', a
   * pair is the original two code units (no re-pairing or dropping). */
  uint16_t first=unit,second=0;
  if(pair){second=units[i+1];++i;}
  else if(unit>=0xd800&&unit<=0xdfff)first=0x3fu;
  if(pair||second){
   if(charset==GET_BYTES_UTF16LE){
    dst[at++]=(unsigned char)(first&0xff);dst[at++]=(unsigned char)(first>>8);
    dst[at++]=(unsigned char)(second&0xff);dst[at++]=(unsigned char)(second>>8);
   }else{
    dst[at++]=(unsigned char)(first>>8);dst[at++]=(unsigned char)(first&0xff);
    dst[at++]=(unsigned char)(second>>8);dst[at++]=(unsigned char)(second&0xff);
   }
  }else if(charset==GET_BYTES_UTF16LE){
   dst[at++]=(unsigned char)(first&0xff);dst[at++]=(unsigned char)(first>>8);
  }else{
   dst[at++]=(unsigned char)(first>>8);dst[at++]=(unsigned char)(first&0xff);
  }
 }
 out->l=array;return true;
}
static bool string_init_bytes(struct dvm *v,dvm_ref self,const union dvm_value *a,bool ranged,bool named,union dvm_value *out){
 (void)out;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/String"))return false;
 dvm_ref charset=ranged?a[3].l:a[1].l;
 if(!charset){dvm__throw(v,"java/lang/NullPointerException","charset");return true;}
 dvm_ref bytes_ref=a[0].l;
 if(!bytes_ref){dvm__throw(v,"java/lang/NullPointerException","bytes");return true;}
 struct dvm_object *bytes=dvm__obj(v,bytes_ref);
 if(!bytes||bytes->kind!=DVM_OBJ_ARRAY||bytes->elem_kind!='B'){
  dvm__throw(v,"java/lang/IllegalArgumentException","not a byte array");return true;
 }
 int64_t offset=ranged?a[1].i:0;
 int64_t length=ranged?a[2].i:(int64_t)bytes->length;
 if(offset<0||length<0||(uint64_t)offset+(uint64_t)length>(uint64_t)bytes->length){
  dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","offset %lld length %lld",(long long)offset,(long long)length);return true;
 }
 const char *name;size_t name_length;
 if(named){
  name=dvm_string_utf8(v,charset);
  if(!name){dvm__throw(v,"java/lang/ClassCastException","charset name must be a String");return true;}
  name_length=dvm_string_utf8_length(v,charset);
 }else if(!charset_object_name(v,charset,&name,&name_length))return true;
 if(!(charset_alias(name,name_length,"utf-8")||charset_alias(name,name_length,"utf8")||charset_alias(name,name_length,"unicode-1-1-utf-8"))){
  dvm__throw(v,named?"java/io/UnsupportedEncodingException":"java/lang/UnsupportedOperationException","String.<init> charset %.*s",name_length>200?200:(int)name_length,name);return true;
 }
 if(length&&!bytes->data){dvm__throw(v,"java/lang/IllegalStateException","byte array has no storage");return true;}
 if((uint64_t)length>((uint64_t)SIZE_MAX-1)/3){dvm__throw(v,"java/lang/OutOfMemoryError","String.<init>");return true;}
 char *decoded=malloc((size_t)length*3+1);
 if(!decoded){dvm__throw(v,"java/lang/OutOfMemoryError","String.<init>");return true;}
 const unsigned char *source=length?(const unsigned char*)bytes->data+(size_t)offset:(const unsigned char*)"";
 size_t decoded_length=java_utf8_decode(source,(size_t)length,decoded);
 if(decoded_length>(size_t)UINT32_MAX){
  free(decoded);dvm__throw(v,"java/lang/OutOfMemoryError","String.<init> result too large");return true;
 }
 decoded[decoded_length]=0;
 /* In-place initialization keeps self's identity.  Free the previous payload
  * only after decoding succeeded, so a failed charset/decode leaves the
  * receiver untouched.  host_handle (when a JNI bridge allocated a wrapper)
  * is deliberately preserved; this runner's new-instance produces PLAIN
  * objects, and the bridge owns its global reference. */
 free(o->utf8);free(o->string_units);
 o->kind=DVM_OBJ_STRING;
 o->utf8=decoded;
 o->utf8_len=(uint32_t)decoded_length;
 o->string_units=NULL;
 o->string_length=0;
 o->string_index_ready=false;
 o->builder_capacity=0;
 return true;
}
/* String(char[]) and String(char[],int,int): copy a range of UTF-16 code
 * units.  from_units() performs the MUTF-8 encoding, so U+0000 is stored as
 * C0 80 instead of terminating the text and a valid surrogate pair becomes one
 * 4-byte sequence.  All validation and the new payload are completed before
 * the receiver is touched: a null array, a non-char array, bad bounds or an
 * allocation failure leaves self exactly as it was.  The finished payload is
 * moved out of the temporary VM String, so there is one allocation and no
 * double free; only after the move is the receiver's old payload released. */
static bool string_init_chars(struct dvm *v,dvm_ref self,const union dvm_value *a,bool ranged,union dvm_value *out){
 (void)out;
 if(!a)return false;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o||!o->cls||!o->cls->name||strcmp(o->cls->name,"java/lang/String"))return false;
 dvm_ref chars_ref=a[0].l;
 if(!chars_ref){dvm__throw(v,"java/lang/NullPointerException","char array");return true;}
 struct dvm_object *chars=dvm__obj(v,chars_ref);
 if(!chars||chars->kind!=DVM_OBJ_ARRAY||chars->elem_kind!='C'){
  dvm__throw(v,"java/lang/IllegalArgumentException","not a char array");return true;
 }
 int64_t offset=ranged?a[1].i:0;
 int64_t count=ranged?a[2].i:(int64_t)chars->length;
 if(offset<0||count<0||(uint64_t)offset+(uint64_t)count>(uint64_t)chars->length){
  dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","offset %lld count %lld",(long long)offset,(long long)count);return true;
 }
 /* from_units() and Java arrays are int-sized. */
 if(count>(int64_t)INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","String.<init> chars too large");return true;}
 if(count&&!chars->data){dvm__throw(v,"java/lang/IllegalStateException","char array has no storage");return true;}
 dvm_ref temporary=from_units(v,count?(const uint16_t*)chars->data+(size_t)offset:NULL,(int)count);
 if(!temporary){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","String.<init> char array");
  return true;
 }
 struct dvm_object *t=dvm__obj(v,temporary);
 if(!t||t->kind!=DVM_OBJ_STRING||!t->utf8){
  dvm__discard_temporary(v,temporary);
  if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","String.<init> char array");
  return true;
 }
 char *payload=t->utf8;uint32_t payload_length=t->utf8_len;
 t->utf8=NULL;t->utf8_len=0;
 dvm__discard_temporary(v,temporary);
 /* Commit in place: identity stays self and the old payload is freed only
  * after every failure path above has been passed. */
 free(o->utf8);free(o->string_units);
 o->kind=DVM_OBJ_STRING;
 o->utf8=payload;
 o->utf8_len=payload_length;
 o->string_units=NULL;
 o->string_length=0;
 o->string_index_ready=false;
 o->builder_capacity=0;
 return true;
}
/* String.copyValueOf(char[]): static factory with the same copy semantics as
 * new String(data).  A fresh VM String is returned and the char array is never
 * aliased or modified. */
static bool string_copy_value_of_chars(struct dvm *v,const union dvm_value *a,int n,union dvm_value *out){
 if(!a||n!=1)return false;
 dvm_ref chars_ref=a[0].l;
 if(!chars_ref){dvm__throw(v,"java/lang/NullPointerException","char array");return true;}
 struct dvm_object *chars=dvm__obj(v,chars_ref);
 if(!chars||chars->kind!=DVM_OBJ_ARRAY||chars->elem_kind!='C'){
  dvm__throw(v,"java/lang/IllegalArgumentException","not a char array");return true;
 }
 if(chars->length>(uint32_t)INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","String.copyValueOf chars too large");return true;}
 if(chars->length&&!chars->data){dvm__throw(v,"java/lang/IllegalStateException","char array has no storage");return true;}
 dvm_ref result=from_units(v,chars->length?(const uint16_t*)chars->data:NULL,(int)chars->length);
 if(!result){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","String.copyValueOf char array");
  return true;
 }
 out->l=result;return true;
}
/* ---- java.lang.String.split(String[, int]) -----------------------------
 *
 * The runtime implements only the subset of Java's regex split that can be
 * proven literal: a non-empty pattern with no regex metacharacter, optionally
 * using the common Java escape sequences \n \t \r \f \\.  Anything else (for
 * example "." or an empty pattern) fails closed with
 * UnsupportedOperationException after reporting the missing API; treating a
 * metacharacter as a literal would silently produce a wrong array.
 *
 * Java limit semantics are implemented exactly for this subset:
 *   limit == 0  : split everywhere, discard trailing empty strings
 *   limit > 0   : apply the pattern at most limit-1 times; the last element
 *                 keeps the remainder of the input
 *   limit < 0   : split everywhere, keep trailing empty strings
 * A pattern that does not occur at all returns a one-element array holding the
 * input, including for the empty input ("").split(",") -> [""].
 */
struct split_span {uint32_t start,length;};
static bool split_span_add(struct split_span **spans,size_t *count,size_t *capacity,uint32_t start,uint32_t length){
 if(*count==*capacity){
  size_t next=*capacity?*capacity*2:8;
  if(next<*capacity||next>SIZE_MAX/sizeof **spans)return false;
  struct split_span *grown=realloc(*spans,next*sizeof *grown);
  if(!grown)return false;
  *spans=grown;*capacity=next;
 }
 (*spans)[*count].start=start;
 (*spans)[*count].length=length;
 (*count)++;
 return true;
}
static bool split_metachar(uint16_t unit){
 switch(unit){
 case '.':case '^':case '$':case '*':case '+':case '?':
 case '(':case ')':case '[':case ']':case '{':case '}':case '|':return true;
 default:return false;
 }
}
static bool split_unsupported(struct dvm *v,const char *signature,const char *reason){
 sdk_trace_missing_api("java/lang/String","split",signature);
 dvm__throw(v,"java/lang/UnsupportedOperationException","String.split pattern not supported: %s",reason);
 return true;
}
/* Parse the pattern's UTF-16 units into literal units.  Returns false after
 * throwing UnsupportedOperationException/OOM when the pattern is outside the
 * supported subset; *out and *out_length are only set on success. */
static bool split_literal_pattern(struct dvm *v,struct dvm_object *pattern,
                                  uint16_t **out,uint32_t *out_length,const char *signature){
 if(!index_text(v,pattern))return false;
 uint32_t length=pattern->string_length;
 if(!length){split_unsupported(v,signature,"empty pattern");return false;}
 uint16_t *literal=malloc((size_t)length*sizeof *literal);
 if(!literal){dvm__throw(v,"java/lang/OutOfMemoryError","String.split pattern");return false;}
 uint32_t used=0;
 for(uint32_t i=0;i<length;++i){
  uint16_t unit=pattern->string_units[i];
  if(unit=='\\'){
   if(++i>=length){free(literal);split_unsupported(v,signature,"trailing backslash");return false;}
   uint16_t escaped=pattern->string_units[i];
   if(escaped=='n')literal[used++]=(uint16_t)'\n';
   else if(escaped=='t')literal[used++]=(uint16_t)'\t';
   else if(escaped=='r')literal[used++]=(uint16_t)'\r';
   else if(escaped=='f')literal[used++]=(uint16_t)'\f';
   else if(escaped=='\\')literal[used++]=(uint16_t)'\\';
   else if(split_metachar(escaped))literal[used++]=escaped;
   else{free(literal);split_unsupported(v,signature,"unsupported escape");return false;}
   continue;
  }
  if(split_metachar(unit)){free(literal);split_unsupported(v,signature,"regex metacharacter");return false;}
  literal[used++]=unit;
 }
 *out=literal;*out_length=used;return true;
}
static bool string_split(struct dvm *v,struct dvm_object *source,dvm_ref pattern_ref,
                         int32_t limit,const char *signature,union dvm_value *out){
 struct dvm_object *pattern=text(v,pattern_ref);
 if(!pattern){dvm__throw(v,"java/lang/ClassCastException","split pattern");return true;}
 uint16_t *literal=NULL;uint32_t literal_length=0;
 if(!split_literal_pattern(v,pattern,&literal,&literal_length,signature))return true;
 if(!index_text(v,source)){free(literal);return true;}
 uint32_t size=source->string_length;
 struct split_span *spans=NULL;size_t count=0,capacity=0;
 /* from_units() takes an int length and Java arrays are int-sized. */
 if(size>(uint32_t)INT32_MAX){free(literal);goto oom;}
 bool matched=false,trim_trailing=limit==0;
 uint32_t index=0;
 while((uint64_t)index+literal_length<=size){
  uint32_t at=index;
  while((uint64_t)at+literal_length<=size&&
        memcmp(source->string_units+at,literal,(size_t)literal_length*sizeof *literal))
   ++at;
  if((uint64_t)at+literal_length>size)break;
  matched=true;
  if(limit>0&&count>=(size_t)(limit-1)){
   /* Java stops applying the pattern after limit-1 matches and puts the rest
    * of the input into the final element. */
   if(!split_span_add(&spans,&count,&capacity,index,size-index))goto oom;
   index=size;
   break;
  }
  if(!split_span_add(&spans,&count,&capacity,index,at-index))goto oom;
  index=at+literal_length;
 }
 if(!matched){
  /* Java returns new String[]{input} when the pattern never matched; this
   * path deliberately does not trim the empty input. */
  count=0;
  if(!split_span_add(&spans,&count,&capacity,0,size))goto oom;
  trim_trailing=false;
 }else if(!(limit>0&&count>=(size_t)limit)){
  if(!split_span_add(&spans,&count,&capacity,index,size-index))goto oom;
 }
 if(trim_trailing)while(count>0&&spans[count-1].length==0)--count;
 if(count>(size_t)INT32_MAX)goto oom;
 dvm_ref array=dvm_new_array(v,'L',"Ljava/lang/String;",(uint32_t)count);
 if(!array)goto oom;
 dvm_ref *elements=count?(dvm_ref*)dvm_array_data(v,array):NULL;
 if(count&&!elements){dvm__discard_temporary(v,array);goto oom;}
 for(size_t i=0;i<count;++i){
  dvm_ref item=from_units(v,source->string_units+spans[i].start,(int)spans[i].length);
  if(!item){
   dvm__discard_temporary(v,array);
   free(literal);free(spans);
   if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","String.split element");
   return true;
  }
  elements[i]=item;
 }
 free(literal);free(spans);
 out->l=array;return true;
oom:
 free(literal);free(spans);
 if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","String.split result");
 return true;
}
/* ---- java.lang.String.replace(CharSequence, CharSequence) --------------
 *
 * This overload is a literal replacement, not a regular expression: the
 * target is matched by UTF-16 code unit and every non-overlapping occurrence
 * is replaced.  It shares no semantics with replaceAll(String,String), whose
 * pattern is a regex subset, so regex metacharacters and replacement '$' or
 * '\' sequences are copied literally.
 *
 * Java converts both arguments with CharSequence.toString().  A String
 * argument already is that result; any other object must be assignable to
 * java/lang/CharSequence under the VM's class metadata, otherwise the cast
 * fails closed with ClassCastException instead of guessing that an arbitrary
 * object is text.  The toString() result must be a genuine VM String; a
 * missing implementation or a non-String result is reported explicitly and
 * never replaced by an empty string.
 *
 * An empty target inserts the replacement before every character and once
 * after the last one, so the empty receiver yields exactly the replacement.
 * All matching and buffering is done on UTF-16 units, which preserves U+0000
 * and surrogate code units.  The output limit is checked before allocation,
 * so an oversized target/replacement pair throws a real OutOfMemoryError
 * instead of exhausting the arena.  The receiver is never modified and a new
 * String is returned on every success, including when no occurrence matched.
 */
#define STRING_REPLACE_OUTPUT_LIMIT ((size_t)1048576)
typedef struct {const uint16_t *units;size_t length;} replace_text;
static bool replace_resolve_text(struct dvm *v,dvm_ref ref,const char *role,replace_text *out){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","String.replace %s",role);return false;}
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o){dvm__throw(v,"java/lang/UnsupportedOperationException","String.replace %s unavailable",role);return false;}
 if(o->kind==DVM_OBJ_STRING){
  if(!index_text(v,o))return false;
  out->units=o->string_units;out->length=o->string_length;return true;
 }
 struct dvm_class *char_sequence=dvm_find_class_or_external(v,"java/lang/CharSequence");
 if(!o->cls||!char_sequence||!dvm__class_assignable(v,o->cls,char_sequence)){
  dvm__throw(v,"java/lang/ClassCastException","String.replace %s is not a CharSequence",role);
  return false;
 }
 union dvm_value text={0};
 bool called=false;
 struct dvm_method *method=dvm_find_method(v,o->cls,"toString","()Ljava/lang/String;");
 if(method&&(method->has_code||method->builtin))called=dvm_call(v,method,ref,NULL,0,&text);
 else if(runtime_extra_call(v,o->cls->name,"toString","()Ljava/lang/String;",ref,NULL,0,&text))called=true;
 else if(java_basics_call(v,o->cls->name,"toString","()Ljava/lang/String;",ref,NULL,0,&text))called=true;
 if(dvm_exception(v))return false;
 if(!called){
  dvm__throw(v,"java/lang/UnsupportedOperationException","String.replace %s toString unavailable",role);
  return false;
 }
 struct dvm_object *s=text.l?dvm__obj(v,text.l):NULL;
 if(!s||s->kind!=DVM_OBJ_STRING){
  dvm__throw(v,text.l?"java/lang/UnsupportedOperationException":"java/lang/NullPointerException",
             "String.replace %s toString did not return String",role);
  return false;
 }
 if(!index_text(v,s))return false;
 out->units=s->string_units;out->length=s->string_length;return true;
}
/* Count literal, non-overlapping occurrences of target in source.  The
 * caller guarantees target_length>0. */
static size_t replace_occurrences(const uint16_t *source,size_t source_length,
                                  const uint16_t *target,size_t target_length){
 size_t count=0;
 for(size_t i=0;i+target_length<=source_length;){
  if(!memcmp(source+i,target,target_length*sizeof *source)){++count;i+=target_length;}
  else ++i;
 }
 return count;
}
/* Exact byte length of the MUTF-8 produced by from_units(); false as soon as
 * it exceeds the same 1 MiB output limit used by String.format/split. */
static bool replace_encoded_length(const uint16_t *units,size_t count){
 size_t total=0;
 for(size_t i=0;i<count;++i){
  uint32_t cp=units[i];
  if(cp>=0xd800&&cp<=0xdbff&&i+1<count&&units[i+1]>=0xdc00&&units[i+1]<=0xdfff){total+=4;++i;}
  else if(cp==0)total+=2;
  else if(cp<0x80)total+=1;
  else if(cp<0x800)total+=2;
  else total+=3;
  if(total>STRING_REPLACE_OUTPUT_LIMIT)return false;
 }
 return true;
}
static bool string_replace_charsequence(struct dvm *v,struct dvm_object *source,
                                        dvm_ref target_ref,dvm_ref replacement_ref,
                                        union dvm_value *out){
 if(!index_text(v,source))return true;
 replace_text target={0},replacement={0};
 if(!replace_resolve_text(v,target_ref,"target",&target))return true;
 if(!replace_resolve_text(v,replacement_ref,"replacement",&replacement))return true;
 const uint16_t *src=source->string_units;
 size_t m=source->string_length,tl=target.length,rl=replacement.length;
 size_t out_units;
 if(!tl){
  /* m+1 insertion points; every insertion contributes rl units. */
  if(rl&&m+1>SIZE_MAX/rl)goto oom;
  out_units=m+(m+1)*rl;
 }else{
  size_t occurrences=replace_occurrences(src,m,target.units,tl);
  if(rl>=tl){
   size_t growth=rl-tl;
   if(growth&&occurrences>(SIZE_MAX-m)/growth)goto oom;
   out_units=m+occurrences*growth;
  }else{
   size_t shrink=tl-rl;
   if(shrink&&occurrences>SIZE_MAX/shrink)goto oom;
   size_t removed=occurrences*shrink;
   if(removed>m)goto oom;
   out_units=m-removed;
  }
 }
 if(out_units>STRING_REPLACE_OUTPUT_LIMIT)goto oom;
 uint16_t *result=malloc((out_units?out_units:1)*sizeof *result);
 if(!result)goto oom;
 size_t at=0;
 if(!tl){
  for(size_t i=0;i<=m;++i){
   if(rl){memcpy(result+at,replacement.units,rl*sizeof *result);at+=rl;}
   if(i<m)result[at++]=src[i];
  }
 }else{
  for(size_t i=0;i<m;){
   if(i+tl<=m&&!memcmp(src+i,target.units,tl*sizeof *src)){
    if(rl){memcpy(result+at,replacement.units,rl*sizeof *result);at+=rl;}
    i+=tl;
   }else result[at++]=src[i++];
  }
 }
 if(!replace_encoded_length(result,out_units)){free(result);goto oom;}
 dvm_ref created=from_units(v,result,(int)out_units);
 free(result);
 if(!created){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","String.replace result");
  return true;
 }
 out->l=created;return true;
oom:
 if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","String.replace output exceeds 1048576 bytes");
 return true;
}
/* ------------------------------------------------------------------------
 * java.lang.StringBuilder.setLength(int) / charAt(int)
 *
 * A builder is created by runtime_extra.inc, which stores its text as UTF-8
 * bytes in o->utf8/o->utf8_len with o->builder_capacity bytes of storage and
 * does not implement setLength or charAt.  That file is a separate
 * translation unit with static sb_* helpers, so the same decoding rules are
 * repeated here: a 4-byte sequence is one supplementary code point and two
 * UTF-16 code units, a 3-byte surrogate is one unit, and malformed storage is
 * an internal error rather than a silently miscounted value.
 * ------------------------------------------------------------------------ */
static bool builder_unit_next(const unsigned char *s,size_t n,size_t at,uint32_t *cp,size_t *width){
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
static bool builder_units(const char *text,size_t n,uint32_t *units){
 uint32_t count=0;size_t at=0;
 while(at<n){
  uint32_t cp;size_t width;
  if(!builder_unit_next((const unsigned char*)text,n,at,&cp,&width))return false;
  at+=width;
  if(cp>0xffff){if(count>INT32_MAX-2)return false;count+=2;}
  else{if(count==INT32_MAX)return false;++count;}
 }
 *units=count;return true;
}
/* Locate the UTF-8 sequence holding UTF-16 code unit `index`.  `trail` is
 * true when the index names the low half of a supplementary code point. */
static bool builder_unit_at(const char *text,size_t n,uint32_t index,size_t *start,size_t *width,uint32_t *cp,bool *trail){
 uint32_t unit=0;size_t at=0;
 while(at<n){
  uint32_t value;size_t w;
  if(!builder_unit_next((const unsigned char*)text,n,at,&value,&w))return false;
  uint32_t span=value>0xffff?2:1;
  if(index<unit+span){*start=at;*width=w;*cp=value;*trail=index!=unit;return true;}
  unit+=span;at+=w;
 }
 return false;
}
static void builder_encode_unit(uint32_t unit,char out[3]){
 out[0]=(char)(0xe0|(unit>>12));
 out[1]=(char)(0x80|((unit>>6)&63));
 out[2]=(char)(0x80|(unit&63));
}
/* Same storage contract and 1 MiB ceiling as runtime_extra.inc's sb_reserve:
 * setLength must extend the builder through the real backing buffer, never
 * report a length the payload does not hold. */
static bool builder_reserve(struct dvm *v,struct dvm_object *o,size_t length){
 if(length>=1048576){dvm__throw(v,"java/lang/OutOfMemoryError","builder capacity");return false;}
 uint32_t need=(uint32_t)length+1;
 if(o->utf8&&need<=o->builder_capacity)return true;
 uint32_t cap=o->builder_capacity?o->builder_capacity:32;
 while(cap<need)cap=cap>UINT32_MAX/2?need:cap*2;
 char *nb=realloc(o->utf8,cap);
 if(!nb){dvm__throw(v,"java/lang/OutOfMemoryError","builder capacity");return false;}
 o->utf8=nb;o->builder_capacity=cap;return true;
}
static bool builder_set_length(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;(void)out;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","StringBuilder");return true;}
 int32_t want=a[0].i;
 if(want<0){dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","setLength(%d)",want);return true;}
 uint32_t units=0;
 if(o->utf8_len&&(!o->utf8||!builder_units(o->utf8,o->utf8_len,&units))){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_BUILDER_INVALID_UTF8 method=setLength bytes=%u\n",o->utf8_len);
  dvm__throw(v,"java/lang/InternalError","StringBuilder internal UTF-8 is malformed");return true;
 }
 if((uint32_t)want<units){
  size_t start=0,width=0;uint32_t cp=0;bool trail=false;
  if(!o->utf8||!builder_unit_at(o->utf8,o->utf8_len,(uint32_t)want,&start,&width,&cp,&trail)){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_BUILDER_INVALID_UTF8 method=setLength index=%d bytes=%u\n",want,o->utf8_len);
   dvm__throw(v,"java/lang/InternalError","StringBuilder internal UTF-8 is malformed");return true;
  }
  size_t new_len=start;
  if(trail&&cp>0xffff){
   /* Java's backing value[] is UTF-16; a shorter length that ends between the
    * two halves of a surrogate pair leaves the high surrogate in place.  The
    * UTF-8 payload represents it in the same 3-byte form append(char) uses
    * for an unpaired surrogate. */
   uint32_t base=cp-0x10000;char encoded[3];
   builder_encode_unit(0xd800u+(base>>10),encoded);
   memcpy(o->utf8+start,encoded,3);
   new_len=start+3;
  }
  o->utf8[new_len]='\0';
  o->utf8_len=(uint32_t)new_len;
  return true;
 }
 if((uint32_t)want>units){
  size_t delta=(size_t)((uint32_t)want-units);
  size_t have=o->utf8_len;
  if(delta>SIZE_MAX-have){dvm__throw(v,"java/lang/OutOfMemoryError","builder capacity");return true;}
  size_t need=have+delta;
  if(!builder_reserve(v,o,need))return true;
  /* AOSP fills the new UTF-16 slots with '\0'; this builder stores U+0000 as
   * a raw NUL byte, exactly as append(char '\0') does.  The VM String decoder
   * accepts both raw 0 and the MUTF-8 C0 80 form as U+0000. */
  memset(o->utf8+have,0,delta);
  o->utf8[need]='\0';
  o->utf8_len=(uint32_t)need;
 }
 return true;
}
static bool builder_char_at(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","StringBuilder");return true;}
 uint32_t units=0;
 if(o->utf8_len&&(!o->utf8||!builder_units(o->utf8,o->utf8_len,&units))){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_BUILDER_INVALID_UTF8 method=charAt bytes=%u\n",o->utf8_len);
  dvm__throw(v,"java/lang/InternalError","StringBuilder internal UTF-8 is malformed");return true;
 }
 int32_t index=a[0].i;
 if(index<0||(uint32_t)index>=units){dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","charAt index %d length %u",index,units);return true;}
 size_t start=0,width=0;uint32_t cp=0;bool trail=false;
 if(!o->utf8||!builder_unit_at(o->utf8,o->utf8_len,(uint32_t)index,&start,&width,&cp,&trail)){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_BUILDER_INVALID_UTF8 method=charAt index=%d bytes=%u\n",index,o->utf8_len);
  dvm__throw(v,"java/lang/InternalError","StringBuilder internal UTF-8 is malformed");return true;
 }
 if(cp>0xffff){uint32_t base=cp-0x10000;out->i=(int)(trail?(0xdc00u+(base&0x3ffu)):(0xd800u+(base>>10)));}
 else out->i=(int)cp;
 return true;
}
bool string_runtime_call(struct dvm* v,const char* c,const char* m,const char* sig,dvm_ref self,const union dvm_value* a,int n,union dvm_value* out){
 /* StringBuilder shares this runtime's UTF-16 semantics, but its payload is
  * owned by runtime_extra.inc, which does not implement setLength/charAt.
  * Handle those two before the String/CharSequence owner gate; every other
  * StringBuilder method returns false so runtime_extra_call keeps serving it. */
 if(!strcmp(c,"java/lang/StringBuilder")){
  if(IS("setLength","(I)V")&&n==1)return builder_set_length(v,self,a,n,out);
  if(IS("charAt","(I)C")&&n==1)return builder_char_at(v,self,a,n,out);
  return false;
 }
 /* String implements java.lang.CharSequence (runtime-contract).  A DEX
  * invoke-interface can name CharSequence as the method owner even though the
  * receiver is a genuine String; only offer the String methods when the
  * receiver really is a VM string, so other CharSequence implementations
  * remain fail-closed instead of being treated as String. */
 if(strcmp(c,"java/lang/String")){
  if(strcmp(c,"java/lang/CharSequence"))return false;
  struct dvm_object* receiver=dvm__obj(v,self);
  if(!receiver||receiver->kind!=DVM_OBJ_STRING)return false;
 }
 if(IS("<init>","([BIILjava/nio/charset/Charset;)V")&&n==4)return string_init_bytes(v,self,a,true,false,out);
 if(IS("<init>","([BLjava/nio/charset/Charset;)V")&&n==2)return string_init_bytes(v,self,a,false,false,out);
 if(IS("<init>","([BLjava/lang/String;)V")&&n==2)return string_init_bytes(v,self,a,false,true,out);
 if(IS("<init>","([C)V")&&n==1)return string_init_chars(v,self,a,false,out);
 if(IS("<init>","([CII)V")&&n==3)return string_init_chars(v,self,a,true,out);
 if(IS("copyValueOf","([C)Ljava/lang/String;")&&n==1)return string_copy_value_of_chars(v,a,n,out);
 /* String.getBytes: FormatStrategy.log encodes its formatted log line with
  * the no-argument overload, whose charset is Android's default UTF-8.  The
  * String overload resolves aliases itself and reports an unknown name as
  * UnsupportedEncodingException (the checked exception Java declares); the
  * Charset overload accepts a real Charset object, and a valid charset this
  * runtime cannot encode fails closed with UnsupportedOperationException
  * rather than returning bytes in the wrong encoding.  Every success creates
  * a fresh byte[] and leaves the receiver untouched. */
 if(IS("getBytes","()[B")&&n==0){
  struct dvm_object* source=text(v,self);if(!source)return false;
  return string_get_bytes(v,source,GET_BYTES_UTF8,out);
 }
 if(IS("getBytes","(Ljava/lang/String;)[B")&&n==1){
  struct dvm_object* source=text(v,self);if(!source||!a)return false;
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","charsetName");return true;}
  const char* name=dvm_string_utf8(v,a[0].l);
  if(!name){dvm__throw(v,"java/lang/ClassCastException","charsetName");return true;}
  size_t name_length=dvm_string_utf8_length(v,a[0].l);
  enum get_bytes_charset charset=GET_BYTES_UNKNOWN;
  if(strlen(name)==name_length)charset=get_bytes_charset_name(name,name_length);
  if(charset==GET_BYTES_UNKNOWN){
   dvm__throw(v,"java/io/UnsupportedEncodingException","%.*s",name_length>200?200:(int)name_length,name);
   return true;
  }
  return string_get_bytes(v,source,charset,out);
 }
 if(IS("getBytes","(Ljava/nio/charset/Charset;)[B")&&n==1){
  struct dvm_object* source=text(v,self);if(!source||!a)return false;
  const char* name;size_t name_length;
  if(!charset_object_name(v,a[0].l,&name,&name_length))return true;
  enum get_bytes_charset charset=get_bytes_charset_name(name,name_length);
  if(charset==GET_BYTES_UNKNOWN){
   dvm__throw(v,"java/lang/UnsupportedOperationException","String.getBytes charset %.*s",name_length>200?200:(int)name_length,name);
   return true;
  }
  return string_get_bytes(v,source,charset,out);
 }
 bool region4=IS("regionMatches","(ILjava/lang/String;II)Z");
 bool region5=IS("regionMatches","(ZILjava/lang/String;II)Z");
 if((region4&&n==4)||(region5&&n==5)){
  int base=region5?1:0;
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!a[base+1].l){dvm__throw(v,"java/lang/NullPointerException","other");return true;}
  struct dvm_object* other=text(v,a[base+1].l);if(!other)return false;
  return region_matches_units(v,source,other,region5&&a[0].i!=0,a[base].i,a[base+2].i,a[base+3].i,out);
 }
 /* Java String.equalsIgnoreCase(String): HttpUrl$Builder.push canonicalizes
  * each path segment and then calls isDot/isDotDot to recognize "." and ".."
  * including their percent-encoded spellings ("%2e", "%2e.", ".%2e",
  * "%2e%2e").  Both helpers invoke equalsIgnoreCase on a literal receiver with
  * the canonicalized segment as the single String argument.  Java implements
  * this as an equal-length check followed by regionMatches(true,0,other,0,len),
  * so the same UTF-16 code-unit matcher above is reused.  A null argument is
  * unequal and must not throw (Java checks anotherString != null first); the
  * receiver must be a java.lang.String or the method stays unhandled and fails
  * closed.  ASCII A-Z/a-z fold symmetrically; non-ASCII code units only match
  * exactly, the same documented conservative subset as regionMatches. */
 if(IS("equalsIgnoreCase","(Ljava/lang/String;)Z")&&n==1){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!a[0].l){out->i=0;return true;}
  struct dvm_object* other=text(v,a[0].l);if(!other)return false;
  if(!index_text(v,source)||!index_text(v,other))return true;
  if(source->string_length!=other->string_length){out->i=0;return true;}
  return region_matches_units(v,source,other,true,0,0,(int32_t)source->string_length,out);
 }
 /* Java String.codePointAt(int): the index is a UTF-16 code-unit offset, not a
  * UTF-8 byte or code-point ordinal.  A high surrogate is combined with an
  * immediately following low surrogate; otherwise the single code unit is
  * returned unchanged (so a lone low surrogate yields its own value).  Android
  * and OpenJDK throw StringIndexOutOfBoundsException, the IndexOutOfBounds
  * subclass used by the other String index methods here, for either bound. */
 if(IS("codePointAt","(I)I")&&n==1){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!index_text(v,source))return true;
  int32_t index=a[0].i;
  if(index<0||(uint32_t)index>=source->string_length){
   dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","codePointAt index");return true;
  }
  uint16_t first=source->string_units[(uint32_t)index];
  if(first>=0xd800&&first<=0xdbff&&(uint32_t)index+1<source->string_length){
   uint16_t second=source->string_units[(uint32_t)index+1];
   if(second>=0xdc00&&second<=0xdfff){out->i=0x10000+((first-0xd800)<<10)+(second-0xdc00);return true;}
  }
  out->i=first;return true;
 }
 /* Java String.toLowerCase(Locale): OkHttp's Util.canonicalizeHost reads
  * Locale.US and lowercases the IDN.toASCII result.  The Locale argument is
  * still part of the contract even though this runtime's mapping is
  * locale-independent: a null locale throws NullPointerException, exactly as
  * Java does, and the receiver must be a java.lang.String.  ASCII A-Z folds to
  * a-z; every other UTF-16 code unit is copied unchanged.  The latter is a
  * documented conservative subset: it does not implement the Unicode simple
  * lowercase mappings (e.g. U+00C9 would stay U+00C9 here), and it never
  * consults the host locale or host tolower(), so the result is deterministic
  * and identical for Locale.US and Locale.ROOT.  IDN.toASCII already produces
  * ASCII-only host names, so this boundary cannot change canonicalizeHost's
  * output.  The call always returns a new String and leaves the receiver
  * untouched.  Any other signature is left unhandled and fails closed.  The
  * zero-argument overload is handled separately below with identical mapping. */
 if(IS("toLowerCase","(Ljava/util/Locale;)Ljava/lang/String;")&&n==1){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","locale");return true;}
  return string_lower_ascii(v,source,out);
 }
 /* Java String.toLowerCase() with no Locale argument: Request$3.onFailure
  * lowercases IOException.getMessage() and then looks for the literal
  * "canceled".  The Java overload is specified in terms of
  * Locale.getDefault(); this runtime deliberately uses the same
  * locale-independent ASCII-only mapping as the Locale overload, so the
  * result never depends on the host locale.  ASCII A-Z folds to a-z and every
  * other UTF-16 code unit is preserved exactly, including non-ASCII letters
  * such as U+00C9 and surrogate pairs.  The receiver must be a
  * java.lang.String; a fresh String is returned even for the empty input and
  * the receiver is never mutated.  A mismatched arity or signature stays
  * unhandled and fails closed. */
 if(IS("toLowerCase","()Ljava/lang/String;")&&n==0){
  struct dvm_object* source=text(v,self);if(!source)return false;
  return string_lower_ascii(v,source,out);
 }
 /* Java String.toUpperCase(Locale): CommonTools.params2BodyMap upper-cases
  * the first character of a @RequestParam name to build a bean getter name,
  * passing Locale.getDefault(); other DEX call sites pass Locale.US,
  * Locale.ENGLISH or Locale.ROOT and use the result for hex strings, enum
  * names and Kotlin case conversion.  This runtime's default Locale is the
  * platform runtime's fixed en_US and the named constants are en or root
  * languages, so ASCII folding is exact for ASCII identifiers; non-ASCII code
  * units are preserved, the documented conservative subset described at
  * string_upper_ascii().  The Locale argument is still part of the contract:
  * a null Locale throws NullPointerException, and a Locale object no runtime
  * recognizes fails closed instead of guessing.  A non-String receiver is
  * left unhandled, and every success returns a fresh String without mutating
  * the receiver. */
 if(IS("toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;")&&n==1){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","locale");return true;}
  return string_upper_locale(v,source,a[0].l,out);
 }
 /* Java String.toUpperCase() with no Locale argument is specified in terms
  * of Locale.getDefault().  Resolve that singleton through the platform
  * runtime (en_US) and run the same locale checks; if it cannot be resolved
  * the call fails closed rather than assuming a locale. */
 if(IS("toUpperCase","()Ljava/lang/String;")&&n==0){
  struct dvm_object* source=text(v,self);if(!source)return false;
  union dvm_value locale_value={0};
  bool have_default=platform_runtime_call(v,"java/util/Locale","getDefault","()Ljava/util/Locale;",0,NULL,0,&locale_value);
  if(!have_default||!locale_value.l){
   if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","String.toUpperCase default Locale unavailable");
   return true;
  }
  return string_upper_locale(v,source,locale_value.l,out);
 }
 if(IS("valueOf","(Ljava/lang/Object;)Ljava/lang/String;")&&n==1){
  if(!a[0].l){out->l=dvm_new_string(v,"null");return true;}
  if(text(v,a[0].l)){out->l=a[0].l;return true;}
  struct dvm_object* o=dvm__obj(v,a[0].l);if(!o)return false;
  struct dvm_method* method=dvm_find_method(v,o->cls,"toString","()Ljava/lang/String;");
  if(method&&(method->has_code||method->builtin)){dvm_call(v,method,a[0].l,NULL,0,out);return true;}
  if(enum_runtime_call(v,"java/lang/Enum","toString","()Ljava/lang/String;",a[0].l,NULL,0,out))return true;
  return java_basics_call(v,o->cls->name,"toString","()Ljava/lang/String;",a[0].l,NULL,0,out);
 }
 /* Java String.concat(String): a new String holding this string's UTF-16
  * units followed by the argument's.  concat never returns the receiver
  * itself, so a fresh String is always allocated even when the argument is
  * empty (Bugly's ay.g called it in 72br). */
 if(IS("concat","(Ljava/lang/String;)Ljava/lang/String;")&&n==1){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","concat String");return true;}
  struct dvm_object* tail=text(v,a[0].l);
  if(!tail){dvm__throw(v,"java/lang/ClassCastException","concat argument");return true;}
  if(!index_text(v,source)||!index_text(v,tail))return true;
  uint64_t total=(uint64_t)source->string_length+tail->string_length;
  if(total>(uint64_t)INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","String.concat result");return true;}
  uint16_t* buffer=total?malloc((size_t)total*sizeof *buffer):NULL;
  if(total&&!buffer){dvm__throw(v,"java/lang/OutOfMemoryError","String.concat buffer");return true;}
  if(source->string_length)memcpy(buffer,source->string_units,(size_t)source->string_length*sizeof *buffer);
  if(tail->string_length)memcpy(buffer+source->string_length,tail->string_units,(size_t)tail->string_length*sizeof *buffer);
  out->l=from_units(v,buffer,(int)total);free(buffer);return true;
 }
 /* java.lang.String.compareTo(String): lexicographic comparison of UTF-16
  * code units, returning the first differing unit difference or the length
  * difference.  Used by Collections.sort on the parameter lists built from
  * the original request map. */
 if(IS("compareTo","(Ljava/lang/String;)I")&&n==1){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","compareTo String");return true;}
  struct dvm_object* other=text(v,a[0].l);
  if(!other){dvm__throw(v,"java/lang/ClassCastException","compareTo argument");return true;}
  if(!index_text(v,source)||!index_text(v,other))return true;
  uint32_t count=source->string_length<other->string_length?source->string_length:other->string_length;
  for(uint32_t i=0;i<count;++i){
   uint16_t left=source->string_units[i],right=other->string_units[i];
   if(left!=right){out->i=(int32_t)left-(int32_t)right;return true;}
  }
  out->i=(int32_t)source->string_length-(int32_t)other->string_length;return true;
 }
 if(IS("charAt","(I)C")&&n==1){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!index_text(v,source))return true;
  int32_t index=a[0].i;
  if(index<0||(uint32_t)index>=source->string_length){dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","charAt index");return true;}
  out->i=source->string_units[(uint32_t)index];return true;
 }
 if(IS("subSequence","(II)Ljava/lang/CharSequence;")&&n==2){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!index_text(v,source))return true;
  int32_t begin=a[0].i,end=a[1].i;
  if(begin<0||end<begin||(uint32_t)end>source->string_length){dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","subSequence bounds");return true;}
  out->l=from_units(v,source->string_units+begin,end-begin);return true;
 }
 if(IS("trim","()Ljava/lang/String;")){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!index_text(v,source))return true;
  uint32_t start=0,end=source->string_length;
  while(start<end&&source->string_units[start]<=0x20)start++;
  while(end>start&&source->string_units[end-1]<=0x20)end--;
  out->l=from_units(v,source->string_units+start,(int)(end-start));return true;
 }
 /* Java String.replace(CharSequence,CharSequence): literal, non-regex
  * replacement.  The receiver must be a genuine VM String; the two arguments
  * are resolved by string_replace_charsequence(), which throws
  * NullPointerException for null and ClassCastException for an object that is
  * not a CharSequence under the VM's class metadata. */
 if(IS("replace","(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;")&&n==2){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!a)return false;
  return string_replace_charsequence(v,source,a[0].l,a[1].l,out);
 }
 /* Java String.replace(char,char): literal single-unit replacement.  Java
  * returns this when oldChar == newChar or when no unit matches; otherwise
  * the result is a fresh String built from the replaced units. */
 if(IS("replace","(CC)Ljava/lang/String;")&&n==2){
  struct dvm_object* source=text(v,self);if(!source)return false;
  if(!index_text(v,source))return true;
  uint16_t old_unit=(uint16_t)a[0].i,new_unit=(uint16_t)a[1].i;
  if(old_unit==new_unit){out->l=self;return true;}
  uint32_t length=source->string_length;
  uint16_t* units=malloc((size_t)(length?length:1)*sizeof *units);
  if(!units){dvm__throw(v,"java/lang/OutOfMemoryError","String.replace");return true;}
  bool changed=false;
  for(uint32_t i=0;i<length;++i){
   units[i]=source->string_units[i];
   if(units[i]==old_unit){units[i]=new_unit;changed=true;}
  }
  out->l=changed?from_units(v,units,(int)length):self;
  free(units);
  return true;
 }
 bool split1=IS("split","(Ljava/lang/String;)[Ljava/lang/String;");
 bool split2=IS("split","(Ljava/lang/String;I)[Ljava/lang/String;");
 if((split1&&n==1)||(split2&&n==2)){
  struct dvm_object* source=text(v,self);if(!source){
   sdk_trace_fprintf(stderr,"STRING_SPLIT_NON_STRING receiver=%s\n",self&&dvm__obj(v,self)&&dvm__obj(v,self)->cls&&dvm__obj(v,self)->cls->name?dvm__obj(v,self)->cls->name:"?");
   return false;
  }
  if(!a)return false;
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","regex");return true;}
  const char* split_pattern=dvm_string_utf8(v,a[0].l);
  sdk_trace_fprintf(stderr,"STRING_SPLIT pattern=%s limit=%d\n",split_pattern?split_pattern:"?",split2?a[1].i:0);
  return string_split(v,source,a[0].l,split2?a[1].i:0,sig,out);
 }
 if(IS("format","(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;")&&n==3)return java_format(v,a[1].l,a[2].l,out);
 if(IS("format","(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;")&&n==2)return java_format(v,a[0].l,a[1].l,out);
 bool length=IS("length","()I"),empty=IS("isEmpty","()Z"),hash=IS("hashCode","()I");
 bool starts=IS("startsWith","(Ljava/lang/String;)Z")||IS("startsWith","(Ljava/lang/String;I)Z");
 bool ends=IS("endsWith","(Ljava/lang/String;)Z"),contains=IS("contains","(Ljava/lang/CharSequence;)Z");
 bool substring=IS("substring","(I)Ljava/lang/String;")||IS("substring","(II)Ljava/lang/String;");
 bool indexof=IS("indexOf","(Ljava/lang/String;)I")||IS("indexOf","(Ljava/lang/String;I)I");
 bool lastindexof=IS("lastIndexOf","(Ljava/lang/String;)I")||IS("lastIndexOf","(Ljava/lang/String;I)I");
 if(!length&&!empty&&!hash&&!starts&&!ends&&!contains&&!substring&&!indexof&&!lastindexof)return false;
 struct dvm_object* source=text(v,self);if(!source)return false;
 if(substring){
  if(!index_text(v,source))return true;
  uint32_t size=source->string_length;int64_t begin=a[0].i,end=n==2?(int64_t)a[1].i:(int64_t)size;
  if(begin<0||begin>(int64_t)size||end<begin||end>(int64_t)size){dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","substring range");return true;}
  out->l=from_units(v,source->string_units+begin,(int)(end-begin));return true;
 }
 struct dvm_object* needle=NULL;
 if(starts||ends||contains||indexof||lastindexof){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","String argument");return true;}needle=text(v,a[0].l);if(!needle)return false;}
 if(!index_text(v,source))return true;
 uint32_t size=source->string_length;
 if(length){out->i=(int32_t)size;return true;}if(empty){out->i=size==0;return true;}
 if(hash){uint32_t h=0;for(uint32_t i=0;i<size;i++)h=h*31+source->string_units[i];out->i=(int32_t)h;return true;}
 if(!index_text(v,needle))return true;
 uint32_t count=needle->string_length;
 if(indexof){
  int64_t start=n==2?a[1].i:0;if(start<0)start=0;
  if(!count){out->i=(int32_t)((uint64_t)start>size?size:start);return true;}
  if(count>size||(uint64_t)start>size-count){out->i=-1;return true;}
  out->i=-1;for(uint32_t i=(uint32_t)start;i<=size-count;i++)if(!memcmp(source->string_units+i,needle->string_units,(size_t)count*2)){out->i=(int32_t)i;break;}return true;
 }
 if(lastindexof){
  /* Java String.lastIndexOf: a negative fromIndex is -1 even for an empty
   * needle; otherwise search backward from min(fromIndex, length-count). */
  int64_t start=n==2?a[1].i:(int64_t)size;
  if(start<0){out->i=-1;return true;}
  if(!count){out->i=(int32_t)((uint64_t)start>size?size:start);return true;}
  if(count>size){out->i=-1;return true;}
  if((uint64_t)start>size-count)start=(int64_t)(size-count);
  out->i=-1;for(int64_t i=start;i>=0;--i)if(!memcmp(source->string_units+i,needle->string_units,(size_t)count*2)){out->i=(int32_t)i;break;}return true;
 }
 out->i=0;if(count>size)return true;
 if(contains){for(uint32_t i=0;i<=size-count;i++)if(!memcmp(source->string_units+i,needle->string_units,(size_t)count*2)){out->i=1;break;}return true;}
 int64_t offset=ends?(int64_t)size-count:(n==2?a[1].i:0);
 if(offset<0||(uint64_t)offset>size-count)return true;
 out->i=!memcmp(source->string_units+(size_t)offset,needle->string_units,(size_t)count*2);return true;
}
/* The integration answers java.lang.String.format through the VM's external
 * hook (strict_runtime.c external() -> string_runtime_call).  The selftest VM
 * has no host hooks, so install the same routing for the duration of one call
 * and enter through dvm__call_out, the VM's real external-call boundary,
 * instead of calling java_format() directly. */
/* Alias charsets used by the constructor selftest.  The real UTF-8/UTF-16LE
 * objects come from asset_reader; these two plain Charset instances only make
 * name() report an alternate UTF-8 spelling, so alias matching is exercised
 * independently of forName()'s canonical-name return. */
static dvm_ref selftest_alias_refs[2];
static const char *selftest_alias_names[2];
static bool selftest_external_dispatch(void *user,struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)user;
 if(!strcmp(c,"java/nio/charset/Charset")&&!strcmp(m,"name")&&!strcmp(sig,"()Ljava/lang/String;")){
  for(int i=0;i<2;i++)if(selftest_alias_refs[i]&&self==selftest_alias_refs[i]){out->l=dvm_new_string(v,selftest_alias_names[i]);return true;}
 }
 if(string_runtime_call(v,c,m,sig,self,a,n,out))return true;
 return asset_reader_call(v,c,m,sig,self,a,n,out);
}
static bool selftest_format_dispatch(struct dvm *v,dvm_ref format,dvm_ref args,dvm_ref *result){
 struct dvm_class *string_cls=dvm_find_class_or_external(v,"java/lang/String");
 if(!string_cls)return false;
 uint32_t slots[2]={(uint32_t)format,(uint32_t)args};
 union dvm_value out={0};
 struct dvm_hooks saved=v->hooks;
 v->hooks.call_external=selftest_external_dispatch;
 bool ok=dvm__call_out(v,string_cls,"format","(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",true,false,0,slots,2,&out);
 v->hooks=saved;
 if(ok&&!dvm_exception(v)&&out.l&&result)*result=out.l;
 return ok;
}
static dvm_ref selftest_object_array1(struct dvm *v,dvm_ref item){
 dvm_ref array=dvm_new_array(v,'L',"Ljava/lang/Object;",1);if(!array)return 0;
 dvm_ref *items=dvm_array_data(v,array);if(!items)return 0;items[0]=item;return array;
}
static dvm_ref selftest_double_object(struct dvm *v,double value){
 struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Double");if(!cls)return 0;
 dvm_ref ref=dvm_new_object(v,cls);if(!ref)return 0;
 union dvm_value arg={.d=value},out={0};
 if(!java_basics_call(v,"java/lang/Double","<init>","(D)V",ref,&arg,1,&out))return 0;
 return ref;
}
static bool selftest_format_value(struct dvm *v,double value,const char *want){
 dvm_ref number=selftest_double_object(v,value);
 dvm_ref args=number?selftest_object_array1(v,number):0;
 dvm_ref format=dvm_new_string(v,"%f");
 dvm_ref result=0;
 if(!args||!format){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL format_double_setup=1\n");return false;}
 if(!selftest_format_dispatch(v,format,args,&result)){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL format_double_dispatch=1\n");return false;}
 if(dvm_exception(v)){dvm_clear_exception(v);sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL format_double_exception=1\n");return false;}
 const char *text=dvm_string_utf8(v,result);
 if(!text||strcmp(text,want)){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL format_double_value want=%s got=%s\n",want,text?text:"<null>");return false;}
 return true;
}
static bool selftest_format_rejects(struct dvm *v,dvm_ref format,dvm_ref args,const char *want_class){
 dvm_ref result=0;
 if(!selftest_format_dispatch(v,format,args,&result))return false;
 dvm_ref exception=dvm_exception(v);
 bool match=false;
 if(exception){
  struct dvm_class *cls=dvm_object_class(v,exception);
  match=cls&&cls->name&&!strcmp(cls->name,want_class);
  if(!match){char why[256];dvm_describe_exception(v,exception,why,sizeof why);sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL format_reject want=%s got=%s\n",want_class,why);}
  dvm_clear_exception(v);
 } else sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL format_reject want=%s got=no-exception\n",want_class);
 return match&&!result;
}
static dvm_ref selftest_long_format(struct dvm *v,size_t length){
 char *bytes=malloc(length);if(!bytes)return 0;
 memset(bytes,'x',length);
 dvm_ref text=dvm_new_string_n(v,bytes,length);
 free(bytes);return text;
}
/* overload 0 selects regionMatches(int,String,int,int); 1 selects
 * regionMatches(boolean,int,String,int,int).  Any pending exception is a
 * failure except in selftest_region_npe. */
static bool selftest_region(struct dvm *v,const char *label,int overload,bool ignore_case,dvm_ref self,dvm_ref other,int32_t toffset,int32_t ooffset,int32_t len,bool want){
 const char *sig=overload?"(ZILjava/lang/String;II)Z":"(ILjava/lang/String;II)Z";
 int n=overload?5:4;
 union dvm_value args[5]={0},out={0};
 if(overload){args[0].i=ignore_case;args[1].i=toffset;args[2].l=other;args[3].i=ooffset;args[4].i=len;}
 else{args[0].i=toffset;args[1].l=other;args[2].i=ooffset;args[3].i=len;}
 bool handled=string_runtime_call(v,"java/lang/String","regionMatches",sig,self,args,n,&out);
 dvm_ref exception=dvm_exception(v);
 if(exception){dvm_clear_exception(v);sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL region_matches_%s unexpected_exception=1\n",label);return false;}
 if(!handled||out.i!=(want?1:0)){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL region_matches_%s handled=%d got=%d want=%d\n",label,handled,out.i,want?1:0);return false;}
 return true;
}
static bool selftest_region_npe(struct dvm *v,int overload){
 const char *sig=overload?"(ZILjava/lang/String;II)Z":"(ILjava/lang/String;II)Z";
 int n=overload?5:4;
 union dvm_value args[5]={0},out={0};
 if(overload){args[0].i=1;args[1].i=0;args[2].l=0;args[3].i=0;args[4].i=0;}
 else{args[0].i=0;args[1].l=0;args[2].i=0;args[3].i=0;}
 dvm_ref self=dvm_new_string(v,"ws://example");
 if(!self)return false;
 bool handled=string_runtime_call(v,"java/lang/String","regionMatches",sig,self,args,n,&out);
 if(!handled){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL region_matches_npe%d handled=0\n",overload);return false;}
 dvm_ref exception=dvm_exception(v);
 bool match=false;
 if(exception){
  struct dvm_class *cls=dvm_object_class(v,exception);
  match=cls&&cls->name&&!strcmp(cls->name,"java/lang/NullPointerException");
  if(!match){char why[256];dvm_describe_exception(v,exception,why,sizeof why);sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL region_matches_npe%d got=%s\n",overload,why);}
  dvm_clear_exception(v);
 } else sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL region_matches_npe%d got=no-exception\n",overload);
 return match&&!out.i;
}
/* codePointAt must reject index<0 and index==UTF-16 length.  Accept either the
 * concrete StringIndexOutOfBoundsException or its IndexOutOfBoundsException
 * parent so the check asserts the Java contract rather than one subclass. */
static bool selftest_codepoint_oob(struct dvm *v,dvm_ref source,int32_t index){
 union dvm_value arg={.i=index},out={0};
 bool handled=string_runtime_call(v,"java/lang/String","codePointAt","(I)I",source,&arg,1,&out);
 dvm_ref exception=dvm_exception(v);
 bool match=false;
 if(exception){
  struct dvm_class *cls=dvm_object_class(v,exception);
  match=cls&&cls->name&&(!strcmp(cls->name,"java/lang/StringIndexOutOfBoundsException")||!strcmp(cls->name,"java/lang/IndexOutOfBoundsException"));
  if(!match){char why[256];dvm_describe_exception(v,exception,why,sizeof why);sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL code_point_at_oob index=%d got=%s\n",index,why);}
  dvm_clear_exception(v);
 } else sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL code_point_at_oob index=%d got=no-exception\n",index);
 if(!handled&&!exception)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL code_point_at_oob index=%d handled=0\n",index);
 return handled&&match&&!out.i;
}
/* equalsIgnoreCase selftest helper.  other_null selects the null argument;
 * otherwise the argument is whatever ref is supplied.  want_handled=false
 * asserts fail-closed: not handled and no exception.  Handled calls must not
 * leave an exception behind and must return want. */
static bool selftest_equals_ignore_case(struct dvm *v,const char *label,dvm_ref self,dvm_ref other,bool other_null,bool want,bool want_handled){
 union dvm_value arg={0},out={0};
 if(!other_null)arg.l=other;
 bool handled=string_runtime_call(v,"java/lang/String","equalsIgnoreCase","(Ljava/lang/String;)Z",self,&arg,1,&out);
 dvm_ref exception=dvm_exception(v);
 if(exception){
  char why[256];dvm_describe_exception(v,exception,why,sizeof why);dvm_clear_exception(v);
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL equals_ignore_case_%s exception=%s\n",label,why);return false;
 }
 if(!want_handled){
  if(handled){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL equals_ignore_case_%s handled=1 want=unhandled\n",label);return false;}
  return true;
 }
 if(!handled||out.i!=(want?1:0)){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL equals_ignore_case_%s handled=%d got=%d want=%d\n",label,handled,out.i,want?1:0);return false;
 }
 return true;
}
/* byte[] and Charset fixtures for the String(byte[],...,Charset) tests.  The
 * real Charset objects come from asset_reader_call, the same provider used by
 * Charset.forName in the strict runner. */
static dvm_ref selftest_byte_array(struct dvm *v,const unsigned char *bytes,size_t length){
 dvm_ref ref=dvm_new_array(v,'B',"B",(uint32_t)length);
 if(!ref)return 0;
 unsigned char *data=dvm_array_data(v,ref);
 if(length&&!data)return 0;
 if(length)memcpy(data,bytes,length);
 return ref;
}
static dvm_ref selftest_charset_for_name(struct dvm *v,const char *name){
 dvm_ref n=dvm_new_string(v,name);if(!n)return 0;
 union dvm_value arg={.l=n},out={0};
 if(!asset_reader_call(v,"java/nio/charset/Charset","forName","(Ljava/lang/String;)Ljava/nio/charset/Charset;",0,&arg,1,&out))return 0;
 return out.l;
}
static bool selftest_string_init_call(struct dvm *v,dvm_ref self,dvm_ref bytes,dvm_ref charset,int32_t offset,int32_t length,bool ranged,union dvm_value *out){
 union dvm_value args[4]={0};
 args[0].l=bytes;
 if(ranged){args[1].i=offset;args[2].i=length;args[3].l=charset;}
 else args[1].l=charset;
 struct dvm_hooks saved=v->hooks;
 v->hooks.call_external=selftest_external_dispatch;
 bool handled=string_runtime_call(v,"java/lang/String","<init>",ranged?"([BIILjava/nio/charset/Charset;)V":"([BLjava/nio/charset/Charset;)V",self,args,ranged?4:2,out);
 v->hooks=saved;
 return handled;
}
static bool selftest_init_ok(struct dvm *v,const char *label,dvm_ref self,dvm_ref bytes,dvm_ref charset,int32_t offset,int32_t length,bool ranged,const char *want,size_t want_len){
 if(!self){self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));if(!self)return false;}
 struct dvm_object *bo=dvm__obj(v,bytes);uint32_t bl=bo?bo->length:0;unsigned char *before=NULL;
 if(bl){before=malloc(bl);if(!before)return false;memcpy(before,bo->data,bl);}
 union dvm_value out={0};
 bool handled=selftest_string_init_call(v,self,bytes,charset,offset,length,ranged,&out);
 bool ok=handled&&!dvm_exception(v);
 struct dvm_object *o=ok?dvm__obj(v,self):NULL;
 if(!ok||!o||o->kind!=DVM_OBJ_STRING||o->utf8_len!=want_len||(want_len&&memcmp(o->utf8,want,want_len))){
  if(dvm_exception(v)){
   char why[256];dvm_describe_exception(v,dvm_exception(v),why,sizeof why);dvm_clear_exception(v);
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL string_init_%s handled=%d exception=%s\n",label,handled,why);
  }else sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL string_init_%s handled=%d got_len=%u want_len=%u\n",label,handled,o?o->utf8_len:0,(unsigned)want_len);
  free(before);return false;
 }
 if(bo&&(bo->length!=bl||(bl&&memcmp(bo->data,before,bl)))){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL string_init_%s input_bytes_modified=1\n",label);free(before);return false;
 }
 free(before);return true;
}
static bool selftest_init_error(struct dvm *v,const char *label,dvm_ref self,dvm_ref bytes,dvm_ref charset,int32_t offset,int32_t length,bool ranged,const char *want_class,bool want_handled){
 if(!self){self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));if(!self)return false;}
 union dvm_value out={0};
 bool handled=selftest_string_init_call(v,self,bytes,charset,offset,length,ranged,&out);
 dvm_ref exception=dvm_exception(v);
 bool match=true;
 if(want_class){
  struct dvm_class *cls=exception?dvm_object_class(v,exception):NULL;
  match=handled&&cls&&cls->name&&!strcmp(cls->name,want_class);
  if(!match){
   char why[256];
   if(exception)dvm_describe_exception(v,exception,why,sizeof why);
   else snprintf(why,sizeof why,"no-exception");
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL string_init_%s handled=%d want=%s got=%s\n",label,handled,want_class,why);
  }
 }else{
  match=handled==want_handled&&!exception;
  if(!match)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL string_init_%s handled=%d want_handled=%d exception=%d\n",label,handled,want_handled,exception!=0);
 }
 if(exception)dvm_clear_exception(v);
 return match;
}
/* char[] fixtures for String(char[]) and String(char[],int,int). */
static dvm_ref selftest_char_array(struct dvm *v,const uint16_t *units,size_t count){
 dvm_ref ref=dvm_new_array(v,'C',"C",(uint32_t)count);
 if(!ref)return 0;
 uint16_t *data=count?(uint16_t*)dvm_array_data(v,ref):NULL;
 if(count&&!data)return 0;
 if(count)memcpy(data,units,count*sizeof *units);
 return ref;
}
static bool selftest_char_init_call(struct dvm *v,dvm_ref self,dvm_ref chars,int32_t offset,int32_t count,bool ranged,union dvm_value *out){
 union dvm_value args[3]={{0}};
 args[0].l=chars;args[1].i=offset;args[2].i=count;
 return string_runtime_call(v,"java/lang/String","<init>",ranged?"([CII)V":"([C)V",self,args,ranged?3:1,out);
}
/* Positive char[] constructor case: exact UTF-8 payload, exact UTF-16 units,
 * unchanged input array and an initialized receiver at the same ref. */
static bool selftest_char_init_ok(struct dvm *v,const char *label,dvm_ref self,dvm_ref chars,int32_t offset,int32_t count,bool ranged,const uint16_t *want,int want_count,const char *want_utf8,size_t want_utf8_len){
 if(!self){self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));if(!self)return false;}
 struct dvm_object *ao=dvm__obj(v,chars);uint32_t alen=ao?ao->length:0;
 uint16_t *before=NULL;
 if(alen){before=malloc((size_t)alen*sizeof *before);if(!before)return false;memcpy(before,ao->data,(size_t)alen*sizeof *before);}
 union dvm_value out={0};
 bool handled=selftest_char_init_call(v,self,chars,offset,count,ranged,&out);
 bool okay=handled&&!dvm_exception(v);
 struct dvm_object *o=okay?dvm__obj(v,self):NULL;
 if(okay&&(!o||o->kind!=DVM_OBJ_STRING||o->utf8_len!=want_utf8_len||(want_utf8_len&&(!o->utf8||memcmp(o->utf8,want_utf8,want_utf8_len))))){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_%s payload mismatch\n",label);okay=false;
 }
 if(okay&&(!index_text(v,o)||o->string_length!=(uint32_t)want_count||(want_count&&memcmp(o->string_units,want,(size_t)want_count*sizeof *want)))){
  if(dvm_exception(v)){
   char why[256];dvm_describe_exception(v,dvm_exception(v),why,sizeof why);dvm_clear_exception(v);
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_%s index exception=%s\n",label,why);
  }else sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_%s units mismatch got=%u want=%d\n",label,o->string_length,want_count);
  okay=false;
 }
 if(okay&&ao&&(ao->length!=alen||(alen&&memcmp(ao->data,before,(size_t)alen*sizeof *before)))){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_%s input_chars_modified=1\n",label);okay=false;
 }
 if(!okay&&dvm_exception(v)){
  char why[256];dvm_describe_exception(v,dvm_exception(v),why,sizeof why);dvm_clear_exception(v);
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_%s handled=%d exception=%s\n",label,handled,why);
 }else if(!okay&&!handled)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_%s handled=0\n",label);
 free(before);return okay;
}
/* Negative char[] constructor case.  When self already holds a string, every
 * field the constructor could rewrite must remain identical; this proves the
 * failure is atomic rather than a partially applied initialization. */
static bool selftest_char_init_error(struct dvm *v,const char *label,dvm_ref self,dvm_ref chars,int32_t offset,int32_t count,bool ranged,const char *want_class){
 if(!self){self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));if(!self)return false;}
 struct dvm_object *ro=dvm__obj(v,self);
 bool was_string=ro&&ro->kind==DVM_OBJ_STRING;
 char *saved=NULL;uint32_t saved_len=0,saved_count=0,saved_capacity=0;uint16_t *saved_units=NULL;bool saved_ready=false;
 if(was_string){
  saved_len=ro->utf8_len;saved_units=ro->string_units;saved_count=ro->string_length;
  saved_ready=ro->string_index_ready;saved_capacity=ro->builder_capacity;
  if(saved_len){saved=malloc(saved_len);if(!saved)return false;memcpy(saved,ro->utf8,saved_len);}
 }
 union dvm_value out={0};
 bool handled=selftest_char_init_call(v,self,chars,offset,count,ranged,&out);
 dvm_ref exception=dvm_exception(v);
 struct dvm_class *cls=exception?dvm_object_class(v,exception):NULL;
 bool okay=handled&&cls&&cls->name&&!strcmp(cls->name,want_class);
 if(!okay){
  char why[256];
  if(exception)dvm_describe_exception(v,exception,why,sizeof why);
  else snprintf(why,sizeof why,"no-exception");
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_%s handled=%d want=%s got=%s\n",label,handled,want_class,why);
 }
 if(exception)dvm_clear_exception(v);
 if(okay&&was_string&&(ro->kind!=DVM_OBJ_STRING||ro->utf8_len!=saved_len||
    (saved_len&&(!ro->utf8||memcmp(ro->utf8,saved,saved_len)))||
    ro->string_units!=saved_units||ro->string_length!=saved_count||
    ro->string_index_ready!=saved_ready||ro->builder_capacity!=saved_capacity)){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_%s receiver_mutated=1\n",label);okay=false;
 }
 free(saved);return okay;
}
/* copyValueOf(char[]): success checks the returned String's payload/units;
 * error cases require the requested Java exception and no result object. */
static bool selftest_copy_value_of_chars(struct dvm *v,const char *label,dvm_ref chars,bool want_handled,const char *want_class,const uint16_t *want,int want_count,const char *want_utf8,size_t want_utf8_len){
 union dvm_value arg={.l=chars},out={0};
 bool handled=string_runtime_call(v,"java/lang/String","copyValueOf","([C)Ljava/lang/String;",0,&arg,1,&out);
 dvm_ref exception=dvm_exception(v);
 bool okay=handled==want_handled;
 if(want_class){
  struct dvm_class *cls=exception?dvm_object_class(v,exception):NULL;
  okay=okay&&cls&&cls->name&&!strcmp(cls->name,want_class)&&!out.l;
 }else{
  okay=okay&&!exception&&out.l;
  struct dvm_object *o=okay?dvm__obj(v,out.l):NULL;
  if(okay&&(!o||o->kind!=DVM_OBJ_STRING||o->utf8_len!=want_utf8_len||
     (want_utf8_len&&(!o->utf8||memcmp(o->utf8,want_utf8,want_utf8_len)))||
     !index_text(v,o)||o->string_length!=(uint32_t)want_count||
     (want_count&&memcmp(o->string_units,want,(size_t)want_count*sizeof *want)))){
   if(dvm_exception(v)){
    char why[256];dvm_describe_exception(v,dvm_exception(v),why,sizeof why);dvm_clear_exception(v);
    sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL copy_value_of_%s index exception=%s\n",label,why);
   }else sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL copy_value_of_%s payload mismatch\n",label);
   okay=false;
  }
 }
 if(!okay)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL copy_value_of_%s handled=%d exception=%d want_class=%s\n",label,handled,exception!=0,want_class?want_class:"none");
 if(exception)dvm_clear_exception(v);
 return okay;
}
/* String.getBytes selftest helpers.  The Charset overload asks the Charset
 * object for name() through dvm__call_out(), so install the same external
 * dispatch the constructor tests use.  ok asserts the exact byte[] type,
 * length and payload; error asserts the Java exception class (or, with
 * want_class==NULL, that the call stayed unhandled with no exception). */
static bool selftest_get_bytes_call(struct dvm *v,dvm_ref self,const char *sig,const union dvm_value *args,int n,union dvm_value *out){
 struct dvm_hooks saved=v->hooks;
 v->hooks.call_external=selftest_external_dispatch;
 bool handled=string_runtime_call(v,"java/lang/String","getBytes",sig,self,args,n,out);
 v->hooks=saved;
 return handled;
}
static bool selftest_get_bytes_ok(struct dvm *v,const char *label,dvm_ref self,const char *sig,const union dvm_value *args,int n,const unsigned char *want,size_t want_len){
 union dvm_value out={0};
 bool handled=selftest_get_bytes_call(v,self,sig,args,n,&out);
 dvm_ref exception=dvm_exception(v);
 if(exception){
  char why[256];dvm_describe_exception(v,exception,why,sizeof why);dvm_clear_exception(v);
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_%s exception=%s\n",label,why);return false;
 }
 struct dvm_object *o=handled&&out.l?dvm__obj(v,out.l):NULL;
 if(!handled||!o||o->kind!=DVM_OBJ_ARRAY||o->elem_kind!='B'||o->length!=want_len||(want_len&&(!o->data||memcmp(o->data,want,want_len)))){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_%s handled=%d len=%u want_len=%u\n",label,handled,o?o->length:0,(unsigned)want_len);
  return false;
 }
 return true;
}
static bool selftest_get_bytes_error(struct dvm *v,const char *label,dvm_ref self,const char *sig,const union dvm_value *args,int n,const char *want_class,bool want_handled){
 union dvm_value out={0};
 bool handled=selftest_get_bytes_call(v,self,sig,args,n,&out);
 dvm_ref exception=dvm_exception(v);
 bool match=false;
 if(want_class){
  struct dvm_class *cls=exception?dvm_object_class(v,exception):NULL;
  match=handled&&cls&&cls->name&&!strcmp(cls->name,want_class);
  if(!match){
   char why[256];
   if(exception)dvm_describe_exception(v,exception,why,sizeof why);
   else snprintf(why,sizeof why,"no-exception");
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_%s handled=%d want=%s got=%s\n",label,handled,want_class,why);
  }
 }else{
  match=handled==want_handled&&!exception;
  if(!match)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_%s handled=%d want_handled=%d exception=%d\n",label,handled,want_handled,exception!=0);
 }
 if(exception)dvm_clear_exception(v);
 return match;
}
static bool selftest_split_expect(struct dvm *v,const char *input,const char *pattern,
                                  int32_t limit,bool has_limit,
                                  const char *const *want,int want_count,const char *label){
 union dvm_value args[2]={{0}},out={0};
 dvm_ref self=dvm_new_string(v,input),regex=dvm_new_string(v,pattern);
 if(!self||!regex){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL split_%s allocate=1\n",label);return false;}
 args[0].l=regex;args[1].i=limit;
 bool handled=has_limit
  ?string_runtime_call(v,"java/lang/String","split","(Ljava/lang/String;I)[Ljava/lang/String;",self,args,2,&out)
  :string_runtime_call(v,"java/lang/String","split","(Ljava/lang/String;)[Ljava/lang/String;",self,args,1,&out);
 bool okay=handled&&!dvm_exception(v)&&out.l;
 if(okay){
  uint32_t length=dvm_array_length(v,out.l);
  okay=length==(uint32_t)want_count;
  if(okay&&want_count>0){
   dvm_ref *items=(dvm_ref*)dvm_array_data(v,out.l);
   okay=items!=NULL;
   for(int i=0;okay&&i<want_count;++i){
    dvm_ref item=items[i];
    const char *text=item?dvm_string_utf8(v,item):NULL;
    size_t want_length=strlen(want[i]);
    okay=text&&dvm_string_utf8_length(v,item)==want_length&&!memcmp(text,want[i],want_length);
   }
  }
 }
 if(!okay)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL split_%s handled=%d exception=%d\n",label,handled,dvm_exception(v)!=0);
 if(dvm_exception(v))dvm_clear_exception(v);
 return okay;
}
static bool selftest_split_error(struct dvm *v,dvm_ref self,dvm_ref regex,bool has_limit,int32_t limit,
                                 const char *want_class,bool want_handled,const char *label){
 union dvm_value args[2]={{0}},out={0};
 args[0].l=regex;args[1].i=limit;
 bool handled=has_limit
  ?string_runtime_call(v,"java/lang/String","split","(Ljava/lang/String;I)[Ljava/lang/String;",self,args,2,&out)
  :string_runtime_call(v,"java/lang/String","split","(Ljava/lang/String;)[Ljava/lang/String;",self,args,1,&out);
 dvm_ref exception=dvm_exception(v);
 bool okay=handled==want_handled&&!out.l;
 if(want_class){
  struct dvm_class *cls=exception?dvm_object_class(v,exception):NULL;
  okay=okay&&cls&&cls->name&&!strcmp(cls->name,want_class);
 }else okay=okay&&!exception;
 if(!okay)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL split_%s handled=%d exception=%d want=%s\n",label,handled,exception!=0,want_class?want_class:"none");
 if(exception)dvm_clear_exception(v);
 return okay;
}
/* String.replace(CharSequence,CharSequence) selftest helpers.  Both strings
 * are compared as the VM's MUTF-8 bytes, so U+0000 and surrogate code units
 * are checked exactly; a successful call must also return a fresh String and
 * leave the receiver unchanged. */
static bool selftest_replace_expect(struct dvm *v,const char *label,
 const char *source,size_t source_len,const char *target,size_t target_len,
 const char *replacement,size_t replacement_len,const char *want,size_t want_len){
 dvm_ref s=dvm_new_string_n(v,source,source_len);
 dvm_ref t=dvm_new_string_n(v,target,target_len);
 dvm_ref r=dvm_new_string_n(v,replacement,replacement_len);
 if(!s||!t||!r){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL replace_%s allocate=1\n",label);return false;}
 union dvm_value args[2]={{.l=t},{.l=r}},out={0};
 bool handled=string_runtime_call(v,"java/lang/String","replace","(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;",s,args,2,&out);
 dvm_ref exception=dvm_exception(v);
 bool okay=handled&&!exception&&out.l&&out.l!=s;
 struct dvm_object *o=okay?dvm__obj(v,out.l):NULL;
 okay=okay&&o&&o->kind==DVM_OBJ_STRING&&o->utf8_len==want_len&&(!want_len||(o->utf8&&!memcmp(o->utf8,want,want_len)));
 struct dvm_object *so=okay?dvm__obj(v,s):NULL;
 okay=okay&&so&&so->kind==DVM_OBJ_STRING&&so->utf8_len==source_len&&(!source_len||(so->utf8&&!memcmp(so->utf8,source,source_len)));
 if(!okay)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL replace_%s handled=%d exception=%d result_len=%u want_len=%u fresh=%d\n",label,handled,exception!=0,okay&&o?o->utf8_len:0,(unsigned)want_len,out.l!=s);
 if(exception)dvm_clear_exception(v);
 return okay;
}
static bool selftest_replace_error(struct dvm *v,const char *label,dvm_ref source,
 dvm_ref target,dvm_ref replacement,const char *want_class,bool want_handled){
 union dvm_value args[2]={{.l=target},{.l=replacement}},out={0};
 bool handled=string_runtime_call(v,"java/lang/String","replace","(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;",source,args,2,&out);
 dvm_ref exception=dvm_exception(v);
 struct dvm_class *cls=exception?dvm_object_class(v,exception):NULL;
 bool okay=handled==want_handled&&!out.l;
 if(want_class)okay=okay&&cls&&cls->name&&!strcmp(cls->name,want_class);
 else okay=okay&&!exception;
 if(!okay)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL replace_%s handled=%d want_handled=%d exception=%s\n",label,handled,want_handled,cls&&cls->name?cls->name:"none");
 if(exception)dvm_clear_exception(v);
 return okay;
}
/* StringBuilder.setLength(int)/charAt(int) selftest probes.  length() is
 * served by runtime_extra.inc, charAt() by the string runtime, and the raw
 * UTF-8 byte count is read from the object so a change of storage cannot hide
 * behind identical Java-visible metadata. */
static bool selftest_builder_state(struct dvm *v,dvm_ref builder,const char *label,
 int32_t want_length,const uint16_t *want,uint32_t want_count,uint32_t want_bytes){
 struct dvm_object *o=dvm__obj(v,builder);
 union dvm_value out={0},arg={0};
 if(!o){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s missing=1\n",label);return false;}
 if(o->utf8_len!=want_bytes||(want_bytes&&!o->utf8)){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s bytes=%u want=%u\n",label,o->utf8_len,want_bytes);return false;
 }
 if(!runtime_extra_call(v,"java/lang/StringBuilder","length","()I",builder,NULL,0,&out)||dvm_exception(v)||out.i!=want_length){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s length=%d want=%d exception=%d\n",label,out.i,want_length,dvm_exception(v)!=0);
  if(dvm_exception(v))dvm_clear_exception(v);return false;
 }
 if((uint32_t)want_length!=want_count){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s length_units=%d want_units=%u\n",label,want_length,want_count);return false;
 }
 for(uint32_t i=0;i<want_count;++i){
  arg.i=(int32_t)i;out.i=0;
  if(!string_runtime_call(v,"java/lang/StringBuilder","charAt","(I)C",builder,&arg,1,&out)||dvm_exception(v)||(uint16_t)out.i!=want[i]){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s charAt=%u got=%d want=%u exception=%d\n",label,(unsigned)i,out.i,(unsigned)want[i],dvm_exception(v)!=0);
   if(dvm_exception(v))dvm_clear_exception(v);return false;
  }
 }
 return true;
}
/* toString() must produce a real String whose UTF-16 view matches the builder
 * after setLength truncated or NUL-extended the payload. */
static bool selftest_builder_to_string(struct dvm *v,dvm_ref builder,const char *label,
 int32_t want_length,const uint16_t *want,uint32_t want_count){
 union dvm_value out={0},arg={0},sout={0};
 if(!runtime_extra_call(v,"java/lang/StringBuilder","toString","()Ljava/lang/String;",builder,NULL,0,&out)||dvm_exception(v)||!out.l){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s to_string exception=%d\n",label,dvm_exception(v)!=0);
  if(dvm_exception(v))dvm_clear_exception(v);return false;
 }
 struct dvm_object *s=dvm__obj(v,out.l);
 if(!s||s->kind!=DVM_OBJ_STRING){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s to_string_type=1\n",label);return false;
 }
 if(!string_runtime_call(v,"java/lang/String","length","()I",out.l,NULL,0,&sout)||dvm_exception(v)||sout.i!=want_length){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s string_length=%d want=%d exception=%d\n",label,sout.i,want_length,dvm_exception(v)!=0);
  if(dvm_exception(v))dvm_clear_exception(v);return false;
 }
 for(uint32_t i=0;i<want_count;++i){
  arg.i=(int32_t)i;sout.i=0;
  if(!string_runtime_call(v,"java/lang/String","charAt","(I)C",out.l,&arg,1,&sout)||dvm_exception(v)||(uint16_t)sout.i!=want[i]){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_%s string_charAt=%u got=%d want=%u exception=%d\n",label,(unsigned)i,sout.i,(unsigned)want[i],dvm_exception(v)!=0);
   if(dvm_exception(v))dvm_clear_exception(v);return false;
  }
 }
 return true;
}
static bool selftest_builder_set_length(struct dvm *v,dvm_ref builder,int32_t value,const char *label){
 union dvm_value arg={.i=value},out={0};
 bool handled=string_runtime_call(v,"java/lang/StringBuilder","setLength","(I)V",builder,&arg,1,&out);
 dvm_ref exception=dvm_exception(v);
 struct dvm_class *cls=exception?dvm_object_class(v,exception):NULL;
 bool okay=handled&&!exception;
 if(!okay)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_setLength_%s handled=%d exception=%s\n",label,handled,cls&&cls->name?cls->name:"none");
 if(exception)dvm_clear_exception(v);
 return okay;
}
static bool selftest_builder_error(struct dvm *v,dvm_ref builder,int32_t value,const char *label){
 union dvm_value arg={.i=value},out={0};
 bool handled=string_runtime_call(v,"java/lang/StringBuilder","setLength","(I)V",builder,&arg,1,&out);
 dvm_ref exception=dvm_exception(v);
 struct dvm_class *cls=exception?dvm_object_class(v,exception):NULL;
 bool okay=handled&&cls&&cls->name&&!strcmp(cls->name,"java/lang/StringIndexOutOfBoundsException");
 if(!okay)sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_setLength_%s handled=%d exception=%s\n",label,handled,cls&&cls->name?cls->name:"none");
 if(exception)dvm_clear_exception(v);
 return okay;
}
bool string_runtime_selftest(struct dvm* v){
 sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST BEGIN exception=%d\n",dvm_exception(v)!=0);
 if(dvm_exception(v)){char why[128]={0};dvm_describe_exception(v,dvm_exception(v),why,sizeof why);sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST PENDING %s\n",why);dvm_clear_exception(v);}
 static const char bytes[]={'a',0,(char)0xF0,(char)0x9F,(char)0x98,(char)0x80,'b'};
 /* Java prints shortest-round-trip digits padded with zeroes, not the exact
  * binary expansion: 1e100 is 1 followed by 100 zeroes.  The two strings are
  * 108/109 bytes, so the old 64-byte stack buffer read out of bounds. */
 static const char double_1e100[]="10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.000000";
 static const char double_neg_1e100[]="-10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.000000";
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
 dvm_ref padded=dvm_new_string(v,"  abc \t");
 if(!string_runtime_call(v,"java/lang/String","trim","()Ljava/lang/String;",padded,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"abc"))return false;
 dvm_ref hay=dvm_new_string(v,"/a/b");
 a[0].l=dvm_new_string(v,"/");a[1].i=1;
 if(!string_runtime_call(v,"java/lang/String","indexOf","(Ljava/lang/String;I)I",hay,a,2,&out)||out.i!=2)return false;
 if(!string_runtime_call(v,"java/lang/String","indexOf","(Ljava/lang/String;)I",hay,a,1,&out)||out.i!=0)return false;
 a[0].l=dvm_new_string(v,"b");
 if(!string_runtime_call(v,"java/lang/String","indexOf","(Ljava/lang/String;)I",hay,a,1,&out)||out.i!=3)return false;
 a[0].l=dvm_new_string(v,"z");
 if(!string_runtime_call(v,"java/lang/String","indexOf","(Ljava/lang/String;)I",hay,a,1,&out)||out.i!=-1)return false;
 a[0].l=dvm_new_string(v,"/");a[1].i=-5;
 if(!string_runtime_call(v,"java/lang/String","indexOf","(Ljava/lang/String;I)I",hay,a,2,&out)||out.i!=0)return false;
 a[0].l=dvm_new_string(v,"");a[1].i=10;
 if(!string_runtime_call(v,"java/lang/String","indexOf","(Ljava/lang/String;I)I",hay,a,2,&out)||out.i!=4)return false;
 /* lastIndexOf: backward search, empty needle, negative fromIndex. */
 a[0].l=dvm_new_string(v,"/");
 if(!string_runtime_call(v,"java/lang/String","lastIndexOf","(Ljava/lang/String;)I",hay,a,1,&out)||out.i!=2)return false;
 a[0].l=dvm_new_string(v,"b");
 if(!string_runtime_call(v,"java/lang/String","lastIndexOf","(Ljava/lang/String;)I",hay,a,1,&out)||out.i!=3)return false;
 a[0].l=dvm_new_string(v,"z");
 if(!string_runtime_call(v,"java/lang/String","lastIndexOf","(Ljava/lang/String;)I",hay,a,1,&out)||out.i!=-1)return false;
 a[0].l=dvm_new_string(v,"");a[1].i=10;
 if(!string_runtime_call(v,"java/lang/String","lastIndexOf","(Ljava/lang/String;I)I",hay,a,2,&out)||out.i!=4)return false;
 a[1].i=-5;
 if(!string_runtime_call(v,"java/lang/String","lastIndexOf","(Ljava/lang/String;I)I",hay,a,2,&out)||out.i!=-1)return false;
 a[0].l=dvm_new_string(v,"/");a[1].i=1;
 if(!string_runtime_call(v,"java/lang/String","lastIndexOf","(Ljava/lang/String;I)I",hay,a,2,&out)||out.i!=0)return false;
 a[0].i=1;a[1].i=2;
 if(!string_runtime_call(v,"java/lang/String","substring","(II)Ljava/lang/String;",hay,a,2,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"a"))return false;
 a[0].i=3;
 if(!string_runtime_call(v,"java/lang/String","substring","(I)Ljava/lang/String;",hay,a,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"b"))return false;
 dvm_ref fmt=dvm_new_string(v,"%s:%d:%x");
 dvm_ref values=dvm_new_array(v,'L',"Ljava/lang/Object;",3);
 dvm_ref *items=dvm_array_data(v,values);
 items[0]=dvm_new_string(v,"a");
 dvm_ref integer=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Integer"));
 union dvm_value iv={.i=7},iv2={.i=255},box={0};
 if(!java_basics_call(v,"java/lang/Integer","<init>","(I)V",integer,&iv,1,&box))return false;
 items[1]=integer;
 dvm_ref integer2=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Integer"));
 if(!java_basics_call(v,"java/lang/Integer","<init>","(I)V",integer2,&iv2,1,&box))return false;
 items[2]=integer2;
 union dvm_value fa[2]={{.l=fmt},{.l=values}};
 if(!string_runtime_call(v,"java/lang/String","format","(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",0,fa,2,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"a:7:ff"))return false;
 dvm_ref locale=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/Locale"));
 union dvm_value fl[3]={{.l=locale},{.l=fmt},{.l=values}};
 if(!string_runtime_call(v,"java/lang/String","format","(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",0,fl,3,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"a:7:ff"))return false;
 /* Double values go through the actual external dispatch path
  * (dvm__call_out -> hooks.call_external -> string_runtime_call), not a
  * direct java_format() call.  The 1e100 cases are 108/109 bytes, so the old
  * 64-byte stack buffer read out of bounds for exactly these inputs. */
 if(!selftest_format_value(v,123.456,"123.456000"))return false;
 if(!selftest_format_value(v,-2.25,"-2.250000"))return false;
 if(!selftest_format_value(v,1e100,double_1e100))return false;
 if(!selftest_format_value(v,-1e100,double_neg_1e100))return false;
 if(!selftest_format_value(v,0.0078125,"0.007813"))return false; /* Java HALF_UP, not C half-even */
 if(!selftest_format_value(v,0.123456789,"0.123457"))return false;
 if(!selftest_format_value(v,0.1,"0.100000"))return false;
 /* Java rounds the shortest round-trip "5e-7" upward even though the exact
  * double is 4.99999999999999977e-7; an exact-expansion formatter prints
  * 0.000000 here, so this separates the two semantics. */
 if(!selftest_format_value(v,5e-7,"0.000001"))return false;
 if(!selftest_format_value(v,2.5e-7,"0.000000"))return false;
 if(!selftest_format_value(v,9.9999999,"10.000000"))return false;
 if(!selftest_format_value(v,1e-300,"0.000000"))return false;
 if(!selftest_format_value(v,-0.0,"-0.000000"))return false;
 if(!selftest_format_value(v,NAN,"NaN"))return false;
 if(!selftest_format_value(v,INFINITY,"Infinity"))return false;
 if(!selftest_format_value(v,-INFINITY,"-Infinity"))return false;
 dvm_ref reject_format=dvm_new_string(v,"%f");
 dvm_ref empty_args=dvm_new_array(v,'L',"Ljava/lang/Object;",0);
 dvm_ref bad_args=selftest_object_array1(v,dvm_new_string(v,"not-a-number"));
 if(!reject_format||!empty_args||!bad_args)return false;
 /* Missing and non-floating arguments must raise through the same dispatch
  * path and must not return a partial or zero-length result as success. */
 if(!selftest_format_rejects(v,reject_format,empty_args,"java/util/MissingFormatArgumentException"))return false;
 if(!selftest_format_rejects(v,reject_format,bad_args,"java/lang/IllegalArgumentException"))return false;
 /* Configured output ceiling: one byte past 1 MiB must throw a real
  * exception instead of truncating or returning a partial result. */
 dvm_ref huge_format=selftest_long_format(v,STRING_FORMAT_OUTPUT_LIMIT+1);
 if(!huge_format)return false;
 if(!selftest_format_rejects(v,huge_format,empty_args,"java/lang/OutOfMemoryError"))return false;
 /* String.regionMatches: HttpUtils.checkUrl calls
  * regionMatches(true,0,"ws:",0,3) and regionMatches(true,0,"wss:",0,4).
  * The selftest covers both overloads, UTF-16 offsets, Java's
  * out-of-range->false rule (including negative len), the null-other NPE,
  * ASCII case folding, and the documented non-ASCII conservative subset. */
 dvm_ref rm_ws_upper=dvm_new_string(v,"WS://Example.com/path");
 dvm_ref rm_wss_upper=dvm_new_string(v,"WSS://Example.com/path");
 dvm_ref rm_ws=dvm_new_string(v,"ws:");
 dvm_ref rm_wss=dvm_new_string(v,"wss:");
 dvm_ref rm_mixed=dvm_new_string(v,"Ws://Example.com/path");
 dvm_ref rm_abc=dvm_new_string(v,"abc");
 dvm_ref rm_xyz=dvm_new_string(v,"xyz");
 dvm_ref rm_zabc=dvm_new_string(v,"zabc");
 dvm_ref rm_xabc=dvm_new_string(v,"xabc");
 dvm_ref rm_xyzabc=dvm_new_string(v,"xyzabc");
 dvm_ref rm_qabc=dvm_new_string(v,"qabc");
 dvm_ref rm_xy=dvm_new_string(v,"xy");
 if(!rm_ws_upper||!rm_wss_upper||!rm_ws||!rm_wss||!rm_mixed||!rm_abc||!rm_xyz||!rm_zabc||!rm_xabc||!rm_xyzabc||!rm_qabc||!rm_xy)return false;
 if(!selftest_region(v,"check_ws",1,true,rm_ws_upper,rm_ws,0,0,3,true))return false;
 if(!selftest_region(v,"check_wss",1,true,rm_wss_upper,rm_wss,0,0,4,true))return false;
 if(!selftest_region(v,"check_ws_4arg",0,false,rm_ws,rm_ws,0,0,3,true))return false;
 if(!selftest_region(v,"ignore_case_on",1,true,rm_mixed,rm_ws,0,0,3,true))return false;
 if(!selftest_region(v,"ignore_case_off",1,false,rm_mixed,rm_ws,0,0,3,false))return false;
 if(!selftest_region(v,"equal_region",0,false,rm_zabc,rm_xabc,1,1,3,true))return false;
 if(!selftest_region(v,"wrong_region",0,false,rm_zabc,rm_xabc,0,0,3,false))return false;
 if(!selftest_region(v,"trailing_region",0,false,rm_xyzabc,rm_qabc,3,1,3,true))return false;
 if(!selftest_region(v,"len_zero",0,false,rm_abc,rm_xyz,0,0,0,true))return false;
 if(!selftest_region(v,"len_zero_at_end",0,false,rm_abc,rm_xyz,3,3,0,true))return false;
 if(!selftest_region(v,"len_zero_past_end",0,false,rm_abc,rm_xyz,4,0,0,false))return false;
 if(!selftest_region(v,"neg_len",0,false,rm_abc,rm_xyz,0,0,-1,true))return false;
 if(!selftest_region(v,"neg_len_past_end",0,false,rm_abc,rm_xyz,5,0,-1,false))return false;
 if(!selftest_region(v,"toffset_past_end",0,false,rm_abc,rm_xyz,1,0,3,false))return false;
 if(!selftest_region(v,"ooffset_past_end",0,false,rm_abc,rm_xyz,0,1,3,false))return false;
 if(!selftest_region(v,"other_too_short",0,false,rm_abc,rm_xy,0,0,3,false))return false;
 if(!selftest_region(v,"negative_toffset",0,false,rm_abc,rm_xyz,-1,0,1,false))return false;
 if(!selftest_region(v,"negative_ooffset",0,false,rm_abc,rm_xyz,0,-1,1,false))return false;
 if(!selftest_region_npe(v,0))return false;
 if(!selftest_region_npe(v,1))return false;
 /* Non-ASCII BMP: identical code units match.  Java's Unicode simple case
  * folding (U+00C9 vs U+00E9) is outside this runtime's ASCII-only
  * ignoreCase subset, so that pair must compare unequal here; this is the
  * documented conservative boundary, not a locale-dependent approximation. */
 dvm_ref rm_upper_eacute=dvm_new_string(v,"\xC3\x89");
 dvm_ref rm_lower_eacute=dvm_new_string(v,"\xC3\xA9");
 dvm_ref rm_ascii_e=dvm_new_string(v,"E");
 if(!rm_upper_eacute||!rm_lower_eacute||!rm_ascii_e)return false;
 if(!selftest_region(v,"bmp_exact",1,true,rm_upper_eacute,rm_upper_eacute,0,0,1,true))return false;
 if(!selftest_region(v,"bmp_case_pair",1,true,rm_upper_eacute,rm_lower_eacute,0,0,1,false))return false;
 if(!selftest_region(v,"bmp_case_pair_exact",0,false,rm_upper_eacute,rm_lower_eacute,0,0,1,false))return false;
 if(!selftest_region(v,"bmp_vs_ascii",1,true,rm_upper_eacute,rm_ascii_e,0,0,1,false))return false;
 /* len counts UTF-16 code units: the emoji is two units, so len=1 compares
  * only its high surrogate and matches it, as Java's code-unit contract does. */
 dvm_ref rm_emoji_self=dvm_new_string(v,"a\xF0\x9F\x98\x80" "b");
 dvm_ref rm_emoji=dvm_new_string(v,"\xF0\x9F\x98\x80");
 if(!rm_emoji_self||!rm_emoji)return false;
 if(!selftest_region(v,"surrogate_pair",1,true,rm_emoji_self,rm_emoji,1,0,2,true))return false;
 if(!selftest_region(v,"surrogate_unit",1,true,rm_emoji_self,rm_emoji,1,0,1,true))return false;
 /* String.codePointAt(int): UTF-16 indices into "a", NUL, U+1F600, "b".
  * The high-surrogate index returns the combined supplementary code point;
  * the low-surrogate index returns that code unit alone, as Java does. */
 union dvm_value cp_arg={0};
 dvm_ref cp_bmp=dvm_new_string(v,"A\xE4\xB8\xAD");
 dvm_ref cp_lone_high=dvm_new_string(v,"\xED\xA0\xBD");
 if(!cp_bmp||!cp_lone_high)return false;
 cp_arg.i=0;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",value,&cp_arg,1,&out)||out.i!=0x61)return false;
 cp_arg.i=1;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",value,&cp_arg,1,&out)||out.i!=0)return false;
 cp_arg.i=2;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",value,&cp_arg,1,&out)||out.i!=0x1F600)return false;
 cp_arg.i=3;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",value,&cp_arg,1,&out)||out.i!=0xDE00)return false;
 cp_arg.i=4;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",value,&cp_arg,1,&out)||out.i!=0x62)return false;
 cp_arg.i=0;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",cp_bmp,&cp_arg,1,&out)||out.i!=0x41)return false;
 cp_arg.i=1;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",cp_bmp,&cp_arg,1,&out)||out.i!=0x4E2D)return false;
 cp_arg.i=0;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",cp_lone_high,&cp_arg,1,&out)||out.i!=0xD83D)return false;
 if(!selftest_codepoint_oob(v,value,-1))return false;
 if(!selftest_codepoint_oob(v,value,5))return false;
 /* Wrong arity/signature and a non-String receiver stay unhandled. */
 union dvm_value cp_wrong={.j=0};
 if(string_runtime_call(v,"java/lang/String","codePointAt","(J)I",value,&cp_wrong,1,&out))return false;
 if(dvm_exception(v))return false;
 struct dvm_class *cp_plain_cls=dvm_find_class_or_external(v,"java/lang/Object");
 dvm_ref cp_not_string=cp_plain_cls?dvm_new_object(v,cp_plain_cls):0;
 if(!cp_not_string)return false;
 cp_arg.i=0;
 if(string_runtime_call(v,"java/lang/String","codePointAt","(I)I",cp_not_string,&cp_arg,1,&out))return false;
 if(dvm_exception(v))return false;
 /* String.toLowerCase(Locale): the device path reads Locale.US through the
  * external static hook and lowercases an IDN.toASCII host.  Drive the same
  * java_basics_static registration and the exact external signature.  The
  * result must be a new String (the receiver is never mutated), null locale
  * must raise NullPointerException, and signatures matching neither overload
  * must stay unhandled.  The non-ASCII case is the documented conservative
  * boundary:
  * U+00C9 has no simple-lowercase mapping here, so it is preserved. */
 union dvm_value locale_static={0};
 if(!java_basics_static(v,"java/util/Locale","US","Ljava/util/Locale;",&locale_static)||!locale_static.l)return false;
 union dvm_value locale_arg={.l=locale_static.l};
 dvm_ref mixed_host=dvm_new_string(v,"Game.BiliBili.COM");
 if(!mixed_host)return false;
 if(!string_runtime_call(v,"java/lang/String","toLowerCase","(Ljava/util/Locale;)Ljava/lang/String;",mixed_host,&locale_arg,1,&out)||!out.l)return false;
 if(out.l==mixed_host)return false;
 const char *lowered=dvm_string_utf8(v,out.l);
 if(!lowered||strcmp(lowered,"game.bilibili.com"))return false;
 const char *mixed_original=dvm_string_utf8(v,mixed_host);
 if(!mixed_original||strcmp(mixed_original,"Game.BiliBili.COM"))return false;
 dvm_ref empty_source=dvm_new_string(v,"");
 if(!empty_source)return false;
 if(!string_runtime_call(v,"java/lang/String","toLowerCase","(Ljava/util/Locale;)Ljava/lang/String;",empty_source,&locale_arg,1,&out)||!out.l||out.l==empty_source||dvm_string_utf8_length(v,out.l)!=0)return false;
 dvm_ref accent_source=dvm_new_string(v,"\xC3\x89" "A");
 if(!accent_source)return false;
 if(!string_runtime_call(v,"java/lang/String","toLowerCase","(Ljava/util/Locale;)Ljava/lang/String;",accent_source,&locale_arg,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"\xC3\x89" "a"))return false;
 union dvm_value null_locale={0};
 bool null_handled=string_runtime_call(v,"java/lang/String","toLowerCase","(Ljava/util/Locale;)Ljava/lang/String;",mixed_host,&null_locale,1,&out);
 dvm_ref null_exception=dvm_exception(v);
 struct dvm_class *null_class=null_exception?dvm_object_class(v,null_exception):NULL;
 bool null_npe=null_handled&&null_class&&null_class->name&&!strcmp(null_class->name,"java/lang/NullPointerException");
 dvm_clear_exception(v);
 if(!null_npe)return false;
 /* String.toLowerCase() (no Locale): Request$3.onFailure lowercases
  * IOException.getMessage() before matching the literal "canceled".  The same
  * locale-independent ASCII mapping applies; the result is a fresh String, the
  * empty input yields a different empty String, the receiver is untouched, and
  * non-ASCII text (U+00C9 and a surrogate pair) is preserved exactly. */
 if(!string_runtime_call(v,"java/lang/String","toLowerCase","()Ljava/lang/String;",mixed_host,NULL,0,&out)||!out.l)return false;
 if(out.l==mixed_host)return false;
 const char *default_lowered=dvm_string_utf8(v,out.l);
 if(!default_lowered||strcmp(default_lowered,"game.bilibili.com"))return false;
 if(!mixed_original||strcmp(dvm_string_utf8(v,mixed_host),"Game.BiliBili.COM"))return false;
 if(!string_runtime_call(v,"java/lang/String","toLowerCase","()Ljava/lang/String;",empty_source,NULL,0,&out)||!out.l||out.l==empty_source||dvm_string_utf8_length(v,out.l)!=0)return false;
 if(!string_runtime_call(v,"java/lang/String","toLowerCase","()Ljava/lang/String;",accent_source,NULL,0,&out)||!out.l||out.l==accent_source||strcmp(dvm_string_utf8(v,out.l),"\xC3\x89" "a"))return false;
 dvm_ref emoji_source=dvm_new_string(v,"\xF0\x9F\x98\x80" "A");
 if(!emoji_source)return false;
 if(!string_runtime_call(v,"java/lang/String","toLowerCase","()Ljava/lang/String;",emoji_source,NULL,0,&out)||!out.l||out.l==emoji_source||strcmp(dvm_string_utf8(v,out.l),"\xF0\x9F\x98\x80" "a"))return false;
 /* Wrong arity/signature and a non-String receiver stay unhandled. */
 union dvm_value lower_int_arg={.i=0};
 if(string_runtime_call(v,"java/lang/String","toLowerCase","(I)Ljava/lang/String;",mixed_host,&lower_int_arg,1,&out))return false;
 if(string_runtime_call(v,"java/lang/String","toLowerCase","()Ljava/lang/String;",mixed_host,&lower_int_arg,1,&out))return false;
 struct dvm_class *lower_plain_cls=dvm_find_class_or_external(v,"java/lang/Object");
 dvm_ref lower_not_string=lower_plain_cls?dvm_new_object(v,lower_plain_cls):0;
 if(!lower_not_string)return false;
 if(string_runtime_call(v,"java/lang/String","toLowerCase","()Ljava/lang/String;",lower_not_string,NULL,0,&out))return false;
 if(string_runtime_call(v,"java/lang/String","toLowerCase","(Ljava/lang/String;)Ljava/lang/String;",mixed_host,&locale_arg,1,&out))return false;
 if(dvm_exception(v))return false;
 /* String.toUpperCase(Locale): params2BodyMap upper-cases the first character
  * of a @RequestParam name with Locale.getDefault(); other DEX sites pass
  * Locale.US/ENGLISH/ROOT.  Locale.US and Locale.ROOT are driven here, plus
  * the platform runtime's fixed en_US default.  ASCII folds exactly, the
  * result is a fresh String, null locale raises NullPointerException, an
  * unrecognized Locale object fails closed with UnsupportedOperationException
  * (the Turkish/Azeri dotted-i branch is the other fail-closed case), and
  * signatures matching neither overload stay unhandled.  Non-ASCII is the
  * documented conservative boundary: U+00E9 is preserved, not folded. */
 union dvm_value upper_us={0};
 if(!java_basics_static(v,"java/util/Locale","US","Ljava/util/Locale;",&upper_us)||!upper_us.l)return false;
 union dvm_value upper_root={0};
 if(!java_basics_static(v,"java/util/Locale","ROOT","Ljava/util/Locale;",&upper_root)||!upper_root.l)return false;
 union dvm_value upper_english={0};
 if(!java_basics_static(v,"java/util/Locale","ENGLISH","Ljava/util/Locale;",&upper_english)||!upper_english.l)return false;
 if(upper_english.l==upper_us.l||upper_english.l==upper_root.l)return false;
 dvm_ref upper_mixed=dvm_new_string(v,"Game.BiliBili.COM");
 if(!upper_mixed)return false;
 union dvm_value upper_locale_arg={.l=upper_us.l};
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_mixed,&upper_locale_arg,1,&out)||!out.l)return false;
 if(out.l==upper_mixed||strcmp(dvm_string_utf8(v,out.l),"GAME.BILIBILI.COM"))return false;
 if(strcmp(dvm_string_utf8(v,upper_mixed),"Game.BiliBili.COM"))return false;
 upper_locale_arg.l=upper_root.l;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_mixed,&upper_locale_arg,1,&out)||!out.l||out.l==upper_mixed||strcmp(dvm_string_utf8(v,out.l),"GAME.BILIBILI.COM"))return false;
 upper_locale_arg.l=upper_english.l;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_mixed,&upper_locale_arg,1,&out)||!out.l||out.l==upper_mixed||strcmp(dvm_string_utf8(v,out.l),"GAME.BILIBILI.COM"))return false;
 upper_locale_arg.l=upper_root.l;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",empty_source,&upper_locale_arg,1,&out)||!out.l||out.l==empty_source||dvm_string_utf8_length(v,out.l)!=0)return false;
 dvm_ref upper_accent=dvm_new_string(v,"\xC3\xA9" "a");
 dvm_ref upper_emoji=dvm_new_string(v,"\xF0\x9F\x98\x80" "a");
 static const char upper_nul_bytes[]={'a',(char)0xC0,(char)0x80,'b'};
 dvm_ref upper_nul=dvm_new_string_n(v,upper_nul_bytes,sizeof upper_nul_bytes);
 if(!upper_accent||!upper_emoji||!upper_nul)return false;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_accent,&upper_locale_arg,1,&out)||!out.l||out.l==upper_accent||strcmp(dvm_string_utf8(v,out.l),"\xC3\xA9" "A"))return false;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_emoji,&upper_locale_arg,1,&out)||!out.l||out.l==upper_emoji||strcmp(dvm_string_utf8(v,out.l),"\xF0\x9F\x98\x80" "A"))return false;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_nul,&upper_locale_arg,1,&out)||!out.l||out.l==upper_nul)return false;
 union dvm_value upper_index={.i=0},upper_cp={0};
 if(!string_runtime_call(v,"java/lang/String","length","()I",out.l,NULL,0,&upper_cp)||upper_cp.i!=3)return false;
 upper_index.i=0;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",out.l,&upper_index,1,&upper_cp)||upper_cp.i!='A')return false;
 upper_index.i=1;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",out.l,&upper_index,1,&upper_cp)||upper_cp.i!=0)return false;
 upper_index.i=2;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",out.l,&upper_index,1,&upper_cp)||upper_cp.i!='B')return false;
 /* A null Locale must raise NullPointerException exactly as Java does. */
 union dvm_value upper_null_locale={0};
 bool upper_null_handled=string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_mixed,&upper_null_locale,1,&out);
 dvm_ref upper_null_exception=dvm_exception(v);
 struct dvm_class *upper_null_class=upper_null_exception?dvm_object_class(v,upper_null_exception):NULL;
 bool upper_null_npe=upper_null_handled&&upper_null_class&&upper_null_class->name&&!strcmp(upper_null_class->name,"java/lang/NullPointerException");
 dvm_clear_exception(v);
 if(!upper_null_npe)return false;
 /* A Locale object created outside the two known runtimes has no language we
  * can trust: it must fail closed, not return an en_US guess. */
 struct dvm_class *upper_locale_cls=dvm_find_class_or_external(v,"java/util/Locale");
 dvm_ref upper_unknown_locale=upper_locale_cls?dvm_new_object(v,upper_locale_cls):0;
 if(!upper_unknown_locale)return false;
 union dvm_value upper_unknown_arg={.l=upper_unknown_locale};
 bool upper_unknown_handled=string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_mixed,&upper_unknown_arg,1,&out);
 dvm_ref upper_unknown_exception=dvm_exception(v);
 struct dvm_class *upper_unknown_class=upper_unknown_exception?dvm_object_class(v,upper_unknown_exception):NULL;
 bool upper_unknown_uoe=upper_unknown_handled&&upper_unknown_class&&upper_unknown_class->name&&!strcmp(upper_unknown_class->name,"java/lang/UnsupportedOperationException");
 dvm_clear_exception(v);
 if(!upper_unknown_uoe)return false;
 /* A non-Locale argument is impossible at a verified call site; this VM is
  * unverified, so it stays fail-closed with ClassCastException. */
 union dvm_value upper_nonlocale_arg={.l=upper_mixed};
 bool upper_nonlocale_handled=string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_mixed,&upper_nonlocale_arg,1,&out);
 dvm_ref upper_nonlocale_exception=dvm_exception(v);
 struct dvm_class *upper_nonlocale_class=upper_nonlocale_exception?dvm_object_class(v,upper_nonlocale_exception):NULL;
 bool upper_nonlocale_cce=upper_nonlocale_handled&&upper_nonlocale_class&&upper_nonlocale_class->name&&!strcmp(upper_nonlocale_class->name,"java/lang/ClassCastException");
 dvm_clear_exception(v);
 if(!upper_nonlocale_cce)return false;
 /* String.toUpperCase() (no Locale): resolve the platform runtime's fixed
  * en_US default and apply the same mapping; a fresh String is returned for
  * the empty input and non-ASCII text is preserved exactly. */
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","()Ljava/lang/String;",upper_mixed,NULL,0,&out)||!out.l||out.l==upper_mixed||strcmp(dvm_string_utf8(v,out.l),"GAME.BILIBILI.COM"))return false;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","()Ljava/lang/String;",empty_source,NULL,0,&out)||!out.l||out.l==empty_source||dvm_string_utf8_length(v,out.l)!=0)return false;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","()Ljava/lang/String;",upper_accent,NULL,0,&out)||!out.l||out.l==upper_accent||strcmp(dvm_string_utf8(v,out.l),"\xC3\xA9" "A"))return false;
 if(!string_runtime_call(v,"java/lang/String","toUpperCase","()Ljava/lang/String;",upper_emoji,NULL,0,&out)||!out.l||out.l==upper_emoji||strcmp(dvm_string_utf8(v,out.l),"\xF0\x9F\x98\x80" "A"))return false;
 /* Wrong arity/signature and a non-String receiver stay unhandled. */
 union dvm_value upper_int_arg={.i=0};
 if(string_runtime_call(v,"java/lang/String","toUpperCase","(I)Ljava/lang/String;",upper_mixed,&upper_int_arg,1,&out))return false;
 if(string_runtime_call(v,"java/lang/String","toUpperCase","()Ljava/lang/String;",upper_mixed,&upper_int_arg,1,&out))return false;
 if(string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/lang/String;)Ljava/lang/String;",upper_mixed,&upper_locale_arg,1,&out))return false;
 struct dvm_class *upper_plain_cls=dvm_find_class_or_external(v,"java/lang/Object");
 dvm_ref upper_not_string=upper_plain_cls?dvm_new_object(v,upper_plain_cls):0;
 if(!upper_not_string)return false;
 if(string_runtime_call(v,"java/lang/String","toUpperCase","()Ljava/lang/String;",upper_not_string,NULL,0,&out))return false;
 if(string_runtime_call(v,"java/lang/String","toUpperCase","(Ljava/util/Locale;)Ljava/lang/String;",upper_not_string,&upper_locale_arg,1,&out))return false;
 if(dvm_exception(v))return false;
 /* String.equalsIgnoreCase(String): HttpUrl$Builder.isDot/isDotDot compare
  * canonicalized path segments against "." / ".." and the percent-encoded
  * literals.  ASCII folds symmetrically over UTF-16 code units; a null
  * argument is false without throwing; non-ASCII simple-case pairs such as
  * U+00C9/U+00E9 compare unequal here (documented conservative subset: no
  * Unicode simple case mapping, no locale or host tolower).  Non-BMP equality
  * is exact over surrogate code units.  A non-String receiver, a non-null
  * non-String argument and any other signature stay unhandled. */
 dvm_ref eq_upper_abc=dvm_new_string(v,"AbC");
 dvm_ref eq_mixed_abc=dvm_new_string(v,"aBc");
 dvm_ref eq_abc=dvm_new_string(v,"abc");
 dvm_ref eq_abd=dvm_new_string(v,"abd");
 dvm_ref eq_ab=dvm_new_string(v,"ab");
 dvm_ref eq_pct_upper=dvm_new_string(v,"%2E");
 dvm_ref eq_pct_lower=dvm_new_string(v,"%2e");
 dvm_ref eq_dot_pct_upper=dvm_new_string(v,".%2E%2E");
 dvm_ref eq_dot_pct_lower=dvm_new_string(v,".%2e%2e");
 dvm_ref eq_upper_eacute=dvm_new_string(v,"\xC3\x89");
 dvm_ref eq_lower_eacute=dvm_new_string(v,"\xC3\xA9");
 dvm_ref eq_emoji=dvm_new_string(v,"\xF0\x9F\x98\x80");
 dvm_ref eq_emoji_other=dvm_new_string(v,"\xF0\x9F\x98\x81");
 if(!eq_upper_abc||!eq_mixed_abc||!eq_abc||!eq_abd||!eq_ab||!eq_pct_upper||!eq_pct_lower||!eq_dot_pct_upper||!eq_dot_pct_lower||!eq_upper_eacute||!eq_lower_eacute||!eq_emoji||!eq_emoji_other)return false;
 if(!selftest_equals_ignore_case(v,"ascii_mixed",eq_upper_abc,eq_mixed_abc,false,true,true))return false;
 if(!selftest_equals_ignore_case(v,"ascii_same",eq_abc,eq_abc,false,true,true))return false;
 if(!selftest_equals_ignore_case(v,"ascii_differ",eq_abc,eq_abd,false,false,true))return false;
 if(!selftest_equals_ignore_case(v,"length_differ",eq_abc,eq_ab,false,false,true))return false;
 if(!selftest_equals_ignore_case(v,"null_arg",eq_abc,0,true,false,true))return false;
 if(!selftest_equals_ignore_case(v,"percent_encoded_fold",eq_pct_upper,eq_pct_lower,false,true,true))return false;
 if(!selftest_equals_ignore_case(v,"dot_percent_encoded_fold",eq_dot_pct_upper,eq_dot_pct_lower,false,true,true))return false;
 if(!selftest_equals_ignore_case(v,"non_ascii_exact",eq_upper_eacute,eq_upper_eacute,false,true,true))return false;
 if(!selftest_equals_ignore_case(v,"non_ascii_case_pair",eq_upper_eacute,eq_lower_eacute,false,false,true))return false;
 if(!selftest_equals_ignore_case(v,"non_bmp_exact",eq_emoji,eq_emoji,false,true,true))return false;
 if(!selftest_equals_ignore_case(v,"non_bmp_differ",eq_emoji,eq_emoji_other,false,false,true))return false;
 struct dvm_class *eq_plain_cls=dvm_find_class_or_external(v,"java/lang/Object");
 dvm_ref eq_not_string=eq_plain_cls?dvm_new_object(v,eq_plain_cls):0;
 if(!eq_not_string)return false;
 if(!selftest_equals_ignore_case(v,"non_string_receiver",eq_not_string,eq_abc,false,false,false))return false;
 if(!selftest_equals_ignore_case(v,"non_string_arg",eq_abc,eq_not_string,false,false,false))return false;
 union dvm_value eq_arg={.l=eq_mixed_abc};
 if(string_runtime_call(v,"java/lang/String","equalsIgnoreCase","(Ljava/lang/Object;)Z",eq_abc,&eq_arg,1,&out))return false;
 if(dvm_exception(v))return false;
 if(string_runtime_call(v,"java/lang/String","unsupportedMethod","()V",value,NULL,0,&out))return false;
 /* String(byte[], [int,int,] Charset): okio.Buffer.readString allocates the
  * receiver with new-instance and then invokes these constructors.  The
  * tests drive both arities, offset/length slicing, Chinese UTF-8, embedded
  * NUL and non-BMP code points, Java's malformed-UTF-8 U+FFFD replacement
  * rules, fail-closed non-UTF-8 charsets, null/type/bounds errors, and
  * receiver identity with an unchanged input byte[]. */
 dvm_ref utf8_charset=selftest_charset_for_name(v,"UTF-8");
 dvm_ref utf16_charset=selftest_charset_for_name(v,"UTF-16LE");
 static const unsigned char init_ascii[]={'a','b','c'};
 static const unsigned char init_slice[]={'x','a','b','c','x'};
 static const unsigned char init_chinese[]={0xe4,0xb8,0xad};
 static const unsigned char init_nul[]={'a',0,'b'};
 static const unsigned char init_emoji[]={0xf0,0x9f,0x98,0x80};
 static const unsigned char init_mixed[]={'a',0,0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
 static const unsigned char init_bad_lone[]={0x80};
 static const unsigned char init_bad_c0[]={0xc0,0xaf};
 static const unsigned char init_bad_e4_trunc[]={0xe4,0xb8};
 static const unsigned char init_bad_e4_a[]={0xe4,0xb8,'A'};
 static const unsigned char init_bad_ed[]={0xed,0xa0,0x80};
 static const unsigned char init_bad_f0_trunc[]={0xf0,0x9f,0x98};
 static const unsigned char init_bad_f0_mid[]={0xf0,0x9f,'A',0x80};
 static const unsigned char init_bad_f4[]={0xf4,0x90,0x80,0x80};
 static const unsigned char init_bad_e0[]={0xe0,0x80,0x80};
 static const unsigned char init_max_cp[]={0xf4,0x8f,0xbf,0xbf};
 static const unsigned char init_min_supp[]={0xf0,0x90,0x80,0x80};
 static const char replacement[]="\xef\xbf\xbd";
 static const char replacement_a[]="\xef\xbf\xbd" "A";
 static const char replacement_a_replacement[]="\xef\xbf\xbd" "A" "\xef\xbf\xbd";
 static const char two_replacements[]="\xef\xbf\xbd\xef\xbf\xbd";
 static const char three_replacements[]="\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd";
 static const char four_replacements[]="\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd";
 if(!utf8_charset||!utf16_charset)return false;
 dvm_ref b_ascii=selftest_byte_array(v,init_ascii,sizeof init_ascii);
 dvm_ref b_slice=selftest_byte_array(v,init_slice,sizeof init_slice);
 dvm_ref b_chinese=selftest_byte_array(v,init_chinese,sizeof init_chinese);
 dvm_ref b_nul=selftest_byte_array(v,init_nul,sizeof init_nul);
 dvm_ref b_emoji=selftest_byte_array(v,init_emoji,sizeof init_emoji);
 dvm_ref b_mixed=selftest_byte_array(v,init_mixed,sizeof init_mixed);
 dvm_ref b_empty=selftest_byte_array(v,NULL,0);
 dvm_ref b_bad_lone=selftest_byte_array(v,init_bad_lone,sizeof init_bad_lone);
 dvm_ref b_bad_c0=selftest_byte_array(v,init_bad_c0,sizeof init_bad_c0);
 dvm_ref b_bad_e4_trunc=selftest_byte_array(v,init_bad_e4_trunc,sizeof init_bad_e4_trunc);
 dvm_ref b_bad_e4_a=selftest_byte_array(v,init_bad_e4_a,sizeof init_bad_e4_a);
 dvm_ref b_bad_ed=selftest_byte_array(v,init_bad_ed,sizeof init_bad_ed);
 dvm_ref b_bad_f0_trunc=selftest_byte_array(v,init_bad_f0_trunc,sizeof init_bad_f0_trunc);
 dvm_ref b_bad_f0_mid=selftest_byte_array(v,init_bad_f0_mid,sizeof init_bad_f0_mid);
 dvm_ref b_bad_f4=selftest_byte_array(v,init_bad_f4,sizeof init_bad_f4);
 dvm_ref b_bad_e0=selftest_byte_array(v,init_bad_e0,sizeof init_bad_e0);
 dvm_ref b_max_cp=selftest_byte_array(v,init_max_cp,sizeof init_max_cp);
 dvm_ref b_min_supp=selftest_byte_array(v,init_min_supp,sizeof init_min_supp);
 if(!b_ascii||!b_slice||!b_chinese||!b_nul||!b_emoji||!b_mixed||!b_empty||!b_bad_lone||!b_bad_c0||!b_bad_e4_trunc||!b_bad_e4_a||!b_bad_ed||!b_bad_f0_trunc||!b_bad_f0_mid||!b_bad_f4||!b_bad_e0||!b_max_cp||!b_min_supp)return false;
 if(!selftest_init_ok(v,"ascii_2arg",0,b_ascii,utf8_charset,0,0,false,"abc",3))return false;
 if(!selftest_init_ok(v,"ascii_4arg",0,b_ascii,utf8_charset,0,3,true,"abc",3))return false;
 if(!selftest_init_ok(v,"slice",0,b_slice,utf8_charset,1,3,true,"abc",3))return false;
 if(!selftest_init_ok(v,"slice_empty_at_end",0,b_slice,utf8_charset,5,0,true,"",0))return false;
 if(!selftest_init_ok(v,"empty_2arg",0,b_empty,utf8_charset,0,0,false,"",0))return false;
 if(!selftest_init_ok(v,"empty_4arg",0,b_empty,utf8_charset,0,0,true,"",0))return false;
 /* UTF-16 length and codePointAt prove the payload is a real String, not
  * just a copied byte buffer: Chinese is one UTF-16 unit, the emoji is two,
  * and the embedded NUL is its own unit. */
 dvm_ref chinese_self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));
 dvm_ref emoji_self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));
 dvm_ref nul_self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));
 dvm_ref mixed_self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));
 if(!chinese_self||!emoji_self||!nul_self||!mixed_self)return false;
 if(!selftest_init_ok(v,"chinese",chinese_self,b_chinese,utf8_charset,0,3,true,(const char*)init_chinese,3))return false;
 if(!selftest_init_ok(v,"emoji",emoji_self,b_emoji,utf8_charset,0,0,false,(const char*)init_emoji,4))return false;
 if(!selftest_init_ok(v,"nul",nul_self,b_nul,utf8_charset,0,0,false,(const char*)init_nul,3))return false;
 if(!selftest_init_ok(v,"mixed",mixed_self,b_mixed,utf8_charset,0,9,true,(const char*)init_mixed,9))return false;
 union dvm_value prop={0};
 if(!string_runtime_call(v,"java/lang/String","length","()I",chinese_self,NULL,0,&prop)||prop.i!=1)return false;
 prop.i=0;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",chinese_self,&prop,1,&out)||out.i!=0x4e2d)return false;
 if(!string_runtime_call(v,"java/lang/String","length","()I",emoji_self,NULL,0,&prop)||prop.i!=2)return false;
 prop.i=0;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",emoji_self,&prop,1,&out)||out.i!=0x1f600)return false;
 if(!string_runtime_call(v,"java/lang/String","length","()I",nul_self,NULL,0,&prop)||prop.i!=3)return false;
 prop.i=1;if(!string_runtime_call(v,"java/lang/String","codePointAt","(I)I",nul_self,&prop,1,&out)||out.i!=0)return false;
 if(!string_runtime_call(v,"java/lang/String","length","()I",mixed_self,NULL,0,&prop)||prop.i!=5)return false;
 /* Java UTF-8 aliases registered by the platform: "UTF8" and
  * "unicode-1-1-utf-8" must behave exactly like "UTF-8". */
 struct dvm_class *charset_cls=dvm_find_class_or_external(v,"java/nio/charset/Charset");
 dvm_ref alias_utf8=charset_cls?dvm_new_object(v,charset_cls):0;
 dvm_ref alias_unicode=charset_cls?dvm_new_object(v,charset_cls):0;
 if(!alias_utf8||!alias_unicode)return false;
 selftest_alias_refs[0]=alias_utf8;selftest_alias_names[0]="UTF8";
 selftest_alias_refs[1]=alias_unicode;selftest_alias_names[1]="unicode-1-1-utf-8";
 bool alias_ok=selftest_init_ok(v,"alias_utf8",0,b_ascii,alias_utf8,0,0,false,"abc",3)
  &&selftest_init_ok(v,"alias_unicode",0,b_ascii,alias_unicode,0,3,true,"abc",3);
 selftest_alias_refs[0]=selftest_alias_refs[1]=0;selftest_alias_names[0]=selftest_alias_names[1]=NULL;
 if(!alias_ok)return false;
 /* Re-initializing an existing String keeps the same object and replaces its
  * payload (the bytecode new-instance receiver has the same shape). */
 dvm_ref reused=dvm_new_string(v,"old");
 if(!reused||!selftest_init_ok(v,"reinitialize",reused,b_ascii,utf8_charset,0,3,true,"abc",3))return false;
 /* Null and type errors. */
 if(!selftest_init_error(v,"null_charset_2arg",0,b_ascii,0,0,0,false,"java/lang/NullPointerException",true))return false;
 if(!selftest_init_error(v,"null_charset_4arg",0,b_ascii,0,0,3,true,"java/lang/NullPointerException",true))return false;
 if(!selftest_init_error(v,"null_bytes_2arg",0,0,utf8_charset,0,0,false,"java/lang/NullPointerException",true))return false;
 if(!selftest_init_error(v,"null_bytes_4arg",0,0,utf8_charset,0,3,true,"java/lang/NullPointerException",true))return false;
 dvm_ref ints=dvm_new_array(v,'I',"I",1);
 if(!ints||!selftest_init_error(v,"non_byte_array",0,ints,utf8_charset,0,0,false,"java/lang/IllegalArgumentException",true))return false;
 if(!selftest_init_error(v,"negative_offset",0,b_ascii,utf8_charset,-1,3,true,"java/lang/StringIndexOutOfBoundsException",true))return false;
 if(!selftest_init_error(v,"negative_length",0,b_ascii,utf8_charset,0,-1,true,"java/lang/StringIndexOutOfBoundsException",true))return false;
 if(!selftest_init_error(v,"offset_past_end",0,b_ascii,utf8_charset,4,0,true,"java/lang/StringIndexOutOfBoundsException",true))return false;
 if(!selftest_init_error(v,"range_past_end",0,b_ascii,utf8_charset,1,3,true,"java/lang/StringIndexOutOfBoundsException",true))return false;
 /* A non-UTF-8 charset must fail closed and must not touch the receiver. */
 dvm_ref keep=dvm_new_string(v,"keep");
 if(!keep||!selftest_init_error(v,"unsupported_charset",keep,b_ascii,utf16_charset,0,3,true,"java/lang/UnsupportedOperationException",true))return false;
 struct dvm_object *keep_o=dvm__obj(v,keep);
 if(!keep_o||keep_o->kind!=DVM_OBJ_STRING||keep_o->utf8_len!=4||memcmp(keep_o->utf8,"keep",4)){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL string_init_unsupported_charset receiver_mutated=1\n");return false;
 }
 dvm_ref object_self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Object"));
 if(!object_self||!selftest_init_error(v,"non_string_receiver",object_self,b_ascii,utf8_charset,0,0,false,NULL,false))return false;
 /* Unknown constructor signatures stay unhandled and leave no exception. */
 dvm_ref sig_self=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));
 union dvm_value sig_args[4]={{.l=b_ascii},{.i=0},{.i=3},{.l=utf8_charset}},sig_out={0};
 if(!sig_self)return false;
 if(string_runtime_call(v,"java/lang/String","<init>","([B)V",sig_self,sig_args,1,&sig_out))return false;
 if(string_runtime_call(v,"java/lang/String","<init>","([BIILjava/lang/Object;)V",sig_self,sig_args,4,&sig_out))return false;
 if(string_runtime_call(v,"java/lang/String","<init>","([BIILjava/nio/charset/Charset;)V",sig_self,sig_args,3,&sig_out))return false;
 if(dvm_exception(v))return false;
 /* Malformed UTF-8: OpenJDK replaces each malformed subsequence with U+FFFD
  * and consumes only the bytes that cannot start a valid sequence. */
 if(!selftest_init_ok(v,"bad_lone_80",0,b_bad_lone,utf8_charset,0,1,true,replacement,3))return false;
 if(!selftest_init_ok(v,"bad_c0_af",0,b_bad_c0,utf8_charset,0,2,true,two_replacements,6))return false;
 if(!selftest_init_ok(v,"bad_e4_truncated",0,b_bad_e4_trunc,utf8_charset,0,2,true,replacement,3))return false;
 if(!selftest_init_ok(v,"bad_e4_then_ascii",0,b_bad_e4_a,utf8_charset,0,3,true,replacement_a,4))return false;
 if(!selftest_init_ok(v,"bad_surrogate_ed",0,b_bad_ed,utf8_charset,0,3,true,replacement,3))return false;
 if(!selftest_init_ok(v,"bad_f0_truncated",0,b_bad_f0_trunc,utf8_charset,0,3,true,replacement,3))return false;
 if(!selftest_init_ok(v,"bad_f0_then_ascii",0,b_bad_f0_mid,utf8_charset,0,4,true,replacement_a_replacement,7))return false;
 if(!selftest_init_ok(v,"bad_f4_overflow",0,b_bad_f4,utf8_charset,0,4,true,four_replacements,12))return false;
 if(!selftest_init_ok(v,"bad_e0_overlong",0,b_bad_e0,utf8_charset,0,3,true,three_replacements,9))return false;
 if(!selftest_init_ok(v,"max_codepoint",0,b_max_cp,utf8_charset,0,4,true,(const char*)init_max_cp,4))return false;
 if(!selftest_init_ok(v,"min_supplementary",0,b_min_supp,utf8_charset,0,4,true,(const char*)init_min_supp,4))return false;
 /* String.getBytes: FormatStrategy.log encodes its formatted line with the
  * no-argument overload (UTF-8, Android's default).  The tests drive the real
  * UTF-16 code-unit encoder: ASCII, Chinese, a non-BMP emoji, isolated high
  * and low surrogates (each becomes '?', not a dropped character), an embedded
  * U+0000 that must become a real 0x00 byte, and the empty string (fresh
  * zero-length array).  UTF-16LE/BE emit no BOM and UTF-16 emits FE FF; an
  * unknown name is UnsupportedEncodingException, a null name is NPE, and a
  * real charset outside this runtime's set fails closed. */
 static const unsigned char gb_ascii[]={'a','b','c'};
 static const unsigned char gb_chinese[]={0xe4,0xb8,0xad};
 static const unsigned char gb_emoji[]={0xf0,0x9f,0x98,0x80};
 static const unsigned char gb_question[]={'?'};
 static const unsigned char gb_nul[]={'a',0,'b'};
 static const unsigned char gb_utf16le_chinese[]={0x2d,0x4e};
 static const unsigned char gb_utf16be_chinese[]={0x4e,0x2d};
 static const unsigned char gb_utf16_chinese[]={0xfe,0xff,0x4e,0x2d};
 static const unsigned char gb_utf16le_emoji[]={0x3d,0xd8,0x00,0xde};
 static const unsigned char gb_utf16le_question[]={0x3f,0x00};
 static const unsigned char gb_utf16be_question[]={0x00,0x3f};
 static const char gb_nul_source[]={'a',(char)0xc0,(char)0x80,'b'};
 dvm_ref gb_ascii_s=dvm_new_string(v,"abc");
 dvm_ref gb_chinese_s=dvm_new_string(v,"\xe4\xb8\xad");
 dvm_ref gb_emoji_s=dvm_new_string(v,"\xf0\x9f\x98\x80");
 dvm_ref gb_lone_high_s=dvm_new_string(v,"\xed\xa0\xbd");
 dvm_ref gb_lone_low_s=dvm_new_string(v,"\xed\xb0\x80");
 dvm_ref gb_nul_s=dvm_new_string_n(v,gb_nul_source,sizeof gb_nul_source);
 dvm_ref gb_empty_s=dvm_new_string(v,"");
 dvm_ref gb_alias_utf8=dvm_new_string(v,"UTF8");
 dvm_ref gb_alias_unicode=dvm_new_string(v,"unicode-1-1-utf-8");
 dvm_ref gb_utf16le_name=dvm_new_string(v,"UTF-16LE");
 dvm_ref gb_utf16be_name=dvm_new_string(v,"UTF-16BE");
 dvm_ref gb_utf16_name=dvm_new_string(v,"UTF-16");
 dvm_ref gb_unknown_name=dvm_new_string(v,"X-NOT-A-CHARSET");
 dvm_ref gb_ascii_name=dvm_new_string(v,"US-ASCII");
 dvm_ref gb_utf16be_charset=selftest_charset_for_name(v,"UTF-16BE");
 dvm_ref gb_ascii_charset=selftest_charset_for_name(v,"US-ASCII");
 if(!gb_ascii_s||!gb_chinese_s||!gb_emoji_s||!gb_lone_high_s||!gb_lone_low_s||!gb_nul_s||!gb_empty_s||!gb_alias_utf8||!gb_alias_unicode||!gb_utf16le_name||!gb_utf16be_name||!gb_utf16_name||!gb_unknown_name||!gb_ascii_name||!gb_utf16be_charset||!gb_ascii_charset)return false;
 if(!selftest_get_bytes_ok(v,"default_ascii",gb_ascii_s,"()[B",NULL,0,gb_ascii,sizeof gb_ascii))return false;
 if(!selftest_get_bytes_ok(v,"default_chinese",gb_chinese_s,"()[B",NULL,0,gb_chinese,sizeof gb_chinese))return false;
 if(!selftest_get_bytes_ok(v,"default_emoji",gb_emoji_s,"()[B",NULL,0,gb_emoji,sizeof gb_emoji))return false;
 if(!selftest_get_bytes_ok(v,"default_lone_high",gb_lone_high_s,"()[B",NULL,0,gb_question,sizeof gb_question))return false;
 if(!selftest_get_bytes_ok(v,"default_lone_low",gb_lone_low_s,"()[B",NULL,0,gb_question,sizeof gb_question))return false;
 if(!selftest_get_bytes_ok(v,"default_nul",gb_nul_s,"()[B",NULL,0,gb_nul,sizeof gb_nul))return false;
 if(!selftest_get_bytes_ok(v,"default_empty",gb_empty_s,"()[B",NULL,0,NULL,0))return false;
 union dvm_value gb_arg={.l=gb_alias_utf8};
 if(!selftest_get_bytes_ok(v,"name_utf8_alias",gb_ascii_s,"(Ljava/lang/String;)[B",&gb_arg,1,gb_ascii,sizeof gb_ascii))return false;
 gb_arg.l=gb_alias_unicode;
 if(!selftest_get_bytes_ok(v,"name_unicode_alias",gb_ascii_s,"(Ljava/lang/String;)[B",&gb_arg,1,gb_ascii,sizeof gb_ascii))return false;
 gb_arg.l=gb_utf16le_name;
 if(!selftest_get_bytes_ok(v,"name_utf16le",gb_chinese_s,"(Ljava/lang/String;)[B",&gb_arg,1,gb_utf16le_chinese,sizeof gb_utf16le_chinese))return false;
 if(!selftest_get_bytes_ok(v,"name_utf16le_emoji",gb_emoji_s,"(Ljava/lang/String;)[B",&gb_arg,1,gb_utf16le_emoji,sizeof gb_utf16le_emoji))return false;
 if(!selftest_get_bytes_ok(v,"name_utf16le_lone",gb_lone_high_s,"(Ljava/lang/String;)[B",&gb_arg,1,gb_utf16le_question,sizeof gb_utf16le_question))return false;
 if(!selftest_get_bytes_ok(v,"name_utf16le_empty",gb_empty_s,"(Ljava/lang/String;)[B",&gb_arg,1,NULL,0))return false;
 gb_arg.l=gb_utf16be_name;
 if(!selftest_get_bytes_ok(v,"name_utf16be",gb_chinese_s,"(Ljava/lang/String;)[B",&gb_arg,1,gb_utf16be_chinese,sizeof gb_utf16be_chinese))return false;
 if(!selftest_get_bytes_ok(v,"name_utf16be_lone",gb_lone_high_s,"(Ljava/lang/String;)[B",&gb_arg,1,gb_utf16be_question,sizeof gb_utf16be_question))return false;
 gb_arg.l=gb_utf16_name;
 if(!selftest_get_bytes_ok(v,"name_utf16_bom",gb_chinese_s,"(Ljava/lang/String;)[B",&gb_arg,1,gb_utf16_chinese,sizeof gb_utf16_chinese))return false;
 if(!selftest_get_bytes_ok(v,"name_utf16_empty",gb_empty_s,"(Ljava/lang/String;)[B",&gb_arg,1,NULL,0))return false;
 gb_arg.l=gb_unknown_name;
 if(!selftest_get_bytes_error(v,"unknown_name",gb_ascii_s,"(Ljava/lang/String;)[B",&gb_arg,1,"java/io/UnsupportedEncodingException",true))return false;
 gb_arg.l=gb_ascii_name;
 if(!selftest_get_bytes_error(v,"unsupported_name",gb_ascii_s,"(Ljava/lang/String;)[B",&gb_arg,1,"java/io/UnsupportedEncodingException",true))return false;
 gb_arg.l=0;
 if(!selftest_get_bytes_error(v,"null_name",gb_ascii_s,"(Ljava/lang/String;)[B",&gb_arg,1,"java/lang/NullPointerException",true))return false;
 gb_arg.l=utf8_charset;
 if(!selftest_get_bytes_ok(v,"charset_utf8",gb_ascii_s,"(Ljava/nio/charset/Charset;)[B",&gb_arg,1,gb_ascii,sizeof gb_ascii))return false;
 gb_arg.l=gb_utf16be_charset;
 if(!selftest_get_bytes_ok(v,"charset_utf16be",gb_chinese_s,"(Ljava/nio/charset/Charset;)[B",&gb_arg,1,gb_utf16be_chinese,sizeof gb_utf16be_chinese))return false;
 if(!selftest_get_bytes_ok(v,"charset_utf16be_empty",gb_empty_s,"(Ljava/nio/charset/Charset;)[B",&gb_arg,1,NULL,0))return false;
 gb_arg.l=gb_ascii_charset;
 if(!selftest_get_bytes_error(v,"charset_unsupported",gb_ascii_s,"(Ljava/nio/charset/Charset;)[B",&gb_arg,1,"java/lang/UnsupportedOperationException",true))return false;
 gb_arg.l=0;
 if(!selftest_get_bytes_error(v,"charset_null",gb_ascii_s,"(Ljava/nio/charset/Charset;)[B",&gb_arg,1,"java/lang/NullPointerException",true))return false;
 gb_arg.l=gb_ascii_s;
 if(!selftest_get_bytes_error(v,"charset_wrong_type",gb_ascii_s,"(Ljava/nio/charset/Charset;)[B",&gb_arg,1,"java/lang/ClassCastException",true))return false;
 union dvm_value gb_int={.i=0};
 if(selftest_get_bytes_call(v,gb_ascii_s,"(I)[B",&gb_int,1,&out)){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_wrong_signature handled=1\n");return false;}
 if(dvm_exception(v)){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_wrong_signature exception=1\n");return false;}
 struct dvm_class *gb_object_cls=dvm_find_class_or_external(v,"java/lang/Object");
 dvm_ref gb_object=gb_object_cls?dvm_new_object(v,gb_object_cls):0;
 if(!gb_object)return false;
 if(selftest_get_bytes_call(v,gb_object,"()[B",NULL,0,&out)){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_non_string_receiver handled=1\n");return false;}
 if(dvm_exception(v)){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_non_string_receiver exception=1\n");return false;}
 /* getBytes must not rewrite the receiver; the UTF-16 index is a cache. */
 if(dvm_string_utf8_length(v,gb_chinese_s)!=3||memcmp(dvm_string_utf8(v,gb_chinese_s),gb_chinese,3)){
  sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL get_bytes_receiver_mutated=1\n");return false;
 }
 sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST PRESTAGE exception=%d\n",dvm_exception(v)!=0);
 /* String.split: literal subset, exact Java limit semantics, fail-closed
  * handling of regex metacharacters and empty patterns. */
 {
  static const char *const split_three[]={"a","b","c"};
  static const char *const split_joined[]={"a,b,c"};
  static const char *const split_empty_middle[]={"a","","b"};
  static const char *const split_a[]={"a"};
  static const char *const split_two_bc[]={"a","b,c"};
  static const char *const split_ab_empty[]={"a","b",""};
  static const char *const split_ab[]={"a","b"};
  static const char *const split_blank[]={""};
  if(dvm_exception(v))return false;
  if(!selftest_split_expect(v,"a\nb\nc","\n",0,false,split_three,3,"newline"))return false;
  if(!selftest_split_expect(v,"a\n\nb","\n",0,false,split_empty_middle,3,"consecutive"))return false;
  if(!selftest_split_expect(v,"a\n\n","\n",0,false,split_a,1,"trailing_empty"))return false;
  if(!selftest_split_expect(v,"a,b,c",",",2,true,split_two_bc,2,"limit_positive"))return false;
  if(!selftest_split_expect(v,"a,b,",",",-1,true,split_ab_empty,3,"limit_negative"))return false;
  if(!selftest_split_expect(v,"a,b,",",",0,true,split_ab,2,"limit_zero"))return false;
  if(!selftest_split_expect(v,"a,b,",",",3,true,split_ab_empty,3,"limit_three"))return false;
  if(!selftest_split_expect(v,"a,b,c",",",1,true,split_joined,1,"limit_one"))return false;
  if(!selftest_split_expect(v,"",",",0,false,split_blank,1,"empty_input"))return false;
  if(!selftest_split_expect(v,",",",",0,false,NULL,0,"single_separator"))return false;
  if(!selftest_split_expect(v,"a\nb","\\n",0,false,split_ab,2,"escaped_newline"))return false;
  dvm_ref split_dot=dvm_new_string(v,"."),split_empty=dvm_new_string(v,"");
  if(!split_dot||!split_empty)return false;
  if(!selftest_split_error(v,dvm_new_string(v,"a.b"),split_dot,false,0,"java/lang/UnsupportedOperationException",true,"metachar_dot"))return false;
  if(!selftest_split_error(v,dvm_new_string(v,"a.b"),split_empty,false,0,"java/lang/UnsupportedOperationException",true,"empty_pattern"))return false;
  if(!selftest_split_error(v,dvm_new_string(v,"a.b"),0,false,0,"java/lang/NullPointerException",true,"null_pattern"))return false;
  struct dvm_class *split_object_cls=dvm_find_class_or_external(v,"java/lang/Object");
  dvm_ref split_object=split_object_cls?dvm_new_object(v,split_object_cls):0;
  if(!split_object||!selftest_split_error(v,split_object,split_dot,false,0,NULL,false,"non_string_receiver"))return false;
 }
 sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST POSTSPLIT exception=%d\n",dvm_exception(v)!=0);
 /* String.charAt is reachable through both java/lang/String and the
  * java/lang/CharSequence interface owner.  Only a genuine VM String may use
  * the String implementation; another CharSequence implementation stays
  * unhandled. */
 {
  dvm_ref cs=dvm_new_string(v,"ABC");
  union dvm_value arg={.i=0},co={0};
  if(!cs)return false;
  bool handled=string_runtime_call(v,"java/lang/CharSequence","charAt","(I)C",cs,&arg,1,&co);
  if(!handled||dvm_exception(v)||co.i!='A'){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL charAt_interface handled=%d exception=%d value=%d\n",handled,dvm_exception(v)!=0,co.i);return false;
  }
  arg.i=2;handled=string_runtime_call(v,"java/lang/String","charAt","(I)C",cs,&arg,1,&co);
  if(!handled||dvm_exception(v)||co.i!='C'){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL charAt_string handled=%d exception=%d value=%d\n",handled,dvm_exception(v)!=0,co.i);return false;
  }
  arg.i=3;handled=string_runtime_call(v,"java/lang/String","charAt","(I)C",cs,&arg,1,&co);
  dvm_ref oob=dvm_exception(v);
  struct dvm_class *oob_cls=oob?dvm_object_class(v,oob):NULL;
  if(!handled||!oob_cls||!oob_cls->name||strcmp(oob_cls->name,"java/lang/StringIndexOutOfBoundsException")){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL charAt_oob handled=%d exception=%s\n",handled,oob_cls&&oob_cls->name?oob_cls->name:"none");return false;
  }
  dvm_clear_exception(v);
  struct dvm_class *object_cls=dvm_find_class_or_external(v,"java/lang/Object");
  dvm_ref object=object_cls?dvm_new_object(v,object_cls):0;
  if(!object)return false;
  handled=string_runtime_call(v,"java/lang/CharSequence","charAt","(I)C",object,&arg,1,&co);
  if(handled||dvm_exception(v)){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL charAt_non_string handled=%d exception=%d\n",handled,dvm_exception(v)!=0);return false;
  }
 }
 /* String(char[])/String(char[],int,int): UTF-16 copy semantics, U+0000
  * preservation, surrogate pairs, Java bounds, in-place identity and failure
  * atomicity.  The same initialized receiver is reused so each success also
  * exercises replacing and freeing the previous payload. */
 {
  static const uint16_t ascii_units[]={'h','i'};
  static const char ascii_utf8[]="hi";
  static const uint16_t chinese_units[]={0x4E2D,0x6587};
  static const char chinese_utf8[]="\xE4\xB8\xAD\xE6\x96\x87";
  static const uint16_t pair_units[]={0xD83D,0xDE00};
  static const char pair_utf8[]="\xF0\x9F\x98\x80";
  static const uint16_t nul_units[]={0x0000,'a'};
  static const char nul_utf8[]="\xC0\x80" "a";
  static const uint16_t mixed_units[]={'x','A',0x4E2D,0xD83D,0xDE00,0x0000,'z','Q'};
  static const char mixed_utf8[]="xA\xE4\xB8\xAD\xF0\x9F\x98\x80\xC0\x80zQ";
  static const uint16_t slice_units[]={'A',0x4E2D,0xD83D,0xDE00,0x0000,'z','Q'};
  static const char slice_utf8[]="A\xE4\xB8\xAD\xF0\x9F\x98\x80\xC0\x80zQ";
  if(dvm_exception(v))return false;
  dvm_ref ascii_chars=selftest_char_array(v,ascii_units,2);
  dvm_ref chinese_chars=selftest_char_array(v,chinese_units,2);
  dvm_ref pair_chars=selftest_char_array(v,pair_units,2);
  dvm_ref nul_chars=selftest_char_array(v,nul_units,2);
  dvm_ref mixed_chars=selftest_char_array(v,mixed_units,8);
  if(!ascii_chars||!chinese_chars||!pair_chars||!nul_chars||!mixed_chars)return false;
  dvm_ref char_receiver=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/String"));
  if(!char_receiver)return false;
  if(!selftest_char_init_ok(v,"ascii",char_receiver,ascii_chars,0,0,false,ascii_units,2,ascii_utf8,sizeof ascii_utf8-1))return false;
  if(!selftest_char_init_ok(v,"chinese",char_receiver,chinese_chars,0,0,false,chinese_units,2,chinese_utf8,sizeof chinese_utf8-1))return false;
  if(!selftest_char_init_ok(v,"surrogate_pair",char_receiver,pair_chars,0,0,false,pair_units,2,pair_utf8,sizeof pair_utf8-1))return false;
  if(!selftest_char_init_ok(v,"nul_leading",char_receiver,nul_chars,0,0,false,nul_units,2,nul_utf8,sizeof nul_utf8-1))return false;
  if(!selftest_char_init_ok(v,"mixed_whole",char_receiver,mixed_chars,0,0,false,mixed_units,8,mixed_utf8,sizeof mixed_utf8-1))return false;
  if(!selftest_char_init_ok(v,"offset_count",char_receiver,mixed_chars,1,7,true,slice_units,7,slice_utf8,sizeof slice_utf8-1))return false;
  if(!selftest_char_init_ok(v,"empty_tail",char_receiver,mixed_chars,8,0,true,NULL,0,"",0))return false;
  dvm_ref keep=dvm_new_string(v,"keep");
  if(!keep)return false;
  if(!selftest_char_init_error(v,"npe_whole",keep,0,0,0,false,"java/lang/NullPointerException"))return false;
  if(!selftest_char_init_error(v,"npe_range",keep,0,0,0,true,"java/lang/NullPointerException"))return false;
  if(!selftest_char_init_error(v,"offset_negative",keep,mixed_chars,-1,1,true,"java/lang/StringIndexOutOfBoundsException"))return false;
  if(!selftest_char_init_error(v,"count_negative",keep,mixed_chars,0,-1,true,"java/lang/StringIndexOutOfBoundsException"))return false;
  if(!selftest_char_init_error(v,"range_overflow",keep,mixed_chars,2,7,true,"java/lang/StringIndexOutOfBoundsException"))return false;
  if(!selftest_char_init_error(v,"offset_past_end",keep,mixed_chars,9,0,true,"java/lang/StringIndexOutOfBoundsException"))return false;
  struct dvm_class *char_object_cls=dvm_find_class_or_external(v,"java/lang/Object");
  dvm_ref char_object=char_object_cls?dvm_new_object(v,char_object_cls):0;
  if(!char_object)return false;
  union dvm_value char_non_string_arg={.l=ascii_chars},char_non_string_out={0};
  if(string_runtime_call(v,"java/lang/String","<init>","([C)V",char_object,&char_non_string_arg,1,&char_non_string_out)||dvm_exception(v)){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL char_init_non_string handled_or_threw=1\n");return false;
  }
  if(!selftest_copy_value_of_chars(v,"ascii",ascii_chars,true,NULL,ascii_units,2,ascii_utf8,sizeof ascii_utf8-1))return false;
  if(!selftest_copy_value_of_chars(v,"mixed",mixed_chars,true,NULL,mixed_units,8,mixed_utf8,sizeof mixed_utf8-1))return false;
  if(!selftest_copy_value_of_chars(v,"null",0,true,"java/lang/NullPointerException",NULL,0,NULL,0))return false;
 }
 /* String.replace(CharSequence,CharSequence): literal replacement semantics,
  * empty target insertion, non-overlapping matching, NUL/surrogate code
  * units, regex metacharacters kept literal, Java null errors and fail-closed
  * handling of objects that are not CharSequence under the VM's class
  * metadata. */
 {
  static const char rp_emoji[]="\xF0\x9F\x98\x80";
  static const char rp_pair_source[]="x" "\xF0\x9F\x98\x80" "y" "\xF0\x9F\x98\x80" "z";
  static const char rp_pair_want[]="xEyEz";
  static const char rp_lone[]="\xED\xA0\xBD";
  static const char rp_lone_source[]="\xED\xA0\xBD" "x";
  static const char rp_a_lone[]={'a',(char)0xED,(char)0xA0,(char)0xBD};
  static const char rp_nul[]={(char)0xC0,(char)0x80};
  static const char rp_a_nul_b[]={'a',(char)0xC0,(char)0x80,'b'};
  static const char rp_nul_nul[]={(char)0xC0,(char)0x80,(char)0xC0,(char)0x80};
  if(dvm_exception(v))return false;
  if(!selftest_replace_expect(v,"single","hello world",11,"world",5,"DVM",3,"hello DVM",9))return false;
  if(!selftest_replace_expect(v,"multiple","a-b-c",5,"-",1,":",1,"a:b:c",5))return false;
  if(!selftest_replace_expect(v,"empty_target","abc",3,"",0,"-",1,"-a-b-c-",7))return false;
  if(!selftest_replace_expect(v,"empty_target_empty_source","",0,"",0,"-",1,"-",1))return false;
  if(!selftest_replace_expect(v,"empty_target_empty_replacement","abc",3,"",0,"",0,"abc",3))return false;
  if(!selftest_replace_expect(v,"not_found","abc",3,"z",1,"y",1,"abc",3))return false;
  if(!selftest_replace_expect(v,"nul_match",rp_a_nul_b,sizeof rp_a_nul_b,rp_nul,sizeof rp_nul,"X",1,"aXb",3))return false;
  if(!selftest_replace_expect(v,"nul_replacement","aa",2,"a",1,rp_nul,sizeof rp_nul,rp_nul_nul,sizeof rp_nul_nul))return false;
  if(!selftest_replace_expect(v,"surrogate_pair",rp_pair_source,sizeof rp_pair_source-1,rp_emoji,sizeof rp_emoji-1,"E",1,rp_pair_want,sizeof rp_pair_want-1))return false;
  if(!selftest_replace_expect(v,"lone_surrogate",rp_lone_source,sizeof rp_lone_source-1,rp_lone,sizeof rp_lone-1,"H",1,"Hx",2))return false;
  if(!selftest_replace_expect(v,"lone_surrogate_replacement","ab",2,"b",1,rp_lone,sizeof rp_lone-1,rp_a_lone,sizeof rp_a_lone))return false;
  if(!selftest_replace_expect(v,"non_overlapping","aaa",3,"aa",2,"b",1,"ba",2))return false;
  if(!selftest_replace_expect(v,"regex_metachar_literal","a.b*c",5,".",1,"/",1,"a/b*c",5))return false;
  if(!selftest_replace_expect(v,"replacement_dollar_literal","x",1,"x",1,"$1",2,"$1",2))return false;
  if(!selftest_replace_expect(v,"replacement_backslash_literal","x",1,"x",1,"\\",1,"\\",1))return false;
  dvm_ref rp_source=dvm_new_string(v,"abc"),rp_target=dvm_new_string(v,"b"),rp_replacement=dvm_new_string(v,"X");
  if(!rp_source||!rp_target||!rp_replacement)return false;
  if(!selftest_replace_error(v,"null_target",rp_source,0,rp_replacement,"java/lang/NullPointerException",true))return false;
  if(!selftest_replace_error(v,"null_replacement",rp_source,rp_target,0,"java/lang/NullPointerException",true))return false;
  if(!selftest_replace_error(v,"null_both",rp_source,0,0,"java/lang/NullPointerException",true))return false;
  struct dvm_class *rp_object_cls=dvm_find_class_or_external(v,"java/lang/Object");
  dvm_ref rp_object=rp_object_cls?dvm_new_object(v,rp_object_cls):0;
  if(!rp_object)return false;
  if(!selftest_replace_error(v,"non_charsequence_argument",rp_source,rp_object,rp_replacement,"java/lang/ClassCastException",true))return false;
  if(!selftest_replace_error(v,"non_string_receiver",rp_object,rp_target,rp_replacement,NULL,false))return false;
  /* The VM's CharSequence edge is installed only for String (runtime
   * contract); StringBuilder has no interface edge in this metadata, so the
   * argument stays fail-closed rather than being guessed to be text. */
  struct dvm_class *rp_builder_cls=dvm_find_class_or_external(v,"java/lang/StringBuilder");
  dvm_ref rp_builder=rp_builder_cls?dvm_new_object(v,rp_builder_cls):0;
  if(!rp_builder)return false;
  if(!selftest_replace_error(v,"builder_argument_fail_closed",rp_source,rp_builder,rp_replacement,"java/lang/ClassCastException",true))return false;
  /* A DEX invoke-interface may name CharSequence as the owner while the
   * receiver is a genuine String; the same implementation must be offered. */
  union dvm_value rp_interface_args[2]={{.l=rp_target},{.l=rp_replacement}},rp_interface_out={0};
  bool rp_interface_handled=string_runtime_call(v,"java/lang/CharSequence","replace",
   "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;",rp_source,
   rp_interface_args,2,&rp_interface_out);
  if(!rp_interface_handled||dvm_exception(v)||!rp_interface_out.l||
     strcmp(dvm_string_utf8(v,rp_interface_out.l),"aXc")){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL replace_interface handled=%d exception=%d\n",rp_interface_handled,dvm_exception(v)!=0);return false;
  }
 }
 /* StringBuilder.setLength(int) and charAt(int): Java reports and indexes
  * UTF-16 code units while the builder stores UTF-8.  Cover initial content,
  * truncation, extension with U+0000, a cut between the two halves of a
  * surrogate pair, negative length, empty builder and toString agreement. */
 {
  static const uint16_t sb_initial[]={'A',0x4E2D,0xD83D,0xDE00,'B'};
  static const uint16_t sb_after_truncate[]={'A',0x4E2D};
  static const uint16_t sb_after_extend[]={'A',0x4E2D,0,0,0};
  static const uint16_t sb_after_split[]={'A',0x4E2D,0xD83D};
  static const uint16_t sb_after_split_nul[]={'A',0x4E2D,0xD83D,0};
  static const uint16_t sb_two_nul[]={0,0};
  struct dvm_class *sb_cls=dvm_find_class_or_external(v,"java/lang/StringBuilder");
  dvm_ref sb=sb_cls?dvm_new_object(v,sb_cls):0;
  dvm_ref sb_source=dvm_new_string(v,"A\xE4\xB8\xAD\xF0\x9F\x98\x80" "B");
  if(!sb||!sb_source){sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_allocate\n");return false;}
  union dvm_value sb_out={0},sb_arg={0};
  if(!runtime_extra_call(v,"java/lang/StringBuilder","<init>","()V",sb,NULL,0,&sb_out)||dvm_exception(v)){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_init exception=%d\n",dvm_exception(v)!=0);
   if(dvm_exception(v))dvm_clear_exception(v);return false;
  }
  sb_arg.l=sb_source;
  if(!runtime_extra_call(v,"java/lang/StringBuilder","append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",sb,&sb_arg,1,&sb_out)||dvm_exception(v)){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_append exception=%d\n",dvm_exception(v)!=0);
   if(dvm_exception(v))dvm_clear_exception(v);return false;
  }
  if(!selftest_builder_state(v,sb,"initial",5,sb_initial,5,9))return false;
  if(!selftest_builder_to_string(v,sb,"initial",5,sb_initial,5))return false;
  if(!selftest_builder_set_length(v,sb,2,"truncate"))return false;
  if(!selftest_builder_state(v,sb,"truncate",2,sb_after_truncate,2,4))return false;
  if(!selftest_builder_to_string(v,sb,"truncate",2,sb_after_truncate,2))return false;
  if(!selftest_builder_set_length(v,sb,5,"extend_nul"))return false;
  if(!selftest_builder_state(v,sb,"extend_nul",5,sb_after_extend,5,7))return false;
  if(!selftest_builder_to_string(v,sb,"extend_nul",5,sb_after_extend,5))return false;
  /* Reinitialize and cut between the two halves of U+1F600. */
  if(!runtime_extra_call(v,"java/lang/StringBuilder","<init>","()V",sb,NULL,0,&sb_out)||dvm_exception(v)){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_reinit exception=%d\n",dvm_exception(v)!=0);
   if(dvm_exception(v))dvm_clear_exception(v);return false;
  }
  sb_arg.l=sb_source;
  if(!runtime_extra_call(v,"java/lang/StringBuilder","append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",sb,&sb_arg,1,&sb_out)||dvm_exception(v)){
   sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST_FAIL builder_reappend exception=%d\n",dvm_exception(v)!=0);
   if(dvm_exception(v))dvm_clear_exception(v);return false;
  }
  if(!selftest_builder_set_length(v,sb,3,"split_pair"))return false;
  if(!selftest_builder_state(v,sb,"split_pair",3,sb_after_split,3,7))return false;
  if(!selftest_builder_to_string(v,sb,"split_pair",3,sb_after_split,3))return false;
  if(!selftest_builder_set_length(v,sb,4,"split_pair_nul"))return false;
  if(!selftest_builder_state(v,sb,"split_pair_nul",4,sb_after_split_nul,4,8))return false;
  if(!selftest_builder_to_string(v,sb,"split_pair_nul",4,sb_after_split_nul,4))return false;
  if(!selftest_builder_error(v,sb,-1,"negative"))return false;
  if(!selftest_builder_state(v,sb,"negative_unchanged",4,sb_after_split_nul,4,8))return false;
  if(!selftest_builder_set_length(v,sb,0,"empty"))return false;
  if(!selftest_builder_state(v,sb,"empty",0,NULL,0,0))return false;
  if(!selftest_builder_to_string(v,sb,"empty",0,NULL,0))return false;
  if(!selftest_builder_set_length(v,sb,2,"extend_empty"))return false;
  if(!selftest_builder_state(v,sb,"extend_empty",2,sb_two_nul,2,2))return false;
  if(!selftest_builder_to_string(v,sb,"extend_empty",2,sb_two_nul,2))return false;
 }
 sdk_trace_fprintf(stderr,"STRING_RUNTIME_SELFTEST PASS charAt=1 charAt_interface=1 UTF16_nonBMP_NUL=1 prefix_offset=1 hash=1 trim=1 substring=1 index_of=1 last_index_of=1 code_point_at=1 region_matches=1 format=1 format_double_dispatch=1 format_double_large=1 format_double_negative=1 format_double_shortest_digits=1 format_double_round_half_up=1 format_double_nan_inf=1 format_double_errors=1 format_output_limit=1 toLowerCase_locale=1 toLowerCase_new_string=1 toLowerCase_npe=1 toLowerCase_signature=1 toLowerCase_unicode_boundary=1 toLowerCase_default=1 toLowerCase_default_new_string=1 toLowerCase_default_empty=1 toLowerCase_default_unicode_boundary=1 toLowerCase_default_signature=1 toLowerCase_default_non_string=1 toUpperCase_locale=1 toUpperCase_root=1 toUpperCase_english=1 toUpperCase_new_string=1 toUpperCase_non_ascii=1 toUpperCase_empty=1 toUpperCase_nul=1 toUpperCase_npe=1 toUpperCase_unknown_locale=1 toUpperCase_locale_type=1 toUpperCase_default=1 toUpperCase_signature=1 toUpperCase_non_string=1 equals_ignore_case=1 equals_ignore_case_null=1 equals_ignore_case_unicode_boundary=1 equals_ignore_case_non_string=1 string_init_charset=1 string_init_2arg=1 string_init_offset=1 string_init_unicode=1 string_init_nul_nonbmp=1 string_init_aliases=1 string_init_errors=1 string_init_malformed_fffd=1 get_bytes=1 get_bytes_default=1 get_bytes_utf8=1 get_bytes_utf16=1 get_bytes_utf16le_be=1 get_bytes_surrogate_replacement=1 get_bytes_nul=1 get_bytes_empty=1 get_bytes_alias=1 get_bytes_charset_object=1 get_bytes_errors=1 get_bytes_signature=1 split=1 split_limit=1 split_escape=1 split_errors=1 strict_unknown=1 char_array_ctor=1 char_array_copy_value_of=1 replace_charsequence=1 string_builder_setLength_charAt=1\n");
 return !dvm_exception(v);
}
