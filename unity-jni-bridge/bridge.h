#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct jb_vm jb_vm;
typedef struct { const char *name, *signature; void *fn; } jb_native;
typedef struct { void *context; void *(*find_class)(void *,const char *); int (*is_native_method)(void *,void *,const char *,const char *); } jb_provider;
jb_vm *jb_create(const jb_provider *);
int jb_destroy(jb_vm *);
void *jb_java_vm(jb_vm *);
void *jb_lookup_native(jb_vm *,void *,const char *,const char *);
const char *jb_pending_exception(jb_vm *);
void jb_clear_exception(jb_vm *);
#ifdef __cplusplus
}
#endif
