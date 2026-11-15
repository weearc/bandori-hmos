#ifndef SURFACE_RUNTIME_NEXT_H
#define SURFACE_RUNTIME_NEXT_H

/*
 * Guest android.view.Surface identity and explicit window binding.
 *
 * This module owns:
 *   - the guest Surface class payload (a real, checkable identity, not a
 *     class-name match against synthesized platform placeholders),
 *   - a per-VM-generation binding registry that ties an exact Surface object
 *     to one window_runtime token and host generation,
 *   - the only trusted creation path for a *bound* Surface: the VM start /
 *     host-owned factory.  A plain `new Surface()` never becomes valid.
 *
 * The registry is process-global because bindings must survive independent of
 * the DVM arena, but every record is keyed by the VM instance serial: a reused
 * malloc address is a different generation and never matches.
 *
 * window_runtime tokens are opaque 64-bit values; this module only ever calls
 * window_runtime_acquire/release/ref_count on them, never dereferences them.
 */

#include "dvm/dvm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SURFACE_RUNTIME_MAX_VMS 4u
#define SURFACE_RUNTIME_MAX_BINDINGS 32u

/* Guest method dispatch for android/view/Surface.  Returns false when the
 * class/method is not ours so the strict runtime keeps reporting MISSING. */
bool surface_runtime_call(struct dvm *vm, const char *cls,
                          const char *method, const char *sig,
                          dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out);

/* Register the class and this VM generation.  Safe to call before any window
 * exists; does not create a Surface object by itself. */
void surface_runtime_vm_start(struct dvm *vm);

/* Disconnect every binding owned by this VM generation and release the
 * corresponding window references exactly once.  Must run before the arena and
 * object payloads are freed (dvm_destroy), and before window_runtime host
 * teardown.  Idempotent. */
void surface_runtime_vm_end(struct dvm *vm);

bool surface_runtime_selftest(struct dvm *vm);

/* True only for objects carrying this module's Surface payload. */
bool surface_runtime_is_surface(struct dvm *vm, dvm_ref ref);

/* VM-owned trusted Surface: created lazily by this factory (never by the plain
 * guest constructor) and bound lazily to the current host window on first use.
 * Returns 0 with a pending DVM exception on allocation failure. */
dvm_ref surface_runtime_host_surface(struct dvm *vm);

/* Explicit trusted bind.  On success the binding owns one window_runtime
 * reference.  Returns false when the object is not our Surface, already bound,
 * the VM generation is unknown, or the registry is full. */
bool surface_runtime_bind(struct dvm *vm, dvm_ref ref, void *window,
                          uint64_t window_generation);

/* Trusted unbound Surface; remains invalid until explicit bind. */
dvm_ref surface_runtime_new_surface_unbound(struct dvm *vm);

/* Trusted factory used by ANativeWindow_toSurface: create a new guest Surface
 * bound to exactly this window token.  Returns 0 on failure. */
dvm_ref surface_runtime_new_surface_bound(struct dvm *vm, void *window,
                                          uint64_t window_generation);

/* Look up the alive binding for exactly this VM generation and object.  The
 * returned window token stays owned by the binding; callers that need to
 * outlive the binding must acquire their own reference after validating it
 * with window_runtime_acquire_alive(). This validates the host window rather
 * than merely the lifetime of a registry token. */
bool surface_runtime_lookup_window(struct dvm *vm, dvm_ref ref,
                                   void **window, uint64_t *window_generation);

/* True when this exact object has a binding to the given window token. */
bool surface_runtime_find_by_window(struct dvm *vm, void *window, dvm_ref *out_ref,
                                    uint64_t *window_generation);

/* Release the binding and its window reference.  Returns true only when a
 * binding was actually released; a second call is a harmless no-op. */
bool surface_runtime_unbind(struct dvm *vm, dvm_ref ref);

/* Diagnostics: alive binding count and total bind attempts (selftest use). */
uint32_t surface_runtime_binding_count(struct dvm *vm);

#ifdef __cplusplus
}
#endif

#endif

