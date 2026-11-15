#pragma once
#include "dvm/dvm_internal.h"
void io_runtime_vm_start(struct dvm *);
bool io_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool io_runtime_selftest(struct dvm *);
bool io_runtime_byte_array_input_selftest(struct dvm *);
bool io_runtime_input_stream_reader_selftest(struct dvm *);

bool io_runtime_method_supported(const char *,const char *,const char *);
