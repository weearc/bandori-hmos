#pragma once
#include "dvm/dvm_internal.h"
/* Declares the platform superclass of exception types thrown by the runtime
 * (and caught by guest code) whose class is not defined by the APK. Without
 * this a synthesized class extends Object and `catch (IOException e)` never
 * matches what the runtime throws. */
void exception_hierarchy_vm_start(struct dvm *);
bool exception_hierarchy_selftest(struct dvm *);
bool exception_hierarchy_class_exists(const char *);
