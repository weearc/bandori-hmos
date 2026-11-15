#pragma once
#include "bridge.h"
struct dvm;
/* The DVM and its loaded dex mappings must outlive the bridge. */
#ifdef __cplusplus
extern "C" {
#endif
jb_provider jb_dvm_provider(struct dvm *vm);
#ifdef __cplusplus
}
#endif
