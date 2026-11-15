#ifndef SURFACE_VIEW_RUNTIME_H
#define SURFACE_VIEW_RUNTIME_H
#include "dvm/dvm.h"
#include <stdbool.h>
bool surface_view_runtime_vm_start(struct dvm *vm);
void surface_view_runtime_vm_shutdown(struct dvm *vm);
bool surface_view_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
/* Requires DVM GIL; binds exact live host token, does not deliver callbacks. */
bool surface_view_runtime_bind(struct dvm *,dvm_ref view,void *window);
/* Call only after real host mount, with verified host pixel format and GIL. */
bool surface_view_runtime_attached(struct dvm *, dvm_ref view, void *window, int actual_format);
#endif

