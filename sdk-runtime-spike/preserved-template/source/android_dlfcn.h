#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*android_dlfcn_observer)(const char *event,const char *path,uintptr_t address,int value,void *userdata);
typedef int (*android_dlfcn_body)(void *userdata);
int elf_android_dlfcn_observe(android_dlfcn_observer observer,void *userdata,android_dlfcn_body body,void *bodydata);
/* Adapter handles are generation-checked opaque tokens, never accepted by libc.
 * Supports LOCAL LAZY/NOW (resolved eagerly). NEXT/GLOBAL/NOLOAD are explicit
 * unsupported paths. Custom mappings stay pinned after logical close. Slots
 * recycle with new generations; up to 512 handles may be live at once. */
void *elf_android_dlopen(const char *path,int flags);
void *elf_android_dlsym(void *handle,const char *name);
int elf_android_dlclose(void *handle);
char *elf_android_dlerror(void);
int elf_android_handle_is_custom(void *handle);
#ifdef __cplusplus
}
#endif
