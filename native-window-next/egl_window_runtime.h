#ifndef EGL_WINDOW_RUNTIME_H
#define EGL_WINDOW_RUNTIME_H
#ifdef __cplusplus
extern "C" {
#endif
void *egl_window_runtime_symbol(const char *name);
unsigned long long egl_window_runtime_successful_submissions(void);
unsigned long long egl_window_runtime_last_submission_ns(void);
int egl_window_runtime_touch_dimensions(int *width,int *height);
#ifdef __cplusplus
}
#endif
#endif
