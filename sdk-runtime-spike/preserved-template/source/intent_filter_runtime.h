#pragma once
#include "dvm/dvm_internal.h"
/* Real android.content.IntentFilter object state for the DVM.
 *
 * This module owns IntentFilter instances created by guest code and preserves
 * the actions/categories/data schemes/authorities/types and priority the guest
 * added.  battery_runtime owns a private IntentFilter payload too; this module
 * deliberately yields the BATTERY_CHANGED action and non-owned filter objects
 * to it, so the existing battery bridge keeps working.
 *
 * match(...) and <init>(IntentFilter) are intentionally not implemented: the
 * packaged DEX has no call site for either, and guessing their semantics would
 * fabricate behavior.  They fall through to the strict runtime's MISSING_API
 * fail-closed path. */
bool intent_filter_runtime_call(struct dvm *vm, const char *cls,
 const char *method, const char *sig, dvm_ref self,
 const union dvm_value *args, int nargs, union dvm_value *out);
bool intent_filter_runtime_selftest(struct dvm *vm);
/* Release process-level receiver bookkeeping before this VM's arena ends. */
void intent_filter_runtime_vm_end(struct dvm *vm);
