/* TASK8a opt-in observation. Only the verified original MethodInfo entry is
 * instrumented. Arguments/result and the entire original body are preserved.
 * No business method is invoked by this observer. Existing metadata/array
 * readers are used; no EventDelegate.Cache or delegate construction is forced.
 * Unsupported bytes or cached-delegate ABI values fail closed. */
static uintptr_t task8a_tap_continuation __attribute__((used));
static const void *task8a_tap_method;
static int task8a_trace_attempted,task8a_trace_installed;
static uintptr_t task8a_finish_continuation;
/* Per-render observations, including after the older observer's sample cap.
 * Direct/cached calls can bypass MethodInfo hooks: never infer a call from state. */
static void path_home_title_tick(void){
 static void *previous_instance,*previous_enumerator;static int previous=-999;static uint64_t last_pv;
 void *title=NULL,*enumerator=NULL;int state=-1;
 if(!task6k_static(task6k.title,"instance",&title)||!title||!task6f_field(title,"processState",&state))return;
 task6f_field(title,"enumerator",&enumerator);
 if(title!=previous_instance||state!=previous||enumerator!=previous_enumerator){
  FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/path-home-title.txt","a");
  if(f){fprintf(f,"PATH_HOME pid=%d mono_ms=%llu process_previous=%d process=%d enumerator=%s sample=post_render callsite=not_observed readonly=1\n",getpid(),(unsigned long long)task6f_ms(),previous,state,task6k_classname(enumerator));fclose(f);}
  previous_instance=title;previous=state;previous_enumerator=enumerator;
  extern void login_panel_title_progress(int,uint64_t);
  login_panel_title_progress(state,task6f_ms());
 }
 if(task6k.active&&task6f_ms()-last_pv>=5000){
  last_pv=task6f_ms();void *button=NULL;
  int found=task6f_field(title,"openingMoviePlayButton",&button);
  task6k_log("PV field=openingMoviePlayButton found=%d ref=%p class=%s process=%d",found,button,task6k_classname(button),state);
  if(button)task6k_binding(button,"onClick");
 }
}
static void task8a_title_method_log(const char *name,const char *stage,void *self,uint64_t elapsed){
 int saved_errno=errno;
 int process=-1,asset=-1;void *enumerator=NULL;
 int pf=task6f_field(self,"processState",&process),af=task6f_field(self,"assetBundleLoadState",&asset);
 task6f_field(self,"enumerator",&enumerator);
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task8a-title.txt","a");
 if(f){fprintf(f,"TASK8A pid=%d tid=%ld mono_ms=%llu method=TitleController.%s stage=%s process_found=%d process=%d asset_found=%d asset=%d enumerator=%p duration_ms=%llu original_body=1\n",getpid(),(long)syscall(SYS_gettid),(unsigned long long)task6f_ms(),name,stage,pf,process,af,asset,enumerator,(unsigned long long)elapsed);fclose(f);}
 errno=saved_errno;
}
static void task8a_tap_entry(void *self,const void *method){
 uint64_t start=task6f_ms();task8a_title_method_log("TouchToStartPressed","enter",self,0);
 ((void (*)(void *,const void *))task8a_tap_continuation)(self,method);
 task8a_title_method_log("TouchToStartPressed","return",self,task6f_ms()-start);
}
static void task8a_finish_entry(void *self,const void *method){
 uint64_t start=task6f_ms();task8a_title_method_log("onFinishTouchToStartAnimation","enter",self,0);
 ((void (*)(void *,const void *))task8a_finish_continuation)(self,method);
 task8a_title_method_log("onFinishTouchToStartAnimation","return",self,task6f_ms()-start);
}
static void task8a_finish_setup(void){
 const void *method=task6f.api.method(task6k.title,"onFinishTouchToStartAnimation",0);uintptr_t address=0;
 if(method&&probe_range_perm((uintptr_t)method,sizeof address,'r'))memcpy(&address,method,sizeof address);
 /* Original ELF RVA 0x2d9fd70: stp/stp/adrp/ldrb, exact objdump words. */
 const uint32_t expected[4]={0xa9be57fe,0xa9014ff4,0x9001d7d4,0x3973c288};int result=-1;
 if(address&&probe_range_perm(address,sizeof expected,'r')&&probe_range_perm(address,sizeof expected,'x')&&
    !memcmp((void*)address,expected,sizeof expected)&&!((uintptr_t)method%sizeof(uintptr_t))&&
    probe_range_perm((uintptr_t)method,sizeof(uintptr_t),'w')){
  task8a_finish_continuation=address;uintptr_t original=address;
  if(__atomic_compare_exchange_n((uintptr_t*)method,&original,(uintptr_t)&task8a_finish_entry,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))result=1;
 }
 int saved_errno=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task8a-title.txt","a");
 if(f){fprintf(f,"TASK8A pid=%d mono_ms=%llu stage=trace_install ok=%d method=TitleController.onFinishTouchToStartAnimation address=%p original_rva=0x2d9fd70 exact_prologue_check=1 methodinfo_data_slot_only=1 static_direct_and_cached_delegate_coverage=incomplete executable_pages_changed=0 persisted_binary_changed=0\n",getpid(),(unsigned long long)task6f_ms(),result,(void*)address);fclose(f);}errno=saved_errno;
}
/* Primitive getter results are boxed by runtime_invoke. No AnimatorStateInfo
 * field offsets, native return ABI, or guessed struct layout are used. */
static void task8a_getter_text(void *klass,void *receiver,const char *getter,const char *type,char *out,size_t cap){
 snprintf(out,cap,"unknown");int ok=0;void *box=task6f_call(klass,receiver,getter,0,NULL,&ok);
 if(!ok||!box)return;void *bc=task6f.api.object_class(box);const char *name=bc?task6f.api.class_name(bc):NULL;
 if(!name||strcmp(name,type))return;void *value=task6f.api.unbox(box);size_t width=!strcmp(type,"Boolean")?1:4;
 if(!value||!probe_range_perm((uintptr_t)value,width,'r'))return;
 if(!strcmp(type,"Single")){float x;memcpy(&x,value,4);snprintf(out,cap,"%.9g",(double)x);}
 else if(width==1){uint8_t x;memcpy(&x,value,1);snprintf(out,cap,"%s",x?"true":"false");}
 else{int32_t x;memcpy(&x,value,4);snprintf(out,cap,"%d",x);}
}
static void task8a_animator_tick(void){
 static uint64_t last;static unsigned samples;uint64_t now=task6f_ms();
 if(samples>=20||!task6k.title||(last&&now-last<5000))return;
 void *title=NULL;int process=-1;if(!task6k_static(task6k.title,"instance",&title)||!title||!task6f_field(title,"processState",&process)||process!=2)return;
 last=now;samples++;int saved_errno=errno;const char *fields[]={"titleAnimator","titleLogoAnimator"};
 for(unsigned i=0;i<2;i++){
  void *animator=NULL;int found=task6f_field(title,fields[i],&animator);void *klass=animator?task6f.api.object_class(animator):NULL;
  char enabled[32]="unknown",speed[32]="unknown",mode[32]="unknown",hash[32]="unknown",time[32]="unknown",length[32]="unknown",loop[32]="unknown";int state_ok=0;uint32_t state_pin=0;
  if(klass){
   task8a_getter_text(klass,animator,"get_enabled","Boolean",enabled,sizeof enabled);
   task8a_getter_text(klass,animator,"get_speed","Single",speed,sizeof speed);
   /* Enum boxes retain their enum class name; its underlying value is Int32. */
   task8a_getter_text(klass,animator,"get_updateMode","AnimatorUpdateMode",mode,sizeof mode);
   int layer=0;void *args[]={&layer};void *state=task6f_call(klass,animator,"GetCurrentAnimatorStateInfo",1,args,&state_ok);
   if(state_ok&&state){
    state_pin=task6f.api.pin(state,false);void *sc=task6f.api.object_class(state);const char *sn=sc?task6f.api.class_name(sc):NULL;
    void *receiver=sn&&!strcmp(sn,"AnimatorStateInfo")?task6f.api.unbox(state):NULL;
    if(receiver&&state_pin){
     task8a_getter_text(sc,receiver,"get_fullPathHash","Int32",hash,sizeof hash);
     task8a_getter_text(sc,receiver,"get_normalizedTime","Single",time,sizeof time);
     task8a_getter_text(sc,receiver,"get_length","Single",length,sizeof length);
     task8a_getter_text(sc,receiver,"get_loop","Boolean",loop,sizeof loop);
    }else state_ok=0;
   }else state_ok=0;
  }
  FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task8a-title.txt","a");
  if(f){fprintf(f,"TASK8A pid=%d tid=%ld mono_ms=%llu stage=animator_sample sample=%u process=2 field=%s found=%d animator=%p enabled=%s speed=%s updateMode=%s state_getter_ok=%d fullPathHash=%s normalizedTime=%s length=%s loop=%s state_layout=reflection_getters unknown_is_not_zero=1\n",getpid(),(long)syscall(SYS_gettid),(unsigned long long)now,samples,fields[i],found,animator,enabled,speed,mode,state_ok,hash,time,length,loop);fclose(f);}
  if(state_pin)task6f.api.unpin(state_pin);
 }errno=saved_errno;
}
static void task8a_cached_delegate_log(const char *reason,void *delegate,uintptr_t *slot,uintptr_t previous,int changed){
 static unsigned skipped;int saved_errno=errno;
 if(!changed&&skipped++>=24){errno=saved_errno;return;}
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task8a-title.txt","a");
 if(f){fprintf(f,"TASK8A pid=%d mono_ms=%llu stage=cached_delegate_trace reason=%s delegate=%p slot=%p previous=%p replacement=%p changed=%d executable_pages_changed=0 forced_Cache=0\n",getpid(),(unsigned long long)task6f_ms(),reason,delegate,(void*)slot,(void*)previous,(void*)&task8a_tap_entry,changed);fclose(f);}
 errno=saved_errno;
}
static void task8a_cached_delegate_tick(void){
 static uint64_t last;uint64_t now=task6f_ms();
 if(!task8a_tap_continuation||!task8a_tap_method||!task6k.title||(last&&now-last<1000))return;last=now;
 void *title=NULL;if(!task6k_static(task6k.title,"instance",&title)||!title)return;
 const char *buttons[]={"titleButton","titleAnimationButton","titleTextureButton"};
 for(unsigned b=0;b<sizeof buttons/sizeof buttons[0];b++){
  void *button=NULL,*list=NULL,*items=NULL;int size=-1;
  if(!task6f_field(title,buttons[b],&button)||!button||!task6f_field(button,"onClick",&list)||!list)continue;
  if(!task6f_field(list,"_items",&items))task6f_field(list,"buffer",&items);
  if(!task6f_field(list,"_size",&size))task6f_field(list,"size",&size);
  if(!items||size<0||size>24)continue;
  for(int i=0;i<size;i++){
   void *event=task6k_array(items,(uintptr_t)i),*target=NULL,*text=NULL,*delegate=NULL;char name[96];
   if(!event||strcmp(task6k_classname(event),"EventDelegate")||!task6f_field(event,"mTarget",&target)||target!=title||!task6f_field(event,"mMethodName",&text)||!text)continue;
   task6k_text(text,name,sizeof name);if(strcmp(name,"TouchToStartPressed"))continue;
   void *cache_field=NULL;for(void *c=task6f.api.object_class(event);c&&!cache_field;c=task6f.api.parent(c))cache_field=task6f.api.field(c,"mCachedCallback");
   /* The exact original Execute reads mCachedCallback at +48, then calls
    * invoke_impl +24 with x0=delegate[+64], x1=delegate[+40]. Validate this
    * specific closed-instance shape; do not assume a generic delegate ABI. */
   if(!cache_field||task6f.api.offset(cache_field)!=48){task8a_cached_delegate_log("cache_field_layout_unverified",NULL,NULL,0,0);continue;}
   task6f.api.get(event,cache_field,&delegate);
   if(!delegate){task8a_cached_delegate_log("not_cached_retry",NULL,NULL,0,0);continue;}
   if(!probe_range_perm((uintptr_t)delegate,72,'r')){task8a_cached_delegate_log("delegate_unreadable",delegate,NULL,0,0);continue;}
   uintptr_t invoke=0,argument_target=0,argument_method=0;
   memcpy(&invoke,(char*)delegate+24,sizeof invoke);memcpy(&argument_target,(char*)delegate+64,sizeof argument_target);memcpy(&argument_method,(char*)delegate+40,sizeof argument_method);
   uintptr_t *slot=(uintptr_t*)((char*)delegate+24);
   if(argument_target!=(uintptr_t)title||argument_method!=(uintptr_t)task8a_tap_method||
      (invoke!=task8a_tap_continuation&&invoke!=(uintptr_t)&task8a_tap_entry)){
    task8a_cached_delegate_log("closed_instance_identity_unverified",delegate,slot,invoke,0);continue;
   }
   if(invoke==(uintptr_t)&task8a_tap_entry)continue;
   if((uintptr_t)slot%sizeof(uintptr_t)||!probe_range_perm((uintptr_t)slot,sizeof *slot,'w')){task8a_cached_delegate_log("slot_not_writable",delegate,slot,invoke,0);continue;}
   uintptr_t expected=task8a_tap_continuation;
   int changed=__atomic_compare_exchange_n(slot,&expected,(uintptr_t)&task8a_tap_entry,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
   task8a_cached_delegate_log(changed?"verified_closed_instance":"slot_changed_retry",delegate,slot,invoke,changed);
  }
 }
}
static void task8a_title_trace_setup(void){
 if(task6k.title)path_home_title_tick();
 if(task8a_trace_attempted){task8a_cached_delegate_tick();task8a_animator_tick();return;}if(!task6k.title)return;task8a_trace_attempted=1;
 const void *method=task6f.api.method(task6k.title,"TouchToStartPressed",0);uintptr_t address=0;
 if(method)memcpy(&address,method,sizeof address);
 const uint32_t expected[4]={0xd102c3ff,0x6d0623e9,0xa90767fe,0xa9085ff8};
 int result=-1;
 /* Instrument only the writable MethodInfo entry. Never change executable
  * pages or memory protection. Already cached/direct native calls may bypass
  * this slot; only actual enter/return logs prove execution coverage. */
 if(address&&probe_range_perm(address,sizeof expected,'x')&&
    !memcmp((void*)address,expected,sizeof expected)&&
    ((uintptr_t)method%sizeof(uintptr_t))==0&&
    probe_range_perm((uintptr_t)method,sizeof(uintptr_t),'w')){
  task8a_tap_continuation=address;
  task8a_tap_method=method;
  uintptr_t original=address;
  if(__atomic_compare_exchange_n((uintptr_t*)method,&original,(uintptr_t)&task8a_tap_entry,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){result=1;task8a_trace_installed=1;}
 }
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task8a-title.txt","a");if(f){fprintf(f,"TASK8A pid=%d mono_ms=%llu stage=trace_install ok=%d method=TitleController.TouchToStartPressed address=%p original_rva=0x2d9dcbc exact_prologue_check=1 methodinfo_data_slot_only=1 cached_direct_delegate_coverage=not_assumed executable_pages_changed=0 persisted_binary_changed=0\n",getpid(),(unsigned long long)task6f_ms(),result,(void*)address);fclose(f);}
 task8a_finish_setup();task8a_cached_delegate_tick();task8a_animator_tick();
}
