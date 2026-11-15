/*
 * Android Build identity backed by a host snapshot.
 *
 * The values are copied from ArkTS @ohos.deviceInfo (brand/manufacturer/
 * model/device/product/osFullName) and, when supplied, the real host ABI list
 * (deviceInfo.abiList as CSV).  No value is invented here: if the host has not
 * supplied an identity field, its read fails closed. Android build properties
 * unavailable on Harmony use Build.UNKNOWN, as Android getString() does for
 * absent properties; no emulator verdict is supplied by this module.
 *
 * Android field ownership is preserved:
 *   android/os/Build          -> BRAND, MANUFACTURER, MODEL, DEVICE, PRODUCT,
 *                                SUPPORTED_ABIS
 *   android/os/Build$VERSION  -> RELEASE (the classic VERSION.RELEASE ref)
 */
#include "device_runtime.h"
#include "dvm/dvm_internal.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define ABI_MAX 16
#define ABI_NAME_MAX 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char host_brand[128];
static char host_manufacturer[128];
static char host_model[128];
static char host_device[128];
static char host_product[128];
static char host_os_full[256];
static char host_abis[512];
static char host_hardware[256];

static void copy_value(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    if (!src) src = "";
    snprintf(dst, cap, "%s", src);
}

/* Harmony deviceInfo.hardwareModel is the host hardware identity. No
 * emulator decision or fallback identity is supplied by the runtime. */
void device_runtime_set_hardware(const char *hardware) {
    pthread_mutex_lock(&lock);
    copy_value(host_hardware, sizeof(host_hardware), hardware);
    pthread_mutex_unlock(&lock);
}

void device_runtime_set_snapshot(const char *brand, const char *manufacturer,
                                 const char *model, const char *device,
                                 const char *product, const char *os_full) {
    pthread_mutex_lock(&lock);
    copy_value(host_brand, sizeof(host_brand), brand);
    copy_value(host_manufacturer, sizeof(host_manufacturer), manufacturer);
    copy_value(host_model, sizeof(host_model), model);
    copy_value(host_device, sizeof(host_device), device);
    copy_value(host_product, sizeof(host_product), product);
    copy_value(host_os_full, sizeof(host_os_full), os_full);
    pthread_mutex_unlock(&lock);
}

/*
 * Host ABI list, comma-separated, in the order reported by the platform
 * (deviceInfo.abiList).  NULL or "" clears the list, so Build.SUPPORTED_ABIS
 * stays missing.  The caller's buffer is copied; it need not outlive the call.
 * Thread-safe: the copy is taken under the same lock used by readers.
 */
void device_runtime_set_abis(const char *csv) {
    pthread_mutex_lock(&lock);
    if (csv && strlen(csv) >= sizeof(host_abis)) {
        /* Never publish a truncated list: a partial ABI set is a false fact. */
        host_abis[0] = 0;
    } else {
        copy_value(host_abis, sizeof(host_abis), csv);
    }
    pthread_mutex_unlock(&lock);
}

static bool emit(struct dvm *v, const char *value, union dvm_value *out) {
    if (!value || !*value) return false;
    out->l = dvm_new_string(v, value);
    if (!out->l) {
        dvm__throw(v, "java/lang/OutOfMemoryError", "Build static string");
        return true;
    }
    return true;
}

/* One ABI token: 1..63 ASCII letters/digits and '.', '_', '-'.  Real values
 * such as "arm64-v8a" and "x86_64" satisfy this; whitespace, empty elements
 * and path characters do not. */
static bool abi_token(const char *s, size_t n) {
    if (!n || n >= ABI_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* Build a real java/lang/String[] from the host ABI CSV.  Any malformed or
 * empty element rejects the whole list rather than returning a subset. */
static bool emit_abis(struct dvm *v, union dvm_value *out) {
    char csv[sizeof(host_abis)];
    char names[ABI_MAX][ABI_NAME_MAX];
    int count = 0;

    pthread_mutex_lock(&lock);
    memcpy(csv, host_abis, sizeof(csv));
    pthread_mutex_unlock(&lock);
    if (!csv[0]) return false;

    const char *part = csv;
    for (;;) {
        const char *comma = strchr(part, ',');
        size_t len = comma ? (size_t)(comma - part) : strlen(part);
        if (count >= ABI_MAX || !abi_token(part, len)) return false;
        memcpy(names[count], part, len);
        names[count][len] = 0;
        count++;
        if (!comma) break;
        part = comma + 1;
    }
    if (!count) return false;

    dvm_ref array = dvm_new_array(v, 'L', "Ljava/lang/String;", (uint32_t)count);
    if (!array) {
        dvm__throw(v, "java/lang/OutOfMemoryError", "Build.SUPPORTED_ABIS");
        return true;
    }
    dvm_ref *elements = dvm_array_data(v, array);
    if (!elements) {
        dvm__throw(v, "java/lang/OutOfMemoryError", "Build.SUPPORTED_ABIS");
        return true;
    }
    for (int i = 0; i < count; i++) {
        dvm_ref item = dvm_new_string(v, names[i]);
        if (!item) {
            dvm__throw(v, "java/lang/OutOfMemoryError", "Build.SUPPORTED_ABIS");
            return true;
        }
        elements[i] = item;
    }
    out->l = array;
    return true;
}

static bool emit_snapshot_string(struct dvm *v, const char *src, size_t cap,
                                 union dvm_value *out) {
    char value[256] = {0};
    if (cap > sizeof(value)) cap = sizeof(value);
    pthread_mutex_lock(&lock);
    copy_value(value, cap, src);
    pthread_mutex_unlock(&lock);
    return emit(v, value, out);
}

bool device_runtime_static(struct dvm *v, const char *c, const char *f,
                           const char *t, union dvm_value *out) {
    if (!c || !f || !t) return false;

    /* The DEX ref for Build.VERSION.RELEASE names android/os/Build$VERSION as
     * the owner; android/os/Build does not define a field called
     * "VERSION.RELEASE". */
    if (!strcmp(c, "android/os/Build$VERSION")) {
        if (strcmp(f, "RELEASE") || strcmp(t, "Ljava/lang/String;")) return false;
        return emit_snapshot_string(v, host_os_full, sizeof(host_os_full), out);
    }

    if (strcmp(c, "android/os/Build")) return false;
    if (!strcmp(f, "SUPPORTED_ABIS")) {
        if (strcmp(t, "[Ljava/lang/String;")) return false;
        return emit_abis(v, out);
    }
    if (strcmp(t, "Ljava/lang/String;")) return false;
    /* AOSP Build.getString(property) uses UNKNOWN for an absent property.
     * Harmony exposes no Android board/bootloader/build-host property; SERIAL
     * is also unknown under modern Android's restricted identifier contract.
     * Never substitute a made-up board, bootloader, serial, or build host. */
    if (!strcmp(f,"UNKNOWN") || !strcmp(f,"BOARD") ||
        !strcmp(f,"BOOTLOADER") || !strcmp(f,"SERIAL") || !strcmp(f,"HOST"))
        return emit(v,"unknown",out);
    if (!strcmp(f,"FINGERPRINT")) {
        /* Same fallback field order as AOSP Build.deriveFingerprint(). Known
         * snapshot values remain host facts; absent build id/incremental/
         * type/tags remain explicitly unknown. */
        char fingerprint[1024];
        pthread_mutex_lock(&lock);
        int length=snprintf(fingerprint,sizeof fingerprint,"%s/%s/%s:%s/unknown/unknown:unknown/unknown",
            host_brand[0]?host_brand:"unknown",host_product[0]?host_product:"unknown",
            host_device[0]?host_device:"unknown",host_os_full[0]?host_os_full:"unknown");
        pthread_mutex_unlock(&lock);
        if(length<0||(size_t)length>=sizeof fingerprint)return false;
        return emit(v,fingerprint,out);
    }
    if (!strcmp(f, "HARDWARE"))
        return emit_snapshot_string(v, host_hardware, sizeof(host_hardware), out);
    if (!strcmp(f, "BRAND"))
        return emit_snapshot_string(v, host_brand, sizeof(host_brand), out);
    if (!strcmp(f, "MANUFACTURER"))
        return emit_snapshot_string(v, host_manufacturer, sizeof(host_manufacturer), out);
    if (!strcmp(f, "MODEL"))
        return emit_snapshot_string(v, host_model, sizeof(host_model), out);
    if (!strcmp(f, "DEVICE"))
        return emit_snapshot_string(v, host_device, sizeof(host_device), out);
    if (!strcmp(f, "PRODUCT"))
        return emit_snapshot_string(v, host_product, sizeof(host_product), out);
    return false;
}

/* selftest: inject a synthetic snapshot, verify the real read paths, then
 * restore the caller's snapshot/ABI list.  The strings used here are test
 * fixtures only and are never exposed as device facts after the restore. */
bool device_runtime_selftest(struct dvm *v) {
    char brand[sizeof(host_brand)], manufacturer[sizeof(host_manufacturer)];
    char model[sizeof(host_model)], device[sizeof(host_device)];
    char product[sizeof(host_product)], os_full[sizeof(host_os_full)];
    char abis[sizeof(host_abis)];
    char hardware[sizeof(host_hardware)];
    union dvm_value out = {0};
    bool ok = true;

    pthread_mutex_lock(&lock);
    memcpy(brand, host_brand, sizeof(brand));
    memcpy(manufacturer, host_manufacturer, sizeof(manufacturer));
    memcpy(model, host_model, sizeof(model));
    memcpy(device, host_device, sizeof(device));
    memcpy(product, host_product, sizeof(product));
    memcpy(os_full, host_os_full, sizeof(os_full));
    memcpy(abis, host_abis, sizeof(abis));
    memcpy(hardware, host_hardware, sizeof(hardware));
    pthread_mutex_unlock(&lock);

    device_runtime_set_snapshot("selftest-brand", "selftest-manufacturer",
                                "selftest-model", "selftest-device",
                                "selftest-product", "selftest-os");
    device_runtime_set_abis("selftest-abi64,selftest-abi32");
    device_runtime_set_hardware("selftest-hardware");
    if (!device_runtime_static(v,"android/os/Build","HARDWARE",
                               "Ljava/lang/String;",&out) || !out.l ||
        strcmp(dvm_string_utf8(v,out.l),"selftest-hardware")) ok=false;

    if (!device_runtime_static(v, "android/os/Build$VERSION", "RELEASE",
                               "Ljava/lang/String;", &out) ||
        !out.l || !dvm_string_utf8(v, out.l) ||
        strcmp(dvm_string_utf8(v, out.l), "selftest-os"))
        ok = false;
    /* The previous wrong owner must not resolve any more. */
    if (ok && device_runtime_static(v, "android/os/Build", "VERSION.RELEASE",
                                    "Ljava/lang/String;", &out))
        ok = false;
    out.l = 0;
    if (ok) {
        ok = device_runtime_static(v, "android/os/Build", "SUPPORTED_ABIS",
                                   "[Ljava/lang/String;", &out) &&
             out.l && dvm_array_length(v, out.l) == 2;
        if (ok) {
            dvm_ref *items = dvm_array_data(v, out.l);
            ok = items && items[0] && items[1] &&
                 dvm_string_utf8(v, items[0]) && dvm_string_utf8(v, items[1]) &&
                 !strcmp(dvm_string_utf8(v, items[0]), "selftest-abi64") &&
                 !strcmp(dvm_string_utf8(v, items[1]), "selftest-abi32");
        }
    }
    /* Wrong declared type is not this field. */
    if (ok && device_runtime_static(v, "android/os/Build", "SUPPORTED_ABIS",
                                    "Ljava/lang/String;", &out))
        ok = false;
    const char *unknown_fields[]={"UNKNOWN","BOARD","BOOTLOADER","SERIAL","HOST"};
    for(unsigned i=0;ok&&i<sizeof unknown_fields/sizeof unknown_fields[0];i++)
        ok=device_runtime_static(v,"android/os/Build",unknown_fields[i],"Ljava/lang/String;",&out)&&
           out.l&&!strcmp(dvm_string_utf8(v,out.l),"unknown");
    if(ok)ok=device_runtime_static(v,"android/os/Build","FINGERPRINT","Ljava/lang/String;",&out)&&
        out.l&&!strcmp(dvm_string_utf8(v,out.l),"selftest-brand/selftest-product/selftest-device:selftest-os/unknown/unknown:unknown/unknown");
    /* Undeclared fields still fail closed. */
    if (ok && device_runtime_static(v, "android/os/Build", "UNDECLARED_FIXTURE_FIELD",
                                    "Ljava/lang/String;", &out))
        ok = false;
    /* A malformed or empty host list never yields a partial array. */
    device_runtime_set_abis("selftest-abi64,,selftest-abi32");
    if (ok && device_runtime_static(v, "android/os/Build", "SUPPORTED_ABIS",
                                    "[Ljava/lang/String;", &out))
        ok = false;
    device_runtime_set_abis("selftest-abi64,");
    if (ok && device_runtime_static(v, "android/os/Build", "SUPPORTED_ABIS",
                                    "[Ljava/lang/String;", &out))
        ok = false;
    device_runtime_set_abis("");
    if (ok && device_runtime_static(v, "android/os/Build", "SUPPORTED_ABIS",
                                    "[Ljava/lang/String;", &out))
        ok = false;
    device_runtime_set_abis(NULL);
    if (ok && device_runtime_static(v, "android/os/Build", "SUPPORTED_ABIS",
                                    "[Ljava/lang/String;", &out))
        ok = false;

    device_runtime_set_snapshot(brand, manufacturer, model, device, product, os_full);
    device_runtime_set_hardware(hardware);
    device_runtime_set_abis(abis);

    /* Restore must be observable: the real RELEASE value (or its absence)
     * is back, not the fixture. */
    out.l = 0;
    bool release = device_runtime_static(v, "android/os/Build$VERSION", "RELEASE",
                                         "Ljava/lang/String;", &out);
    if (os_full[0]) {
        if (ok && (!release || !out.l || !dvm_string_utf8(v, out.l) ||
                   strcmp(dvm_string_utf8(v, out.l), os_full)))
            ok = false;
    } else if (ok && release) {
        ok = false;
    }
    if (dvm_exception(v)) ok = false;
    return ok;
}
