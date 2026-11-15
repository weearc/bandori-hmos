#include "dvm/dvm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern int jb_self_test(struct dvm *);
static _Thread_local char report[512];

/* Caller serializes this with all other users of the shared DVM arena. */
const char *jni_batch_run(const char *dexdir) {
    int count = -1, result = -1;
    if (!dexdir || !*dexdir) {
        snprintf(report, sizeof(report), "BUILD JNI_BATCH_V1\nRESULT invalid dex directory\n");
        return report;
    }
    struct dvm_hooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    struct dvm *vm = dvm_create(&hooks);
    if (vm) {
        count = dvm_add_apk_dir(vm, dexdir);
        if (count == 3) result = jb_self_test(vm);
        dvm_destroy(vm);
    }
    snprintf(report, sizeof(report), "BUILD JNI_BATCH_V1\nDEX_COUNT %d\nSELF_TEST_CODE %d\nRESULT %s\n", count, result,
             count == 3 && result == 0 ? "PASS" : "FAIL");
    return report;
}
