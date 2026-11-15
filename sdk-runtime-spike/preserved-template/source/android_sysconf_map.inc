/* Android 14 bionic sysconf IDs -> host names. Never pass numeric IDs through.
 * Source: https://raw.githubusercontent.com/aosp-mirror/platform_bionic/android-14.0.0_r1/libc/include/bits/sysconf.h */
#ifdef _SC_ARG_MAX
case 0x0000: host_name = _SC_ARG_MAX; break;
#endif
#ifdef _SC_BC_BASE_MAX
case 0x0001: host_name = _SC_BC_BASE_MAX; break;
#endif
#ifdef _SC_BC_DIM_MAX
case 0x0002: host_name = _SC_BC_DIM_MAX; break;
#endif
#ifdef _SC_BC_SCALE_MAX
case 0x0003: host_name = _SC_BC_SCALE_MAX; break;
#endif
#ifdef _SC_BC_STRING_MAX
case 0x0004: host_name = _SC_BC_STRING_MAX; break;
#endif
#ifdef _SC_CHILD_MAX
case 0x0005: host_name = _SC_CHILD_MAX; break;
#endif
#ifdef _SC_CLK_TCK
case 0x0006: host_name = _SC_CLK_TCK; break;
#endif
#ifdef _SC_COLL_WEIGHTS_MAX
case 0x0007: host_name = _SC_COLL_WEIGHTS_MAX; break;
#endif
#ifdef _SC_EXPR_NEST_MAX
case 0x0008: host_name = _SC_EXPR_NEST_MAX; break;
#endif
#ifdef _SC_LINE_MAX
case 0x0009: host_name = _SC_LINE_MAX; break;
#endif
#ifdef _SC_NGROUPS_MAX
case 0x000a: host_name = _SC_NGROUPS_MAX; break;
#endif
#ifdef _SC_OPEN_MAX
case 0x000b: host_name = _SC_OPEN_MAX; break;
#endif
#ifdef _SC_PASS_MAX
case 0x000c: host_name = _SC_PASS_MAX; break;
#endif
#ifdef _SC_2_C_BIND
case 0x000d: host_name = _SC_2_C_BIND; break;
#endif
#ifdef _SC_2_C_DEV
case 0x000e: host_name = _SC_2_C_DEV; break;
#endif
#ifdef _SC_2_C_VERSION
case 0x000f: host_name = _SC_2_C_VERSION; break;
#endif
#ifdef _SC_2_CHAR_TERM
case 0x0010: host_name = _SC_2_CHAR_TERM; break;
#endif
#ifdef _SC_2_FORT_DEV
case 0x0011: host_name = _SC_2_FORT_DEV; break;
#endif
#ifdef _SC_2_FORT_RUN
case 0x0012: host_name = _SC_2_FORT_RUN; break;
#endif
#ifdef _SC_2_LOCALEDEF
case 0x0013: host_name = _SC_2_LOCALEDEF; break;
#endif
#ifdef _SC_2_SW_DEV
case 0x0014: host_name = _SC_2_SW_DEV; break;
#endif
#ifdef _SC_2_UPE
case 0x0015: host_name = _SC_2_UPE; break;
#endif
#ifdef _SC_2_VERSION
case 0x0016: host_name = _SC_2_VERSION; break;
#endif
#ifdef _SC_JOB_CONTROL
case 0x0017: host_name = _SC_JOB_CONTROL; break;
#endif
#ifdef _SC_SAVED_IDS
case 0x0018: host_name = _SC_SAVED_IDS; break;
#endif
#ifdef _SC_VERSION
case 0x0019: host_name = _SC_VERSION; break;
#endif
#ifdef _SC_RE_DUP_MAX
case 0x001a: host_name = _SC_RE_DUP_MAX; break;
#endif
#ifdef _SC_STREAM_MAX
case 0x001b: host_name = _SC_STREAM_MAX; break;
#endif
#ifdef _SC_TZNAME_MAX
case 0x001c: host_name = _SC_TZNAME_MAX; break;
#endif
#ifdef _SC_XOPEN_CRYPT
case 0x001d: host_name = _SC_XOPEN_CRYPT; break;
#endif
#ifdef _SC_XOPEN_ENH_I18N
case 0x001e: host_name = _SC_XOPEN_ENH_I18N; break;
#endif
#ifdef _SC_XOPEN_SHM
case 0x001f: host_name = _SC_XOPEN_SHM; break;
#endif
#ifdef _SC_XOPEN_VERSION
case 0x0020: host_name = _SC_XOPEN_VERSION; break;
#endif
#ifdef _SC_XOPEN_XCU_VERSION
case 0x0021: host_name = _SC_XOPEN_XCU_VERSION; break;
#endif
#ifdef _SC_XOPEN_REALTIME
case 0x0022: host_name = _SC_XOPEN_REALTIME; break;
#endif
#ifdef _SC_XOPEN_REALTIME_THREADS
case 0x0023: host_name = _SC_XOPEN_REALTIME_THREADS; break;
#endif
#ifdef _SC_XOPEN_LEGACY
case 0x0024: host_name = _SC_XOPEN_LEGACY; break;
#endif
#ifdef _SC_ATEXIT_MAX
case 0x0025: host_name = _SC_ATEXIT_MAX; break;
#endif
#ifdef _SC_IOV_MAX
case 0x0026: host_name = _SC_IOV_MAX; break;
#endif
#ifdef _SC_PAGESIZE
case 0x0027: host_name = _SC_PAGESIZE; break;
#endif
#ifdef _SC_PAGE_SIZE
case 0x0028: host_name = _SC_PAGE_SIZE; break;
#endif
#ifdef _SC_XOPEN_UNIX
case 0x0029: host_name = _SC_XOPEN_UNIX; break;
#endif
#ifdef _SC_XBS5_ILP32_OFF32
case 0x002a: host_name = _SC_XBS5_ILP32_OFF32; break;
#endif
#ifdef _SC_XBS5_ILP32_OFFBIG
case 0x002b: host_name = _SC_XBS5_ILP32_OFFBIG; break;
#endif
#ifdef _SC_XBS5_LP64_OFF64
case 0x002c: host_name = _SC_XBS5_LP64_OFF64; break;
#endif
#ifdef _SC_XBS5_LPBIG_OFFBIG
case 0x002d: host_name = _SC_XBS5_LPBIG_OFFBIG; break;
#endif
#ifdef _SC_AIO_LISTIO_MAX
case 0x002e: host_name = _SC_AIO_LISTIO_MAX; break;
#endif
#ifdef _SC_AIO_MAX
case 0x002f: host_name = _SC_AIO_MAX; break;
#endif
#ifdef _SC_AIO_PRIO_DELTA_MAX
case 0x0030: host_name = _SC_AIO_PRIO_DELTA_MAX; break;
#endif
#ifdef _SC_DELAYTIMER_MAX
case 0x0031: host_name = _SC_DELAYTIMER_MAX; break;
#endif
#ifdef _SC_MQ_OPEN_MAX
case 0x0032: host_name = _SC_MQ_OPEN_MAX; break;
#endif
#ifdef _SC_MQ_PRIO_MAX
case 0x0033: host_name = _SC_MQ_PRIO_MAX; break;
#endif
#ifdef _SC_RTSIG_MAX
case 0x0034: host_name = _SC_RTSIG_MAX; break;
#endif
#ifdef _SC_SEM_NSEMS_MAX
case 0x0035: host_name = _SC_SEM_NSEMS_MAX; break;
#endif
#ifdef _SC_SEM_VALUE_MAX
case 0x0036: host_name = _SC_SEM_VALUE_MAX; break;
#endif
#ifdef _SC_SIGQUEUE_MAX
case 0x0037: host_name = _SC_SIGQUEUE_MAX; break;
#endif
#ifdef _SC_TIMER_MAX
case 0x0038: host_name = _SC_TIMER_MAX; break;
#endif
#ifdef _SC_ASYNCHRONOUS_IO
case 0x0039: host_name = _SC_ASYNCHRONOUS_IO; break;
#endif
#ifdef _SC_FSYNC
case 0x003a: host_name = _SC_FSYNC; break;
#endif
#ifdef _SC_MAPPED_FILES
case 0x003b: host_name = _SC_MAPPED_FILES; break;
#endif
#ifdef _SC_MEMLOCK
case 0x003c: host_name = _SC_MEMLOCK; break;
#endif
#ifdef _SC_MEMLOCK_RANGE
case 0x003d: host_name = _SC_MEMLOCK_RANGE; break;
#endif
#ifdef _SC_MEMORY_PROTECTION
case 0x003e: host_name = _SC_MEMORY_PROTECTION; break;
#endif
#ifdef _SC_MESSAGE_PASSING
case 0x003f: host_name = _SC_MESSAGE_PASSING; break;
#endif
#ifdef _SC_PRIORITIZED_IO
case 0x0040: host_name = _SC_PRIORITIZED_IO; break;
#endif
#ifdef _SC_PRIORITY_SCHEDULING
case 0x0041: host_name = _SC_PRIORITY_SCHEDULING; break;
#endif
#ifdef _SC_REALTIME_SIGNALS
case 0x0042: host_name = _SC_REALTIME_SIGNALS; break;
#endif
#ifdef _SC_SEMAPHORES
case 0x0043: host_name = _SC_SEMAPHORES; break;
#endif
#ifdef _SC_SHARED_MEMORY_OBJECTS
case 0x0044: host_name = _SC_SHARED_MEMORY_OBJECTS; break;
#endif
#ifdef _SC_SYNCHRONIZED_IO
case 0x0045: host_name = _SC_SYNCHRONIZED_IO; break;
#endif
#ifdef _SC_TIMERS
case 0x0046: host_name = _SC_TIMERS; break;
#endif
#ifdef _SC_GETGR_R_SIZE_MAX
case 0x0047: host_name = _SC_GETGR_R_SIZE_MAX; break;
#endif
#ifdef _SC_GETPW_R_SIZE_MAX
case 0x0048: host_name = _SC_GETPW_R_SIZE_MAX; break;
#endif
#ifdef _SC_LOGIN_NAME_MAX
case 0x0049: host_name = _SC_LOGIN_NAME_MAX; break;
#endif
#ifdef _SC_THREAD_DESTRUCTOR_ITERATIONS
case 0x004a: host_name = _SC_THREAD_DESTRUCTOR_ITERATIONS; break;
#endif
#ifdef _SC_THREAD_KEYS_MAX
case 0x004b: host_name = _SC_THREAD_KEYS_MAX; break;
#endif
#ifdef _SC_THREAD_STACK_MIN
case 0x004c: host_name = _SC_THREAD_STACK_MIN; break;
#endif
#ifdef _SC_THREAD_THREADS_MAX
case 0x004d: host_name = _SC_THREAD_THREADS_MAX; break;
#endif
#ifdef _SC_TTY_NAME_MAX
case 0x004e: host_name = _SC_TTY_NAME_MAX; break;
#endif
#ifdef _SC_THREADS
case 0x004f: host_name = _SC_THREADS; break;
#endif
#ifdef _SC_THREAD_ATTR_STACKADDR
case 0x0050: host_name = _SC_THREAD_ATTR_STACKADDR; break;
#endif
#ifdef _SC_THREAD_ATTR_STACKSIZE
case 0x0051: host_name = _SC_THREAD_ATTR_STACKSIZE; break;
#endif
#ifdef _SC_THREAD_PRIORITY_SCHEDULING
case 0x0052: host_name = _SC_THREAD_PRIORITY_SCHEDULING; break;
#endif
#ifdef _SC_THREAD_PRIO_INHERIT
case 0x0053: host_name = _SC_THREAD_PRIO_INHERIT; break;
#endif
#ifdef _SC_THREAD_PRIO_PROTECT
case 0x0054: host_name = _SC_THREAD_PRIO_PROTECT; break;
#endif
#ifdef _SC_THREAD_SAFE_FUNCTIONS
case 0x0055: host_name = _SC_THREAD_SAFE_FUNCTIONS; break;
#endif
#ifdef _SC_NPROCESSORS_CONF
case 0x0060: host_name = _SC_NPROCESSORS_CONF; break;
#endif
#ifdef _SC_NPROCESSORS_ONLN
case 0x0061: host_name = _SC_NPROCESSORS_ONLN; break;
#endif
#ifdef _SC_PHYS_PAGES
case 0x0062: host_name = _SC_PHYS_PAGES; break;
#endif
#ifdef _SC_AVPHYS_PAGES
case 0x0063: host_name = _SC_AVPHYS_PAGES; break;
#endif
#ifdef _SC_MONOTONIC_CLOCK
case 0x0064: host_name = _SC_MONOTONIC_CLOCK; break;
#endif
#ifdef _SC_2_PBS
case 0x0065: host_name = _SC_2_PBS; break;
#endif
#ifdef _SC_2_PBS_ACCOUNTING
case 0x0066: host_name = _SC_2_PBS_ACCOUNTING; break;
#endif
#ifdef _SC_2_PBS_CHECKPOINT
case 0x0067: host_name = _SC_2_PBS_CHECKPOINT; break;
#endif
#ifdef _SC_2_PBS_LOCATE
case 0x0068: host_name = _SC_2_PBS_LOCATE; break;
#endif
#ifdef _SC_2_PBS_MESSAGE
case 0x0069: host_name = _SC_2_PBS_MESSAGE; break;
#endif
#ifdef _SC_2_PBS_TRACK
case 0x006a: host_name = _SC_2_PBS_TRACK; break;
#endif
#ifdef _SC_ADVISORY_INFO
case 0x006b: host_name = _SC_ADVISORY_INFO; break;
#endif
#ifdef _SC_BARRIERS
case 0x006c: host_name = _SC_BARRIERS; break;
#endif
#ifdef _SC_CLOCK_SELECTION
case 0x006d: host_name = _SC_CLOCK_SELECTION; break;
#endif
#ifdef _SC_CPUTIME
case 0x006e: host_name = _SC_CPUTIME; break;
#endif
#ifdef _SC_HOST_NAME_MAX
case 0x006f: host_name = _SC_HOST_NAME_MAX; break;
#endif
#ifdef _SC_IPV6
case 0x0070: host_name = _SC_IPV6; break;
#endif
#ifdef _SC_RAW_SOCKETS
case 0x0071: host_name = _SC_RAW_SOCKETS; break;
#endif
#ifdef _SC_READER_WRITER_LOCKS
case 0x0072: host_name = _SC_READER_WRITER_LOCKS; break;
#endif
#ifdef _SC_REGEXP
case 0x0073: host_name = _SC_REGEXP; break;
#endif
#ifdef _SC_SHELL
case 0x0074: host_name = _SC_SHELL; break;
#endif
#ifdef _SC_SPAWN
case 0x0075: host_name = _SC_SPAWN; break;
#endif
#ifdef _SC_SPIN_LOCKS
case 0x0076: host_name = _SC_SPIN_LOCKS; break;
#endif
#ifdef _SC_SPORADIC_SERVER
case 0x0077: host_name = _SC_SPORADIC_SERVER; break;
#endif
#ifdef _SC_SS_REPL_MAX
case 0x0078: host_name = _SC_SS_REPL_MAX; break;
#endif
#ifdef _SC_SYMLOOP_MAX
case 0x0079: host_name = _SC_SYMLOOP_MAX; break;
#endif
#ifdef _SC_THREAD_CPUTIME
case 0x007a: host_name = _SC_THREAD_CPUTIME; break;
#endif
#ifdef _SC_THREAD_PROCESS_SHARED
case 0x007b: host_name = _SC_THREAD_PROCESS_SHARED; break;
#endif
#ifdef _SC_THREAD_ROBUST_PRIO_INHERIT
case 0x007c: host_name = _SC_THREAD_ROBUST_PRIO_INHERIT; break;
#endif
#ifdef _SC_THREAD_ROBUST_PRIO_PROTECT
case 0x007d: host_name = _SC_THREAD_ROBUST_PRIO_PROTECT; break;
#endif
#ifdef _SC_THREAD_SPORADIC_SERVER
case 0x007e: host_name = _SC_THREAD_SPORADIC_SERVER; break;
#endif
#ifdef _SC_TIMEOUTS
case 0x007f: host_name = _SC_TIMEOUTS; break;
#endif
#ifdef _SC_TRACE
case 0x0080: host_name = _SC_TRACE; break;
#endif
#ifdef _SC_TRACE_EVENT_FILTER
case 0x0081: host_name = _SC_TRACE_EVENT_FILTER; break;
#endif
#ifdef _SC_TRACE_EVENT_NAME_MAX
case 0x0082: host_name = _SC_TRACE_EVENT_NAME_MAX; break;
#endif
#ifdef _SC_TRACE_INHERIT
case 0x0083: host_name = _SC_TRACE_INHERIT; break;
#endif
#ifdef _SC_TRACE_LOG
case 0x0084: host_name = _SC_TRACE_LOG; break;
#endif
#ifdef _SC_TRACE_NAME_MAX
case 0x0085: host_name = _SC_TRACE_NAME_MAX; break;
#endif
#ifdef _SC_TRACE_SYS_MAX
case 0x0086: host_name = _SC_TRACE_SYS_MAX; break;
#endif
#ifdef _SC_TRACE_USER_EVENT_MAX
case 0x0087: host_name = _SC_TRACE_USER_EVENT_MAX; break;
#endif
#ifdef _SC_TYPED_MEMORY_OBJECTS
case 0x0088: host_name = _SC_TYPED_MEMORY_OBJECTS; break;
#endif
#ifdef _SC_V7_ILP32_OFF32
case 0x0089: host_name = _SC_V7_ILP32_OFF32; break;
#endif
#ifdef _SC_V7_ILP32_OFFBIG
case 0x008a: host_name = _SC_V7_ILP32_OFFBIG; break;
#endif
#ifdef _SC_V7_LP64_OFF64
case 0x008b: host_name = _SC_V7_LP64_OFF64; break;
#endif
#ifdef _SC_V7_LPBIG_OFFBIG
case 0x008c: host_name = _SC_V7_LPBIG_OFFBIG; break;
#endif
#ifdef _SC_XOPEN_STREAMS
case 0x008d: host_name = _SC_XOPEN_STREAMS; break;
#endif
#ifdef _SC_XOPEN_UUCP
case 0x008e: host_name = _SC_XOPEN_UUCP; break;
#endif
#ifdef _SC_LEVEL1_ICACHE_SIZE
case 0x008f: host_name = _SC_LEVEL1_ICACHE_SIZE; break;
#endif
#ifdef _SC_LEVEL1_ICACHE_ASSOC
case 0x0090: host_name = _SC_LEVEL1_ICACHE_ASSOC; break;
#endif
#ifdef _SC_LEVEL1_ICACHE_LINESIZE
case 0x0091: host_name = _SC_LEVEL1_ICACHE_LINESIZE; break;
#endif
#ifdef _SC_LEVEL1_DCACHE_SIZE
case 0x0092: host_name = _SC_LEVEL1_DCACHE_SIZE; break;
#endif
#ifdef _SC_LEVEL1_DCACHE_ASSOC
case 0x0093: host_name = _SC_LEVEL1_DCACHE_ASSOC; break;
#endif
#ifdef _SC_LEVEL1_DCACHE_LINESIZE
case 0x0094: host_name = _SC_LEVEL1_DCACHE_LINESIZE; break;
#endif
#ifdef _SC_LEVEL2_CACHE_SIZE
case 0x0095: host_name = _SC_LEVEL2_CACHE_SIZE; break;
#endif
#ifdef _SC_LEVEL2_CACHE_ASSOC
case 0x0096: host_name = _SC_LEVEL2_CACHE_ASSOC; break;
#endif
#ifdef _SC_LEVEL2_CACHE_LINESIZE
case 0x0097: host_name = _SC_LEVEL2_CACHE_LINESIZE; break;
#endif
#ifdef _SC_LEVEL3_CACHE_SIZE
case 0x0098: host_name = _SC_LEVEL3_CACHE_SIZE; break;
#endif
#ifdef _SC_LEVEL3_CACHE_ASSOC
case 0x0099: host_name = _SC_LEVEL3_CACHE_ASSOC; break;
#endif
#ifdef _SC_LEVEL3_CACHE_LINESIZE
case 0x009a: host_name = _SC_LEVEL3_CACHE_LINESIZE; break;
#endif
#ifdef _SC_LEVEL4_CACHE_SIZE
case 0x009b: host_name = _SC_LEVEL4_CACHE_SIZE; break;
#endif
#ifdef _SC_LEVEL4_CACHE_ASSOC
case 0x009c: host_name = _SC_LEVEL4_CACHE_ASSOC; break;
#endif
#ifdef _SC_LEVEL4_CACHE_LINESIZE
case 0x009d: host_name = _SC_LEVEL4_CACHE_LINESIZE; break;
#endif
