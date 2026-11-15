/* TASK6f: observe existing uGUI state only, at nativeRender return.
 * Requires the task6f helpers defined by task6f_download_preflight.inc.
 * No event construction, pointer mutation, handler invocation, or touch hook.
 * Metadata names/types were checked against original metadata v31 (see note).
 */
typedef struct {
 void *event_system,*module,*dictionary,*event;
 void *raycast,*press_raycast,*press,*raw_press,*last_press,*enter;
 float x,y;
 int pointer_id,lookup,fields;
} task6f_ugui_snapshot;
static struct {
 void *event_class,*object_class;
 void *ngui_class;
 task6f_ugui_snapshot previous[4];
 uint64_t last_log[4],last_status;
 unsigned long long ticks,logs;
 int started;
} task6f_ugui;

static void task6f_ugui_log(const char *fmt,...){
 int saved=errno;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task6f-ugui.txt","a");
 if(f){
  dprintf(fileno(f),"TASK6F_UGUI pid=%d tid=%ld mono_ms=%llu label=%s screen=(%.3f,%.3f) ",getpid(),(long)syscall(SYS_gettid),(unsigned long long)task6f_ms(),task6f.click_label,task6f.screen_x,task6f.screen_y);
  va_list a;va_start(a,fmt);vdprintf(fileno(f),fmt,a);va_end(a);dprintf(fileno(f),"\n");fclose(f);
 }
 errno=saved;
}

/* runtime_invoke boxes RaycastResult; il2cpp_field_get_value understands its
 * actual boxed layout. Do not assume a struct size, header size, or offset. */
static int task6f_ugui_raycast(void *event,const char *getter,void **game_object){
 *game_object=NULL;if(!event)return 0;
 int ok=0;
 void *boxed=task6f_call(task6f.api.object_class(event),event,getter,0,NULL,&ok);
 return ok&&boxed&&task6f_field(boxed,"m_GameObject",game_object);
}

static void task6f_ugui_name(void *obj,char *out,size_t cap){
 if(!cap)return;
 snprintf(out,cap,"%s",obj?"unknown":"<none>");
 if(!obj||!task6f_ugui.object_class)return;
 int ok=0;void *name=task6f_call(task6f_ugui.object_class,obj,"get_name",0,NULL,&ok);
 if(!ok||!name)return;
 task6f_string(name,out,cap);
 /* Keep one log line even for an unusual GameObject name. */
 for(char *p=out;*p;p++)if(*p=='\n'||*p=='\r'||*p=='\t'||*p=='"')*p=' ';
}

static void task6f_ugui_sample(unsigned slot,const char *source,task6f_ugui_snapshot *s,uint64_t now){
 if(s->event&&slot<3){
  float position[2]={0,0};
  if(task6f_field(s->event,"pointerId",&s->pointer_id))s->fields|=1;
  if(task6f_field(s->event,"position",position)){s->fields|=2;s->x=position[0];s->y=position[1];}
  if(task6f_field(s->event,"m_PointerPress",&s->press))s->fields|=4;
  if(task6f_field(s->event,"<rawPointerPress>k__BackingField",&s->raw_press))s->fields|=8;
  if(task6f_field(s->event,"<lastPress>k__BackingField",&s->last_press))s->fields|=16;
  if(task6f_field(s->event,"<pointerEnter>k__BackingField",&s->enter))s->fields|=32;
  if(task6f_ugui_raycast(s->event,"get_pointerCurrentRaycast",&s->raycast))s->fields|=64;
  if(task6f_ugui_raycast(s->event,"get_pointerPressRaycast",&s->press_raycast))s->fields|=128;
 }
 int changed=memcmp(&task6f_ugui.previous[slot],s,sizeof *s)!=0;
 if(!changed&&task6f_ugui.last_log[slot]&&now-task6f_ugui.last_log[slot]<1000)return;
 char ray[160],press_ray[160],press[160],raw[160],last[160],enter[160];
 task6f_ugui_name(s->raycast,ray,sizeof ray);task6f_ugui_name(s->press_raycast,press_ray,sizeof press_ray);
 task6f_ugui_name(s->press,press,sizeof press);task6f_ugui_name(s->raw_press,raw,sizeof raw);
 task6f_ugui_name(s->last_press,last,sizeof last);task6f_ugui_name(s->enter,enter,sizeof enter);
 task6f_ugui_log("sample=%llu tick=%llu changed=%d source=%s eventSystem=%p module=%p dictionary=%p lookup=%d event=%p pointerId=%d position=(%.3f,%.3f) fields=0x%x currentRaycast=%p name=\"%s\" pressRaycast=%p name=\"%s\" pointerPress=%p name=\"%s\" rawPointerPress=%p name=\"%s\" lastPress=%p name=\"%s\" pointerEnter=%p name=\"%s\" readonly=1 release_cleanup_may_hide_hit=1",
  ++task6f_ugui.logs,task6f_ugui.ticks,changed,source,s->event_system,s->module,s->dictionary,s->lookup,s->event,s->pointer_id,s->x,s->y,s->fields,
  s->raycast,ray,s->press_raycast,press_ray,s->press,press,s->raw_press,raw,s->last_press,last,s->enter,enter);
 task6f_ugui.previous[slot]=*s;task6f_ugui.last_log[slot]=now;
}

/* NGUI may be the active UI system. Only read fields whose actual runtime
 * attributes establish static storage; no guessed static-data offsets. */
static int task6f_ugui_read_static(void *klass,const char *name,void *out){
 if(!klass||!task6f.api.field_flags)return 0;
 void *field=task6f.api.field(klass,name);
 if(!field||!(task6f.api.field_flags(field)&0x10u))return 0;
 task6f.api.static_get(field,out);return 1;
}

static void task6f_ugui_ngui(uint64_t now){
 if(!task6f_ugui.ngui_class)task6f_ugui.ngui_class=task6f_class("","UICamera");
 task6f_ugui_snapshot s={0};s.pointer_id=-999;s.lookup=-1;
 void *klass=task6f_ugui.ngui_class;
 if(task6f_ugui_read_static(klass,"current",&s.module))s.fields|=1;
 if(task6f_ugui_read_static(klass,"mHover",&s.raycast))s.fields|=2;
 if(task6f_ugui_read_static(klass,"currentTouch",&s.event))s.fields|=4;
 if(s.event){
  float position[2]={0,0};s.lookup=1;
  if(task6f_field(s.event,"pos",position)){s.fields|=8;s.x=position[0];s.y=position[1];}
  if(task6f_field(s.event,"current",&s.enter))s.fields|=16;
  if(task6f_field(s.event,"pressed",&s.press))s.fields|=32;
  if(task6f_field(s.event,"last",&s.last_press))s.fields|=64;
  if(task6f_field(s.event,"dragged",&s.raw_press))s.fields|=128;
 }
 task6f_ugui_sample(3,"NGUI:mHover,currentTouch.current/pressed/last/dragged",&s,now);
}

static void task6f_ugui_tick(void){
 if(!task6f.ready)return;
 uint64_t now=task6f_ms();task6f_ugui.ticks++;
 if(!task6f_ugui.started){
  task6f_ugui.started=1;char comm[64]="unknown";
  FILE *f=fopen("/proc/thread-self/comm","r");if(f){if(fgets(comm,sizeof comm,f))comm[strcspn(comm,"\r\n")]=0;fclose(f);}
  task6f_ugui_log("observer_start thread=%s nativeRender_return=1 cadence=every_return log=change_or_1s existing_events_only=1",comm);
 }
 if(!task6f_ugui.event_class)task6f_ugui.event_class=task6f_class("UnityEngine.EventSystems","EventSystem");
 if(!task6f_ugui.object_class)task6f_ugui.object_class=task6f_class("UnityEngine","Object");
 task6f_ugui_ngui(now);
 if(!task6f_ugui.event_class){
  if(!task6f_ugui.last_status||now-task6f_ugui.last_status>=1000){task6f_ugui.last_status=now;task6f_ugui_log("status=unknown reason=EventSystem_class_not_found tick=%llu",task6f_ugui.ticks);}return;
 }
 int ok=0;void *system=task6f_call(task6f_ugui.event_class,NULL,"get_current",0,NULL,&ok),*module=NULL,*dictionary=NULL,*input=NULL;
 int module_found=system&&task6f_field(system,"m_CurrentInputModule",&module);
 int dict_found=module&&task6f_field(module,"m_PointerData",&dictionary);
 int input_found=module&&task6f_field(module,"m_InputPointerEvent",&input);
 if(!task6f_ugui.last_status||now-task6f_ugui.last_status>=1000){
  task6f_ugui.last_status=now;
  void *klass=module?task6f.api.object_class(module):NULL;
  const char *module_name=klass&&task6f.api.class_name?task6f.api.class_name(klass):"<none>";
  task6f_ugui_log("status=%s getter_ok=%d eventSystem=%p module_found=%d module=%p module_class=%s dictionary_found=%d dictionary=%p input_field_found=%d input_event=%p tick=%llu",
   ok&&system&&module?"observed":"unknown",ok,system,module_found,module,module_name,dict_found,dictionary,input_found,input,task6f_ugui.ticks);
 }
 task6f_ugui_snapshot s={0};s.event_system=system;s.module=module;s.dictionary=dictionary;s.event=input;s.pointer_id=-999;s.lookup=input_found?1:-1;
 task6f_ugui_sample(0,"module.m_InputPointerEvent",&s,now);
 const int ids[2]={-1,0};
 for(unsigned i=0;i<2;i++){
  task6f_ugui_snapshot d={0};d.event_system=system;d.module=module;d.dictionary=dictionary;d.pointer_id=ids[i];d.lookup=-1;
  if(dictionary){
   int id=ids[i];void *event=NULL;void *args[2]={&id,&event};
   void *result=task6f_call(task6f.api.object_class(dictionary),dictionary,"TryGetValue",2,args,&ok);
   void *value=ok&&result?task6f.api.unbox(result):NULL;
   if(value){d.lookup=*(uint8_t*)value?1:0;if(d.lookup)d.event=event;}
  }
  task6f_ugui_sample(i+1,i==0?"dictionary[-1]":"dictionary[0]",&d,now);
 }
}
