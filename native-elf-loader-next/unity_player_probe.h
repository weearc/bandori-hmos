#ifndef UNITY_PLAYER_PROBE_H
#define UNITY_PLAYER_PROBE_H
struct dvm;
/* Diagnostic foreground observation. Runs without GIL; only return zero on
 * an actual host window visibility/focus read. Not a synthetic lifecycle. */
void sdk_set_unity_activity_state_host(int (*query)(void *,int *,int *),void *user);
/* Caller has configured VM/platform, original main JNI, and nativeLibraryDir;
 * keep VM alive until the entire probe process is terminated. */
int unity_player_probe_run(struct dvm *vm,const char *library_directory,const char *report_directory);
#endif
