#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "android_stdio_shim.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdatomic.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

/* Diagnostic guard for the engine's strlen calls.  The PlayerPrefs upgrade
 * formats a message whose backing string is empty and then calls strlen on a
 * NULL data pointer.  Returning 0 for that empty string lets the engine
 * continue so the host can observe the real next step; every occurrence is
 * logged with the caller's module offset and the two libunity globals used by
 * the formatter (S at +0x1883300, L at +0x182d7c0 for this libunity build). */
static void guard_probe_log(const char *fmt,...)
{
    FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
    if (!f) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fclose(f);
}
size_t android_strlen_guard(const char *s)
{
    if (s) return strlen(s);
    Dl_info di;
    void *lr = __builtin_return_address(0);
    uintptr_t base = 0;
    const char *module = "?";
    unsigned long off = 0;
    if (lr && dladdr(lr, &di)) {
        base = (uintptr_t)di.dli_fbase;
        module = di.dli_fname ? di.dli_fname : "?";
        if (base) off = (unsigned long)((uintptr_t)lr - base);
    }
    uint64_t s_global = 0, l_global = 0;
    if (base) {
        s_global = *(volatile uint64_t *)(base + 0x1883300);
        l_global = *(volatile uint64_t *)(base + 0x182d7c0);
    }
    guard_probe_log("STRLEN_NULL_GUARD module=%s off=0x%lx S=0x%llx L=0x%llx result=0",
                    module, off, (unsigned long long)s_global, (unsigned long long)l_global);
    sdk_trace_fprintf(stderr,
        "STRLEN_NULL_GUARD module=%s off=0x%lx S=0x%llx L=0x%llx result=0\n",
        module, off, (unsigned long long)s_global,
        (unsigned long long)l_global);
    return 0;
}

_Alignas(8) unsigned char android_stdio_sF[3][152];
#define SLOT_COUNT 256
#define TAG (((uintptr_t)0xd3) << 56)
_Static_assert(sizeof(void *) == 8 && sizeof(long) == 8 && sizeof(off_t) == 8,
               "Android LP64 required");
typedef struct {
  pthread_mutex_t op;
  FILE *file;
  uintptr_t token;
  unsigned refs;
  int state;
  int trace_archive;
} stream;
static stream streams[SLOT_COUNT];
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t drained = PTHREAD_COND_INITIALIZER;
static uintptr_t next_token = 1;
static void setup(void) {
  for (unsigned i = 0; i < SLOT_COUNT; i++)
    pthread_mutex_init(&streams[i].op, NULL);
  streams[0].file = stdin;
  streams[1].file = stdout;
  streams[2].file = stderr;
  for (unsigned i = 0; i < 3; i++) {
    streams[i].state = 1;
    streams[i].token = (uintptr_t)&android_stdio_sF[i][0];
  }
}
static stream *pin(void *token) {
  pthread_once(&once, setup);
  pthread_mutex_lock(&registry);
  stream *s = NULL;
  for (unsigned i = 0; i < SLOT_COUNT; i++)
    if (streams[i].state == 1 && streams[i].token == (uintptr_t)token) {
      s = &streams[i];
      s->refs++;
      break;
    }
  pthread_mutex_unlock(&registry);
  if (!s)
    errno = EBADF;
  return s;
}
static void unpin(stream *s, int e) {
  pthread_mutex_unlock(&s->op);
  pthread_mutex_lock(&registry);
  if (!--s->refs)
    pthread_cond_broadcast(&drained);
  pthread_mutex_unlock(&registry);
  errno = e;
}
static stream *enter(void *t) {
  stream *s = pin(t);
  if (s)
    pthread_mutex_lock(&s->op);
  return s;
}
void *android_stdio_fopen(const char *path, const char *mode) {
  if (!path || !mode) {
    errno = EINVAL;
    return NULL;
  }
  FILE *f = fopen(path, mode);
  int saved_errno=errno;static _Atomic unsigned fopen_logs;
  if(atomic_fetch_add(&fopen_logs,1)<128)
    guard_probe_log("NATIVE_FILE_FOPEN path=%s mode=%s success=%d errno=%d",path,mode,f!=NULL,f?0:saved_errno);
  errno=saved_errno;
  if (!f)
    return NULL;
  pthread_once(&once, setup);
  pthread_mutex_lock(&registry);
  stream *s = NULL;
  for (unsigned i = 3; i < SLOT_COUNT; i++)
    if (!streams[i].state) {
      s = &streams[i];
      break;
    }
  if (!s || next_token > UINT64_C(0xffffffffffff)) {
    pthread_mutex_unlock(&registry);
    fclose(f);
    errno = EMFILE;
    return NULL;
  }
  s->file = f;
  const char *archive_name=strstr(path,"/base.apk");
  s->trace_archive=archive_name&&archive_name[9]=='\0';
  s->refs = 0;
  s->token = TAG | (next_token++ << 8) | (uintptr_t)(s - streams);
  s->state = 1;
  void *t = (void *)s->token;
  pthread_mutex_unlock(&registry);
  return t;
}
typedef struct {
  stream *s;
  int locked;
} close_context;
static void cancel_close(void *arg) {
  close_context *c = arg;
  if (c->locked) {
    c->s->state = 1;
    pthread_mutex_unlock(&registry);
  } else {
    pthread_mutex_lock(&registry);
    c->s->state = 3;
    pthread_mutex_unlock(&registry);
  }
}
int android_stdio_fclose(void *t) {
  pthread_once(&once, setup);
  pthread_mutex_lock(&registry);
  stream *s = NULL;
  for (unsigned i = 0; i < SLOT_COUNT; i++)
    if (streams[i].state == 1 && streams[i].token == (uintptr_t)t) {
      s = &streams[i];
      break;
    }
  if (!s || s < &streams[3]) {
    int e = s ? ENOTSUP : EBADF;
    pthread_mutex_unlock(&registry);
    errno = e;
    return EOF;
  }
  int r, e;
  close_context ctx = {s, 1};
  s->state = 2;
  pthread_cleanup_push(cancel_close, &ctx);
  while (s->refs)
    pthread_cond_wait(&drained, &registry);
  pthread_mutex_unlock(&registry);
  ctx.locked = 0;
  r = fclose(s->file);
  e = errno;
  pthread_mutex_lock(&registry);
  s->file = NULL;
  s->token = 0;
  s->state = 0;
  pthread_mutex_unlock(&registry);
  pthread_cleanup_pop(0);
  errno = e;
  return r;
}
static void cancel_stream(void *p) { unpin((stream *)p, errno); }
static _Atomic unsigned archive_io_logs;
#define RUN_STREAM(token, type, failure, expression)                           \
  do {                                                                         \
    stream *s = enter(token);                                                  \
    if (!s)                                                                    \
      return (failure);                                                        \
    type result;                                                               \
    int saved;                                                                 \
    pthread_cleanup_push(cancel_stream, s);                                    \
    result = (expression);                                                     \
    saved = errno;                                                             \
    if(s->trace_archive&&atomic_fetch_add(&archive_io_logs,1)<256)                \
      guard_probe_log("NATIVE_ARCHIVE_IO token=%p op=%s result=%lld errno=%d pos=%lld eof=%d error=%d", \
        token,#expression,(long long)(intptr_t)result,saved,                     \
        (long long)ftello(s->file),feof(s->file),ferror(s->file));                \
    pthread_cleanup_pop(0);                                                    \
    unpin(s, saved);                                                           \
    return result;                                                             \
  } while (0)
int android_stdio_fflush(void *t) {
  if (!t)
    return fflush(NULL);
  RUN_STREAM(t, int, EOF, fflush(s->file));
}
size_t android_stdio_fread(void *p, size_t z, size_t n, void *t) {
  RUN_STREAM(t, size_t, 0, fread(p, z, n, s->file));
}
size_t android_stdio_fwrite(const void *p, size_t z, size_t n, void *t) {
  RUN_STREAM(t, size_t, 0, fwrite(p, z, n, s->file));
}
int android_stdio_fseek(void *t, long off, int whence) {
  RUN_STREAM(t, int, -1, fseek(s->file, off, whence));
}
int android_stdio_fseeko(void *t, off_t off, int whence) {
  RUN_STREAM(t, int, -1, fseeko(s->file, off, whence));
}
off_t android_stdio_ftello(void *t) {
  RUN_STREAM(t, off_t, -1, ftello(s->file));
}
int android_stdio_fputc(int c, void *t) {
  RUN_STREAM(t, int, EOF, fputc(c, s->file));
}
int android_stdio_vfprintf(void *t, const char *f, va_list args) {
  if (!f) {
    errno = EINVAL;
    return -1;
  }
  RUN_STREAM(t, int, -1, vfprintf(s->file, f, args));
}
int android_stdio_fprintf(void *t, const char *f, ...) {
  va_list args;
  va_start(args, f);
  int r = android_stdio_vfprintf(t, f, args);
  va_end(args);
  return r;
}
static void *wrap_file(FILE *f) {
  if (!f)
    return NULL;
  pthread_once(&once, setup);
  pthread_mutex_lock(&registry);
  stream *s = NULL;
  for (unsigned i = 3; i < SLOT_COUNT; i++)
    if (!streams[i].state) {
      s = &streams[i];
      break;
    }
  if (!s || next_token > UINT64_C(0xffffffffffff)) {
    pthread_mutex_unlock(&registry);
    fclose(f);
    errno = EMFILE;
    return NULL;
  }
  s->file = f;
  s->trace_archive=0;
  s->refs = 0;
  s->token = TAG | (next_token++ << 8) | (uintptr_t)(s - streams);
  s->state = 1;
  void *t = (void *)s->token;
  pthread_mutex_unlock(&registry);
  return t;
}
void *android_stdio_fdopen(int fd, const char *mode) {
  if (!mode) {
    errno = EINVAL;
    return NULL;
  }
  return wrap_file(fdopen(fd, mode));
}
int android_stdio_vfscanf(void *t, const char *f, va_list args) {
  if (!f) {
    errno = EINVAL;
    return -1;
  }
  RUN_STREAM(t, int, -1, vfscanf(s->file, f, args));
}
int android_stdio_fscanf(void *t, const char *f, ...) {
  va_list args;
  va_start(args, f);
  int r = android_stdio_vfscanf(t, f, args);
  va_end(args);
  return r;
}
char *android_stdio_fgets(char *buf, int size, void *t) {
  RUN_STREAM(t, char *, NULL, fgets(buf, size, s->file));
}
int android_stdio_fputs(const char *str, void *t) {
  RUN_STREAM(t, int, EOF, fputs(str, s->file));
}
int android_stdio_feof(void *t) {
  RUN_STREAM(t, int, 0, feof(s->file));
}
int android_stdio_ferror(void *t) {
  RUN_STREAM(t, int, 0, ferror(s->file));
}
void android_stdio_clearerr(void *t) {
  stream *s = enter(t);
  if (!s)
    return;
  int saved;
  pthread_cleanup_push(cancel_stream, s);
  clearerr(s->file);
  saved = errno;
  pthread_cleanup_pop(0);
  unpin(s, saved);
}
int android_stdio_fileno(void *t) {
  RUN_STREAM(t, int, -1, fileno(s->file));
}
long android_stdio_ftell(void *t) {
  RUN_STREAM(t, long, -1, ftell(s->file));
}
void android_stdio_setbuf(void *t, char *buf) {
  stream *s = enter(t);
  if (!s)
    return;
  int saved;
  pthread_cleanup_push(cancel_stream, s);
  setbuf(s->file, buf);
  saved = errno;
  pthread_cleanup_pop(0);
  unpin(s, saved);
}
int android_stdio_setvbuf(void *t, char *buf, int mode, size_t size) {
  RUN_STREAM(t, int, -1, setvbuf(s->file, buf, mode, size));
}
