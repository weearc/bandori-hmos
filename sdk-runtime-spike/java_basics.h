#pragma once
#include "dvm/dvm_internal.h"
bool java_basics_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool java_basics_static(struct dvm *,const char *,const char *,const char *,union dvm_value *);
bool java_basics_selftest(void);
bool java_basics_equal(struct dvm *,dvm_ref,dvm_ref);
/* Only the object produced for the supported TimeUnit.SECONDS static is valid. */
bool java_basics_timeunit_nanos(struct dvm *,dvm_ref,uint64_t *);
