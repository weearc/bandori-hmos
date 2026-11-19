/* DIAGNOSTIC ONLY -- never compiled into a user-facing build.
 *
 * This unit injects synthetic touch events through the system input service
 * (after the platform grants input-injection authorization).  It exists to
 * reproduce dropped touches and to compare input latency before/after a change.
 * A normal build must not contain it: guard the include site with
 * defined(BG_ENABLE_TOUCH_REPLAY) and leave that undefined in release builds.
 * It is not referenced anywhere in this tree by default.
 */
#if defined(BG_ENABLE_TOUCH_REPLAY)
// Public host input API only: OS routing -> XComponent -> existing guest path.
// Does not call queueTouch, queueMultiTouch or any guest/runtime entry point.
#include <multimodalinput/oh_input_manager.h>
#include <array>
static std::atomic<bool> replayRunning{false},replayStop{false};
static std::atomic<int> replayAuthorization{-1};
static uint64_t ReplayNow(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static void ReplayAuthorize(Input_InjectionStatus s){replayAuthorization=(int)s;}
struct ReplayRow {uint64_t rel,seq;int action,id,x,y;unsigned source;};
struct ReplayJob {uint64_t nonce;double speed;unsigned lead;};
static ReplayJob replayJob;
static void ReplayWriteStatus(const std::string& text){
 std::string path=std::string(RecDir)+"touch-replay-status.txt";
 FILE* f=RecOpen(path,"w");if(f){fprintf(f,"%s\n",text.c_str());fclose(f);}
}
static void ReplayRun(void*){
 const auto job=replayJob;std::vector<ReplayRow> rows;
 std::string prefix=std::string(RecDir)+"touch-replay-issued-"+std::to_string(job.nonce);
 FILE* f=fopen((std::string(RecDir)+"touch-replay-plan.csv").c_str(),"r");
 bool valid=f!=nullptr;char line[512];if(f)fgets(line,sizeof line,f);
 while(f&&fgets(line,sizeof line,f)){
  unsigned long long rel,seq;ReplayRow r{};char extra;
  if(rows.size()>=1000000||sscanf(line,"%llu,%d,%d,%d,%d,%llu,%u %c",&rel,&r.action,&r.id,&r.x,&r.y,&seq,&r.source,&extra)!=7||r.action<0||r.action>3||r.id<0||r.id>=10||r.x<0||r.y<0||(!rows.empty()&&rel<rows.back().rel)){valid=false;break;}
  r.rel=rel;r.seq=seq;rows.push_back(r);
 }if(f)fclose(f);
 if(!valid||rows.empty()){ReplayWriteStatus("nonce="+std::to_string(job.nonce)+" error=invalid_plan sent=0");replayRunning=false;return;}
 Input_InjectionStatus permission=UNAUTHORIZED;int query=OH_Input_QueryAuthorizedStatus(&permission);
 if(query||permission!=AUTHORIZED){ReplayWriteStatus("nonce="+std::to_string(job.nonce)+" error=os_authorization_required query_rc="+std::to_string(query)+" authorized_status="+std::to_string((int)permission)+" sent=0");replayRunning=false;return;}
 auto* event=OH_Input_CreateTouchEvent();FILE* out=event?RecOpen(prefix+".csv","x"):nullptr;
 if(!out){if(event)OH_Input_DestroyTouchEvent(&event);ReplayWriteStatus("nonce="+std::to_string(job.nonce)+" error=create_or_output_failed sent=0");replayRunning=false;return;}
 struct Result {uint64_t target,send,end;int rc;};std::vector<Result> result(rows.size());
 std::array<bool,10> active{};std::array<ReplayRow,10> last{};
 const uint64_t start=ReplayNow()+job.lead*1000000ull;int error=0;unsigned accepted=0;
 const int actions[]={TOUCH_ACTION_DOWN,TOUCH_ACTION_UP,TOUCH_ACTION_MOVE,TOUCH_ACTION_CANCEL};
 auto send=[&](const ReplayRow& r){
  OH_Input_SetTouchEventAction(event,actions[r.action]);OH_Input_SetTouchEventFingerId(event,r.id);
  OH_Input_SetTouchEventDisplayX(event,r.x);OH_Input_SetTouchEventDisplayY(event,r.y);
  OH_Input_SetTouchEventActionTime(event,ReplayNow()/1000);return OH_Input_InjectTouchEvent(event);
 };
 unsigned issued=0;
 for(unsigned i=0;i<rows.size()&&!replayStop.load();i++){
  uint64_t target=start+(uint64_t)(rows[i].rel/job.speed);
  // Bounded sleeps allow stop to release held fingers promptly even in a long
  // recorded gap. No diagnostic IO, allocation or guest/GIL access here.
  while(!replayStop.load()){
   uint64_t now=ReplayNow();if(now>=target)break;
   uint64_t delay=std::min<uint64_t>(target-now,2000000);timespec t{0,(long)delay};nanosleep(&t,nullptr);
  }
  if(replayStop.load())break;
  auto& r=rows[i];auto& res=result[i];res.target=target;res.send=ReplayNow();res.rc=send(r);res.end=ReplayNow();issued++;
  if(res.rc){error=res.rc;break;}accepted++;last[r.id]=r;
  if(r.action==0)active[r.id]=true;else if(r.action==1)active[r.id]=false;else if(r.action==3)active.fill(false);
 }
 unsigned cleanup=0;for(int id=0;id<10;id++)if(active[id]){last[id].action=1;send(last[id]);cleanup++;}
 OH_Input_DestroyTouchEvent(&event);
 fputs("index,source_row,seq,relative_ns,target_ns,send_ns,return_ns,rc,lateness_ns,action,pointer_id,x,y\n",out);
 for(unsigned i=0;i<issued;i++){
  const auto& r=rows[i];const auto& q=result[i];fprintf(out,"%u,%u,%llu,%llu,%llu,%llu,%llu,%d,%lld,%d,%d,%d,%d\n",i,r.source,(unsigned long long)r.seq,(unsigned long long)r.rel,(unsigned long long)q.target,(unsigned long long)q.send,(unsigned long long)q.end,q.rc,(long long)q.send-(long long)q.target,r.action,r.id,r.x,r.y);
 }
 bool io=ferror(out);if(fclose(out))io=true;
 ReplayWriteStatus("nonce="+std::to_string(job.nonce)+" planned="+std::to_string(rows.size())+" issued="+std::to_string(issued)+" os_accepted="+std::to_string(accepted)+" cleanup_events="+std::to_string(cleanup)+" error="+std::to_string(error)+" interrupted="+(replayStop.load()?"1":"0")+" io_error="+(io?"1":"0")+" file="+prefix+".csv");
 replayRunning.store(false,std::memory_order_release);
}
static void ReplayPoll(){
 static uint64_t seen=0;static bool authorizationRequested=false,initialized=false;
 FILE* f=fopen((std::string(RecDir)+"touch-replay-authorize").c_str(),"r");int auth=0;
 if(f){(void)fscanf(f,"%d",&auth);fclose(f);}
 if(initialized&&auth==1&&!authorizationRequested){
  int rc=OH_Input_RequestInjection(ReplayAuthorize);
  ReplayWriteStatus("authorization_request_rc="+std::to_string(rc)+" callback_status="+std::to_string(replayAuthorization.load()));
 }authorizationRequested=auth==1;
 FILE* cancel=fopen((std::string(RecDir)+"touch-replay-stop").c_str(),"r");int stopped=0;if(cancel){(void)fscanf(cancel,"%d",&stopped);fclose(cancel);}if(stopped==1)replayStop=true;
 if(replayRunning.load())return;
 f=fopen((std::string(RecDir)+"touch-replay-command").c_str(),"r");unsigned long long nonce=0;double speed=0;unsigned lead=0;char extra;
 bool ok=f&&fscanf(f,"%llu %lf %u %c",&nonce,&speed,&lead,&extra)==3;if(f)fclose(f);
 if(!initialized){seen=nonce;initialized=true;return;} // Never replay a stale command after cold start.
 if(!ok||nonce<=seen||!std::isfinite(speed)||speed<=0||speed>8||lead>600000)return;
 seen=nonce;replayJob={nonce,speed,lead};replayStop=false;replayRunning=true;
 auto* task=static_cast<ffrt_function_header_t*>(ffrt_alloc_auto_managed_function_storage_base(ffrt_function_kind_general));
 if(!task){replayRunning=false;ReplayWriteStatus("error=pool_allocation sent=0");return;}
 memset(task,0,sizeof *task);task->exec=ReplayRun;task->destroy=[](void*){};
 ffrt_task_attr_t attr;ffrt_task_attr_init(&attr);ffrt_task_attr_set_name(&attr,"os-touch-replay");
 ffrt_submit_base(task,nullptr,nullptr,&attr);ffrt_task_attr_destroy(&attr);
}
#endif /* BG_ENABLE_TOUCH_REPLAY */
