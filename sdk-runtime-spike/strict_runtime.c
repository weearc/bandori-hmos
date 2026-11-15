/* Diagnostic framework boundary. Missing app-facing APIs fail explicitly.
 * Empty teardown/tick functions concern services never installed by this runner.
 * No login, signature, network or Android UI result is synthesized. */
#include "dvm/dvm_internal.h"
#include "portable.h"
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#include "java_basics.h"
#include "android_basics.h"
#include "guest_profile.h"
#include "collections.h"
#include "runtime_extra.h"

static _Thread_local char trace[131072];
static _Thread_local size_t trace_used;
static _Thread_local unsigned missing;
static _Thread_local bool details_truncated;
static pthread_mutex_t runner_lock=PTHREAD_MUTEX_INITIALIZER;
_Thread_local dvm_ref g_current_looper;
int sdk_trace_fprintf(FILE *f, const char *fmt, ...) {
 (void)f;
 bool verbose=!strncmp(fmt,"[dvm]",5)||!strncmp(fmt,"JAVA_",5)||!strncmp(fmt," on @",5);
 if(trace_used>98304&&verbose){
  if(!details_truncated){details_truncated=true;sdk_trace_fprintf(f,"TRACE_TRUNCATED verbose details capped; stage summaries and missing APIs retained\n");}
  return 0;
 }
 if(!verbose&&trace_used&&trace[trace_used-1]!='\n'&&trace_used<sizeof trace-1){trace[trace_used++]='\n';trace[trace_used]=0;}
 va_list a; va_start(a,fmt);
 int n=vsnprintf(trace+trace_used,sizeof(trace)-trace_used,fmt,a); va_end(a);
 if(n>0) trace_used += (size_t)n < sizeof(trace)-trace_used ? (size_t)n : sizeof(trace)-trace_used-1;
 return n;
}
int sdk_trace_fputc(int ch,FILE *f){if(!details_truncated)sdk_trace_fprintf(f,"%c",ch);return ch;}
static bool external(void *u,struct dvm *vm,const char *c,const char *m,const char *s,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out) {
 (void)u;(void)self;(void)a;(void)n;(void)out;
 /* java.lang.Object has no superclass constructor body or observable fields. */
 if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"<init>")&&!strcmp(s,"()V")) return true;
 /* Android Handler.handleMessage is intentionally an empty base callback. */
 if(!strcmp(c,"android/os/Handler")&&!strcmp(m,"handleMessage")&&!strcmp(s,"(Landroid/os/Message;)V")) return true;
 if(runtime_extra_call(vm,c,m,s,self,a,n,out)||collections_call(vm,c,m,s,self,a,n,out)||java_basics_call(vm,c,m,s,self,a,n,out)||android_basics_call(vm,c,m,s,self,a,n,out)||guest_profile_call(vm,c,m,s,out)){
  sdk_trace_fprintf(stderr,"JAVA_API %s.%s%s\n",c,m,s);return true;
 }
 sdk_trace_missing_api(c,m,s);
 sdk_trace_fprintf(stderr,"BOUNDARY method caller=%s.%s\n",vm->cur_method&&vm->cur_method->cls?vm->cur_method->cls->name:"?",vm->cur_method?vm->cur_method->name:"?");
 return false;
}
/* Ownership guards must not trust a replacement extension solely because it
 * implements a familiar Java method name. This certifies our dispatcher. */
bool leak_scope_external_is_trusted(struct dvm *vm){return vm&&vm->hooks.call_external==external;}
static bool get_static(void *u,struct dvm *v,const char *c,const char *f,const char *t,union dvm_value *o) {
 (void)u;
 if(java_basics_static(v,c,f,t,o)||guest_profile_static(v,c,f,t,o)){
  sdk_trace_fprintf(stderr,"JAVA_STATIC %s.%s:%s\n",c,f,t);return true;
 }
 ++missing;
 sdk_trace_fprintf(stderr,"MISSING_STATIC %s.%s:%s\n",c,f,t);
 sdk_trace_fprintf(stderr,"BOUNDARY static caller=%s.%s\n",v->cur_method&&v->cur_method->cls?v->cur_method->cls->name:"?",v->cur_method?v->cur_method->name:"?");return false;
}
bool dvm_platform_class_exists(const char *d) {
 const char *collections[]={"Ljava/util/ArrayList;","Ljava/util/ArrayList$Itr;","Ljava/util/HashMap;","Ljava/util/List;","Ljava/util/Map;","Ljava/util/Collection;","Ljava/lang/Iterable;","Ljava/util/RandomAccess;","Ljava/lang/Cloneable;","Ljava/io/Serializable;","Ljava/util/AbstractCollection;","Ljava/util/AbstractList;","Ljava/util/AbstractMap;"};
 for(size_t i=0;i<sizeof collections/sizeof collections[0];++i)if(!strcmp(d,collections[i]))return true;
 const char *known[]={"Ljava/lang/Object;","Ljava/lang/Class;","Ljava/lang/String;","Ljava/lang/Void;","Ljava/lang/Boolean;","Ljava/lang/Byte;","Ljava/lang/Character;","Ljava/lang/Short;","Ljava/lang/Integer;","Ljava/lang/Long;","Ljava/lang/Float;","Ljava/lang/Double;","Ljava/lang/ThreadLocal;","Ljava/util/concurrent/atomic/AtomicBoolean;","Ljava/util/concurrent/CopyOnWriteArrayList;","Ljava/util/concurrent/CopyOnWriteArrayList$COWIterator;","Ljava/util/Iterator;"};
 for(size_t i=0;i<sizeof known/sizeof known[0];++i)if(!strcmp(d,known[i]))return true;
 return false;
}
void sdk_trace_unresolved_field(const char *c,const char *f,const char *t){
 ++missing;sdk_trace_fprintf(stderr,"MISSING_FIELD %s.%s:%s\n",c?c:"?",f?f:"?",t?t:"?");
}
void sdk_trace_missing_api(const char *c,const char *m,const char *s){++missing;sdk_trace_fprintf(stderr,"MISSING_API %s.%s%s\n",c,m,s);}
void dvm_runtime_install(struct dvm *v) {android_basics_vm_start(v);collections_vm_start(v);runtime_extra_vm_start(v);}
struct dvm_class *dvm_runtime_define(struct dvm *v,const char *d) {(void)v;(void)d;return NULL;}
uint64_t dvm__current_thread_id(struct dvm *v) {(void)v;return (uint64_t)(uintptr_t)pthread_self();}
dvm_ref dvm__current_thread(struct dvm *v) {(void)v;return 0;}
bool dvm__logical_main_thread(struct dvm *v) {(void)v;return true;}
bool dvm__thread_take_interrupt(struct dvm *v) {(void)v;return false;}
void dvm__thread_set_wait(struct dvm *v,uintptr_t c) {(void)v;(void)c;}
void dvm__thread_set_state(struct dvm *v,dvm_ref t,enum dvm_thread_state s) {(void)v;(void)t;(void)s;}
bool dvm_proxy_try_invoke(struct dvm *v,dvm_ref r,struct dvm_class *c,const char *n,const char *s,const union dvm_value *a,int l,union dvm_value *o) {(void)v;(void)r;(void)c;(void)n;(void)s;(void)a;(void)l;(void)o;return false;}
bool dvm__queue_runnable_at(struct dvm *v,dvm_ref r,bool t,int64_t d) {(void)r;(void)t;(void)d;++missing;dvm__throw(v,"java/lang/UnsupportedOperationException","diagnostic runner has no scheduler");return false;}
void dvm_runtime_looper_log(struct dvm *v,dvm_ref l,bool b,dvm_ref r) {(void)v;(void)l;(void)b;(void)r;}
void dvm__main_queue_idle(struct dvm *v,bool d) {(void)v;(void)d;}
#define EMPTY(name) void name(struct dvm *v) {(void)v;}
EMPTY(dvm__network_tick) EMPTY(dvm__ui_tick) EMPTY(dvm_alarm_poll)
EMPTY(dvm_alarm_finish) EMPTY(dvm_nsd_poll) EMPTY(dvm_nsd_finish)
EMPTY(dvm_prefs_finish) EMPTY(dvm_glsurface_finish) EMPTY(dvm_threads_finish)

struct sdk_stage{const char *label,*klass,*method,*sig;int mode;};
#include "sdk_stages.h"
__attribute__((visibility("default"))) const char *sdk_trace_run(const char *dir) {
 pthread_mutex_lock(&runner_lock); /* Upstream has a singleton GIL/TLS VM. */
 trace_used=0; trace[0]=0; missing=0;details_truncated=false;
 sdk_trace_fprintf(stderr,"SDK_TRACE marker=standard-batch-v7 upstream=8ed4b75368153f2b9d1174c986e50c8fe87aee59 strict=1 network=disabled\n");
 sdk_trace_fprintf(stderr,"SCOPE eight isolated original SDK paths; independent methods do not constitute complete startup or login\n");
 sdk_trace_fprintf(stderr,"BUDGET 100000 instructions per stage, 256 frames, no guest native loader or network hooks\n");
 sdk_trace_fprintf(stderr,"THREAD_MODEL VM main Looper belongs to interpreter creation thread (NAPI worker); no ArkTS/UI-thread binding, post or loop scheduler\n");
 if(!guest_profile_load(dir)){
  sdk_trace_fprintf(stderr,"RESULT BLOCKED explicit guest configuration required; SDK not executed\n");
  pthread_mutex_unlock(&runner_lock);return trace;
 }
 if(!java_basics_selftest()){
  sdk_trace_fprintf(stderr,"RESULT BLOCKED Java base-class semantic selftest failed\n");
  pthread_mutex_unlock(&runner_lock);return trace;
 }
 missing=0; /* Expected negative selftests are outside SDK stage accounting. */
 struct dvm_hooks h={.call_external=external,.get_external_static=get_static};
 for(size_t stage=0;stage<sizeof sdk_stages/sizeof sdk_stages[0];++stage){
 const struct sdk_stage *s=&sdk_stages[stage];const char *name=s->label;
 unsigned before=missing;
 sdk_trace_fprintf(stderr,"BEGIN_STAGE %s isolated_fresh_VM=1\n",name);
 struct dvm *v=dvm_create(&h);
 if(!v){sdk_trace_fprintf(stderr,"BLOCKED VM allocation/socketpair\n");break;}
 int count=dvm_add_apk_dir(v,dir);
 sdk_trace_fprintf(stderr,"DEX_FILES %d\n",count);
 if(count!=3){sdk_trace_fprintf(stderr,"BLOCKED expected all 3 original DEX files\n"); dvm_destroy(v);break;}
 dvm_set_trace(v,2); dvm_set_step_limit(v,100000);
 struct dvm_method *m=dvm_lookup(v,s->klass,s->method,s->sig);
 union dvm_value result={0};
 bool ok=false;
 if(!m)sdk_trace_fprintf(stderr,"STAGE_LOOKUP_FAILED %s.%s%s\n",s->klass,s->method,s->sig);
 else if(s->mode==1){
  union dvm_value arg={.l=android_basics_application(v)};
  sdk_trace_fprintf(stderr,"ARG stable diagnostic Application; context identity implemented, services unavailable\n");
  ok=arg.l&&dvm_call(v,m,0,&arg,1,&result);
 }else if(s->mode==2){
  dvm_ref object=dvm_new_object(v,dvm_find_class(v,s->klass));
  ok=object&&dvm_call(v,m,object,NULL,0,&result);
 }else if(s->mode==3){
  struct dvm_method *setup=dvm_lookup(v,"com/base/trackingdata/constants/GlobeContext","setGlobeContext","(Landroid/content/Context;)V");
  union dvm_value arg={.l=android_basics_application(v)},ignored={0};
  bool ready=setup&&arg.l&&dvm_call(v,setup,0,&arg,1,&ignored);
  sdk_trace_fprintf(stderr,"STAGE_SETUP original GlobeContext.setGlobeContext returned=%d missing=%u\n",ready,missing-before);
  uint64_t spent=dvm_instructions(v);
  if(ready&&missing==before&&spent<100000){dvm_set_step_limit(v,100000-spent);ok=dvm_call(v,m,0,NULL,0,&result);}
 }else {ok=dvm_call(v,m,0,NULL,0,&result);
 }
 sdk_trace_fprintf(stderr,"END_STAGE %s returned=%d instructions=%"PRIu64" missing=%u\n",name,ok,dvm_instructions(v),missing-before);
 if(dvm_exception(v)){char why[2048];dvm_describe_exception(v,dvm_exception(v),why,sizeof why);sdk_trace_fprintf(stderr,"EXCEPTION %s\n",why);}
 sdk_trace_fprintf(stderr,"RESULT %s; SDK initialization and login remain unverified\n",ok&&missing==before?"isolated method completed":"BLOCKED at explicit runtime boundary");
 dvm_destroy(v);
 }
 sdk_trace_fprintf(stderr,"BATCH_COMPLETE marker=standard-batch-v7 stages=%zu total_missing=%u SDK_init_and_login=unverified\n",sizeof sdk_stages/sizeof sdk_stages[0],missing);
 pthread_mutex_unlock(&runner_lock);return trace;
}
