#include "gc_runtime.h"
/* Real java.io.RandomAccessFile over the guest file roots owned by
 * file_runtime.c.  All file effects go through host descriptors; a failure is
 * reported as the Java exception it would be on Android, never as success. */
#include "random_access_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0
#endif

extern int sdk_trace_fprintf(FILE *, const char *, ...);
extern bool file_runtime_call(struct dvm *, const char *, const char *,
                              const char *, dvm_ref, const union dvm_value *,
                              int, union dvm_value *) __attribute__((weak));

static struct dvm *active;
static char private_base[PATH_MAX];
static char root[PATH_MAX];
static char cache_root[PATH_MAX];

typedef struct raf_handle {
   pthread_mutex_t lock;
   int fd;
   int closed;
   int writable;
   int refs;
   int64_t pos;
} raf_handle;

static void handle_release(raf_handle *h);
static void raf_free(void *p) { handle_release((raf_handle *)p); }
static void fd_free(void *p) { handle_release((raf_handle *)p); }
static void channel_free(void *p) { handle_release((raf_handle *)p); }

/* ---------------- diagnostics and small helpers ---------------- */

static bool io_error(struct dvm *v, const char *cls, const char *what, int err) {
   char msg[320];
   if (err > 0)
      snprintf(msg, sizeof msg, "%s: %s (errno %d)", what, strerror(err), err);
   else
      snprintf(msg, sizeof msg, "%s", what);
   dvm__throw(v, cls, "%s", msg);
   return true;
}

static bool throw_msg(struct dvm *v, const char *cls, const char *fmt, ...) {
   char msg[320];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof msg, fmt, ap);
   va_end(ap);
   dvm__throw(v, cls, "%s", msg);
   return true;
}

static bool under_base(const char *base, const char *path) {
   size_t n = strlen(base);
   if (!n) return false;
   if (strncmp(path, base, n) != 0) return false;
   if (n == 1 && base[0] == '/') return path[1] != 0;
   return path[n] == '/' && path[n + 1] != 0;
}

static bool allowed_guest(const char *path) {
   if (!private_base[0]) return false;
   if (!under_base(private_base, path)) return false;
   return under_base(root, path) || under_base(cache_root, path);
}

/* Rejects empty, "." and ".." components, duplicate slashes and a trailing
 * slash.  It is a lexical companion to the openat(O_NOFOLLOW) walk below. */
static bool safe_path(const char *p) {
   size_t n = strlen(p);
   size_t i;
   if (!n || p[0] != '/' || p[n - 1] == '/') return false;
   i = 1;
   while (i < n) {
      size_t j = i;
      size_t len;
      while (j < n && p[j] != '/') j++;
      len = j - i;
      if (len == 0) return false;
      if ((len == 1 && p[i] == '.') ||
          (len == 2 && p[i] == '.' && p[i + 1] == '.')) return false;
      i = j;
      if (i < n) {
         i++;
         if (i == n) return false;
      }
   }
   return true;
}

/* Internal DVM strings are modified UTF-8.  Guest paths must be ordinary
 * absolute UTF-8 without U+0000, so decode (combining surrogate pairs) and
 * re-encode.  NUL, lone surrogates, overlong forms and 5/6-byte forms fail. */
static bool path_from_string(const char *src, size_t n, char *out, size_t cap) {
   size_t i = 0, o = 0;
   while (i < n) {
      unsigned b = (unsigned char)src[i++];
      uint32_t cp;
      if (b < 0x80) {
         if (b == 0) return false;
         cp = b;
      } else if ((b & 0xE0) == 0xC0) {
         unsigned b2;
         if (i >= n) return false;
         b2 = (unsigned char)src[i];
         if ((b2 & 0xC0) != 0x80) return false;
         i++;
         cp = ((uint32_t)(b & 0x1F) << 6) | (b2 & 0x3F);
         if (cp < 0x80) return false; /* includes C0 80 (U+0000) */
      } else if ((b & 0xF0) == 0xE0) {
         unsigned b2, b3;
         if (i + 1 >= n) return false;
         b2 = (unsigned char)src[i];
         b3 = (unsigned char)src[i + 1];
         if ((b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
         i += 2;
         cp = ((uint32_t)(b & 0x0F) << 12) |
              ((uint32_t)(b2 & 0x3F) << 6) | (b3 & 0x3F);
         if (cp < 0x800) return false;
         if (cp >= 0xD800 && cp <= 0xDBFF) {
            unsigned c1, c2, c3;
            uint32_t lo;
            if (i + 2 >= n) return false;
            c1 = (unsigned char)src[i];
            c2 = (unsigned char)src[i + 1];
            c3 = (unsigned char)src[i + 2];
            if ((c1 & 0xF0) != 0xE0 || (c2 & 0xC0) != 0x80 ||
                (c3 & 0xC0) != 0x80) return false;
            lo = ((uint32_t)(c1 & 0x0F) << 12) |
                 ((uint32_t)(c2 & 0x3F) << 6) | (c3 & 0x3F);
            if (lo < 0xDC00 || lo > 0xDFFF) return false;
            i += 3;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
         } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
         }
      } else if ((b & 0xF8) == 0xF0) {
         unsigned b2, b3, b4;
         if (i + 2 >= n) return false;
         b2 = (unsigned char)src[i];
         b3 = (unsigned char)src[i + 1];
         b4 = (unsigned char)src[i + 2];
         if ((b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80 ||
             (b4 & 0xC0) != 0x80) return false;
         i += 3;
         cp = ((uint32_t)(b & 0x07) << 18) |
              ((uint32_t)(b2 & 0x3F) << 12) |
              ((uint32_t)(b3 & 0x3F) << 6) | (b4 & 0x3F);
         if (cp < 0x10000 || cp > 0x10FFFF) return false;
      } else {
         return false;
      }
      if (cp < 0x80) {
         if (o + 1 >= cap) return false;
         out[o++] = (char)cp;
      } else if (cp < 0x800) {
         if (o + 2 >= cap) return false;
         out[o++] = (char)(0xC0 | (cp >> 6));
         out[o++] = (char)(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
         if (o + 3 >= cap) return false;
         out[o++] = (char)(0xE0 | (cp >> 12));
         out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
         out[o++] = (char)(0x80 | (cp & 0x3F));
      } else {
         if (o + 4 >= cap) return false;
         out[o++] = (char)(0xF0 | (cp >> 18));
         out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
         out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
         out[o++] = (char)(0x80 | (cp & 0x3F));
      }
   }
   if (o == 0 || o >= cap) return false;
   out[o] = 0;
   return true;
}

/* ---------------- handle lifetime ---------------- */

static raf_handle *handle_new(int fd, int writable) {
   raf_handle *h = calloc(1, sizeof *h);
   if (!h) {
      close(fd);
      return NULL;
   }
   if (pthread_mutex_init(&h->lock, NULL) != 0) {
      close(fd);
      free(h);
      return NULL;
   }
   h->fd = fd;
   h->writable = writable;
   h->refs = 1;
   return h;
}

static void handle_addref(raf_handle *h) {
   if (h) __atomic_add_fetch(&h->refs, 1, __ATOMIC_RELAXED);
}

static void handle_release(raf_handle *h) {
   int fd;
   if (!h) return;
   if (__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
   pthread_mutex_lock(&h->lock);
   fd = h->fd;
   h->fd = -1;
   h->closed = 1;
   pthread_mutex_unlock(&h->lock);
   if (fd >= 0) close(fd);
   pthread_mutex_destroy(&h->lock);
   free(h);
}

/* ---------------- JVM-wide FileLock table ----------------
 * POSIX fcntl record locks are owned by the process, so a second request
 * through the same process would silently merge instead of failing.  Java
 * requires OverlappingFileLockException for that case; this table mirrors
 * FileLockTable.  Entries are also kept while a blocking lock is pending. */
typedef struct raf_file_lock {
   struct raf_file_lock *next;
   raf_handle *h;          /* not owned by the table */
   int64_t position;
   int64_t size;
   int shared;
   int valid;              /* 1: fcntl lock held, 0: pending, -1: invalid */
   dev_t dev;
   ino_t ino;
} raf_file_lock;

static pthread_mutex_t lock_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static raf_file_lock *lock_table;

/* Caller holds h->lock.  Closing the descriptor releases every fcntl lock
 * this process holds on that file, so every matching Java FileLock becomes
 * invalid too. */
static void lock_table_invalidate(dev_t dev, ino_t ino) {
   raf_file_lock **pp;
   pthread_mutex_lock(&lock_table_mutex);
   pp = &lock_table;
   while (*pp) {
      raf_file_lock *p = *pp;
      if (p->dev == dev && p->ino == ino) {
         *pp = p->next;
         p->next = NULL;
         p->valid = -1;
         continue;
      }
      pp = &p->next;
   }
   pthread_mutex_unlock(&lock_table_mutex);
}

static raf_handle *payload_of(struct dvm *v, dvm_ref ref, void (*tag)(void *)) {
   struct dvm_object *o;
   if (!ref) return NULL;
   o = dvm__obj(v, ref);
   if (!o || !o->data || !o->data_destroy) return NULL;
   return o->data_destroy == tag ? (raf_handle *)o->data : NULL;
}

static raf_handle *raf_of(struct dvm *v, dvm_ref ref) {
   return payload_of(v, ref, raf_free);
}

int random_access_file_fd_of(struct dvm *v, dvm_ref ref) {
   raf_handle *h = payload_of(v, ref, fd_free);
   int fd;
   if (!h) h = payload_of(v, ref, channel_free);
   if (!h) return -1;
   pthread_mutex_lock(&h->lock);
   fd = h->closed ? -1 : h->fd;
   pthread_mutex_unlock(&h->lock);
   return fd;
}

static bool begin_op(struct dvm *v, raf_handle *h, bool writing) {
   pthread_mutex_lock(&h->lock);
   if (h->closed || h->fd < 0) {
      pthread_mutex_unlock(&h->lock);
      return io_error(v, "java/io/IOException", "Stream Closed", 0);
   }
   if (writing && !h->writable) {
      pthread_mutex_unlock(&h->lock);
      return io_error(v, "java/io/IOException", "Bad file descriptor", EBADF);
   }
   return true;
}

static bool close_handle(struct dvm *v, raf_handle *h) {
   int err = 0;
   dev_t dev = 0;
   ino_t ino = 0;
   int have_id = 0;
   pthread_mutex_lock(&h->lock);
   if (!h->closed && h->fd >= 0) {
      int fd = h->fd;
      struct stat st;
      if (fstat(fd, &st) == 0) {
         dev = st.st_dev;
         ino = st.st_ino;
         have_id = 1;
      }
      h->fd = -1;
      h->closed = 1;
      if (close(fd) != 0 && errno != EBADF) err = errno;
      if (have_id) lock_table_invalidate(dev, ino);
   } else {
      h->fd = -1;
      h->closed = 1;
   }
   pthread_mutex_unlock(&h->lock);
   if (err) return io_error(v, "java/io/IOException", "close", err);
   return true;
}

/* ---------------- descriptor I/O, caller holds h->lock ---------------- */

static ssize_t read_some_locked(raf_handle *h, void *buf, size_t n) {
   ssize_t r;
   do {
      r = pread(h->fd, buf, n, (off_t)h->pos);
   } while (r < 0 && errno == EINTR);
   if (r > 0) h->pos += (int64_t)r;
   return r;
}

static int write_all_locked(raf_handle *h, const void *buf, size_t n) {
   const unsigned char *p = (const unsigned char *)buf;
   while (n) {
      ssize_t w;
      do {
         w = pwrite(h->fd, p, n, (off_t)h->pos);
      } while (w < 0 && errno == EINTR);
      if (w <= 0) return w < 0 ? errno : EIO;
      h->pos += (int64_t)w;
      p += w;
      n -= (size_t)w;
   }
   return 0;
}

/* Reads exactly n bytes.  On failure the exception is raised and the lock is
 * released; on success the lock is still held. */
static bool read_exact_locked(struct dvm *v, raf_handle *h, void *buf, size_t n) {
   size_t got = 0;
   while (got < n) {
      ssize_t r = read_some_locked(h, (unsigned char *)buf + got, n - got);
      if (r < 0) {
         int e = errno;
         pthread_mutex_unlock(&h->lock);
         return io_error(v, "java/io/IOException", "read", e);
      }
      if (r == 0) {
         pthread_mutex_unlock(&h->lock);
         dvm__throw(v, "java/io/EOFException", "EOF reached while reading");
         return false;
      }
      got += (size_t)r;
   }
   return true;
}

static bool read_into_locked(struct dvm *v, raf_handle *h, void *buf, size_t n,
                             int *result) {
   ssize_t r;
   int e;
   *result = -1;
   if (n == 0) {
      *result = 0;
      return true;
   }
   r = read_some_locked(h, buf, n);
   e = r < 0 ? errno : 0;
   if (r > 0) *result = (int)r;
   if (e) {
      pthread_mutex_unlock(&h->lock);
      return io_error(v, "java/io/IOException", "read", e);
   }
   return true;
}

static int64_t file_length_locked(raf_handle *h, int *err) {
   struct stat st;
   if (fstat(h->fd, &st) != 0) {
      *err = errno;
      return -1;
   }
   *err = 0;
   return (int64_t)st.st_size;
}

/* ---------------- guest path opening ---------------- */

static bool parse_mode(const char *s, size_t n, int *writable, int *flags) {
   if (!s || strlen(s) != n) return false;
   if (!strcmp(s, "r")) {
      *writable = 0;
      *flags = O_RDONLY;
      return true;
   }
   if (!strcmp(s, "rw")) {
      *writable = 1;
      *flags = O_RDWR | O_CREAT;
      return true;
   }
   if (!strcmp(s, "rws")) {
      *writable = 1;
      *flags = O_RDWR | O_CREAT | O_SYNC;
      return true;
   }
   if (!strcmp(s, "rwd")) {
      *writable = 1;
#ifdef O_DSYNC
      *flags = O_RDWR | O_CREAT | O_DSYNC;
#else
      *flags = O_RDWR | O_CREAT | O_SYNC;
#endif
      return true;
   }
   return false;
}

/* Walk from private_base with openat(O_NOFOLLOW|O_DIRECTORY) so a symlink in
 * any component is refused atomically, then open the leaf with O_NOFOLLOW. */
static int open_guest(const char *path, int flags, mode_t mode, int *errp) {
   char work[PATH_MAX];
   char *rel;
   int dir;
   size_t blen;
   *errp = 0;
   if (!safe_path(path) || !allowed_guest(path)) {
      *errp = EACCES;
      return -1;
   }
   if (strlen(path) >= sizeof work) {
      *errp = ENAMETOOLONG;
      return -1;
   }
   memcpy(work, path, strlen(path) + 1);
   blen = strlen(private_base);
   rel = work + blen;
   while (*rel == '/') rel++;
   dir = open(private_base, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
   if (dir < 0) {
      *errp = errno;
      return -1;
   }
   for (;;) {
      char *slash = strchr(rel, '/');
      int last = slash == NULL;
      if (slash) *slash = 0;
      if (!*rel) {
         close(dir);
         *errp = EINVAL;
         return -1;
      }
      if (!last) {
         int next = openat(dir, rel,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
         if (next < 0) {
            *errp = errno;
            close(dir);
            return -1;
         }
         close(dir);
         dir = next;
         rel = slash + 1;
      } else {
         struct stat st;
         int fd;
         if (fstatat(dir, rel, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISLNK(st.st_mode)) {
               close(dir);
               *errp = ELOOP;
               return -1;
            }
            if (!S_ISREG(st.st_mode)) {
               close(dir);
               *errp = EISDIR;
               return -1;
            }
         } else if (errno != ENOENT) {
            *errp = errno;
            close(dir);
            return -1;
         }
         fd = openat(dir, rel, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                     mode);
         if (fd < 0) {
            *errp = errno;
            close(dir);
            return -1;
         }
         close(dir);
         if (fstat(fd, &st) != 0) {
            *errp = errno;
            close(fd);
            return -1;
         }
         if (!S_ISREG(st.st_mode)) {
            close(fd);
            *errp = EISDIR;
            return -1;
         }
         {
            int fl = fcntl(fd, F_GETFL);
            if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
         }
         return fd;
      }
   }
}

bool random_access_file_configure(const char *sdk_dir) {
   char path[PATH_MAX];
   char *slash;
   struct stat st;
   if (!sdk_dir || !realpath(sdk_dir, path)) return false;
   slash = strrchr(path, '/');
   if (!slash || slash == path) return false;
   *slash = 0;
   if (!path[0]) return false;
   if (snprintf(private_base, sizeof private_base, "%s", path) >=
       (int)sizeof private_base) return false;
   if (snprintf(root, sizeof root, "%s/guest-files", private_base) >=
       (int)sizeof root) return false;
   if (snprintf(cache_root, sizeof cache_root, "%s/guest-cache", private_base) >=
       (int)sizeof cache_root) return false;
   if (mkdir(root, 0700) && access(root, F_OK)) return false;
   if (mkdir(cache_root, 0700) && access(cache_root, F_OK)) return false;
   if (lstat(private_base, &st) || !S_ISDIR(st.st_mode) ||
       S_ISLNK(st.st_mode)) return false;
   if (lstat(root, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
      return false;
   if (lstat(cache_root, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
      return false;
   return true;
}

static void install_guest_methods(struct dvm *vm);

void random_access_file_vm_start(struct dvm *vm) {
   active = vm;
   install_guest_methods(vm);
}

/* ---------------- construction ---------------- */

static bool init_with_refs(struct dvm *v, dvm_ref self, dvm_ref path_ref,
                           dvm_ref mode_ref) {
   struct dvm_object *o;
   const char *ps, *ms;
   uint32_t pl, ml;
   char path[PATH_MAX];
   int writable = 0, flags = 0, err = 0, fd;
   raf_handle *h;

   if (!self) return false;
   o = dvm__obj(v, self);
   if (!o) return false;
   if (o->data || o->data_destroy) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "RandomAccessFile already initialized");
      return true;
   }
   if (!path_ref) {
      dvm__throw(v, "java/lang/NullPointerException", "path");
      return true;
   }
   if (!mode_ref) {
      dvm__throw(v, "java/lang/NullPointerException", "mode");
      return true;
   }
   ps = dvm_string_utf8(v, path_ref);
   pl = dvm_string_utf8_length(v, path_ref);
   if (!ps) {
      dvm__throw(v, "java/lang/IllegalArgumentException", "path is not a String");
      return true;
   }
   if (!path_from_string(ps, pl, path, sizeof path)) {
      return io_error(v, "java/io/IOException", "invalid guest path", 0);
   }
   if (!safe_path(path) || !allowed_guest(path)) {
      return io_error(v, "java/io/IOException",
                      "path is outside the guest file sandbox", 0);
   }
   ms = dvm_string_utf8(v, mode_ref);
   ml = dvm_string_utf8_length(v, mode_ref);
   if (!ms) {
      dvm__throw(v, "java/lang/IllegalArgumentException", "mode is not a String");
      return true;
   }
   if (!parse_mode(ms, ml, &writable, &flags)) {
      return throw_msg(v, "java/lang/IllegalArgumentException",
                       "Illegal mode \"%s\"", ms);
   }
   fd = open_guest(path, flags, 0600, &err);
   if (fd < 0) {
      return io_error(v, "java/io/FileNotFoundException", path, err);
   }
   h = handle_new(fd, writable);
   if (!h) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "RandomAccessFile");
      return true;
   }
   o->data = h;
   o->data_destroy = raf_free;
   return true;
}

static bool init_with_file(struct dvm *v, dvm_ref self, dvm_ref file_ref,
                           dvm_ref mode_ref) {
   union dvm_value out = {0};
   bool handled;
   if (!file_ref) {
      dvm__throw(v, "java/lang/NullPointerException", "file");
      return true;
   }
   if (!mode_ref) {
      dvm__throw(v, "java/lang/NullPointerException", "mode");
      return true;
   }
   if (!file_runtime_call) {
      return io_error(v, "java/io/IOException", "File runtime is unavailable", 0);
   }
   handled = file_runtime_call(v, "java/io/File", "getPath",
                               "()Ljava/lang/String;", file_ref, NULL, 0, &out);
   if (!handled) {
      if (dvm_exception(v)) return true;
      return io_error(v, "java/io/IOException", "File.getPath is unavailable", 0);
   }
   if (dvm_exception(v)) return true;
   if (!out.l) return io_error(v, "java/io/IOException", "File has no path", 0);
   return init_with_refs(v, self, out.l, mode_ref);
}

/* ---------------- primitive helpers ---------------- */

static bool read_prim(struct dvm *v, raf_handle *h, void *buf, size_t n) {
   if (!begin_op(v, h, false)) return false;
   if (!read_exact_locked(v, h, buf, n)) return false;
   pthread_mutex_unlock(&h->lock);
   return true;
}

static bool write_prim(struct dvm *v, raf_handle *h, const void *buf, size_t n) {
   int err;
   if (!begin_op(v, h, true)) return false;
   err = write_all_locked(h, buf, n);
   pthread_mutex_unlock(&h->lock);
   if (err) return io_error(v, "java/io/IOException", "write", err);
   return true;
}

static unsigned char *byte_array(struct dvm *v, dvm_ref ref, uint32_t *alen) {
   struct dvm_object *o;
   if (!ref) {
      dvm__throw(v, "java/lang/NullPointerException", "b");
      return NULL;
   }
   o = dvm__obj(v, ref);
   if (!o || o->kind != DVM_OBJ_ARRAY || o->elem_kind != 'B') {
      dvm__throw(v, "java/lang/IllegalArgumentException",
                 "not a byte array");
      return NULL;
   }
   *alen = o->length;
   return (unsigned char *)o->data;
}

static bool check_range(struct dvm *v, int off, int len, uint32_t alen) {
   if (off < 0 || len < 0 ||
       (uint64_t)(uint32_t)off + (uint64_t)(uint32_t)len > (uint64_t)alen) {
      dvm__throw(v, "java/lang/IndexOutOfBoundsException",
                 "off=%d len=%d length=%u", off, len, alen);
      return false;
   }
   return true;
}

static int read_one(struct dvm *v, raf_handle *h, bool *ok) {
   unsigned char b = 0;
   int result = -1;
   *ok = false;
   if (!begin_op(v, h, false)) return 0;
   if (!read_into_locked(v, h, &b, 1, &result)) return 0;
   pthread_mutex_unlock(&h->lock);
   *ok = true;
   return result < 0 ? -1 : (int)b;
}

static bool read_byte_value(struct dvm *v, raf_handle *h, unsigned char *out) {
   unsigned char b;
   if (!read_prim(v, h, &b, 1)) return false;
   *out = b;
   return true;
}

/* Strict modified UTF-8 for DataInput.readUTF: no raw NUL, no 4-byte forms,
 * and lone surrogates are legal because they are stored as 3-byte forms. */
static bool valid_mutf8(const unsigned char *s, size_t n) {
   size_t i = 0;
   while (i < n) {
      unsigned b = s[i++];
      if (b >= 1 && b <= 0x7F) continue;
      if ((b & 0xE0) == 0xC0) {
         unsigned cp;
         if (i >= n || (s[i] & 0xC0) != 0x80) return false;
         cp = ((uint32_t)(b & 0x1F) << 6) | (s[i++] & 0x3F);
         if (cp < 0x80 && cp != 0) return false;
         continue;
      }
      if ((b & 0xF0) == 0xE0) {
         uint32_t cp;
         if (i + 1 >= n || (s[i] & 0xC0) != 0x80 ||
             (s[i + 1] & 0xC0) != 0x80) return false;
         cp = ((uint32_t)(b & 0x0F) << 12) |
              ((uint32_t)(s[i] & 0x3F) << 6) | (s[i + 1] & 0x3F);
         i += 2;
         if (cp < 0x800) return false;
         continue;
      }
      return false;
   }
   return true;
}

static bool emit_unit(unsigned char *out, size_t cap, size_t *o, uint32_t u) {
   if (u == 0) {
      if (*o + 2 > cap) return false;
      out[(*o)++] = 0xC0;
      out[(*o)++] = 0x80;
   } else if (u < 0x80) {
      if (*o + 1 > cap) return false;
      out[(*o)++] = (unsigned char)u;
   } else if (u < 0x800) {
      if (*o + 2 > cap) return false;
      out[(*o)++] = (unsigned char)(0xC0 | (u >> 6));
      out[(*o)++] = (unsigned char)(0x80 | (u & 0x3F));
   } else {
      if (!(u >= 0xD800 && u <= 0xDFFF) && !(u >= 0xE000 && u <= 0xFFFF))
         return false;
      if (*o + 3 > cap) return false;
      out[(*o)++] = (unsigned char)(0xE0 | (u >> 12));
      out[(*o)++] = (unsigned char)(0x80 | ((u >> 6) & 0x3F));
      out[(*o)++] = (unsigned char)(0x80 | (u & 0x3F));
   }
   return true;
}

/* Encode a DVM internal string as modified UTF-8.  Returns 1 on success,
 * 0 on malformed input, -1 when the requested capacity is exceeded. */
static int encode_mutf8(const unsigned char *src, size_t n, unsigned char *out,
                        size_t cap, size_t *outn) {
   size_t i = 0, o = 0;
   while (i < n) {
      unsigned b = src[i++];
      uint32_t cp;
      if (b == 0) {
         cp = 0;
      } else if (b < 0x80) {
         cp = b;
      } else if ((b & 0xE0) == 0xC0) {
         if (i >= n || (src[i] & 0xC0) != 0x80) return 0;
         cp = ((uint32_t)(b & 0x1F) << 6) | (src[i++] & 0x3F);
         if (cp < 0x80 && cp != 0) return 0;
      } else if ((b & 0xF0) == 0xE0) {
         if (i + 1 >= n || (src[i] & 0xC0) != 0x80 ||
             (src[i + 1] & 0xC0) != 0x80) return 0;
         cp = ((uint32_t)(b & 0x0F) << 12) |
              ((uint32_t)(src[i] & 0x3F) << 6) | (src[i + 1] & 0x3F);
         i += 2;
         if (cp < 0x800) return 0;
      } else if ((b & 0xF8) == 0xF0) {
         if (i + 2 >= n || (src[i] & 0xC0) != 0x80 ||
             (src[i + 1] & 0xC0) != 0x80 ||
             (src[i + 2] & 0xC0) != 0x80) return 0;
         cp = ((uint32_t)(b & 0x07) << 18) |
              ((uint32_t)(src[i] & 0x3F) << 12) |
              ((uint32_t)(src[i + 1] & 0x3F) << 6) | (src[i + 2] & 0x3F);
         i += 3;
         if (cp < 0x10000 || cp > 0x10FFFF) return 0;
      } else {
         return 0;
      }
      if (cp >= 0x10000) {
         cp -= 0x10000;
         if (!emit_unit(out, cap, &o, 0xD800 + (cp >> 10))) return -1;
         if (!emit_unit(out, cap, &o, 0xDC00 + (cp & 0x3FF))) return -1;
      } else if (!emit_unit(out, cap, &o, cp)) {
         return -1;
      }
   }
   *outn = o;
   return 1;
}

/* ---------------- RandomAccessFile methods ---------------- */

static bool raf_read_utf(struct dvm *v, raf_handle *h, union dvm_value *out) {
   unsigned char lenb[2];
   unsigned char *buf = NULL;
   size_t n;
   dvm_ref s;
   if (!begin_op(v, h, false)) return true;
   if (!read_exact_locked(v, h, lenb, 2)) return true;
   n = ((size_t)lenb[0] << 8) | lenb[1];
   if (n) {
      buf = malloc(n);
      if (!buf) {
         pthread_mutex_unlock(&h->lock);
         dvm__throw(v, "java/lang/OutOfMemoryError", "readUTF");
         return true;
      }
      if (!read_exact_locked(v, h, buf, n)) {
         free(buf);
         return true;
      }
   }
   pthread_mutex_unlock(&h->lock);
   if (n && !valid_mutf8(buf, n)) {
      free(buf);
      dvm__throw(v, "java/io/UTFDataFormatException",
                 "malformed input around byte 0");
      return true;
   }
   s = dvm_new_string_n(v, (const char *)buf, n);
   free(buf);
   if (!s) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "readUTF");
      return true;
   }
   out->l = s;
   return true;
}

static bool raf_write_utf(struct dvm *v, raf_handle *h, dvm_ref sref) {
   const char *s;
   uint32_t sl;
   unsigned char *buf;
   size_t n = 0;
   int enc;
   bool ok;
   if (!sref) {
      dvm__throw(v, "java/lang/NullPointerException", "s");
      return true;
   }
   s = dvm_string_utf8(v, sref);
   sl = dvm_string_utf8_length(v, sref);
   if (!s) {
      dvm__throw(v, "java/lang/IllegalArgumentException", "s is not a String");
      return true;
   }
   if ((size_t)sl > 65535) {
      return throw_msg(v, "java/io/UTFDataFormatException",
                       "encoded string too long: %u bytes", sl);
   }
   buf = malloc((size_t)sl * 3 + 3);
   if (!buf) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "writeUTF");
      return true;
   }
   enc = encode_mutf8((const unsigned char *)s, sl, buf + 2,
                      (size_t)sl * 3 + 1, &n);
   if (enc == 0) {
      free(buf);
      dvm__throw(v, "java/io/UTFDataFormatException", "malformed string");
      return true;
   }
   if (enc < 0 || n > 65535) {
      free(buf);
      return throw_msg(v, "java/io/UTFDataFormatException",
                       "encoded string too long: %u bytes", sl);
   }
   buf[0] = (unsigned char)((n >> 8) & 0xFF);
   buf[1] = (unsigned char)(n & 0xFF);
   ok = write_prim(v, h, buf, n + 2);
   free(buf);
   return ok;
}

static bool raf_read_line(struct dvm *v, raf_handle *h, union dvm_value *out) {
   unsigned char *buf = NULL;
   size_t len = 0, cap = 0;
   bool any = false;
   int err = 0;
   dvm_ref s;
   out->l = 0;
   if (!begin_op(v, h, false)) return true;
   for (;;) {
      unsigned char b;
      ssize_t r = read_some_locked(h, &b, 1);
      if (r < 0) {
         err = errno;
         break;
      }
      if (r == 0) break;
      any = true;
      if (b == '\n') break;
      if (b == '\r') {
         unsigned char c;
         ssize_t r2 = read_some_locked(h, &c, 1);
         if (r2 < 0) {
            err = errno;
            break;
         }
         if (r2 == 1 && c != '\n') h->pos -= 1;
         break;
      }
      {
         size_t need = (b == 0 || b >= 0x80) ? 2 : 1;
         if (len + need > cap) {
            size_t ncap = cap ? cap * 2 : 32;
            unsigned char *grown;
            while (ncap < len + need) ncap *= 2;
            grown = realloc(buf, ncap);
            if (!grown) {
               err = ENOMEM;
               break;
            }
            buf = grown;
            cap = ncap;
         }
         if (b == 0) {
            buf[len++] = 0xC0;
            buf[len++] = 0x80;
         } else if (b < 0x80) {
            buf[len++] = b;
         } else {
            buf[len++] = (unsigned char)(0xC0 | (b >> 6));
            buf[len++] = (unsigned char)(0x80 | (b & 0x3F));
         }
      }
   }
   pthread_mutex_unlock(&h->lock);
   if (err == ENOMEM) {
      free(buf);
      dvm__throw(v, "java/lang/OutOfMemoryError", "readLine");
      return true;
   }
   if (err) {
      free(buf);
      return io_error(v, "java/io/IOException", "readLine", err);
   }
   if (!any) {
      free(buf);
      return true; /* null at EOF */
   }
   s = dvm_new_string_n(v, (const char *)buf, len);
   free(buf);
   if (!s) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "readLine");
      return true;
   }
   out->l = s;
   return true;
}

static bool make_wrapper(struct dvm *v, raf_handle *h, const char *cls_name,
                         void (*tag)(void *), union dvm_value *out) {
   dvm_ref r = dvm_new_object(v, dvm_find_class_or_external(v, cls_name));
   struct dvm_object *o;
   if (!r) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "%s", cls_name);
      return true;
   }
   o = dvm__obj(v, r);
   if (!o) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "%s", cls_name);
      return true;
   }
   handle_addref(h);
   o->data = h;
   o->data_destroy = tag;
   out->l = r;
   return true;
}

/* ---------------- java.io.File equals()/hashCode() ----------------
 * KVs.Companion.of() keeps a static HashMap keyed by java.io.File.  Every
 * BLKV.getBLSharedPreferences() call creates a new File, so the second lookup
 * compares two keys.  java_basics_equal() only consults the class method
 * table; java/io/File is a synthesized external class with no methods, which
 * used to raise UnsupportedOperationException from that lookup. */
static bool file_path_of(struct dvm *v, dvm_ref ref, const char **path) {
   struct dvm_object *o = ref ? dvm__obj(v, ref) : NULL;
   struct dvm_class *k;
   if (!o || !o->cls || !o->data) return false;
   for (k = o->cls; k; k = k->super)
      if (k->name && !strcmp(k->name, "java/io/File")) {
         *path = (const char *)o->data;
         return true;
      }
   return false;
}

/* java.lang.String.hashCode() over UTF-16 code units.  Guest paths are
 * modified UTF-8 (possibly CESU-8 surrogates); plain 4-byte UTF-8 is folded
 * into a surrogate pair so the result matches a real JVM. */
static int32_t file_path_hash(const char *str) {
   const unsigned char *s = (const unsigned char *)str;
   uint32_t h = 0;
   while (*s) {
      uint32_t cp;
      int n;
      if (s[0] < 0x80) {
         cp = s[0];
         n = 1;
      } else if ((s[0] & 0xE0) == 0xC0 && s[1]) {
         cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
         n = 2;
      } else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
         cp = ((uint32_t)(s[0] & 0x0F) << 12) |
              ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
         n = 3;
      } else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
         cp = ((uint32_t)(s[0] & 0x07) << 18) |
              ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
         n = 4;
      } else {
         cp = s[0]; /* malformed: hash the byte, keep the value stable */
         n = 1;
      }
      s += n;
      if (cp >= 0x10000) {
         cp -= 0x10000;
         h = 31 * h + (0xD800 + (cp >> 10));
         h = 31 * h + (0xDC00 + (cp & 0x3FF));
      } else {
         h = 31 * h + cp;
      }
   }
   return (int32_t)h;
}

static bool file_equals_builtin(struct dvm *v, dvm_ref self,
                                const union dvm_value *a, int n,
                                union dvm_value *out) {
   const char *pa = NULL, *pb = NULL;
   out->i = 0;
   if (n < 1 || !a) return true;
   if (file_path_of(v, self, &pa) && file_path_of(v, a[0].l, &pb))
      out->i = strcmp(pa, pb) == 0;
   return true;
}

static bool file_hashcode_builtin(struct dvm *v, dvm_ref self,
                                  const union dvm_value *a, int n,
                                  union dvm_value *out) {
   const char *p = NULL;
   (void)a;
   (void)n;
   out->i = file_path_of(v, self, &p) ? file_path_hash(p) : 0;
   return true;
}

/* ---------------- FileLock payload and builtins ---------------- */

static void file_lock_free(void *q);

static bool file_lock_bad_payload(struct dvm *v) {
   dvm__throw(v, "java/lang/IllegalStateException", "invalid FileLock");
   return true;
}

static raf_file_lock *file_lock_of(struct dvm *v, dvm_ref self) {
   return (raf_file_lock *)payload_of(v, self, file_lock_free);
}

static bool file_lock_position_builtin(struct dvm *v, dvm_ref self,
                                       const union dvm_value *a, int n,
                                       union dvm_value *out) {
   raf_file_lock *p = file_lock_of(v, self);
   (void)a;
   (void)n;
   if (!p) return file_lock_bad_payload(v);
   out->j = p->position;
   return true;
}

static bool file_lock_size_builtin(struct dvm *v, dvm_ref self,
                                   const union dvm_value *a, int n,
                                   union dvm_value *out) {
   raf_file_lock *p = file_lock_of(v, self);
   (void)a;
   (void)n;
   if (!p) return file_lock_bad_payload(v);
   out->j = p->size;
   return true;
}

static bool file_lock_shared_builtin(struct dvm *v, dvm_ref self,
                                     const union dvm_value *a, int n,
                                     union dvm_value *out) {
   raf_file_lock *p = file_lock_of(v, self);
   (void)a;
   (void)n;
   if (!p) return file_lock_bad_payload(v);
   out->i = p->shared;
   return true;
}

static bool file_lock_valid_builtin(struct dvm *v, dvm_ref self,
                                    const union dvm_value *a, int n,
                                    union dvm_value *out) {
   raf_file_lock *p = file_lock_of(v, self);
   (void)a;
   (void)n;
   if (!p) return file_lock_bad_payload(v);
   pthread_mutex_lock(&lock_table_mutex);
   out->i = p->valid == 1;
   pthread_mutex_unlock(&lock_table_mutex);
   return true;
}

/* Detach from the JVM table and, while the descriptor is still open, release
 * exactly this region with F_UNLCK.  Caller must not hold h->lock/table. */
static int file_lock_detach(raf_file_lock *p) {
   int err = 0;
   pthread_mutex_lock(&p->h->lock);
   if (p->valid == 1 && !p->h->closed && p->h->fd >= 0) {
      struct flock fl;
      memset(&fl, 0, sizeof fl);
      fl.l_type = F_UNLCK;
      fl.l_whence = SEEK_SET;
      fl.l_start = (off_t)p->position;
      fl.l_len = (off_t)p->size;
      if (fcntl(p->h->fd, F_SETLK, &fl) != 0) err = errno;
   }
   pthread_mutex_lock(&lock_table_mutex);
   {
      raf_file_lock **pp = &lock_table;
      while (*pp && *pp != p) pp = &(*pp)->next;
      if (*pp) {
         *pp = p->next;
         p->next = NULL;
      }
   }
   p->valid = -1;
   pthread_mutex_unlock(&lock_table_mutex);
   pthread_mutex_unlock(&p->h->lock);
   return err;
}

static void file_lock_free(void *q) {
   raf_file_lock *p = (raf_file_lock *)q;
   if (!p) return;
   if (p->h) {
      (void)file_lock_detach(p);
      handle_release(p->h);
   }
   free(p);
}

static bool file_lock_release_builtin(struct dvm *v, dvm_ref self,
                                      const union dvm_value *a, int n,
                                      union dvm_value *out) {
   raf_file_lock *p = file_lock_of(v, self);
   int err;
   (void)a;
   (void)n;
   if (!p) return file_lock_bad_payload(v);
   err = file_lock_detach(p);
   if (err) return io_error(v, "java/io/IOException", "release", err);
   return true;
}

/* ---------------- FileChannel.lock()/tryLock() ---------------- */

static bool lock_ranges_overlap(int64_t p1, int64_t s1, int64_t p2, int64_t s2) {
   __int128 e1, e2;
   if (s1 == 0 || s2 == 0) return false;
   e1 = (__int128)p1 + (__int128)s1;
   e2 = (__int128)p2 + (__int128)s2;
   return (__int128)p1 < e2 && (__int128)p2 < e1;
}

/* Caller holds h->lock and h->fd is known open. */
static int file_lock_fcntl(raf_handle *h, int64_t position, int64_t size,
                           int shared, int blocking) {
   struct flock fl;
   int cmd = blocking ? F_SETLKW : F_SETLK;
   memset(&fl, 0, sizeof fl);
   fl.l_type = shared ? F_RDLCK : F_WRLCK;
   fl.l_whence = SEEK_SET;
   fl.l_start = (off_t)position;
   fl.l_len = (off_t)size;
   for (;;) {
      if (fcntl(h->fd, cmd, &fl) == 0) return 0;
      if (errno == EINTR && blocking) continue;
      return errno;
   }
}

static bool file_channel_lock(struct dvm *v, raf_handle *h, int64_t position,
                              int64_t size, int shared, int blocking,
                              union dvm_value *out) {
   raf_file_lock *p;
   struct stat st;
   struct dvm_class *cls;
   struct dvm_object *o;
   dvm_ref ref;
   int err;

   out->l = 0;
   if (position < 0 || size < 0 ||
       (__int128)position + (__int128)size > (__int128)INT64_MAX) {
      dvm__throw(v, "java/lang/IllegalArgumentException", "lock range");
      return true;
   }

   pthread_mutex_lock(&h->lock);
   if (h->closed || h->fd < 0) {
      pthread_mutex_unlock(&h->lock);
      dvm__throw(v, "java/nio/channels/ClosedChannelException",
                 "channel closed");
      return true;
   }
   if (!shared && !h->writable) {
      pthread_mutex_unlock(&h->lock);
      dvm__throw(v, "java/nio/channels/NonWritableChannelException",
                 "channel is read-only");
      return true;
   }
   if (fstat(h->fd, &st) != 0) {
      err = errno;
      pthread_mutex_unlock(&h->lock);
      return io_error(v, "java/io/IOException", "lock", err);
   }
   p = calloc(1, sizeof *p);
   if (!p) {
      pthread_mutex_unlock(&h->lock);
      dvm__throw(v, "java/lang/OutOfMemoryError", "FileLock");
      return true;
   }
   p->h = h;
   p->position = position;
   p->size = size;
   p->shared = shared;
   p->valid = 0;
   p->dev = st.st_dev;
   p->ino = st.st_ino;

   pthread_mutex_lock(&lock_table_mutex);
   {
      raf_file_lock *q;
      for (q = lock_table; q; q = q->next) {
         if (q->dev == p->dev && q->ino == p->ino &&
             lock_ranges_overlap(p->position, p->size, q->position,
                                 q->size)) {
            pthread_mutex_unlock(&lock_table_mutex);
            pthread_mutex_unlock(&h->lock);
            free(p);
            dvm__throw(v, "java/nio/channels/OverlappingFileLockException",
                       "overlapping file lock");
            return true;
         }
      }
   }
   p->next = lock_table;
   lock_table = p;
   pthread_mutex_unlock(&lock_table_mutex);

   handle_addref(h);
   err = file_lock_fcntl(h, position, size, shared, blocking);

   pthread_mutex_lock(&lock_table_mutex);
   if (!err) {
      p->valid = 1;
   } else {
      raf_file_lock **pp = &lock_table;
      while (*pp && *pp != p) pp = &(*pp)->next;
      if (*pp) {
         *pp = p->next;
         p->next = NULL;
      }
   }
   pthread_mutex_unlock(&lock_table_mutex);
   pthread_mutex_unlock(&h->lock);

   if (err) {
      handle_release(h);
      free(p);
      if (!blocking && (err == EACCES || err == EAGAIN)) return true;
      return io_error(v, "java/io/IOException", "lock", err);
   }

   cls = dvm_find_class_or_external(v, "java/nio/channels/FileLock");
   ref = cls ? dvm_new_object(v, cls) : 0;
   o = ref ? dvm__obj(v, ref) : NULL;
   if (!o) {
      if (p->h) {
         (void)file_lock_detach(p);
         handle_release(p->h);
      }
      free(p);
      dvm__throw(v, "java/lang/OutOfMemoryError", "FileLock object");
      return true;
   }
   o->data = p;
   o->data_destroy = file_lock_free;
   out->l = ref;
   return true;
}

/* ---------------- builtin method installation ---------------- */

static bool guest_install_method(struct dvm *v, struct dvm_class *cls,
                                 const char *name, const char *sig,
                                 dvm_builtin_fn fn, char ret, int args,
                                 int slots) {
   struct dvm_method *m;
   int i;
   if (!cls) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "guest class");
      return false;
   }
   for (i = 0; i < cls->nmethods; i++) {
      m = &cls->methods[i];
      if (m->name && m->sig && !strcmp(m->name, name) &&
          !strcmp(m->sig, sig)) {
         m->builtin = fn;
         m->ret_kind = ret;
         m->arg_count = args;
         m->arg_slots = slots;
         return true;
      }
   }
   m = realloc(cls->methods, ((size_t)cls->nmethods + 1) * sizeof *m);
   if (!m) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "method table");
      return false;
   }
   cls->methods = m;
   m = &m[cls->nmethods];
   memset(m, 0, sizeof *m);
   m->cls = cls;
   m->name = name;
   m->sig = strdup(sig);
   m->access = 0x0001; /* public */
   m->builtin = fn;
   m->ret_kind = ret;
   m->arg_count = args;
   m->arg_slots = slots;
   if (!m->sig) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "method name");
      return false;
   }
   cls->nmethods++;
   return true;
}

static void install_guest_methods(struct dvm *v) {
   struct dvm_class *file = dvm_find_class_or_external(v, "java/io/File");
   struct dvm_class *lock =
       dvm_find_class_or_external(v, "java/nio/channels/FileLock");
   if (file) {
      (void)guest_install_method(v, file, "equals", "(Ljava/lang/Object;)Z",
                                 file_equals_builtin, 'Z', 1, 1);
      (void)guest_install_method(v, file, "hashCode", "()I",
                                 file_hashcode_builtin, 'I', 0, 0);
   }
   if (lock) {
      (void)guest_install_method(v, lock, "position", "()J",
                                 file_lock_position_builtin, 'J', 0, 0);
      (void)guest_install_method(v, lock, "size", "()J",
                                 file_lock_size_builtin, 'J', 0, 0);
      (void)guest_install_method(v, lock, "isShared", "()Z",
                                 file_lock_shared_builtin, 'Z', 0, 0);
      (void)guest_install_method(v, lock, "isValid", "()Z",
                                 file_lock_valid_builtin, 'Z', 0, 0);
      (void)guest_install_method(v, lock, "release", "()V",
                                 file_lock_release_builtin, 'V', 0, 0);
      (void)guest_install_method(v, lock, "close", "()V",
                                 file_lock_release_builtin, 'V', 0, 0);
   }
}

/* ---------------- dispatch ---------------- */

bool random_access_file_call(struct dvm *v, const char *cls, const char *m,
                             const char *sig, dvm_ref self,
                             const union dvm_value *a, int n,
                             union dvm_value *out) {
   raf_handle *h;
   if (!v || !cls || !m || !sig || !root[0]) return false;

   if (!strcmp(cls, "java/io/RandomAccessFile")) {
      if (!strcmp(m, "<init>") && !strcmp(sig, "(Ljava/lang/String;Ljava/lang/String;)V")) {
         if (n != 2 || !a) return false;
         return init_with_refs(v, self, a[0].l, a[1].l);
      }
      if (!strcmp(m, "<init>") && !strcmp(sig, "(Ljava/io/File;Ljava/lang/String;)V")) {
         if (n != 2 || !a) return false;
         return init_with_file(v, self, a[0].l, a[1].l);
      }
      h = raf_of(v, self);
      if (!h) {
         dvm__throw(v, "java/io/IOException",
                    "RandomAccessFile is not initialized");
         return true;
      }
      if (!strcmp(m, "read") && !strcmp(sig, "()I")) {
         bool ok;
         int b = read_one(v, h, &ok);
         if (ok) out->i = b;
         return true;
      }
      if (!strcmp(m, "read") && !strcmp(sig, "([B)I")) {
         uint32_t alen;
         unsigned char *p;
         int got = -1;
         if (n != 1 || !a) return false;
         p = byte_array(v, a[0].l, &alen);
         if (!p) return true;
         if (alen > (uint32_t)INT_MAX) {
            dvm__throw(v, "java/lang/IndexOutOfBoundsException",
                       "length=%u", alen);
            return true;
         }
         if (!begin_op(v, h, false)) return true;
         if (!read_into_locked(v, h, p, alen, &got)) return true;
         pthread_mutex_unlock(&h->lock);
         out->i = got;
         return true;
      }
      if (!strcmp(m, "read") && !strcmp(sig, "([BII)I")) {
         uint32_t alen;
         unsigned char *p;
         int got = -1;
         if (n != 3 || !a) return false;
         p = byte_array(v, a[0].l, &alen);
         if (!p) return true;
         if (!check_range(v, a[1].i, a[2].i, alen)) return true;
         if (!begin_op(v, h, false)) return true;
         if (!read_into_locked(v, h, p + a[1].i, (size_t)a[2].i, &got))
            return true;
         pthread_mutex_unlock(&h->lock);
         out->i = got;
         return true;
      }
      if (!strcmp(m, "readFully") && !strcmp(sig, "([B)V")) {
         uint32_t alen;
         unsigned char *p;
         if (n != 1 || !a) return false;
         p = byte_array(v, a[0].l, &alen);
         if (!p) return true;
         if (!begin_op(v, h, false)) return true;
         if (!read_exact_locked(v, h, p, alen)) return true;
         pthread_mutex_unlock(&h->lock);
         return true;
      }
      if (!strcmp(m, "readFully") && !strcmp(sig, "([BII)V")) {
         uint32_t alen;
         unsigned char *p;
         if (n != 3 || !a) return false;
         p = byte_array(v, a[0].l, &alen);
         if (!p) return true;
         if (!check_range(v, a[1].i, a[2].i, alen)) return true;
         if (!begin_op(v, h, false)) return true;
         if (!read_exact_locked(v, h, p + a[1].i, (size_t)a[2].i)) return true;
         pthread_mutex_unlock(&h->lock);
         return true;
      }
      if (!strcmp(m, "skipBytes") && !strcmp(sig, "(I)I")) {
         int64_t cur, np, fl;
         int err = 0;
         if (n != 1 || !a) return false;
         if (!begin_op(v, h, false)) return true;
         cur = h->pos;
         if (a[0].i <= 0) {
            pthread_mutex_unlock(&h->lock);
            out->i = 0;
            return true;
         }
         np = a[0].i > 0 && cur > INT64_MAX - (int64_t)a[0].i
                 ? INT64_MAX
                 : cur + (int64_t)a[0].i;
         fl = file_length_locked(h, &err);
         if (err) {
            pthread_mutex_unlock(&h->lock);
            io_error(v, "java/io/IOException", "skipBytes", err);
            return true;
         }
         if (np > fl) np = fl;
         h->pos = np;
         pthread_mutex_unlock(&h->lock);
         out->i = (int)(np - cur);
         return true;
      }
      if (!strcmp(m, "write") && !strcmp(sig, "(I)V")) {
         unsigned char b;
         if (n != 1 || !a) return false;
         b = (unsigned char)(a[0].i & 0xFF);
         write_prim(v, h, &b, 1);
         return true;
      }
      if (!strcmp(m, "write") && !strcmp(sig, "([B)V")) {
         uint32_t alen;
         unsigned char *p;
         if (n != 1 || !a) return false;
         p = byte_array(v, a[0].l, &alen);
         if (!p) return true;
         write_prim(v, h, p, alen);
         return true;
      }
      if (!strcmp(m, "write") && !strcmp(sig, "([BII)V")) {
         uint32_t alen;
         unsigned char *p;
         if (n != 3 || !a) return false;
         p = byte_array(v, a[0].l, &alen);
         if (!p) return true;
         if (!check_range(v, a[1].i, a[2].i, alen)) return true;
         write_prim(v, h, p + a[1].i, (size_t)a[2].i);
         return true;
      }
      if (!strcmp(m, "seek") && !strcmp(sig, "(J)V")) {
         int64_t np;
         if (n != 1 || !a) return false;
         np = a[0].j;
         if (np < 0) {
            dvm__throw(v, "java/io/IOException", "Negative seek offset");
            return true;
         }
         if (!begin_op(v, h, false)) return true;
         h->pos = np;
         pthread_mutex_unlock(&h->lock);
         return true;
      }
      if (!strcmp(m, "getFilePointer") && !strcmp(sig, "()J")) {
         if (!begin_op(v, h, false)) return true;
         out->j = h->pos;
         pthread_mutex_unlock(&h->lock);
         return true;
      }
      if (!strcmp(m, "length") && !strcmp(sig, "()J")) {
         int64_t fl;
         int err = 0;
         if (!begin_op(v, h, false)) return true;
         fl = file_length_locked(h, &err);
         pthread_mutex_unlock(&h->lock);
         if (err) {
            io_error(v, "java/io/IOException", "length", err);
            return true;
         }
         out->j = fl;
         return true;
      }
      if (!strcmp(m, "setLength") && !strcmp(sig, "(J)V")) {
         int64_t nl;
         int err = 0;
         if (n != 1 || !a) return false;
         nl = a[0].j;
         if (nl < 0) {
            dvm__throw(v, "java/io/IOException", "Negative new length");
            return true;
         }
         if (!begin_op(v, h, true)) return true;
         if (ftruncate(h->fd, (off_t)nl) != 0) err = errno;
         pthread_mutex_unlock(&h->lock);
         if (err) io_error(v, "java/io/IOException", "setLength", err);
         return true;
      }
      if (!strcmp(m, "close") && !strcmp(sig, "()V")) {
         close_handle(v, h);
         return true;
      }
      if (!strcmp(m, "readBoolean") && !strcmp(sig, "()Z")) {
         unsigned char b;
         if (!read_byte_value(v, h, &b)) return true;
         out->i = b != 0;
         return true;
      }
      if (!strcmp(m, "readByte") && !strcmp(sig, "()B")) {
         unsigned char b;
         if (!read_byte_value(v, h, &b)) return true;
         out->i = (int8_t)b;
         return true;
      }
      if (!strcmp(m, "readUnsignedByte") && !strcmp(sig, "()I")) {
         unsigned char b;
         if (!read_byte_value(v, h, &b)) return true;
         out->i = b;
         return true;
      }
      if (!strcmp(m, "readShort") && !strcmp(sig, "()S")) {
         unsigned char b[2];
         if (!read_prim(v, h, b, 2)) return true;
         out->i = (int16_t)(((uint16_t)b[0] << 8) | b[1]);
         return true;
      }
      if (!strcmp(m, "readUnsignedShort") && !strcmp(sig, "()I")) {
         unsigned char b[2];
         if (!read_prim(v, h, b, 2)) return true;
         out->i = ((int)b[0] << 8) | b[1];
         return true;
      }
      if (!strcmp(m, "readChar") && !strcmp(sig, "()C")) {
         unsigned char b[2];
         if (!read_prim(v, h, b, 2)) return true;
         out->i = ((int)b[0] << 8) | b[1];
         return true;
      }
      if (!strcmp(m, "readInt") && !strcmp(sig, "()I")) {
         unsigned char b[4];
         if (!read_prim(v, h, b, 4)) return true;
         out->i = (int32_t)(((uint32_t)b[0] << 24) |
                            ((uint32_t)b[1] << 16) |
                            ((uint32_t)b[2] << 8) | b[3]);
         return true;
      }
      if (!strcmp(m, "readLong") && !strcmp(sig, "()J")) {
         unsigned char b[8];
         uint64_t u;
         if (!read_prim(v, h, b, 8)) return true;
         u = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
             ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
             ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
             ((uint64_t)b[6] << 8) | b[7];
         out->j = (int64_t)u;
         return true;
      }
      if (!strcmp(m, "readFloat") && !strcmp(sig, "()F")) {
         unsigned char b[4];
         uint32_t u;
         float f;
         if (!read_prim(v, h, b, 4)) return true;
         u = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
             ((uint32_t)b[2] << 8) | b[3];
         memcpy(&f, &u, sizeof f);
         out->f = f;
         return true;
      }
      if (!strcmp(m, "readDouble") && !strcmp(sig, "()D")) {
         unsigned char b[8];
         uint64_t u;
         double d;
         if (!read_prim(v, h, b, 8)) return true;
         u = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
             ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
             ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
             ((uint64_t)b[6] << 8) | b[7];
         memcpy(&d, &u, sizeof d);
         out->d = d;
         return true;
      }
      if (!strcmp(m, "readUTF") && !strcmp(sig, "()Ljava/lang/String;"))
         return raf_read_utf(v, h, out);
      if (!strcmp(m, "readLine") && !strcmp(sig, "()Ljava/lang/String;"))
         return raf_read_line(v, h, out);
      if (!strcmp(m, "writeBoolean") && !strcmp(sig, "(Z)V")) {
         unsigned char b;
         if (n != 1 || !a) return false;
         b = a[0].i ? 1 : 0;
         write_prim(v, h, &b, 1);
         return true;
      }
      if ((!strcmp(m, "writeByte") && !strcmp(sig, "(I)V")) ||
          (!strcmp(m, "writeShort") && !strcmp(sig, "(I)V"))) {
         unsigned char b[2];
         if (n != 1 || !a) return false;
         if (!strcmp(m, "writeByte")) {
            b[0] = (unsigned char)(a[0].i & 0xFF);
            write_prim(v, h, b, 1);
         } else {
            b[0] = (unsigned char)((a[0].i >> 8) & 0xFF);
            b[1] = (unsigned char)(a[0].i & 0xFF);
            write_prim(v, h, b, 2);
         }
         return true;
      }
      if (!strcmp(m, "writeChar") && !strcmp(sig, "(I)V")) {
         unsigned char b[2];
         if (n != 1 || !a) return false;
         b[0] = (unsigned char)((a[0].i >> 8) & 0xFF);
         b[1] = (unsigned char)(a[0].i & 0xFF);
         write_prim(v, h, b, 2);
         return true;
      }
      if (!strcmp(m, "writeInt") && !strcmp(sig, "(I)V")) {
         unsigned char b[4];
         uint32_t u;
         if (n != 1 || !a) return false;
         u = (uint32_t)a[0].i;
         b[0] = (unsigned char)(u >> 24);
         b[1] = (unsigned char)(u >> 16);
         b[2] = (unsigned char)(u >> 8);
         b[3] = (unsigned char)u;
         write_prim(v, h, b, 4);
         return true;
      }
      if (!strcmp(m, "writeLong") && !strcmp(sig, "(J)V")) {
         unsigned char b[8];
         uint64_t u;
         if (n != 1 || !a) return false;
         u = (uint64_t)a[0].j;
         b[0] = (unsigned char)(u >> 56);
         b[1] = (unsigned char)(u >> 48);
         b[2] = (unsigned char)(u >> 40);
         b[3] = (unsigned char)(u >> 32);
         b[4] = (unsigned char)(u >> 24);
         b[5] = (unsigned char)(u >> 16);
         b[6] = (unsigned char)(u >> 8);
         b[7] = (unsigned char)u;
         write_prim(v, h, b, 8);
         return true;
      }
      if (!strcmp(m, "writeFloat") && !strcmp(sig, "(F)V")) {
         unsigned char b[4];
         uint32_t u;
         if (n != 1 || !a) return false;
         memcpy(&u, &a[0].f, sizeof u);
         b[0] = (unsigned char)(u >> 24);
         b[1] = (unsigned char)(u >> 16);
         b[2] = (unsigned char)(u >> 8);
         b[3] = (unsigned char)u;
         write_prim(v, h, b, 4);
         return true;
      }
      if (!strcmp(m, "writeDouble") && !strcmp(sig, "(D)V")) {
         unsigned char b[8];
         uint64_t u;
         if (n != 1 || !a) return false;
         memcpy(&u, &a[0].d, sizeof u);
         b[0] = (unsigned char)(u >> 56);
         b[1] = (unsigned char)(u >> 48);
         b[2] = (unsigned char)(u >> 40);
         b[3] = (unsigned char)(u >> 32);
         b[4] = (unsigned char)(u >> 24);
         b[5] = (unsigned char)(u >> 16);
         b[6] = (unsigned char)(u >> 8);
         b[7] = (unsigned char)u;
         write_prim(v, h, b, 8);
         return true;
      }
      if (!strcmp(m, "writeUTF") && !strcmp(sig, "(Ljava/lang/String;)V")) {
         if (n != 1 || !a) return false;
         return raf_write_utf(v, h, a[0].l);
      }
      if (!strcmp(m, "getFD") && !strcmp(sig, "()Ljava/io/FileDescriptor;"))
         return make_wrapper(v, h, "java/io/FileDescriptor", fd_free, out);
      if (!strcmp(m, "getChannel") &&
          !strcmp(sig, "()Ljava/nio/channels/FileChannel;"))
         return make_wrapper(v, h, "java/nio/channels/FileChannel",
                             channel_free, out);
      return false;
   }

   /* Channel.close()/isOpen() are inherited by FileChannel; the DEX may name
    * any of these declaring classes.  Only our own payload is accepted. */
   if (!strcmp(cls, "java/nio/channels/FileChannel") ||
       !strcmp(cls, "java/nio/channels/spi/AbstractInterruptibleChannel") ||
       !strcmp(cls, "java/nio/channels/Channel") ||
       !strcmp(cls, "java/io/Closeable")) {
      raf_handle *ch = payload_of(v, self, channel_free);
      if (!ch) return false;
      if (!strcmp(m, "close") && !strcmp(sig, "()V")) {
         close_handle(v, ch);
         return true;
      }
      if (!strcmp(m, "isOpen") && !strcmp(sig, "()Z")) {
         int open;
         pthread_mutex_lock(&ch->lock);
         open = !ch->closed && ch->fd >= 0;
         pthread_mutex_unlock(&ch->lock);
         out->i = open;
         return true;
      }
      if (!strcmp(m, "lock") && !strcmp(sig, "()Ljava/nio/channels/FileLock;"))
         return file_channel_lock(v, ch, 0, INT64_MAX, 0, 1, out);
      if (!strcmp(m, "tryLock") &&
          !strcmp(sig, "()Ljava/nio/channels/FileLock;"))
         return file_channel_lock(v, ch, 0, INT64_MAX, 0, 0, out);
      if (!strcmp(m, "lock") &&
          !strcmp(sig, "(JJZ)Ljava/nio/channels/FileLock;")) {
         if (n < 3 || !a) return false;
         return file_channel_lock(v, ch, a[0].j, a[1].j, a[2].i != 0, 1, out);
      }
      if (!strcmp(m, "tryLock") &&
          !strcmp(sig, "(JJZ)Ljava/nio/channels/FileLock;")) {
         if (n < 3 || !a) return false;
         return file_channel_lock(v, ch, a[0].j, a[1].j, a[2].i != 0, 0, out);
      }
      return false;
   }

   if (!strcmp(cls, "java/nio/channels/FileLock")) {
      raf_file_lock *fl = (raf_file_lock *)payload_of(v, self, file_lock_free);
      if (!fl) return false;
      if (!strcmp(m, "position") && !strcmp(sig, "()J")) {
         out->j = fl->position;
         return true;
      }
      if (!strcmp(m, "size") && !strcmp(sig, "()J")) {
         out->j = fl->size;
         return true;
      }
      if (!strcmp(m, "isShared") && !strcmp(sig, "()Z")) {
         out->i = fl->shared;
         return true;
      }
      if (!strcmp(m, "isValid") && !strcmp(sig, "()Z")) {
         pthread_mutex_lock(&lock_table_mutex);
         out->i = fl->valid == 1;
         pthread_mutex_unlock(&lock_table_mutex);
         return true;
      }
      if ((!strcmp(m, "release") || !strcmp(m, "close")) &&
          !strcmp(sig, "()V")) {
         int err = file_lock_detach(fl);
         if (err) return io_error(v, "java/io/IOException", "release", err);
         return true;
      }
      return false;
   }

   if (!strcmp(cls, "java/io/FileDescriptor")) {
      raf_handle *fdh = payload_of(v, self, fd_free);
      if (!fdh) return false;
      if (!strcmp(m, "sync") && !strcmp(sig, "()V")) {
         int err = 0;
         if (!begin_op(v, fdh, false)) return true;
         if (fsync(fdh->fd) != 0) err = errno;
         pthread_mutex_unlock(&fdh->lock);
         if (err) io_error(v, "java/io/SyncFailedException", "sync", err);
         return true;
      }
      if (!strcmp(m, "valid") && !strcmp(sig, "()Z")) {
         int valid;
         pthread_mutex_lock(&fdh->lock);
         valid = !fdh->closed && fdh->fd >= 0;
         pthread_mutex_unlock(&fdh->lock);
         out->i = valid;
         return true;
      }
      return false;
   }

   if (!strcmp(cls, "java/io/Closeable") && !strcmp(m, "close") &&
       !strcmp(sig, "()V")) {
      h = raf_of(v, self);
      if (h) {
         close_handle(v, h);
         return true;
      }
   }
   return false;
}

/* ---------------- selftest ---------------- */

#define RAF_CHECK(cond) do { ++checks; if (!(cond)) okay = false; } while (0)

static dvm_ref raf_str(struct dvm *v, const char *s) {
   return dvm_new_string(v, s);
}

static dvm_ref raf_new_object(struct dvm *v) {
   struct dvm_class *c = dvm_find_class_or_external(v, "java/io/RandomAccessFile");
   return c ? dvm_new_object(v, c) : 0;
}

static bool raf_call_ctor2(struct dvm *v, dvm_ref self, dvm_ref path,
                           dvm_ref mode) {
   union dvm_value args[2] = {{0}}, out = {0};
   args[0].l = path;
   args[1].l = mode;
   return random_access_file_call(v, "java/io/RandomAccessFile", "<init>",
                                  "(Ljava/lang/String;Ljava/lang/String;)V",
                                  self, args, 2, &out);
}

static bool raf_expect(struct dvm *v, const char *class_name) {
   dvm_ref e = dvm_exception(v);
   struct dvm_class *c;
   bool match;
   if (!e) return false;
   c = dvm_object_class(v, e);
   match = c && c->name && !strcmp(c->name, class_name);
   dvm_clear_exception(v);
   return match;
}

static dvm_ref raf_bytes(struct dvm *v, const void *p, uint32_t n) {
   dvm_ref a = dvm_new_array(v, 'B', "B", n);
   if (a && n && p) memcpy(dvm_array_data(v, a), p, n);
   return a;
}

static dvm_ref raf_file_object(struct dvm *v, const char *path) {
   struct dvm_class *c = dvm_find_class_or_external(v, "java/io/File");
   dvm_ref r = c ? dvm_new_object(v, c) : 0;
   struct dvm_object *o = r ? dvm__obj(v, r) : NULL;
   if (!o) return 0;
   o->data = strdup(path);
   o->data_destroy = free;
   return o->data ? r : 0;
}

bool random_access_file_selftest(struct dvm *v) {
   char tmpl[PATH_MAX], path[PATH_MAX], linkpath[PATH_MAX];
   char *dir;
   dvm_ref pathstr, modestr, f = 0;
   union dvm_value a[3] = {{0}}, out = {0};
   int checks = 0;
   bool okay = true;

   if (!v || !root[0]) {
      sdk_trace_fprintf(stderr,
                        "SELFTEST SKIP random_access_file configured=0\n");
      return true;
   }
   if (snprintf(tmpl, sizeof tmpl, "%s/raf-test-XXXXXX", root) >=
       (int)sizeof tmpl) return false;
   dir = mkdtemp(tmpl);
   if (!dir) {
      sdk_trace_fprintf(stderr,
                        "SELFTEST FAIL random_access_file mkdtemp errno=%d\n",
                        errno);
      return false;
   }
   if (snprintf(path, sizeof path, "%s/data.bin", dir) >= (int)sizeof path ||
       snprintf(linkpath, sizeof linkpath, "%s/link", dir) >= (int)sizeof linkpath) {
      rmdir(dir);
      return false;
   }

   pathstr = raf_str(v, path);
   modestr = raf_str(v, "rw");
   f = raf_new_object(v);
   RAF_CHECK(pathstr && modestr && f);
   if (pathstr && modestr && f) {
      RAF_CHECK(raf_call_ctor2(v, f, pathstr, modestr));
      RAF_CHECK(!dvm_exception(v));
   }

   /* real bytes through write/read, seek, readFully, length */
   if (f && !dvm_exception(v)) {
      static const unsigned char sample[8] = {'A','B','C','D','E','F','G','H'};
      dvm_ref b = raf_bytes(v, sample, 8);
      a[0].l = b;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "write",
                                        "([B)V", f, a, 1, &out));
      RAF_CHECK(!dvm_exception(v));
      out.j = 0;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "length",
                                        "()J", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.j == 8);
      out.j = 0;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "getFilePointer",
                                        "()J", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.j == 8);
      a[0].j = 2;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "seek",
                                        "(J)V", f, a, 1, &out));
      RAF_CHECK(!dvm_exception(v));
      out.i = 0;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "read",
                                        "()I", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.i == 'C');
      {
         unsigned char want[3] = {'D','E','F'};
         dvm_ref dst = raf_bytes(v, NULL, 3);
         a[0].l = dst;
         a[1].i = 0;
         a[2].i = 3;
         RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                           "readFully", "([BII)V", f, a, 3,
                                           &out));
         RAF_CHECK(!dvm_exception(v));
         RAF_CHECK(!memcmp(dvm_array_data(v, dst), want, 3));
      }
      a[0].j = 8;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "seek",
                                        "(J)V", f, a, 1, &out));
      out.i = 1;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "read",
                                        "()I", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.i == -1);
      a[0].j = 0;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "seek",
                                        "(J)V", f, a, 1, &out));
      out.j = 0;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "readLong",
                                        "()J", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.j == (int64_t)0x4142434445464748LL);
      a[0].j = 0;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "seek",
                                        "(J)V", f, a, 1, &out));
      out.i = 0;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "readInt",
                                        "()I", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.i == 0x41424344);
      a[0].j = 5;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                        "setLength", "(J)V", f, a, 1, &out));
      RAF_CHECK(!dvm_exception(v));
      out.j = 0;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "length",
                                        "()J", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.j == 5);
      a[0].j = 5;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "seek",
                                        "(J)V", f, a, 1, &out));
      out.i = 1;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "read",
                                        "()I", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.i == -1);
      a[0].j = 8;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                        "setLength", "(J)V", f, a, 1, &out));
      RAF_CHECK(!dvm_exception(v));
      a[0].j = 5;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "seek",
                                        "(J)V", f, a, 1, &out));
      out.i = 1;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "read",
                                        "()I", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v) && out.i == 0);
   }

   /* read-only rejects writes; closed operations reject */
   {
      dvm_ref ro = raf_new_object(v);
      dvm_ref rmode = raf_str(v, "r");
      dvm_ref robytes = raf_bytes(v, "X", 1);
      RAF_CHECK(ro && rmode && robytes);
      if (ro && rmode && robytes) {
         RAF_CHECK(raf_call_ctor2(v, ro, pathstr, rmode));
         RAF_CHECK(!dvm_exception(v));
         a[0].l = robytes;
         RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                           "write", "([B)V", ro, a, 1, &out));
         RAF_CHECK(raf_expect(v, "java/io/IOException"));
         RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                           "close", "()V", ro, NULL, 0, &out));
         RAF_CHECK(!dvm_exception(v));
         RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                           "read", "()I", ro, NULL, 0, &out));
         RAF_CHECK(raf_expect(v, "java/io/IOException"));
         RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                           "close", "()V", ro, NULL, 0, &out));
         RAF_CHECK(!dvm_exception(v));
      }
   }
   if (f) {
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "close",
                                        "()V", f, NULL, 0, &out));
      RAF_CHECK(!dvm_exception(v));
      out.i = 1;
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile", "read",
                                        "()I", f, NULL, 0, &out));
      RAF_CHECK(raf_expect(v, "java/io/IOException"));
      RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                        "getFilePointer", "()J", f, NULL, 0,
                                        &out));
      RAF_CHECK(raf_expect(v, "java/io/IOException"));
   }

   /* mode, null and sandbox failures */
   {
      dvm_ref s1 = raf_new_object(v);
      dvm_ref bad = raf_str(v, "x");
      RAF_CHECK(s1 && bad);
      if (s1 && bad) {
         RAF_CHECK(raf_call_ctor2(v, s1, pathstr, bad));
         RAF_CHECK(raf_expect(v, "java/lang/IllegalArgumentException"));
      }
      s1 = raf_new_object(v);
      if (s1) {
         RAF_CHECK(raf_call_ctor2(v, s1, 0, modestr));
         RAF_CHECK(raf_expect(v, "java/lang/NullPointerException"));
      }
      s1 = raf_new_object(v);
      if (s1) {
         char escape[PATH_MAX];
         snprintf(escape, sizeof escape, "%s/../escape.bin", dir);
         dvm_ref es = raf_str(v, escape);
         RAF_CHECK(raf_call_ctor2(v, s1, es, modestr));
         RAF_CHECK(raf_expect(v, "java/io/IOException"));
      }
      s1 = raf_new_object(v);
      if (s1) {
         dvm_ref outside = raf_str(v, "/etc/passwd");
         RAF_CHECK(raf_call_ctor2(v, s1, outside, modestr));
         RAF_CHECK(raf_expect(v, "java/io/IOException"));
      }
      if (symlink(path, linkpath) == 0) {
         s1 = raf_new_object(v);
         if (s1) {
            dvm_ref ls = raf_str(v, linkpath);
            RAF_CHECK(raf_call_ctor2(v, s1, ls, modestr));
            RAF_CHECK(raf_expect(v, "java/io/FileNotFoundException"));
         }
         unlink(linkpath);
      }
      s1 = raf_new_object(v);
      if (s1) {
         dvm_ref ds = raf_str(v, dir);
         RAF_CHECK(raf_call_ctor2(v, s1, ds, modestr));
         RAF_CHECK(raf_expect(v, "java/io/FileNotFoundException"));
      }
   }

   /* readFully past EOF is EOFException, negative seek/setLength rejected */
   {
      dvm_ref s2 = raf_new_object(v);
      dvm_ref rm = raf_str(v, "r");
      if (s2 && rm) {
         RAF_CHECK(raf_call_ctor2(v, s2, pathstr, rm));
         RAF_CHECK(!dvm_exception(v));
         a[0].j = 8;
         RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                           "seek", "(J)V", s2, a, 1, &out));
         RAF_CHECK(!dvm_exception(v));
         {
            dvm_ref one = raf_bytes(v, NULL, 1);
            a[0].l = one;
            RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                              "readFully", "([B)V", s2, a, 1,
                                              &out));
            RAF_CHECK(raf_expect(v, "java/io/EOFException"));
         }
         a[0].j = -1;
         RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                           "seek", "(J)V", s2, a, 1, &out));
         RAF_CHECK(raf_expect(v, "java/io/IOException"));
         RAF_CHECK(random_access_file_call(v, "java/io/RandomAccessFile",
                                           "close", "()V", s2, NULL, 0, &out));
      }
   }

   /* File.equals()/hashCode() through the real method table used by
    * java_basics_equal(); this is the KVs.Companion.of() gap. */
   {
      struct dvm_class *fc = dvm_find_class_or_external(v, "java/io/File");
      struct dvm_method *eq =
          fc ? dvm_find_method(v, fc, "equals", "(Ljava/lang/Object;)Z") : NULL;
      struct dvm_method *hc =
          fc ? dvm_find_method(v, fc, "hashCode", "()I") : NULL;
      dvm_ref fa = raf_file_object(v, path);
      dvm_ref fb = raf_file_object(v, path);
      dvm_ref fx = raf_file_object(v, linkpath);
      union dvm_value ea = {0}, er = {0}, h1 = {0}, h2 = {0};
      RAF_CHECK(eq && eq->builtin && hc && hc->builtin && fa && fb && fx);
      if (eq && eq->builtin && fa && fb && fx) {
         ea.l = fb;
         er.i = 0;
         RAF_CHECK(dvm_call(v, eq, fa, &ea, 1, &er) &&
                   !dvm_exception(v) && er.i);
         ea.l = fx;
         er.i = 1;
         RAF_CHECK(dvm_call(v, eq, fa, &ea, 1, &er) &&
                   !dvm_exception(v) && !er.i);
         ea.l = 0;
         er.i = 1;
         RAF_CHECK(dvm_call(v, eq, fa, &ea, 1, &er) &&
                   !dvm_exception(v) && !er.i);
         RAF_CHECK(dvm_call(v, hc, fa, NULL, 0, &h1) &&
                   !dvm_exception(v));
         RAF_CHECK(dvm_call(v, hc, fb, NULL, 0, &h2) &&
                   !dvm_exception(v) && h1.i == h2.i);
      }
   }

   /* FileChannel.lock/tryLock over the real descriptor, JVM overlap table,
    * read-only and invalid ranges, close, and a forked process conflict. */
   {
      dvm_ref rw = raf_new_object(v);
      dvm_ref ch = 0, ro = 0, rch = 0;
      union dvm_value la[3] = {{0}}, lo = {0};
      if (rw) {
         RAF_CHECK(raf_call_ctor2(v, rw, pathstr, modestr));
         RAF_CHECK(!dvm_exception(v));
      }
      if (rw) {
         lo.l = 0;
         RAF_CHECK(random_access_file_call(
             v, "java/io/RandomAccessFile", "getChannel",
             "()Ljava/nio/channels/FileChannel;", rw, NULL, 0, &lo));
         ch = lo.l;
         RAF_CHECK(ch && !dvm_exception(v));
      }
      if (ch) {
         dvm_ref lock1 = 0, lock2 = 0;
         la[0].j = 0;
         la[1].j = INT64_MAX;
         la[2].i = 0;
         lo.l = 0;
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileChannel", "lock",
             "(JJZ)Ljava/nio/channels/FileLock;", ch, la, 3, &lo));
         RAF_CHECK(!dvm_exception(v) && lo.l);
         lock1 = lo.l;
         lo.i = 0;
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileLock", "isValid", "()Z", lock1, NULL,
             0, &lo));
         RAF_CHECK(!dvm_exception(v) && lo.i);
         lo.l = 0;
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileChannel", "tryLock",
             "(JJZ)Ljava/nio/channels/FileLock;", ch, la, 3, &lo));
         RAF_CHECK(raf_expect(v, "java/nio/channels/OverlappingFileLockException"));
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileLock", "release", "()V", lock1, NULL, 0,
             &lo));
         RAF_CHECK(!dvm_exception(v));
         lo.i = 1;
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileLock", "isValid", "()Z", lock1, NULL,
             0, &lo));
         RAF_CHECK(!dvm_exception(v) && !lo.i);
         lo.l = 0;
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileChannel", "tryLock",
             "(JJZ)Ljava/nio/channels/FileLock;", ch, la, 3, &lo));
         RAF_CHECK(!dvm_exception(v) && lo.l);
         lock2 = lo.l;
         if (lock2) {
            RAF_CHECK(random_access_file_call(
                v, "java/nio/channels/FileLock", "release", "()V", lock2, NULL,
                0, &lo));
            RAF_CHECK(!dvm_exception(v));
         }
         la[0].j = -1;
         la[1].j = 0;
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileChannel", "tryLock",
             "(JJZ)Ljava/nio/channels/FileLock;", ch, la, 3, &lo));
         RAF_CHECK(raf_expect(v, "java/lang/IllegalArgumentException"));
         la[0].j = 1;
         la[1].j = INT64_MAX;
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileChannel", "tryLock",
             "(JJZ)Ljava/nio/channels/FileLock;", ch, la, 3, &lo));
         RAF_CHECK(raf_expect(v, "java/lang/IllegalArgumentException"));

         ro = raf_new_object(v);
         {
            dvm_ref rmode = raf_str(v, "r");
            RAF_CHECK(ro && rmode && raf_call_ctor2(v, ro, pathstr, rmode));
            RAF_CHECK(!dvm_exception(v));
         }
         if (ro) {
            lo.l = 0;
            RAF_CHECK(random_access_file_call(
                v, "java/io/RandomAccessFile", "getChannel",
                "()Ljava/nio/channels/FileChannel;", ro, NULL, 0, &lo));
            rch = lo.l;
            RAF_CHECK(rch && !dvm_exception(v));
         }
         if (rch) {
            la[0].j = 0;
            la[1].j = INT64_MAX;
            la[2].i = 0;
            lo.l = 0;
            RAF_CHECK(random_access_file_call(
                v, "java/nio/channels/FileChannel", "tryLock",
                "(JJZ)Ljava/nio/channels/FileLock;", rch, la, 3, &lo));
            RAF_CHECK(raf_expect(v, "java/nio/channels/NonWritableChannelException"));
            la[2].i = 1;
            lo.l = 0;
            RAF_CHECK(random_access_file_call(
                v, "java/nio/channels/FileChannel", "tryLock",
                "(JJZ)Ljava/nio/channels/FileLock;", rch, la, 3, &lo));
            RAF_CHECK(!dvm_exception(v) && lo.l);
            if (lo.l) {
               RAF_CHECK(random_access_file_call(
                   v, "java/nio/channels/FileLock", "release", "()V", lo.l,
                   NULL, 0, &lo));
               RAF_CHECK(!dvm_exception(v));
            }
         }

         /* close invalidates the channel and raises ClosedChannelException */
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileChannel", "close", "()V", ch, NULL, 0,
             &lo));
         RAF_CHECK(!dvm_exception(v));
         la[2].i = 0;
         lo.l = 0;
         RAF_CHECK(random_access_file_call(
             v, "java/nio/channels/FileChannel", "lock",
             "(JJZ)Ljava/nio/channels/FileLock;", ch, la, 3, &lo));
         RAF_CHECK(raf_expect(v, "java/nio/channels/ClosedChannelException"));

         /* a different process holds a write lock: tryLock must return null,
          * and succeed after that process exits */
         if (rch) {
            int p1[2] = {-1, -1}, p2[2] = {-1, -1};
            pid_t pid = -1;
            if (pipe(p1) == 0 && pipe(p2) == 0) {
               pid = fork();
               if (pid == 0) {
                  int fd;
                  struct flock fl;
                  char c;
                  close(p1[0]);
                  close(p2[1]);
                  fd = open(path, O_RDWR);
                  if (fd >= 0) {
                     memset(&fl, 0, sizeof fl);
                     fl.l_type = F_WRLCK;
                     fl.l_whence = SEEK_SET;
                     fl.l_start = 0;
                     fl.l_len = 0; /* to EOF */
                     if (fcntl(fd, F_SETLK, &fl) == 0) {
                        c = 1;
                        (void)write(p1[1], &c, 1);
                        (void)read(p2[0], &c, 1);
                        fl.l_type = F_UNLCK;
                        (void)fcntl(fd, F_SETLK, &fl);
                     }
                     close(fd);
                  }
                  _exit(0);
               }
               if (pid > 0) {
                  char c = 0;
                  close(p1[1]);
                  close(p2[0]);
                  if (read(p1[0], &c, 1) == 1) {
                     union dvm_value co = {0};
                     la[0].j = 0;
                     la[1].j = INT64_MAX;
                     la[2].i = 1;
                     RAF_CHECK(random_access_file_call(
                         v, "java/nio/channels/FileChannel", "tryLock",
                         "(JJZ)Ljava/nio/channels/FileLock;", rch, la, 3,
                         &co));
                     RAF_CHECK(!dvm_exception(v) && !co.l);
                     c = 1;
                     (void)write(p2[1], &c, 1);
                     (void)waitpid(pid, NULL, 0);
                     co.l = 0;
                     RAF_CHECK(random_access_file_call(
                         v, "java/nio/channels/FileChannel", "tryLock",
                         "(JJZ)Ljava/nio/channels/FileLock;", rch, la, 3,
                         &co));
                     RAF_CHECK(!dvm_exception(v) && co.l);
                     if (co.l) {
                        RAF_CHECK(random_access_file_call(
                            v, "java/nio/channels/FileLock", "release", "()V",
                            co.l, NULL, 0, &co));
                        RAF_CHECK(!dvm_exception(v));
                     }
                  } else {
                     (void)waitpid(pid, NULL, 0);
                  }
                  close(p1[0]);
                  close(p2[1]);
               } else {
                  close(p1[0]);
                  close(p1[1]);
                  close(p2[0]);
                  close(p2[1]);
               }
            }
         }
         if (rch) {
            RAF_CHECK(random_access_file_call(
                v, "java/nio/channels/FileChannel", "close", "()V", rch, NULL,
                0, &lo));
            RAF_CHECK(!dvm_exception(v));
         }
         if (ro) {
            RAF_CHECK(random_access_file_call(
                v, "java/io/RandomAccessFile", "close", "()V", ro, NULL, 0,
                &lo));
            RAF_CHECK(!dvm_exception(v));
         }
      }
      if (rw) {
         RAF_CHECK(random_access_file_call(
             v, "java/io/RandomAccessFile", "close", "()V", rw, NULL, 0, &lo));
         RAF_CHECK(!dvm_exception(v));
      }
   }

   unlink(path);
   rmdir(dir);
   sdk_trace_fprintf(stderr, "SELFTEST %s random_access_file checks=%d\n",
                     okay ? "PASS" : "FAIL", checks);
   return okay;
}

int raf_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 (void)v;(void)visit;(void)u;return o->data_destroy==raf_free||o->data_destroy==fd_free||o->data_destroy==channel_free||o->data_destroy==file_lock_free?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
