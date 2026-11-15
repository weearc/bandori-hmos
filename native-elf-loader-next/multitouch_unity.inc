/* Read-only, once per actual nativeRender return. Unity Input exposes a
 * per-frame state, not a raw event stream; do not claim event-count equality. */
typedef struct {uint64_t ns;int count,id,phase;float x,y,dx,dy,dt;} mt_unity_sample;
static mt_unity_sample mt_unity_samples[16384];static unsigned mt_unity_used;
static uint64_t mt_unity_overflow,mt_unity_frames;static int mt_unity_metadata;
static void multitouch_unity_tick(void){
 if(!task6f.ready)return;
 if(!mt_unity_metadata&&perf_play_enabled()){
  mt_unity_metadata=1;int ok=0;void *app=task6f_class("UnityEngine","Application"),*quality=task6f_class("UnityEngine","QualitySettings");
  void *target=task6f_call(app,NULL,"get_targetFrameRate",0,NULL,&ok);int rate=ok&&target?*(int*)task6f.api.unbox(target):-999;
  void *vs=task6f_call(quality,NULL,"get_vSyncCount",0,NULL,&ok);int sync=ok&&vs?*(int*)task6f.api.unbox(vs):-999;
  FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/multitouch-unity-config.txt","w");if(f){fprintf(f,"pid=%d targetFrameRate=%d vSyncCount=%d readonly=1 touch_layout=field_metadata getters_on_nativeRender_return=1\n",getpid(),rate,sync);fclose(f);}
 }
 if(!atomic_load(&multitouch_observe))return;
 static void *input,*count_method,*touch_method;
 if(!input)input=task6f_class("UnityEngine","Input");
 if(!input)return;
 if(!count_method)count_method=task6f.api.method(input,"get_touchCount",0);
 if(!touch_method)touch_method=task6f.api.method(input,"GetTouch",1);
 if(!count_method||!touch_method)return;
 void *error=NULL,*box=task6f.api.invoke(count_method,NULL,NULL,&error);
 if(error||!box)return;
 int count=*(int32_t*)task6f.api.unbox(box);if(count<0||count>MT_MAX)return;
 mt_unity_frames++;uint64_t now=perf_now();
 for(int i=0;i<(count?count:1);i++){
  mt_unity_sample sample={.ns=now,.count=count,.id=-1,.phase=-1};
  if(count){void *args[]={&i};box=task6f.api.invoke(touch_method,NULL,args,&error);if(error||!box)continue;
   float xy[2]={0},delta[2]={0};
   if(!task6f_field(box,"m_FingerId",&sample.id)||!task6f_field(box,"m_Phase",&sample.phase)||!task6f_field(box,"m_Position",xy))continue;
   task6f_field(box,"m_PositionDelta",delta);task6f_field(box,"m_TimeDelta",&sample.dt);
   sample.x=xy[0];sample.y=xy[1];sample.dx=delta[0];sample.dy=delta[1];
  }
  /* Unity thread is the only producer. The existing poll flushes under GIL;
   * use a separate mutex because nativeRender observation runs without GIL. */
  pthread_mutex_lock(&touch_lock);
  if(mt_unity_used<16384)mt_unity_samples[mt_unity_used++]=sample;else mt_unity_overflow++;
  pthread_mutex_unlock(&touch_lock);
 }

}
static void multitouch_unity_flush(void){
 if(!atomic_load(&multitouch_observe)&&!mt_unity_used)return;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/multitouch-unity.csv","a");if(!f)return;
 fseek(f,0,SEEK_END);if(ftell(f)==0)fputs("pid,mono_ns,touchCount,fingerId,phase,x,y,deltaX,deltaY,deltaTime\n",f);
 /* Copy bounded chunks; no filesystem operation while holding touch_lock. */
 mt_unity_sample batch[256];
 for(unsigned n=0;n<64;n++){
  pthread_mutex_lock(&touch_lock);unsigned count=mt_unity_used>256?256:mt_unity_used;
  memcpy(batch,mt_unity_samples,count*sizeof *batch);mt_unity_used-=count;
  memmove(mt_unity_samples,mt_unity_samples+count,mt_unity_used*sizeof *batch);pthread_mutex_unlock(&touch_lock);
  for(unsigned i=0;i<count;i++){mt_unity_sample *s=&batch[i];fprintf(f,"%d,%llu,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.9f\n",getpid(),(unsigned long long)s->ns,s->count,s->id,s->phase,s->x,s->y,s->dx,s->dy,s->dt);}
  if(count<256)break;
 }fclose(f);
}
