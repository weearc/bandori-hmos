#ifndef PERF_DIAGNOSTIC_LOG_H
#define PERF_DIAGNOSTIC_LOG_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
FILE *perf_diag_fopen(const char *,const char *);
/* Call once before guest/render startup, never lazily on a hot callback. */
void perf_diag_start(const char *directory);
void perf_diag_flush(void);
void perf_diag_pause(int);
void perf_diag_net_vlog(const char *,va_list);
void perf_diag_response_summary(const unsigned char *,size_t);
void perf_diag_connectivity(int,int,const char *);
/* Callers pass only bounded structural diagnostics, never bodies/credentials. */
void perf_diag_connectivity_detail(const char *,const char *);
int perf_diag_fileno(FILE *);
int perf_diag_fclose(FILE *);
ssize_t perf_diag_write(int,const void *,size_t);
int perf_diag_dprintf(int,const char *,...);
int perf_diag_vdprintf(int,const char *,va_list);
int perf_diag_fsync(int);
#ifndef PERF_DIAG_IMPLEMENTATION
#define fclose(stream) perf_diag_fclose(stream)
#define fopen(path,mode) perf_diag_fopen(path,mode)
#define fileno(stream) perf_diag_fileno(stream)
#define write(fd,buffer,count) perf_diag_write(fd,buffer,count)
#define dprintf(...) perf_diag_dprintf(__VA_ARGS__)
#define vdprintf(fd,format,args) perf_diag_vdprintf(fd,format,args)
#define fsync(fd) perf_diag_fsync(fd)
#endif
#endif
