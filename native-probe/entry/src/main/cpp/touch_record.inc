// Host-only observer. No game inspection or input delivery changes.
#include "touch_record_core.h"
#include "touch_record_format.h"
#include <ffrt/task.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <set>
#include <algorithm>
static touchrec::Ring recRing;
static std::atomic<bool> recStop{false},recRunning{false};
static constexpr const char* RecDir="/data/storage/el2/base/haps/entry/files/sdkdex/";
static void RecordTouch(const OH_NativeXComponent_TouchEvent& e,uint64_t seq,uint64_t ns){
 if(!recRing.enabled.load(std::memory_order_relaxed))return;
 touchrec::Row r{};r.recv_ns=ns;r.seq=seq;r.event_ns=e.timeStamp;
 r.action=(int)e.type;r.pointer_id=e.id;r.x=e.x;r.y=e.y;r.pressure=e.force;r.screen_x=e.screenX;r.screen_y=e.screenY;r.num_points=e.numPoints;
 for(unsigned i=0;i<e.numPoints&&i<touchrec::Points;i++){
  const auto& p=e.touchPoints[i];r.points[i]={p.id,p.isPressed?1:0,p.x,p.y,p.force,p.screenX,p.screenY};
 }
 recRing.push(r);
}
static FILE* RecOpen(const std::string& path,const char* mode){
 int flags=O_WRONLY|O_CREAT|O_CLOEXEC|(mode[0]=='x'?O_EXCL:O_TRUNC);
 int fd=open(path.c_str(),flags,0644);if(fd<0)return nullptr;
 if(fchmod(fd,0644)){close(fd);return nullptr;}
 FILE* f=fdopen(fd,"w");if(!f)close(fd);return f;
}
static void RecStatus(const std::string& status){
 FILE* f=RecOpen(std::string(RecDir)+"touch-record-status.txt","w");
 if(f){fprintf(f,"%s\n",status.c_str());fclose(f);}
}
static bool RecHighFrequencyFlush(){
 FILE* f=fopen((std::string(RecDir)+"touch-record-high-frequency-flush").c_str(),"r");int on=0;
 if(f){(void)fscanf(f,"%d",&on);fclose(f);}return on==1;
}
static bool RecRequested(){
 FILE* f=fopen((std::string(RecDir)+"touch-record-control").c_str(),"r");int on=0;
 if(f){(void)fscanf(f,"%d",&on);fclose(f);}return on==1;
}
#include "touch_replay_host.inc"
static void RecWorker(void*){
 FILE* out=nullptr;std::string path;bool previous=false,ioError=false,invalid=false;
 uint64_t count=0,first=0,last=0;unsigned serial=0;std::set<int> ids;
 double minx=0,maxx=0,miny=0,maxy=0;int width=0,height=0,maxPoints=0;uint64_t batteryAt=0;
 touchrec::FlushPolicy flushPolicy;bool highFrequencyFlush=false;uint64_t flushControlAt=0;
 auto drain=[&](){touchrec::Row r;
  while(recRing.pop(r)){
   if(!count){first=r.recv_ns;minx=maxx=r.x;miny=maxy=r.y;}last=r.recv_ns;count++;ids.insert(r.pointer_id);
   invalid|=r.num_points>(int)touchrec::Points||r.action<0||r.action>3;
   maxPoints=std::max(maxPoints,r.num_points);
   minx=std::min(minx,(double)r.x);maxx=std::max(maxx,(double)r.x);miny=std::min(miny,(double)r.y);maxy=std::max(maxy,(double)r.y);
   char line[touchrec::RowTextCapacity];int length=touchrec::format_row(line,sizeof line,r);
   if(length<0||(size_t)length>=sizeof line){ioError=true;continue;}
   size_t written=fwrite(line,1,(size_t)length,out);flushPolicy.bytes+=written;
   if(written!=(size_t)length)ioError=true;
  }
  ioError|=ferror(out)!=0;uint64_t now=ReplayNow();
  if(flushPolicy.due(now,highFrequencyFlush)){
   if(fflush(out))ioError=true;flushPolicy.flushed(now);
  }
 };
 auto finish=[&](){
  recRing.enabled.store(false,std::memory_order_release);
  while(recRing.writers.load(std::memory_order_acquire))std::this_thread::sleep_for(std::chrono::milliseconds(1));
  drain();if(fflush(out))ioError=true;if(fclose(out))ioError=true;out=nullptr;
  FILE* f=RecOpen(path+".summary.json","w");
  double duration=count>1?(last-first)/1e9:0;
   if(f){fprintf(f,"{\n\"schema\":1,\"source\":\"XComponent_original_events\",\"pid\":%d,\"events\":%llu,\"distinct_pointer_ids\":%zu,\"max_pointer_count\":%d,\"duration_s\":%.9f,\"events_per_second\":%.6f,\"first_recv_ns\":%llu,\"last_recv_ns\":%llu,\"x_range\":[%.9g,%.9g],\"y_range\":[%.9g,%.9g],\"surface_width\":%d,\"surface_height\":%d,\"ring_bytes\":%zu,\"dropped_recording_rows\":%llu,\"io_error\":%s,\"valid\":%s\n}\n",getpid(),(unsigned long long)count,ids.size(),maxPoints,duration,duration?count/duration:0,(unsigned long long)first,(unsigned long long)last,minx,maxx,miny,maxy,width,height,sizeof(recRing),(unsigned long long)recRing.lost.load(),ioError?"true":"false",(!ioError&&!invalid&&!recRing.lost.load())?"true":"false");fclose(f);}
  RecStatus(std::string("recording=0 file=")+path+" events="+std::to_string(count)+" lost="+std::to_string(recRing.lost.load())+" io_error="+(ioError?"1":"0"));
 };
 RecStatus("recording=0 default=off schema=1");
 while(!recStop.load()){
  bool requested=RecRequested();
  uint64_t controlNow=ReplayNow();
  if((requested||out)&&(!flushControlAt||controlNow-flushControlAt>=250000000ull)){
   highFrequencyFlush=RecHighFrequencyFlush();flushControlAt=controlNow;
  }
  if(requested&&!previous){
   recRing.head=0;recRing.tail=0;recRing.lost=0;count=first=last=0;maxPoints=0;ids.clear();ioError=invalid=false;
   time_t t=time(nullptr);tm tmv{};localtime_r(&t,&tmv);char name[100];strftime(name,sizeof name,"rec-%Y%m%dT%H%M%S",&tmv);
   path=std::string(RecDir)+name+"-"+std::to_string(getpid())+"-"+std::to_string(++serial)+".csv";
   out=RecOpen(path,"x");
   if(out){
    {std::lock_guard<std::mutex> g(nativeWindowLock);width=nativeWindowBaseWidth;height=nativeWindowBaseHeight;}
    fputs("recv_ns,action,pointer_id,x,y,pressure,seq,screen_x,screen_y,event_ns,num_points",out);
    for(unsigned i=0;i<touchrec::Points;i++)fprintf(out,",p%u_id,p%u_x,p%u_y,p%u_pressure,p%u_screen_x,p%u_screen_y,p%u_pressed",i,i,i,i,i,i,i);
    fputc('\n',out);if(fflush(out))ioError=true;
    flushPolicy.flushed(ReplayNow());
    RecStatus("recording=1 file="+path+" schema=1");recRing.epoch.fetch_add(1,std::memory_order_release);recRing.enabled.store(true,std::memory_order_release);
   }else RecStatus("recording=0 error=open_failed errno="+std::to_string(errno));
  }
  if(out)drain();
  if(!requested&&out)finish();
  ReplayPoll();
  if(out||replayRunning.load()){
   uint64_t ns=ReplayNow();if(ns-batteryAt>=1000000000ull){
    batteryAt=ns;static void* lib=dlopen("libohbattery_info.so",RTLD_NOW|RTLD_LOCAL);
    static auto capacity=lib?reinterpret_cast<int32_t(*)()>(dlsym(lib,"OH_BatteryInfo_GetCapacity")):nullptr;
    int level=capacity?capacity():-1;
    if(level>=0&&level<=30){
     recRing.enabled=false;replayStop=true;recStop=true;
     FILE* f=RecOpen(std::string(RecDir)+"NEED-USER.txt","w");if(f){fprintf(f,"REC-REPLAY-R1 stopped: battery=%d <=30\n",level);fclose(f);}
    }
   }
  }
  previous=requested;
  std::this_thread::sleep_for(std::chrono::milliseconds(out||replayRunning.load()?20:250));
 }
 if(out)finish();recRunning.store(false,std::memory_order_release);
}
static void RecDestroy(void*){}
static void RecCleanup(void*){
 recStop=true;while(recRunning.load(std::memory_order_acquire))std::this_thread::sleep_for(std::chrono::milliseconds(1));
 replayStop=true;while(replayRunning.load(std::memory_order_acquire))std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
static void RecStart(napi_env env){
 if(recRunning.exchange(true))return;recStop=false;
 auto* f=static_cast<ffrt_function_header_t*>(ffrt_alloc_auto_managed_function_storage_base(ffrt_function_kind_general));
 if(!f){recRunning=false;return;}memset(f,0,sizeof *f);f->exec=RecWorker;f->destroy=RecDestroy;
 ffrt_task_attr_t a;ffrt_task_attr_init(&a);ffrt_task_attr_set_name(&a,"touch-record");
 ffrt_submit_base(f,nullptr,nullptr,&a);ffrt_task_attr_destroy(&a);
 napi_add_env_cleanup_hook(env,RecCleanup,nullptr);
}
