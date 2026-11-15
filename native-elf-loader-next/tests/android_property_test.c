#define _GNU_SOURCE 1
#include "../android_property_shim.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sdk_trace_fprintf(FILE *f, const char *fmt, ...)
{
    (void)f; (void)fmt;
    return 0;
}

static void putenv_checked(const char *key, const char *value)
{
#ifdef _WIN32
    size_t n = strlen(key) + strlen(value) + 2;
    char *entry = (char *)malloc(n);
    assert(entry);
    snprintf(entry, n, "%s=%s", key, value);
    assert(_putenv(entry) == 0);
    free(entry);
#else
    assert(setenv(key, value, 1) == 0);
#endif
}

struct slot_layout { char name[92]; char value[92]; int used; };

static void test_basic_and_bounds(void)
{
    char name[ANDROID_PROP_NAME_MAX + 2];
    char value[ANDROID_PROP_VALUE_MAX + 2];
    void *pi;
    const char *key = "ANDROID_PROPERTY_TEST_BASIC";
    putenv_checked(key, "mapped-env-value");
    pi = android_system_property_find(key);
    assert(pi != NULL);
    memset(name, 0xA5, sizeof(name));
    memset(value, 0x5A, sizeof(value));
    assert(android_system_property_read(pi, name + 1, value + 1) == 16);
    assert(strcmp(name + 1, key) == 0);
    assert(strcmp(value + 1, "mapped-env-value") == 0);
    assert((unsigned char)name[0] == 0xA5 && (unsigned char)name[33] == 0xA5);
    assert((unsigned char)value[0] == 0x5A && (unsigned char)value[93] == 0x5A);
    assert(android_system_property_find("ANDROID_PROPERTY_TEST_MISSING") == NULL);

    /* Neither arbitrary/interior nor in-range-but-never-published pointers
       may be treated as prop_info. The table starts at the first returned
       slot and slots have this private implementation layout. */
    assert(android_system_property_read((void *)(uintptr_t)1, NULL, NULL) == 0);
    assert(android_system_property_read((char *)pi + 1, NULL, NULL) == 0);
    assert(android_system_property_read((char *)pi + sizeof(struct slot_layout),
                                        NULL, NULL) == 0);
}

static void test_legacy_capacities_and_long_inputs(void)
{
    char name[ANDROID_PROP_NAME_MAX + 2];
    char value[ANDROID_PROP_VALUE_MAX + 2];
    char long_name[93], long_value[93];
    void *pi;
    memset(long_name, 'n', sizeof(long_name) - 1); long_name[92] = 0;
    memset(long_value, 'v', sizeof(long_value) - 1); long_value[92] = 0;

    putenv_checked("ANDROID_PROPERTY_TEST_LONGNAME_ABCDEFGHIJKLMNOPQRSTUVWXYZ", "ok");
    pi = android_system_property_find("ANDROID_PROPERTY_TEST_LONGNAME_ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    assert(pi != NULL);
    memset(name, 0xC3, sizeof(name));
    memset(value, 0xD4, sizeof(value));
    assert(android_system_property_read(pi, name + 1, value + 1) == 2);
    assert(name[32] == 0 && (unsigned char)name[33] == 0xC3);
    assert(strcmp(value + 1, "ok") == 0);

    putenv_checked(long_name, "x");
    assert(android_system_property_find(long_name) == NULL); /* > storage cap */
    putenv_checked("ANDROID_PROPERTY_TEST_LONGVALUE", long_value);
    assert(android_system_property_find("ANDROID_PROPERTY_TEST_LONGVALUE") == NULL);
}

static void *concurrent_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3000; ++i) {
        void *pi = android_system_property_find("ANDROID_PROPERTY_TEST_CONCURRENT");
        char n[ANDROID_PROP_NAME_MAX], v[ANDROID_PROP_VALUE_MAX];
        assert(pi != NULL);
        assert(android_system_property_read(pi, n, v) == 5);
        assert(strcmp(v, "value") == 0);
        assert(strcmp(n, "ANDROID_PROPERTY_TEST_CONCURREN") == 0);
    }
    return NULL;
}

static void test_concurrent_find_read(void)
{
    pthread_t threads[8];
    putenv_checked("ANDROID_PROPERTY_TEST_CONCURRENT", "value");
    for (size_t i = 0; i < sizeof(threads) / sizeof(threads[0]); ++i)
        assert(pthread_create(&threads[i], NULL, concurrent_worker, NULL) == 0);
    for (size_t i = 0; i < sizeof(threads) / sizeof(threads[0]); ++i)
        assert(pthread_join(threads[i], NULL) == 0);
}

int main(void)
{
    test_basic_and_bounds();
    test_legacy_capacities_and_long_inputs();
    test_concurrent_find_read();
    puts("android_property_test: PASS");
    return 0;
}
