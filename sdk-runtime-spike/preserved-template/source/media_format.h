#ifndef NATIVE_MEDIA_NEXT_MEDIA_FORMAT_H
#define NATIVE_MEDIA_NEXT_MEDIA_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

struct OH_AVFormat;

#ifdef __cplusplus
extern "C" {
#endif

/* Android NDK-compatible status values used by this compatibility layer. */
typedef int32_t android_media_status_t;
enum {
    ANDROID_MEDIA_OK = 0,
    ANDROID_MEDIA_ERROR_UNKNOWN = -10000,
    ANDROID_MEDIA_ERROR_UNSUPPORTED = -10002,
    ANDROID_MEDIA_ERROR_INVALID_OBJECT = -10003,
    ANDROID_MEDIA_ERROR_INVALID_PARAMETER = -10004,
    ANDROID_MEDIA_ERROR_INVALID_OPERATION = -10005
};

/* Opaque controlled handle; never cast this pointer to OH_AVFormat. */
typedef struct android_media_format android_media_format_t;

/*
 * Takes ownership of owned_format. On success returns a registered opaque
 * handle. Handles are supported only on 64-bit targets. On failure
 * (including capacity exhaustion or token exhaustion), destroys
 * owned_format before returning NULL.
 */
void *media_format_wrap(struct OH_AVFormat *owned_format);

/* Returns an owned audio-only OH_AVFormat copy, or NULL if unsupported. */
struct OH_AVFormat *media_format_clone_for_audio(void *format);

android_media_status_t android_media_format_delete(void *format);
bool android_media_format_get_float(void *format, const char *key, float *out);
bool android_media_format_get_int32(void *format, const char *key, int32_t *out);
bool android_media_format_get_int64(void *format, const char *key, int64_t *out);
/* Returned string is borrowed from OH_AVFormat until format modification/deletion. */
bool android_media_format_get_string(void *format, const char *key, const char **out);
void android_media_format_set_int32(void *format, const char *key, int32_t value);

#ifdef __cplusplus
}
#endif
#endif


