#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INTENT_LAUNCH_HOST_ABI_VERSION 1u

typedef enum intent_launch_operation {
 INTENT_LAUNCH_START = 1,
 INTENT_LAUNCH_START_FOR_RESULT = 2,
 INTENT_LAUNCH_OVERRIDE_TRANSITION = 3
} intent_launch_operation;

/* Strings are borrowed from the guest VM and valid only during the callback. */
typedef struct intent_launch_request_v2 {
 uint32_t abi_version;
 uint32_t struct_size;
 uint32_t operation;
 uint32_t caller_ref;
 uint32_t intent_ref;
 uint32_t flags;
 const char *action;
 const char *package_name;
 const char *class_name;
 int32_t request_code;
 int32_t enter_animation;
 int32_t exit_animation;
} intent_launch_request_v2;

/* Return 0 only when the launch request was truly received, 1 when unsupported,
 * and any other value for a runtime failure. Acceptance does not mean an
 * Activity was created. Borrowed strings expire when this call returns. */
typedef int (*intent_launch_host_v2_fn)(void *user,
 const intent_launch_request_v2 *request);
void intent_runtime_set_launch_host_v2(intent_launch_host_v2_fn launch, void *user);
void intent_runtime_clear_launch_host_v2(void);

#ifdef __cplusplus
}
#endif
