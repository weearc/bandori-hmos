#pragma once
#include "dvm/dvm_internal.h"
bool guest_profile_load(const char *directory);
bool guest_profile_static(struct dvm *,const char *,const char *,const char *,union dvm_value *);
bool guest_profile_call(struct dvm *,const char *,const char *,const char *,union dvm_value *);
bool guest_profile_selftest(struct dvm *,unsigned *checks);
