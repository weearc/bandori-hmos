#pragma once
#include "dvm/dvm_internal.h"
#include "intent_launch_host.h"
/* Minimal android/content/Intent payload plus the Context/Activity launch
 * methods Router uses. Launch calls stay fail-closed and throw
 * UnsupportedOperationException until the host installs an explicit bridge. */

/* Host launch bridge. 'launch' receives the guest Intent reference used by the
 * launch call; overridePendingTransition carries no Intent, so it passes 0.
 * Returning 0 means the host accepted the launch request, not that a page was
 * actually started. A non-zero return makes the launch method throw
 * RuntimeException. intent_runtime_set_launch_host(NULL, user) clears too. */
void intent_runtime_set_launch_host(int (*launch)(void *user, dvm_ref intent),
 void *user);
void intent_runtime_clear_launch_host(void);

bool intent_runtime_call(struct dvm *vm, const char *cls, const char *method,
 const char *sig, dvm_ref self, const union dvm_value *args, int nargs,
 union dvm_value *out);
bool intent_runtime_selftest(struct dvm *vm);
