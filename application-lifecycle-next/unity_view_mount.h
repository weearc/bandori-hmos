#ifndef UNITY_VIEW_MOUNT_H
#define UNITY_VIEW_MOUNT_H
#include "dvm/dvm.h"
#include <stdbool.h>
/* Called without DVM GIL. Success (0) transfers one owned window_runtime
 * token reference and a verified actual Android pixel format. On failure no
 * reference is transferred. Callback and user must outlive in-flight calls. */
typedef int (*unity_view_mount_host)(void *user,void **owned_window_token,int *actual_android_format);
void sdk_set_unity_view_mount_host(unity_view_mount_host callback,void *user);
/* GIL required. VM and host lifetime must extend through callbacks. A failed
 * partial attach is terminal; it is not safe to retry or destroy the VM. */
bool unity_view_mount(struct dvm *vm,dvm_ref root);
#endif
