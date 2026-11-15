#pragma once
#include "dvm/dvm_internal.h"
/* Original com.bilibili.star.bili.MainApplication lifecycle for the diagnostic
 * runner. Each function runs exactly one original DEX method. */
dvm_ref sdk_application_create(struct dvm *);
bool sdk_application_construct(struct dvm *, dvm_ref);
bool sdk_application_attach(struct dvm *, dvm_ref);
bool sdk_application_on_create(struct dvm *, dvm_ref);
/* Read-only observation of Router.hasInit after the original onCreate. */
void sdk_application_router_state(struct dvm *, bool *known, bool *has_init);
