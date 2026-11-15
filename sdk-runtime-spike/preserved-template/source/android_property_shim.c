/*
 * Bionic system-property shims. See android_property_shim.h.
 * Entries have process lifetime. The mutex protects both publication and
 * reads so a reader cannot observe a partially initialized slot.
 */
#define _GNU_SOURCE 1
#include "android_property_shim.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define PROP_SHIM_SLOTS 16
#define PROP_SHIM_NAME_MAX 91
#define PROP_SHIM_VALUE_MAX 91

struct prop_info_shim {
    char name[PROP_SHIM_NAME_MAX + 1];
    char value[PROP_SHIM_VALUE_MAX + 1];
    int used;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct prop_info_shim g_props[PROP_SHIM_SLOTS];

/* Integer range/alignment checks avoid undefined relational pointer
 * comparisons. Only the exact address of a published slot is a prop_info. */
static struct prop_info_shim *prop_in_table(const void *pi)
{
    uintptr_t p, base, span, offset;
    if (!pi) return NULL;
    p = (uintptr_t)pi;
    base = (uintptr_t)&g_props[0];
    span = sizeof(g_props);
    if (p < base || p - base >= span) return NULL;
    offset = p - base;
    if (offset % sizeof(g_props[0]) != 0) return NULL;
    return &g_props[offset / sizeof(g_props[0])];
}

void *android_system_property_find(const char *name)
{
    const char *env;
    size_t n, v;
    struct prop_info_shim *free_slot = NULL, *result = NULL;
    if (!name || !*name) return NULL;

    /* Properties intentionally come only from the real process environment. */
    env = getenv(name);
    if (!env) {
        sdk_trace_fprintf(stderr,
            "ANDROID_PROP find name=%s found=0 honest_missing=1\n", name);
        return NULL;
    }
    n = strlen(name);
    v = strlen(env);
    /* Reject instead of truncating: distinct inputs must never alias a slot. */
    if (n > PROP_SHIM_NAME_MAX || v > PROP_SHIM_VALUE_MAX) return NULL;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < PROP_SHIM_SLOTS; ++i) {
        struct prop_info_shim *slot = &g_props[i];
        if (slot->used && strcmp(slot->name, name) == 0) {
            result = slot;
            break;
        }
        if (!slot->used && !free_slot) free_slot = slot;
    }
    if (!result && free_slot) {
        memcpy(free_slot->name, name, n + 1);
        memcpy(free_slot->value, env, v + 1);
        /* Publish last while holding the same lock used by readers. */
        free_slot->used = 1;
        result = free_slot;
    }
    pthread_mutex_unlock(&g_lock);
    sdk_trace_fprintf(stderr,
        "ANDROID_PROP find name=%s found=%d value_len=%zu real_source=environment\n",
        name, result != NULL, result ? v : 0);
    return result;
}

int android_system_property_read(const void *prop_info, char *name, char *value)
{
    struct prop_info_shim *slot = prop_in_table(prop_info);
    size_t name_len, value_len;
    if (!slot) return 0;

    pthread_mutex_lock(&g_lock);
    if (!slot->used) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    name_len = strlen(slot->name);
    value_len = strlen(slot->value);
    /* AOSP's deprecated ABI has no size arguments: name is PROP_NAME_MAX
     * (32) and value is PROP_VALUE_MAX (92), independently. */
    if (name) {
        size_t copy = name_len < ANDROID_PROP_NAME_MAX - 1
                    ? name_len : ANDROID_PROP_NAME_MAX - 1;
        memcpy(name, slot->name, copy);
        name[copy] = '\0';
    }
    if (value) memcpy(value, slot->value, value_len + 1);
    pthread_mutex_unlock(&g_lock);
    return (int)value_len;
}
