/* Included only in the app-owned runtime. Diagnostics never own this VM. */
static _Atomic(struct dvm *) app_vm;
static atomic_int app_destroy_requested,app_window_ready,app_foreground;
static atomic_ullong app_vm_serial;
static atomic_int app_owner_claimed;
extern void dvm_lifetime_note(struct dvm *,const char *);
extern void login_panel_unbind(struct dvm *);
extern int login_panel_get_state(char *,size_t);

__attribute__((visibility("default"))) void sdk_app_vm_window_ready(void){atomic_store(&app_window_ready,1);}
__attribute__((visibility("default"))) void sdk_app_vm_set_foreground(int yes){atomic_store(&app_foreground,yes!=0);}
__attribute__((visibility("default"))) int sdk_app_vm_should_destroy(void){return atomic_load(&app_destroy_requested);}
__attribute__((visibility("default"))) int sdk_app_vm_is_foreground(void){return atomic_load(&app_foreground);}
__attribute__((visibility("default"))) void sdk_app_vm_destroy_request(void){atomic_store(&app_destroy_requested,1);}

static void app_owner_wait(struct dvm *vm){
 unsigned depth=dvm_gil_unlock_all(vm);
 struct timespec tick={0,100000000};nanosleep(&tick,NULL);
 dvm_gil_relock(vm,depth);
}
static void app_owner_publish(struct dvm *vm){
 atomic_store(&app_vm,vm);atomic_store(&app_vm_serial,vm->serial);
 dvm_lifetime_note(vm,"APP_VM_CREATED owner=EntryAbility");
}
static bool app_owner_wait_window(struct dvm *vm){
 while(!atomic_load(&app_destroy_requested)&&!atomic_load(&app_window_ready))app_owner_wait(vm);
 if(atomic_load(&app_destroy_requested))return false;
 dvm_lifetime_note(vm,"APP_VM_WINDOW_READY");return true;
}
static void app_owner_retire(struct dvm *vm){
 if(!vm)return;
 /* Boot failures retain ownership and evidence; a diagnostic cannot destroy it. */
 if(!atomic_load(&app_destroy_requested))dvm_lifetime_note(vm,"APP_VM_OWNER_IDLE awaiting_onDestroy");
 while(!atomic_load(&app_destroy_requested))app_owner_wait(vm);
 dvm_lifetime_note(vm,"APP_VM_DESTROY owner=EntryAbility.onDestroy");
 atomic_store(&app_vm,NULL);
 login_panel_unbind(vm);
 jni_bridge_shutdown(vm);dvm_destroy(vm);
}
static const char *app_diagnostic_snapshot(const char *kind){
 static _Thread_local char report[4096];char state[2048];
 login_panel_get_state(state,sizeof state);
 snprintf(report,sizeof report,"SDK_LOGIN_TRACE\nAPP_VM_DIAGNOSTIC kind=%s attached=%d vm=%p serial=%llu creates_vm=0 unbinds_vm=0\n%s\nRESULT login diagnostic stopped; app VM retained\n",
   kind,atomic_load(&app_vm)!=NULL,(void*)atomic_load(&app_vm),
   (unsigned long long)atomic_load(&app_vm_serial),state);
 return report;
}
__attribute__((visibility("default"))) const char *sdk_login_trace_run(const char *dir){(void)dir;return app_diagnostic_snapshot("login");}
__attribute__((visibility("default"))) const char *sdk_trace_run(const char *dir){(void)dir;return app_diagnostic_snapshot("sdk");}
