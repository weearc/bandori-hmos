#define _POSIX_C_SOURCE 200809L
#include "unity_touch.h"
#include "thread_runtime.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static struct {bool active;struct dvm *vm;dvm_ref player,handler,thread;unity_touch_packet queue[256];unsigned head,count;uint64_t received,accepted,posted,dispatched,consumed,errors;} input;
static void trace(const char *fmt,...){
 char text[1024];va_list a;va_start(a,fmt);vsnprintf(text,sizeof text,fmt,a);va_end(a);
 sdk_trace_fprintf(stderr,"%s\n",text);
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){fprintf(f,"%s\n",text);fclose(f);}
}
__attribute__((visibility("default"))) int sdk_unity_touch_submit(const unity_touch_packet *p){
 if(!p||p->size!=sizeof *p||p->version!=1||p->count<1||p->count>UNITY_TOUCH_MAX||p->event_ms<p->down_ms||p->down_ms<0)return -EINVAL;
 int action=p->action&255,index=(p->action>>8)&255;
 if((action!=0&&action!=1&&action!=2&&action!=3&&action!=5&&action!=6)||index>=p->count||
    ((action==0||action==1)&&p->count!=1))return -EINVAL;
 for(int i=0;i<p->count;i++){
  if(p->points[i].id<0||p->points[i].id>31||!isfinite(p->points[i].x)||!isfinite(p->points[i].y)||!isfinite(p->points[i].pressure))return -EINVAL;
  for(int j=0;j<i;j++)if(p->points[j].id==p->points[i].id)return -EINVAL;
 }
 pthread_mutex_lock(&mu);++input.received;
 int rc=0;if(!input.active)rc=-EAGAIN;else if(input.count==256)rc=-ENOBUFS;
 else {input.queue[(input.head+input.count)%256]=*p;++input.count;++input.accepted;}
 if(rc)++input.errors;pthread_mutex_unlock(&mu);return rc;
}
static void payload_free(void *p){free(p);}
static bool motion_get(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o||o->data_destroy!=payload_free||!o->data){dvm__throw(v,"java/lang/IllegalArgumentException","MotionEvent payload");return false;}
 const unity_touch_packet *p=o->data;const struct dvm_method *m=dvm__builtin_method();const char *name=m->name;
 if(!strcmp(name,"getAction")){out->i=p->action;return true;}
 if(!strcmp(name,"getActionMasked")){out->i=p->action&255;return true;}
 if(!strcmp(name,"getActionIndex")){out->i=(p->action>>8)&255;return true;}
 if(!strcmp(name,"getPointerCount")){out->i=p->count;return true;}
 if(!strcmp(name,"getEventTime")){out->j=p->event_ms;return true;}
 if(!strcmp(name,"getEventTimeNano")){out->j=p->event_ms*1000000;return true;}
 if(!strcmp(name,"getDownTime")){out->j=p->down_ms;return true;}
 if(!strcmp(name,"getSource")){out->i=0x1002;return true;}
 if(!strcmp(name,"getDeviceId")||!strcmp(name,"getDisplayId")||!strcmp(name,"getFlags")||!strcmp(name,"getEdgeFlags")||!strcmp(name,"getMetaState")||!strcmp(name,"getButtonState")||!strcmp(name,"getHistorySize")){out->i=0;return true;}
 if(!strcmp(name,"findPointerIndex")){out->i=-1;for(int i=0;i<p->count;i++)if(p->points[i].id==a[0].i){out->i=i;break;}return true;}
 if(!strcmp(name,"recycle"))return true; /* immutable snapshot; VM owns storage */
 if(!strncmp(name,"getHistorical",13)){dvm__throw(v,"java/lang/IllegalArgumentException","MotionEvent history empty");return false;}
 int index=n?a[0].i:0;
 if(!strcmp(name,"getAxisValue"))index=n==2?a[1].i:0;
 if(index<0||index>=p->count){dvm__throw(v,"java/lang/IllegalArgumentException","MotionEvent pointerIndex");return false;}
 if(!strcmp(name,"getPointerId")){out->i=p->points[index].id;return true;}
 if(!strcmp(name,"getToolType")){out->i=1;return true;} /* touchscreen finger */
 if(!strcmp(name,"getX")||!strcmp(name,"getRawX"))out->f=p->points[index].x;
 else if(!strcmp(name,"getY")||!strcmp(name,"getRawY"))out->f=p->points[index].y;
 else if(!strcmp(name,"getPressure"))out->f=p->points[index].pressure;
 else if(!strcmp(name,"getAxisValue")){switch(a[0].i){case 0:out->f=p->points[index].x;break;case 1:out->f=p->points[index].y;break;case 2:out->f=p->points[index].pressure;break;default:out->f=0;}}
 else out->f=0; /* absent size/orientation axes: Android's unpopulated-axis value */
 return true;
}
static struct dvm_class *install_motion(struct dvm *v){
 struct dvm_class *base=dvm_find_class_or_external(v,"android/view/InputEvent"),*c=dvm_find_class_or_external(v,"android/view/MotionEvent");if(!base||!c)return NULL;c->super=base;
 if(c->nmethods)return c;
 static const struct {const char *name,*sig;} fixed[]={
  {"getAction","()I"},{"getActionMasked","()I"},{"getActionIndex","()I"},{"getPointerCount","()I"},{"getPointerId","(I)I"},{"findPointerIndex","(I)I"},{"getToolType","(I)I"},
  {"getEventTime","()J"},{"getEventTimeNano","()J"},{"getDownTime","()J"},{"getSource","()I"},{"getDeviceId","()I"},{"getDisplayId","()I"},{"getFlags","()I"},{"getEdgeFlags","()I"},{"getMetaState","()I"},{"getButtonState","()I"},{"getHistorySize","()I"},{"getAxisValue","(I)F"},{"getAxisValue","(II)F"},{"recycle","()V"}
 };
 static const char *floats[]={"getX","getY","getRawX","getRawY","getPressure","getSize","getTouchMajor","getTouchMinor","getToolMajor","getToolMinor","getOrientation"};
 int nf=sizeof fixed/sizeof fixed[0],nx=sizeof floats/sizeof floats[0],total=nf+nx*2;
 struct dvm_method *methods=calloc((size_t)total,sizeof *methods);if(!methods)return NULL;
 for(int i=0;i<total;i++){
  struct dvm_method *m=&methods[i];m->cls=c;m->name=i<nf?fixed[i].name:floats[(i-nf)/2];
  m->sig=strdup(i<nf?fixed[i].sig:((i-nf)%2?"(I)F":"()F"));
  if(!m->sig){for(int j=0;j<=i;j++)free(methods[j].sig);free(methods);return NULL;}
  m->builtin=motion_get;m->access=1;m->arg_count=dvm_sig_arg_count(m->sig);m->arg_slots=dvm_sig_arg_slots(m->sig);m->ret_kind=dvm_sig_return_kind(m->sig);
 }
 c->methods=methods;c->nmethods=total;free(c->mcache);c->mcache=NULL;c->mcache_cap=c->mcache_len=0;return c;
}
typedef struct {dvm_ref player,event,thread;uint64_t sequence;} input_task;
static bool run_input(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)out;struct dvm_object *o=dvm__obj(v,self);input_task *t=o?o->data:NULL;if(!t)return false;
 bool owner=thread_runtime_current(v)==t->thread;union dvm_value arg={.l=t->event},result={0};
 struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,t->player),"onTouchEvent","(Landroid/view/MotionEvent;)Z");
 bool ok=owner&&m&&dvm_call(v,m,t->player,&arg,1,&result)&&!dvm_exception(v);
 if(!owner&&!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Unity input wrong engine thread");
 pthread_mutex_lock(&mu);++input.dispatched;if(ok&&result.i)++input.consumed;if(!ok)++input.errors;
 uint64_t received=input.received,dispatched=input.dispatched,consumed=input.consumed;pthread_mutex_unlock(&mu);
 const unity_touch_packet *p=dvm__obj(v,t->event)->data;
 trace("TOUCH_INPUT touch_events=%llu dispatched=%llu consumed=%llu seq=%llu action=%d pointers=%d x=%.3f y=%.3f event_ms=%lld down_ms=%lld owner_ok=%d original_onTouchEvent_ok=%d consumed_event=%d exception=%u",(unsigned long long)received,(unsigned long long)dispatched,(unsigned long long)consumed,(unsigned long long)t->sequence,p->action,p->count,p->points[0].x,p->points[0].y,(long long)p->event_ms,(long long)p->down_ms,owner,ok,ok?result.i:0,dvm_exception(v));
 dvm_unpin(v,t->player);dvm_unpin(v,t->event);o->data=NULL;o->data_destroy=NULL;free(t);dvm_unpin(v,self);return ok;
}
bool unity_touch_begin(struct dvm *v,dvm_ref player){
 if(!v||!player||!dvm_gil_held(v))return false;
 if(!install_motion(v)){dvm__throw(v,"java/lang/OutOfMemoryError","MotionEvent declarations");return false;}
 union dvm_value mt={0},h={0};struct dvm_class *pc=dvm_object_class(v,player);
 if(!dvm__get_field_from(v,player,pc,"m_MainThread","Lcom/unity3d/player/U0;",&mt)||!mt.l||
 !dvm_get_field(v,mt.l,"a","Landroid/os/Handler;",&h)||!h.l){dvm__throw(v,"java/lang/IllegalStateException","Unity input Handler not ready");return false;}
 struct dvm_class *c=dvm__register_builtin(v,"Lsdktrace/input/TouchTask;");if(!c)return false;
 if(!c->nmethods){c->methods=calloc(1,sizeof *c->methods);if(!c->methods)return false;c->nmethods=1;
  c->methods[0]=(struct dvm_method){.cls=c,.name="run",.sig=strdup("()V"),.access=1,.ret_kind='V',.builtin=run_input};if(!c->methods[0].sig)return false;}
 dvm_pin(v,player);dvm_pin(v,h.l);dvm_pin(v,mt.l);
 pthread_mutex_lock(&mu);memset(&input,0,sizeof input);input.vm=v;input.player=player;input.handler=h.l;input.thread=mt.l;input.active=true;pthread_mutex_unlock(&mu);
 trace("TOUCH_INPUT_READY player=%u handler=%u engine_thread=%u queue_capacity=256",player,h.l,mt.l);return true;
}
bool unity_touch_pump(struct dvm *v){
 for(unsigned k=0;k<64;k++){
  pthread_mutex_lock(&mu);if(!input.active||input.vm!=v||!input.count){pthread_mutex_unlock(&mu);return true;}
  unity_touch_packet p=input.queue[input.head];input.head=(input.head+1)%256;--input.count;
  dvm_ref player=input.player,handler=input.handler,thread=input.thread;uint64_t sequence=++input.posted;pthread_mutex_unlock(&mu);
  struct dvm_class *mc=dvm_find_class_or_external(v,"android/view/MotionEvent"),*tc=dvm_find_class(v,"sdktrace/input/TouchTask");
  dvm_ref event=dvm_new_object(v,mc),task=dvm_new_object(v,tc);
  struct dvm_object *eo=dvm__obj(v,event),*to=dvm__obj(v,task);
  unity_touch_packet *payload=malloc(sizeof *payload);input_task *t=calloc(1,sizeof *t);
  if(!eo||!to||!payload||!t){free(payload);free(t);dvm__throw(v,"java/lang/OutOfMemoryError","Unity touch task");return false;}
  *payload=p;eo->data=payload;eo->data_destroy=payload_free;*t=(input_task){.player=player,.event=event,.thread=thread,.sequence=sequence};to->data=t;to->data_destroy=payload_free;
  dvm_pin(v,event);dvm_pin(v,task);dvm_pin(v,player);
  union dvm_value arg={.l=task},out={0};
  bool posted=thread_runtime_call(v,"android/os/Handler","post","(Ljava/lang/Runnable;)Z",handler,&arg,1,&out)&&out.i&&!dvm_exception(v);
  if(!posted){dvm_unpin(v,event);dvm_unpin(v,task);dvm_unpin(v,player);trace("TOUCH_INPUT_POST failed=1 seq=%llu exception=%u",(unsigned long long)sequence,dvm_exception(v));return false;}
 }
 return true;
}
void unity_touch_end(struct dvm *v,FILE *f){
 pthread_mutex_lock(&mu);input.active=false;
 uint64_t received=input.received,accepted=input.accepted,posted=input.posted,dispatched=input.dispatched,consumed=input.consumed,errors=input.errors;
 dvm_ref player=input.player,handler=input.handler,thread=input.thread;input.count=0;pthread_mutex_unlock(&mu);
 trace("TOUCH_INPUT_SUMMARY touch_events=%llu accepted=%llu posted=%llu dispatched=%llu consumed=%llu errors=%llu",(unsigned long long)received,(unsigned long long)accepted,(unsigned long long)posted,(unsigned long long)dispatched,(unsigned long long)consumed,(unsigned long long)errors);
 if(f){fprintf(f,"event=touch_summary touch_events=%llu accepted=%llu posted=%llu dispatched=%llu consumed=%llu errors=%llu\n",(unsigned long long)received,(unsigned long long)accepted,(unsigned long long)posted,(unsigned long long)dispatched,(unsigned long long)consumed,(unsigned long long)errors);fflush(f);}
 dvm_unpin(v,player);dvm_unpin(v,handler);dvm_unpin(v,thread);
}
