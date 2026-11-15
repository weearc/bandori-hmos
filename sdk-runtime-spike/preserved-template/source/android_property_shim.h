#ifndef ANDROID_PROPERTY_SHIM_H
#define ANDROID_PROPERTY_SHIM_H

/*
 * Bionic __system_property_find/__system_property_read shims.
 *
 * libunity calls __system_property_find(name) once and checks the result with
 * cbz before __system_property_read(pi, NULL, value); the property name is a
 * runtime std::string (candidates in the binary include ro.debuggable,
 * ro.build.fingerprint and ro.product.cpu.abi*, but the caller decides).
 *
 * The only real property source this layer has is the process environment,
 * exactly like the existing __system_property_get shim.  A property that is
 * not present returns NULL -- the documented bionic semantics for a missing
 * property -- and Unity skips it.  No build fingerprint, debuggable flag, or
 * other value is ever fabricated.
 *
 * prop_info is opaque to callers: find() returns stable pointers to published
 * entries in a bounded static table. read() accepts only exact published
 * entry addresses and synchronizes with publication. The deprecated Bionic
 * read ABI has no buffer-size parameters: callers must provide a 32-byte name
 * buffer and a 92-byte value buffer (either may be NULL). Name output follows
 * Bionic's legacy 31-character-plus-NUL limit; long names remain distinct for
 * find(), but their read() name output is truncated as in Bionic. The shim
 * rejects names/values beyond its storage capacity rather than storing a
 * silently truncated alias.
 */

#include <stddef.h>

#define ANDROID_PROP_NAME_MAX 32
#define ANDROID_PROP_VALUE_MAX 92

#ifdef __cplusplus
extern "C" {
#endif

void *android_system_property_find(const char *name);
int android_system_property_read(const void *prop_info, char *name, char *value);

#ifdef __cplusplus
}
#endif

#endif
