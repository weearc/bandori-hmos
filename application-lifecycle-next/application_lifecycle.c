#include "application_lifecycle.h"
#include "android_basics.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);
#define APP_CLASS "com/bilibili/star/bili/MainApplication"
static dvm_ref base_context;
static struct dvm *base_vm;
static bool application_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 if(!v||!visit||context!=&base_vm||base_vm!=v||!dvm_gil_held(v))return false;
 /* Duplicate process-owned alias of android_basics.base, never a weak cache. */
 if(base_context)visit(&base_context,user);return true;
}
void application_lifecycle_gc_vm_end(struct dvm *v){
 dvm_gc_root_provider_remove(v,"application-lifecycle/base",&base_vm);
 if(base_vm==v){base_context=0;base_vm=NULL;}
}
static struct dvm_method *method(struct dvm *v, const char *name, const char *sig) {
 struct dvm_method *m = dvm_lookup(v, APP_CLASS, name, sig);
 if (!m) sdk_trace_fprintf(stderr, "APPLICATION_LOOKUP_FAILED %s.%s%s\n", APP_CLASS, name, sig);
 return m;
}
dvm_ref sdk_application_create(struct dvm *v) {
 if(base_vm!=v){base_vm=v;base_context=0;}
 if(!dvm_gc_root_provider_add(v,"application-lifecycle/base",DVM_GC_ROOT_STRONG,application_gc_roots,&base_vm)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Application base GC root");return 0;
 }
 struct dvm_class *cls = dvm_find_class(v, APP_CLASS);
 if (!cls) { sdk_trace_fprintf(stderr, "APPLICATION_CLASS_MISSING %s\n", APP_CLASS); return 0; }
 dvm_ref app = dvm_new_object(v, cls);
 if (!app) { sdk_trace_fprintf(stderr, "APPLICATION_ALLOCATION_FAILED\n"); return 0; }
 base_context = android_basics_bind_application(v, app);
 if (!base_context) { sdk_trace_fprintf(stderr, "APPLICATION_BIND_FAILED\n"); return 0; }
 return app;
}
static bool invoke(struct dvm *v, const char *name, const char *sig, dvm_ref app,
                   const union dvm_value *args, int nargs) {
 struct dvm_method *m = method(v, name, sig);
 union dvm_value result = {0};
 return m && dvm_call(v, m, app, args, nargs, &result);
}
bool sdk_application_construct(struct dvm *v, dvm_ref app) {
 return invoke(v, "<init>", "()V", app, NULL, 0);
}
bool sdk_application_attach(struct dvm *v, dvm_ref app) {
 union dvm_value base = {.l = base_context};
 return base_context && invoke(v, "attachBaseContext", "(Landroid/content/Context;)V", app, &base, 1);
}
bool sdk_application_on_create(struct dvm *v, dvm_ref app) {
 return invoke(v, "onCreate", "()V", app, NULL, 0);
}
void sdk_application_router_state(struct dvm *v, bool *known, bool *has_init) {
 *known = false; *has_init = false;
 struct dvm_class *cls = dvm_find_class(v, "com/base/router/launcher/Router");
 if (!cls) return;
 union dvm_value value = {0};
 if (!dvm_get_static(v, cls, "hasInit", "Z", &value)) return;
 *known = true; *has_init = value.i != 0;
}
