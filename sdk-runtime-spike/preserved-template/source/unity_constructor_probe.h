#ifndef UNITY_CONSTRUCTOR_PROBE_H
#define UNITY_CONSTRUCTOR_PROBE_H
/* Run once in a fresh process, without a diagnostic preflight. The supplied
 * report directory must already exist and be private to the application. */
int unity_constructor_probe_run(const char *library_directory,
                                const char *report_directory);
#endif
