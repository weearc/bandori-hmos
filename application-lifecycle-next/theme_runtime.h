#ifndef THEME_RUNTIME_H
#define THEME_RUNTIME_H
#include "dvm/dvm.h"
#include <stdbool.h>
/* All APIs require the DVM GIL; caller proves theme attribute provenance. */
bool theme_runtime_vm_start(struct dvm *vm);
void theme_runtime_vm_shutdown(struct dvm *vm);
/* state: -1 unresolved, 0 absent, 1 false, 2 true. */
dvm_ref theme_runtime_create(struct dvm *vm,int state);
/* Original APK UnityThemeSelector with AOSP Android 9 API 28 parent chain. */
dvm_ref theme_runtime_create_unity_theme(struct dvm *vm);
bool theme_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool theme_runtime_selftest(struct dvm *vm);
#endif



