/* Async host commands are drained only by the live VM owner. No SDK replacement. */
#include "dvm/dvm_internal.h"
#include "gc_runtime.h"
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
extern bool activity_runtime_probe_context(struct dvm *,const char *,dvm_ref *,dvm_ref *);
extern bool activity_runtime_task4_component_probe(struct dvm *,dvm_ref,const char *);
extern void activity_runtime_task5_minimal_layout(struct dvm *);
extern bool json_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static pthread_mutex_t panel_lock=PTHREAD_MUTEX_INITIALIZER;
static void (*emit_host)(const char *);
static struct dvm *live;
static unsigned long session;
static int pending,busy,required,ready,prepared,timed_out,sdk_callback;
static dvm_ref panel_activity,panel_presenter,panel_security;
static uint64_t submitted_ms;
static char phase[64]="unavailable",last_error[650];
static char request[129],account[513],password[1025],last[4096];
static char required_activity[160];
static int sdk_saved_known,sdk_saved_present,sdk_saved_refresh=1,saved_pending;
static char sdk_saved_name[513];
static uint64_t init_failure_notice_ms;
/* Exact SDK entrypoints only; normalize Android dot/slash class notation. */
static int panel_login_class(const char *cls,char *out,size_t cap){
 if(!cls||strlen(cls)>=cap)return 0;
 size_t n=0;for(;cls[n];n++)out[n]=cls[n]=='.'?'/':cls[n];out[n]=0;
 return !strcmp(out,"com/gsc/account_login/AccountLoginActivity")||
        !strcmp(out,"com/gsc/phone_login/PhoneLoginActivity")||
        !strcmp(out,"com/gsc/auto_login/AutoLoginActivity")||
        !strcmp(out,"com/gsc/login_record/LoginRecordActivity");
}
static void path_home_selftest(void){
 char name[160];int account_ok=panel_login_class("com.gsc.account_login.AccountLoginActivity",name,sizeof name);
 int phone_ok=panel_login_class("com.gsc.phone_login.PhoneLoginActivity",name,sizeof name)&&!strcmp(name,"com/gsc/phone_login/PhoneLoginActivity");
 int slash_ok=panel_login_class("com/gsc/phone_login/PhoneLoginActivity",name,sizeof name);
 int reject_ok=!panel_login_class("com/gsc/agreement/AgreementWebActivity",name,sizeof name)&&!panel_login_class(NULL,name,sizeof name);
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/path-home-selftest.txt","w");
 if(f){fprintf(f,"PATH_HOME_SELFTEST account=%d phone=%d slash=%d reject=%d credentials_submitted=0 network=0\n",account_ok,phone_ok,slash_ok,reject_ok);fclose(f);}
}
static _Thread_local int observing,preparing;
static struct {dvm_ref body;uint64_t body_generation;int status,emitted;char url[768];} responses[16];
static unsigned response_next;
static dvm_ref captcha_web,captcha_interface;static char captcha_name[128],captcha_payload[8193],captcha_method[32];static int captcha_pending,captcha_active;
/* The registry owns no borrowed object pointer. All slots below remain stable
 * dvm_ref IDs. Callback order is GIL -> panel_lock; host command entrypoints
 * never acquire the GIL while holding panel_lock. */
static struct dvm *panel_gc_vm;
/* Diagnostic observer only: it must not extend SDK object lifetime. */
static struct {struct dvm *vm;dvm_ref object;uint64_t generation;unsigned records;} init_common;
static bool panel_common_identity(struct dvm *v){
 struct dvm_object *o=init_common.vm==v&&init_common.object?dvm__obj(v,init_common.object):NULL;
 if(o&&init_common.generation&&o->identity_generation==init_common.generation)return true;
 init_common.object=0;init_common.generation=0;return false;
}
static bool panel_response_identity(struct dvm *v,unsigned i){
 struct dvm_object *o=responses[i].body?dvm__obj(v,responses[i].body):NULL;
 if(o&&responses[i].body_generation&&o->identity_generation==responses[i].body_generation)return true;
 responses[i].body=0;responses[i].body_generation=0;return false;
}
static bool panel_gc_strong(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 if(!v||!visit||context!=&panel_gc_vm||!dvm_gil_held(v))return false;
 pthread_mutex_lock(&panel_lock);
 if(live==v){
  dvm_ref *slots[]={&panel_activity,&panel_presenter,&panel_security,&captcha_web,&captcha_interface};
  for(unsigned i=0;i<sizeof slots/sizeof slots[0];i++)if(*slots[i])visit(slots[i],user);
 }
 pthread_mutex_unlock(&panel_lock);return true;
}
static bool panel_gc_weak(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 if(!v||!visit||context!=&panel_gc_vm||!dvm_gil_held(v))return false;
 pthread_mutex_lock(&panel_lock);
 if(live==v)for(unsigned i=0;i<sizeof responses/sizeof responses[0];i++)
  if(panel_response_identity(v,i)){visit(&responses[i].body,user);if(!responses[i].body)responses[i].body_generation=0;}
 if(init_common.vm==v&&panel_common_identity(v)){
  visit(&init_common.object,user);if(!init_common.object)init_common.generation=0;
 }
 pthread_mutex_unlock(&panel_lock);return true;
}
static bool panel_gc_bind(struct dvm *v){
 if(!v||!dvm_gil_held(v))return false;
 if(panel_gc_vm==v)return true;
 if(panel_gc_vm)return false; /* singleton owner cannot switch without unbind */
 if(!dvm_gc_root_provider_add(v,"login-panel/strong",DVM_GC_ROOT_STRONG,panel_gc_strong,&panel_gc_vm))return false;
 if(!dvm_gc_root_provider_add(v,"login-panel/response-observer",DVM_GC_ROOT_WEAK,panel_gc_weak,&panel_gc_vm)){
  dvm_gc_root_provider_remove(v,"login-panel/strong",&panel_gc_vm);return false;
 }
 panel_gc_vm=v;return true;
}
static void panel_gc_unbind(struct dvm *v){
 if(panel_gc_vm!=v)return;
 if(init_common.vm==v)memset(&init_common,0,sizeof init_common);
 dvm_gc_root_provider_remove(v,"login-panel/response-observer",&panel_gc_vm);
 dvm_gc_root_provider_remove(v,"login-panel/strong",&panel_gc_vm);panel_gc_vm=NULL;
}
static uint64_t monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
#include "task6j_diagnosis.inc"
static void wipe(void *p,size_t n){volatile unsigned char *b=p;while(n--)*b++=0;}
static void captcha_reset_locked(void){captcha_web=captcha_interface=0;captcha_name[0]=captcha_method[0]=0;captcha_pending=captcha_active=0;wipe(captcha_payload,sizeof captcha_payload);}
static void quote(const char *s,char *out,size_t cap){size_t n=0;if(cap<3)return;out[n++]='"';for(;s&&*s&&n+7<cap;s++){unsigned char c=*s;if(c=='"'||c=='\\'){out[n++]='\\';out[n++]=c;}else if(c<32){snprintf(out+n,7,"\\u%04x",c);n+=6;}else out[n++]=c;}out[n++]='"';out[n]=0;}
static void event_locked(const char *text){if(emit_host)emit_host(text);FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/login-panel-events.jsonl","a");if(f){fprintf(f,"%s\n",text);fclose(f);}}
/* Actual original title state only. The host presents progress without
 * invoking business callbacks or changing the Unity scene/surface. */
void login_panel_title_progress(int process,uint64_t mono_ms){
 char event[256];snprintf(event,sizeof event,"{\"type\":\"game/progress\",\"process\":%d,\"monoMs\":%llu}",process,(unsigned long long)mono_ms);
 pthread_mutex_lock(&panel_lock);event_locked(event);pthread_mutex_unlock(&panel_lock);
}
/* Present the real final SDK rejection; do not synthesize init/consent or
 * change the game's retry/state machine. Debounce repeated original retries. */
static void title_init_failure(struct dvm *v,struct dvm_method *m,const uint32_t *args,int count,int ok){
 if(!ok||v->exception||!args||count!=1||strcmp(m->cls->name,"com/bilibili/star/bili/Sdk$4$1")||
    strcmp(m->name,"onFailed")||strcmp(m->sig,"(Lcom/bsgamesdk/android/callbacklistener/BSGameSdkError;)V"))return;
 dvm_ref saved=v->exception;bool parked=v->parked;uint64_t steps=v->call_steps;
 observing=1;union dvm_value code={0};struct dvm_object *o=dvm__obj(v,args[0]);
 struct dvm_method *getter=o?dvm_find_method(v,o->cls,"getErrorCode","()I"):NULL;
 bool got=getter&&dvm_call(v,getter,args[0],NULL,0,&code)&&!v->exception;
 if(got&&code.i==3004){uint64_t now=monotonic_ms();pthread_mutex_lock(&panel_lock);
  if(live==v&&(!init_failure_notice_ms||now-init_failure_notice_ms>=10000)){
   init_failure_notice_ms=now;event_locked("{\"type\":\"game/init-required\",\"code\":\"3004\",\"source\":\"original_sdk_onFailed_return\"}");
  }pthread_mutex_unlock(&panel_lock);
 }
 dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;observing=0;
}
static void state_locked(const char *,int,const char *);
static void webview_panel_interface(struct dvm *,dvm_ref,dvm_ref,const char *);
void login_panel_web_interface(struct dvm *v,dvm_ref web,dvm_ref object,const char *name){
 if(!panel_gc_bind(v)){dvm__throw(v,"java/lang/OutOfMemoryError","login root registration failed");return;}
 webview_panel_interface(v,web,object,name);
 if(dvm_exception(v))return;
 pthread_mutex_lock(&panel_lock);if(live==v&&name){
  bool match=web==captcha_web&&!strcmp(name,captcha_name);
  if(object&&(match||(!captcha_active&&!captcha_pending))){captcha_web=web;captcha_interface=object;snprintf(captcha_name,sizeof captcha_name,"%s",name);}
  else if(!object&&match)captcha_reset_locked();
 }pthread_mutex_unlock(&panel_lock);
}
bool login_panel_web_url(struct dvm *v,dvm_ref web,const char *url){
 int handled=0;pthread_mutex_lock(&panel_lock);if(live==v&&busy&&web==captcha_web&&captcha_interface&&url&&(!strncmp(url,"https://",8)||!strncmp(url,"http://",7))){
 handled=1;captcha_active=1;char u[12000],r[800],name[400],event[14000];quote(url,u,sizeof u);quote(request,r,sizeof r);quote(captcha_name,name,sizeof name);
 snprintf(event,sizeof event,"{\"type\":\"login/captcha\",\"webRef\":%u,\"url\":%s,\"interfaceName\":%s,\"sessionId\":\"%lu\",\"requestId\":%s}",web,u,name,session,r);if(emit_host)emit_host(event);
 snprintf(event,sizeof event,"{\"type\":\"login/captcha\",\"urlLength\":%zu,\"interfaceName\":%s,\"sessionId\":\"%lu\",\"requestId\":%s}",strlen(url),name,session,r);FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/login-panel-events.jsonl","a");if(f){fprintf(f,"%s\n",event);fclose(f);}state_locked("waiting_captcha",1,NULL);
 }pthread_mutex_unlock(&panel_lock);return handled!=0;
}
__attribute__((visibility("default"))) int login_panel_captcha_result(const char *id,const char *payload,const char *method){
 if(!id||!payload||strlen(payload)>8192||!method||(strcmp(method,"finishWithResult")&&strcmp(method,"goBackWithResult")))return -2;pthread_mutex_lock(&panel_lock);int rc=-1;
 if(live&&captcha_active&&captcha_interface&&!strcmp(request,id)&&!captcha_pending){snprintf(captcha_payload,sizeof captcha_payload,"%s",payload);snprintf(captcha_method,sizeof captcha_method,"%s",method);captcha_pending=1;captcha_active=0;rc=0;state_locked("captcha_verifying",1,NULL);}pthread_mutex_unlock(&panel_lock);return rc;
}
static void state_locked(const char *next,int is_busy,const char *error){char r[800],e[1400],s[3000],p[64],err[650];snprintf(p,sizeof p,"%s",next);snprintf(err,sizeof err,"%s",error?error:"");strcpy(phase,p);strcpy(last_error,err);quote(request,r,sizeof r);quote(last_error,e,sizeof e);busy=is_busy;snprintf(s,sizeof s,"{\"type\":\"login/state\",\"sessionId\":\"%lu\",\"requestId\":%s,\"phase\":\"%s\",\"ready\":%s,\"busy\":%s,\"timedOut\":%s,\"error\":%s}",session,r,phase,ready?"true":"false",busy?"true":"false",timed_out?"true":"false",e);event_locked(s);}
static void timeout_locked(void){if(busy&&submitted_ms&&!last[0]&&!sdk_callback&&!timed_out&&monotonic_ms()-submitted_ms>=15000){char error[650];snprintf(error,sizeof error,"submit timeout (no response); last_phase=%.63s; last_exception=%.480s",phase,last_error[0]?last_error:"none observed");timed_out=1;state_locked("timeout",1,error);}}
static void required_locked(void){char s[1000];snprintf(s,sizeof s,"{\"type\":\"login/required\",\"sessionId\":\"%lu\",\"ready\":%s,\"activityClass\":\"%s\",\"reason\":\"activity_launch_requested\"}",session,ready?"true":"false",required_activity);event_locked(s);}
__attribute__((visibility("default"))) void login_panel_set_event_host(void(*fn)(const char *)){pthread_mutex_lock(&panel_lock);emit_host=fn;pthread_mutex_unlock(&panel_lock);}
__attribute__((visibility("default"))) int login_panel_get_state(char *out,size_t cap){pthread_mutex_lock(&panel_lock);timeout_locked();char r[800],e[1400],name[3200];quote(request,r,sizeof r);quote(last_error,e,sizeof e);quote(sdk_saved_name,name,sizeof name);snprintf(out,cap,"{\"sessionId\":\"%lu\",\"ready\":%s,\"activityReady\":%s,\"securityReady\":%s,\"required\":%s,\"busy\":%s,\"activeRequestId\":%s,\"phase\":\"%s\",\"timedOut\":%s,\"error\":%s,\"sdkSavedKnown\":%s,\"sdkSavedPresent\":%s,\"sdkSavedName\":%s}",session,ready?"true":"false",panel_activity&&panel_presenter?"true":"false",panel_security?"true":"false",required?"true":"false",busy?"true":"false",r,phase,timed_out?"true":"false",e,sdk_saved_known?"true":"false",sdk_saved_present?"true":"false",name);pthread_mutex_unlock(&panel_lock);return 0;}
__attribute__((visibility("default"))) int login_panel_submit(const char *id,const char *user,const char *pwd){
 if(!id||!*id||strlen(id)>128||!user||!*user||strlen(user)>512||!pwd||!*pwd||strlen(pwd)>1024)return -2;
 pthread_mutex_lock(&panel_lock);if(!live||!ready||busy){pthread_mutex_unlock(&panel_lock);return -1;}
 strcpy(request,id);strcpy(account,user);strcpy(password,pwd);pending=1;last[0]=0;timed_out=0;sdk_callback=0;submitted_ms=monotonic_ms();memset(responses,0,sizeof responses);state_locked("queued",1,NULL);pthread_mutex_unlock(&panel_lock);return 0;
}
__attribute__((visibility("default"))) int login_panel_saved_login(const char *id){
 if(!id||!*id||strlen(id)>128)return -2;
 pthread_mutex_lock(&panel_lock);
 if(!live||!ready||busy||!sdk_saved_present){pthread_mutex_unlock(&panel_lock);return -1;}
 strcpy(request,id);saved_pending=1;last[0]=0;timed_out=0;sdk_callback=0;submitted_ms=monotonic_ms();
 state_locked("queued",1,NULL);pthread_mutex_unlock(&panel_lock);return 0;
}
bool login_panel_uses_arkts(const char *cls){char normalized[160];return !preparing&&panel_login_class(cls,normalized,sizeof normalized);}
__attribute__((visibility("default"))) void login_panel_required(const char *cls){
 char normalized[160];if(preparing||!panel_login_class(cls,normalized,sizeof normalized))return;
 pthread_mutex_lock(&panel_lock);required=1;sdk_saved_refresh=1;strcpy(required_activity,normalized);required_locked();pthread_mutex_unlock(&panel_lock);
}
static bool invoke(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *a,int n,union dvm_value *q){struct dvm_object*o=dvm__obj(v,self);struct dvm_method*m=o?dvm_find_method(v,o->cls,name,sig):NULL;return m&&dvm_call(v,m,self,a,n,q)&&!dvm_exception(v);}
static bool sdk_saved_model(struct dvm *v,union dvm_value *q){
 struct dvm_class *c=dvm_find_class(v,"com/gsc/base/utils/UserInfoUtils");
 struct dvm_method *m=c?dvm_find_method(v,c,"isLogin","()Lcom/gsc/base/model/UserInfoModel;"):NULL;
 return m&&dvm_call(v,m,0,NULL,0,q)&&!dvm_exception(v);
}
static void sdk_saved_poll(struct dvm *v){
 pthread_mutex_lock(&panel_lock);int refresh=sdk_saved_refresh,run=saved_pending;sdk_saved_refresh=0;saved_pending=0;pthread_mutex_unlock(&panel_lock);
 if(!refresh&&!run)return;
 dvm_ref previous=v->exception;uint64_t steps=v->call_steps;bool parked=v->parked;
 v->exception=0;v->call_steps=0;observing=1;union dvm_value q={0},name={0};
 bool ok=sdk_saved_model(v,&q);dvm_ref model=ok?q.l:0;if(model)dvm_pin(v,model);
 char text[513]="";
 if(model){
  ok=dvm_get_field(v,model,"username","Ljava/lang/String;",&name);
  if(ok&&!name.l)ok=dvm_get_field(v,model,"uname","Ljava/lang/String;",&name);
  const char *s=ok&&name.l?dvm_string_utf8(v,name.l):NULL;if(s)snprintf(text,sizeof text,"%s",s);
 }
 pthread_mutex_lock(&panel_lock);sdk_saved_known=ok;sdk_saved_present=ok&&model;sdk_saved_name[0]=0;
 if(ok&&model)snprintf(sdk_saved_name,sizeof sdk_saved_name,"%s",text);
 pthread_mutex_unlock(&panel_lock);
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/captcha-home-sdk-account.txt","a");
 if(f){fprintf(f,"pid=%d mono_ms=%llu source=original_UserInfoUtils.isLogin query_ok=%d model_present=%d name_length=%zu saved_login_requested=%d exception=%u secrets_exported=0\n",getpid(),(unsigned long long)monotonic_ms(),ok,model!=0,strlen(text),run,v->exception);fclose(f);}
 observing=0;
 if(run){
  extern bool activity_runtime_arkts_saved_login(struct dvm *,dvm_ref);
  bool started=ok&&model&&activity_runtime_arkts_saved_login(v,model);
  if(!started){pthread_mutex_lock(&panel_lock);state_locked("failed",0,"SDK saved login unavailable; choose another account");pthread_mutex_unlock(&panel_lock);}
 }
 if(model)dvm_unpin(v,model);
 dvm_clear_exception(v);v->exception=previous;v->call_steps=steps;v->parked=parked;
}
#include "webview_panel.inc"
#include "task6l_io_selftest.inc"
/* Pure local cancellation-model round trip. Never invokes SDK listeners,
 * login, routes or network; fixture is not a server response. */
static void task6j_model_selftest(struct dvm *v){
 dvm_ref saved=v->exception;bool parked=v->parked;uint64_t steps=v->call_steps,limit=v->step_limit;v->exception=0;v->parked=false;v->call_steps=0;v->step_limit=100000;diag6j_fixture=1;
 const char *stage="class_lookup";union dvm_value q={0},a[3]={{0}};dvm_ref model=0,json=0,serialized=0;bool ok=false;
 struct dvm_class *mc=dvm_find_class(v,"com/gsc/base/model/CallBackModel"),*jc=dvm_find_class(v,"copy/google/json/JSON");
 if(!mc||!jc)goto done;model=dvm_new_object(v,mc);json=dvm_new_object(v,jc);stage="model_ctor";
 a[0].l=dvm_new_string(v,"6002");a[1].l=dvm_new_string(v,"\xe7\x94\xa8\xe6\x88\xb7\xe5\x8f\x96\xe6\xb6\x88\xe7\x99\xbb\xe5\xbd\x95");a[2].l=dvm_new_string(v,"");
 if(!model||!json||!invoke(v,model,"<init>","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",a,3,&q))goto done;
 stage="json_ctor";if(!invoke(v,json,"<init>","()V",NULL,0,&q))goto done;
 stage="toJson";a[0].l=model;if(!invoke(v,json,"toJson","(Ljava/lang/Object;)Ljava/lang/String;",a,1,&q))goto done;serialized=q.l;diag6j_string(v,serialized,"selftest.cancel.serialized");{struct dvm_object *s=diag6j_obj(v,serialized);if(s&&s->utf8&&s->utf8_len<256)diag6j_log(v,"kind=fixture_only_serialized literal=%s synthetic_local_model=1 no_server_data=1",s->utf8);}
 stage="fromJson";a[0].l=serialized;a[1].l=dvm_class_object(v,mc);if(!invoke(v,json,"fromJson","(Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/Object;",a,2,&q))goto done;diag6j_model(v,q.l,"selftest.cancel.parsed");
 stage="Integer.valueOf";a[0].l=diag6j_ref(v,q.l,"code");struct dvm_method *value=dvm_find_method(v,dvm_find_class_or_external(v,"java/lang/Integer"),"valueOf","(Ljava/lang/String;)Ljava/lang/Integer;");
 if(!value||!dvm_call(v,value,0,a,1,&q)||v->exception)goto done;stage="Integer.intValue";if(!invoke(v,q.l,"intValue","()I",NULL,0,&q))goto done;ok=q.i==6002;
 done: {struct dvm_object *e=diag6j_obj(v,v->exception);diag6j_log(v,"kind=cancel_model_selftest ok=%d stage=%s original_type=%s steps=%llu limit=100000 no_callback=1 no_network=1",ok,stage,e&&e->cls?e->cls->name:"none",(unsigned long long)v->call_steps);if(v->exception){char detail[700];dvm_describe_exception(v,v->exception,detail,sizeof detail);diag6j_log(v,"kind=fixture_only_exception detail=%s synthetic_local_model=1 no_server_data=1",detail);}}
 diag6j_fixture=0;
 v->exception=saved;v->parked=parked;v->call_steps=steps;v->step_limit=limit;
}
/* Observe SDK-internal callback dispatch only; never deliver a callback here. */
static bool callback_signature(struct dvm_method *m){
 return m&&m->sig&&((!strcmp(m->name,"onSuccess")&&!strcmp(m->sig,"(Landroid/os/Bundle;)V"))||
  ((!strcmp(m->name,"onError")||!strcmp(m->name,"onFailed"))&&
   (!strcmp(m->sig,"(Lcom/bsgamesdk/android/callbacklistener/BSGameSdkError;)V")||!strcmp(m->sig,"(Ljava/lang/String;Lcom/bsgamesdk/android/callbacklistener/BSGameSdkError;)V"))));
}
static void callback_evidence(struct dvm *v,struct dvm_method *m,const uint32_t *args,int count,const char *stage,int ok){
 if(!callback_signature(m)||!args||count<1)return;
 observing=1;int saved_errno=errno;dvm_ref saved=dvm_exception(v);bool parked=v->parked;uint64_t steps=v->call_steps;
 dvm_clear_exception(v);
 unsigned long callback_session;char callback_request[129];pthread_mutex_lock(&panel_lock);callback_session=session;strcpy(callback_request,request);pthread_mutex_unlock(&panel_lock);
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/sdk-callback.txt","a");
 if(f){
  dprintf(fileno(f),"SDK_CALLBACK time=%lld mono_ms=%llu session=%lu request=%s thread=%u class=%s method=%s sig=%s stage=%s ok=%d original_dispatch=1\n",(long long)time(NULL),(unsigned long long)monotonic_ms(),callback_session,callback_request,v->cur_thread,m->cls->name,m->name,m->sig,stage,ok);
  if(!strcmp(stage,"enter")){
   if(!strcmp(m->name,"onSuccess")){extern bool bundle_runtime_trace_keys(struct dvm *,dvm_ref,int);bundle_runtime_trace_keys(v,args[0],fileno(f));}
   else {dvm_ref error=args[count-1];union dvm_value code={0},text={0};bool a=invoke(v,error,"getErrorCode","()I",NULL,0,&code);bool b=a&&invoke(v,error,"getErrorMessage","()Ljava/lang/String;",NULL,0,&text);const char *message=b?dvm_string_utf8(v,text.l):NULL;dprintf(fileno(f),"SDK_CALLBACK_ERROR code_available=%d code=%d message=%.*s getters_ok=%d\n",a,code.i,500,message?message:"<unavailable>",b);}
   const struct dvm_call_frame *frame=v->invoke_frame;
   for(int n=0;frame&&n<12;frame=frame->previous,n++){const struct dvm_method *p=frame->method;if(p&&p->cls)dprintf(fileno(f),"SDK_CALLBACK_CALLER depth=%d class=%s method=%s sig=%s\n",n,p->cls->name,p->name,p->sig?p->sig:"");}
  }
  fclose(f);
 }
 dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;errno=saved_errno;observing=0;
}
void login_panel_poll(struct dvm *v,dvm_ref caller){
 /* Bounded read-only slices run only from this idle owner poll, never render. */
 extern void dvm_mem_idle_observe(struct dvm *);dvm_mem_idle_observe(v);
 if(!panel_gc_bind(v)||!webview_panel_gc_bind(v)){
  dvm__throw(v,"java/lang/OutOfMemoryError","login/Web root registration failed");return;
 }
 android_web_poll(v);
 webview_panel_poll(v);
 static struct dvm *mem_vm;static uint64_t mem_last;
 uint64_t mem_now=monotonic_ms();
 /* Optional removal of the diagnostic O(objects * classes) census, not of
  * collection or any game work. Keep its old behavior unless opted in. */
 static uint64_t census_control_last;static int census_suppressed;
 if(!census_control_last||mem_now-census_control_last>=1000){
  census_control_last=mem_now;const char *path="/data/storage/el2/base/haps/entry/files/sdkdex/multitouch-no-census";
  FILE *cf=fopen(path,"wx");if(cf){fputs("0\n",cf);fclose(cf);chmod(path,0666);}
  census_suppressed=0;cf=fopen(path,"r");if(cf){int flag=0;if(fscanf(cf,"%d",&flag)==1)census_suppressed=flag==1;fclose(cf);}
 }
 if(!census_suppressed&&(mem_vm!=v||mem_now-mem_last>=10000)){
  extern void dvm_gil_deep_host(struct dvm *,const char *,int);
  dvm_gil_deep_host(v,"login_panel_heap_census",1);
  mem_vm=v;mem_last=mem_now;
  FILE *mf=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/vm-memory.txt","a");
  if(mf){extern void vm_arena_diagnostic_write(int,const char *);vm_arena_diagnostic_write(fileno(mf),"panel_poll");
   struct {struct dvm_class *cls;uint32_t count;size_t bytes;} totals[512]={0};unsigned used=0;
   for(dvm_ref r=1;r<=v->heap_size;r++){struct dvm_object *o=dvm__obj(v,r);if(!o||!o->cls)continue;unsigned j=0;for(;j<used&&totals[j].cls!=o->cls;j++);if(j==used){if(used==512)continue;totals[used++].cls=o->cls;}totals[j].count++;totals[j].bytes+=sizeof *o+(o->slots?o->cls->islots*sizeof(union dvm_value):0)+(o->utf8?o->utf8_len+1:0);}
   dprintf(fileno(mf),"HEAP_CENSUS pid=%d time=%lld mono_ms=%llu objects=%u classes_counted=%u partial_bytes=object_slots_utf8_only\n",getpid(),(long long)time(NULL),(unsigned long long)mem_now,v->heap_size,used);
   for(unsigned k=0;k<5&&k<used;k++){unsigned best=k;for(unsigned j=k+1;j<used;j++)if(totals[j].bytes>totals[best].bytes)best=j;if(best!=k){struct dvm_class *c=totals[k].cls;uint32_t n=totals[k].count;size_t b=totals[k].bytes;totals[k]=totals[best];totals[best].cls=c;totals[best].count=n;totals[best].bytes=b;}dprintf(fileno(mf),"HEAP_TOP rank=%u class=%s objects=%u partial_bytes=%zu\n",k+1,totals[k].cls->name,totals[k].count,totals[k].bytes);}
   fclose(mf);
  }
  dvm_gil_deep_host(v,"login_panel_heap_census",0);
 }
 pthread_mutex_lock(&panel_lock);int deliver=live==v&&captcha_pending;char payload[8193],method[32];dvm_ref target=captcha_interface;if(deliver){strcpy(payload,captcha_payload);strcpy(method,captcha_method);wipe(captcha_payload,sizeof captcha_payload);captcha_pending=0;}pthread_mutex_unlock(&panel_lock);
 if(deliver){dvm_ref saved=dvm_exception(v);bool parked=v->parked;uint64_t steps=v->call_steps;dvm_clear_exception(v);v->call_steps=0;union dvm_value a={.l=dvm_new_string(v,payload)},q={0};wipe(payload,sizeof payload);bool ok=invoke(v,target,method,"(Ljava/lang/String;)V",&a,1,&q);bool cancelled=ok&&!strcmp(method,"goBackWithResult");pthread_mutex_lock(&panel_lock);char dispatch_event[400];snprintf(dispatch_event,sizeof dispatch_event,"{\"type\":\"login/captcha-dispatch\",\"sessionId\":\"%lu\",\"requestId\":\"%s\",\"method\":\"%s\",\"ok\":%s}",session,request,method,ok?"true":"false");event_locked(dispatch_event);if(cancelled){captcha_reset_locked();submitted_ms=0;state_locked("ready",0,NULL);}else state_locked(ok?"captcha_verifying":"failed",1,ok?NULL:"captcha result dispatch failed");pthread_mutex_unlock(&panel_lock);dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;}
 pthread_mutex_lock(&panel_lock);if(live!=v){captcha_reset_locked();wipe(account,sizeof account);wipe(password,sizeof password);live=v;session=(unsigned long)time(NULL);memset(responses,0,sizeof responses);response_next=0;panel_activity=panel_presenter=panel_security=0;ready=prepared=timed_out=sdk_callback=pending=required=0;request[0]=last[0]=0;submitted_ms=0;state_locked("preparing",0,NULL);}int prepare=!prepared;prepared=1;timeout_locked();pthread_mutex_unlock(&panel_lock);
 if(prepare){
 pthread_mutex_lock(&panel_lock);sdk_saved_known=sdk_saved_present=saved_pending=0;sdk_saved_name[0]=0;sdk_saved_refresh=1;pthread_mutex_unlock(&panel_lock);
 path_home_selftest();
 extern void sqlite_runtime_column_name_selftest(struct dvm *);
 sqlite_runtime_column_name_selftest(v);
 extern bool collections_weak_map_selftest(struct dvm *);
 extern bool uri_runtime_selftest(struct dvm *);
 dvm_ref web_test_saved=v->exception;uint64_t web_test_steps=v->call_steps;bool web_test_parked=v->parked;
 v->exception=0;v->call_steps=0;v->parked=false;
 bool weak_map_ok=collections_weak_map_selftest(v);bool weak_map_exception=v->exception!=0;dvm_clear_exception(v);
 bool uri_ok=uri_runtime_selftest(v);bool uri_exception=v->exception!=0;dvm_clear_exception(v);
 union dvm_value uri_arg={.l=dvm_new_string(v,"https://example.invalid/a?b=c+d")},uri_result={0};
 dvm_ref uri_object=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/URI"));
 bool uri_dispatch_ok=uri_object&&invoke(v,uri_object,"<init>","(Ljava/lang/String;)V",&uri_arg,1,&uri_result)&&invoke(v,uri_object,"toString","()Ljava/lang/String;",NULL,0,&uri_result);
 const char *uri_result_text=uri_result.l?dvm_string_utf8(v,uri_result.l):NULL;uri_dispatch_ok=uri_dispatch_ok&&uri_result_text&&!strcmp(uri_result_text,"https://example.invalid/a?b=c+d")&&!v->exception;
 FILE *web_test_log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/webview-platform-selftest.txt","w");if(web_test_log){fprintf(web_test_log,"WEBVIEW_PLATFORM_SELFTEST weak_map=%d weak_map_exception=%d uri_suite=%d uri_exception=%d uri_virtual_toString=%d gc_tested=0\n",weak_map_ok,weak_map_exception,uri_ok,uri_exception,uri_dispatch_ok);fclose(web_test_log);}
 dvm_clear_exception(v);v->exception=web_test_saved;v->call_steps=web_test_steps;v->parked=web_test_parked;
 extern bool thread_runtime_memory_selftest(struct dvm *);
 thread_runtime_memory_selftest(v);
#ifdef LUNARIA_TASK6N_PERSISTENCE_PROBE
 extern void sqlite_runtime_persistence_probe(struct dvm *,dvm_ref);
 sqlite_runtime_persistence_probe(v,caller);
#endif
 task6l_io_selftest(v);
 task6j_model_selftest(v);
 extern bool dvm_task_profile_selftest(struct dvm *);
 dvm_task_profile_selftest(v);
 extern bool platform_timezone_selftest(struct dvm *);
 platform_timezone_selftest(v);
 dvm_ref platform_saved=v->exception;uint64_t platform_steps=v->call_steps;bool platform_parked=v->parked;v->exception=0;v->call_steps=0;v->parked=false;
 struct dvm_method *sleep_method=dvm_find_method(v,dvm_find_class_or_external(v,"java/lang/Thread"),"sleep","(J)V");union dvm_value sleep_arg={.j=10},platform_q={0};uint64_t sleep_start=monotonic_ms();
 bool sleep_ok=sleep_method&&(sleep_method->access&DEX_ACC_STATIC)&&dvm_call(v,sleep_method,0,&sleep_arg,1,&platform_q)&&!v->exception&&monotonic_ms()-sleep_start>=8;
 sleep_arg.j=-1;bool sleep_negative=sleep_method&&!dvm_call(v,sleep_method,0,&sleep_arg,1,&platform_q)&&v->exception;struct dvm_object *sleep_error=dvm__obj(v,v->exception);sleep_negative=sleep_negative&&sleep_error&&sleep_error->cls&&!strcmp(sleep_error->cls->name,"java/lang/IllegalArgumentException");v->exception=0;
 dvm_ref fragment=dvm_new_object(v,dvm_find_class_or_external(v,"android/app/Fragment")),bundle=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Bundle"));
 bool fragment_ok=fragment&&bundle&&invoke(v,fragment,"<init>","()V",NULL,0,&platform_q);union dvm_value fragment_arg={.l=bundle};fragment_ok=fragment_ok&&invoke(v,fragment,"setArguments","(Landroid/os/Bundle;)V",&fragment_arg,1,&platform_q)&&invoke(v,fragment,"getArguments","()Landroid/os/Bundle;",NULL,0,&platform_q)&&platform_q.l==bundle;
 FILE *platform_log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/init-platform-selftest.txt","w");if(platform_log){fprintf(platform_log,"INIT_PLATFORM_SELFTEST sleep_10ms=%d sleep_negative_exception=%d fragment_arguments_identity=%d\n",sleep_ok,sleep_negative,fragment_ok);fclose(platform_log);}v->exception=platform_saved;v->call_steps=platform_steps;v->parked=platform_parked;
 extern bool integer_runtime_selftest(struct dvm *);
 integer_runtime_selftest(v);
 extern bool uri_runtime_builder_selftest(struct dvm *);
 uri_runtime_builder_selftest(v);
 extern bool android_basics_interpolator_selftest(struct dvm *,unsigned *);
 unsigned interpolation_checks=0;bool interpolation_ok=android_basics_interpolator_selftest(v,&interpolation_checks);
 FILE *interpolation_log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/math-interpolator-selftest.txt","w");if(interpolation_log){fprintf(interpolation_log,"MATH_INTERPOLATOR_SELFTEST ok=%d checks=%u\n",interpolation_ok,interpolation_checks);fclose(interpolation_log);}
 extern bool string_runtime_okio_selftest(struct dvm *);
 string_runtime_okio_selftest(v);
 extern bool string_runtime_okio_read_selftest(struct dvm *);
 extern bool string_runtime_response_read_selftest(struct dvm *);
 string_runtime_okio_read_selftest(v);
 string_runtime_response_read_selftest(v);
 extern bool io_runtime_byte_array_input_selftest(struct dvm *);
 extern bool jni_throwable_pressure_selftest(struct dvm *);
 io_runtime_byte_array_input_selftest(v);
 extern bool io_runtime_input_stream_reader_selftest(struct dvm *);
 io_runtime_input_stream_reader_selftest(v);
 if(vm_pressure_selftest_gate("jni_throwable_pressure_selftest"))
  jni_throwable_pressure_selftest(v);
 extern bool jni_hashmap_dispatch_selftest(struct dvm *);
 jni_hashmap_dispatch_selftest(v);
 dvm_ref saved=dvm_exception(v);bool parked=v->parked;uint64_t steps=v->call_steps;dvm_clear_exception(v);v->call_steps=0;preparing=1;
 dvm_ref activity=0,presenter=0;const char *component="com/gsc/account_login/AccountLoginActivity";
 /* ArkTS owns login UI; initialize SDK backend without Android UI lifecycle. */
 union dvm_value q={0};bool ok=activity_runtime_probe_context(v,component,&activity,&presenter);
 if(!ok){
  /* Construct only the original SDK model/presenter; never call onCreate,
   * inflate a layout or mount an Android view. ArkTS owns all input nodes. */
  extern bool activity_runtime_arkts_login_backend(struct dvm *,dvm_ref *,dvm_ref *);
  ok=activity_runtime_arkts_login_backend(v,&activity,&presenter);
 }
 struct dvm_class *sc=dvm_find_class(v,"com/gsc/security/SecurityInterface");dvm_ref security=ok&&sc?dvm_new_object(v,sc):0;union dvm_value context={.l=activity};
 ok=ok&&security&&invoke(v,security,"<init>","()V",NULL,0,&q)&&invoke(v,security,"init","(Landroid/content/Context;)V",&context,1,&q);
 {FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/tap-home-selftest.txt","w");if(f){fprintf(f,"pid=%d sdk_activity_present=%d presenter_present=%d security_present=%d backend_ready=%d original_layout_inflated=0 credentials_submitted=0 result=%s\n",getpid(),activity!=0,presenter!=0,security!=0,ok,ok?"PASS":"FAIL");fclose(f);}}
 char error[650]="Activity/Presenter/security unavailable";if(!ok&&dvm_exception(v))dvm_describe_exception(v,dvm_exception(v),error,sizeof error);
 if(ok){dvm_pin(v,activity);dvm_pin(v,presenter);dvm_pin(v,security);}
 preparing=0;dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;
 pthread_mutex_lock(&panel_lock);if(ok){panel_activity=activity;panel_presenter=presenter;panel_security=security;ready=1;}state_locked(ok?"ready":"unavailable",0,ok?NULL:error);if(required&&ready)required_locked();pthread_mutex_unlock(&panel_lock);
 }
 sdk_saved_poll(v);
 pthread_mutex_lock(&panel_lock);int run=pending;pending=0;char user[513],pwd[1025];dvm_ref activity=panel_activity,presenter=panel_presenter,security=panel_security;bool ok=ready;if(run){strcpy(user,account);strcpy(pwd,password);wipe(password,sizeof password);wipe(account,sizeof account);}pthread_mutex_unlock(&panel_lock);if(!run)return;
 dvm_ref saved=dvm_exception(v);bool parked=v->parked;uint64_t steps=v->call_steps;dvm_clear_exception(v);v->call_steps=0;
 union dvm_value q={0};
 union dvm_value a[6]={{.l=dvm_new_string(v,user)},{.l=dvm_new_string(v,pwd)},{.l=security},{.l=0},{.i=0},{.l=activity}};
 wipe(pwd,sizeof pwd);wipe(user,sizeof user);
 pthread_mutex_lock(&panel_lock);state_locked("entry",1,NULL);pthread_mutex_unlock(&panel_lock);
 if(ok)ok=invoke(v,presenter,"a","(Ljava/lang/String;Ljava/lang/String;Lcom/gsc/base/service/ISecurityService;Lcom/gsc/base/model/CipherBaseResModel;ILandroid/app/Activity;)V",a,6,&q);
 if(!ok){char error[650]="Activity/Presenter/security unavailable";if(dvm_exception(v))dvm_describe_exception(v,dvm_exception(v),error,sizeof error);pthread_mutex_lock(&panel_lock);state_locked("failed",0,error);pthread_mutex_unlock(&panel_lock);}
 dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;
}
void login_panel_unbind(struct dvm *v){
 /* Never wait for the GIL while holding either panel/web mutex. */
 if(!v)return;bool owned=!dvm_gil_held(v);if(owned)dvm_gil_acquire(v);
 android_web_unbind(v);webview_panel_unbind(v);pthread_mutex_lock(&panel_lock);if(live==v){extern void dvm_lifetime_note(struct dvm *,const char *) __attribute__((weak));if(dvm_lifetime_note)dvm_lifetime_note(v,"login_panel_unbind");if(panel_activity)dvm_unpin(v,panel_activity);if(panel_presenter)dvm_unpin(v,panel_presenter);if(panel_security)dvm_unpin(v,panel_security);panel_activity=panel_presenter=panel_security=0;captcha_reset_locked();memset(responses,0,sizeof responses);response_next=0;live=NULL;ready=required=pending=prepared=timed_out=sdk_callback=0;submitted_ms=0;last[0]=0;wipe(password,sizeof password);wipe(account,sizeof account);state_locked("stopped",0,"VM session ended");}pthread_mutex_unlock(&panel_lock);
 panel_gc_unbind(v);if(owned)dvm_gil_release(v);
}
/* Diagnostic observations only: do not replace or short circuit any method. */
static void init_evidence(struct dvm *v,struct dvm_method *m,dvm_ref self,const uint32_t *args,int count,const char *stage,int ok){
 if(init_common.vm!=v){memset(&init_common,0,sizeof init_common);init_common.vm=v;}
 const char *c=m->cls->name;
 bool pub=!strcmp(c,"com/gsc/pub/GSCPubCommon");
 bool selected=pub||!strncmp(c,"com/gsc/pub/GSCPubCommon$",sizeof("com/gsc/pub/GSCPubCommon$")-1)||!strcmp(c,"com/gsc/base/c")||!strncmp(c,"com/gsc/base/c$",sizeof("com/gsc/base/c$")-1)||strstr(c,"InitAgreementRouteService")||strstr(c,"BaseRouteProcessService")||!strcmp(c,"java/util/Observable")||strstr(c,"CallbackManager")||(!strcmp(c,"android/os/Handler")&&!strcmp(m->name,"postAtFrontOfQueue"))||(!strcmp(c,"java/util/concurrent/atomic/AtomicBoolean")&&(!strcmp(m->name,"set")||!strcmp(m->name,"lazySet")));
 if(!selected||init_common.records++>=1200)return;
 if(!panel_gc_bind(v)){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","SDK observer GC roots");return;}
 if(pub&&self){struct dvm_object *o=dvm__obj(v,self);init_common.object=o?self:0;init_common.generation=o?o->identity_generation:0;}
 dvm_ref common=panel_common_identity(v)?init_common.object:0;
 int saved_errno=errno;dvm_ref saved=v->exception;bool parked=v->parked;uint64_t steps=v->call_steps;v->exception=0;
 union dvm_value flag={0},value={0};bool got=common&&dvm_get_field(v,common,"a","Ljava/util/concurrent/atomic/AtomicBoolean;",&flag);
 extern bool java_basics_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
 bool read=got&&flag.l&&java_basics_call(v,"java/util/concurrent/atomic/AtomicBoolean","get","()Z",flag.l,NULL,0,&value)&&!v->exception;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/sdk-init-state.txt","a");
 if(f){dprintf(fileno(f),"SDK_INIT pid=%d time=%lld mono_ms=%llu thread=%u class=%s method=%s sig=%s self=%u stage=%s ok=%d common=%u atomic=%u initialized=%d exception=%u",getpid(),(long long)time(NULL),(unsigned long long)monotonic_ms(),v->cur_thread,c,m->name,m->sig,self,stage,ok,common,flag.l,read?value.i:-1,saved);
 struct dvm_object *receiver=dvm__obj(v,self);if(receiver&&receiver->cls)dprintf(fileno(f)," receiver_class=%s",receiver->cls->name);
 if(saved){struct dvm_object *error=dvm__obj(v,saved);const char *ec=error&&error->cls?error->cls->name:"unknown";dprintf(fileno(f)," exception_class=%s",ec);if(strstr(ec,"NoSuch")||strstr(ec,"NoClassDefFound")||strstr(ec,"OutOfMemory")||strstr(ec,"NullPointer")){char detail[512]={0};dvm_describe_exception(v,saved,detail,sizeof detail);dprintf(fileno(f)," detail=%s",detail);}}
 if(!strcmp(c,"java/util/concurrent/atomic/AtomicBoolean")&&count&&(!strcmp(m->name,"set")||!strcmp(m->name,"lazySet")))dprintf(fileno(f)," boolean_arg=%u",args[0]);
 if(!strcmp(m->name,"callback")&&count){union dvm_value r={0};if(dvm_get_field(v,args[0],"result","Ljava/lang/String;",&r)){const char *text=dvm_string_utf8(v,r.l);if(text&&strlen(text)<3)dprintf(fileno(f)," result=%s",text);}}
 dprintf(fileno(f),"\n");fclose(f);}
 v->exception=saved;v->parked=parked;v->call_steps=steps;errno=saved_errno;
}
void login_panel_before_call(struct dvm *v,struct dvm_method *m,dvm_ref self,const uint32_t *args,int count){
 if(preparing||observing||!m||!m->cls)return;
 diag6j_call(v,m,self,args,count,NULL,1,1);
 init_evidence(v,m,self,args,count,"enter",1);
 callback_evidence(v,m,args,count,"enter",1);
 const char *next=NULL;
 if(count==1&&!strcmp(m->cls->name,"javax/crypto/Cipher")&&!strcmp(m->name,"getInstance")&&!strcmp(m->sig,"(Ljava/lang/String;)Ljavax/crypto/Cipher;")){const char *algorithm=dvm_string_utf8(v,args[0]);if(algorithm&&!strncmp(algorithm,"RSA",3))next="RSA";}
 bool callback=(!strcmp(m->name,"onSuccess")&&!strcmp(m->sig,"(Landroid/os/Bundle;)V"))||((!strcmp(m->name,"onError")||!strcmp(m->name,"onFailed"))&&!strcmp(m->sig,"(Lcom/bsgamesdk/android/callbacklistener/BSGameSdkError;)V"));
 pthread_mutex_lock(&panel_lock);if(live==v&&busy&&!last[0]){if(callback){sdk_callback=1;state_locked("callback",1,NULL);}else if(next&&strcmp(phase,next)&&!timed_out)state_locked(next,1,NULL);}pthread_mutex_unlock(&panel_lock);
}
/* Observe real SDK response objects; never call ResponseBody.string ourselves. */
void login_panel_trace(struct dvm *v,struct dvm_method *m,dvm_ref self,const uint32_t *args,int count,const union dvm_value *out,int ok){
 if(!m||!m->cls)return;web_listener_observe(v,m,self,args,count,ok);
 const char *pc=m->cls->name;
 if(!strcmp(pc,"com/gsc/base/GSCBaseConfig")||!strcmp(pc,"com/gsc/base/utils/CommonTools")||!strcmp(pc,"com/gsc/base/utils/AgreementUtils")){
  bool selected=(!strcmp(pc,"com/gsc/base/GSCBaseConfig")&&(!strcmp(m->sig,"()Z")||!strcmp(m->name,"I")))||strstr(m->name,"Agreement")||!strcmp(m->name,"compareVersion");
  if(selected){FILE *pf=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/prefpersist-runtime.txt","a");if(pf){dprintf(fileno(pf),"PREFPERSIST sdk pid=%d class=%s method=%s sig=%s ok=%d exception=%u",getpid(),pc,m->name,m->sig,ok,v->exception);if(out&&!strcmp(m->sig,"()Z"))dprintf(fileno(pf)," result=%d",out->i);if(!strcmp(m->name,"openAgreement")&&count==2)dprintf(fileno(pf)," type=%u reason=%u",args[0],args[1]);if(!strcmp(m->name,"compareVersion")&&count==2){const char *a=dvm_string_utf8(v,args[0]),*b=dvm_string_utf8(v,args[1]);dprintf(fileno(pf)," left=%.80s right=%.80s result=%d",a?a:"null",b?b:"null",out?out->i:-1);}dprintf(fileno(pf),"\n");fclose(pf);}}
 }
 if(observing||preparing)return;const char*c=m->cls->name;
 diag6j_call(v,m,self,args,count,out,0,ok);
 init_evidence(v,m,self,args,count,"return",ok);
 callback_evidence(v,m,args,count,"return",ok);
 title_init_failure(v,m,args,count,ok);
 /* Only the game's final SDK success callback establishes saved-login
  * metadata. HTTP code=0 and SDK-internal callbacks are insufficient. */
 if(ok&&!v->exception&&!strcmp(c,"com/bilibili/star/bili/Sdk$4$1")&&
    !strcmp(m->name,"onSuccess")&&!strcmp(m->sig,"(Landroid/os/Bundle;)V")){
  pthread_mutex_lock(&panel_lock);
  if(live==v&&request[0]&&busy){char event[512],id[300];quote(request,id,sizeof id);
   snprintf(event,sizeof event,"{\"type\":\"login/completed\",\"requestId\":%s,\"source\":\"game_sdk_callback_return\"}",id);
   event_locked(event);busy=0;required=0;sdk_saved_refresh=1;
  }
  pthread_mutex_unlock(&panel_lock);
 }
 bool request_build=!strcmp(c,"okhttp3/Request$Builder")&&!strcmp(m->name,"build");
 bool response=!strcmp(c,"okhttp3/Response$Builder")&&!strcmp(m->name,"build");
 bool body=!strcmp(c,"okhttp3/ResponseBody")&&!strcmp(m->name,"string");
 /* Interceptors copy ResponseBody through source()/Buffer, without string().
  * Observe their returned copy by argument identity, never consume the source. */
 if(count==1&&!strcmp(m->sig,"(Lokhttp3/ResponseBody;)Ljava/lang/String;")){body=true;self=args[0];}
 if(!ok){pthread_mutex_lock(&panel_lock);if(live==v&&busy&&!last[0]&&dvm_exception(v)){char error[650];struct dvm_object *exception=dvm__obj(v,dvm_exception(v));snprintf(error,sizeof error,"exception=%s at %.180s.%.100s",exception&&exception->cls?exception->cls->name:"unknown",c,m->name);if(strcmp(last_error,error))state_locked(phase,1,error);}pthread_mutex_unlock(&panel_lock);return;}
 if(!out||(!request_build&&!response&&!body))return;
 pthread_mutex_lock(&panel_lock);bool enabled=live==v&&busy;pthread_mutex_unlock(&panel_lock);if(!enabled)return;
 observing=1;dvm_ref saved=dvm_exception(v);bool parked=v->parked;
 if(request_build){union dvm_value url={0},text={0};dvm_get_field(v,out->l,"url","Lokhttp3/HttpUrl;",&url);dvm_get_field(v,url.l,"url","Ljava/lang/String;",&text);const char *address=dvm_string_utf8(v,text.l);const char *next=address&&strstr(address,"/api/external/issue/cipher/v3")?"cipher":address&&strstr(address,"/api/external/login/v3")?"login":NULL;if(next){union dvm_value method={0};dvm_get_field(v,out->l,"method","Ljava/lang/String;",&method);char event[2000],id[800],url_json[800],method_json[100];pthread_mutex_lock(&panel_lock);quote(request,id,sizeof id);quote(address,url_json,sizeof url_json);quote(dvm_string_utf8(v,method.l),method_json,sizeof method_json);snprintf(event,sizeof event,"{\"type\":\"login/request\",\"sessionId\":\"%lu\",\"requestId\":%s,\"method\":%s,\"url\":%s,\"source\":\"original_request_builder\"}",session,id,method_json,url_json);event_locked(event);if(!last[0]&&!timed_out&&strcmp(phase,next))state_locked(next,1,NULL);pthread_mutex_unlock(&panel_lock);}
 }else if(response){union dvm_value req={0},url={0},text={0},b={0},code={0};dvm_get_field(v,out->l,"request","Lokhttp3/Request;",&req);dvm_get_field(v,req.l,"url","Lokhttp3/HttpUrl;",&url);dvm_get_field(v,url.l,"url","Ljava/lang/String;",&text);const char *address=dvm_string_utf8(v,text.l);
  if(address&&strstr(address,"/api/external/login/v3")){dvm_get_field(v,out->l,"body","Lokhttp3/ResponseBody;",&b);dvm_get_field(v,out->l,"code","I",&code);struct dvm_object *body_object=b.l?dvm__obj(v,b.l):NULL;uint64_t generation=body_object?body_object->identity_generation:0;pthread_mutex_lock(&panel_lock);int existing=-1;for(int j=0;j<16;j++)if(panel_response_identity(v,(unsigned)j)&&b.l&&responses[j].body==b.l&&responses[j].body_generation==generation)existing=j;unsigned i=existing>=0?(unsigned)existing:response_next++%16;if(existing<0)responses[i].emitted=0;responses[i].body=generation?b.l:0;responses[i].body_generation=generation;responses[i].status=code.i;snprintf(responses[i].url,sizeof responses[i].url,"%s",address);pthread_mutex_unlock(&panel_lock);}
 }else{
  int index=-1;pthread_mutex_lock(&panel_lock);for(int i=0;i<16;i++)if(panel_response_identity(v,(unsigned)i)&&self&&responses[i].body==self)index=i;pthread_mutex_unlock(&panel_lock);
  if(index>=0&&!responses[index].emitted){union dvm_value a={.l=out->l},q={0};dvm_ref obj=dvm_new_object(v,dvm_find_class_or_external(v,"org/json/JSONObject"));
   if(json_runtime_call(v,"org/json/JSONObject","<init>","(Ljava/lang/String;)V",obj,&a,1,&q)&&!dvm_exception(v)){
    const char*keys[]={"code","message","server_message","need_captch"};char values[4][1100];bool valid=true;
    for(int i=0;i<4;i++){a.l=dvm_new_string(v,keys[i]);if(!json_runtime_call(v,"org/json/JSONObject","optString","(Ljava/lang/String;)Ljava/lang/String;",obj,&a,1,&q)||dvm_exception(v)){valid=false;break;}quote(dvm_string_utf8(v,q.l),values[i],sizeof values[i]);}
    if(valid){char r[800],u[1600],event[8192];pthread_mutex_lock(&panel_lock);quote(request,r,sizeof r);quote(responses[index].url,u,sizeof u);snprintf(event,sizeof event,"{\"type\":\"login/response\",\"sessionId\":\"%lu\",\"requestId\":%s,\"url\":%s,\"httpStatus\":%d,\"code\":%s,\"message\":%s,\"server_message\":%s,\"need_captch\":%s,\"source\":\"network\",\"interim\":true}",session,r,u,responses[index].status,values[0],values[1],values[2],values[3]);
     if(!responses[index].emitted){responses[index].emitted=1;snprintf(last,sizeof last,"received");event_locked(event);/* Keep single-flight until SDK worker ends; captcha remains unresolved. */state_locked(!strcmp(values[3],"\"1\"")?"waiting_captcha":"response",1,NULL);}pthread_mutex_unlock(&panel_lock);}
   }
  }
 }
 v->exception=saved;v->parked=parked;observing=0;
}

/* Hidden strong aliases bind ownership certificates to these reviewed bodies,
 * even if a public/weak callback name is replaced at link time. */
extern __typeof__(login_panel_before_call) leak_cert_panel_pre __attribute__((alias("login_panel_before_call"),visibility("hidden")));
extern __typeof__(login_panel_trace) leak_cert_panel_post __attribute__((alias("login_panel_trace"),visibility("hidden")));
