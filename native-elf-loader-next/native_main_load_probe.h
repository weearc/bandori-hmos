#ifndef NATIVE_MAIN_LOAD_PROBE_H
#define NATIVE_MAIN_LOAD_PROBE_H
struct dvm;
int native_main_load_probe_run(struct dvm *vm,const char *library_directory,const char *report_directory);
#endif
