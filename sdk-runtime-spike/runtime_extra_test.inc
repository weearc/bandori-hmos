#include "runtime_extra.h"
static union dvm_value extra(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref r,union dvm_value a,union dvm_value b){union dvm_value args[]={a,b},out={0};if(!runtime_extra_call(v,c,m,s,r,args,2,&out))dvm__throw(v,"java/lang/AssertionError","standard test dispatch %s.%s",c,m);return out;}
static union dvm_value extra_android(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref r,union dvm_value a,union dvm_value b){union dvm_value args[]={a,b},out={0};if(!android_basics_call(v,c,m,s,r,args,2,&out))dvm__throw(v,"java/lang/AssertionError","thread test dispatch %s.%s",c,m);return out;}
bool runtime_extra_selftest(struct dvm *v,unsigned *checks){
 bool okay=true;union dvm_value z={0},out={0};const char *builder="java/lang/StringBuilder",*uuid="java/util/UUID",*runtime="java/lang/Runtime";
#define EXTRA_CHECK(expr) do{++*checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL standard line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 dvm_ref b=dvm_new_object(v,dvm_find_class_or_external(v,builder));extra(v,builder,"<init>","()V",b,z,z);
 union dvm_value a={.l=dvm_new_string(v,"abc")};EXTRA_CHECK(extra(v,builder,"append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",b,a,z).l==b);
 dvm_ref before=extra(v,builder,"toString","()Ljava/lang/String;",b,z,z).l;extra(v,builder,"append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",b,z,z);
 EXTRA_CHECK(!strcmp(dvm_string_utf8(v,before),"abc"));EXTRA_CHECK(!strcmp(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",b,z,z).l),"abcnull"));
 for(int i=0;i<100;++i)extra(v,builder,"append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",b,a,z);
 EXTRA_CHECK(dvm_string_utf8_length(v,extra(v,builder,"toString","()Ljava/lang/String;",b,z,z).l)==307);
 union dvm_value number={.j=-1234567890123LL};extra(v,builder,"append","(J)Ljava/lang/StringBuilder;",b,number,z);EXTRA_CHECK(strstr(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",b,z,z).l),"-1234567890123")!=NULL);
 number.i=-1;extra(v,builder,"<init>","(I)V",b,number,z);EXTRA_CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 uint64_t previous=0;for(int i=0;i<16;++i){dvm_ref u=extra(v,uuid,"randomUUID","()Ljava/util/UUID;",0,z,z).l;EXTRA_CHECK(u!=0);if(!u)break;
  EXTRA_CHECK(extra(v,uuid,"version","()I",u,z,z).i==4);EXTRA_CHECK(extra(v,uuid,"variant","()I",u,z,z).i==2);
  dvm_ref text=extra(v,uuid,"toString","()Ljava/lang/String;",u,z,z).l;const char *s=dvm_string_utf8(v,text);EXTRA_CHECK(s&&strlen(s)==36&&s[8]=='-'&&s[13]=='-'&&s[14]=='4'&&s[18]=='-'&&s[23]=='-');
  union dvm_value hi=extra(v,uuid,"getMostSignificantBits","()J",u,z,z),lo=extra(v,uuid,"getLeastSignificantBits","()J",u,z,z);EXTRA_CHECK(i==0||lo.ju!=previous);previous=lo.ju;
  dvm_ref copy=dvm_new_object(v,dvm_find_class_or_external(v,uuid));extra(v,uuid,"<init>","(JJ)V",copy,hi,lo);union dvm_value original={.l=u};EXTRA_CHECK(extra(v,uuid,"equals","(Ljava/lang/Object;)Z",copy,original,z).i==1);
 }
 dvm_ref r=extra(v,runtime,"getRuntime","()Ljava/lang/Runtime;",0,z,z).l;EXTRA_CHECK(r&&extra(v,runtime,"getRuntime","()Ljava/lang/Runtime;",0,z,z).l==r);EXTRA_CHECK(extra(v,runtime,"availableProcessors","()I",r,z,z).i==sysconf(_SC_NPROCESSORS_ONLN));EXTRA_CHECK(!runtime_extra_call(v,runtime,"maxMemory","()J",r,NULL,0,&out));
 const char *thread="android/os/HandlerThread";dvm_ref ht=dvm_new_object(v,dvm_find_class_or_external(v,thread));union dvm_value name={.l=dvm_new_string(v,"diagnostic-not-started")};extra_android(v,thread,"<init>","(Ljava/lang/String;)V",ht,name,z);EXTRA_CHECK(extra_android(v,"java/lang/Thread","getName","()Ljava/lang/String;",ht,z,z).l==name.l);EXTRA_CHECK(extra_android(v,"java/lang/Thread","isAlive","()Z",ht,z,z).i==0);EXTRA_CHECK(extra_android(v,thread,"getLooper","()Landroid/os/Looper;",ht,z,z).l==0);EXTRA_CHECK(!android_basics_call(v,thread,"start","()V",ht,NULL,0,&out));
 dvm_ref s=dvm_new_string(v,"A-B-CD");union dvm_value pattern={.l=dvm_new_string(v,"-")},replacement={.l=dvm_new_string(v,"")};union dvm_value args[]={pattern,replacement};EXTRA_CHECK(android_basics_call(v,"java/lang/String","replaceAll","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",s,args,2,&out));EXTRA_CHECK(!strcmp(dvm_string_utf8(v,out.l),"ABCD"));dvm_ref replaced=out.l;EXTRA_CHECK(android_basics_call(v,"java/lang/String","toLowerCase","()Ljava/lang/String;",replaced,NULL,0,&out)&&!strcmp(dvm_string_utf8(v,out.l),"abcd"));args[0].l=dvm_new_string(v,".*");EXTRA_CHECK(!android_basics_call(v,"java/lang/String","replaceAll","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",s,args,2,&out));
 EXTRA_CHECK(!dvm_exception(v));sdk_trace_fprintf(stderr,"STANDARD_SELFTEST %s marker=standard-batch-v7 builder_growth=100 uuid_samples=16 os_entropy=1 runtime_identity=1 thread_new_only=1 unknown_regex_rejected=1\n",okay?"PASS":"FAIL");return okay;
}
