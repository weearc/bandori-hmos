#pragma once
#include "dvm/dvm_internal.h"
#include <pthread.h>
void thread_runtime_init(struct dvm *);
bool thread_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool thread_runtime_looper(struct dvm *,dvm_ref);
dvm_ref thread_runtime_current(struct dvm *);
bool thread_runtime_is_main(struct dvm *);
void thread_runtime_shutdown(struct dvm *,unsigned);
void thread_runtime_join_host(struct dvm *,pthread_t);
bool thread_runtime_selftest(struct dvm *);
void sdk_trace_async_failure(struct dvm *,const char *);

bool thread_runtime_executor_bind(struct dvm *,dvm_ref,dvm_ref);
void thread_runtime_executor_finished(struct dvm *,dvm_ref);
