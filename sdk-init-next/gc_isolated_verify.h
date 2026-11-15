#pragma once
#include <stddef.h>

/* Verification builds only. No constructor: carrier must explicitly opt in.
 * One background mutator owns an isolated strong-only graph and pauses its
 * OWN work around a complete gcs_evacuate. No real VM safepoint is measured.
 * No weak refs/finalizers, canonical VM, Unity hooks or real VM collection. */
struct gciv_config {
 const char *directory; /* Existing writable directory, UTF-8 path. */
 unsigned duration_seconds; /* Explicit range 1..86400; no default long run. */
};
struct gciv_report {
 int state; /* 0 not started, 1 running, 2 completed, 3 stopped, -1 failed. */
 int error;
 unsigned elapsed_seconds;
 size_t collections;
 unsigned long long max_isolated_pause_ns;
 char pause_csv[1024],memory_csv[1024],report_json[1024];
};
/* Returns 0 once thread starts, -1 invalid/IO/thread failure, -2 already used.
 * Process singleton: no restart. Files are separate from GC3/4/5 reports. */
int gc_isolated_verify_start(const struct gciv_config *);
void gc_isolated_verify_get_report(struct gciv_report *);
void gc_isolated_verify_request_stop(void);
