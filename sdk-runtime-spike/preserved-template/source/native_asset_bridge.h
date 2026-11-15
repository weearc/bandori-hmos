#ifndef NATIVE_ASSET_BRIDGE_H
#define NATIVE_ASSET_BRIDGE_H
#include <stdbool.h>
struct dvm;
bool native_asset_bridge_configure(struct dvm *vm, const char *root);
void *native_asset_bridge_symbol(const char *name);
#endif
