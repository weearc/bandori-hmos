"""Build the app-owned runtime; finite diagnostic builds remain unchanged."""
def player_source(body):
    assert body.count('for(int i=0;perf_now()<owner_deadline;i++)')==1
    body=body.replace(' bool submission_reported=false;', ''' extern int sdk_app_vm_should_destroy(void),sdk_app_vm_is_foreground(void);
 extern void dvm_lifetime_note(struct dvm *,const char *);
 bool app_paused=false;
 bool resume_started=false,resume_fault=false,resume_verify=false;
 uint64_t resume_since=0,resume_steps=0,resume_submissions=0,owner_health=0;
 extern int activity_runtime_resume_flag(const char *);
 dvm_lifetime_note(s->vm,"APP_VM_DRIVER_BEGIN");
 bool submission_reported=false;''',1)
    body=body.replace('for(int i=0;perf_now()<owner_deadline;i++)','for(uint64_t i=0;!sdk_app_vm_should_destroy()&&!dvm_is_stopping(s->vm);i++)',1)
    body=body.replace('  drain_touches(s);', '''  extern void login_panel_poll(struct dvm *,dvm_ref);
  if(!app_paused){if(owner_poll_tick)login_panel_poll(s->vm,s->activity);drain_touches(s);}
#ifdef LUNARIA_TASK8A
  task8a_beat(2);
#endif
''',1)
    body=body.replace('(i+1)/10);log_threads','(int)((i+1)/10));log_threads',1)
    body=body.replace('  unsigned long long submitted=egl_window_runtime_successful_submissions();', '''  unsigned long long submitted=egl_window_runtime_successful_submissions();
  if(owner_now-owner_health>=1000000000ull){owner_health=owner_now;
  if(activity_runtime_resume_flag("freeze180-observe")){
   extern void result_black_wait_snapshot(void);result_black_wait_snapshot();
   FILE *health=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/freeze180-heartbeat.csv","a");
   if(health){int life,started,resumed;extern void activity_runtime_lifecycle_snapshot(struct dvm *,int *,int *,int *);activity_runtime_lifecycle_snapshot(s->vm,&life,&started,&resumed);
    fprintf(health,"%d,%llu,%llu,%llu,%llu,%d,%d,%d,%d\\n",getpid(),(unsigned long long)perf_now(),submitted,egl_window_runtime_last_submission_ns(),(unsigned long long)s->vm->steps,app_paused,life,started,resumed);fclose(health);}
  }}''',1)
    body=body.replace('owner_threads=owner_now;log_threads', 'owner_threads=owner_now;dvm_lifetime_note(s->vm,app_paused?"APP_VM_BACKGROUND_HEARTBEAT":"APP_VM_FOREGROUND_HEARTBEAT");log_threads',1)
    start=body.index('   int next_focus=0;')
    end=body.index('\n  }\n }\n unity_attach_debug_snapshot("frame_wait_end");',start)
    body=body[:start]+'''   int host_foreground=sdk_app_vm_is_foreground();
   if(!host_foreground&&!app_paused){
    focus.i=0;lifecycle_call(s,"onWindowFocusChanged","(Z)V",&focus,1);
    bool paused=lifecycle_call(s,"onPause","()V",NULL,0);
    bool stopped=paused&&lifecycle_call(s,"onStop","()V",NULL,0);
    app_paused=true;focused=0;
    resume_started=false;resume_verify=false;
    dvm_lifetime_note(s->vm,stopped?"APP_VM_BACKGROUND retained=1":"APP_VM_BACKGROUND lifecycle_error retained=1");
   }else if(host_foreground&&app_paused){
    int guarded=activity_runtime_resume_flag("resume-guard");
    bool blocked=guarded&&resume_fault;
    bool started=!blocked&&((guarded&&resume_started)||lifecycle_call(s,"onStart","()V",NULL,0));
    if(guarded&&started)resume_started=true;
    bool resumed=started&&lifecycle_call(s,"onResume","()V",NULL,0);
    if(resumed){app_paused=false;dvm_lifetime_note(s->vm,"APP_VM_RESUMED retained=1");
     resume_verify=guarded;resume_since=perf_now();resume_steps=s->vm->steps;resume_submissions=egl_window_runtime_successful_submissions();
    }else if(guarded&&!blocked){resume_fault=true;
     require_record(s,fprintf(s->file,"event=RESUME_FAIL_FAST attempts=1 started_completed=%d restart_required=1 in_process_rebuild_unsafe=1\\n",resume_started));
     dvm_lifetime_note(s->vm,"RESUME_FAIL_FAST no_infinite_retry restart_required=1");
    }
   }
   if(resume_verify){
    uint64_t frames=egl_window_runtime_successful_submissions()-resume_submissions;
    if(frames&&s->vm->steps>resume_steps){require_record(s,fprintf(s->file,"event=RESUME_FRAMES_OK frames=%llu steps_delta=%llu elapsed_ms=%.3f\\n",(unsigned long long)frames,(unsigned long long)(s->vm->steps-resume_steps),(perf_now()-resume_since)/1e6));resume_verify=false;}
    else if(perf_now()-resume_since>5000000000ull){require_record(s,fprintf(s->file,"event=RESUME_FRAME_TIMEOUT frames=%llu elapsed_ms=%.3f\\n",(unsigned long long)frames,(perf_now()-resume_since)/1e6));resume_verify=false;}
   }
   if(!app_paused){
    int next_focus=0;
    if(foreground_observation(s->vm,&showing,&next_focus)&&next_focus!=focused){
     focus.i=next_focus;lifecycle_call(s,"onWindowFocusChanged","(Z)V",&focus,1);focused=next_focus;
    }
   }'''+body[end:]
    anchor=' unity_attach_debug_snapshot("frame_wait_end");'
    body=body.replace(anchor,''' dvm_lifetime_note(s->vm,"APP_VM_DRIVER_END");
 if(!app_paused){lifecycle_call(s,"onPause","()V",NULL,0);lifecycle_call(s,"onStop","()V",NULL,0);}
 lifecycle_call(s,"onDestroy","()V",NULL,0);
'''+anchor,1)
    assert 'login_panel_unbind' not in body
    return body

def runtime_source(text):
    # Remove the independent eight-VM diagnostic batch, not just its public name.
    start=text.index('__attribute__((visibility("default"))) const char *sdk_trace_run(')
    end=text.index('/* Original Activity entry probe.',start)
    text=text[:start]+text[end:]
    marker='__attribute__((visibility("default"))) const char *sdk_login_trace_run(const char *dir) {'
    assert text.count(marker)==1
    text=text.replace(marker,'''#include <stdatomic.h>
#include <time.h>
#include "app_vm_owner.inc"
__attribute__((visibility("default"))) const char *sdk_app_vm_run(const char *dir) {
 perf_diag_start(dir);
 struct dvm *v=NULL;
 if(atomic_exchange(&app_owner_claimed,1))return app_diagnostic_snapshot("owner_already_created");''',1)
    text=text.replace(' certificate_batch_run();','',1)
    text=text.replace(' if(!battery_runtime_selftest()){sdk_trace_fprintf(stderr,"RESULT BLOCKED battery fixture failed; original SDK not executed\\n");goto done;}','',1)
    text=text.replace('||!java_basics_selftest()','',1)
    assert text.count('struct dvm *v=dvm_create(&h);')==1
    text=text.replace('struct dvm *v=dvm_create(&h);','v=dvm_create(&h);\n if(v){app_owner_publish(v);if(!app_owner_wait_window(v))goto done;}',1)
    start=text.index(' extern bool activity_runtime_task4_selftest',text.index('sdk_app_vm_run'))
    end=text.index(' #ifdef LUNARIA_UNITY_BOOTSTRAP',start) if ' #ifdef LUNARIA_UNITY_BOOTSTRAP' in text[start:] else text.index('#ifdef LUNARIA_UNITY_BOOTSTRAP',start)
    text=text[:start]+text[end:]
    # All failure exits retain the owner VM until the real onDestroy request.
    start=text.index('\ncleanup:',text.index('sdk_app_vm_run'))
    end=text.index('\ndone:',start)
    text=text[:start]+'\ncleanup:\n goto done;'+text[end:]
    text=text.replace('\ndone:\n progress_note(', '\ndone:\n app_owner_retire(v);\n progress_note(',1)
    assert text.count('dvm_create(&h)')==1
    assert 'dvm_destroy(v)' not in text
    return text
