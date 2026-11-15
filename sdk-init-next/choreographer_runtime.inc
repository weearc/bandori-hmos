/* Included after the worker queue helpers. Only the owner Looper enters Java.
 * Host VSync callbacks manipulate preallocated C queue nodes under queue_lock.
 * Retired native callback cookies occupy bounded static slots and are never
 * reused, so a late OS callback cannot dereference a destroyed VM/arena. */
#include <native_vsync/native_vsync.h>
#include "perf_attr.h"
#define CHOREO_LIMIT 64
struct choreo;
struct choreo_request {
 struct choreo *owner;struct choreo_request *next,*all_next;
 struct task *task;dvm_ref callback,runnable;uint64_t due;
 int64_t timestamp;bool queued,cancelled,released;int error;
};
struct choreo {
 bool used,active,scheduled,in_frame;struct dvm *vm;struct worker *worker;
 dvm_ref self,looper;OH_NativeVSync *native;
 struct choreo_request *pending,*all,*retired;struct task *main_head;
 int64_t frame_time;unsigned events;uint64_t period_session;long long measured_period;int period_result;
};
static struct choreo choreos[CHOREO_LIMIT];
static pthread_mutex_t choreo_native_lock=PTHREAD_MUTEX_INITIALIZER;
struct queue_identity {struct dvm *vm;dvm_ref self,looper;};
static struct queue_identity queue_identities[CHOREO_LIMIT];
static void choreo_cookie_release(void *unused){(void)unused;}
static void choreo_request_release(void *data){
 struct choreo_request *r=data;if(!r)return;
 if(!r->released){dvm_unpin(r->owner->vm,r->callback);dvm_unpin(r->owner->vm,r->runnable);}
 free(r);
}
/* Only our non-escaping dispatcher is reclaimable. The original FrameCallback
 * and InvocationHandler's argument array remain guest-owned. A retired request
 * is published after doFrame returns; reap on a subsequent owner entry under
 * the GIL, never from its own run() stack or from the OS callback thread. */
static void choreo_reap(struct choreo *c){
 pthread_mutex_lock(&queue_lock);struct choreo_request *r=c->retired;c->retired=NULL;pthread_mutex_unlock(&queue_lock);
 while(r){struct choreo_request *next=r->all_next;dvm__discard_temporary(c->vm,r->runnable);r=next;}
}
static void choreo_log(struct choreo *c,const char *event,int value,int64_t stamp){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
 if(f){fprintf(f,"HARMONY_VSYNC event=%s looper=%u value=%d timestamp=%lld\n",event,(unsigned)c->looper,value,(long long)stamp);fclose(f);}
}
static void choreo_native_frame(long long timestamp,void *cookie);
static int choreo_arm(struct choreo *c){
 pthread_mutex_lock(&choreo_native_lock);pthread_mutex_lock(&queue_lock);
 OH_NativeVSync *native=c->active?c->native:NULL;
 pthread_mutex_unlock(&queue_lock);
 int rc=native?OH_NativeVSync_RequestFrame(native,choreo_native_frame,c):-1;
 pthread_mutex_unlock(&choreo_native_lock);
 return rc;
}
static void choreo_queue_locked(struct choreo *c,struct choreo_request *r){
 struct task *t=r->task;t->due=now_ms();t->sequence=++run.sequence;
 struct task **at=c->worker?&c->worker->head:&c->main_head;
 while(*at&&((*at)->due<t->due||((*at)->due==t->due&&(*at)->sequence<t->sequence)))at=&(*at)->next;
 t->next=*at;*at=t;r->queued=true;
 if(c->worker)++c->worker->queued;
 pthread_cond_broadcast(&changed);
}
static void choreo_native_frame(long long timestamp,void *cookie){
 uint64_t callback_start=perf_now();
 perf_result_event(5,(uint64_t)timestamp,(uintptr_t)cookie);
 struct choreo *c=cookie;bool again=false,log=false;
 pthread_mutex_lock(&queue_lock);
 if(!c->active||!c->vm){pthread_mutex_unlock(&queue_lock);perf_result_event(12,callback_start,(uintptr_t)cookie);return;}
 uint64_t session=perf_play_session();
 if(session){
  if(c->period_session!=session){c->period_session=session;c->measured_period=0;c->period_result=OH_NativeVSync_GetPeriod(c->native,&c->measured_period);}
  perf_result_event(25,(uint64_t)c->measured_period,(uint64_t)(int64_t)c->period_result);
 }
 c->scheduled=false;
 struct choreo_request **at=&c->pending;
 while(*at){
  struct choreo_request *r=*at;
  bool ready=r->cancelled||r->due<=now_ms();
  bool room=!c->worker||(!c->worker->quit&&c->worker->alive&&c->worker->queued<1024);
  if(ready&&room){*at=r->next;r->next=NULL;r->timestamp=timestamp;choreo_queue_locked(c,r);}
  else at=&r->next;
 }
 again=c->pending&&(!c->worker||(!c->worker->quit&&c->worker->alive));
 c->scheduled=again;log=c->events++<128;
 pthread_mutex_unlock(&queue_lock);
 (void)log; /* No diagnostic file IO from the native vsync callback. */
 if(again){int rc=choreo_arm(c);if(rc){
  pthread_mutex_lock(&queue_lock);c->scheduled=false;
  if(c->active)while(c->pending){struct choreo_request *r=c->pending;c->pending=r->next;r->next=NULL;r->error=rc;choreo_queue_locked(c,r);}
  pthread_mutex_unlock(&queue_lock); /* Error is delivered via r->error on owner. */
 }}
 perf_result_event(12,callback_start,(uintptr_t)cookie);
}
static bool choreo_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)out;struct dvm_object *o=dvm__obj(v,self);
 struct choreo_request *r=o&&o->data_destroy==choreo_request_release?o->data:NULL;
 if(!r||r->released){dvm__throw(v,"java/lang/IllegalStateException","VSync callback state");return false;}
 struct choreo *c=r->owner;bool ok=true;
 pthread_mutex_lock(&queue_lock);bool active=c->active&&c->vm==v;pthread_mutex_unlock(&queue_lock);
 if(active&&!r->cancelled){
  bool correct_owner=dvm_gil_held(v)&&(c->worker?
   (pthread_equal(pthread_self(),c->worker->thread)&&thread_runtime_current(v)==c->worker->self):thread_runtime_is_main(v));
  if(!correct_owner){dvm__throw(v,"java/lang/IllegalStateException","VSync callback on wrong Looper");ok=false;}
  else if(r->error){dvm__throw(v,"java/lang/IllegalStateException","NativeVSync request failed: %d",r->error);ok=false;}
  else{
   /* Owner dispatch performs no diagnostic file IO. */
   c->frame_time=r->timestamp;c->in_frame=true;perf_window_enter();
   union dvm_value stamp={.j=r->timestamp};
   perf_result_event(6,(uint64_t)r->timestamp,(uintptr_t)c);
   ok=call_method(v,r->callback,"doFrame","(J)V",&stamp,1);c->in_frame=false;perf_window_leave();
   perf_result_event(7,(uint64_t)r->timestamp,ok&&!dvm_exception(v));
   extern void dvm_gc_frame_boundary(struct dvm *);dvm_gc_frame_boundary(v);
  }
 }
 pthread_mutex_lock(&queue_lock);
 struct choreo_request **at=&c->all;while(*at&&*at!=r)at=&(*at)->all_next;if(*at)*at=r->all_next;
 r->released=true;r->task=NULL;r->all_next=c->retired;c->retired=r;pthread_mutex_unlock(&queue_lock);
 dvm_unpin(v,r->callback);dvm_unpin(v,r->runnable);return ok&&!dvm_exception(v);
}
static bool family_metadata(struct dvm *v,const char *name,const char *const rows[][3],size_t count){
 struct dvm_class *c=dvm_find_class_or_external(v,name);if(!c)return false;
 if(c->nmethods)return true;
 struct dvm_method *ms=calloc(count,sizeof *ms);if(!ms)return false;
 for(size_t i=0;i<count;i++){
  ms[i].cls=c;ms[i].name=rows[i][0];ms[i].sig=strdup(rows[i][1]);
  if(!ms[i].sig){for(size_t j=0;j<i;j++)free(ms[j].sig);free(ms);return false;}
  ms[i].access=DEX_ACC_PUBLIC|(!strcmp(rows[i][2],"S")?DEX_ACC_STATIC:0);
  ms[i].arg_slots=dvm_sig_arg_slots(ms[i].sig);ms[i].arg_count=dvm_sig_arg_count(ms[i].sig);ms[i].ret_kind=dvm_sig_return_kind(ms[i].sig);
 }
 c->methods=ms;c->nmethods=(int)count;return true;
}
static void choreographer_install(struct dvm *v){
 static const char *const looper[][3]={
  {"getMainLooper","()Landroid/os/Looper;","S"},{"myLooper","()Landroid/os/Looper;","S"},
  {"prepare","()V","S"},{"loop","()V","S"},{"myQueue","()Landroid/os/MessageQueue;","S"},
  {"getQueue","()Landroid/os/MessageQueue;","I"},{"getThread","()Ljava/lang/Thread;","I"},
  {"isCurrentThread","()Z","I"},{"quit","()V","I"}};
 static const char *const queue[][3]={{"isIdle","()Z","I"}};
 static const char *const choreo[][3]={
  {"getInstance","()Landroid/view/Choreographer;","S"},
  {"postFrameCallback","(Landroid/view/Choreographer$FrameCallback;)V","I"},
  {"postFrameCallbackDelayed","(Landroid/view/Choreographer$FrameCallback;J)V","I"},
  {"removeFrameCallback","(Landroid/view/Choreographer$FrameCallback;)V","I"},
  {"getFrameTimeNanos","()J","I"}};
 bool ok=family_metadata(v,"android/os/Looper",looper,sizeof looper/sizeof looper[0])&&
  family_metadata(v,"android/os/MessageQueue",queue,1)&&
  family_metadata(v,"android/view/Choreographer",choreo,sizeof choreo/sizeof choreo[0]);
 struct dvm_class *r=dvm_find_class_or_external(v,"sdktrace/NativeVSyncDispatch");
 if(r&&!r->nmethods){r->methods=calloc(1,sizeof *r->methods);if(r->methods){r->nmethods=1;ok=ok&&stack_trace_element_install(r,0,"run","()V",choreo_run,'V',0);}else ok=false;}
 if(!ok)dvm__throw(v,"java/lang/OutOfMemoryError","Looper/VSync metadata");
}
static dvm_ref current_looper(struct dvm *v){
 union dvm_value out={0};
 if(!thread_runtime_call(v,"android/os/Looper","myLooper","()Landroid/os/Looper;",0,NULL,0,&out))
  android_basics_call(v,"android/os/Looper","myLooper","()Landroid/os/Looper;",0,NULL,0,&out);
 return out.l;
}
static bool known_looper(struct dvm *v,dvm_ref looper){
 if(worker_for_looper(v,looper))return true;
 union dvm_value main={0};return android_basics_call(v,"android/os/Looper","getMainLooper","()Landroid/os/Looper;",0,NULL,0,&main)&&main.l==looper;
}
static dvm_ref queue_for_looper(struct dvm *v,dvm_ref looper){
 if(!looper||!known_looper(v,looper)){dvm__throw(v,"java/lang/IllegalStateException","MessageQueue owner Looper");return 0;}
 struct queue_identity *empty=NULL;
 for(unsigned i=0;i<CHOREO_LIMIT;i++){
  struct queue_identity *q=&queue_identities[i];if(q->vm==v&&q->looper==looper)return q->self;if(!q->vm&&!empty)empty=q;
 }
 if(!empty){dvm__throw(v,"java/lang/OutOfMemoryError","MessageQueue identity limit");return 0;}
 dvm_ref ref=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/MessageQueue"));
 if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","MessageQueue");return 0;}
 *empty=(struct queue_identity){.vm=v,.self=ref,.looper=looper};dvm_pin(v,ref);dvm_pin(v,looper);return ref;
}
static bool choreographer_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!strcmp(c,"android/os/Looper")&&!strcmp(sig,"()Landroid/os/MessageQueue;")&&n==0&&(!strcmp(m,"getQueue")||!strcmp(m,"myQueue"))){
  out->l=queue_for_looper(v,!strcmp(m,"myQueue")?current_looper(v):self);return true;
 }
 if(!strcmp(c,"android/os/MessageQueue")&&!strcmp(m,"isIdle")&&!strcmp(sig,"()Z")&&n==0){
  struct queue_identity *q=NULL;for(unsigned i=0;i<CHOREO_LIMIT;i++)if(queue_identities[i].vm==v&&queue_identities[i].self==self){q=&queue_identities[i];break;}
  if(!q){dvm__throw(v,"java/lang/IllegalArgumentException","MessageQueue receiver");return true;}
  struct worker *w=worker_for_looper(v,q->looper);out->i=1;
  if(w){pthread_mutex_lock(&queue_lock);out->i=!w->head||w->head->due>now_ms();pthread_mutex_unlock(&queue_lock);}
  else{
   for(int i=0;i<v->npending;i++)if(!v->pending_is_thread[i]&&!v->pending_looper[i]&&v->pending_due_ms[i]<=now_ms()){out->i=0;break;}
   pthread_mutex_lock(&queue_lock);
   for(unsigned i=0;i<CHOREO_LIMIT;i++)if(choreos[i].active&&choreos[i].vm==v&&choreos[i].looper==q->looper&&choreos[i].main_head)out->i=0;
   pthread_mutex_unlock(&queue_lock);
  }
  return true;
 }
 if(strcmp(c,"android/view/Choreographer"))return false;
 if(!strcmp(m,"getInstance")&&!strcmp(sig,"()Landroid/view/Choreographer;")&&n==0){
  dvm_ref looper=current_looper(v);if(!looper){dvm__throw(v,"java/lang/IllegalStateException","Choreographer requires a prepared Looper");return true;}
  struct choreo *empty=NULL;
  for(unsigned i=0;i<CHOREO_LIMIT;i++){struct choreo *c=&choreos[i];if(c->active&&c->vm==v&&c->looper==looper){out->l=c->self;return true;}if(!c->used&&!empty)empty=c;}
  if(!empty){dvm__throw(v,"java/lang/OutOfMemoryError","NativeVSync cookie limit");return true;}
  OH_NativeVSync *native=OH_NativeVSync_Create("BandDreamGuest",14);
  if(!native){dvm__throw(v,"java/lang/UnsupportedOperationException","Harmony NativeVSync unavailable");return true;}
  dvm_ref ref=dvm_new_object(v,dvm_find_class_or_external(v,"android/view/Choreographer"));
  if(!ref){OH_NativeVSync_Destroy(native);dvm__throw(v,"java/lang/OutOfMemoryError","Choreographer");return true;}
  struct dvm_object *o=dvm__obj(v,ref);
  *empty=(struct choreo){.used=true,.active=true,.vm=v,.self=ref,.looper=looper,.worker=worker_for_looper(v,looper),.native=native};
  o->data=empty;o->data_destroy=choreo_cookie_release;dvm_pin(v,ref);out->l=ref;choreo_log(empty,"created",0,0);return true;
 }
 struct dvm_object *o=dvm__obj(v,self);struct choreo *owner=o&&o->data_destroy==choreo_cookie_release?o->data:NULL;
 if(!owner||!owner->active||owner->vm!=v){dvm__throw(v,"java/lang/IllegalStateException","Choreographer receiver");return true;}
 if(!strcmp(m,"getFrameTimeNanos")&&!strcmp(sig,"()J")&&n==0){if(!owner->in_frame)dvm__throw(v,"java/lang/IllegalStateException","not inside a frame callback");else out->j=owner->frame_time;return true;}
 bool post=!strcmp(m,"postFrameCallback")&&!strcmp(sig,"(Landroid/view/Choreographer$FrameCallback;)V")&&n==1;
 bool delayed=!strcmp(m,"postFrameCallbackDelayed")&&!strcmp(sig,"(Landroid/view/Choreographer$FrameCallback;J)V")&&n==2;
 bool remove=!strcmp(m,"removeFrameCallback")&&!strcmp(sig,"(Landroid/view/Choreographer$FrameCallback;)V")&&n==1;
 if(!post&&!delayed&&!remove)return false;
 choreo_reap(owner);
 if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","FrameCallback");return true;}
 if(remove){pthread_mutex_lock(&queue_lock);for(struct choreo_request *r=owner->all;r;r=r->all_next)if(r->callback==a[0].l)r->cancelled=true;pthread_mutex_unlock(&queue_lock);return true;}
 if(owner->worker){pthread_mutex_lock(&queue_lock);bool alive=owner->worker->alive&&!owner->worker->quit;pthread_mutex_unlock(&queue_lock);if(!alive){dvm__throw(v,"java/lang/IllegalStateException","frame Looper has quit");return true;}}
 unsigned count=0;for(struct choreo_request *r=owner->all;r;r=r->all_next)++count;
 if(count>=1024){dvm__throw(v,"java/lang/IllegalStateException","VSync pending callback limit");return true;}
 struct choreo_request *r=calloc(1,sizeof *r);struct task *t=calloc(1,sizeof *t);
 if(!r||!t){free(r);free(t);dvm__throw(v,"java/lang/OutOfMemoryError","VSync request");return true;}
 dvm_ref runnable=dvm_new_object(v,dvm_find_class_or_external(v,"sdktrace/NativeVSyncDispatch"));
 if(!runnable){free(r);free(t);dvm__throw(v,"java/lang/OutOfMemoryError","VSync dispatcher");return true;}
 uint64_t delay=delayed&&a[1].j>0?(uint64_t)a[1].j:0;
 if(delay>UINT64_MAX-now_ms()){free(r);free(t);dvm__throw(v,"java/lang/IllegalArgumentException","frame delay overflow");return true;}
 *r=(struct choreo_request){.owner=owner,.task=t,.callback=a[0].l,.runnable=runnable,.due=now_ms()+delay};t->runnable=runnable;
 o=dvm__obj(v,runnable);o->data=r;o->data_destroy=choreo_request_release;dvm_pin(v,runnable);dvm_pin(v,r->callback);
 pthread_mutex_lock(&queue_lock);r->all_next=owner->all;owner->all=r;r->next=owner->pending;owner->pending=r;bool arm=!owner->scheduled;owner->scheduled=true;pthread_mutex_unlock(&queue_lock);
 if(arm){int rc=choreo_arm(owner);if(rc){pthread_mutex_lock(&queue_lock);owner->scheduled=false;r->cancelled=true;pthread_mutex_unlock(&queue_lock);dvm__throw(v,"java/lang/IllegalStateException","NativeVSync request failed: %d",rc);}}
 return true;
}
void thread_runtime_vsync_tick(struct dvm *v){
 if(!thread_runtime_is_main(v))return;
 for(unsigned i=0;i<CHOREO_LIMIT;i++){
  struct choreo *c=&choreos[i];if(!c->active||c->vm!=v||c->worker)continue;
  choreo_reap(c);
  for(unsigned batch=0;batch<128;batch++){
   pthread_mutex_lock(&queue_lock);struct task *t=c->main_head;if(t)c->main_head=t->next;pthread_mutex_unlock(&queue_lock);if(!t)break;
   bool ok=call_method(v,t->runnable,"run","()V",NULL,0);free(t);choreo_reap(c);
   if(!ok||dvm_exception(v)){sdk_trace_async_failure(v,"main VSync callback");dvm_clear_exception(v);break;}
  }
 }
}
static void choreographer_shutdown(struct dvm *v){
 for(unsigned i=0;i<CHOREO_LIMIT;i++){
  struct choreo *c=&choreos[i];if(!c->active||c->vm!=v)continue;
  choreo_reap(c);
  pthread_mutex_lock(&choreo_native_lock);pthread_mutex_lock(&queue_lock);
  c->active=false;OH_NativeVSync *native=c->native;c->native=NULL;
  struct choreo_request *requests=c->all;c->pending=c->all=NULL;c->main_head=NULL;
  pthread_mutex_unlock(&queue_lock);OH_NativeVSync_Destroy(native);pthread_mutex_unlock(&choreo_native_lock);
  while(requests){struct choreo_request *r=requests;requests=r->all_next;
   if(!r->queued||!c->worker)free(r->task);r->task=NULL;r->released=true;
   dvm_ref cb=r->callback,obj=r->runnable;dvm_unpin(v,cb);dvm_unpin(v,obj);
  }
  dvm_unpin(v,c->self);c->vm=NULL;c->worker=NULL;
 }
 for(unsigned i=0;i<CHOREO_LIMIT;i++){struct queue_identity *q=&queue_identities[i];if(q->vm==v){dvm_unpin(v,q->self);dvm_unpin(v,q->looper);memset(q,0,sizeof *q);}}
}
