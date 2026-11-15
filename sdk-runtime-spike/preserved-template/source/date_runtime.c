#include "date_runtime.h"
#include "simple_date_format.h"
#include "java_basics.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define DATE_MAGIC 0x44544131u /* "DTA1" */

struct date_value {
   uint32_t magic;
   int64_t millis;
};

static void date_value_destroy(void *p)
{
   struct date_value *d = p;
   if (d) {
      d->magic = 0;
      free(d);
   }
}

static struct date_value *date_value_of(struct dvm *v, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(v, ref);
   if (!o || o->data_destroy != date_value_destroy) return NULL;
   struct date_value *d = o->data;
   return d && d->magic == DATE_MAGIC ? d : NULL;
}

bool date_runtime_is_date(struct dvm *v, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(v, ref);
   return o && o->cls && o->cls->name &&
          !strcmp(o->cls->name, "java/util/Date");
}

bool date_runtime_get_millis(struct dvm *v, dvm_ref ref, int64_t *millis)
{
   struct date_value *d = date_value_of(v, ref);
   if (!d) return false;
   if (millis) *millis = d->millis;
   return true;
}

static bool date_attach(struct dvm *v, dvm_ref self, int64_t millis)
{
   struct dvm_object *o = dvm__obj(v, self);
   if (!o) {
      dvm__throw(v, "java/lang/NullPointerException", "Date receiver");
      return false;
   }
   if (o->data_destroy == date_value_destroy) {
      struct date_value *old = o->data;
      if (old) {
         old->magic = 0;
         free(old);
      }
      o->data = NULL;
      o->data_destroy = NULL;
   }
   struct date_value *d = calloc(1, sizeof *d);
   if (!d) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "Date payload");
      return false;
   }
   d->magic = DATE_MAGIC;
   d->millis = millis;
   o->data = d;
   o->data_destroy = date_value_destroy;
   o->data_owner = NULL;
   return true;
}

/* Days since 1970-01-01 for a proleptic Gregorian civil date. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
   y -= m <= 2;
   int64_t era = (y >= 0 ? y : y - 399) / 400;
   unsigned yoe = (unsigned)(y - era * 400);
   unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
   unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
   return era * 146097 + (int64_t)doe - 719468;
}

bool date_runtime_host_fields(int64_t millis, struct tm *tm,
                              int64_t *offset_seconds, int *millis_part)
{
   int64_t sec = millis / 1000;
   int64_t rem = millis % 1000;
   if (rem < 0) {
      rem += 1000;
      sec -= 1;
   }
   time_t t = (time_t)sec;
   if ((int64_t)t != sec) return false;
   struct tm tmp;
   if (!localtime_r(&t, &tmp)) return false;
   int64_t year = (int64_t)tmp.tm_year + 1900;
   if (year < 1 || year > 9999) return false;
   if (tmp.tm_mon < 0 || tmp.tm_mon > 11 || tmp.tm_mday < 1 || tmp.tm_mday > 31)
      return false;
   int64_t days = days_from_civil(year, (unsigned)tmp.tm_mon + 1u,
                                  (unsigned)tmp.tm_mday);
   int64_t local_sec = days * 86400 + (int64_t)tmp.tm_hour * 3600 +
                       (int64_t)tmp.tm_min * 60 + (int64_t)tmp.tm_sec;
   int64_t off = local_sec - sec;
   /* Real zones are within +/- 24h; a larger value means the host answer is
    * not a usable time-zone fact. */
   if (off < -86400 || off > 86400) return false;
   if (tm) *tm = tmp;
   if (offset_seconds) *offset_seconds = off;
   if (millis_part) *millis_part = (int)rem;
   return true;
}

static bool date_tostring(struct dvm *v, dvm_ref self, union dvm_value *out)
{
   static const char *const wday[] = {
      "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
   };
   static const char *const mon[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
   };
   int64_t millis;
   struct tm tm;
   int64_t off = 0;
   int ms = 0;
   if (!date_runtime_get_millis(v, self, &millis)) {
      dvm__throw(v, "java/lang/IllegalStateException", "Date not initialized");
      return true;
   }
   if (!date_runtime_host_fields(millis, &tm, &off, &ms)) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "host local time unavailable");
      return true;
   }
   if (tm.tm_wday < 0 || tm.tm_wday > 6) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "host returned an invalid weekday");
      return true;
   }
   char zone[64];
   const char *zonename = NULL;
#ifdef __tm_zone
   if (tm.tm_zone && tm.tm_zone[0]) {
      snprintf(zone, sizeof zone, "%s", tm.tm_zone);
      zonename = zone;
   }
#endif
   if (!zonename) {
      int64_t a = off < 0 ? -off : off;
      int hh = (int)(a / 3600), mm = (int)((a % 3600) / 60);
      if (mm || (a % 60))
         snprintf(zone, sizeof zone, "GMT%c%02d:%02d",
                  off < 0 ? '-' : '+', hh, mm);
      else
         snprintf(zone, sizeof zone, "GMT%c%02d", off < 0 ? '-' : '+', hh);
      zonename = zone;
   }
   char text[160];
   int n = snprintf(text, sizeof text, "%s %s %02d %02d:%02d:%02d %s %d",
                    wday[tm.tm_wday], mon[tm.tm_mon], tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec, zonename,
                    tm.tm_year + 1900);
   if (n < 0 || (size_t)n >= sizeof text) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "Date.toString output unavailable");
      return true;
   }
   out->l = dvm_new_string_n(v, text, (size_t)n);
   if (!out->l)
      dvm__throw(v, "java/lang/OutOfMemoryError", "Date.toString");
   return true;
}

#define IS(name, signature) (!strcmp(m, (name)) && !strcmp(sig, (signature)))

bool date_runtime_call(struct dvm *v, const char *c, const char *m,
                       const char *sig, dvm_ref self,
                       const union dvm_value *a, int nargs,
                       union dvm_value *out)
{
   if (strcmp(c, "java/util/Date")) return simple_date_format_call(
      v, c, m, sig, self, a, nargs, out);

   if (IS("<init>", "()V")) {
      if (nargs != 0) return false;
      struct timespec now;
      if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "host wall clock unavailable");
         return true;
      }
      int64_t sec = (int64_t)now.tv_sec;
      if (sec > (INT64_MAX - 999) / 1000) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "host wall clock out of range");
         return true;
      }
      int64_t millis = sec * 1000 + (int64_t)(now.tv_nsec / 1000000);
      if (!date_attach(v, self, millis)) return true;
      return true;
   }
   if (IS("<init>", "(J)V")) {
      if (nargs != 1) return false;
      if (!date_attach(v, self, a[0].j)) return true;
      return true;
   }
   if (IS("getTime", "()J")) {
      if (nargs != 0) return false;
      int64_t millis;
      if (!date_runtime_get_millis(v, self, &millis)) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "Date not initialized");
         return true;
      }
      out->j = millis;
      return true;
   }
   if (IS("setTime", "(J)V")) {
      if (nargs != 1) return false;
      struct dvm_object *o = dvm__obj(v, self);
      struct date_value *d = date_value_of(v, self);
      if (!o) {
         dvm__throw(v, "java/lang/NullPointerException", "Date receiver");
         return true;
      }
      if (!d) {
         /* A constructor is the only Java way to reach a Date, so this is
          * an uninitialized VM object rather than a valid Date value. */
         if (!date_attach(v, self, a[0].j)) return true;
         return true;
      }
      d->millis = a[0].j;
      return true;
   }
   if (IS("before", "(Ljava/util/Date;)Z") ||
       IS("after", "(Ljava/util/Date;)Z")) {
      if (nargs != 1) return false;
      int64_t mine;
      if (!date_runtime_get_millis(v, self, &mine)) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "Date not initialized");
         return true;
      }
      if (!a[0].l) {
         dvm__throw(v, "java/lang/NullPointerException", "Date argument");
         return true;
      }
      int64_t other;
      if (!date_runtime_get_millis(v, a[0].l, &other)) {
         dvm__throw(v, "java/lang/ClassCastException", "not a Date");
         return true;
      }
      out->i = IS("before", "(Ljava/util/Date;)Z") ? mine < other : mine > other;
      return true;
   }
   if (IS("equals", "(Ljava/lang/Object;)Z")) {
      if (nargs != 1) return false;
      int64_t mine;
      if (!date_runtime_get_millis(v, self, &mine)) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "Date not initialized");
         return true;
      }
      int64_t other;
      out->i = date_runtime_get_millis(v, a[0].l, &other) && mine == other;
      return true;
   }
   if (IS("hashCode", "()I")) {
      if (nargs != 0) return false;
      int64_t millis;
      if (!date_runtime_get_millis(v, self, &millis)) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "Date not initialized");
         return true;
      }
      uint64_t u = (uint64_t)millis;
      out->i = (int32_t)(uint32_t)(u ^ (u >> 32));
      return true;
   }
   if (IS("toString", "()Ljava/lang/String;")) {
      if (nargs != 0) return false;
      return date_tostring(v, self, out);
   }
   return false;
}

#undef IS

/* ---------------------------------------------------------------------- */
/* selftest                                                               */

static bool exception_is(struct dvm *v, const char *name)
{
   dvm_ref e = dvm_exception(v);
   struct dvm_object *o = dvm__obj(v, e);
   return o && o->cls && o->cls->name && !strcmp(o->cls->name, name);
}

static dvm_ref new_instance(struct dvm *v, const char *name)
{
   struct dvm_class *cls = dvm_find_class_or_external(v, name);
   return dvm_new_object(v, cls);
}

static bool call_date(struct dvm *v, const char *method, const char *sig,
                      dvm_ref self, const union dvm_value *a, int nargs,
                      union dvm_value *out)
{
   return date_runtime_call(v, "java/util/Date", method, sig, self,
                            a, nargs, out);
}

static bool call_sdf(struct dvm *v, const char *method, const char *sig,
                     dvm_ref self, const union dvm_value *a, int nargs,
                     union dvm_value *out)
{
   return date_runtime_call(v, "java/text/SimpleDateFormat", method, sig,
                            self, a, nargs, out);
}

bool date_runtime_selftest(struct dvm *v)
{
   int checks = 0, failures = 0;
   union dvm_value out = {0}, arg = {0};
   if (!v) {
      sdk_trace_fprintf(stderr, "DATE_RUNTIME_SELFTEST FAIL null_vm\n");
      return false;
   }
   sdk_trace_fprintf(stderr, "DATE_RUNTIME_SELFTEST BEGIN\n");

#define CHECK(expr, label) do { \
   ++checks; \
   if (!(expr)) { ++failures; \
      sdk_trace_fprintf(stderr, "DATE_RUNTIME_SELFTEST FAIL %s line=%d\n", \
                        (label), __LINE__); \
   } \
} while (0)

   dvm_ref d1 = new_instance(v, "java/util/Date");
   CHECK(d1 != 0, "date_alloc");
   if (d1) {
      CHECK(call_date(v, "<init>", "()V", d1, NULL, 0, &out) &&
            !dvm_exception(v), "date_ctor_now");
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
      CHECK(call_date(v, "getTime", "()J", d1, NULL, 0, &out) &&
            out.j >= now - 2000 && out.j <= now + 2000, "date_getTime_now");
      arg.j = 951782400000LL; /* 2000-02-29T00:00:00Z */
      CHECK(call_date(v, "setTime", "(J)V", d1, &arg, 1, &out) &&
            !dvm_exception(v), "date_setTime");
      CHECK(call_date(v, "getTime", "()J", d1, NULL, 0, &out) &&
            out.j == arg.j, "date_getTime_fixed");
   }

   dvm_ref d2 = new_instance(v, "java/util/Date");
   CHECK(d2 != 0, "date_alloc2");
   if (d2) {
      arg.j = 951782399999LL;
      CHECK(call_date(v, "<init>", "(J)V", d2, &arg, 1, &out) &&
            !dvm_exception(v), "date_ctor_long");
      CHECK(call_date(v, "before", "(Ljava/util/Date;)Z", d2, &(union dvm_value){.l = d1}, 1, &out) &&
            out.i == 1, "date_before");
      CHECK(call_date(v, "after", "(Ljava/util/Date;)Z", d2, &(union dvm_value){.l = d1}, 1, &out) &&
            out.i == 0, "date_after");
      CHECK(call_date(v, "equals", "(Ljava/lang/Object;)Z", d2, &(union dvm_value){.l = d1}, 1, &out) &&
            out.i == 0, "date_equals_false");
      CHECK(call_date(v, "equals", "(Ljava/lang/Object;)Z", d1, &(union dvm_value){.l = d1}, 1, &out) &&
            out.i == 1, "date_equals_true");
      int64_t m = 951782400000LL;
      uint64_t u = (uint64_t)m;
      CHECK(call_date(v, "hashCode", "()I", d1, NULL, 0, &out) &&
            (uint32_t)out.i == (uint32_t)(u ^ (u >> 32)), "date_hashCode");
      CHECK(call_date(v, "toString", "()Ljava/lang/String;", d1, NULL, 0, &out) &&
            out.l && dvm_string_utf8(v, out.l) && dvm_string_utf8_length(v, out.l) > 20,
            "date_toString");
   }

   if (!simple_date_format_selftest(v)) ++failures;

   CHECK(failures == 0, "aggregate");
   sdk_trace_fprintf(stderr, "DATE_RUNTIME_SELFTEST %s checks=%d failures=%d\n",
                     failures ? "FAIL" : "PASS", checks, failures);
   return failures == 0;
#undef CHECK
}
