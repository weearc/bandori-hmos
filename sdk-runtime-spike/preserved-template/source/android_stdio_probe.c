#define _POSIX_C_SOURCE 200809L
#include "android_stdio_probe.h"
#include "android_stdio_shim.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);
static int callv(void *stream, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = android_stdio_vfprintf(stream, fmt, args);
  va_end(args);
  return n;
}
int android_stdio_probe_run(const char *directory) {
  char path[2048], buffer[64];
  const char expected[] = "stdio-vfprintf-fixture\n";
  void *file = NULL, *stale = NULL;
  int rc = 0, file_ok = 0, slots_ok = 0;
#define CHECK(c, n)                                                            \
  do {                                                                         \
    if (!(c)) {                                                                \
      rc = (n);                                                                \
      goto done;                                                               \
    }                                                                          \
  } while (0)
  CHECK(directory, 1);
  int n = snprintf(path, sizeof path, "%s/native-stdio-fixture.txt", directory);
  CHECK(n > 0 && (size_t)n < sizeof path, 2);
  file = android_stdio_fopen(path, "w+");
  CHECK(file, 3);
  CHECK(android_stdio_fprintf(file, "%s", "stdio-") == 6, 4);
  CHECK(callv(file, "%s", "vfprintf-") == 9, 5);
  CHECK(android_stdio_fwrite("fixture", 1, 7, file) == 7 &&
            android_stdio_fputc('\n', file) == '\n',
        6);
  CHECK(android_stdio_ftello(file) == (off_t)(sizeof expected - 1), 7);
  CHECK(android_stdio_fflush(NULL) == 0 &&
            android_stdio_fseeko(file, 0, SEEK_SET) == 0,
        8);
  CHECK(android_stdio_fread(buffer, 1, sizeof expected - 1, file) ==
                sizeof expected - 1 &&
            !memcmp(buffer, expected, sizeof expected - 1),
        9);
  CHECK(android_stdio_fseek(file, 0, SEEK_END) == 0 &&
            android_stdio_fwrite("!", 1, 1, file) == 1,
        10);
  stale = file;
  int closed = android_stdio_fclose(file);
  file = NULL;
  CHECK(closed == 0, 11);
  errno = 0;
  CHECK(android_stdio_fread(buffer, 1, 1, stale) == 0 && errno == EBADF, 12);
  file = android_stdio_fopen(path, "r");
  CHECK(file && file != stale, 13);
  CHECK(android_stdio_fread(buffer, 1, sizeof expected, file) ==
                sizeof expected &&
            !memcmp(buffer, expected, sizeof expected - 1) &&
            buffer[sizeof expected - 1] == '!',
        14);
  closed = android_stdio_fclose(file);
  file = NULL;
  CHECK(closed == 0, 15);
  file_ok = 1;
  errno = 0;
  CHECK(android_stdio_fread(buffer, 1, 1, (void *)1) == 0 && errno == EBADF,
        16);
  for (int i = 0; i < 3; i++) {
    errno = 0;
    CHECK(android_stdio_fread(buffer, 0, 1, &android_stdio_sF[i][0]) == 0 &&
              errno == 0,
          17 + i);
  }
  errno = 0;
  CHECK(android_stdio_fclose(&android_stdio_sF[0][0]) == EOF &&
            errno == ENOTSUP,
        20);
  const char marker[] = "ANDROID_STDIO_DEVICE_MARKER\n";
  CHECK(android_stdio_fwrite(marker, 1, sizeof marker - 1,
                             &android_stdio_sF[2][0]) == sizeof marker - 1 &&
            android_stdio_fflush(&android_stdio_sF[2][0]) == 0,
        21);
  slots_ok = 1;
done:
  if (file)
    android_stdio_fclose(file);
  sdk_trace_fprintf(stderr,
                    "ANDROID_STDIO_PROBE %s check=%d real_file=%d std_slots=%d "
                    "engine_entry_called=0 direct_FILE_fields=unsupported\n",
                    rc ? "FAIL" : "PASS", rc, file_ok, slots_ok);
  return rc;
#undef CHECK
}
