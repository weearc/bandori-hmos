#ifndef UNITY_JNI_PROBE_H
#define UNITY_JNI_PROBE_H
struct dvm;
/* Diagnostic only. Caller must first complete the constructor probe on the
 * same thread, in the same fresh process, with a live initialized DVM. */
int unity_jni_onload_probe_run(struct dvm *vm, const char *library_directory,
                               const char *report_directory);
#endif
