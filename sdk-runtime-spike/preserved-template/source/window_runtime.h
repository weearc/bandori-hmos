#ifndef WINDOW_RUNTIME_H
#define WINDOW_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINDOW_RUNTIME_HOST_ABI_VERSION 2u

/* Host bridge contract. Callbacks and user storage must remain valid until
 * window_runtime_set_host(NULL) succeeds. Callbacks must not call back into
 * this module. acquire_current returns one owned OH window reference on success.
 */
typedef struct window_runtime_host {
    uint32_t abi_version;
    uint32_t struct_size;
    void *user;
    int (*acquire_current)(void *user, void **native, uint64_t *generation);
    int (*query)(void *user, void *native, uint64_t generation,
                 int32_t *width, int32_t *height);
    int (*release)(void *user, void *native);
    /* Android format value; zero parameters restore base values, not the
     * last configured values. Optional: absent callback returns -ENOTSUP. */
    int (*configure)(void *user, void *native, uint64_t generation,
                     int32_t width, int32_t height, int32_t android_format);
} window_runtime_host;

/* Install/copy the host bridge. Replacing or clearing it fails with -EBUSY
 * while a token is live, being acquired, or being released. NULL clears it.
 */
int window_runtime_set_host(const window_runtime_host *host);

/* Acquires the currently alive host window and returns a process-local opaque
 * token. NULL means there is no usable current window or acquisition failed.
 */
void *window_runtime_acquire_current(void);

/* Android NDK shim entry points. Dimensions are real host buffer pixels in
 * the host's reported order; this module never swaps them to imply landscape.
 * get_width/get_height return a positive dimension or a negative errno value.
 */
int32_t window_runtime_get_width(void *handle);
int32_t window_runtime_get_height(void *handle);
void window_runtime_acquire(void *handle);
/* Take a checked reference before publishing a binding/JNI result. Validates
 * the actual host generation, not just the lifetime of the registry token.
 * On failure no reference is transferred. */
bool window_runtime_acquire_alive(void *handle, uint64_t *host_generation);
/* Pins a checked live token and returns its borrowed native OH window pointer.
 * The pointer remains allocated until matching window_runtime_release(token).
 * This does not guarantee that the host window is still displayed/usable. */
bool window_runtime_acquire_native_lease(void *token, void **native,
                                        uint64_t *generation);
void window_runtime_release(void *handle);
int32_t window_runtime_set_buffers_geometry(void *handle, int32_t width,
                                            int32_t height, int32_t format);

/* Diagnostic helper: returns logical reference count, or a negative errno. */
int32_t window_runtime_ref_count(void *handle);

#ifdef __cplusplus
}
#endif

#endif
