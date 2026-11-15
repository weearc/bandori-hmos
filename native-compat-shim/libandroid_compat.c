/*
 * Minimal Bionic compatibility shim for the original Android libraries.
 *
 * This is NOT a Bionic implementation.  It exists so the dynamic loader can
 * resolve the small set of LIBC-versioned symbols that the original
 * libcustom_rules/libblkv/libcrypto objects reference but OHOS musl does not
 * export.  Each function documents its deviation.  In particular __sF is only
 * a placeholder: if an executed path dereferences it as a Bionic FILE array,
 * the load is declared no-go instead of pretending stdio works.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

__attribute__((visibility("default"))) int *__errno(void) { return __errno_location(); }

/* Bionic's three stdio FILE slots.  Placeholder storage only; see file top. */
__attribute__((visibility("default"))) unsigned char __sF[3][256];

__attribute__((visibility("default"))) int __open_2(const char *path, int flags) {
    if (flags & O_CREAT) { errno = EINVAL; return -1; }
    return open(path, flags);
}

__attribute__((visibility("default"))) ssize_t __read_chk(int fd, void *buf,
                                                         size_t count,
                                                         size_t buflen) {
    if (count > buflen) { errno = EINVAL; return -1; }
    return read(fd, buf, count);
}

__attribute__((visibility("default"))) void *__memmove_chk(void *dst,
                                                           const void *src,
                                                           size_t len,
                                                           size_t dstlen) {
    if (len > dstlen) __builtin_trap();
    return memmove(dst, src, len);
}

__attribute__((visibility("default"))) char *__strcpy_chk(char *dst,
                                                          const char *src,
                                                          size_t dstlen) {
    size_t n = strlen(src) + 1;
    if (n > dstlen) __builtin_trap();
    return memcpy(dst, src, n);
}

__attribute__((visibility("default"))) char *__strcat_chk(char *dst,
                                                          const char *src,
                                                          size_t dstlen) {
    size_t d = strlen(dst);
    size_t n = strlen(src) + 1;
    if (n > dstlen || d >= dstlen || n > dstlen - d) __builtin_trap();
    memcpy(dst + d, src, n);
    return dst;
}

__attribute__((visibility("default"))) size_t __strlen_chk(const char *s,
                                                           size_t maxlen) {
    size_t n = strnlen(s, maxlen);
    if (n == maxlen) __builtin_trap();
    return n;
}

__attribute__((visibility("default"))) int __vsnprintf_chk(char *dst,
                                                           size_t size,
                                                           int flags,
                                                           size_t dstlen,
                                                           const char *fmt,
                                                           va_list ap) {
    (void)flags;
    if (size > dstlen) __builtin_trap();
    return vsnprintf(dst, size, fmt, ap);
}

__attribute__((visibility("default"))) int __vsprintf_chk(char *dst, int flags,
                                                          size_t dstlen,
                                                          const char *fmt,
                                                          va_list ap) {
    (void)flags;
    return vsnprintf(dst, dstlen, fmt, ap);
}

__attribute__((visibility("default"))) long long strtoll_l(const char *nptr,
                                                           char **endptr,
                                                           int base,
                                                           void *locale) {
    (void)locale;
    return strtoll(nptr, endptr, base);
}

__attribute__((visibility("default"))) unsigned long long strtoull_l(
    const char *nptr, char **endptr, int base, void *locale) {
    (void)locale;
    return strtoull(nptr, endptr, base);
}

__attribute__((visibility("default"))) long double strtold_l(
    const char *nptr, char **endptr, void *locale) {
    (void)locale;
    return strtold(nptr, endptr);
}

__attribute__((visibility("default"))) size_t __ctype_get_mb_cur_max(void) {
    return 4; /* UTF-8: musl defines MB_CUR_MAX as a call to this symbol */
}

__attribute__((visibility("default"))) int __system_property_get(
    const char *name, char *value) {
    (void)name;
    if (value) value[0] = '\0';
    /* OHOS does not expose Android's property service.  Return "absent"
     * instead of inventing a property value. */
    return 0;
}

__attribute__((visibility("default"))) void android_set_abort_message(
    const char *message) {
    /* Bionic records this for the crash report; use stderr here.  Do not
     * abort: the caller decides whether the missing semantics are fatal. */
    if (message) fprintf(stderr, "ANDROID_ABORT_MESSAGE %s\n", message);
}
