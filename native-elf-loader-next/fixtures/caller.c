#include <dlfcn.h>

extern int *__errno(void);

#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif

enum {
    CALLER_OK = 0,
    CALLER_BAD_ARGUMENT = 1,
    CALLER_OPEN_FAILED = 2,
    CALLER_SYMBOL_FAILED = 3,
    CALLER_WRONG_VALUE = 4,
    CALLER_CONSTRUCTOR_COUNT = 5,
    CALLER_MISSING_ERROR = 6,
    CALLER_ERROR_NOT_CONSUMED = 7,
    CALLER_CLOSE_FAILED = 8,
    CALLER_CLOSED_HANDLE_ACCEPTED = 9
};

static void touch_errno_preserving(void) {
    int *slot = __errno();
    if (slot == 0) {
        return;
    }
    int saved = *slot;
    *slot = 0x4e43;
    *slot = saved;
}

__attribute__((visibility("default"))) int nested_caller_run(const char *leaf_path) {
    touch_errno_preserving();
    if (leaf_path == 0) {
        return CALLER_BAD_ARGUMENT;
    }

    /* Clear any error left by an earlier caller before starting this check. */
    (void)dlerror();
    void *first = dlopen(leaf_path, RTLD_NOW | RTLD_LOCAL);
    if (first == 0) {
        return CALLER_OPEN_FAILED;
    }
    void *second = dlopen(leaf_path, RTLD_LAZY | RTLD_LOCAL);
    if (second == 0) {
        (void)dlclose(first);
        return CALLER_OPEN_FAILED;
    }

    int (*get_count)(void) = (int (*)(void))dlsym(first, "leaf_get_constructor_count");
    int (*fixed_value)(void) = (int (*)(void))dlsym(first, "leaf_fixed_value");
    if (get_count == 0 || fixed_value == 0) {
        (void)dlclose(second);
        (void)dlclose(first);
        return CALLER_SYMBOL_FAILED;
    }
    if (fixed_value() != 0x4e57) {
        (void)dlclose(second);
        (void)dlclose(first);
        return CALLER_WRONG_VALUE;
    }
    if (get_count() != 1) {
        (void)dlclose(second);
        (void)dlclose(first);
        return CALLER_CONSTRUCTOR_COUNT;
    }
    int (*ctype_check)(void) = (int (*)(void))dlsym(first, "leaf_ctype_check");
    if (!ctype_check || ctype_check() != 0) {
        (void)dlclose(second);
        (void)dlclose(first);
        return 10;
    }
    int (*media_check)(void) = (int (*)(void))dlsym(first, "leaf_media_keys_check");
    if (!media_check || media_check() != 15) {
        (void)dlclose(second);
        (void)dlclose(first);
        return 11;
    }

    (void)dlerror();
    (void)dlsym(first, "nested_fixture_symbol_that_does_not_exist");
    const char *missing_error = dlerror();
    if (missing_error == 0 || missing_error[0] == '\0') {
        (void)dlclose(second);
        (void)dlclose(first);
        return CALLER_MISSING_ERROR;
    }
    if (dlerror() != 0) {
        (void)dlclose(second);
        (void)dlclose(first);
        return CALLER_ERROR_NOT_CONSUMED;
    }

    if (dlclose(second) != 0 || dlclose(first) != 0) {
        return CALLER_CLOSE_FAILED;
    }
    (void)dlerror();
    (void)dlsym(first, "leaf_get_constructor_count");
    const char *closed_error = dlerror();
    if (closed_error == 0 || closed_error[0] == '\0') {
        return CALLER_CLOSED_HANDLE_ACCEPTED;
    }
    return CALLER_OK;
}
