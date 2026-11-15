#ifndef ACTIVITY_RUNTIME_H
#define ACTIVITY_RUNTIME_H
#include "dvm/dvm_internal.h"
void activity_runtime_set_host(int (*mount)(void *, int), void *user);
void sdk_set_activity_host(int (*mount)(void *, int), void *user);
/* A callback must synchronously confirm the actual Activity window feature.
 * Only Android FEATURE_NO_TITLE (1) is currently requested. Return 0 only
 * after confirmation, >0 for unsupported, <0 on host failure. It runs without
 * the DVM lock; marshal to the UI thread if required. `user` and callback must
 * remain valid until all calls finish and the VM has stopped. */
typedef int (*activity_window_feature_host)(void *user, int feature);
void activity_runtime_set_window_feature_host(activity_window_feature_host callback, void *user);
void sdk_set_activity_window_feature_host(activity_window_feature_host callback, void *user);

void activity_runtime_vm_start(struct dvm *v);
bool activity_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out);
/* The Activity object created by activity_runtime_vm_start/Activity.<init>,
 * or 0 when v does not own the installed activity runtime. */
dvm_ref activity_runtime_current(struct dvm *v);
/* Bind a host-supplied launch Intent to the currently constructed Activity. */
bool activity_runtime_bind_intent(struct dvm *v, dvm_ref intent);
bool activity_runtime_selftest(struct dvm *v, unsigned *checks);
#endif
