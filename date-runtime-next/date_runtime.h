#pragma once

#include <time.h>
#include "dvm/dvm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* java/util/Date + java/text/SimpleDateFormat external dispatch.
 * Return false when the class/method pair is not owned by this module. */
bool date_runtime_call(struct dvm *v, const char *c, const char *m,
                       const char *sig, dvm_ref self,
                       const union dvm_value *a, int nargs,
                       union dvm_value *out);

/* Minimal, deterministic-enough checks for the runtime selftest harness.
 * The fixed-millisecond formatting oracle is derived from the host civil
 * time fields in the test, not from a hard-coded date in production code. */
bool date_runtime_selftest(struct dvm *v);

/* Internal helpers shared by date_runtime.c and simple_date_format.c. */
bool date_runtime_get_millis(struct dvm *v, dvm_ref ref, int64_t *millis);
bool date_runtime_is_date(struct dvm *v, dvm_ref ref);

/* Convert Unix-epoch milliseconds to the host's current local civil time.
 * On success fills *tm, *millis (0..999) and the real UTC offset in seconds.
 * Returns false when the host clock/time zone facts are unavailable. */
bool date_runtime_host_fields(int64_t millis, struct tm *tm,
                              int64_t *offset_seconds, int *millis_part);

#ifdef __cplusplus
}
#endif
