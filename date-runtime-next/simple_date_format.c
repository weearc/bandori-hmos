#include "simple_date_format.h"
#include "java_basics.h"
#include "small_boxes.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define SDF_MAGIC 0x53444631u /* "SDF1" */

struct sdf_value {
   uint32_t magic;
   struct dvm *vm;
   dvm_ref locale;       /* pinned; 0 for the one-argument constructor */
   char *pattern;
};

extern int sdk_trace_fprintf(FILE *, const char *, ...);

static void sdf_value_destroy(void *p)
{
   struct sdf_value *s = p;
   if (!s) return;
   if (s->vm && s->locale) dvm_unpin(s->vm, s->locale);
   free(s->pattern);
   s->magic = 0;
   free(s);
}

static struct sdf_value *sdf_value_of(struct dvm *v, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(v, ref);
   if (!o || o->data_destroy != sdf_value_destroy) return NULL;
   struct sdf_value *s = o->data;
   return s && s->magic == SDF_MAGIC ? s : NULL;
}

static bool is_ascii_letter(unsigned char ch)
{
   return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

enum pattern_result {
   PATTERN_OK = 0,
   PATTERN_ILLEGAL,
   PATTERN_UNSUPPORTED
};

/* Validate the Java SimpleDateFormat pattern subset owned by this module.
 * Valid Java pattern letters that this module does not implement fail closed
 * as UnsupportedOperationException; characters that are not pattern letters
 * at all are IllegalArgumentException, as in java.text.SimpleDateFormat. */
static enum pattern_result validate_pattern(const char *p, size_t len,
                                            unsigned char *bad,
                                            size_t *bad_count)
{
   bool quoted = false;
   size_t i = 0;
   while (i < len) {
      unsigned char ch = (unsigned char)p[i];
      if (ch == '\'') {
         if (i + 1 < len && p[i + 1] == '\'') {
            i += 2;
            continue;
         }
         quoted = !quoted;
         ++i;
         continue;
      }
      if (quoted || !is_ascii_letter(ch)) {
         ++i;
         continue;
      }
      size_t start = i;
      while (i < len && (unsigned char)p[i] == ch) ++i;
      size_t count = i - start;
      *bad = ch;
      *bad_count = count;
      switch (ch) {
      /* Valid SimpleDateFormat letters that are not implemented here. */
      case 'G': case 'Y': case 'L': case 'w': case 'W': case 'D':
      case 'F': case 'E': case 'u': case 'a': case 'z':
         return PATTERN_UNSUPPORTED;
      case 'y':
         if (count > 4) return PATTERN_UNSUPPORTED;
         break;
      case 'M':
         if (count > 2) return PATTERN_UNSUPPORTED; /* MMM/MMMM are text */
         break;
      case 'd': case 'H': case 'k': case 'K': case 'h': case 'm': case 's':
         if (count > 2) return PATTERN_UNSUPPORTED;
         break;
      case 'S':
         if (count > 3) return PATTERN_UNSUPPORTED;
         break;
      case 'Z':
         if (count > 3) return PATTERN_UNSUPPORTED; /* ZZZZ is localized GMT */
         break;
      case 'X':
         if (count > 3) return PATTERN_UNSUPPORTED;
         break;
      default:
         return PATTERN_ILLEGAL;
      }
   }
   if (quoted) {
      *bad = '\'';
      *bad_count = 1;
      return PATTERN_ILLEGAL;
   }
   return PATTERN_OK;
}

struct sb {
   char *p;
   size_t len;
   size_t cap;
};

static bool sb_reserve(struct sb *b, size_t extra)
{
   if (extra > (size_t)-1 - b->len) return false;
   size_t need = b->len + extra;
   if (need <= b->cap) return true;
   size_t cap = b->cap ? b->cap : 64;
   while (cap < need) {
      if (cap > (size_t)-1 / 2) {
         cap = need;
         break;
      }
      cap *= 2;
   }
   char *q = realloc(b->p, cap);
   if (!q) return false;
   b->p = q;
   b->cap = cap;
   return true;
}

static bool sb_putn(struct sb *b, const char *p, size_t n)
{
   if (!sb_reserve(b, n)) return false;
   memcpy(b->p + b->len, p, n);
   b->len += n;
   return true;
}

static bool sb_putc(struct sb *b, char ch)
{
   return sb_putn(b, &ch, 1);
}

static bool sb_put_uint(struct sb *b, unsigned long long v, int width)
{
   char tmp[32];
   int n = 0;
   do {
      tmp[n++] = (char)('0' + (v % 10u));
      v /= 10u;
   } while (v && n < (int)sizeof tmp);
   while (n < width && n < (int)sizeof tmp) tmp[n++] = '0';
   char out[32];
   for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
   return sb_putn(b, out, (size_t)n);
}

static bool put_zone_rfc822(struct sb *b, int64_t off)
{
   if (off % 60 != 0) return false;
   int64_t a = off < 0 ? -off : off;
   unsigned long long hh = (unsigned long long)(a / 3600);
   unsigned long long mm = (unsigned long long)((a % 3600) / 60);
   if (hh > 23 || mm > 59) return false;
   if (!sb_putc(b, off < 0 ? '-' : '+')) return false;
   if (!sb_put_uint(b, hh, 2)) return false;
   return sb_put_uint(b, mm, 2);
}

static bool put_zone_iso(struct sb *b, int64_t off, size_t count)
{
   if (off == 0) return sb_putc(b, 'Z');
   if (off % 60 != 0) return false;
   int64_t a = off < 0 ? -off : off;
   unsigned long long hh = (unsigned long long)(a / 3600);
   unsigned long long mm = (unsigned long long)((a % 3600) / 60);
   if (hh > 23 || mm > 59) return false;
   if (!sb_putc(b, off < 0 ? '-' : '+')) return false;
   if (!sb_put_uint(b, hh, 2)) return false;
   if (count == 1 && mm == 0) return true;
   if (count == 3 && !sb_putc(b, ':')) return false;
   return sb_put_uint(b, mm, 2);
}

static bool emit_field(struct dvm *v, struct sb *b, unsigned char ch,
                       size_t count, const struct tm *tm,
                       int millis_part, int64_t offset)
{
   int64_t year = (int64_t)tm->tm_year + 1900;
   switch (ch) {
   case 'y':
      if (year < 1) {
         dvm__throw(v, "java/lang/UnsupportedOperationException",
                    "negative or zero year is outside the supported subset");
         return false;
      }
      if (count == 2)
         return sb_put_uint(b, (unsigned long long)(year % 100), 2);
      return sb_put_uint(b, (unsigned long long)year, (int)count);
   case 'M':
      return sb_put_uint(b, (unsigned long long)(tm->tm_mon + 1), (int)count);
   case 'd':
      return sb_put_uint(b, (unsigned long long)tm->tm_mday, (int)count);
   case 'H':
      return sb_put_uint(b, (unsigned long long)tm->tm_hour, (int)count);
   case 'k': {
      int h = tm->tm_hour == 0 ? 24 : tm->tm_hour;
      return sb_put_uint(b, (unsigned long long)h, (int)count);
   }
   case 'K':
      return sb_put_uint(b, (unsigned long long)(tm->tm_hour % 12), (int)count);
   case 'h': {
      int h = tm->tm_hour % 12;
      if (h == 0) h = 12;
      return sb_put_uint(b, (unsigned long long)h, (int)count);
   }
   case 'm':
      return sb_put_uint(b, (unsigned long long)tm->tm_min, (int)count);
   case 's':
      return sb_put_uint(b, (unsigned long long)tm->tm_sec, (int)count);
   case 'S':
      return sb_put_uint(b, (unsigned long long)millis_part, (int)count);
   case 'Z':
      if (!put_zone_rfc822(b, offset)) {
         dvm__throw(v, "java/lang/UnsupportedOperationException",
                    "zone offset is not representable as RFC 822 time");
         return false;
      }
      return true;
   case 'X':
      if (!put_zone_iso(b, offset, count)) {
         dvm__throw(v, "java/lang/UnsupportedOperationException",
                    "zone offset is not representable as ISO 8601 time");
         return false;
      }
      return true;
   default:
      dvm__throw(v, "java/lang/UnsupportedOperationException",
                 "unsupported pattern letter '%c'", (int)ch);
      return false;
   }
}

static bool format_pattern(struct dvm *v, const struct sdf_value *s,
                           int64_t millis, union dvm_value *out)
{
   struct tm tm;
   int millis_part = 0;
   int64_t offset = 0;
   if (!date_runtime_host_fields(millis, &tm, &offset, &millis_part)) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "host local time unavailable");
      return false;
   }
   const char *p = s->pattern;
   size_t len = strlen(p);
   struct sb b = {0};
   bool quoted = false;
   bool okay = true;
   size_t i = 0;
   while (i < len) {
      unsigned char ch = (unsigned char)p[i];
      if (ch == '\'') {
         if (i + 1 < len && p[i + 1] == '\'') {
            if (!(okay = sb_putc(&b, '\''))) break;
            i += 2;
            continue;
         }
         quoted = !quoted;
         ++i;
         continue;
      }
      if (quoted) {
         if (!(okay = sb_putc(&b, (char)ch))) break;
         ++i;
         continue;
      }
      if (!is_ascii_letter(ch)) {
         if (!(okay = sb_putc(&b, (char)ch))) break;
         ++i;
         continue;
      }
      size_t start = i;
      while (i < len && (unsigned char)p[i] == ch) ++i;
      size_t count = i - start;
      okay = emit_field(v, &b, ch, count, &tm, millis_part, offset);
      if (!okay) break;
   }
   if (okay && quoted) {
      dvm__throw(v, "java/lang/IllegalArgumentException",
                 "Unterminated quote");
      okay = false;
   }
   if (!okay) {
      free(b.p);
      if (!dvm_exception(v))
         dvm__throw(v, "java/lang/OutOfMemoryError", "SimpleDateFormat buffer");
      return false;
   }
   dvm_ref text = dvm_new_string_n(v, b.p ? b.p : "", b.len);
   free(b.p);
   if (!text) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "SimpleDateFormat result");
      return false;
   }
   out->l = text;
   return true;
}

static bool sdf_attach(struct dvm *v, dvm_ref self, const char *pattern,
                       size_t len, dvm_ref locale)
{
   struct dvm_object *o = dvm__obj(v, self);
   if (!o) {
      dvm__throw(v, "java/lang/NullPointerException",
                 "SimpleDateFormat receiver");
      return false;
   }
   if (o->data_destroy == sdf_value_destroy) {
      struct sdf_value *old = o->data;
      if (old) {
         if (old->vm && old->locale) dvm_unpin(old->vm, old->locale);
         free(old->pattern);
         old->magic = 0;
         free(old);
      }
      o->data = NULL;
      o->data_destroy = NULL;
   }
   struct sdf_value *s = calloc(1, sizeof *s);
   char *copy = malloc(len + 1);
   if (!s || !copy) {
      free(s);
      free(copy);
      dvm__throw(v, "java/lang/OutOfMemoryError", "SimpleDateFormat payload");
      return false;
   }
   memcpy(copy, pattern, len);
   copy[len] = '\0';
   s->magic = SDF_MAGIC;
   s->vm = v;
   s->pattern = copy;
   if (locale) {
      dvm_pin(v, locale);
      s->locale = locale;
   }
   o->data = s;
   o->data_destroy = sdf_value_destroy;
   o->data_owner = NULL;
   return true;
}

static bool sdf_construct(struct dvm *v, dvm_ref self,
                          const union dvm_value *a, int nargs,
                          bool with_locale)
{
   if (nargs != (with_locale ? 2 : 1)) return false;
   if (!a[0].l) {
      dvm__throw(v, "java/lang/NullPointerException", "pattern");
      return true;
   }
   const char *pattern = dvm_string_utf8(v, a[0].l);
   size_t len = dvm_string_utf8_length(v, a[0].l);
   if (!pattern) {
      dvm__throw(v, "java/lang/IllegalArgumentException",
                 "pattern is not a String");
      return true;
   }
   if (strlen(pattern) != len) {
      dvm__throw(v, "java/lang/UnsupportedOperationException",
                 "pattern contains an embedded NUL");
      return true;
   }
   if (with_locale && !a[1].l) {
      dvm__throw(v, "java/lang/NullPointerException", "locale");
      return true;
   }
   unsigned char bad = 0;
   size_t bad_count = 0;
   enum pattern_result pr = validate_pattern(pattern, len, &bad, &bad_count);
   if (pr == PATTERN_ILLEGAL) {
      if (bad == '\'')
         dvm__throw(v, "java/lang/IllegalArgumentException",
                    "Unterminated quote");
      else
         dvm__throw(v, "java/lang/IllegalArgumentException",
                    "Illegal pattern character '%c'", (int)bad);
      return true;
   }
   if (pr == PATTERN_UNSUPPORTED) {
      dvm__throw(v, "java/lang/UnsupportedOperationException",
                 "unsupported pattern letter '%c' count=%u",
                 (int)bad, (unsigned)bad_count);
      return true;
   }
   if (!sdf_attach(v, self, pattern, len, with_locale ? a[1].l : 0))
      return true;
   return true;
}

static bool number_as_millis(struct dvm *v, dvm_ref ref, int64_t *millis)
{
   struct dvm_object *o = dvm__obj(v, ref);
   const char *cls = o && o->cls ? o->cls->name : NULL;
   if (!cls) return false;
   union dvm_value r = {0};
   if (!strcmp(cls, "java/lang/Long") || !strcmp(cls, "java/lang/Integer") ||
       !strcmp(cls, "java/lang/Float") || !strcmp(cls, "java/lang/Double")) {
      if (!java_basics_call(v, cls, "longValue", "()J", ref, NULL, 0, &r))
         return false;
   } else if (!strcmp(cls, "java/lang/Short") || !strcmp(cls, "java/lang/Byte")) {
      if (!small_boxes_call(v, cls, "longValue", "()J", ref, NULL, 0, &r))
         return false;
   } else {
      /* DateFormat.format(Object) reports a non-Date, non-Number argument as
       * IllegalArgumentException.  A Number subclass without an implemented
       * longValue is likewise not guessed. */
      dvm__throw(v, "java/lang/IllegalArgumentException",
                 "Cannot format given Object as a Date");
      return false;
   }
   if (dvm_exception(v)) return false;
   *millis = r.j;
   return true;
}

#define IS(name, signature) (!strcmp(m, (name)) && !strcmp(sig, (signature)))

bool simple_date_format_call(struct dvm *v, const char *c, const char *m,
                             const char *sig, dvm_ref self,
                             const union dvm_value *a, int nargs,
                             union dvm_value *out)
{
   if (strcmp(c, "java/text/SimpleDateFormat")) return false;

   if (IS("<init>", "(Ljava/lang/String;)V"))
      return sdf_construct(v, self, a, nargs, false);
   if (IS("<init>", "(Ljava/lang/String;Ljava/util/Locale;)V"))
      return sdf_construct(v, self, a, nargs, true);

   if (IS("format", "(Ljava/util/Date;)Ljava/lang/String;")) {
      if (nargs != 1) return false;
      if (!a[0].l) {
         dvm__throw(v, "java/lang/NullPointerException", "date");
         return true;
      }
      struct sdf_value *s = sdf_value_of(v, self);
      if (!s) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "SimpleDateFormat not initialized");
         return true;
      }
      int64_t millis;
      if (!date_runtime_get_millis(v, a[0].l, &millis)) {
         if (!date_runtime_is_date(v, a[0].l))
            dvm__throw(v, "java/lang/ClassCastException", "not a Date");
         else
            dvm__throw(v, "java/lang/IllegalStateException",
                       "Date not initialized");
         return true;
      }
      if (!format_pattern(v, s, millis, out)) return true;
      return true;
   }
   if (IS("format", "(Ljava/lang/Object;)Ljava/lang/String;")) {
      if (nargs != 1) return false;
      struct sdf_value *s = sdf_value_of(v, self);
      if (!s) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "SimpleDateFormat not initialized");
         return true;
      }
      int64_t millis;
      if (date_runtime_get_millis(v, a[0].l, &millis)) {
         if (!format_pattern(v, s, millis, out)) return true;
         return true;
      }
      if (dvm_exception(v)) return true;
      if (!a[0].l) {
         dvm__throw(v, "java/lang/IllegalArgumentException",
                    "Cannot format given Object as a Date");
         return true;
      }
      if (!number_as_millis(v, a[0].l, &millis)) {
         if (!dvm_exception(v))
            dvm__throw(v, "java/lang/IllegalArgumentException",
                       "Cannot format given Object as a Date");
         return true;
      }
      if (!format_pattern(v, s, millis, out)) return true;
      return true;
   }
   /* parse, setLenient, setTimeZone, toPattern are intentionally not owned:
    * an unimplemented call must surface as a missing API, never a fake value. */
   return false;
}

#undef IS

/* ---------------------------------------------------------------------- */
/* selftest                                                               */

static dvm_ref new_instance(struct dvm *v, const char *name)
{
   struct dvm_class *cls = dvm_find_class_or_external(v, name);
   return dvm_new_object(v, cls);
}

static bool exception_is(struct dvm *v, const char *name)
{
   dvm_ref e = dvm_exception(v);
   struct dvm_object *o = dvm__obj(v, e);
   return o && o->cls && o->cls->name && !strcmp(o->cls->name, name);
}

static bool make_sdf(struct dvm *v, dvm_ref pattern, dvm_ref locale,
                     dvm_ref *out)
{
   dvm_ref sdf = new_instance(v, "java/text/SimpleDateFormat");
   if (!sdf) return false;
   union dvm_value a[2] = {0}, r = {0};
   a[0].l = pattern;
   a[1].l = locale;
   if (locale) {
      if (!simple_date_format_call(v, "java/text/SimpleDateFormat", "<init>",
                                   "(Ljava/lang/String;Ljava/util/Locale;)V",
                                   sdf, a, 2, &r))
         return false;
   } else {
      if (!simple_date_format_call(v, "java/text/SimpleDateFormat", "<init>",
                                   "(Ljava/lang/String;)V",
                                   sdf, a, 1, &r))
         return false;
   }
   if (dvm_exception(v)) return false;
   *out = sdf;
   return true;
}

static bool format_date(struct dvm *v, dvm_ref sdf, dvm_ref date,
                        dvm_ref *text)
{
   union dvm_value a = {.l = date}, r = {0};
   if (!simple_date_format_call(v, "java/text/SimpleDateFormat", "format",
                                "(Ljava/util/Date;)Ljava/lang/String;",
                                sdf, &a, 1, &r))
      return false;
   if (dvm_exception(v) || !r.l) return false;
   *text = r.l;
   return true;
}

static bool format_object(struct dvm *v, dvm_ref sdf, dvm_ref object,
                          dvm_ref *text)
{
   union dvm_value a = {.l = object}, r = {0};
   if (!simple_date_format_call(v, "java/text/SimpleDateFormat", "format",
                                "(Ljava/lang/Object;)Ljava/lang/String;",
                                sdf, &a, 1, &r))
      return false;
   if (dvm_exception(v) || !r.l) return false;
   *text = r.l;
   return true;
}

static bool text_equals(struct dvm *v, dvm_ref ref, const char *want)
{
   const char *s = dvm_string_utf8(v, ref);
   size_t n = dvm_string_utf8_length(v, ref);
   return s && strlen(s) == n && !strcmp(s, want);
}

bool simple_date_format_selftest(struct dvm *v)
{
   int checks = 0, failures = 0;
   if (!v) return false;

#define CHECK(expr, label) do { \
   ++checks; \
   if (!(expr)) { ++failures; \
      sdk_trace_fprintf(stderr, "SDF_SELFTEST FAIL %s line=%d\n", \
                        (label), __LINE__); \
   } \
} while (0)

   /* Fixed instant: 2000-02-29T00:00:00Z.  The expected civil fields are an
    * independent Java-semantics oracle: Unix epoch seconds are converted by
    * localtime_r in the test, not read back from the formatter. */
   const int64_t fixed_ms = 951782400000LL;
   struct tm tm;
   int ms_part = (int)(fixed_ms % 1000);
   time_t fixed_sec = (time_t)(fixed_ms / 1000);
   bool have_fields = localtime_r(&fixed_sec, &tm) != NULL;
   CHECK(have_fields, "host_fields");
   if (!have_fields) {
      sdk_trace_fprintf(stderr, "SDF_SELFTEST FAIL host local time unavailable\n");
      return false;
   }

   char expected_date[32];
   snprintf(expected_date, sizeof expected_date, "%04d-%02d-%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
   char expected_full[96];
   snprintf(expected_full, sizeof expected_full,
            "%04d-%02d-%02d %02d:%02d:%02d.%03d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ms_part);
   char expected_literal[128];
   snprintf(expected_literal, sizeof expected_literal,
            "%04d年%02d月%02d日",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

   dvm_ref date = new_instance(v, "java/util/Date");
   CHECK(date != 0, "date_alloc");
   union dvm_value a = {.j = fixed_ms}, r = {0};
   CHECK(date != 0 && date_runtime_call(v, "java/util/Date", "<init>", "(J)V",
                                        date, &a, 1, &r) && !dvm_exception(v),
         "date_ctor");

   dvm_ref job_pattern = dvm_new_string(v, "yyyy-MM-dd");
   CHECK(job_pattern != 0, "pattern_alloc_job");
   dvm_ref sdf_job = 0;
   CHECK(make_sdf(v, job_pattern, 0, &sdf_job), "sdf_ctor_string_job");
   if (sdf_job) {
      dvm_ref text = 0;
      CHECK(format_date(v, sdf_job, date, &text), "sdf_format_date_job");
      if (text) CHECK(text_equals(v, text, expected_date), "sdf_value_date_job");
      dvm_clear_exception(v);
   }

   /* The JobManager DEX path is format(Object) with Long.valueOf(ms). */
   dvm_ref boxed = 0;
   union dvm_value la = {.j = fixed_ms}, lr = {0};
   CHECK(java_basics_call(v, "java/lang/Long", "valueOf", "(J)Ljava/lang/Long;",
                          0, &la, 1, &lr) && lr.l, "long_box");
   boxed = lr.l;
   if (sdf_job && boxed) {
      dvm_ref text = 0;
      CHECK(format_object(v, sdf_job, boxed, &text), "sdf_format_object_job");
      if (text) CHECK(text_equals(v, text, expected_date), "sdf_value_object_job");
      dvm_clear_exception(v);
   }

   dvm_ref full_pattern = dvm_new_string(v, "yyyy-MM-dd HH:mm:ss.SSS");
   CHECK(full_pattern != 0, "pattern_alloc_full");
   dvm_ref sdf_full = 0;
   CHECK(make_sdf(v, full_pattern, 0, &sdf_full), "sdf_ctor_full");
   if (sdf_full) {
      dvm_ref text = 0;
      CHECK(format_date(v, sdf_full, date, &text), "sdf_format_full");
      if (text) CHECK(text_equals(v, text, expected_full), "sdf_value_full");
      dvm_clear_exception(v);
   }

   /* RFC 822 zone output, using the host's real tm_gmtoff as the oracle.
    * The production path derives the same offset from civil fields. */
#ifdef __tm_gmtoff
   if (tm.tm_gmtoff % 60 == 0) {
      long off = tm.tm_gmtoff;
      unsigned long a = (unsigned long)(off < 0 ? -off : off);
      char expected_zone[128];
      snprintf(expected_zone, sizeof expected_zone,
               "%s%c%02lu%02lu", expected_full,
               off < 0 ? '-' : '+', a / 3600u, (a % 3600u) / 60u);
      dvm_ref zone_pattern = dvm_new_string(v, "yyyy-MM-dd HH:mm:ss.SSSZ");
      dvm_ref sdf_zone = 0;
      CHECK(zone_pattern != 0 && make_sdf(v, zone_pattern, 0, &sdf_zone),
            "sdf_ctor_zone");
      if (sdf_zone) {
         dvm_ref text = 0;
         CHECK(format_date(v, sdf_zone, date, &text), "sdf_format_zone");
         if (text) CHECK(text_equals(v, text, expected_zone), "sdf_value_zone");
         dvm_clear_exception(v);
      }
   }
#endif

   dvm_ref literal_pattern = dvm_new_string(v, "yyyy'年'MM'月'dd'日'");
   CHECK(literal_pattern != 0, "pattern_alloc_literal");
   dvm_ref sdf_literal = 0;
   CHECK(make_sdf(v, literal_pattern, 0, &sdf_literal), "sdf_ctor_literal");
   if (sdf_literal) {
      dvm_ref text = 0;
      CHECK(format_date(v, sdf_literal, date, &text), "sdf_format_literal");
      if (text) CHECK(text_equals(v, text, expected_literal), "sdf_value_literal");
      dvm_clear_exception(v);
   }

   /* (String, Locale) constructor, the exact 72p signature. */
   dvm_ref locale = new_instance(v, "java/util/Locale");
   CHECK(locale != 0, "locale_alloc");
   dvm_ref sdf_locale = 0;
   CHECK(locale != 0 && make_sdf(v, job_pattern, locale, &sdf_locale),
         "sdf_ctor_locale");
   if (sdf_locale) {
      dvm_ref text = 0;
      CHECK(format_date(v, sdf_locale, date, &text), "sdf_format_locale");
      if (text) CHECK(text_equals(v, text, expected_date), "sdf_value_locale");
      dvm_clear_exception(v);
   }

   /* Fail closed: valid Java text letters are not implemented, and an
    * unterminated quote is an illegal pattern. */
   dvm_ref unsupported = dvm_new_string(v, "EEE");
   dvm_ref dummy = 0;
   CHECK(unsupported != 0 && !make_sdf(v, unsupported, 0, &dummy) &&
         exception_is(v, "java/lang/UnsupportedOperationException"),
         "unsupported_letter");
   dvm_clear_exception(v);
   dvm_ref unterminated = dvm_new_string(v, "yyyy'");
   dummy = 0;
   CHECK(unterminated != 0 && !make_sdf(v, unterminated, 0, &dummy) &&
         exception_is(v, "java/lang/IllegalArgumentException"),
         "unterminated_quote");
   dvm_clear_exception(v);

   dvm_ref plain = dvm_new_string(v, "not a date");
   if (sdf_job && plain) {
      union dvm_value oa = {.l = plain}, orv = {0};
      CHECK(simple_date_format_call(v, "java/text/SimpleDateFormat", "format",
                                    "(Ljava/lang/Object;)Ljava/lang/String;",
                                    sdf_job, &oa, 1, &orv) &&
            exception_is(v, "java/lang/IllegalArgumentException"),
            "format_object_reject");
      dvm_clear_exception(v);
   }

   CHECK(failures == 0, "aggregate");
   sdk_trace_fprintf(stderr, "SDF_SELFTEST %s checks=%d failures=%d\n",
                     failures ? "FAIL" : "PASS", checks, failures);
   return failures == 0;
#undef CHECK
}
