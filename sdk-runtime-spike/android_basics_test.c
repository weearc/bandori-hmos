#include "android_basics.h"
#include "portable.h"
#include <pthread.h>
#include <stdlib.h>
static union dvm_value call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref r,union dvm_value a,union dvm_value b){
 union dvm_value args[]={a,b},out={0};if(!android_basics_call(v,c,m,s,r,args,2,&out))dvm__throw(v,"java/lang/AssertionError","android test method not implemented");return out;
}
static bool sequence_length(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)a;(void)n;out->i=*(int *)dvm__obj(v,self)->data;return true;}
static dvm_ref sequence(struct dvm *v,int n){
 struct dvm_class *c=dvm__register_builtin(v,"Lsdktrace/tests/Sequence;");c->methods=calloc(1,sizeof *c->methods);c->nmethods=1;c->methods[0].cls=c;c->methods[0].name="length";c->methods[0].sig=strdup("()I");c->methods[0].ret_kind='I';c->methods[0].builtin=sequence_length;
 dvm_ref r=dvm_new_object(v,c);struct dvm_object *o=dvm__obj(v,r);o->data=malloc(sizeof(int));*(int *)o->data=n;return r;
}
struct thread_case{struct dvm *v;dvm_ref looper;bool pass;};
static void *worker(void *arg){struct thread_case *t=arg;union dvm_value z={0};t->pass=call(t->v,"android/os/Looper","myLooper","()Landroid/os/Looper;",0,z,z).l==0&&call(t->v,"android/os/Looper","isCurrentThread","()Z",t->looper,z,z).i==0;return NULL;}
bool android_basics_selftest(struct dvm *v,unsigned *checks){
 bool okay=true;union dvm_value z={0},out={0};
#define CHECK(expr) do{++*checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL android line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 const char *text="android/text/TextUtils",*loop="android/os/Looper",*handler="android/os/Handler";
 dvm_ref application=android_basics_application(v);CHECK(application!=0);CHECK(android_basics_application(v)==application);
 CHECK(call(v,"android/content/Context","getApplicationContext","()Landroid/content/Context;",application,z,z).l==application);
 CHECK(dvm__class_assignable(v,dvm_object_class(v,application),dvm_find_class_or_external(v,"android/content/Context")));
 union dvm_value package=call(v,"android/content/Context","getPackageName","()Ljava/lang/String;",application,z,z);CHECK(package.l&&strlen(dvm_string_utf8(v,package.l))>0);
 dvm_ref wrapper=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/ContextWrapper"));union dvm_value base={.l=application};call(v,"android/content/ContextWrapper","<init>","(Landroid/content/Context;)V",wrapper,base,z);
 CHECK(call(v,"android/content/ContextWrapper","getBaseContext","()Landroid/content/Context;",wrapper,z,z).l==application);CHECK(call(v,"android/content/Context","getApplicationContext","()Landroid/content/Context;",wrapper,z,z).l==application);
 CHECK(!android_basics_call(v,"android/content/Context","getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",application,&z,1,&out));
 struct timespec now;clock_gettime(CLOCK_REALTIME,&now);int64_t wall=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;union dvm_value clock=call(v,"java/lang/System","currentTimeMillis","()J",0,z,z);CHECK(clock.j>=wall&&clock.j-wall<1000);
 CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,z,z).i==1);
 union dvm_value empty={.l=dvm_new_string(v,"")},str={.l=dvm_new_string(v,"abc")};
 CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,empty,z).i==1);CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,str,z).i==0);
 union dvm_value seq={.l=sequence(v,0)};CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,seq,z).i==1);*(int *)dvm__obj(v,seq.l)->data=7;CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,seq,z).i==0);
 sdk_trace_fprintf(stderr,"SELFTEST_EXPECTED_MISSING custom CharSequence without length must throw\n");
 union dvm_value unknown={.l=dvm_new_object(v,dvm__register_builtin(v,"Lsdktrace/tests/UnknownSequence;"))};call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,unknown,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 dvm_ref u=dvm_new_string(v,"a\xf0\x9f\x98\x80" "b");union dvm_value index={.i=3},face={.i=0x1f600};
 CHECK(call(v,"java/lang/String","length","()I",u,z,z).i==4);CHECK(call(v,"java/lang/String","charAt","(I)C",u,index,z).i=='b');CHECK(call(v,"java/lang/String","indexOf","(I)I",u,face,z).i==1);
 index.i=2;CHECK(call(v,"java/lang/String","indexOf","(II)I",u,face,index).i==-1);index.i='b';CHECK(call(v,"java/lang/String","indexOf","(I)I",u,index,z).i==3);
 dvm_ref main=call(v,loop,"getMainLooper","()Landroid/os/Looper;",0,z,z).l;CHECK(main!=0);CHECK(call(v,loop,"getMainLooper","()Landroid/os/Looper;",0,z,z).l==main);CHECK(call(v,loop,"myLooper","()Landroid/os/Looper;",0,z,z).l==main);CHECK(call(v,loop,"isCurrentThread","()Z",main,z,z).i==1);
 dvm_ref h=dvm_new_object(v,dvm_find_class_or_external(v,handler));union dvm_value l={.l=main};call(v,handler,"<init>","(Landroid/os/Looper;)V",h,l,z);CHECK(call(v,handler,"getLooper","()Landroid/os/Looper;",h,z,z).l==main);
 dvm_ref implicit=dvm_new_object(v,dvm_find_class_or_external(v,handler));call(v,handler,"<init>","()V",implicit,z,z);CHECK(call(v,handler,"getLooper","()Landroid/os/Looper;",implicit,z,z).l==main);
 CHECK(!android_basics_call(v,handler,"post","(Ljava/lang/Runnable;)Z",h,&z,1,&out));CHECK(!android_basics_call(v,loop,"loop","()V",0,NULL,0,&out));
 struct thread_case test={.v=v,.looper=main};pthread_t thread;int created=pthread_create(&thread,NULL,worker,&test);CHECK(created==0);if(!created){pthread_join(thread,NULL);CHECK(test.pass);}
 call(v,handler,"<init>","(Landroid/os/Looper;)V",h,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 CHECK(!dvm_exception(v));sdk_trace_fprintf(stderr,"ANDROID_SELFTEST %s custom_charsequence=1 utf16=1 vm_looper_identity=1 handler_binding=1 scheduler_unimplemented=1\n",okay?"PASS":"FAIL");return okay;
}
