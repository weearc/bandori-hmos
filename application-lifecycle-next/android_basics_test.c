#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* bionic declares gettid() only under _GNU_SOURCE */
#endif
#include "thread_runtime.h"
#include "android_basics.h"
#include "portable.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
static union dvm_value call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref r,union dvm_value a,union dvm_value b){
 union dvm_value args[]={a,b},out={0};if(!android_basics_call(v,c,m,s,r,args,2,&out))dvm__throw(v,"java/lang/AssertionError","android test method not implemented");return out;
}
static bool sequence_length(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)a;(void)n;out->i=*(int *)dvm__obj(v,self)->data;return true;}
static dvm_ref sequence(struct dvm *v,int n){
 struct dvm_class *c=dvm__register_builtin(v,"Lsdktrace/tests/Sequence;");c->methods=calloc(1,sizeof *c->methods);c->nmethods=1;c->methods[0].cls=c;c->methods[0].name="length";c->methods[0].sig=strdup("()I");c->methods[0].ret_kind='I';c->methods[0].builtin=sequence_length;
 dvm_ref r=dvm_new_object(v,c);struct dvm_object *o=dvm__obj(v,r);o->data=malloc(sizeof(int));*(int *)o->data=n;return r;
}
struct text_units{int length;uint16_t *units;};
static bool text_units_length(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)a;(void)n;struct text_units *s=dvm__obj(v,self)->data;out->i=s?s->length:0;return true;}
static bool text_units_char_at(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)n;struct text_units *s=dvm__obj(v,self)->data;int i=a[0].i;if(!s||i<0||i>=s->length){dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","test charAt");return false;}out->i=s->units[i];return true;}
/* Custom CharSequence with real UTF-16 content. TextUtils.equals must use
 * length()/charAt() on it, not compare object references. */
static dvm_ref text_units(struct dvm *v,const uint16_t *units,int length){
 struct dvm_class *c=dvm__register_builtin(v,"Lsdktrace/tests/TextSequence;");if(!c)return 0;
 if(!c->methods){
  c->methods=calloc(2,sizeof *c->methods);if(!c->methods)return 0;c->nmethods=2;
  c->methods[0]=(struct dvm_method){.cls=c,.name="length",.sig=strdup("()I"),.ret_kind='I',.builtin=text_units_length};
  c->methods[1]=(struct dvm_method){.cls=c,.name="charAt",.sig=strdup("(I)C"),.arg_count=1,.arg_slots=1,.ret_kind='C',.builtin=text_units_char_at};
 }
 dvm_ref r=dvm_new_object(v,c);struct dvm_object *o=dvm__obj(v,r);if(!o)return 0;
 struct text_units *s=malloc(sizeof *s);if(!s)return 0;
 s->length=length;s->units=malloc((size_t)length*sizeof *s->units);
 if(!s->units){free(s);return 0;}
 memcpy(s->units,units,(size_t)length*sizeof *s->units);o->data=s;return r;
}
static union dvm_value call1(struct dvm *v,const char *c,const char *m,const char *s,union dvm_value a){union dvm_value args[]={a},out={0};if(!android_basics_call(v,c,m,s,0,args,1,&out))dvm__throw(v,"java/lang/AssertionError","android test method not implemented");return out;}
static bool throws(struct dvm *v,const char *type,const char *c,const char *m,const char *s,union dvm_value a,union dvm_value b){union dvm_value args[]={a,b},out={0};bool claimed=android_basics_call(v,c,m,s,0,args,2,&out),ok=claimed&&dvm_exception(v)!=0;if(ok){struct dvm_object *e=dvm__obj(v,dvm_exception(v));if(!e||!e->cls||strcmp(e->cls->name,type))ok=false;}if(dvm_exception(v))dvm_clear_exception(v);return ok;}
/* A failed System.loadLibrary/load must surface a real UnsatisfiedLinkError
 * (missing/invalid native library), never a claimed success. */
static bool link_error(struct dvm *v,const char *c,const char *m,const char *sig,const char *text,const char *type){
 union dvm_value arg={.l=dvm_new_string(v,text)},out={0};
 if(!arg.l)return false;
 bool claimed=android_basics_call(v,c,m,sig,0,&arg,1,&out),ok=claimed&&dvm_exception(v)!=0;
 if(ok){struct dvm_object *e=dvm__obj(v,dvm_exception(v));ok=e&&e->cls&&e->cls->name&&!strcmp(e->cls->name,type);}
 if(dvm_exception(v))dvm_clear_exception(v);
 return ok;
}
struct thread_case{struct dvm *v;dvm_ref looper;bool pass;};
static void *worker(void *arg){struct thread_case *t=arg;dvm_gil_acquire(t->v);union dvm_value z={0};t->pass=call(t->v,"android/os/Looper","myLooper","()Landroid/os/Looper;",0,z,z).l==0&&call(t->v,"android/os/Looper","isCurrentThread","()Z",t->looper,z,z).i==0;dvm_gil_release(t->v);return NULL;}
bool android_basics_selftest(struct dvm *v,unsigned *checks){
 bool okay=true;union dvm_value z={0},out={0};
#define CHECK(expr) do{++*checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL android line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 const char *text="android/text/TextUtils",*loop="android/os/Looper",*handler="android/os/Handler",*process="android/os/Process";
 dvm_ref application=android_basics_application(v);CHECK(application!=0);CHECK(android_basics_application(v)==application);
 CHECK(call(v,"android/content/Context","getApplicationContext","()Landroid/content/Context;",application,z,z).l==application);
 CHECK(dvm__class_assignable(v,dvm_object_class(v,application),dvm_find_class_or_external(v,"android/content/Context")));
 union dvm_value package=call(v,"android/content/Context","getPackageName","()Ljava/lang/String;",application,z,z);CHECK(package.l&&strlen(dvm_string_utf8(v,package.l))>0);
 dvm_ref wrapper=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/ContextWrapper"));union dvm_value base={.l=application};call(v,"android/content/ContextWrapper","<init>","(Landroid/content/Context;)V",wrapper,base,z);
 CHECK(call(v,"android/content/ContextWrapper","getBaseContext","()Landroid/content/Context;",wrapper,z,z).l==application);CHECK(call(v,"android/content/Context","getApplicationContext","()Landroid/content/Context;",wrapper,z,z).l==application);
 /* Context.checkPermission: only the two normal permissions this HAP declares
  * are granted, and only for the real process identity.  Unknown permissions
  * are denied, never fabricated as granted. */
 union dvm_value permission[3]={{.l=dvm_new_string(v,"android.permission.INTERNET")},{.i=(int32_t)getpid()},{.i=(int32_t)getuid()}};
 CHECK(android_basics_call(v,"android/content/Context","checkPermission","(Ljava/lang/String;II)I",application,permission,3,&out)&&out.i==0);
 permission[0].l=dvm_new_string(v,"android.permission.ACCESS_NETWORK_STATE");
 CHECK(android_basics_call(v,"android/content/Context","checkPermission","(Ljava/lang/String;II)I",application,permission,3,&out)&&out.i==0);
 permission[0].l=dvm_new_string(v,"com.android.launcher.permission.INSTALL_SHORTCUT");
 CHECK(android_basics_call(v,"android/content/Context","checkPermission","(Ljava/lang/String;II)I",application,permission,3,&out)&&out.i==-1);
 permission[0].l=dvm_new_string(v,"android.permission.INTERNET");permission[1].i=(int32_t)getpid()+1;
 CHECK(android_basics_call(v,"android/content/Context","checkPermission","(Ljava/lang/String;II)I",application,permission,3,&out)&&out.i==-1);
 permission[1].i=(int32_t)getpid();permission[0].l=0;
 bool permission_npe=false;
 if(android_basics_call(v,"android/content/Context","checkPermission","(Ljava/lang/String;II)I",application,permission,3,&out)){
  dvm_ref exception=dvm_exception(v);struct dvm_object *object=exception?dvm__obj(v,exception):NULL;
  permission_npe=object&&object->cls&&object->cls->name&&!strcmp(object->cls->name,"java/lang/NullPointerException");
 }
 dvm_clear_exception(v);CHECK(permission_npe);
 CHECK(!android_basics_call(v,"android/content/Context","checkPermission","(Ljava/lang/String;)I",application,permission,1,&out)&&!dvm_exception(v));
 CHECK(!android_basics_call(v,"android/content/Context","getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",application,&z,1,&out));
 struct timespec now;clock_gettime(CLOCK_REALTIME,&now);int64_t wall=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;union dvm_value clock=call(v,"java/lang/System","currentTimeMillis","()J",0,z,z);CHECK(clock.j>=wall&&clock.j-wall<1000);
 /* Process identity is the real kernel pid/tid/uid of this app process and
  * must remain stable across calls; a wrong signature must not be claimed. */
 union dvm_value pid1=call(v,process,"myPid","()I",0,z,z),pid2=call(v,process,"myPid","()I",0,z,z);
 CHECK(pid1.i==(int)getpid()&&pid1.i>0);CHECK(pid2.i==pid1.i);
 union dvm_value tid=call(v,process,"myTid","()I",0,z,z);CHECK(tid.i==(int)gettid()&&tid.i>0);
 union dvm_value uid=call(v,process,"myUid","()I",0,z,z);CHECK(uid.i==(int)getuid());
 CHECK(!android_basics_call(v,process,"myPid","()J",0,NULL,0,&out)&&!dvm_exception(v));
 CHECK(!android_basics_call(v,process,"killProcess","(I)V",0,NULL,0,&out)&&!dvm_exception(v));
 /* System.loadLibrary is a real host dlopen: invalid names and missing
  * libraries raise UnsatisfiedLinkError; relative System.load paths too. */
 CHECK(link_error(v,"java/lang/System","loadLibrary","(Ljava/lang/String;)V","bad/name","java/lang/UnsatisfiedLinkError"));
 CHECK(link_error(v,"java/lang/System","loadLibrary","(Ljava/lang/String;)V","sdktrace_selftest_missing_lib","java/lang/UnsatisfiedLinkError"));
 CHECK(link_error(v,"java/lang/System","load","(Ljava/lang/String;)V","relative.so","java/lang/UnsatisfiedLinkError"));
 union dvm_value null_lib={0};
 bool null_library=android_basics_call(v,"java/lang/System","loadLibrary","(Ljava/lang/String;)V",0,&null_lib,1,&out)&&dvm_exception(v);
 if(null_library){struct dvm_object *e=dvm__obj(v,dvm_exception(v));null_library=e&&e->cls&&e->cls->name&&!strcmp(e->cls->name,"java/lang/NullPointerException");}
 if(dvm_exception(v))dvm_clear_exception(v);CHECK(null_library);
 CHECK(!android_basics_call(v,"java/lang/System","loadLibrary","(I)V",0,&null_lib,1,&out)&&!dvm_exception(v));
 CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,z,z).i==1);
 union dvm_value empty={.l=dvm_new_string(v,"")},str={.l=dvm_new_string(v,"abc")};
 CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,empty,z).i==1);CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,str,z).i==0);
 union dvm_value seq={.l=sequence(v,0)};CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,seq,z).i==1);*(int *)dvm__obj(v,seq.l)->data=7;CHECK(call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,seq,z).i==0);
 sdk_trace_fprintf(stderr,"SELFTEST_EXPECTED_MISSING custom CharSequence without length must throw\n");
 union dvm_value unknown={.l=dvm_new_object(v,dvm__register_builtin(v,"Lsdktrace/tests/UnknownSequence;"))};call(v,text,"isEmpty","(Ljava/lang/CharSequence;)Z",0,unknown,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 /* TextUtils.equals: identity/null handling plus UTF-16 content comparison of
  * String and third-party CharSequence instances (never object identity). */
 const char *equals_sig="(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Z";
 union dvm_value pair[2]={z,z};
 CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==1&&!dvm_exception(v));
 pair[0]=z;pair[1]=str;CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==0&&!dvm_exception(v));
 pair[0]=str;pair[1]=z;CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==0&&!dvm_exception(v));
 dvm_ref str_copy=dvm_new_string(v,"abc"),abd=dvm_new_string(v,"abd"),empty_copy=dvm_new_string(v,"");
 pair[0]=str;pair[1]=(union dvm_value){.l=str_copy};CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==1&&!dvm_exception(v));
 pair[1]=(union dvm_value){.l=abd};CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==0&&!dvm_exception(v));
 pair[0]=empty;pair[1]=(union dvm_value){.l=empty_copy};CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==1&&!dvm_exception(v));
 uint16_t abc_units[]={'a','b','c'},abd_units[]={'a','b','d'},emoji_units[]={'a',0xd83d,0xde00,'b'};
 dvm_ref seq_abc=text_units(v,abc_units,3),seq_abd=text_units(v,abd_units,3),seq_emoji=text_units(v,emoji_units,4),emoji_string=dvm_new_string(v,"a\xf0\x9f\x98\x80" "b");
 CHECK(seq_abc&&seq_abd&&seq_emoji&&emoji_string);
 pair[0]=(union dvm_value){.l=seq_abc};pair[1]=str;CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==1&&!dvm_exception(v));
 pair[0]=str;pair[1]=(union dvm_value){.l=seq_abc};CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==1&&!dvm_exception(v));
 pair[0]=(union dvm_value){.l=seq_abc};pair[1]=(union dvm_value){.l=seq_abd};CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==0&&!dvm_exception(v));
 pair[0]=(union dvm_value){.l=seq_emoji};pair[1]=(union dvm_value){.l=emoji_string};CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==1&&!dvm_exception(v));
 pair[0]=(union dvm_value){.l=emoji_string};pair[1]=(union dvm_value){.l=seq_emoji};CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==1&&!dvm_exception(v));
 pair[0]=str;pair[1]=(union dvm_value){.l=seq_abd};CHECK(android_basics_call(v,text,"equals",equals_sig,0,pair,2,&out)&&out.i==0&&!dvm_exception(v));
 CHECK(!android_basics_call(v,text,"equals","(Ljava/lang/String;Ljava/lang/String;)Z",0,pair,2,&out)&&!dvm_exception(v));
 /* Color.parseColor: shorthand hex, ARGB alpha placement, AOSP name table,
  * case-insensitive input, and fail-closed rejection of invalid colors. */
 const char *color="android/graphics/Color",*parse_sig="(Ljava/lang/String;)I";
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"#FFF")}).i==-1);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"#80FF0000")}).i==(int32_t)0x80ff0000u);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"#FF0000")}).i==(int32_t)0xffff0000u);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"#ff0000")}).i==(int32_t)0xffff0000u);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"#8F00")}).i==(int32_t)0x88ff0000u);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"#99000000")}).i==(int32_t)0x99000000u);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"red")}).i==(int32_t)0xffff0000u);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"ReD")}).i==(int32_t)0xffff0000u);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"darkgray")}).i==(int32_t)0xff444444u);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"lightgrey")}).i==(int32_t)0xffccccccu);
 CHECK(call1(v,color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"silver")}).i==(int32_t)0xffc0c0c0u);
 CHECK(throws(v,"java/lang/IllegalArgumentException",color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"#GG0000")},z));
 CHECK(throws(v,"java/lang/IllegalArgumentException",color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"")},z));
 CHECK(throws(v,"java/lang/IllegalArgumentException",color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"#12345")},z));
 CHECK(throws(v,"java/lang/IllegalArgumentException",color,"parseColor",parse_sig,(union dvm_value){.l=dvm_new_string(v,"notacolor")},z));
 CHECK(throws(v,"java/lang/NullPointerException",color,"parseColor",parse_sig,z,z));
 CHECK(!android_basics_call(v,color,"parseColor","(Ljava/lang/CharSequence;)I",0,&str,1,&out)&&!dvm_exception(v));
 dvm_ref u=dvm_new_string(v,"a\xf0\x9f\x98\x80" "b");union dvm_value index={.i=3},face={.i=0x1f600};
 CHECK(call(v,"java/lang/String","length","()I",u,z,z).i==4);CHECK(call(v,"java/lang/String","charAt","(I)C",u,index,z).i=='b');CHECK(call(v,"java/lang/String","indexOf","(I)I",u,face,z).i==1);
 index.i=2;CHECK(call(v,"java/lang/String","indexOf","(II)I",u,face,index).i==-1);index.i='b';CHECK(call(v,"java/lang/String","indexOf","(I)I",u,index,z).i==3);
 dvm_ref main=call(v,loop,"getMainLooper","()Landroid/os/Looper;",0,z,z).l;CHECK(main!=0);CHECK(call(v,loop,"getMainLooper","()Landroid/os/Looper;",0,z,z).l==main);CHECK(call(v,loop,"myLooper","()Landroid/os/Looper;",0,z,z).l==main);CHECK(call(v,loop,"isCurrentThread","()Z",main,z,z).i==1);
 dvm_ref h=dvm_new_object(v,dvm_find_class_or_external(v,handler));union dvm_value l={.l=main};call(v,handler,"<init>","(Landroid/os/Looper;)V",h,l,z);CHECK(call(v,handler,"getLooper","()Landroid/os/Looper;",h,z,z).l==main);
 dvm_ref implicit=dvm_new_object(v,dvm_find_class_or_external(v,handler));call(v,handler,"<init>","()V",implicit,z,z);CHECK(call(v,handler,"getLooper","()Landroid/os/Looper;",implicit,z,z).l==main);
 CHECK(android_basics_call(v,handler,"post","(Ljava/lang/Runnable;)Z",h,&z,1,&out));CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);CHECK(!android_basics_call(v,loop,"loop","()V",0,NULL,0,&out));
 struct thread_case test={.v=v,.looper=main};pthread_t thread;int created=pthread_create(&thread,NULL,worker,&test);CHECK(created==0);if(!created){thread_runtime_join_host(v,thread);CHECK(test.pass);}
 call(v,handler,"<init>","(Landroid/os/Looper;)V",h,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 CHECK(android_basics_handler_selftest(v,checks));CHECK(!dvm_exception(v));sdk_trace_fprintf(stderr,"ANDROID_SELFTEST %s custom_charsequence=1 utf16=1 text_equals=1 color_parse=1 vm_looper_identity=1 handler_binding=1 main_scheduler=1 process_identity=1 permission_check=1 system_load_library=1\n",okay?"PASS":"FAIL");return okay;
}
