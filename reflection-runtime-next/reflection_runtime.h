#pragma once
#include "dvm/dvm_internal.h"
void reflection_runtime_vm_start(struct dvm *);
void reflection_runtime_vm_end(struct dvm *);
bool reflection_runtime_call(struct dvm *, const char *, const char *, const char *, dvm_ref, const union dvm_value *, int, union dvm_value *);
bool reflection_runtime_selftest(struct dvm *);
/* JNI FromReflectedMethod/FromReflectedField：从 java.lang.reflect.Method 或
 * Field 对象取底层 dvm_method/dvm_field。对象不是对应反射类型时返回 false，
 * 调用方按 JNI 契约报错，不得编造句柄。 */
bool reflection_runtime_method_handle(struct dvm *, dvm_ref, struct dvm_method **);
bool reflection_runtime_field_handle(struct dvm *, dvm_ref, struct dvm_field **, struct dvm_class **, bool *);
