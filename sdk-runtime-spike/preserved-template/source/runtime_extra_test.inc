#include "runtime_extra.h"
#include <stdlib.h>
static bool extra_object_text(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)self;(void)a;(void)n;out->l=dvm_new_string(v,"object-text");return true;}
static bool extra_object_bad_text(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)self;(void)a;(void)n;out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/StringBuilder"));return out->l!=0;}
static bool extra_object_null_text(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)v;(void)self;(void)a;(void)n;out->l=0;return true;}
/* A registered builtin class gives append(Object) a reliable toString()
 * implementation without loading a dex; the bad variants return a non-String
 * or null so the fail-closed boundary is exercised. */
static dvm_ref extra_object_with_text(struct dvm *v,const char *desc,dvm_builtin_fn fn){
 struct dvm_class *c=dvm__register_builtin(v,desc);if(!c)return 0;
 c->methods=calloc(1,sizeof *c->methods);if(!c->methods)return 0;
 c->nmethods=1;c->methods[0].cls=c;c->methods[0].name="toString";c->methods[0].sig=strdup("()Ljava/lang/String;");c->methods[0].ret_kind='L';c->methods[0].builtin=fn;
 return dvm_new_object(v,c);
}
static union dvm_value extra(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref r,union dvm_value a,union dvm_value b){union dvm_value args[]={a,b},out={0};if(!runtime_extra_call(v,c,m,s,r,args,2,&out))dvm__throw(v,"java/lang/AssertionError","standard test dispatch %s.%s",c,m);return out;}
static union dvm_value extra_android(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref r,union dvm_value a,union dvm_value b){union dvm_value args[]={a,b},out={0};if(!android_basics_call(v,c,m,s,r,args,2,&out))dvm__throw(v,"java/lang/AssertionError","thread test dispatch %s.%s",c,m);return out;}
static bool extra_exception_named(struct dvm *v,const char *name){struct dvm_object *e=dvm__obj(v,dvm_exception(v));return e&&e->cls&&e->cls->name&&!strcmp(e->cls->name,name);}
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
 union dvm_value slash={.i='/'};EXTRA_CHECK(extra(v,builder,"append","(C)Ljava/lang/StringBuilder;",b,slash,z).l==b);EXTRA_CHECK(strstr(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",b,z,z).l),"/")!=NULL);
 number.i=-1;extra(v,builder,"<init>","(I)V",b,number,z);EXTRA_CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 /* StringBuilder.length() and deleteCharAt(int) are the tail of
  * HttpUtils.appendUrlParams: length() -> deleteCharAt(length-1) -> toString().
  * Java counts UTF-16 code units while this builder stores UTF-8 bytes. */
 dvm_ref q=dvm_new_object(v,dvm_find_class_or_external(v,builder));extra(v,builder,"<init>","()V",q,z,z);
 union dvm_value abc={.l=dvm_new_string(v,"abc")},chd={.i='d'},idx0={.i=0},idx1={.i=1},idxm1={.i=-1};
 extra(v,builder,"append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",q,abc,z);
 EXTRA_CHECK(extra(v,builder,"length","()I",q,z,z).i==3);
 EXTRA_CHECK(extra(v,builder,"append","(C)Ljava/lang/StringBuilder;",q,chd,z).l==q);
 EXTRA_CHECK(extra(v,builder,"length","()I",q,z,z).i==4);
 EXTRA_CHECK(extra(v,builder,"deleteCharAt","(I)Ljava/lang/StringBuilder;",q,idx0,z).l==q);
 EXTRA_CHECK(extra(v,builder,"length","()I",q,z,z).i==3);
 EXTRA_CHECK(!strcmp(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",q,z,z).l),"bcd"));
 dvm_ref cjk=dvm_new_object(v,dvm_find_class_or_external(v,builder));extra(v,builder,"<init>","()V",cjk,z,z);
 union dvm_value zhong={.l=dvm_new_string(v,"\xe4\xb8\xad")};
 extra(v,builder,"append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",cjk,zhong,z);
 EXTRA_CHECK(extra(v,builder,"length","()I",cjk,z,z).i==1);
 EXTRA_CHECK(extra(v,builder,"deleteCharAt","(I)Ljava/lang/StringBuilder;",cjk,idx0,z).l==cjk);
 EXTRA_CHECK(extra(v,builder,"length","()I",cjk,z,z).i==0);
 EXTRA_CHECK(dvm_string_utf8_length(v,extra(v,builder,"toString","()Ljava/lang/String;",cjk,z,z).l)==0);
 dvm_ref face=dvm_new_object(v,dvm_find_class_or_external(v,builder));extra(v,builder,"<init>","()V",face,z,z);
 union dvm_value emoji={.l=dvm_new_string(v,"\xf0\x9f\x98\x80")};
 extra(v,builder,"append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",face,emoji,z);
 EXTRA_CHECK(extra(v,builder,"length","()I",face,z,z).i==2);
 /* Deleting the low half leaves the high surrogate U+D83D, stored in the
  * same three-byte form append(char) uses for an unpaired surrogate. */
 EXTRA_CHECK(extra(v,builder,"deleteCharAt","(I)Ljava/lang/StringBuilder;",face,idx1,z).l==face);
 EXTRA_CHECK(extra(v,builder,"length","()I",face,z,z).i==1);
 const char *half=dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",face,z,z).l);
 EXTRA_CHECK(dvm_string_utf8_length(v,extra(v,builder,"toString","()Ljava/lang/String;",face,z,z).l)==3&&!memcmp(half,"\xed\xa0\xbd",3));
 EXTRA_CHECK(extra(v,builder,"deleteCharAt","(I)Ljava/lang/StringBuilder;",face,idx0,z).l==face);
 EXTRA_CHECK(extra(v,builder,"length","()I",face,z,z).i==0);
 dvm_ref face2=dvm_new_object(v,dvm_find_class_or_external(v,builder));extra(v,builder,"<init>","()V",face2,z,z);
 extra(v,builder,"append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",face2,emoji,z);
 /* Deleting the high half leaves the low surrogate U+DE00. */
 EXTRA_CHECK(extra(v,builder,"deleteCharAt","(I)Ljava/lang/StringBuilder;",face2,idx0,z).l==face2);
 half=dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",face2,z,z).l);
 EXTRA_CHECK(dvm_string_utf8_length(v,extra(v,builder,"toString","()Ljava/lang/String;",face2,z,z).l)==3&&!memcmp(half,"\xed\xb8\x80",3));
 EXTRA_CHECK(extra(v,builder,"length","()I",face2,z,z).i==1);
 dvm_ref split=dvm_new_object(v,dvm_find_class_or_external(v,builder));extra(v,builder,"<init>","()V",split,z,z);
 union dvm_value hi_unit={.i=0xd83d},lo_unit={.i=0xde00};
 extra(v,builder,"append","(C)Ljava/lang/StringBuilder;",split,hi_unit,z);
 extra(v,builder,"append","(C)Ljava/lang/StringBuilder;",split,lo_unit,z);
 EXTRA_CHECK(extra(v,builder,"length","()I",split,z,z).i==2);
 half=dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",split,z,z).l);
 EXTRA_CHECK(dvm_string_utf8_length(v,extra(v,builder,"toString","()Ljava/lang/String;",split,z,z).l)==6&&!memcmp(half,"\xed\xa0\xbd\xed\xb8\x80",6));
 EXTRA_CHECK(extra(v,builder,"deleteCharAt","(I)Ljava/lang/StringBuilder;",split,idx0,z).l==split);
 half=dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",split,z,z).l);
 EXTRA_CHECK(dvm_string_utf8_length(v,extra(v,builder,"toString","()Ljava/lang/String;",split,z,z).l)==3&&!memcmp(half,"\xed\xb8\x80",3));
 EXTRA_CHECK(extra(v,builder,"length","()I",split,z,z).i==1);
 dvm_ref empty=dvm_new_object(v,dvm_find_class_or_external(v,builder));extra(v,builder,"<init>","()V",empty,z,z);
 extra(v,builder,"deleteCharAt","(I)Ljava/lang/StringBuilder;",empty,idx0,z);
 EXTRA_CHECK(dvm_exception(v)!=0&&extra_exception_named(v,"java/lang/StringIndexOutOfBoundsException"));dvm_clear_exception(v);
 extra(v,builder,"deleteCharAt","(I)Ljava/lang/StringBuilder;",empty,idxm1,z);
 EXTRA_CHECK(dvm_exception(v)!=0&&extra_exception_named(v,"java/lang/StringIndexOutOfBoundsException"));dvm_clear_exception(v);
 union dvm_value reject={0};
 EXTRA_CHECK(!runtime_extra_call(v,builder,"length","()J",q,NULL,0,&reject));
 EXTRA_CHECK(!dvm_exception(v));
 EXTRA_CHECK(!runtime_extra_call(v,builder,"deleteCharAt","(J)Ljava/lang/StringBuilder;",q,&idx0,1,&reject));
 EXTRA_CHECK(!dvm_exception(v));
 /* Malformed internal UTF-8 must fail and be logged, not be counted as bytes. */
 dvm_ref broken=dvm_new_object(v,dvm_find_class_or_external(v,builder));
 union dvm_value badutf={.l=dvm_new_string_n(v,"\xe4\xb8",2)};
 extra(v,builder,"<init>","(Ljava/lang/String;)V",broken,badutf,z);
 extra(v,builder,"length","()I",broken,z,z);
 EXTRA_CHECK(dvm_exception(v)!=0&&extra_exception_named(v,"java/lang/InternalError"));dvm_clear_exception(v);
 /* Java StringBuilder.append(Object) is append(String.valueOf(object)):
  * null appends "null", a String contributes its contents, and any other
  * object contributes toString().  The okhttp3/RealCall$AsyncCall.execute
  * caller appends a caught Throwable, whose toString() is a host stub. */
 dvm_ref objb=dvm_new_object(v,dvm_find_class_or_external(v,builder));extra(v,builder,"<init>","()V",objb,z,z);
 EXTRA_CHECK(extra(v,builder,"append","(Ljava/lang/Object;)Ljava/lang/StringBuilder;",objb,z,z).l==objb);
 EXTRA_CHECK(!strcmp(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",objb,z,z).l),"null"));
 union dvm_value object_string={.l=dvm_new_string(v,"object-string")};
 EXTRA_CHECK(extra(v,builder,"append","(Ljava/lang/Object;)Ljava/lang/StringBuilder;",objb,object_string,z).l==objb);
 EXTRA_CHECK(!strcmp(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",objb,z,z).l),"nullobject-string"));
 dvm_ref text_object=extra_object_with_text(v,"Lsdktrace/tests/StandardObjectText;",extra_object_text);
 union dvm_value text_arg={.l=text_object};
 EXTRA_CHECK(text_object&&extra(v,builder,"append","(Ljava/lang/Object;)Ljava/lang/StringBuilder;",objb,text_arg,z).l==objb);
 EXTRA_CHECK(!strcmp(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",objb,z,z).l),"nullobject-stringobject-text"));
 /* A null or non-String toString() result must fail closed, not append an
  * empty string or a fabricated value. */
 dvm_ref bad_object=extra_object_with_text(v,"Lsdktrace/tests/StandardBadObjectText;",extra_object_bad_text);
 EXTRA_CHECK(bad_object!=0);
 union dvm_value bad_arg={.l=bad_object};
 extra(v,builder,"append","(Ljava/lang/Object;)Ljava/lang/StringBuilder;",objb,bad_arg,z);
 EXTRA_CHECK(dvm_exception(v)!=0&&extra_exception_named(v,"java/lang/ClassCastException"));dvm_clear_exception(v);
 EXTRA_CHECK(!strcmp(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",objb,z,z).l),"nullobject-stringobject-text"));
 dvm_ref null_object=extra_object_with_text(v,"Lsdktrace/tests/StandardNullObjectText;",extra_object_null_text);
 EXTRA_CHECK(null_object!=0);
 union dvm_value null_arg={.l=null_object};
 extra(v,builder,"append","(Ljava/lang/Object;)Ljava/lang/StringBuilder;",objb,null_arg,z);
 EXTRA_CHECK(dvm_exception(v)!=0&&extra_exception_named(v,"java/lang/ClassCastException"));dvm_clear_exception(v);
 /* A receiver with no toString() at all also fails closed. */
 dvm_ref no_text_object=dvm_new_object(v,dvm__register_builtin(v,"Lsdktrace/tests/StandardNoObjectText;"));
 EXTRA_CHECK(no_text_object!=0);
 union dvm_value no_text_arg={.l=no_text_object};
 extra(v,builder,"append","(Ljava/lang/Object;)Ljava/lang/StringBuilder;",objb,no_text_arg,z);
 EXTRA_CHECK(dvm_exception(v)!=0&&extra_exception_named(v,"java/lang/NoSuchMethodError"));dvm_clear_exception(v);
 EXTRA_CHECK(!strcmp(dvm_string_utf8(v,extra(v,builder,"toString","()Ljava/lang/String;",objb,z,z).l),"nullobject-stringobject-text"));
 EXTRA_CHECK(!runtime_extra_call(v,builder,"append","(D)Ljava/lang/StringBuilder;",objb,&number,1,&reject));
 EXTRA_CHECK(!dvm_exception(v));
 EXTRA_CHECK(!runtime_extra_call(v,builder,"append","(Ljava/lang/Object;)Ljava/lang/Object;",objb,&text_arg,1,&reject));
 EXTRA_CHECK(!dvm_exception(v));
 uint64_t previous=0;for(int i=0;i<16;++i){dvm_ref u=extra(v,uuid,"randomUUID","()Ljava/util/UUID;",0,z,z).l;EXTRA_CHECK(u!=0);if(!u)break;
  EXTRA_CHECK(extra(v,uuid,"version","()I",u,z,z).i==4);EXTRA_CHECK(extra(v,uuid,"variant","()I",u,z,z).i==2);
  dvm_ref text=extra(v,uuid,"toString","()Ljava/lang/String;",u,z,z).l;const char *s=dvm_string_utf8(v,text);EXTRA_CHECK(s&&strlen(s)==36&&s[8]=='-'&&s[13]=='-'&&s[14]=='4'&&s[18]=='-'&&s[23]=='-');
  union dvm_value hi=extra(v,uuid,"getMostSignificantBits","()J",u,z,z),lo=extra(v,uuid,"getLeastSignificantBits","()J",u,z,z);EXTRA_CHECK(i==0||lo.ju!=previous);previous=lo.ju;
  dvm_ref copy=dvm_new_object(v,dvm_find_class_or_external(v,uuid));extra(v,uuid,"<init>","(JJ)V",copy,hi,lo);union dvm_value original={.l=u};EXTRA_CHECK(extra(v,uuid,"equals","(Ljava/lang/Object;)Z",copy,original,z).i==1);
 }
 dvm_ref r=extra(v,runtime,"getRuntime","()Ljava/lang/Runtime;",0,z,z).l;EXTRA_CHECK(r&&extra(v,runtime,"getRuntime","()Ljava/lang/Runtime;",0,z,z).l==r);EXTRA_CHECK(extra(v,runtime,"availableProcessors","()I",r,z,z).i==sysconf(_SC_NPROCESSORS_ONLN));EXTRA_CHECK(runtime_extra_call(v,runtime,"maxMemory","()J",r,NULL,0,&out)&&out.j==VM_ARENA_LIMIT);
 const char *thread="android/os/HandlerThread";dvm_ref ht=dvm_new_object(v,dvm_find_class_or_external(v,thread));union dvm_value name={.l=dvm_new_string(v,"diagnostic-not-started")};extra_android(v,thread,"<init>","(Ljava/lang/String;)V",ht,name,z);EXTRA_CHECK(extra_android(v,"java/lang/Thread","getName","()Ljava/lang/String;",ht,z,z).l==name.l);EXTRA_CHECK(extra_android(v,"java/lang/Thread","isAlive","()Z",ht,z,z).i==0);EXTRA_CHECK(extra_android(v,thread,"getLooper","()Landroid/os/Looper;",ht,z,z).l==0);EXTRA_CHECK(!thread_runtime_looper(v,0));
 dvm_ref s=dvm_new_string(v,"A-B-CD");union dvm_value pattern={.l=dvm_new_string(v,"-")},replacement={.l=dvm_new_string(v,"")};union dvm_value args[]={pattern,replacement};EXTRA_CHECK(android_basics_call(v,"java/lang/String","replaceAll","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",s,args,2,&out));EXTRA_CHECK(!strcmp(dvm_string_utf8(v,out.l),"ABCD"));dvm_ref replaced=out.l;EXTRA_CHECK(android_basics_call(v,"java/lang/String","toLowerCase","()Ljava/lang/String;",replaced,NULL,0,&out)&&!strcmp(dvm_string_utf8(v,out.l),"abcd"));args[0].l=dvm_new_string(v,".*");EXTRA_CHECK(!android_basics_call(v,"java/lang/String","replaceAll","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",s,args,2,&out));
 EXTRA_CHECK(!dvm_exception(v));sdk_trace_fprintf(stderr,"STANDARD_SELFTEST %s marker=numeric-next builder_growth=100 builder_utf16=1 builder_bounds=1 builder_invalid_utf8=1 builder_append_object=1 uuid_samples=16 os_entropy=1 runtime_identity=1 thread_initial_state_NEW=1 unknown_regex_rejected=1\n",okay?"PASS":"FAIL");return okay;
}
