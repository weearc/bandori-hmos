#pragma once
#include "dvm/dvm_internal.h"

void small_boxes_vm_start(struct dvm *vm);
void small_boxes_vm_end(struct dvm *vm);
bool small_boxes_call(struct dvm *vm, const char *cls, const char *method,
                      const char *sig, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out);
bool small_boxes_selftest(struct dvm *vm);
