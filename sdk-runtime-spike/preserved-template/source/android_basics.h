#pragma once
#include "dvm/dvm_internal.h"
void android_basics_vm_start(struct dvm *);
dvm_ref android_basics_application(struct dvm *);
/* Bind the single original guest Application instance to this VM. The host
 * owns a distinct base Context; it is returned for Application.attachBaseContext.
 * Binding does not execute any original lifecycle method. */
dvm_ref android_basics_bind_application(struct dvm *, dvm_ref);
dvm_ref android_basics_base_context(struct dvm *);
bool android_basics_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool android_basics_selftest(struct dvm *,unsigned *);
/* Main-Looper post/drain/removal coverage; must run on the VM main thread. */
bool android_basics_handler_selftest(struct dvm *,unsigned *);

/* Requires a live VM on its guest-main Looper thread; tests listener bookkeeping only. */
bool android_basics_phone_listener_selftest(struct dvm *vm,unsigned *checks);
