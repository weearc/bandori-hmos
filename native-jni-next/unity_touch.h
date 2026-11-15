#pragma once
#include "dvm/dvm_internal.h"
#include "unity_touch_abi.h"
#include <stdio.h>
bool unity_touch_begin(struct dvm *,dvm_ref);
bool unity_touch_pump(struct dvm *);
void unity_touch_end(struct dvm *,FILE *);
int sdk_unity_touch_submit(const unity_touch_packet *);
