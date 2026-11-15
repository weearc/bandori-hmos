#ifndef IL2CPP_PROBE_H
#define IL2CPP_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Map libil2cpp.so only. Constructors and all IL2CPP/JNI entry points stay skipped. */
int il2cpp_library_preflight(const char *directory);

#ifdef __cplusplus
}
#endif

#endif
