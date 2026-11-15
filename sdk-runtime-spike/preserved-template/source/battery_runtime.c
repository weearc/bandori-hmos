#include "battery_runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

#define ACTION "android.intent.action.BATTERY_CHANGED"
enum { FILTER_MAGIC=0x4246494c, INTENT_MAGIC=0x42494e54, RECEIVER_MAGIC=0x42524356 };
struct payload { unsigned magic; char *action; battery_host_event event; };
struct queued { battery_host_event event; int sticky; struct queued *next; };
struct state { struct dvm *vm; dvm_ref receiver, context; int subscribed, pumping, closing;
 pthread_mutex_t lock; struct queued *head,*tail; unsigned queue_size; uint64_t last_generation; struct state *next; };
static pthread_mutex_t globals_lock=PTHREAD_MUTEX_INITIALIZER;
static struct state *states;
static battery_host_callbacks host_api;
static void *host_ctx;
static void payload_free(void *ptr){struct payload *p=ptr;if(p){free(p->action);free(p);}}
static struct payload *payload(struct dvm *v,dvm_ref ref,unsigned magic,int create){
 struct dvm_object *o=dvm__obj(v,ref);if(!o){dvm__throw(v,"java/lang/NullPointerException","battery receiver");return NULL;}
 struct payload *p=o->data;
 if(!p&&create){p=calloc(1,sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","battery payload");return NULL;}p->magic=magic;o->data=p;o->data_destroy=payload_free;}
 if(!p||o->data_destroy!=payload_free||p->magic!=magic){dvm__throw(v,"java/lang/IllegalStateException","battery object not initialized");return NULL;}return p;
}
/* Non-throwing ownership probe.  IntentFilter is shared with
 * intent_filter_runtime: battery may only claim objects carrying its own
 * 'BFIL' payload, and must let every other filter fall through to that
 * module.  Returning NULL here is a dispatch decision, not an error. */
static struct payload *payload_peek(struct dvm *v,dvm_ref ref,unsigned magic){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->data||o->data_destroy!=payload_free)return NULL;
 struct payload *p=o->data;
 return p->magic==magic?p:NULL;
}
static const char *string(struct dvm *v,dvm_ref r){struct dvm_object *o=dvm__obj(v,r);return o&&o->kind==DVM_OBJ_STRING?o->utf8:NULL;}
static struct state *state_get(struct dvm *v,int create){
 pthread_mutex_lock(&globals_lock);struct state *s=states;while(s&&s->vm!=v)s=s->next;
 if(!s&&create){s=calloc(1,sizeof *s);if(s){s->vm=v;pthread_mutex_init(&s->lock,NULL);s->next=states;states=s;}}
 pthread_mutex_unlock(&globals_lock);return s;
}
static void emit(void *user,const battery_host_event *event){
 struct state *s=user;if(!event)return;struct queued *q=malloc(sizeof *q);if(!q)return;
 q->event=*event;q->event.technology[sizeof q->event.technology-1]=0;q->sticky=0;q->next=NULL;
 pthread_mutex_lock(&s->lock);if(s->closing||!s->receiver){pthread_mutex_unlock(&s->lock);free(q);return;}
 if(event->generation&&event->generation<=s->last_generation){pthread_mutex_unlock(&s->lock);free(q);return;}
 if(event->generation)s->last_generation=event->generation;
 if(s->queue_size>=16){struct queued *old=s->head;s->head=old->next;if(!s->head)s->tail=NULL;--s->queue_size;free(old);}
 if(s->tail)s->tail->next=q;else s->head=q;s->tail=q;++s->queue_size;pthread_mutex_unlock(&s->lock);
}
static void clear_queue(struct state *s){pthread_mutex_lock(&s->lock);struct queued *q=s->head;s->head=s->tail=NULL;s->queue_size=0;s->last_generation=0;pthread_mutex_unlock(&s->lock);while(q){struct queued *next=q->next;free(q);q=next;}}
static int enqueue_sticky(struct state *s,const battery_host_event *event){struct queued *q=malloc(sizeof *q);if(!q)return -1;q->event=*event;q->event.technology[63]=0;q->sticky=1;
 pthread_mutex_lock(&s->lock);struct queued **p=&s->head;while(*p){struct queued *old=*p;if(event->generation&&old->event.generation&&old->event.generation<=event->generation){*p=old->next;--s->queue_size;free(old);}else p=&old->next;}
 s->tail=NULL;for(struct queued *it=s->head;it;it=it->next)s->tail=it;
 if(event->generation>s->last_generation)s->last_generation=event->generation;
 q->next=s->head;s->head=q;if(!s->tail)s->tail=q;++s->queue_size;
 if(s->queue_size>16){struct queued *it=s->head;while(it->next&&it->next!=s->tail)it=it->next;if(it->next){free(s->tail);it->next=NULL;s->tail=it;--s->queue_size;}}
 pthread_mutex_unlock(&s->lock);return 0;}
void battery_runtime_set_host(const battery_host_callbacks *api,void *ctx){pthread_mutex_lock(&globals_lock);host_api=api?*api:(battery_host_callbacks){0};host_ctx=ctx;pthread_mutex_unlock(&globals_lock);}
void battery_runtime_get_host(battery_host_callbacks *api,void **ctx){pthread_mutex_lock(&globals_lock);if(api)*api=host_api;if(ctx)*ctx=host_ctx;pthread_mutex_unlock(&globals_lock);}
static dvm_ref intent_new(struct dvm *v,const battery_host_event *e){
 struct dvm_class *c=dvm_find_class_or_external(v,"android/content/Intent");if(!c)return 0;
 dvm_ref r=dvm_new_object(v,c);if(!r)return 0;struct payload *p=payload(v,r,INTENT_MAGIC,1);if(p){p->action=strdup(ACTION);p->event=*e;p->event.technology[63]=0;
  if(!(e->present_mask&BATTERY_HAS_LEVEL))p->event.level=-1;
  if(!(e->present_mask&BATTERY_HAS_SCALE))p->event.scale=100;
  if(!(e->present_mask&BATTERY_HAS_STATUS))p->event.status=1;
  if(!(e->present_mask&BATTERY_HAS_HEALTH))p->event.health=1;
  if(!(e->present_mask&BATTERY_HAS_PLUGGED))p->event.plugged=0;
  if(!(e->present_mask&BATTERY_HAS_TEMPERATURE))p->event.temperature=-1;
  if(!(e->present_mask&BATTERY_HAS_VOLTAGE))p->event.voltage=-1;
  if(!(e->present_mask&BATTERY_HAS_PRESENT))p->event.present=-1;
  if(!(e->present_mask&BATTERY_HAS_TECHNOLOGY))p->event.technology[0]=0;
 }return r;
}
bool battery_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 memset(out,0,sizeof *out);
 if(!strcmp(c,"android/content/IntentFilter")){
  /* This module is ahead of intent_filter_runtime in the dispatch chain.
   * Claim only the battery filter payload; every ordinary IntentFilter
   * (ConnectivityMonitor's CONNECTIVITY_CHANGE filter included) must fall
   * through to intent_filter_runtime without an exception. */
  if(!strcmp(m,"<init>")&&!strcmp(sig,"(Ljava/lang/String;)V")&&n==1){
   const char *action=string(v,a[0].l);
   if(!action){dvm__throw(v,"java/lang/IllegalArgumentException","IntentFilter action");return true;}
   if(strcmp(action,ACTION))return false;
   struct payload *p=payload(v,self,FILTER_MAGIC,1);
   if(p){free(p->action);p->action=strdup(action);}
   return true;
  }
  if(!strcmp(m,"addAction")&&!strcmp(sig,"(Ljava/lang/String;)V")&&n==1){
   struct payload *p=payload_peek(v,self,FILTER_MAGIC);
   if(!p)return false;
   const char *action=string(v,a[0].l);
   if(action){if(!p->action)p->action=strdup(action);else if(strcmp(p->action,action))dvm__throw(v,"java/lang/UnsupportedOperationException","multiple battery filter actions");}
   return true;
  }
  return false;
 }
 if(!strcmp(c,"android/content/BroadcastReceiver")&&!strcmp(m,"<init>")&&!strcmp(sig,"()V")){payload(v,self,RECEIVER_MAGIC,1);return true;}
 if(strcmp(c,"android/content/Intent"))return false;
 /* This hook is first in the platform dispatch chain. Only claim an Intent
  * that carries this module's battery payload; ordinary Intents (including
  * intent_runtime payloads) must fall through without an exception. */
 struct dvm_object *intent_object=dvm__obj(v,self);
 struct payload *intent=!intent_object||intent_object->data_destroy!=payload_free?NULL:intent_object->data;
 if(!intent||intent->magic!=INTENT_MAGIC)return false;
 if(!strcmp(m,"getAction")&&!strcmp(sig,"()Ljava/lang/String;")){out->l=dvm_new_string(v,intent->action?intent->action:"");return true;}
 if(!strcmp(m,"getIntExtra")&&!strcmp(sig,"(Ljava/lang/String;I)I")&&n==2){const char *key=string(v,a[0].l);out->i=a[1].i;if(!key)return true;
  const battery_host_event *e=&intent->event;
  if(!strcmp(key,"level")&&(e->present_mask&BATTERY_HAS_LEVEL))out->i=e->level;
  else if(!strcmp(key,"scale")&&(e->present_mask&BATTERY_HAS_SCALE))out->i=e->scale;
  else if(!strcmp(key,"status")&&(e->present_mask&BATTERY_HAS_STATUS))out->i=e->status;
  else if(!strcmp(key,"health")&&(e->present_mask&BATTERY_HAS_HEALTH))out->i=e->health;
  else if(!strcmp(key,"plugged")&&(e->present_mask&BATTERY_HAS_PLUGGED))out->i=e->plugged;
  else if(!strcmp(key,"temperature")&&(e->present_mask&BATTERY_HAS_TEMPERATURE))out->i=e->temperature;
  else if(!strcmp(key,"voltage")&&(e->present_mask&BATTERY_HAS_VOLTAGE))out->i=e->voltage;
  else if(!strcmp(key,"present")&&(e->present_mask&BATTERY_HAS_PRESENT))out->i=e->present;return true;}
 if(!strcmp(m,"getStringExtra")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;")&&n==1){const char *key=string(v,a[0].l);if(key&&!strcmp(key,"technology")&&intent->event.technology[0])out->l=dvm_new_string(v,intent->event.technology);return true;}
 return false;
}
bool battery_runtime_context_broadcast(struct dvm *v,const char *m,const char *sig,dvm_ref context,const union dvm_value *a,int n,union dvm_value *out){
 memset(out,0,sizeof *out);
 if(!strcmp(m,"registerReceiver")&&n==2&&!strcmp(sig,"(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;")){
  struct dvm_object *ctx=dvm__obj(v,context);struct dvm_class *ctx_base=dvm_find_class_or_external(v,"android/content/Context");
  if(!ctx||!ctx_base||!dvm__class_assignable(v,ctx->cls,ctx_base)){dvm__throw(v,"java/lang/IllegalArgumentException","invalid Context");return true;}
  struct payload *f=payload_peek(v,a[1].l,FILTER_MAGIC);if(!f)return false;
  if(!f->action||strcmp(f->action,ACTION)){dvm__throw(v,"java/lang/UnsupportedOperationException","unsupported broadcast action");return true;}
  if(!a[0].l){dvm__throw(v,"java/lang/IllegalArgumentException","battery receiver required");return true;}
  struct dvm_object *receiver=dvm__obj(v,a[0].l);struct dvm_class *base=dvm_find_class_or_external(v,"android/content/BroadcastReceiver");
  if(!receiver||!base||!dvm__class_assignable(v,receiver->cls,base)){dvm__throw(v,"java/lang/IllegalArgumentException","invalid battery receiver");return true;}
  if(!host_api.subscribe||!host_api.snapshot||!host_api.unsubscribe){dvm__throw(v,"java/lang/IllegalStateException","battery host unavailable");return true;}
  struct state *s=state_get(v,1);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","battery state");return true;}
  if(s->receiver){dvm__throw(v,"java/lang/IllegalStateException","battery receiver already registered");return true;}
  s->receiver=a[0].l;s->context=context;dvm_pin(v,s->receiver);dvm_pin(v,s->context);
  battery_host_event snap={0};
  if(host_api.subscribe(host_ctx,emit,s)!=0||host_api.snapshot(host_ctx,&snap)!=0||enqueue_sticky(s,&snap)!=0){
   host_api.unsubscribe(host_ctx,s);clear_queue(s);s->receiver=0;s->context=0;dvm_unpin(v,a[0].l);dvm_unpin(v,context);dvm__throw(v,"java/lang/IllegalStateException","battery host subscription failed");return true;}
  s->subscribed=1;out->l=intent_new(v,&snap);if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","battery intent");
  sdk_trace_fprintf(stderr,"BATTERY_REGISTER accepted=1 snapshot_mask=%llu sticky_queued=1 os_change_delivery_not_assumed=1\n",(unsigned long long)snap.present_mask);return true;
 }
 if(!strcmp(m,"unregisterReceiver")&&n==1&&!strcmp(sig,"(Landroid/content/BroadcastReceiver;)V")){
  struct state *s=state_get(v,0);if(!s||s->receiver!=a[0].l){dvm__throw(v,"java/lang/IllegalArgumentException","receiver not registered");return true;}
  host_api.unsubscribe(host_ctx,s);s->subscribed=0;clear_queue(s);dvm_unpin(v,s->receiver);dvm_unpin(v,s->context);s->receiver=s->context=0;return true;
 }return false;
}
void battery_runtime_pump(struct dvm *v){struct state *s=state_get(v,0);if(!s||s->pumping||!s->receiver)return;s->pumping=1;
 for(int i=0;i<32&&!dvm_exception(v);++i){pthread_mutex_lock(&s->lock);struct queued *q=s->head;if(q){s->head=q->next;if(!s->head)s->tail=NULL;--s->queue_size;}pthread_mutex_unlock(&s->lock);if(!q)break;
  fprintf(stderr,"BATTERY_DELIVERY source=%s generation=%llu\n",q->sticky?"sticky_snapshot":"os_change",(unsigned long long)q->event.generation);
  dvm_ref intent=intent_new(v,&q->event);free(q);if(!intent)break;struct dvm_object *r=dvm__obj(v,s->receiver);struct dvm_method *method=r?dvm_find_method(v,r->cls,"onReceive","(Landroid/content/Context;Landroid/content/Intent;)V"):NULL;
  if(!method){dvm__throw(v,"java/lang/AbstractMethodError","BroadcastReceiver.onReceive");break;}
  union dvm_value args[2]={{.l=s->context},{.l=intent}},out={0};dvm_call(v,method,s->receiver,args,2,&out);
 }s->pumping=0;}
void battery_runtime_finish(struct dvm *v){struct state *s=state_get(v,0);if(!s)return;pthread_mutex_lock(&s->lock);s->closing=1;pthread_mutex_unlock(&s->lock);
 if(s->subscribed&&host_api.unsubscribe)host_api.unsubscribe(host_ctx,s);
 pthread_mutex_lock(&globals_lock);struct state **p=&states;while(*p&&*p!=s)p=&(*p)->next;if(*p)*p=s->next;pthread_mutex_unlock(&globals_lock);
 clear_queue(s);if(s->receiver)dvm_unpin(v,s->receiver);if(s->context)dvm_unpin(v,s->context);pthread_mutex_destroy(&s->lock);free(s);
}
