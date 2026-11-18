/* Included by unity_player_probe.c. All switches default off. Host enqueue
 * never enters VM/GIL, allocates objects, calls guest code or writes files. */
extern dvm_ref sdk_motion_event_create_multi(struct dvm *,const mt_packet *);
extern uint64_t sdk_motion_event_sequence(struct dvm *,dvm_ref);
#define MT_TRACE_CAP 4096
typedef struct {mt_packet host,event;uint64_t drain,construct_begin,construct,returned;int stage,ok,consumed;dvm_ref exception;char text[256];} mt_trace;
static mt_state multitouch_state;
static mt_trace mt_traces[MT_TRACE_CAP];static unsigned mt_trace_head,mt_trace_tail;
static uint64_t mt_trace_lost,mt_rejected[8],mt_failures[9],mt_received;
static uint64_t mt_timestamp_regressions;
static struct {uint64_t seq,ns;int accepted;} mt_native[MT_TRACE_CAP];static unsigned mt_native_head,mt_native_tail;
static uint64_t mt_native_lost;
static atomic_int multitouch_observe;

__attribute__((visibility("default"))) int sdk_input_queue_multitouch(const mt_packet *host){
 if(!atomic_load(&multitouch_enabled))return -ENOSYS;
 if(!host)return -EINVAL;
 pthread_mutex_lock(&touch_lock);mt_received++;
 unsigned next=(touch_tail+1)%TOUCH_QUEUE_CAP;
 if(next==touch_head){touch_full_count++;touch_dropped++;input_obs_record(INPUT_REJECT,host->sequence,host->action,-ENOBUFS,host->callback_ns,perf_now());pthread_mutex_unlock(&touch_lock);return -ENOBUFS;}
 bool regression=multitouch_state.last_ns&&host->event_ns<multitouch_state.last_ns;
 mt_packet packet;int reason=mt_normalize(&multitouch_state,host,&packet);
 if(reason){mt_rejected[reason<8?reason:0]++;input_obs_record(INPUT_REJECT,host->sequence,host->action,-reason,host->callback_ns,perf_now());if(reason==3||reason==5)atomic_fetch_add(&play_unpaired,1);pthread_mutex_unlock(&touch_lock);return -EINVAL;}
 if(regression)mt_timestamp_regressions++;
 packet.enqueue_ns=perf_now();queued_touch *q=&touch_queue[touch_tail];memset(q,0,sizeof *q);
 q->multi=1;q->action=packet.action;q->pointer_id=packet.points[0].id;q->x=packet.points[0].x;q->y=packet.points[0].y;
 q->host_ns=host->callback_ns;q->event_ms=packet.event_ns/1000000;q->packet=packet;q->trace_slot=UINT32_MAX;
 if(atomic_load(&multitouch_observe)){
  unsigned tn=(mt_trace_tail+1)%MT_TRACE_CAP;
  if(tn!=mt_trace_head){q->trace_slot=mt_trace_tail;mt_trace *t=&mt_traces[mt_trace_tail];memset(t,0,sizeof *t);t->host=*host;t->event=packet;mt_trace_tail=tn;}else mt_trace_lost++;
 }
 input_obs_record_count(INPUT_ENQUEUE,packet.sequence,packet.action,0,host->callback_ns,packet.enqueue_ns,(int)packet.count);
 touch_tail=next;touch_queued++;pthread_mutex_unlock(&touch_lock);ed_notify();return 0;
}
static void multitouch_native_done(struct dvm *vm,dvm_ref event,int accepted){
 if(!atomic_load(&multitouch_observe)&&!atomic_load(&input_obs_enabled))return;
 uint64_t seq=sdk_motion_event_sequence(vm,event);if(!seq)return;
 input_obs_record(INPUT_NATIVE,seq,-1,accepted?1:0,0,perf_now());
 if(!atomic_load(&multitouch_observe))return;
 pthread_mutex_lock(&touch_lock);unsigned next=(mt_native_tail+1)%MT_TRACE_CAP;
 if(next==mt_native_head)mt_native_lost++;else{mt_native[mt_native_tail].seq=seq;mt_native[mt_native_tail].ns=perf_now();mt_native[mt_native_tail].accepted=accepted;mt_native_tail=next;}
 pthread_mutex_unlock(&touch_lock);
}
static bool multitouch_inject(struct dvm *vm,dvm_ref player,const queued_touch *q,int *consumed){
 uint64_t begin=0,constructed=0,call_begin=0;int stage=0;dvm_ref me=0;bool ok=false,pinned=false;union dvm_value out={0};
 if(dvm_exception(vm)){stage=4;goto done;}
 if(!player){stage=1;goto done;}
 struct dvm_class *up=dvm_find_class(vm,"com/unity3d/player/UnityPlayer");
 struct dvm_method *m=up?dvm_find_method(vm,up,"onTouchEvent","(Landroid/view/MotionEvent;)Z"):NULL;
 if(!m){stage=2;goto done;}
 begin=perf_now();me=sdk_motion_event_create_multi(vm,&q->packet);constructed=perf_now();
 if(!me||dvm_exception(vm)){stage=3;goto done;}
 dvm_pin(vm,me);pinned=true;
 /* Same device identity as the legacy route, cached after first successful
  * query. No fresh device-id array and 18 diagnostic getters on every MOVE. */
 static struct dvm *device_vm;static int device_id=-1;static bool device_ready;
 if(device_vm!=vm){device_vm=vm;device_ready=false;device_id=-1;}
 if(!device_ready){
  struct dvm_class *devices=dvm_find_class(vm,"android/view/InputDevice");
  struct dvm_method *ids=devices?dvm_find_method(vm,devices,"getDeviceIds","()[I"):NULL;union dvm_value list={0};
  if(ids&&(!dvm_call(vm,ids,0,NULL,0,&list)||dvm_exception(vm))){stage=5;goto done;}
  if(list.l&&dvm_array_length(vm,list.l)>0){int32_t *data=dvm_array_data(vm,list.l);if(data)device_id=data[0];}
  device_ready=true;
 }
 if(!sdk_motion_event_set_device_id(vm,me,device_id)){stage=6;goto done;}
 union dvm_value arg={.l=me};call_begin=perf_now();
 input_obs_record_count(INPUT_GUEST_BEGIN,q->packet.sequence,q->packet.action,0,call_begin,call_begin,(int)q->packet.count);
 ok=dvm_call(vm,m,player,&arg,1,&out)&&!dvm_exception(vm);
 if(!ok)stage=7;
done:
 input_obs_record_count(INPUT_GUEST,q->packet.sequence,q->packet.action,ok?(out.i?1:0):-(stage?stage:8),call_begin,perf_now(),(int)q->packet.count);
 ed_record((struct ed_row){.seq=q->packet.sequence,.host=q->host_ns,.enqueue=q->packet.enqueue_ns,.drain=q->drain_ns,.construct_begin=begin,.construct=constructed,.end=perf_now(),.ok=ok,.consumed=ok&&out.i});
 if(pinned)dvm_unpin(vm,me);
 if(consumed)*consumed=ok&&out.i;
 char description[256]="-";dvm_ref exc=dvm_exception(vm);
 if(exc)dvm_describe_exception(vm,exc,description,sizeof description);
 pthread_mutex_lock(&touch_lock);
 if(stage>=0&&stage<9)mt_failures[stage]++;
 if(q->trace_slot!=UINT32_MAX){mt_trace *t=&mt_traces[q->trace_slot];if(t->event.sequence==q->packet.sequence){
  t->drain=q->drain_ns;t->construct_begin=begin;t->construct=constructed;t->returned=perf_now();t->stage=stage;t->ok=ok;t->consumed=ok&&out.i;t->exception=exc;snprintf(t->text,sizeof t->text,"%s",description);
 }}
 pthread_mutex_unlock(&touch_lock);return ok;
}
static void mt_write_packet(FILE *f,const mt_trace *t,const mt_packet *p,const char *side){
 for(unsigned i=0;i<p->count;i++){const mt_point *v=&p->points[i];
  fprintf(f,"%d,%llu,%s,%u,%d,%u,%d,%d,%.3f,%.3f,%lld,%lld,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%lld,%d,%d,%d,%u\n",getpid(),(unsigned long long)p->sequence,side,p->count,p->action,i,v->host_id,v->id,v->x,v->y,(long long)(p->down_ns/1000000),(long long)(p->event_ns/1000000),(unsigned long long)p->callback_ns,(unsigned long long)t->event.enqueue_ns,(unsigned long long)t->drain,(unsigned long long)t->construct_begin,(unsigned long long)t->construct,(unsigned long long)t->returned,(long long)p->event_ns,(long long)p->down_ns,t->stage,t->ok,t->consumed,t->exception);
 }
}
static void multitouch_poll(void){
 static uint64_t last;uint64_t now=perf_now();if(now-last<1000000000ull)return;last=now;
#if defined(LUNARIA_TASK6F_PREFLIGHT) || defined(LUNARIA_TASK6K_OBSERVER)
 multitouch_unity_flush();
#endif
 const char *names[]={"multitouch-enable","multitouch-observe"};int flags[2]={0};
 for(unsigned i=0;i<2;i++){char path[256];snprintf(path,sizeof path,"/data/storage/el2/base/haps/entry/files/sdkdex/%s",names[i]);FILE *f=fopen(path,"wx");if(f){fputs("0\n",f);fclose(f);chmod(path,0666);}f=fopen(path,"r");if(f){if(fscanf(f,"%d",&flags[i])!=1)flags[i]=0;fclose(f);}}
 pthread_mutex_lock(&touch_lock);
 if(touch_head==touch_tail&&!multitouch_state.count){
  atomic_store(&multitouch_enabled,flags[0]==1);
  if(flags[0]==1){touch_head=touch_tail=0;touch_capacity=4096;}
 }
 atomic_store(&multitouch_observe,flags[1]==1);
 pthread_mutex_unlock(&touch_lock);
 if(!flags[1]&&mt_trace_head==mt_trace_tail&&mt_native_head==mt_native_tail)return;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/multitouch-events.csv","a");
 if(f){fseek(f,0,SEEK_END);if(ftell(f)==0)fputs("pid,seq,side,count,action,index,host_id,guest_id,x,y,down_ms,event_ms,callback_ns,enqueue_ns,drain_ns,construct_begin_ns,construct_ns,return_ns,host_event_ns,stream_down_ns,stage,ok,consumed,exception\n",f);
  for(unsigned n=0;n<1024;n++){
   mt_trace copy;pthread_mutex_lock(&touch_lock);
   bool ready=mt_trace_head!=mt_trace_tail&&mt_traces[mt_trace_head].returned;
   if(ready){copy=mt_traces[mt_trace_head];mt_trace_head=(mt_trace_head+1)%MT_TRACE_CAP;}
   pthread_mutex_unlock(&touch_lock);if(!ready)break;
   mt_write_packet(f,&copy,&copy.host,"host");mt_write_packet(f,&copy,&copy.event,"motion");
   if(copy.stage){FILE *e=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/multitouch-failures.txt","a");if(e){fprintf(e,"pid=%d seq=%llu ns=%llu stage=%d exception=%u errno=NA_guest_exception detail=%s\n",getpid(),(unsigned long long)copy.event.sequence,(unsigned long long)copy.returned,copy.stage,copy.exception,copy.text);fclose(e);}}
  }fclose(f);
 }
 f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/multitouch-native.csv","a");
 if(f){fseek(f,0,SEEK_END);if(ftell(f)==0)fputs("pid,seq,native_return_ns,accepted\n",f);
  for(unsigned n=0;n<1024;n++){uint64_t seq=0,ns=0;int accepted=0;pthread_mutex_lock(&touch_lock);if(mt_native_head!=mt_native_tail){seq=mt_native[mt_native_head].seq;ns=mt_native[mt_native_head].ns;accepted=mt_native[mt_native_head].accepted;mt_native_head=(mt_native_head+1)%MT_TRACE_CAP;}pthread_mutex_unlock(&touch_lock);if(!seq)break;fprintf(f,"%d,%llu,%llu,%d\n",getpid(),(unsigned long long)seq,(unsigned long long)ns,accepted);}fclose(f);
 }
 f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/multitouch-status.txt","w");
 if(f){uint64_t received,lost,native_lost,regressions,rejected[8],failures[9],full,dropped;unsigned active,depth,capacity;
  pthread_mutex_lock(&touch_lock);received=mt_received;lost=mt_trace_lost;native_lost=mt_native_lost;active=multitouch_state.count;depth=(touch_tail+TOUCH_QUEUE_CAP-touch_head)%TOUCH_QUEUE_CAP;capacity=TOUCH_QUEUE_CAP;regressions=mt_timestamp_regressions;full=touch_full_count;dropped=touch_dropped;memcpy(rejected,mt_rejected,sizeof rejected);memcpy(failures,mt_failures,sizeof failures);pthread_mutex_unlock(&touch_lock);
  fprintf(f,"pid=%d enabled=%d observe=%d received=%llu trace_lost=%llu native_lost=%llu active=%u queue=%u capacity=%u full=%llu dropped=%llu timestamp_regressions_preserved=%llu unpaired_definition=protocol_rejections_not_every_MOVE\n",getpid(),atomic_load(&multitouch_enabled),atomic_load(&multitouch_observe),(unsigned long long)received,(unsigned long long)lost,(unsigned long long)native_lost,active,depth,capacity,(unsigned long long)full,(unsigned long long)dropped,(unsigned long long)regressions);
  for(unsigned i=0;i<8;i++)fprintf(f,"reject_reason_%u=%llu\n",i,(unsigned long long)rejected[i]);for(unsigned i=0;i<9;i++)fprintf(f,"inject_stage_%u=%llu\n",i,(unsigned long long)failures[i]);fclose(f);}
}
