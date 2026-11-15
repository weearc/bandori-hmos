/* Strict bounded signed parsing. Cutoff arithmetic follows pinned Lunaria's
 * jp_parse_long; unlike its i_parseInt/l_parseLong adapters, reject trailing
 * input, whitespace and overflow. Character.digit semantics cover BMP decimal
 * digit blocks and fullwidth Latin letters; supplementary UTF16 surrogates are
 * not digits in the char-by-char Integer/Long.parse APIs. */
#include "numeric_runtime.h"
#include "portable.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static int digit(uint32_t cp){
 if(cp>='0'&&cp<='9')return (int)(cp-'0');if(cp>='a'&&cp<='z')return (int)(cp-'a'+10);if(cp>='A'&&cp<='Z')return (int)(cp-'A'+10);
 if(cp>=0xff21&&cp<=0xff3a)return (int)(cp-0xff21+10);if(cp>=0xff41&&cp<=0xff5a)return (int)(cp-0xff41+10);
 static const uint32_t starts[]={0x660,0x6f0,0x7c0,0x966,0x9e6,0xa66,0xae6,0xb66,0xbe6,0xc66,0xce6,0xd66,0xde6,0xe50,0xed0,0xf20,0x1040,0x1090,0x17e0,0x1810,0x1946,0x19d0,0x1a80,0x1a90,0x1b50,0x1bb0,0x1c40,0x1c50,0xa620,0xa8d0,0xa900,0xa9d0,0xa9f0,0xaa50,0xabf0,0xff10,0x104a0,0x10d30,0x11066,0x110f0,0x11136,0x111d0,0x112f0,0x11450,0x114d0,0x11650,0x116c0,0x11730,0x118e0,0x11950,0x11c50,0x11d50,0x11da0,0x11f50,0x16a60,0x16ac0,0x16b50,0x1e140,0x1e2f0,0x1e4f0,0x1e5f1,0x1e950,0x1fbf0};
 for(size_t i=0;i<sizeof starts/sizeof starts[0];++i)if(cp>=starts[i]&&cp<starts[i]+10u)return (int)(cp-starts[i]);
 return -1;
}
static bool next(const unsigned char *s,size_t n,size_t *i,uint32_t *cp){
 if(*i>=n)return false;uint32_t c=s[(*i)++],minimum=0;unsigned tail=0;if(c<128){}else if(c>=0xc2&&c<=0xdf){c&=31;tail=1;minimum=0x80;}else if(c>=0xe0&&c<=0xef){c&=15;tail=2;minimum=0x800;}else if(c>=0xf0&&c<=0xf4){c&=7;tail=3;minimum=0x10000;}else return false;
 if(tail>n-*i)return false;for(unsigned j=0;j<tail;++j){unsigned b=s[(*i)++];if((b&0xc0)!=0x80)return false;c=c<<6|(b&63);}if((tail&&c<minimum)||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;*cp=c;return true;
}
static bool parse(struct dvm *v,dvm_ref text,int radix,bool wide,int64_t *out){
 const unsigned char *s=(const unsigned char *)dvm_string_utf8(v,text);size_t n=dvm_string_utf8_length(v,text),i=0;
 if(!s||!n||radix<2||radix>36)goto invalid;bool negative=false;if(s[0]=='-'||s[0]=='+'){negative=s[0]=='-';if(++i==n)goto invalid;}
 uint64_t maximum=wide?(uint64_t)INT64_MAX:(uint64_t)INT32_MAX;if(negative)++maximum;uint64_t value=0;
 while(i<n){uint32_t cp;if(!next(s,n,&i,&cp))goto invalid;int d=digit(cp);if(d<0||d>=radix||value>(maximum-(unsigned)d)/(unsigned)radix)goto invalid;value=value*(unsigned)radix+(unsigned)d;}
 *out=negative?(value==(1ULL<<63)?INT64_MIN:-(int64_t)value):(int64_t)value;return true;
invalid:dvm__throw(v,"java/lang/NumberFormatException","invalid signed number, radix or range");return false;
}
bool numeric_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)self;
 bool integer=!strcmp(c,"java/lang/Integer")&&!strcmp(m,"parseInt")&&(!strcmp(sig,"(Ljava/lang/String;)I")||!strcmp(sig,"(Ljava/lang/String;I)I"));
 bool wide=!strcmp(c,"java/lang/Long")&&!strcmp(m,"parseLong")&&(!strcmp(sig,"(Ljava/lang/String;)J")||!strcmp(sig,"(Ljava/lang/String;I)J"));
 if(integer||wide){bool with_radix=strstr(sig,";I)")!=NULL;if(nargs!=(with_radix?2:1))return false;int64_t value=0;int radix=with_radix?a[1].i:10;if(parse(v,a[0].l,radix,wide,&value)){if(wide)out->j=value;else out->i=(int32_t)value;}return true;}
 if(!strcmp(c,"java/lang/Boolean")&&!strcmp(m,"parseBoolean")&&!strcmp(sig,"(Ljava/lang/String;)Z")){
  if(nargs!=1)return false;
  const char *s=dvm_string_utf8(v,a[0].l);size_t n=dvm_string_utf8_length(v,a[0].l);out->i=s&&n==4&&(s[0]=='t'||s[0]=='T')&&(s[1]=='r'||s[1]=='R')&&(s[2]=='u'||s[2]=='U')&&(s[3]=='e'||s[3]=='E');return true;
 }
 return false;
}
