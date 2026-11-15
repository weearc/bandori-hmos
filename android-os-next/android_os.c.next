/* Real android.system.Os subset over host descriptors and host mappings.
 *
 * This round implements:
 *   - android.system.Os.posix_fallocate(FileDescriptor, long, long)
 *   - android.system.Os.mmap(long, long, int, int, FileDescriptor, long)
 *   - android.system.Os.munmap(long, long)
 *   - android.system.Os.msync(long, long, int)
 *   - android.system.OsConstants int constants and S_ISREG/S_ISDIR/S_ISLNK
 *
 * The BLKV mapped-buffer path returns the host mmap address to the guest as a
 * Java long and then dereferences it from native code in the same process, so
 * mmap must return the real host address or fail.  Nothing here invents an
 * address or reports success for an unmapped range.  File descriptors are
 * owned by random_access_file.c; this module only borrows the real host fd and
 * never closes it.
 *
 * Failure semantics follow Android: bad parameters are
 * java.lang.IllegalArgumentException, a missing/invalid/closed descriptor or a
 * failing syscall is android.system.ErrnoException with the host errno.
 * ErrnoException is registered as a direct subclass of java.lang.Exception so
 * BLKV's `catch (Exception)` sees it.  If that parent edge cannot be
 * established, the error is raised as java.io.IOException rather than being
 * swallowed. */
#include "android_os.h"
#include "random_access_file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MS_ASYNC
#define MS_ASYNC 1
#endif
#ifndef MS_INVALIDATE
#define MS_INVALIDATE 2
#endif
#ifndef MS_SYNC
#define MS_SYNC 4
#endif

extern int sdk_trace_fprintf(FILE *, const char *, ...);

/* Java-side android.system.OsConstants values.  These are the bionic/Linux
 * numbers the guest's DEX was compiled against, kept explicit so a host
 * header cannot silently change what the guest sees.  The host translation
 * below maps only the bits this module actually accepts. */
enum {
   OS_PROT_NONE = 0,
   OS_PROT_READ = 1,
   OS_PROT_WRITE = 2,
   OS_PROT_EXEC = 4,
   OS_PROT_MASK = 7,

   OS_MAP_SHARED = 1,
   OS_MAP_PRIVATE = 2,
   OS_MAP_FIXED = 0x10,
   OS_MAP_ANONYMOUS = 0x20,
   OS_MAP_MASK = 0x33,

   OS_MS_ASYNC = 1,
   OS_MS_INVALIDATE = 2,
   OS_MS_SYNC = 4,
   OS_MS_MASK = 7,

   OS_S_IFMT = 0170000,
   OS_S_IFREG = 0100000,
   OS_S_IFDIR = 0040000,
   OS_S_IFLNK = 0120000,
   OS_S_IFCHR = 0020000,
   OS_S_IFBLK = 0060000,
   OS_S_IFIFO = 0010000,
   OS_S_IFSOCK = 0140000,

   OS_S_ISUID = 0004000,
   OS_S_ISGID = 0002000,
   OS_S_ISVTX = 0001000,
   OS_S_IRWXU = 0000700,
   OS_S_IRUSR = 0000400,
   OS_S_IWUSR = 0000200,
   OS_S_IXUSR = 0000100,
   OS_S_IRWXG = 0000070,
   OS_S_IRGRP = 0000040,
   OS_S_IWGRP = 0000020,
   OS_S_IXGRP = 0000010,
   OS_S_IRWXO = 0000007,
   OS_S_IROTH = 0000004,
   OS_S_IWOTH = 0000002,
   OS_S_IXOTH = 0000001
};

struct os_int_constant {
   const char *name;
   int32_t value;
};

/* The constants BLKV and ordinary file/socket code read.  Anything absent
 * fails closed with NoSuchFieldError instead of acquiring a guessed value. */
static const struct os_int_constant os_int_constants[] = {
   /* mmap protection */
   { "PROT_NONE", OS_PROT_NONE },
   { "PROT_READ", OS_PROT_READ },
   { "PROT_WRITE", OS_PROT_WRITE },
   { "PROT_EXEC", OS_PROT_EXEC },
   /* mmap flags */
   { "MAP_SHARED", OS_MAP_SHARED },
   { "MAP_PRIVATE", OS_MAP_PRIVATE },
   { "MAP_FIXED", OS_MAP_FIXED },
   { "MAP_ANONYMOUS", OS_MAP_ANONYMOUS },
   /* msync flags */
   { "MS_ASYNC", OS_MS_ASYNC },
   { "MS_INVALIDATE", OS_MS_INVALIDATE },
   { "MS_SYNC", OS_MS_SYNC },
   /* file type */
   { "S_IFMT", OS_S_IFMT },
   { "S_IFREG", OS_S_IFREG },
   { "S_IFDIR", OS_S_IFDIR },
   { "S_IFLNK", OS_S_IFLNK },
   { "S_IFCHR", OS_S_IFCHR },
   { "S_IFBLK", OS_S_IFBLK },
   { "S_IFIFO", OS_S_IFIFO },
   { "S_IFSOCK", OS_S_IFSOCK },
   /* permission bits */
   { "S_ISUID", OS_S_ISUID },
   { "S_ISGID", OS_S_ISGID },
   { "S_ISVTX", OS_S_ISVTX },
   { "S_IRWXU", OS_S_IRWXU },
   { "S_IRUSR", OS_S_IRUSR },
   { "S_IWUSR", OS_S_IWUSR },
   { "S_IXUSR", OS_S_IXUSR },
   { "S_IRWXG", OS_S_IRWXG },
   { "S_IRGRP", OS_S_IRGRP },
   { "S_IWGRP", OS_S_IWGRP },
   { "S_IXGRP", OS_S_IXGRP },
   { "S_IRWXO", OS_S_IRWXO },
   { "S_IROTH", OS_S_IROTH },
   { "S_IWOTH", OS_S_IWOTH },
   { "S_IXOTH", OS_S_IXOTH },
   /* open flags */
   { "O_RDONLY", 0 },
   { "O_WRONLY", 1 },
   { "O_RDWR", 2 },
   { "O_ACCMODE", 3 },
   { "O_CREAT", 64 },
   { "O_EXCL", 128 },
   { "O_NOCTTY", 256 },
   { "O_TRUNC", 512 },
   { "O_APPEND", 1024 },
   { "O_NONBLOCK", 2048 },
   { "O_DSYNC", 4096 },
   { "O_DIRECT", 16384 },
   { "O_DIRECTORY", 65536 },
   { "O_NOFOLLOW", 131072 },
   { "O_CLOEXEC", 524288 },
   { "O_SYNC", 1052672 },
   { "O_RSYNC", 1052672 },
   /* access(2) */
   { "F_OK", 0 },
   { "X_OK", 1 },
   { "W_OK", 2 },
   { "R_OK", 4 },
   /* lseek(2) */
   { "SEEK_SET", 0 },
   { "SEEK_CUR", 1 },
   { "SEEK_END", 2 },
   /* standard descriptors */
   { "STDIN_FILENO", 0 },
   { "STDOUT_FILENO", 1 },
   { "STDERR_FILENO", 2 },
   /* waitpid(2) */
   { "WNOHANG", 1 },
   { "WUNTRACED", 2 },
   { "WSTOPPED", 2 },
   { "WEXITED", 4 },
   { "WCONTINUED", 8 },
   { "WNOWAIT", 0x1000000 },
   /* signals */
   { "SIGHUP", 1 },
   { "SIGINT", 2 },
   { "SIGQUIT", 3 },
   { "SIGILL", 4 },
   { "SIGTRAP", 5 },
   { "SIGABRT", 6 },
   { "SIGBUS", 7 },
   { "SIGFPE", 8 },
   { "SIGKILL", 9 },
   { "SIGUSR1", 10 },
   { "SIGSEGV", 11 },
   { "SIGUSR2", 12 },
   { "SIGPIPE", 13 },
   { "SIGALRM", 14 },
   { "SIGTERM", 15 },
   { "SIGCHLD", 17 },
   { "SIGCONT", 18 },
   { "SIGSTOP", 19 },
   { "SIGTSTP", 20 },
   { "SIGTTIN", 21 },
   { "SIGTTOU", 22 },
   { "SIGURG", 23 },
   { "SIGXCPU", 24 },
   { "SIGXFSZ", 25 },
   { "SIGVTALRM", 26 },
   { "SIGPROF", 27 },
   { "SIGWINCH", 28 },
   { "SIGIO", 29 },
   { "SIGPWR", 30 },
   { "SIGSYS", 31 },
   /* errno values callers compare against */
   { "EPERM", 1 },
   { "ENOENT", 2 },
   { "ESRCH", 3 },
   { "EINTR", 4 },
   { "EIO", 5 },
   { "ENXIO", 6 },
   { "E2BIG", 7 },
   { "ENOEXEC", 8 },
   { "EBADF", 9 },
   { "ECHILD", 10 },
   { "EAGAIN", 11 },
   { "EWOULDBLOCK", 11 },
   { "ENOMEM", 12 },
   { "EACCES", 13 },
   { "EFAULT", 14 },
   { "EBUSY", 16 },
   { "EEXIST", 17 },
   { "EXDEV", 18 },
   { "ENODEV", 19 },
   { "ENOTDIR", 20 },
   { "EISDIR", 21 },
   { "EINVAL", 22 },
   { "ENFILE", 23 },
   { "EMFILE", 24 },
   { "ENOTTY", 25 },
   { "ETXTBSY", 26 },
   { "EFBIG", 27 },
   { "ENOSPC", 28 },
   { "ESPIPE", 29 },
   { "EROFS", 30 },
   { "EMLINK", 31 },
   { "EPIPE", 32 },
   { "EDOM", 33 },
   { "ERANGE", 34 },
   { "EDEADLK", 35 },
   { "ENAMETOOLONG", 36 },
   { "ENOSYS", 38 },
   { "ENOTEMPTY", 39 },
   { "ELOOP", 40 },
   { "ENOMSG", 42 },
   { "EIDRM", 43 },
   { "EBADMSG", 74 },
   { "EOVERFLOW", 75 },
   { "ENOTSUP", 95 },
   { "EOPNOTSUPP", 95 },
   { "EADDRINUSE", 98 },
   { "EADDRNOTAVAIL", 99 },
   { "ENETDOWN", 100 },
   { "ENETUNREACH", 101 },
   { "ECONNABORTED", 103 },
   { "ECONNRESET", 104 },
   { "ENOBUFS", 105 },
   { "EISCONN", 106 },
   { "ENOTCONN", 107 },
   { "ETIMEDOUT", 110 },
   { "ECONNREFUSED", 111 },
   { "EHOSTUNREACH", 113 },
   { "EALREADY", 114 },
   { "EINPROGRESS", 115 },
   { "ECANCELED", 125 }
};

/* One host mapping this module created.  Only addresses present here may be
 * passed to munmap/msync; the table is what keeps guest-supplied addresses
 * from unmapping unrelated host memory. */
struct os_mapping {
   int64_t addr;
   int64_t length;
   int32_t prot;
   int32_t flags;
};

#define OS_MAPPING_MAX 65536u

static struct dvm *active_vm;
static char guest_root[PATH_MAX];
static bool guest_root_ready;
static bool raf_configured;

static struct os_mapping *os_mappings;
static size_t os_mapping_count;
static size_t os_mapping_capacity;

/* dvm__class_by_desc() synthesizes an external class with Object as its
 * default parent.  Set the Exception edge only when nobody else has chosen a
 * parent, mirroring exception_hierarchy.c.  Returns whether ErrnoException is
 * actually assignable to java.lang.Exception in this VM. */
static bool ensure_errno_exception(struct dvm *vm) {
   struct dvm_class *object;
   struct dvm_class *exception;
   struct dvm_class *errno_class;

   if (!vm) return false;
   object = dvm_find_class_or_external(vm, "java/lang/Object");
   exception = dvm_find_class_or_external(vm, "java/lang/Exception");
   errno_class = dvm_find_class_or_external(vm, "android/system/ErrnoException");
   if (!exception || !errno_class) return false;
   if (errno_class != exception && errno_class->external &&
       (!errno_class->super || errno_class->super == object))
      errno_class->super = exception;
   return dvm__class_assignable(vm, errno_class, exception);
}

static void throw_errno_op(struct dvm *vm, const char *operation, int err) {
   char message[384];
   const char *text = "unknown error";
   if (err <= 0) err = EIO;
   if (strerror(err)) text = strerror(err);
   snprintf(message, sizeof message, "%s failed: %s (errno %d)", operation,
            text, err);
   if (ensure_errno_exception(vm))
      dvm__throw(vm, "android/system/ErrnoException", "%s", message);
   else
      dvm__throw(vm, "java/io/IOException", "%s", message);
}

static void throw_errno(struct dvm *vm, int err) {
   throw_errno_op(vm, "posix_fallocate", err);
}

static void throw_illegal(struct dvm *vm, const char *message) {
   dvm__throw(vm, "java/lang/IllegalArgumentException", "%s", message);
}

/* posix_fallocate returns 0 on success.  POSIX specifies the error number as
 * the return value; some libcs use -1 plus errno.  Accept both, and never turn
 * an unknown failure into success. */
static int fallocate_errno(int rc) {
   if (rc == 0) return 0;
   if (rc > 0) return rc;
   if (rc == -1 && errno) return errno;
   return EIO;
}

static bool os_fallocate_fd(struct dvm *vm, int fd, int64_t offset,
                            int64_t length) {
   int rc;
   int err;
   errno = 0;
   rc = posix_fallocate(fd, (off_t)offset, (off_t)length);
   err = fallocate_errno(rc);
   if (!err) return true;
   throw_errno(vm, err);
   return false;
}

/* ---------------- mapping registry ---------------- */

static struct os_mapping *os_mapping_find_exact(int64_t addr, int64_t length) {
   for (size_t i = 0; i < os_mapping_count; ++i)
      if (os_mappings[i].addr == addr && os_mappings[i].length == length)
         return &os_mappings[i];
   return NULL;
}

/* msync may name any page-aligned range inside a mapping, as the host allows. */
static struct os_mapping *os_mapping_find_range(int64_t addr, int64_t length) {
   if (addr <= 0 || length <= 0) return NULL;
   for (size_t i = 0; i < os_mapping_count; ++i) {
      struct os_mapping *m = &os_mappings[i];
      int64_t delta;
      if (addr < m->addr || length > m->length) continue;
      delta = addr - m->addr;
      if (delta <= m->length - length) return m;
   }
   return NULL;
}

static bool os_mapping_record(int64_t addr, int64_t length, int32_t prot,
                              int32_t flags) {
   if (os_mapping_count == os_mapping_capacity) {
      size_t capacity = os_mapping_capacity ? os_mapping_capacity * 2 : 64;
      struct os_mapping *grown;
      if (capacity > OS_MAPPING_MAX) capacity = OS_MAPPING_MAX;
      if (capacity <= os_mapping_capacity) return false;
      grown = realloc(os_mappings, capacity * sizeof *grown);
      if (!grown) return false;
      os_mappings = grown;
      os_mapping_capacity = capacity;
   }
   os_mappings[os_mapping_count++] = (struct os_mapping){
      .addr = addr, .length = length, .prot = prot, .flags = flags };
   return true;
}

static void os_mapping_forget(struct os_mapping *mapping) {
   size_t index = (size_t)(mapping - os_mappings);
   if (index >= os_mapping_count) return;
   os_mappings[index] = os_mappings[--os_mapping_count];
}

/* Process teardown: a mapping the guest did not close must not keep host
 * memory alive past the VM that owns it. */
static void os_mappings_release(struct dvm *vm) {
   size_t failed = 0;
   (void)vm;
   for (size_t i = 0; i < os_mapping_count; ++i)
      if (munmap((void *)(intptr_t)os_mappings[i].addr,
                 (size_t)os_mappings[i].length))
         ++failed;
   if (os_mapping_count)
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_CLEANUP mappings=%zu munmap_failed=%zu\n",
                        os_mapping_count, failed);
   free(os_mappings);
   os_mappings = NULL;
   os_mapping_count = 0;
   os_mapping_capacity = 0;
}

/* ---------------- OsConstants ---------------- */

/* The static OsConstants predicates AOSP declares.  The file-type tests mask
 * with S_IFMT; the set-id/sticky tests ask whether the bit is present. */
static const char *const os_predicate_names[] = {
   "S_ISREG", "S_ISDIR", "S_ISLNK", "S_ISCHR", "S_ISBLK", "S_ISFIFO",
   "S_ISSOCK", "S_ISUID", "S_ISGID", "S_ISVTX"
};
#define OS_PREDICATE_COUNT (sizeof os_predicate_names / sizeof os_predicate_names[0])

static bool os_predicate_value(const char *method, int32_t mode,
                               int32_t *out) {
   if (!method || !out) return false;
   if (!strcmp(method, "S_ISREG")) {
      *out = (mode & OS_S_IFMT) == OS_S_IFREG;
      return true;
   }
   if (!strcmp(method, "S_ISDIR")) {
      *out = (mode & OS_S_IFMT) == OS_S_IFDIR;
      return true;
   }
   if (!strcmp(method, "S_ISLNK")) {
      *out = (mode & OS_S_IFMT) == OS_S_IFLNK;
      return true;
   }
   if (!strcmp(method, "S_ISCHR")) {
      *out = (mode & OS_S_IFMT) == OS_S_IFCHR;
      return true;
   }
   if (!strcmp(method, "S_ISBLK")) {
      *out = (mode & OS_S_IFMT) == OS_S_IFBLK;
      return true;
   }
   if (!strcmp(method, "S_ISFIFO")) {
      *out = (mode & OS_S_IFMT) == OS_S_IFIFO;
      return true;
   }
   if (!strcmp(method, "S_ISSOCK")) {
      *out = (mode & OS_S_IFMT) == OS_S_IFSOCK;
      return true;
   }
   if (!strcmp(method, "S_ISUID")) {
      *out = (mode & OS_S_ISUID) != 0;
      return true;
   }
   if (!strcmp(method, "S_ISGID")) {
      *out = (mode & OS_S_ISGID) != 0;
      return true;
   }
   if (!strcmp(method, "S_ISVTX")) {
      *out = (mode & OS_S_ISVTX) != 0;
      return true;
   }
   return false;
}

/* One body serves all three predicates; dvm__builtin_method() says which
 * declaration was invoked, exactly as the shared Field.get/set handlers do. */
static bool os_predicate_builtin(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out) {
   struct dvm_method *method = dvm__builtin_method();
   int32_t value;
   (void)self;
   if (!vm || !out || !args || nargs != 1 || !method || !method->name) {
      if (vm) throw_illegal(vm, "OsConstants predicate: expected 1 argument");
      return false;
   }
   if (!os_predicate_value(method->name, args[0].i, &value)) {
      throw_illegal(vm, "OsConstants predicate: unknown method");
      return false;
   }
   out->i = value;
   return true;
}

static void install_os_constant_predicates(struct dvm_class *cls) {
   const size_t count = OS_PREDICATE_COUNT;
   int installed = 0;

   cls->methods = calloc(count, sizeof *cls->methods);
   if (!cls->methods) return;
   for (size_t i = 0; i < count; ++i) {
      char *sig = strdup("(I)Z");
      struct dvm_method *m;
      if (!sig) break;
      m = &cls->methods[i];
      m->cls = cls;
      m->name = os_predicate_names[i];
      m->sig = sig;
      m->access = DEX_ACC_PUBLIC | DEX_ACC_STATIC;
      m->builtin = os_predicate_builtin;
      m->ret_kind = 'Z';
      m->arg_count = 1;
      m->arg_slots = 1;
      ++installed;
   }
   cls->nmethods = installed;
}

/* Register the real int fields before any application code runs.  The DEX
 * `sget` path resolves this class from the registry and reads sslots
 * directly, so the values are ordinary VM static fields, not a host hook. */
static void register_os_constants(struct dvm *vm) {
   const size_t count = sizeof os_int_constants / sizeof os_int_constants[0];
   struct dvm_class *cls;

   if (!vm) return;
   cls = dvm__register_builtin(vm, "Landroid/system/OsConstants;");
   if (!cls) {
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_WARN osconstants_registration=0 "
                        "reason=class\n");
      return;
   }
   cls->super = dvm_find_class_or_external(vm, "java/lang/Object");
   cls->access = DEX_ACC_PUBLIC | DEX_ACC_FINAL;
   cls->sfields = calloc(count, sizeof *cls->sfields);
   cls->sslots = calloc(count, sizeof *cls->sslots);
   if (!cls->sfields || !cls->sslots) {
      free(cls->sfields);
      free(cls->sslots);
      cls->sfields = NULL;
      cls->sslots = NULL;
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_WARN osconstants_registration=0 "
                        "reason=memory\n");
      return;
   }
   cls->nsfields = (int)count;
   cls->nsslots = (int)count;
   for (size_t i = 0; i < count; ++i) {
      struct dvm_field *field = &cls->sfields[i];
      field->name = os_int_constants[i].name;
      field->type = "I";
      field->access = DEX_ACC_PUBLIC | DEX_ACC_STATIC | DEX_ACC_FINAL;
      field->cls = cls;
      field->slot = (uint16_t)i;
      field->width = 1;
      field->kind = 'I';
      cls->sslots[i].i = os_int_constants[i].value;
   }
   install_os_constant_predicates(cls);
}

/* ---------------- Os call-out ---------------- */

static bool os_fallocate_call(struct dvm *vm, const union dvm_value *args,
                              int nargs, union dvm_value *out) {
   int64_t offset;
   int64_t length;
   int fd;

   (void)out;
   if (!args || nargs != 3) {
      throw_illegal(vm, "posix_fallocate: expected 3 arguments");
      return true;
   }

   offset = args[1].j;
   length = args[2].j;

   /* Validate the long pair before touching the descriptor, matching the
    * platform's argument checks and keeping offset+length representable. */
   if (offset < 0 || length < 0) {
      throw_illegal(vm, "posix_fallocate: offset and length must be non-negative");
      return true;
   }
   if (offset > INT64_MAX - length) {
      throw_illegal(vm, "posix_fallocate: offset + length is out of range");
      return true;
   }

   if (!args[0].l) {
      dvm__throw(vm, "java/lang/NullPointerException", "fd");
      return true;
   }

   fd = random_access_file_fd_of(vm, args[0].l);
   if (fd < 0) {
      /* Covers objects that are not our FileDescriptor payload, and closed
       * descriptors, exactly as Android reports a stale fd. */
      throw_errno(vm, EBADF);
      return true;
   }

   (void)os_fallocate_fd(vm, fd, offset, length);
   return true;
}

/* Java-side mmap flags to host flags.  Only the four flags the guest actually
 * needs are accepted; an unknown bit is rejected before this is called. */
static int os_host_map_flags(int flags, bool anonymous) {
   int host = 0;
   if (flags & OS_MAP_SHARED) host |= MAP_SHARED;
   if (flags & OS_MAP_PRIVATE) host |= MAP_PRIVATE;
   if (anonymous) host |= MAP_ANONYMOUS;
   return host;
}

static int os_host_prot(int prot) {
   int host = 0;
   if (prot & OS_PROT_READ) host |= PROT_READ;
   if (prot & OS_PROT_WRITE) host |= PROT_WRITE;
   if (prot & OS_PROT_EXEC) host |= PROT_EXEC;
   return host;
}

static int os_host_msync_flags(int flags) {
   int host = 0;
   if (flags & OS_MS_ASYNC) host |= MS_ASYNC;
   if (flags & OS_MS_SYNC) host |= MS_SYNC;
   if (flags & OS_MS_INVALIDATE) host |= MS_INVALIDATE;
   return host;
}

static bool os_mmap_call(struct dvm *vm, const union dvm_value *args,
                         int nargs, union dvm_value *out) {
   int64_t address;
   int64_t length;
   int64_t offset;
   int prot;
   int flags;
   int sharing;
   int fd = -1;
   int host_flags;
   int host_prot;
   int err;
   bool anonymous;
   dvm_ref fd_ref;
   void *mapped;

   if (!args || nargs != 6) {
      throw_illegal(vm, "mmap: expected 6 arguments");
      return true;
   }
   address = args[0].j;
   length = args[1].j;
   prot = args[2].i;
   flags = args[3].i;
   fd_ref = args[4].l;
   offset = args[5].j;

   /* Android's Os.mmap only supports the null-address form; a fixed address
    * would let guest data choose a host mapping and is never needed here. */
   if (address != 0) {
      throw_illegal(vm, "mmap: non-zero address is not supported");
      return true;
   }
   if (length < 0) {
      throw_illegal(vm, "mmap: length must be non-negative");
      return true;
   }
   if (length == 0 || (uint64_t)length > (uint64_t)SIZE_MAX) {
      throw_errno_op(vm, "mmap", EINVAL);
      return true;
   }
   if (prot & ~OS_PROT_MASK) {
      throw_illegal(vm, "mmap: invalid prot");
      return true;
   }
   if (flags & ~OS_MAP_MASK) {
      throw_illegal(vm, "mmap: unknown flags");
      return true;
   }
   sharing = flags & (OS_MAP_SHARED | OS_MAP_PRIVATE);
   if (sharing != OS_MAP_SHARED && sharing != OS_MAP_PRIVATE) {
      throw_illegal(
         vm, "mmap: exactly one of MAP_SHARED or MAP_PRIVATE is required");
      return true;
   }
   if (flags & OS_MAP_FIXED) {
      throw_illegal(vm, "mmap: MAP_FIXED is not supported with address 0");
      return true;
   }

   /* A null FileDescriptor means an anonymous mapping, matching
    * android.system.Os; MAP_ANONYMOUS may also be given explicitly. */
   anonymous = fd_ref == 0 || (flags & OS_MAP_ANONYMOUS);
   if (anonymous) {
      if (offset != 0) {
         throw_errno_op(vm, "mmap", EINVAL);
         return true;
      }
   } else {
      if (offset < 0) {
         throw_illegal(vm, "mmap: offset must be non-negative");
         return true;
      }
      fd = random_access_file_fd_of(vm, fd_ref);
      if (fd < 0) {
         throw_errno_op(vm, "mmap", EBADF);
         return true;
      }
   }
   if (!out) {
      dvm__throw(vm, "java/lang/IllegalStateException",
                 "mmap: missing result slot");
      return true;
   }

   host_prot = os_host_prot(prot);
   host_flags = os_host_map_flags(flags, anonymous);
   errno = 0;
   mapped = mmap(NULL, (size_t)length, host_prot, host_flags,
                 anonymous ? -1 : fd, anonymous ? (off_t)0 : (off_t)offset);
   if (mapped == MAP_FAILED) {
      err = errno ? errno : EIO;
      throw_errno_op(vm, "mmap", err);
      return true;
   }

   /* If the registry cannot grow, drop the mapping rather than hand out an
    * address munmap would later refuse. */
   if (!os_mapping_record((int64_t)(intptr_t)mapped, length, prot, flags)) {
      (void)munmap(mapped, (size_t)length);
      throw_errno_op(vm, "mmap", ENOMEM);
      return true;
   }
   out->j = (int64_t)(intptr_t)mapped;
   return true;
}

static bool os_munmap_call(struct dvm *vm, const union dvm_value *args,
                           int nargs, union dvm_value *out) {
   int64_t address;
   int64_t length;
   int err;
   struct os_mapping *mapping;

   (void)out;
   if (!args || nargs != 2) {
      throw_illegal(vm, "munmap: expected 2 arguments");
      return true;
   }
   address = args[0].j;
   length = args[1].j;
   if (length < 0) {
      throw_illegal(vm, "munmap: length must be non-negative");
      return true;
   }
   if (length == 0 || (uint64_t)length > (uint64_t)SIZE_MAX) {
      throw_errno_op(vm, "munmap", EINVAL);
      return true;
   }

   /* Exact base/length only: a partial range could unmap host memory this
    * module did not create, and BLKV always closes the whole mapping. */
   mapping = os_mapping_find_exact(address, length);
   if (!mapping) {
      throw_errno_op(vm, "munmap", EINVAL);
      return true;
   }

   errno = 0;
   if (munmap((void *)(intptr_t)address, (size_t)length)) {
      err = errno ? errno : EIO;
      throw_errno_op(vm, "munmap", err);
      return true;
   }
   os_mapping_forget(mapping);
   return true;
}

static bool os_msync_call(struct dvm *vm, const union dvm_value *args,
                          int nargs, union dvm_value *out) {
   int64_t address;
   int64_t length;
   int flags;
   int sync;
   int err;

   (void)out;
   if (!args || nargs != 3) {
      throw_illegal(vm, "msync: expected 3 arguments");
      return true;
   }
   address = args[0].j;
   length = args[1].j;
   flags = args[2].i;
   if (length < 0) {
      throw_illegal(vm, "msync: length must be non-negative");
      return true;
   }
   if (length == 0 || (uint64_t)length > (uint64_t)SIZE_MAX) {
      throw_errno_op(vm, "msync", EINVAL);
      return true;
   }
   if (flags & ~OS_MS_MASK) {
      throw_illegal(vm, "msync: unknown flags");
      return true;
   }
   sync = flags & (OS_MS_ASYNC | OS_MS_SYNC);
   if (sync != OS_MS_ASYNC && sync != OS_MS_SYNC) {
      throw_illegal(vm, "msync: exactly one of MS_ASYNC or MS_SYNC is required");
      return true;
   }
   if (!os_mapping_find_range(address, length)) {
      throw_errno_op(vm, "msync", EINVAL);
      return true;
   }

   errno = 0;
   if (msync((void *)(intptr_t)address, (size_t)length,
             os_host_msync_flags(flags))) {
      err = errno ? errno : EIO;
      throw_errno_op(vm, "msync", err);
      return true;
   }
   return true;
}

static bool os_constants_call(struct dvm *vm, const char *method,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out) {
   int32_t value;
   if (!args || nargs != 1 || !out) {
      throw_illegal(vm, "OsConstants predicate: expected 1 argument");
      return true;
   }
   if (!os_predicate_value(method, args[0].i, &value)) {
      throw_illegal(vm, "OsConstants predicate: unknown method");
      return true;
   }
   out->i = value;
   return true;
}

/* Only the methods the DEX actually references are handled.  Everything else
 * returns false so strict_runtime.c keeps emitting MISSING_API. */
bool android_os_call(struct dvm *vm, const char *cls, const char *method,
                     const char *sig, dvm_ref self,
                     const union dvm_value *args, int nargs,
                     union dvm_value *out) {
   (void)self;
   if (!vm || !cls || !method || !sig) return false;

   if (!strcmp(cls, "android/system/Os")) {
      if (!strcmp(method, "posix_fallocate") &&
          !strcmp(sig, "(Ljava/io/FileDescriptor;JJ)V"))
         return os_fallocate_call(vm, args, nargs, out);
      if (!strcmp(method, "mmap") &&
          !strcmp(sig, "(JJIILjava/io/FileDescriptor;J)J"))
         return os_mmap_call(vm, args, nargs, out);
      if (!strcmp(method, "munmap") && !strcmp(sig, "(JJ)V"))
         return os_munmap_call(vm, args, nargs, out);
      if (!strcmp(method, "msync") && !strcmp(sig, "(JJI)V"))
         return os_msync_call(vm, args, nargs, out);
      return false;
   }
   if (!strcmp(cls, "android/system/OsConstants")) {
      bool known = false;
      for (size_t i = 0; i < OS_PREDICATE_COUNT; ++i)
         if (!strcmp(method, os_predicate_names[i])) {
            known = true;
            break;
         }
      /* An unmodelled predicate must reach strict_runtime's MISSING_API
       * instead of being answered with an exception from here. */
      if (known && !strcmp(sig, "(I)Z"))
         return os_constants_call(vm, method, args, nargs, out);
      return false;
   }
   return false;
}

bool android_os_configure(const char *sdk_dir) {
   char path[PATH_MAX];
   char *slash;
   struct stat st;

   /* The dispatcher can only obtain a host descriptor through
    * random_access_file's wrappers, so its root is the real dependency.
    * Calling this again with the same directory is idempotent. */
   raf_configured = random_access_file_configure(sdk_dir);
   if (!raf_configured) {
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_WARN random_access_file_configure=0\n");
      return false;
   }

   /* Derive the same <private_base>/guest-files root only so the selftest can
    * create a real guest file.  A failure here degrades the selftest to the
    * host /tmp path; it must not block a runtime whose descriptor path is
    * already configured. */
   guest_root[0] = 0;
   guest_root_ready = false;
   if (sdk_dir && realpath(sdk_dir, path) &&
       (slash = strrchr(path, '/')) != NULL && slash != path) {
      *slash = 0; /* parent of the SDK directory, like random_access_file.c */
      if (path[0] &&
          snprintf(guest_root, sizeof guest_root, "%s/guest-files", path) <
             (int)sizeof guest_root &&
          !(mkdir(guest_root, 0700) && access(guest_root, F_OK)) &&
          lstat(guest_root, &st) == 0 && S_ISDIR(st.st_mode) &&
          !S_ISLNK(st.st_mode))
         guest_root_ready = true;
   }
   if (!guest_root_ready) {
      guest_root[0] = 0;
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_WARN guest_root=unavailable "
                        "selftest_file_descriptor=skip\n");
   }
   return true;
}

void android_os_vm_start(struct dvm *vm) {
   /* A previous VM that was not closed must not leave host mappings behind. */
   if (active_vm && active_vm != vm) os_mappings_release(active_vm);
   active_vm = vm;
   register_os_constants(vm);
   if (!guest_root_ready)
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_WARN guest_root=unconfigured "
                        "selftest_file_descriptor=skip\n");
   if (vm && !ensure_errno_exception(vm))
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_WARN errno_exception_parent=unresolved "
                        "fallback=java/io/IOException\n");
}

void android_os_vm_end(struct dvm *vm) {
   if (vm && active_vm && vm != active_vm) return;
   os_mappings_release(vm);
   if (!vm || vm == active_vm) active_vm = NULL;
}

/* ---------------- selftest ---------------- */

#define OS_CHECK(cond)                                                        \
   do {                                                                       \
      ++checks;                                                               \
      if (!(cond)) {                                                          \
         okay = false;                                                        \
         sdk_trace_fprintf(stderr,                                            \
                           "ANDROID_OS_SELFTEST FAIL check=%s line=%d\n",     \
                           #cond, __LINE__);                                  \
      }                                                                       \
   } while (0)

/* Fetch and clear the pending exception, checking its exact class and, for
 * ErrnoException, that the Exception parent edge is visible to catch matching. */
static bool exception_expect(struct dvm *vm, const char *class_name,
                             const char *needle, bool must_be_exception) {
   dvm_ref e = dvm_exception(vm);
   char message[512];
   struct dvm_class *actual;
   bool match = false;

   if (!e) return false;
   actual = dvm_object_class(vm, e);
   if (actual && actual->name && !strcmp(actual->name, class_name)) {
      match = true;
      if (must_be_exception) {
         struct dvm_class *base =
            dvm_find_class_or_external(vm, "java/lang/Exception");
         match = base && dvm__class_assignable(vm, actual, base);
      }
      if (match && needle) {
         message[0] = 0;
         dvm_describe_exception(vm, e, message, sizeof message);
         match = strstr(message, needle) != NULL;
      }
   }
   dvm_clear_exception(vm);
   return match;
}

static bool dispatch_fallocate(struct dvm *vm, dvm_ref fd, int64_t offset,
                               int64_t length) {
   union dvm_value args[3] = {{0}}, out = {0};
   args[0].l = fd;
   args[1].j = offset;
   args[2].j = length;
   return android_os_call(vm, "android/system/Os", "posix_fallocate",
                          "(Ljava/io/FileDescriptor;JJ)V", 0, args, 3, &out);
}

static bool dispatch_mmap(struct dvm *vm, int64_t address, int64_t length,
                          int32_t prot, int32_t flags, dvm_ref fd,
                          int64_t offset, int64_t *addr_out) {
   union dvm_value args[6] = {{0}}, out = {0};
   bool handled;
   args[0].j = address;
   args[1].j = length;
   args[2].i = prot;
   args[3].i = flags;
   args[4].l = fd;
   args[5].j = offset;
   handled = android_os_call(vm, "android/system/Os", "mmap",
                             "(JJIILjava/io/FileDescriptor;J)J", 0, args, 6,
                             &out);
   if (addr_out)
      *addr_out = (handled && !dvm_exception(vm)) ? out.j : 0;
   return handled;
}

static bool dispatch_munmap(struct dvm *vm, int64_t address,
                            int64_t length) {
   union dvm_value args[2] = {{0}}, out = {0};
   args[0].j = address;
   args[1].j = length;
   return android_os_call(vm, "android/system/Os", "munmap", "(JJ)V", 0, args,
                          2, &out);
}

static bool dispatch_msync(struct dvm *vm, int64_t address, int64_t length,
                           int32_t flags) {
   union dvm_value args[3] = {{0}}, out = {0};
   args[0].j = address;
   args[1].j = length;
   args[2].i = flags;
   return android_os_call(vm, "android/system/Os", "msync", "(JJI)V", 0, args,
                          3, &out);
}

static bool dispatch_predicate(struct dvm *vm, const char *method,
                               int32_t mode, int32_t *value_out) {
   union dvm_value args[1] = {{0}}, out = {0};
   bool handled;
   args[0].i = mode;
   handled = android_os_call(vm, "android/system/OsConstants", method, "(I)Z",
                             0, args, 1, &out);
   if (value_out) *value_out = out.i;
   return handled;
}

static bool os_constant_is(struct dvm *vm, const char *name, int32_t want) {
   struct dvm_class *cls =
      dvm_find_class_or_external(vm, "android/system/OsConstants");
   if (!cls || !cls->sfields || !cls->sslots) return false;
   for (int i = 0; i < cls->nsfields; ++i) {
      struct dvm_field *field = &cls->sfields[i];
      int slot;
      if (!field->name || strcmp(field->name, name)) continue;
      if (!field->type || strcmp(field->type, "I")) continue;
      slot = field->slot;
      return slot >= 0 && slot < cls->nsslots &&
             cls->sslots[slot].i == want;
   }
   return false;
}

static bool predicate_is(struct dvm *vm, const char *method, int32_t mode,
                         int32_t want) {
   int32_t value = -1;
   bool handled = dispatch_predicate(vm, method, mode, &value);
   bool okay = handled && !dvm_exception(vm) && value == want;
   if (dvm_exception(vm)) dvm_clear_exception(vm);
   return okay;
}

static dvm_ref raf_open(struct dvm *vm, const char *path, const char *mode,
                        bool *okay_out) {
   struct dvm_class *cls =
      dvm_find_class_or_external(vm, "java/io/RandomAccessFile");
   dvm_ref file = cls ? dvm_new_object(vm, cls) : 0;
   dvm_ref path_string = dvm_new_string(vm, path);
   dvm_ref mode_string = dvm_new_string(vm, mode);
   union dvm_value args[2] = {{0}}, out = {0};
   bool okay = false;

   if (file && path_string && mode_string) {
      args[0].l = path_string;
      args[1].l = mode_string;
      okay = random_access_file_call(
                vm, "java/io/RandomAccessFile", "<init>",
                "(Ljava/lang/String;Ljava/lang/String;)V", file, args, 2,
                &out) &&
             !dvm_exception(vm);
   }
   *okay_out = okay;
   return okay ? file : 0;
}

static dvm_ref raf_get_fd(struct dvm *vm, dvm_ref file, bool *okay_out) {
   union dvm_value out = {0};
   bool okay = random_access_file_call(
                  vm, "java/io/RandomAccessFile", "getFD",
                  "()Ljava/io/FileDescriptor;", file, NULL, 0, &out) &&
               !dvm_exception(vm) && out.l;
   *okay_out = okay;
   return okay ? out.l : 0;
}

static void raf_close(struct dvm *vm, dvm_ref file) {
   union dvm_value out = {0};
   if (!file) return;
   (void)random_access_file_call(vm, "java/io/RandomAccessFile", "close",
                                 "()V", file, NULL, 0, &out);
   if (dvm_exception(vm)) dvm_clear_exception(vm);
}

bool android_os_selftest(struct dvm *vm) {
   char tmpl[PATH_MAX] = {0};
   char path[PATH_MAX] = {0};
   char *dir = NULL;
   const char *base;
   unsigned checks = 0;
   bool okay = true;
   bool use_guest;
   bool opened;
   int fd = -1;
   int ro_fd = -1;
   int host_fd;
   dvm_ref raf = 0;
   dvm_ref ro_raf = 0;
   dvm_ref fd_ref = 0;
   dvm_ref ro_fd_ref = 0;
   dvm_ref bogus_fd;
   struct stat st;
   struct dvm_class *fd_class;
   struct dvm_class *constants;
   int64_t map_addr;
   unsigned char byte = 0;

   if (!vm) {
      sdk_trace_fprintf(stderr, "ANDROID_OS_SELFTEST FAIL null_vm\n");
      return false;
   }
   OS_CHECK(ensure_errno_exception(vm));

   /* The class must be a real VM class with real int slots: BLKV reads the
    * constants with sget, not through an external-static hook. */
   constants = dvm_find_class_or_external(vm, "android/system/OsConstants");
   OS_CHECK(constants != NULL && !constants->external);
   OS_CHECK(constants && constants->nsfields > 0 && constants->sslots);
   OS_CHECK(os_constant_is(vm, "PROT_READ", OS_PROT_READ));
   OS_CHECK(os_constant_is(vm, "PROT_WRITE", OS_PROT_WRITE));
   OS_CHECK(os_constant_is(vm, "MAP_SHARED", OS_MAP_SHARED));
   OS_CHECK(os_constant_is(vm, "MAP_PRIVATE", OS_MAP_PRIVATE));
   OS_CHECK(os_constant_is(vm, "MAP_ANONYMOUS", OS_MAP_ANONYMOUS));
   OS_CHECK(os_constant_is(vm, "MS_SYNC", OS_MS_SYNC));
   OS_CHECK(os_constant_is(vm, "S_IFMT", OS_S_IFMT));
   OS_CHECK(os_constant_is(vm, "S_IFREG", OS_S_IFREG));
   OS_CHECK(os_constant_is(vm, "S_IFDIR", OS_S_IFDIR));
   OS_CHECK(os_constant_is(vm, "S_IFLNK", OS_S_IFLNK));
   OS_CHECK(os_constant_is(vm, "S_IRUSR", OS_S_IRUSR));
   OS_CHECK(os_constant_is(vm, "S_IWUSR", OS_S_IWUSR));
   OS_CHECK(predicate_is(vm, "S_ISREG", OS_S_IFREG | 0644, 1));
   OS_CHECK(predicate_is(vm, "S_ISDIR", OS_S_IFDIR | 0755, 1));
   OS_CHECK(predicate_is(vm, "S_ISLNK", OS_S_IFLNK | 0777, 1));
   OS_CHECK(predicate_is(vm, "S_ISREG", OS_S_IFDIR | 0644, 0));
   OS_CHECK(predicate_is(vm, "S_ISCHR", OS_S_IFCHR | 0600, 1));
   OS_CHECK(predicate_is(vm, "S_ISBLK", OS_S_IFBLK | 0600, 1));
   OS_CHECK(predicate_is(vm, "S_ISFIFO", OS_S_IFIFO | 0600, 1));
   OS_CHECK(predicate_is(vm, "S_ISSOCK", OS_S_IFSOCK | 0600, 1));
   OS_CHECK(predicate_is(vm, "S_ISUID", OS_S_IFREG | OS_S_ISUID | 0644, 1));
   OS_CHECK(predicate_is(vm, "S_ISUID", OS_S_IFREG | 0644, 0));
   /* The predicates must also be installed as real static builtin methods so
    * reflection and direct invoke find them; the dispatcher path above is the
    * fallback. */
   {
      struct dvm_method *pred =
         constants ? dvm_find_method(vm, constants, "S_ISREG", "(I)Z") : NULL;
      OS_CHECK(pred != NULL);
      OS_CHECK(pred && pred->builtin == os_predicate_builtin);
      OS_CHECK(pred && (pred->access & DEX_ACC_STATIC));
   }

   /* Anonymous mmap: null descriptor means MAP_ANONYMOUS is supplied, and the
    * returned value is a real host address this process can read and write. */
   map_addr = 0;
   OS_CHECK(dispatch_mmap(vm, 0, 4096, OS_PROT_READ | OS_PROT_WRITE,
                          OS_MAP_PRIVATE, 0, 0, &map_addr));
   OS_CHECK(!dvm_exception(vm));
   if (dvm_exception(vm)) dvm_clear_exception(vm);
   OS_CHECK(map_addr != 0 && map_addr != -1);
   if (map_addr > 0) {
      volatile unsigned char *bytes =
         (volatile unsigned char *)(intptr_t)map_addr;
      bytes[0] = 0x5a;
      bytes[4095] = 0xa5;
      OS_CHECK(bytes[0] == 0x5a);
      OS_CHECK(bytes[4095] == 0xa5);

      OS_CHECK(dispatch_munmap(vm, map_addr, 4096));
      OS_CHECK(!dvm_exception(vm));
      if (dvm_exception(vm)) dvm_clear_exception(vm);

      /* No dereference after either failure check: the range is gone. */
      OS_CHECK(dispatch_munmap(vm, map_addr, 4096));
      OS_CHECK(exception_expect(vm, "android/system/ErrnoException",
                                "(errno 22)", true));
      OS_CHECK(dispatch_msync(vm, map_addr, 4096, OS_MS_SYNC));
      OS_CHECK(exception_expect(vm, "android/system/ErrnoException",
                                "(errno 22)", true));
   }

   /* mmap parameter failures must be raised, never rounded to success. */
   OS_CHECK(dispatch_mmap(vm, 0x1000, 4096, OS_PROT_READ,
                          OS_MAP_PRIVATE, 0, 0, NULL));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "non-zero address", false));
   OS_CHECK(dispatch_mmap(vm, 0, -1, OS_PROT_READ, OS_MAP_PRIVATE, 0, 0,
                          NULL));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "non-negative", false));
   OS_CHECK(dispatch_mmap(vm, 0, 0, OS_PROT_READ, OS_MAP_PRIVATE, 0, 0, NULL));
   OS_CHECK(exception_expect(vm, "android/system/ErrnoException", "(errno 22)",
                             true));
   OS_CHECK(dispatch_mmap(vm, 0, 4096, 0x8, OS_MAP_PRIVATE, 0, 0, NULL));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException", "prot",
                             false));
   OS_CHECK(dispatch_mmap(vm, 0, 4096, OS_PROT_READ, 0x8, 0, 0, NULL));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException", "flags",
                             false));
   OS_CHECK(dispatch_mmap(vm, 0, 4096, OS_PROT_READ, 0, 0, 0, NULL));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "MAP_SHARED", false));
   OS_CHECK(dispatch_mmap(vm, 0, 4096, OS_PROT_READ,
                          OS_MAP_SHARED | OS_MAP_PRIVATE, 0, 0, NULL));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "MAP_SHARED", false));
   OS_CHECK(dispatch_mmap(vm, 0, 4096, OS_PROT_READ,
                          OS_MAP_PRIVATE | OS_MAP_ANONYMOUS | OS_MAP_FIXED, 0,
                          0, NULL));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "MAP_FIXED", false));
   OS_CHECK(dispatch_mmap(vm, 0, 4096, OS_PROT_READ,
                          OS_MAP_PRIVATE | OS_MAP_ANONYMOUS, 0, 4096, NULL));
   OS_CHECK(exception_expect(vm, "android/system/ErrnoException", "(errno 22)",
                             true));

   /* munmap/msync must reject ranges they did not create. */
   OS_CHECK(dispatch_munmap(vm, 0x1000, 4096));
   OS_CHECK(exception_expect(vm, "android/system/ErrnoException", "(errno 22)",
                             true));
   OS_CHECK(dispatch_msync(vm, 0x1000, 4096, OS_MS_SYNC));
   OS_CHECK(exception_expect(vm, "android/system/ErrnoException", "(errno 22)",
                             true));
   OS_CHECK(dispatch_munmap(vm, 0, 4096));
   OS_CHECK(exception_expect(vm, "android/system/ErrnoException", "(errno 22)",
                             true));
   OS_CHECK(dispatch_msync(vm, 0, 4096, 0));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "MS_ASYNC", false));
   OS_CHECK(dispatch_msync(vm, 0, 4096, OS_MS_ASYNC | OS_MS_SYNC));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "MS_ASYNC", false));

   /* Dispatcher validation is independent of any guest file.  Parameter errors
    * must be raised before descriptor access, so the null descriptor here does
    * not hide the illegal offset/length checks. */
   OS_CHECK(dispatch_fallocate(vm, 0, 0, 4096));
   OS_CHECK(exception_expect(vm, "java/lang/NullPointerException", "fd",
                             false));
   OS_CHECK(dispatch_fallocate(vm, 0, -1, 4096));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "non-negative", false));
   OS_CHECK(dispatch_fallocate(vm, 0, 0, -1));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "non-negative", false));
   OS_CHECK(dispatch_fallocate(vm, 0, INT64_MAX, 1));
   OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                             "out of range", false));

   /* A syntactically valid FileDescriptor with no host payload is exactly the
    * closed/foreign-descriptor failure Android reports as EBADF. */
   fd_class = dvm_find_class_or_external(vm, "java/io/FileDescriptor");
   bogus_fd = fd_class ? dvm_new_object(vm, fd_class) : 0;
   OS_CHECK(bogus_fd != 0);
   if (bogus_fd) {
      OS_CHECK(dispatch_fallocate(vm, bogus_fd, 0, 4096));
      OS_CHECK(exception_expect(vm, "android/system/ErrnoException",
                                "(errno 9)", true));
      OS_CHECK(dispatch_mmap(vm, 0, 4096, OS_PROT_READ, OS_MAP_SHARED,
                             bogus_fd, 0, NULL));
      OS_CHECK(exception_expect(vm, "android/system/ErrnoException",
                                "(errno 9)", true));
   }

   use_guest = guest_root_ready;
   base = use_guest ? guest_root : "/tmp";
   if (snprintf(tmpl, sizeof tmpl, "%s/android-os-test-XXXXXX", base) >=
       (int)sizeof tmpl) {
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_SELFTEST FAIL temp_template_too_long\n");
      return false;
   }
   dir = mkdtemp(tmpl);
   if (!dir) {
      int saved = errno;
      if (use_guest) {
         sdk_trace_fprintf(stderr,
                           "ANDROID_OS_SELFTEST FAIL mkdtemp "
                           "base=guest_files errno=%d\n",
                           saved);
         return false;
      }
      /* No guest root and no writable host /tmp: only the dispatcher and
       * anonymous-mapping checks above were possible.  They are real
       * assertions, but the descriptor path must not be reported as
       * exercised. */
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_SELFTEST SKIP file_descriptor "
                        "configured=0 host_tmp_unavailable=1 errno=%d\n",
                        saved);
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_SELFTEST %s checks=%u "
                        "dispatcher_only=1 mmap_anon=%d\n",
                        okay ? "PASS" : "FAIL", checks, map_addr != 0);
      return okay;
   }
   if (snprintf(path, sizeof path, "%s/data.bin", dir) >= (int)sizeof path) {
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_SELFTEST FAIL temp_path_too_long\n");
      rmdir(dir);
      return false;
   }

   /* 1. Direct host descriptor: real fallocate, then fstat the size. */
   fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   OS_CHECK(fd >= 0);
   if (fd >= 0) {
      OS_CHECK(os_fallocate_fd(vm, fd, 4096, 8192));
      OS_CHECK(!dvm_exception(vm));
      if (dvm_exception(vm)) dvm_clear_exception(vm);
      memset(&st, 0, sizeof st);
      OS_CHECK(fstat(fd, &st) == 0);
      OS_CHECK(st.st_size == (off_t)(4096 + 8192));
      OS_CHECK(close(fd) == 0);
      fd = -1;
   }

   /* 2. Direct read-only descriptor: the syscall must fail, not fake success. */
   ro_fd = open(path, O_RDONLY | O_CLOEXEC);
   OS_CHECK(ro_fd >= 0);
   if (ro_fd >= 0) {
      OS_CHECK(!os_fallocate_fd(vm, ro_fd, 0, 4096));
      OS_CHECK(exception_expect(vm, "android/system/ErrnoException",
                                "(errno 9)", true));
      OS_CHECK(close(ro_fd) == 0);
      ro_fd = -1;
   }

   if (!use_guest) {
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_SELFTEST SKIP file_descriptor "
                        "configured=0\n");
   } else if (!raf_configured) {
      OS_CHECK(false);
      sdk_trace_fprintf(stderr,
                        "ANDROID_OS_SELFTEST FAIL random_access_file=0\n");
   } else {
      /* 4. The production shape: RandomAccessFile -> getFD ->
       * Os.posix_fallocate, then stat/fstat the same underlying file. */
      opened = false;
      raf = raf_open(vm, path, "rw", &opened);
      OS_CHECK(opened && raf != 0);
      if (dvm_exception(vm)) dvm_clear_exception(vm);
      if (raf) {
         opened = false;
         fd_ref = raf_get_fd(vm, raf, &opened);
         OS_CHECK(opened && fd_ref != 0);
         if (dvm_exception(vm)) dvm_clear_exception(vm);
      }
      if (fd_ref) {
         OS_CHECK(dispatch_fallocate(vm, fd_ref, 16384, 4096));
         OS_CHECK(!dvm_exception(vm));
         if (dvm_exception(vm)) dvm_clear_exception(vm);

         memset(&st, 0, sizeof st);
         OS_CHECK(stat(path, &st) == 0);
         OS_CHECK(st.st_size == (off_t)(16384 + 4096));

         host_fd = random_access_file_fd_of(vm, fd_ref);
         OS_CHECK(host_fd >= 0);
         if (host_fd >= 0) {
            memset(&st, 0, sizeof st);
            OS_CHECK(fstat(host_fd, &st) == 0);
            OS_CHECK(st.st_size == (off_t)(16384 + 4096));

            /* 4a. File-backed mmap through the guest FileDescriptor.  The
             * write must reach the file through msync, proving both the
             * returned address and the mapping table are real. */
            map_addr = 0;
            OS_CHECK(dispatch_mmap(vm, 0, 4096, OS_PROT_READ | OS_PROT_WRITE,
                                   OS_MAP_SHARED, fd_ref, 0, &map_addr));
            OS_CHECK(!dvm_exception(vm));
            if (dvm_exception(vm)) dvm_clear_exception(vm);
            OS_CHECK(map_addr != 0 && map_addr != -1);
            if (map_addr > 0) {
               volatile unsigned char *bytes =
                  (volatile unsigned char *)(intptr_t)map_addr;
               bytes[0] = 0x5a;
               bytes[4095] = 0xa5;
               OS_CHECK(dispatch_msync(vm, map_addr, 4096, OS_MS_SYNC));
               OS_CHECK(!dvm_exception(vm));
               if (dvm_exception(vm)) dvm_clear_exception(vm);
               OS_CHECK(dispatch_munmap(vm, map_addr, 4096));
               OS_CHECK(!dvm_exception(vm));
               if (dvm_exception(vm)) dvm_clear_exception(vm);
               byte = 0;
               OS_CHECK(pread(host_fd, &byte, 1, 0) == 1);
               OS_CHECK(byte == 0x5a);
            }
         }

         /* The same validation must fire before descriptor access. */
         OS_CHECK(dispatch_fallocate(vm, fd_ref, -1, 4096));
         OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                                   "non-negative", false));
         OS_CHECK(dispatch_fallocate(vm, fd_ref, 0, -1));
         OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                                   "non-negative", false));
         OS_CHECK(dispatch_fallocate(vm, fd_ref, INT64_MAX, 1));
         OS_CHECK(exception_expect(vm, "java/lang/IllegalArgumentException",
                                   "out of range", false));
      }

      /* 5. Read-only RandomAccessFile: underlying fallocate is EBADF and the
       * file size must remain unchanged. */
      opened = false;
      ro_raf = raf_open(vm, path, "r", &opened);
      OS_CHECK(opened && ro_raf != 0);
      if (dvm_exception(vm)) dvm_clear_exception(vm);
      if (ro_raf) {
         opened = false;
         ro_fd_ref = raf_get_fd(vm, ro_raf, &opened);
         OS_CHECK(opened && ro_fd_ref != 0);
         if (dvm_exception(vm)) dvm_clear_exception(vm);
      }
      if (ro_fd_ref) {
         OS_CHECK(dispatch_fallocate(vm, ro_fd_ref, 0, 4096));
         OS_CHECK(exception_expect(vm, "android/system/ErrnoException",
                                   "(errno 9)", true));
         memset(&st, 0, sizeof st);
         OS_CHECK(stat(path, &st) == 0);
         OS_CHECK(st.st_size == (off_t)(16384 + 4096));
      }
   }

   raf_close(vm, raf);
   raf_close(vm, ro_raf);
   if (fd >= 0) close(fd);
   if (ro_fd >= 0) close(ro_fd);
   unlink(path);
   rmdir(dir);

   if (okay) {
      sdk_trace_fprintf(
         stderr,
         "ANDROID_OS_SELFTEST PASS checks=%u guest_root=%s "
         "real_fd_fallocate=1 stat_size=1 invalid_fd_EBADF=1 "
         "readonly_fd_EBADF=1 overflow_illegal=1 osconstants=1 "
         "mmap_anon_rw=1 mmap_file_msync=1 munmap_einval=1\n",
         checks, use_guest ? "configured" : "unconfigured");
   } else {
      sdk_trace_fprintf(stderr, "ANDROID_OS_SELFTEST FAIL checks=%u\n",
                        checks);
   }
   return okay;
}
