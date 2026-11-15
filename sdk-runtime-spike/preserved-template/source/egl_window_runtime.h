#ifndef EGL_WINDOW_RUNTIME_H
#define EGL_WINDOW_RUNTIME_H
#ifdef __cplusplus
extern "C" {
#endif
void *egl_window_runtime_symbol(const char *name);
unsigned long long egl_window_runtime_successful_submissions(void);
#ifdef __cplusplus
}
#endif
#endif
