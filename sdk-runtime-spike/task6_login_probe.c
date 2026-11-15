/* Explicit TASK6 diagnostic. Calls original DEX through Java reflection.
 * The observer only records calls/returns; it never supplies SDK results. */
#include "dvm/dvm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
extern bool activity_runtime_probe_context(struct dvm *,const char *,dvm_ref *,dvm_ref *);
extern bool reflection_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
extern bool collections_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static int active;
static _Thread_local int observing;
static char secret[16];
static void log6(const char *fmt,...){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 fputs("TASK6 ",f);va_list ap;va_start(ap,fmt);vfprintf(f,fmt,ap);va_end(ap);fputc('\n',f);fclose(f);
}
static const char *safe(const char *src,char *out,size_t capacity){
 if(!src)src="<null>";size_t n=0,secret_len=strlen(secret);
 while(*src&&n+1<capacity){if(secret_len&&!strncmp(src,secret,secret_len)){const char *mask="<pwd-redacted>";while(*mask&&n+1<capacity)out[n++]=*mask++;src+=secret_len;}else{char ch=*src++;out[n++]=(ch=='\n'||ch=='\r')?' ':ch;}}
 out[n]=0;return out;
}
static bool call(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *args,int count,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);struct dvm_method *m=o?dvm_find_method(v,o->cls,name,sig):NULL;
 return m&&dvm_call(v,m,self,args,count,out)&&!dvm_exception(v);
}
static const unsigned char rsa_test_der[]={0x30,0x81,0x9f,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x05,0x00,0x03,0x81,0x8d,0x00,0x30,0x81,0x89,0x02,0x81,0x81,0x00,0xf7,0x4c,0x10,0xda,0x06,0x28,0x3c,0xa2,0x10,0x09,0xb6,0x88,0x43,0x55,0xae,0x4d,0x13,0xcd,0x85,0xd6,0x0c,0x40,0x32,0xc9,0x60,0x7b,0xfd,0xfa,0x43,0x0b,0xec,0xf4,0x93,0xeb,0xb6,0xf3,0x4f,0xb9,0x10,0x8e,0x35,0x6c,0xf1,0xd8,0xe3,0x88,0x83,0xf5,0xaf,0xc6,0x9b,0xa0,0x5d,0xcc,0x13,0xf6,0xde,0x5f,0x58,0xca,0x42,0x83,0x45,0x2a,0x73,0x47,0xc9,0xdb,0x19,0xee,0xfa,0x85,0xc0,0xd2,0x4f,0xf3,0xf6,0x92,0xb3,0xc0,0xdb,0xa6,0xd8,0x11,0x7b,0xe8,0xcf,0x9d,0x43,0xd3,0x53,0x45,0x53,0x41,0xad,0x1d,0x4a,0x54,0x77,0x46,0xe3,0x1f,0x6d,0x56,0x2b,0x0b,0xe0,0x38,0x33,0x4f,0x79,0xf9,0xb9,0x0e,0xda,0x3a,0xb8,0xda,0x65,0x49,0x00,0x38,0x89,0x6f,0xf2,0xa2,0x02,0x21,0x02,0x03,0x01,0x00,0x01};
static bool rsa_invoke(struct dvm *v,const char *owner,dvm_ref self,const char *name,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm_find_method(v,dvm_find_class_or_external(v,owner),name,sig);
 return m&&dvm_call(v,m,self,a,n,out)&&!dvm_exception(v);
}
static const unsigned char rsa_test_signature[]={0x05,0x76,0x36,0x1a,0xf4,0x5b,0x29,0x7e,0x09,0x46,0x4d,0x24,0x04,0x8f,0x29,0x05,0xdd,0x96,0x84,0x30,0x22,0xa9,0x83,0x04,0x3a,0x3b,0x03,0x2e,0xe4,0xfa,0x83,0x46,0x12,0xe0,0x01,0x23,0x2a,0xeb,0xe3,0x34,0x83,0x4a,0x4b,0x32,0xa7,0x82,0x5f,0x3e,0xc3,0xab,0x2a,0x1c,0xb1,0xdf,0x02,0x03,0x4e,0x01,0xe5,0xc0,0x4a,0x43,0x5a,0x28,0x91,0x3d,0x05,0x6a,0xac,0xb9,0x68,0xd3,0x21,0x92,0x18,0xac,0x57,0x8b,0x6f,0x6d,0xc3,0xdc,0x19,0xcc,0x48,0x76,0xae,0xf3,0x85,0x47,0x8c,0x5a,0x87,0x67,0x01,0xee,0x44,0x9a,0x9e,0x44,0x86,0x96,0x64,0x21,0xe9,0xa8,0xe2,0x64,0xe2,0xec,0x0c,0xc8,0xce,0x36,0x66,0xf7,0xef,0xd3,0xe8,0x7f,0xcf,0x69,0x4b,0x8c,0xd2,0x9b,0x86,0x35};
#define RSA_COVER(expr) do{if(!(expr)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_COVERAGE_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}}while(0)
static bool task6_rsa_coverage(struct dvm *v,dvm_ref key){
 const char *classes[]={"java.security.spec.EncodedKeySpec","java.security.spec.X509EncodedKeySpec","java.security.spec.KeySpec","java.security.spec.InvalidKeySpecException","java.security.KeyFactory","java.security.PublicKey","java.security.interfaces.RSAPublicKey","java.security.Signature","java.security.SignatureException","javax.crypto.Cipher"};
 union dvm_value a[3]={{0}},q={0};
 for(unsigned i=0;i<sizeof classes/sizeof classes[0];i++){
  RSA_COVER(dvm_class_exists(v,classes[i]));a[0].l=dvm_new_string(v,classes[i]);a[1].i=0;a[2].l=0;
  RSA_COVER(rsa_invoke(v,"java/lang/Class",0,"forName","(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",a,3,&q)&&q.l);
 }
 a[0].l=dvm_new_string(v,"java.security.spec.Task6MissingSpec");
 RSA_COVER(!rsa_invoke(v,"java/lang/Class",0,"forName","(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",a,3,&q)&&dvm_exception(v));
 RSA_COVER(!strcmp(dvm_object_class(v,dvm_exception(v))->name,"java/lang/ClassNotFoundException"));dvm_clear_exception(v);
 a[0].l=dvm_new_string(v,"NO_SUCH_ALGORITHM");
 RSA_COVER(!rsa_invoke(v,"javax/crypto/Cipher",0,"getInstance","(Ljava/lang/String;)Ljavax/crypto/Cipher;",a,1,&q)&&dvm_exception(v));
 RSA_COVER(!strcmp(dvm_object_class(v,dvm_exception(v))->name,"java/security/NoSuchAlgorithmException"));dvm_clear_exception(v);
 a[0].l=dvm_new_string(v,"UTF-8");RSA_COVER(rsa_invoke(v,"java/nio/charset/Charset",0,"forName","(Ljava/lang/String;)Ljava/nio/charset/Charset;",a,1,&q));
 dvm_ref charset=q.l,source=dvm_new_string(v,"A\xc0\x80\xe4\xb8\xad\xf0\x9f\x98\x80");a[0].l=charset;
 const unsigned char expected[]={'A',0,0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
 RSA_COVER(rsa_invoke(v,"java/lang/String",source,"getBytes","(Ljava/nio/charset/Charset;)[B",a,1,&q)&&dvm_array_length(v,q.l)==sizeof expected&&!memcmp(dvm_array_data(v,q.l),expected,sizeof expected));
 const unsigned char b64bytes[]={0,1,255,128,65};a[0].l=dvm_new_array(v,'B',"B",sizeof b64bytes);memcpy(dvm_array_data(v,a[0].l),b64bytes,sizeof b64bytes);
 RSA_COVER(rsa_invoke(v,"com/gsc/base/utils/Base64",0,"encode","([B)Ljava/lang/String;",a,1,&q)&&!strcmp(dvm_string_utf8(v,q.l),"AAH/gEE="));
 a[0].l=q.l;RSA_COVER(rsa_invoke(v,"com/gsc/base/utils/Base64",0,"decode","(Ljava/lang/String;)[B",a,1,&q)&&dvm_array_length(v,q.l)==sizeof b64bytes&&!memcmp(dvm_array_data(v,q.l),b64bytes,sizeof b64bytes));
 const char *signature="java/security/Signature";a[0].l=dvm_new_string(v,"SHA1WithRSA");
 RSA_COVER(rsa_invoke(v,signature,0,"getInstance","(Ljava/lang/String;)Ljava/security/Signature;",a,1,&q));dvm_ref verifier=q.l;
 dvm_ref message=dvm_new_array(v,'B',"B",14);memcpy(dvm_array_data(v,message),"rsa-oracle-test",14);a[0].l=message;
 RSA_COVER(!rsa_invoke(v,signature,verifier,"update","([B)V",a,1,&q)&&dvm_exception(v));dvm_clear_exception(v);
 a[0].l=key;RSA_COVER(rsa_invoke(v,signature,verifier,"initVerify","(Ljava/security/PublicKey;)V",a,1,&q));
 dvm_ref signed_bytes=dvm_new_array(v,'B',"B",sizeof rsa_test_signature);memcpy(dvm_array_data(v,signed_bytes),rsa_test_signature,sizeof rsa_test_signature);
 for(int i=0;i<3;i++){
  a[0].l=message;RSA_COVER(rsa_invoke(v,signature,verifier,"update","([B)V",a,1,&q));
  unsigned char *bytes=dvm_array_data(v,signed_bytes);if(i==1)bytes[17]^=1;if(i==2)bytes[17]^=1;
  a[0].l=signed_bytes;RSA_COVER(rsa_invoke(v,signature,verifier,"verify","([B)Z",a,1,&q)&&q.i==(i!=1));
 }
 extern int sdk_trace_fprintf(FILE *,const char *,...);
 sdk_trace_fprintf(stderr,"TASK6_RSA_COVERAGE PASS Class_forName=10 missing_class=ClassNotFoundException missing_algorithm=NoSuchAlgorithmException Charset_UTF8_NUL_CJK_surrogate=1 original_SDK_Base64_roundtrip=1 SHA1WithRSA_JDK_positive=2 tampered_false=1 verify_resets=1 update_before_init_rejected=1 login_probe_called=0\n");
 return true;
}
static bool task6_rsa_selftest(struct dvm *v){
 const char *spec="java/security/spec/X509EncodedKeySpec",*factory="java/security/KeyFactory",*cipher="javax/crypto/Cipher";
 dvm_ref array=dvm_new_array(v,'B',"B",sizeof rsa_test_der);memcpy(dvm_array_data(v,array),rsa_test_der,sizeof rsa_test_der);
 dvm_ref x=dvm_new_object(v,dvm_find_class_or_external(v,spec));union dvm_value a[2]={{.l=array},{.l=dvm_new_string(v,"RSA")}},q={0};
 if(!rsa_invoke(v,spec,x,"<init>","([B)V",a,1,&q)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 ((unsigned char*)dvm_array_data(v,array))[0]^=0xff;
 if(!rsa_invoke(v,spec,x,"getEncoded","()[B",NULL,0,&q)||dvm_array_length(v,q.l)!=sizeof rsa_test_der||memcmp(dvm_array_data(v,q.l),rsa_test_der,sizeof rsa_test_der)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 ((unsigned char*)dvm_array_data(v,q.l))[0]=0;
 if(!rsa_invoke(v,spec,x,"getEncoded","()[B",NULL,0,&q)||memcmp(dvm_array_data(v,q.l),rsa_test_der,sizeof rsa_test_der)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 if(!rsa_invoke(v,spec,x,"getFormat","()Ljava/lang/String;",NULL,0,&q)||strcmp(dvm_string_utf8(v,q.l),"X.509")){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 dvm_ref x2=dvm_new_object(v,dvm_find_class_or_external(v,spec));a[0].l=dvm_new_array(v,'B',"B",0);
 if(!rsa_invoke(v,spec,x2,"<init>","([BLjava/lang/String;)V",a,2,&q)||!rsa_invoke(v,spec,x2,"getEncoded","()[B",NULL,0,&q)||!q.l||dvm_array_length(v,q.l)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 a[0].l=dvm_new_string(v,"RSA");if(!rsa_invoke(v,factory,0,"getInstance","(Ljava/lang/String;)Ljava/security/KeyFactory;",a,1,&q)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 dvm_ref f=q.l;a[0].l=x2;
 if(rsa_invoke(v,factory,f,"generatePublic","(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;",a,1,&q)||!dvm_exception(v)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 dvm_clear_exception(v);a[0].l=x;
 if(!rsa_invoke(v,factory,f,"generatePublic","(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;",a,1,&q)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 dvm_ref key=q.l;if(!task6_rsa_coverage(v,key))return false;a[0].l=dvm_new_string(v,"RSA/ECB/PKCS1Padding");
 if(!rsa_invoke(v,cipher,0,"getInstance","(Ljava/lang/String;)Ljavax/crypto/Cipher;",a,1,&q)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 dvm_ref enc=q.l;a[0].i=1;a[1].l=key;
 if(!rsa_invoke(v,cipher,enc,"init","(ILjava/security/Key;)V",a,2,&q)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 a[0].l=dvm_new_array(v,'B',"B",14);memcpy(dvm_array_data(v,a[0].l),"rsa-oracle-test",14);
 if(!rsa_invoke(v,cipher,enc,"doFinal","([B)[B",a,1,&q)||dvm_array_length(v,q.l)!=128){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 unsigned char first[128];memcpy(first,dvm_array_data(v,q.l),128);
 if(!rsa_invoke(v,cipher,enc,"doFinal","([B)[B",a,1,&q)||!memcmp(first,dvm_array_data(v,q.l),128)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}
 a[0].l=dvm_new_array(v,'B',"B",118);
 if(rsa_invoke(v,cipher,enc,"doFinal","([B)[B",a,1,&q)||!dvm_exception(v)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST_FAIL line=%d exception=%u\n",__LINE__,dvm_exception(v));return false;}dvm_clear_exception(v);
 char hex[257];for(int i=0;i<128;i++)sprintf(hex+2*i,"%02x",first[i]);
 extern int sdk_trace_fprintf(FILE *,const char *,...);
 sdk_trace_fprintf(stderr,"TASK6_RSA_SELFTEST PASS constructors=2 copies=2 empty=1 format=1 invalid_der=1 rsa_encrypt=1 randomized=1 oversize_rejected=1 ciphertext_hex=%s\n",hex);
 return true;
}
bool task6_hotpath_selftest(struct dvm *v){
 if(!task6_rsa_selftest(v))return false;
 dvm_ref source=dvm_new_string(v,"A\xc0\x80\xe4\xb8\xad\xf0\x9f\x98\x80");union dvm_value chars={0},again={0};
 const uint16_t expected_units[]={'A',0,0x4e2d,0xd83d,0xde00};
 if(!call(v,source,"toCharArray","()[C",NULL,0,&chars)||dvm_array_length(v,chars.l)!=5||memcmp(dvm_array_data(v,chars.l),expected_units,sizeof expected_units))return false;
 ((uint16_t*)dvm_array_data(v,chars.l))[0]='Z';
 if(!call(v,source,"toCharArray","()[C",NULL,0,&again)||again.l==chars.l||memcmp(dvm_array_data(v,again.l),expected_units,sizeof expected_units))return false;
 dvm_ref empty=dvm_new_string(v,"");if(!call(v,empty,"toCharArray","()[C",NULL,0,&again)||!again.l||dvm_array_length(v,again.l))return false;
 const char *inputs[]={"ascii text","A\xc0\x80\xe4\xb8\xad\xf0\x9f\x98\x80","\xed\xa0\x80"};
 const unsigned char expected1[]={'A',0,0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
 for(int i=0;i<4;i++){
  char *large=NULL;const char *input=i<3?inputs[i]:NULL;
  if(i==3){large=malloc(9001);if(!large)return false;memset(large,'x',9000);large[9000]=0;input=large;}
  dvm_ref buffer=dvm_new_object(v,dvm_find_class(v,"okio/Buffer"));union dvm_value q={0};
  bool ok=call(v,buffer,"<init>","()V",NULL,0,&q);dvm_ref str=dvm_new_string(v,input);free(large);
  int length=i==0?10:i==1?5:i==2?1:9000;
  union dvm_value args[3]={{.l=str},{.i=0},{.i=length}};
  ok=ok&&call(v,buffer,"writeUtf8","(Ljava/lang/String;II)Lokio/Buffer;",args,3,&q)&&q.l==buffer;
  ok=ok&&call(v,buffer,"readByteArray","()[B",NULL,0,&q);
  unsigned count=i==0?10:i==1?sizeof expected1:i==2?1:9000;
  if(!ok||dvm_array_length(v,q.l)!=count)return false;
  unsigned char *bytes=dvm_array_data(v,q.l);
  if(i==0&&memcmp(bytes,inputs[0],10))return false;
  if(i==1&&memcmp(bytes,expected1,sizeof expected1))return false;
  if(i==2&&bytes[0]!='?')return false;
  if(i==3)for(unsigned j=0;j<count;j++)if(bytes[j]!='x')return false;
  args[1].i=-1;if(!call(v,buffer,"writeUtf8","(Ljava/lang/String;II)Lokio/Buffer;",args,3,&q)&&!dvm_exception(v))return false;
  if(!dvm_exception(v))return false;dvm_clear_exception(v);
 }
 struct dvm_class *boolean=dvm_find_class_or_external(v,"java/lang/Boolean");
 struct dvm_method *format=dvm_find_method(v,boolean,"toString","(Z)Ljava/lang/String;");
 for(int i=0;i<2;i++){union dvm_value arg={.i=i},q={0};if(!format||!dvm_call(v,format,0,&arg,1,&q)||dvm_exception(v)||strcmp(dvm_string_utf8(v,q.l),i?"true":"false"))return false;}
 dvm_ref byte_buffer=dvm_new_object(v,dvm_find_class(v,"okio/Buffer"));union dvm_value q={0};
 if(!call(v,byte_buffer,"<init>","()V",NULL,0,&q))return false;
 for(unsigned i=0;i<9000;i++){union dvm_value arg={.i=(int32_t)(i+256)};if(!call(v,byte_buffer,"writeByte","(I)Lokio/Buffer;",&arg,1,&q)||q.l!=byte_buffer)return false;}
 if(!call(v,byte_buffer,"readByteArray","()[B",NULL,0,&q)||dvm_array_length(v,q.l)!=9000)return false;
 unsigned char *actual=dvm_array_data(v,q.l);for(unsigned i=0;i<9000;i++)if(actual[i]!=(unsigned char)i)return false;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){fputs("TASK6_HOTPATH_SELFTEST PASS utf8_ascii=1 nul=1 cjk=1 surrogate_pair=1 unpaired_replacement=1 segment_boundary_9000=1 range_errors=4 boolean_static=2\n",f);fclose(f);}
 extern int sdk_trace_fprintf(FILE *,const char *,...);
 sdk_trace_fprintf(stderr,"TASK6_HOTPATH_SELFTEST PASS utf8_cases=4 range_errors=4 boolean_static=2 writeByte_9000=1 low_byte_truncation=1 toCharArray_utf16_nul_surrogate_copy_empty=1\n");
 return true;
}
static void map_log(struct dvm *v,dvm_ref map){
 union dvm_value keys={0},it={0},q={0};
 if(!collections_call(v,"java/util/Map","keySet","()Ljava/util/Set;",map,NULL,0,&keys)||dvm_exception(v)||!keys.l){log6("stage=map map=%u keys_unavailable=1",map);return;}
 if(!collections_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",keys.l,NULL,0,&it)&&!call(v,keys.l,"iterator","()Ljava/util/Iterator;",NULL,0,&it))return;
 for(unsigned i=0;i<64;i++){
  if((!collections_call(v,"java/util/Iterator","hasNext","()Z",it.l,NULL,0,&q)&&!call(v,it.l,"hasNext","()Z",NULL,0,&q))||!q.i)break;
  if(!collections_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",it.l,NULL,0,&q)&&!call(v,it.l,"next","()Ljava/lang/Object;",NULL,0,&q))break;
  union dvm_value key={.l=q.l},value={0};
  if(!collections_call(v,"java/util/Map","get","(Ljava/lang/Object;)Ljava/lang/Object;",map,&key,1,&value)||dvm_exception(v))break;
  char k[128],val[320];safe(dvm_string_utf8(v,key.l),k,sizeof k);
  const char *text=dvm_string_utf8(v,value.l);
  if(strstr(k,"pwd")||strstr(k,"password"))log6("stage=map key=%s value_bytes=%zu prefix=%.4s redacted=1",k,text?strlen(text):0,text?text:"");
  else log6("stage=map key=%s value=%s",k,safe(text,val,sizeof val));
 }
}
void task6_trace_event(struct dvm *v,struct dvm_method *m,dvm_ref self,const uint32_t *a,int n,const union dvm_value *out,int entering){
 if(!active||observing||!m||!m->cls)return;
 const char *c=m->cls->name,*name=m->name,*sig=m->sig;bool selected=false;
 if(!strcmp(c,"com/gsc/account_login/mvp/a")||!strncmp(c,"com/gsc/account_login/mvp/a$",28)||!strcmp(c,"com/gsc/account_login/mvp/AccountLoginModel")||!strcmp(c,"com/gsc/security/SecurityInterface")||!strncmp(c,"com/gsc/security/interfaces/",28)||!strcmp(c,"com/gsc/base/utils/Rsa"))selected=true;
 if(strstr(c,"Cipher")||!strcmp(c,"java/security/KeyFactory")||!strcmp(c,"java/security/spec/X509EncodedKeySpec"))selected=true;
 if(!strcmp(name,"execute")&&strstr(c,"Model"))selected=true;
 bool request=!strcmp(c,"okhttp3/Request$Builder")&&!strcmp(name,"build");
 bool response=!strcmp(c,"okhttp3/Response$Builder")&&!strcmp(name,"build");
 bool body=!strcmp(c,"okhttp3/ResponseBody")&&!strcmp(name,"string");
 if(request||response||body)selected=true;
 if(!selected)return;
 observing=1;dvm_ref saved=v->exception;bool parked=v->parked;
 char err[512]={0};if(saved)dvm_describe_exception(v,saved,err,sizeof err);
 log6("stage=method phase=%s class=%s method=%s sig=%s self=%u thread=%u exception=%s",entering==1?"enter":entering==0?"exit":"fail",c,name,sig,self,v->cur_thread,err);
 if(entering==0&&out&&(request||response)){
  union dvm_value req={.l=out->l},code={0},url={0},text={0},verb={0},payload={0},length={0},headers={0};
  if(response){dvm_get_field(v,out->l,"request","Lokhttp3/Request;",&req);dvm_get_field(v,out->l,"code","I",&code);}
  dvm_get_field(v,req.l,"url","Lokhttp3/HttpUrl;",&url);if(url.l)dvm_get_field(v,url.l,"url","Ljava/lang/String;",&text);
  dvm_get_field(v,req.l,"method","Ljava/lang/String;",&verb);dvm_get_field(v,req.l,"body","Lokhttp3/RequestBody;",&payload);
  dvm_get_field(v,req.l,"headers","Lokhttp3/Headers;",&headers);
  if(payload.l)call(v,payload.l,"contentLength","()J",NULL,0,&length);
  char address[700];log6("stage=http_%s url=%s method=%s status=%d body_bytes=%lld headers_ref=%u thread=%u",request?"request":"response",safe(dvm_string_utf8(v,text.l),address,sizeof address),dvm_string_utf8(v,verb.l),code.i,(long long)length.j,headers.l,v->cur_thread);
 }
 if(entering==0&&out&&body){char snippet[650];log6("stage=http_body bytes=%zu text=%s thread=%u",(size_t)dvm_string_utf8_length(v,out->l),safe(dvm_string_utf8(v,out->l),snippet,sizeof snippet),v->cur_thread);}
 if(entering==0&&out&&!strcmp(name,"doFinal")&&strstr(c,"Cipher"))log6("stage=rsa_output bytes=%u exception=%u",out->l?dvm_array_length(v,out->l):0,dvm_exception(v));
 if(entering==1&&!strcmp(c,"com/gsc/account_login/mvp/a")&&!strcmp(name,"a")&&n==6&&strstr(sig,"ISecurityService"))log6("stage=entry user=test0000 pwd_length=8 pwd_prefix=%.4s security=%u cipher=%u retry=%u activity=%u",secret,a[2],a[3],a[4],a[5]);
 if(entering==0&&out&&!strcmp(c,"com/gsc/account_login/mvp/a")&&!strcmp(name,"b")&&!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Ljava/util/Map;"))map_log(v,out->l);
 if(entering==1&&n>=1&&strstr(sig,"(Ljava/util/Map;")&&!strcmp(name,"execute"))map_log(v,a[0]);
 if(entering==0&&out&&strstr(sig,")Ljava/lang/String;")){
  char buf[480];const char *str=dvm_string_utf8(v,out->l);log6("stage=string_return class=%s method=%s bytes=%zu text=%s",c,name,str?strlen(str):0,safe(str,buf,sizeof buf));
 }
 if(entering==1&&n&&strstr(c,"mvp/a$")&&!strcmp(name,"a")&&!strcmp(sig,"(Ljava/lang/String;)V")){char buf[700];log6("stage=callback raw=%s",safe(dvm_string_utf8(v,a[0]),buf,sizeof buf));}
 if(dvm_exception(v)!=saved){log6("stage=observer diagnostic_exception=%u",dvm_exception(v));v->exception=saved;v->parked=parked;}
 observing=0;
}
bool task6_login_probe(struct dvm *v){
 /* Test secret assembled only in process memory; never emit the full value. */
 for(int i=0;i<3;i++)secret[i]=(char)('a'+i);for(int i=0;i<5;i++)secret[3+i]=(char)('1'+i);secret[8]=0;
 dvm_ref activity=0,presenter=0;
 if(!activity_runtime_probe_context(v,"com/gsc/account_login/AccountLoginActivity",&activity,&presenter)){log6("stage=context ok=0");return false;}
 log6("stage=context activity=%u presenter=%u original_binding=1",activity,presenter);
 struct dvm_class *sc=dvm_find_class(v,"com/gsc/security/SecurityInterface");
 if(!sc){log6("stage=security found=0");return false;}
 dvm_ref security=dvm_new_object(v,sc);union dvm_value out={0},context={.l=activity};active=1;
 bool ok=security&&call(v,security,"<init>","()V",NULL,0,&out)&&call(v,security,"init","(Landroid/content/Context;)V",&context,1,&out);
 log6("stage=security ok=%d ref=%u exception=%u",ok,security,dvm_exception(v));if(!ok)return false;
 const char *types[]={"java/lang/String","java/lang/String","com/gsc/base/service/ISecurityService","com/gsc/base/model/CipherBaseResModel","I","android/app/Activity"};
 dvm_ref params=dvm_new_array(v,'L',"Ljava/lang/Class;",6);dvm_ref *ps=dvm_array_data(v,params);
 for(int i=0;i<6;i++)ps[i]=dvm_class_object(v,dvm_find_class_or_external(v,types[i]));
 union dvm_value lookup[2]={{.l=dvm_new_string(v,"a")},{.l=params}};
 struct dvm_object *po=dvm__obj(v,presenter);
 ok=reflection_runtime_call(v,"java/lang/Class","getDeclaredMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",dvm_class_object(v,po->cls),lookup,2,&out)&&!dvm_exception(v)&&out.l;
 log6("stage=reflection_lookup ok=%d method=%u exception=%u",ok,out.l,dvm_exception(v));if(!ok)return false;
 dvm_ref method=out.l,values=dvm_new_array(v,'L',"Ljava/lang/Object;",6);dvm_ref *vs=dvm_array_data(v,values);
 vs[0]=dvm_new_string(v,"test0000");vs[1]=dvm_new_string(v,secret);vs[2]=security;vs[3]=0;vs[5]=activity;
 struct dvm_class *integer=dvm_find_class_or_external(v,"java/lang/Integer");struct dvm_method *box=dvm_find_method(v,integer,"valueOf","(I)Ljava/lang/Integer;");union dvm_value zero={.i=0};
 if(!box||!dvm_call(v,box,0,&zero,1,&out)||dvm_exception(v)){log6("stage=boxing ok=0 exception=%u",dvm_exception(v));return false;}vs[4]=out.l;
 union dvm_value invoke[2]={{.l=presenter},{.l=values}};
 ok=reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",method,invoke,2,&out)&&!dvm_exception(v);
 char err[900]={0};if(dvm_exception(v))dvm_describe_exception(v,dvm_exception(v),err,sizeof err);
 log6("stage=reflection_invoke ok=%d exception=%u detail=%s",ok,dvm_exception(v),err);return ok;
}

/* Hidden strong aliases bind ownership certificates to these reviewed bodies,
 * even if a public/weak callback name is replaced at link time. */
extern __typeof__(task6_trace_event) leak_cert_task6 __attribute__((alias("task6_trace_event"),visibility("hidden")));
