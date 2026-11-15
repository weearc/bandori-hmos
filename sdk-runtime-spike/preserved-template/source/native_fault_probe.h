#ifndef NATIVE_FAULT_PROBE_H
#define NATIVE_FAULT_PROBE_H
#include <stdbool.h>
/* One process-lifetime installation; path is a private writable file. */
bool native_fault_probe_begin(const char *path, const char *candidate);
void native_fault_probe_end(void);
#endif
