#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* Manual diagnostics only: the host chooses when to call. No constructor starts this probe. */
__attribute__((visibility("default")))
int spike_check_system_dependency(int index, char *result, unsigned long size) {
    static const char *const names[] = {"libc.so", "libdl.so", "libm.so", "liblog.so"};
    if (!result || !size || index < 0 || index >= 4) return -1;
    dlerror(); void *handle = dlopen(names[index], RTLD_NOW | RTLD_LOCAL); const char *error = dlerror();
    if (!handle) { snprintf(result,size,"%s: unavailable: %s",names[index],error?error:"unknown"); return 1; }
    snprintf(result,size,"%s: dlopen succeeded; symbol versions and ABI not verified",names[index]);
    dlclose(handle); return 0;
}
/* Explicit opt-in only. Dlopen may execute initializers and process exit may run finalizers.
   Deliberately never calls JNI_OnLoad and retains handle until process exit. */
__attribute__((visibility("default")))
int spike_open_original_main(char *result, unsigned long size) {
    static void *handle;
    if (!result || !size) return -1;
    if (!handle) { dlerror(); handle=dlopen("libspike_original_main.so", RTLD_NOW | RTLD_LOCAL); const char *error=dlerror();
        if (!handle) { snprintf(result,size,"original main dlopen failed: %s",error?error:"unknown"); return 1; } }
    dlerror(); void *symbol=dlsym(handle,"JNI_OnLoad"); const char *error=dlerror();
    if (error || !symbol) { snprintf(result,size,"JNI_OnLoad lookup failed: %s",error?error:"null"); return 2; }
    snprintf(result,size,"main loaded; JNI_OnLoad resolved but not called; runtime compatibility unproven"); return 0;
}
