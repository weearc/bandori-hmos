#pragma once
#include "dvm/dvm_internal.h"
#include "battery_host.h"

void battery_runtime_set_host(const battery_host_callbacks *callbacks, void *host);
void battery_runtime_get_host(battery_host_callbacks *callbacks, void **host);
bool battery_runtime_call(struct dvm *vm, const char *cls, const char *method,
 const char *sig, dvm_ref self, const union dvm_value *args, int nargs,
 union dvm_value *out);
bool battery_runtime_context_broadcast(struct dvm *vm, const char *method,
 const char *sig, dvm_ref context, const union dvm_value *args, int nargs,
 union dvm_value *out);
void battery_runtime_pump(struct dvm *vm); /* dvm__ui_tick; VM main/GIL only */
void battery_runtime_finish(struct dvm *vm); /* before dvm_destroy */
bool battery_runtime_selftest(void); /* explicitly injected C fixture */
