/* OHOS/POSIX host adapter; no guest CPU exists in this diagnostic runner. */
#pragma once
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
int sdk_trace_fprintf(FILE *, const char *, ...);
int sdk_trace_fputc(int,FILE *);
void sdk_trace_unresolved_field(const char *,const char *,const char *);
void sdk_trace_missing_api(const char *,const char *,const char *);
#define LUNA_SEND_NOSIGNAL MSG_NOSIGNAL
#define luna_socket_send send
#define luna_socket_pair socketpair
#define luna_fd_close close
static inline bool luna_guest_paused(void) { return false; }
static inline unsigned arm_lock_unlock_all(void) { return 0; }
static inline void arm_lock_relock(unsigned n) { (void)n; }
static inline void arm_lock_yield(void) {}
static inline void arm_exec_pause_poll(void) {}
static inline int arm_exec_boot_present(void) { return 0; }
static inline size_t luna_os_stack_remaining(void) { return SIZE_MAX; }
static inline uint64_t luna_os_thread_token(void) { return 1; }
static inline uint64_t luna_os_boottime_ns(void) {
 struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
 return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
#define luna_os_uptime_ns luna_os_boottime_ns
static inline void luna_boot_dex_loaded(const char *p, unsigned c, unsigned m, uint64_t b) {(void)p;(void)c;(void)m;(void)b;}
static inline void luna_boot_dex_total(unsigned f, uint64_t b) {(void)f;(void)b;}
bool dvm_platform_class_exists(const char *desc);
