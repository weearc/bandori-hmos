#ifndef VIEW_STATE_RUNTIME_H
#define VIEW_STATE_RUNTIME_H
#include "dvm/dvm.h"
#include <stdbool.h>
#include <stddef.h>
/* One active VM at a time. Call start during isolated VM creation, or with
 * the DVM lock held, after framework Context setup; call shutdown only after all view calls and native callbacks
 * have stopped, before destroying this VM. */
bool view_state_runtime_vm_start(struct dvm *vm);
void view_state_runtime_vm_shutdown(struct dvm *vm);
bool view_state_runtime_call(struct dvm *vm,const char *owner,const char *method,
 const char *signature,dvm_ref receiver,const union dvm_value *args,int nargs,
 union dvm_value *out);
/* Guest-only stable DecorView identity; does not create or mount a host view. */
dvm_ref view_state_runtime_create_decor(struct dvm *vm,dvm_ref context);
/* GIL required. Snapshot is pre-order including root. Negative means invalid
 * tree or insufficient capacity. Attached is guest state, not rendering. */
int view_state_runtime_snapshot_tree(struct dvm *vm,dvm_ref root,dvm_ref *out,size_t cap);
bool view_state_runtime_set_attached(struct dvm *vm,dvm_ref root,bool attached);
/* callback returns 0 only when OS requests complete successfully, nonzero on
 * rejection. Callback/user must remain alive until VM and in-flight calls end.
 * Supported bits: HIDE_NAVIGATION, FULLSCREEN, LAYOUT_STABLE,
 * LAYOUT_HIDE_NAVIGATION, LAYOUT_FULLSCREEN; no immersive semantics. */
typedef int (*view_focus_host)(void *user,dvm_ref target);
void sdk_set_view_focus_host(view_focus_host callback,void *user);
typedef int (*view_system_ui_host)(void *user,int flags);
void sdk_set_view_system_ui_host(view_system_ui_host callback,void *user);
bool view_state_system_ui_selftest(struct dvm *vm,dvm_ref context);
/* Test with an existing valid Context. Retains no view entries after return. */
bool view_state_runtime_selftest(struct dvm *vm,dvm_ref context);
#endif

