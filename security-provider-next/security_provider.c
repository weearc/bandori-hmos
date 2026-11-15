#include "gc_runtime.h"
#include "security_provider.h"
#include "dvm/dvm_internal.h"
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

/* The TLS facade is implemented in ssl_runtime.c. This provider does not
 * expose Provider.Service until JCA service lookup and SPI creation exist. */
static const char provider_name[] = "DvmTLS";
static const char provider_info[] = "DVM built-in TLS facade";
static struct dvm *registry_vm;
static dvm_ref registry_provider;

static bool security_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 (void)context;if(!dvm_gil_held(v)||registry_vm!=v)return false;if(registry_provider)visit(&registry_provider,user);return true;
}
static bool match(const char *m, const char *s, const char *wm, const char *ws)
{ return m && s && !strcmp(m, wm) && !strcmp(s, ws); }

void security_provider_vm_end(struct dvm *vm)
{
    if (vm && vm == registry_vm) {
        dvm_gc_root_provider_remove(vm,"security/provider",&registry_vm);
        if (registry_provider) dvm_unpin(vm, registry_provider);
        registry_provider = 0;
        registry_vm = NULL;
    }
}

void security_provider_vm_start(struct dvm *vm)
{
    struct dvm_class *provider, *properties;
    if (registry_vm == vm) security_provider_vm_end(vm);
    registry_vm = vm;
    registry_provider = 0;
    if (!vm) return;
    if(!dvm_gc_root_provider_add(vm,"security/provider",DVM_GC_ROOT_STRONG,security_gc_roots,&registry_vm)){dvm__throw(vm,"java/lang/OutOfMemoryError","security GC roots");return;}
    provider = dvm_find_class_or_external(vm, "java/security/Provider");
    properties = dvm_find_class_or_external(vm, "java/util/Properties");
    if (provider && provider->external && properties) provider->super = properties;
    if (!provider) {dvm__throw(vm,"java/lang/OutOfMemoryError","provider class");return;}
    registry_provider = dvm_new_object(vm, provider);
    if (registry_provider) dvm_pin(vm, registry_provider);
    else dvm__throw(vm,"java/lang/OutOfMemoryError","provider registry");
}

bool security_provider_call(struct dvm *vm, const char *class_name,
                            const char *method, const char *sig, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
    if (!vm || vm != registry_vm || !class_name || !out) return false;
    if (!strcmp(class_name, "java/security/Security") && !self) {
        if (match(method, sig, "getProviders", "()[Ljava/security/Provider;") && nargs == 0) {
            dvm_ref array = dvm_new_array(vm, 'L', "Ljava/security/Provider;", registry_provider ? 1 : 0);
            if (!array) return false;
            if (registry_provider) ((dvm_ref *)dvm_array_data(vm, array))[0] = registry_provider;
            out->l = array;
            return true;
        }
        if (match(method, sig, "getProvider", "(Ljava/lang/String;)Ljava/security/Provider;") && nargs == 1) {
            const char *name = args && args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
            out->l = name && !strcmp(name, provider_name) ? registry_provider : 0;
            return true;
        }
        return false;
    }
    if (strcmp(class_name, "java/security/Provider") || self != registry_provider || !self)
        return false;
    if (match(method, sig, "getName", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, provider_name); return out->l != 0;
    }
    if (match(method, sig, "getInfo", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, provider_info); return out->l != 0;
    }
    if (match(method, sig, "getVersion", "()D") && nargs == 0) {
        out->d = 1.0; return true;
    }
    if (match(method, sig, "toString", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, provider_name); return out->l != 0;
    }
    if (match(method, sig, "getService", "(Ljava/lang/String;Ljava/lang/String;)Ljava/security/Provider$Service;") && nargs == 2) {
        out->l = 0; return true;
    }
    return false;
}

bool security_provider_selftest(struct dvm *vm)
{
    union dvm_value a = {0}, b = {0}, c = {0};
    dvm_ref *items;
    if (!vm || !registry_provider || registry_vm != vm) return false;
    if (!security_provider_call(vm, "java/security/Security", "getProviders",
            "()[Ljava/security/Provider;", 0, NULL, 0, &a)) return false;
    if (!security_provider_call(vm, "java/security/Security", "getProviders",
            "()[Ljava/security/Provider;", 0, NULL, 0, &b)) return false;
    if (!a.l || !b.l || a.l == b.l || dvm_array_length(vm, a.l) != 1 ||
        dvm_array_length(vm, b.l) != 1) return false;
    items = dvm_array_data(vm, a.l);
    if (!items || items[0] != registry_provider) return false;
    items = dvm_array_data(vm, b.l);
    if (!items || items[0] != registry_provider) return false;
    if (!security_provider_call(vm, "java/security/Provider", "getName",
            "()Ljava/lang/String;", registry_provider, NULL, 0, &c)) return false;
    if (!c.l || strcmp(dvm_string_utf8(vm, c.l), provider_name) ||
        !strcmp(provider_name, "Conscrypt")) return false;
    if(security_provider_call(vm, "java/security/Provider", "unknown",
            "()V", registry_provider, NULL, 0, &c))return false;
    sdk_trace_fprintf(stderr,"SECURITY_PROVIDER_SELFTEST PASS local_provider=1 copied_arrays=1 stable_identity=1 services_unimplemented=1\n");return !dvm_exception(vm);
}
