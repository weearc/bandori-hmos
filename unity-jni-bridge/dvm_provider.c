#include "dvm_provider.h"
#include "dvm/dvm_internal.h"
#include <string.h>

static void *find_class(void *context, const char *name) {
    struct dvm *vm = context;
    struct dvm_class *c = dvm_find_class(vm, name);
    return c && c->dex && !c->external && !c->synthesized ? c : NULL;
}

static int is_native(void *context, void *identity, const char *name, const char *sig) {
    struct dvm_class *c = identity;
    struct dvm_method *m = dvm_find_method(context, c, name, sig);
    return m && m->cls == c && m->dex && (m->access & DEX_ACC_NATIVE) &&
           !strcmp(m->name, name) && !strcmp(m->sig, sig);
}

jb_provider jb_dvm_provider(struct dvm *vm) {
    jb_provider p = {vm, find_class, is_native};
    return p;
}
