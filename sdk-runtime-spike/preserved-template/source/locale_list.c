/* Real android.os.LocaleList / java.util.Locale subset for the DVM platform
 * runtime.  Locale objects carry a host-side payload; LocaleList objects cache
 * real java.util.Locale entries and the canonical language-tag/display forms.
 * Nothing here claims a device locale unless locale_runtime_set_system_locales
 * was called by the host bridge. */
#include "locale_list.h"
#include "platform_runtime.h"
#include "java_basics.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define LOCALE_MAX_HOST 32
#define LOCALE_TAG_CAP 64
#define LOCALE_DISPLAY_CAP 96
#define LOCALE_HOST_CSV_CAP 2048
#define LOCALE_OBJ_MAGIC 0x4c4f4341u
#define LOCALE_LIST_MAGIC 0x4c4f534cu

typedef struct {
 char language[12];
 char script[8];
 char region[8];
 char variant[48];
} locale_fields;

typedef struct {
 uint32_t magic;
 locale_fields fields;
 char tag[LOCALE_TAG_CAP];
 char display[LOCALE_DISPLAY_CAP];
 int32_t hash;
} locale_data;

typedef struct {
 uint32_t magic;
 dvm_ref *locales;
 uint32_t count;
 uint64_t generation;
 bool explicit_snapshot;
 int32_t hash;
 char *tags;
 char *display;
} locale_list_data;

typedef struct {
 bool present;
 uint32_t count;
 char tags[LOCALE_MAX_HOST][LOCALE_TAG_CAP];
 uint64_t generation;
} locale_host_state;

static pthread_mutex_t locale_host_lock = PTHREAD_MUTEX_INITIALIZER;
static locale_host_state locale_host_values;
static dvm_ref locale_default_ref;
static dvm_ref locale_list_default_ref;
static dvm_ref locale_list_adjusted_ref;

static bool locale_alpha(const char *s, size_t n, size_t min, size_t max) {
 if (n < min || n > max) return false;
 for (size_t i = 0; i < n; i++) {
  char c = s[i];
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
 }
 return true;
}
static bool locale_digit(char c) { return c >= '0' && c <= '9'; }
static bool locale_alnum(char c) {
 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || locale_digit(c);
}
static void locale_lower_ascii(char *s) {
 for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}
static void locale_upper_ascii(char *s) {
 for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}
static bool locale_copy_lower(char *dst, size_t cap, const char *src, size_t n) {
 if (n >= cap) return false;
 memcpy(dst, src, n); dst[n] = '\0'; locale_lower_ascii(dst); return true;
}
static bool locale_copy_upper(char *dst, size_t cap, const char *src, size_t n) {
 if (n >= cap) return false;
 memcpy(dst, src, n); dst[n] = '\0'; locale_upper_ascii(dst); return true;
}
static bool locale_copy_title(char *dst, size_t cap, const char *src, size_t n) {
 if (n >= cap) return false;
 memcpy(dst, src, n); dst[n] = '\0';
 if (dst[0] >= 'a' && dst[0] <= 'z') dst[0] = (char)(dst[0] - 'a' + 'A');
 for (size_t i = 1; i < n; i++) if (dst[i] >= 'A' && dst[i] <= 'Z') dst[i] = (char)(dst[i] - 'A' + 'a');
 return true;
}
static bool locale_append(char *dst, size_t cap, const char *text) {
 size_t used = strlen(dst), add = strlen(text);
 if (used + add >= cap) return false;
 memcpy(dst + used, text, add + 1); return true;
}
static void locale_fields_normalize(locale_fields *f) {
 locale_lower_ascii(f->language);
 locale_lower_ascii(f->script);
 if (f->script[0] >= 'a' && f->script[0] <= 'z') f->script[0] = (char)(f->script[0] - 'a' + 'A');
 locale_upper_ascii(f->region);
}
static bool locale_fields_equal(const locale_fields *a, const locale_fields *b) {
 return !strcmp(a->language, b->language) && !strcmp(a->script, b->script) &&
        !strcmp(a->region, b->region) && !strcmp(a->variant, b->variant);
}
static bool locale_fields_tag(const locale_fields *f, char *out, size_t cap) {
 out[0] = '\0';
 if (!f->language[0]) return true;
 if (!locale_append(out, cap, f->language)) return false;
 if (f->script[0]) {
  if (!locale_append(out, cap, "-") || !locale_append(out, cap, f->script)) return false;
 }
 if (f->region[0]) {
  if (!locale_append(out, cap, "-") || !locale_append(out, cap, f->region)) return false;
 }
 if (f->variant[0]) {
  if (!locale_append(out, cap, "-") || !locale_append(out, cap, f->variant)) return false;
 }
 return true;
}
/* Locale.toLanguageTag() returns "und" when the language is empty. */
static bool locale_fields_language_tag(const locale_fields *f, char *out, size_t cap) {
 if (!f->language[0]) {
  if (cap < 4) return false;
  memcpy(out, "und", 4); return true;
 }
 return locale_fields_tag(f, out, cap);
}
/* Android 7+ Locale.toString(): language[_region][_variant][_#script]. */
static bool locale_fields_display(const locale_fields *f, char *out, size_t cap) {
 out[0] = '\0';
 if (!f->language[0]) return true;
 if (!locale_append(out, cap, f->language)) return false;
 if (f->region[0]) {
  if (!locale_append(out, cap, "_") || !locale_append(out, cap, f->region)) return false;
 }
 if (f->variant[0]) {
  if (!locale_append(out, cap, "_") || !locale_append(out, cap, f->variant)) return false;
 }
 if (f->script[0]) {
  if (!locale_append(out, cap, "_#") || !locale_append(out, cap, f->script)) return false;
 }
 return true;
}
static bool locale_parse_tag(const char *text, locale_fields *out) {
 if (!text) return false;
 size_t len = strlen(text);
 if (len == 0 || len >= LOCALE_TAG_CAP) return false;
 for (size_t i = 0; i < len; i++) {
  char c = text[i];
  if (!(locale_alnum(c) || c == '-' || c == '_')) return false;
 }
 char work[LOCALE_TAG_CAP];
 memcpy(work, text, len + 1);
 char *parts[8];
 int np = 0;
 parts[np++] = work;
 for (char *p = work; *p; p++) {
  if (*p == '-' || *p == '_') {
   if (np >= 8) return false;
   *p = '\0'; parts[np++] = p + 1;
  }
 }
 for (int i = 0; i < np; i++) if (!parts[i][0]) return false;
 memset(out, 0, sizeof *out);
 size_t n = strlen(parts[0]);
 if (n == 3 && (parts[0][0] == 'u' || parts[0][0] == 'U') &&
     (parts[0][1] == 'n' || parts[0][1] == 'N') &&
     (parts[0][2] == 'd' || parts[0][2] == 'D')) {
  /* "und" is the well-formed undetermined language tag. */
 } else {
  if (!locale_alpha(parts[0], n, 2, 8) ||
      !locale_copy_lower(out->language, sizeof out->language, parts[0], n)) return false;
 }
 int i = 1;
 if (i < np && strlen(parts[i]) == 4 && locale_alpha(parts[i], 4, 4, 4)) {
  if (!locale_copy_title(out->script, sizeof out->script, parts[i], 4)) return false;
  i++;
 }
 if (i < np) {
  size_t rn = strlen(parts[i]);
  bool alpha_region = rn == 2 && locale_alpha(parts[i], rn, 2, 2);
  bool numeric_region = rn == 3 && locale_digit(parts[i][0]) && locale_digit(parts[i][1]) && locale_digit(parts[i][2]);
  if (alpha_region || numeric_region) {
   if (!locale_copy_upper(out->region, sizeof out->region, parts[i], rn)) return false;
   i++;
  }
 }
 while (i < np) {
  size_t vn = strlen(parts[i]);
  bool variant = (vn >= 5 && vn <= 8 && locale_alnum(parts[i][0])) ||
                 (vn == 4 && locale_digit(parts[i][0]));
  if (!variant) return false;
  for (size_t j = 0; j < vn; j++) if (!locale_alnum(parts[i][j])) return false;
  if (out->variant[0] && !locale_append(out->variant, sizeof out->variant, "-")) return false;
  if (!locale_append(out->variant, sizeof out->variant, parts[i])) return false;
  i++;
 }
 locale_fields_normalize(out);
 return true;
}
static void locale_host_read(locale_host_state *out) {
 pthread_mutex_lock(&locale_host_lock);
 *out = locale_host_values;
 pthread_mutex_unlock(&locale_host_lock);
}
static uint64_t locale_host_generation(void) {
 pthread_mutex_lock(&locale_host_lock);
 uint64_t generation = locale_host_values.generation;
 pthread_mutex_unlock(&locale_host_lock);
 return generation;
}
static bool locale_host_same(const locale_host_state *a, const locale_host_state *b) {
 if (a->present != b->present || a->count != b->count) return false;
 for (uint32_t i = 0; i < a->count; i++) if (strcmp(a->tags[i], b->tags[i])) return false;
 return true;
}
__attribute__((visibility("default"))) bool locale_runtime_set_system_locales(const char *tags) {
 locale_host_state next;
 memset(&next, 0, sizeof next);
 if (tags && *tags) {
  char work[LOCALE_HOST_CSV_CAP];
  size_t len = strlen(tags);
  if (len >= sizeof work) return false;
  memcpy(work, tags, len + 1);
  char *p = work;
  while (*p) {
   while (*p == ' ' || *p == '\t') p++;
   if (!*p || *p == ',') return false;
   char *start = p;
   while (*p && *p != ',') p++;
   size_t n = (size_t)(p - start);
   while (n && (start[n - 1] == ' ' || start[n - 1] == '\t')) n--;
   if (!n || n >= LOCALE_TAG_CAP) return false;
   char tag[LOCALE_TAG_CAP];
   memcpy(tag, start, n); tag[n] = '\0';
   locale_fields f;
   if (!locale_parse_tag(tag, &f) || !f.language[0]) return false;
   char canonical[LOCALE_TAG_CAP];
   if (!locale_fields_tag(&f, canonical, sizeof canonical)) return false;
   bool duplicate = false;
   for (uint32_t i = 0; i < next.count; i++) if (!strcmp(next.tags[i], canonical)) { duplicate = true; break; }
   if (!duplicate) {
    if (next.count >= LOCALE_MAX_HOST) return false;
    memcpy(next.tags[next.count], canonical, sizeof canonical);
    next.count++;
   }
   if (*p == ',') {
    p++;
    if (!*p) return false;
   }
  }
  if (!next.count) return false;
  next.present = true;
 }
 pthread_mutex_lock(&locale_host_lock);
 if (locale_host_same(&locale_host_values, &next)) {
  pthread_mutex_unlock(&locale_host_lock);
  return true;
 }
 next.generation = locale_host_values.generation + 1;
 locale_host_values = next;
 pthread_mutex_unlock(&locale_host_lock);
 return true;
}
__attribute__((visibility("default"))) bool locale_runtime_get_system_locales(char *tags, size_t cap) {
 locale_host_state snapshot;
 locale_host_read(&snapshot);
 if (tags && cap) {
  size_t used = 0; tags[0] = '\0';
  for (uint32_t i = 0; i < snapshot.count; i++) {
   const char *text = snapshot.tags[i];
   size_t n = strlen(text);
   if (i) {
    if (used + 1 >= cap) break;
    tags[used++] = ','; tags[used] = '\0';
   }
   size_t room = cap - used - 1;
   if (n > room) n = room;
   memcpy(tags + used, text, n); used += n; tags[used] = '\0';
   if (n < strlen(text)) break;
  }
 }
 return snapshot.present;
}

/* ---- java.util.Locale payload ------------------------------------------- */
static void locale_data_destroy(void *data) { free(data); }
static locale_data *locale_data_of(struct dvm *v, dvm_ref ref) {
 struct dvm_object *o = dvm__obj(v, ref);
 if (!o || !o->cls || !o->cls->name || strcmp(o->cls->name, "java/util/Locale")) return NULL;
 if (!o->data || o->data_destroy != locale_data_destroy) return NULL;
 locale_data *d = o->data;
 return d->magic == LOCALE_OBJ_MAGIC ? d : NULL;
}
static int32_t locale_string_hash(const char *s) {
 int32_t h = 0;
 for (; *s; s++) h = 31 * h + (int32_t)(unsigned char)*s;
 return h;
}
static int32_t locale_fields_hash(const locale_fields *f) {
 int32_t h = 0;
 h = 31 * h + locale_string_hash(f->language);
 h = 31 * h + locale_string_hash(f->script);
 h = 31 * h + locale_string_hash(f->region);
 h = 31 * h + locale_string_hash(f->variant);
 return h;
}
static dvm_ref locale_create(struct dvm *v, const locale_fields *input) {
 struct dvm_class *cls = dvm_find_class_or_external(v, "java/util/Locale");
 if (!cls) { dvm__throw(v, "java/lang/NoClassDefFoundError", "java/util/Locale"); return 0; }
 dvm_ref ref = dvm_new_object(v, cls);
 if (!ref) { dvm__throw(v, "java/lang/OutOfMemoryError", "java.util.Locale"); return 0; }
 locale_data *d = calloc(1, sizeof *d);
 if (!d) { dvm__discard_temporary(v, ref); dvm__throw(v, "java/lang/OutOfMemoryError", "java.util.Locale"); return 0; }
 d->magic = LOCALE_OBJ_MAGIC;
 d->fields = *input;
 locale_fields_normalize(&d->fields);
 if (!locale_fields_tag(&d->fields, d->tag, sizeof d->tag) ||
     !locale_fields_display(&d->fields, d->display, sizeof d->display)) {
  free(d); dvm__discard_temporary(v, ref);
  dvm__throw(v, "java/lang/IllegalArgumentException", "Locale fields too long"); return 0;
 }
 d->hash = locale_fields_hash(&d->fields);
 struct dvm_object *o = dvm__obj(v, ref);
 if (!o) { free(d); dvm__discard_temporary(v, ref); dvm__throw(v, "java/lang/IllegalStateException", "Locale object unavailable"); return 0; }
 o->data = d; o->data_destroy = locale_data_destroy;
 return ref;
}
/* 1 = fields copied, 0 = object is not a Locale this runtime understands,
 * -1 = a recognition call failed and left an exception. */
static int locale_fields_lookup(struct dvm *v, dvm_ref ref, locale_fields *out) {
 struct dvm_object *o = dvm__obj(v, ref);
 if (!o || !o->cls || !o->cls->name || strcmp(o->cls->name, "java/util/Locale")) return 0;
 locale_data *d = locale_data_of(v, ref);
 if (d) { *out = d->fields; return 1; }
 /* java_basics owns Locale.US/ROOT/ENGLISH; read their public language and
  * country instead of inventing script/variant values for them. */
 union dvm_value language = {0}, country = {0};
 if (!java_basics_call(v, "java/util/Locale", "getLanguage", "()Ljava/lang/String;", ref, NULL, 0, &language)) {
  return dvm_exception(v) ? -1 : 0;
 }
 if (!language.l) { dvm__throw(v, "java/lang/IllegalStateException", "Locale.getLanguage returned null"); return -1; }
 if (!java_basics_call(v, "java/util/Locale", "getCountry", "()Ljava/lang/String;", ref, NULL, 0, &country)) {
  if (!dvm_exception(v)) dvm__throw(v, "java/lang/IllegalStateException", "Locale.getCountry unavailable");
  return -1;
 }
 if (!country.l) { dvm__throw(v, "java/lang/IllegalStateException", "Locale.getCountry returned null"); return -1; }
 const char *ls = dvm_string_utf8(v, language.l), *cs = dvm_string_utf8(v, country.l);
 size_t ll = dvm_string_utf8_length(v, language.l), cl = dvm_string_utf8_length(v, country.l);
 if (!ls || strlen(ls) != ll || !cs || strlen(cs) != cl || ll >= sizeof out->language || cl >= sizeof out->region) {
  dvm__throw(v, "java/lang/IllegalStateException", "Locale fields unavailable");
  return -1;
 }
 memset(out, 0, sizeof *out);
 memcpy(out->language, ls, ll + 1); memcpy(out->region, cs, cl + 1);
 locale_fields_normalize(out);
 return 1;
}
static bool locale_out_string(struct dvm *v, const char *text, union dvm_value *out) {
 dvm_ref value = dvm_new_string(v, text);
 if (!value) { dvm__throw(v, "java/lang/OutOfMemoryError", "Locale string"); return true; }
 out->l = value; return true;
}
/* ---- android.os.LocaleList payload -------------------------------------- */
static void locale_list_data_destroy(void *data) {
 locale_list_data *d = data;
 if (!d) return;
 free(d->locales); free(d->tags); free(d->display); free(d);
}
static locale_list_data *locale_list_data_of(struct dvm *v, dvm_ref ref) {
 struct dvm_object *o = dvm__obj(v, ref);
 if (!o || !o->cls || !o->cls->name || strcmp(o->cls->name, "android/os/LocaleList")) return NULL;
 if (!o->data || o->data_destroy != locale_list_data_destroy) return NULL;
 locale_list_data *d = o->data;
 return d->magic == LOCALE_LIST_MAGIC ? d : NULL;
}
static bool locale_list_attach(struct dvm *v, dvm_ref ref, locale_list_data *d) {
 struct dvm_object *o = dvm__obj(v, ref);
 if (!o || !o->cls || !o->cls->name || strcmp(o->cls->name, "android/os/LocaleList")) {
  locale_list_data_destroy(d); return false;
 }
 if (o->data) {
  if (o->data_destroy == locale_list_data_destroy) locale_list_data_destroy(o->data);
  else { locale_list_data_destroy(d); return false; }
 }
 o->data = d; o->data_destroy = locale_list_data_destroy;
 return true;
}
static bool locale_list_build_data(struct dvm *v, const dvm_ref *refs, uint32_t n,
                                   bool clone_refs, bool explicit_snapshot,
                                   uint64_t generation, locale_list_data **out) {
 locale_list_data *d = NULL;
 dvm_ref *kept = NULL;
 locale_fields *fields = NULL;
 dvm_ref *owned = NULL;
 uint32_t owned_count = 0;
 kept = calloc(n ? n : 1, sizeof *kept);
 fields = calloc(n ? n : 1, sizeof *fields);
 if (!kept || !fields) {
  dvm__throw(v, "java/lang/OutOfMemoryError", "LocaleList entries"); goto fail;
 }
 uint32_t count = 0;
 for (uint32_t i = 0; i < n; i++) {
  if (!refs[i]) { dvm__throw(v, "java/lang/NullPointerException", "list[%u] is null", i); goto fail; }
  locale_fields f;
  int status = locale_fields_lookup(v, refs[i], &f);
  if (status < 0) goto fail;
  if (status == 0) {
   dvm__throw(v, "java/lang/UnsupportedOperationException", "LocaleList entry is not a supported java.util.Locale");
   goto fail;
  }
  bool duplicate = false;
  for (uint32_t j = 0; j < count; j++) if (locale_fields_equal(&f, &fields[j])) { duplicate = true; break; }
  if (duplicate) continue;
  if (clone_refs) {
   dvm_ref copy = locale_create(v, &f);
   if (!copy) goto fail;
   dvm_ref *grown = realloc(owned, (size_t)(owned_count + 1) * sizeof *owned);
   if (!grown) { dvm__discard_temporary(v, copy); dvm__throw(v, "java/lang/OutOfMemoryError", "LocaleList entries"); goto fail; }
   owned = grown; owned[owned_count++] = copy; kept[count] = copy;
  } else kept[count] = refs[i];
  fields[count] = f; count++;
 }
 d = calloc(1, sizeof *d);
 if (!d) { dvm__throw(v, "java/lang/OutOfMemoryError", "LocaleList"); goto fail; }
 size_t tag_cap = (size_t)count * (LOCALE_TAG_CAP + 1) + 1;
 size_t display_cap = (size_t)count * (LOCALE_DISPLAY_CAP + 2) + 3;
 d->tags = calloc(tag_cap, 1);
 d->display = calloc(display_cap, 1);
 if (!d->tags || !d->display) { dvm__throw(v, "java/lang/OutOfMemoryError", "LocaleList text"); goto fail; }
 for (uint32_t i = 0; i < count; i++) {
  char tag[LOCALE_TAG_CAP];
  if (!locale_fields_language_tag(&fields[i], tag, sizeof tag) ||
      (i && !locale_append(d->tags, tag_cap, ",")) ||
      !locale_append(d->tags, tag_cap, tag)) {
   dvm__throw(v, "java/lang/IllegalArgumentException", "LocaleList language tag too long"); goto fail;
  }
 }
 locale_append(d->display, display_cap, "[");
 for (uint32_t i = 0; i < count; i++) {
  char shown[LOCALE_DISPLAY_CAP];
  if (!locale_fields_display(&fields[i], shown, sizeof shown)) {
   dvm__throw(v, "java/lang/IllegalArgumentException", "LocaleList display too long"); goto fail;
  }
  if (i) locale_append(d->display, display_cap, ", ");
  locale_append(d->display, display_cap, shown);
 }
 locale_append(d->display, display_cap, "]");
 d->magic = LOCALE_LIST_MAGIC;
 d->locales = kept; kept = NULL;
 d->count = count;
 d->generation = generation;
 d->explicit_snapshot = explicit_snapshot;
 d->hash = 1;
 for (uint32_t i = 0; i < count; i++) d->hash = 31 * d->hash + locale_fields_hash(&fields[i]);
 free(fields); free(owned);
 *out = d;
 return true;
fail:
 free(kept); free(fields); free(owned);
 if (d) locale_list_data_destroy(d);
 return false;
}
static bool locale_list_new(struct dvm *v, const dvm_ref *refs, uint32_t n,
                            bool clone_refs, bool explicit_snapshot,
                            uint64_t generation, dvm_ref *out) {
 locale_list_data *d = NULL;
 if (!locale_list_build_data(v, refs, n, clone_refs, explicit_snapshot, generation, &d)) return false;
 struct dvm_class *cls = dvm_find_class_or_external(v, "android/os/LocaleList");
 if (!cls) { locale_list_data_destroy(d); dvm__throw(v, "java/lang/NoClassDefFoundError", "android/os/LocaleList"); return false; }
 dvm_ref ref = dvm_new_object(v, cls);
 if (!ref) { locale_list_data_destroy(d); dvm__throw(v, "java/lang/OutOfMemoryError", "android.os.LocaleList"); return false; }
 if (!locale_list_attach(v, ref, d)) {
  dvm__discard_temporary(v, ref);
  dvm__throw(v, "java/lang/IllegalStateException", "LocaleList object unavailable"); return false;
 }
 *out = ref; return true;
}
static bool locale_list_clone_new(struct dvm *v, dvm_ref source, bool explicit_snapshot, dvm_ref *out) {
 locale_list_data *d = locale_list_data_of(v, source);
 if (!d) { dvm__throw(v, "java/lang/IllegalStateException", "LocaleList payload unavailable"); return false; }
 return locale_list_new(v, d->locales, d->count, true, explicit_snapshot, d->generation, out);
}
static bool locale_default_get(struct dvm *v, dvm_ref *out) {
 if (!locale_default_ref) {
  /* This is the VM's established runtime default (the previous platform
   * runtime exposed exactly this real Locale identity).  A host snapshot, when
   * present, takes priority in Configuration.getLocales(); this fallback is
   * only used when the host bridge has not provided a locale. */
  locale_fields f;
  memset(&f, 0, sizeof f);
  memcpy(f.language, "en", 3); memcpy(f.region, "US", 3);
  dvm_ref ref = locale_create(v, &f);
  if (!ref) return false;
  dvm_pin(v, ref);
  locale_default_ref = ref;
 }
 *out = locale_default_ref; return true;
}
static bool locale_list_from_default(struct dvm *v, bool explicit_snapshot, dvm_ref *out) {
 dvm_ref def;
 if (!locale_default_get(v, &def)) return false;
 dvm_ref refs[1] = { def };
 return locale_list_new(v, refs, 1, false, explicit_snapshot, 0, out);
}

/* ---- Configuration support ---------------------------------------------- */
static bool locale_config_read_list(struct dvm *v, dvm_ref configuration, dvm_ref *out) {
 union dvm_value value = {0};
 if (!dvm_get_field(v, configuration, "localeList", "Landroid/os/LocaleList;", &value)) {
  dvm__throw(v, "java/lang/IllegalStateException", "Configuration.localeList field unavailable");
  return false;
 }
 *out = value.l; return true;
}
static bool locale_config_write_list(struct dvm *v, dvm_ref configuration, dvm_ref list) {
 union dvm_value value = { .l = list };
 if (!dvm_set_field(v, configuration, "localeList", "Landroid/os/LocaleList;", value)) {
  dvm__throw(v, "java/lang/IllegalStateException", "Configuration.localeList field unavailable");
  return false;
 }
 return true;
}
bool locale_runtime_configuration_locales(struct dvm *v, dvm_ref configuration, dvm_ref *out) {
 dvm_ref current = 0;
 if (!locale_config_read_list(v, configuration, &current)) return false;
 uint64_t generation = locale_host_generation();
 if (current) {
  locale_list_data *d = locale_list_data_of(v, current);
  if (!d) { dvm__throw(v, "java/lang/IllegalStateException", "Configuration.localeList is not a known LocaleList"); return false; }
  if (d->explicit_snapshot || d->generation == generation) { *out = current; return true; }
 }
 locale_host_state host;
 locale_host_read(&host);
 dvm_ref list = 0;
 if (host.present && host.count) {
  dvm_ref refs[LOCALE_MAX_HOST];
  memset(refs, 0, sizeof refs);
  for (uint32_t i = 0; i < host.count; i++) {
   locale_fields f;
   if (!locale_parse_tag(host.tags[i], &f) || !f.language[0]) {
    dvm__throw(v, "java/lang/IllegalStateException", "host locale snapshot corrupted");
    return false;
   }
   refs[i] = locale_create(v, &f);
   if (!refs[i]) return false;
  }
  if (!locale_list_new(v, refs, host.count, false, false, generation, &list)) return false;
 } else {
  dvm_ref def;
  if (!locale_default_get(v, &def)) return false;
  dvm_ref refs[1] = { def };
  /* Stamp the current host generation so a Configuration keeps returning the
   * same LocaleList object even after the host snapshot was cleared. */
  if (!locale_list_new(v, refs, 1, false, false, generation, &list)) return false;
 }
 dvm_pin(v, list);
 if (!locale_config_write_list(v, configuration, list)) return false;
 *out = list; return true;
}
bool locale_runtime_configuration_copy_locales(struct dvm *v, dvm_ref dst, dvm_ref src) {
 dvm_ref source = 0;
 if (!locale_config_read_list(v, src, &source)) return false;
 if (!source) return locale_config_write_list(v, dst, 0);
 dvm_ref copy = 0;
 if (!locale_list_clone_new(v, source, true, &copy)) return false;
 dvm_pin(v, copy);
 return locale_config_write_list(v, dst, copy);
}
bool locale_runtime_configuration_clear_locales(struct dvm *v, dvm_ref configuration) {
 return locale_config_write_list(v, configuration, 0);
}

/* ---- LocaleList getFirstMatch (AOSP-compatible subset) ------------------- */
static bool locale_fields_matches(const locale_fields *supported, const locale_fields *desired) {
 if (locale_fields_equal(supported, desired)) return true;
 if (strcmp(supported->language, desired->language)) return false;
 if (!supported->language[0]) return false;
 bool supported_pseudo = !strcmp(supported->language, "zh") || !strcmp(supported->language, "ja") || !strcmp(supported->language, "ko");
 bool desired_pseudo = !strcmp(desired->language, "zh") || !strcmp(desired->language, "ja") || !strcmp(desired->language, "ko");
 if (!supported_pseudo || !desired_pseudo) return false;
 const char *supported_script = supported->script;
 char inferred[8] = {0};
 if (!supported_script[0] && !strcmp(supported->language, "zh")) {
  if (!strcmp(supported->region, "TW") || !strcmp(supported->region, "HK") || !strcmp(supported->region, "MO"))
   memcpy(inferred, "Hant", 5);
  else if (!strcmp(supported->region, "CN") || !strcmp(supported->region, "SG"))
   memcpy(inferred, "Hans", 5);
  supported_script = inferred;
 }
 if (supported_script[0] && desired->script[0] && strcmp(supported_script, desired->script)) return false;
 if (!supported->region[0] || !strcmp(supported->region, desired->region)) return true;
 return false;
}
static bool locale_list_throw_oob(struct dvm *v, int32_t index) {
 dvm__throw(v, "java/lang/IndexOutOfBoundsException", "LocaleList.get(%d)", index);
 return true;
}

/* ---- Dispatch ------------------------------------------------------------ */
#define LOCALE_IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
static bool locale_method_receiver(struct dvm *v, dvm_ref self) {
 struct dvm_object *o = dvm__obj(v, self);
 return o && o->cls && o->cls->name && !strcmp(o->cls->name, "java/util/Locale");
}
static bool locale_runtime_locale_call(struct dvm *v, const char *m, const char *sig,
                                       dvm_ref self, const union dvm_value *a, int n,
                                       union dvm_value *out) {
 if (LOCALE_IS("getDefault", "()Ljava/util/Locale;") && n == 0) {
  if (!locale_default_get(v, &out->l)) out->l = 0;
  return true;
 }
 if (LOCALE_IS("getAdjustedDefault", "()Ljava/util/Locale;") && n == 0) {
  if (!locale_default_get(v, &out->l)) out->l = 0;
  return true;
 }
 if (LOCALE_IS("forLanguageTag", "(Ljava/lang/String;)Ljava/util/Locale;") && n == 1) {
  if (!a[0].l) { dvm__throw(v, "java/lang/NullPointerException", "languageTag"); return true; }
  const char *text = dvm_string_utf8(v, a[0].l);
  size_t length = dvm_string_utf8_length(v, a[0].l);
  if (!text || strlen(text) != length) { dvm__throw(v, "java/lang/IllegalArgumentException", "languageTag"); return true; }
  locale_fields f;
  if (!locale_parse_tag(text, &f)) memset(&f, 0, sizeof f); /* Android: invalid tag -> Locale.ROOT */
  dvm_ref ref = locale_create(v, &f);
  if (ref) out->l = ref;
  return true;
 }
 if (LOCALE_IS("setDefault", "(Ljava/util/Locale;)V") && n == 1) {
  if (!a[0].l) { dvm__throw(v, "java/lang/NullPointerException", "locale"); return true; }
  locale_fields f;
  int status = locale_fields_lookup(v, a[0].l, &f);
  if (status < 0) return true;
  if (status == 0) { dvm__throw(v, "java/lang/IllegalArgumentException", "locale"); return true; }
  dvm_pin(v, a[0].l);
  locale_default_ref = a[0].l;
  locale_list_default_ref = 0;
  locale_list_adjusted_ref = 0;
  return true;
 }
 if (!locale_method_receiver(v, self)) return false;
 locale_fields f;
 int status = locale_fields_lookup(v, self, &f);
 if (status < 0) return true;
 if (status == 0) return false;
 if (LOCALE_IS("getLanguage", "()Ljava/lang/String;") && n == 0) return locale_out_string(v, f.language, out);
 if (LOCALE_IS("getCountry", "()Ljava/lang/String;") && n == 0) return locale_out_string(v, f.region, out);
 if (LOCALE_IS("getScript", "()Ljava/lang/String;") && n == 0) return locale_out_string(v, f.script, out);
 if (LOCALE_IS("getVariant", "()Ljava/lang/String;") && n == 0) return locale_out_string(v, f.variant, out);
 if (LOCALE_IS("toString", "()Ljava/lang/String;") && n == 0) {
  char display[LOCALE_DISPLAY_CAP];
  if (!locale_fields_display(&f, display, sizeof display)) { dvm__throw(v, "java/lang/IllegalStateException", "Locale display too long"); return true; }
  return locale_out_string(v, display, out);
 }
 if (LOCALE_IS("toLanguageTag", "()Ljava/lang/String;") && n == 0) {
  char tag[LOCALE_TAG_CAP];
  if (!locale_fields_language_tag(&f, tag, sizeof tag)) { dvm__throw(v, "java/lang/IllegalStateException", "Locale tag too long"); return true; }
  return locale_out_string(v, tag, out);
 }
 if (LOCALE_IS("equals", "(Ljava/lang/Object;)Z") && n == 1) {
  if (!a[0].l) { out->i = 0; return true; }
  if (a[0].l == self) { out->i = 1; return true; }
  locale_fields other;
  int other_status = locale_fields_lookup(v, a[0].l, &other);
  if (other_status < 0) return true;
  out->i = other_status == 1 && locale_fields_equal(&f, &other);
  return true;
 }
 if (LOCALE_IS("hashCode", "()I") && n == 0) { out->i = locale_fields_hash(&f); return true; }
 if (LOCALE_IS("clone", "()Ljava/lang/Object;") && n == 0) {
  dvm_ref copy = locale_create(v, &f);
  if (copy) out->l = copy;
  return true;
 }
 return false;
}
static bool locale_runtime_locale_list_call(struct dvm *v, const char *m, const char *sig,
                                            dvm_ref self, const union dvm_value *a, int n,
                                            union dvm_value *out) {
 if (LOCALE_IS("getDefault", "()Landroid/os/LocaleList;") && n == 0) {
  if (!locale_list_default_ref) {
   dvm_ref list;
   if (!locale_list_from_default(v, true, &list)) return true;
   dvm_pin(v, list); locale_list_default_ref = list;
  }
  out->l = locale_list_default_ref; return true;
 }
 if (LOCALE_IS("getAdjustedDefault", "()Landroid/os/LocaleList;") && n == 0) {
  if (!locale_list_adjusted_ref) {
   dvm_ref list;
   if (!locale_list_from_default(v, true, &list)) return true;
   dvm_pin(v, list); locale_list_adjusted_ref = list;
  }
  out->l = locale_list_adjusted_ref; return true;
 }
 struct dvm_object *self_object = dvm__obj(v, self);
 if (!self_object || !self_object->cls || !self_object->cls->name ||
     strcmp(self_object->cls->name, "android/os/LocaleList")) return false;
 if (LOCALE_IS("<init>", "([Ljava/util/Locale;)V") && n == 1) {
  dvm_ref array = a[0].l;
  if (!array) { dvm__throw(v, "java/lang/NullPointerException", "list"); return true; }
  struct dvm_object *ao = dvm__obj(v, array);
  if (!ao || ao->kind != DVM_OBJ_ARRAY || !ao->cls || !ao->cls->desc || strcmp(ao->cls->desc, "[Ljava/util/Locale;")) {
   dvm__throw(v, "java/lang/IllegalArgumentException", "LocaleList requires Locale[]"); return true;
  }
  const dvm_ref *refs = ao->data;
  locale_list_data *data = NULL;
  if (!locale_list_build_data(v, refs, ao->length, true, false, locale_host_generation(), &data)) return true;
  if (!locale_list_attach(v, self, data)) {
   dvm__throw(v, "java/lang/IllegalStateException", "LocaleList object unavailable");
  }
  return true;
 }
 locale_list_data *d = locale_list_data_of(v, self);
 if (!d) { return false; }
 if (LOCALE_IS("size", "()I") && n == 0) { out->i = (int32_t)d->count; return true; }
 if (LOCALE_IS("isEmpty", "()Z") && n == 0) { out->i = d->count == 0; return true; }
 if (LOCALE_IS("get", "(I)Ljava/util/Locale;") && n == 1) {
  int32_t index = a[0].i;
  if (index < 0 || (uint32_t)index >= d->count) return locale_list_throw_oob(v, index);
  out->l = d->locales[index]; return true;
 }
 if (LOCALE_IS("getFirst", "()Ljava/util/Locale;") && n == 0) {
  out->l = d->count ? d->locales[0] : 0; return true;
 }
 if (LOCALE_IS("toString", "()Ljava/lang/String;") && n == 0) return locale_out_string(v, d->display, out);
 if (LOCALE_IS("toLanguageTags", "()Ljava/lang/String;") && n == 0) return locale_out_string(v, d->tags, out);
 if (LOCALE_IS("hashCode", "()I") && n == 0) { out->i = d->hash; return true; }
 if (LOCALE_IS("equals", "(Ljava/lang/Object;)Z") && n == 1) {
  if (!a[0].l) { out->i = 0; return true; }
  if (a[0].l == self) { out->i = 1; return true; }
  locale_list_data *other = locale_list_data_of(v, a[0].l);
  if (!other || other->count != d->count) { out->i = 0; return true; }
  bool same = true;
  for (uint32_t i = 0; i < d->count; i++) {
   locale_fields left, right;
   int ls = locale_fields_lookup(v, d->locales[i], &left);
   int rs = locale_fields_lookup(v, other->locales[i], &right);
   if (ls < 0 || rs < 0) return true;
   if (ls != 1 || rs != 1 || !locale_fields_equal(&left, &right)) { same = false; break; }
  }
  out->i = same; return true;
 }
 if (LOCALE_IS("indexOf", "(Ljava/util/Locale;)I") && n == 1) {
  if (!a[0].l) { out->i = -1; return true; }
  locale_fields wanted;
  int status = locale_fields_lookup(v, a[0].l, &wanted);
  if (status < 0) return true;
  if (status == 0) { out->i = -1; return true; }
  out->i = -1;
  for (uint32_t i = 0; i < d->count; i++) {
   locale_fields current;
   int current_status = locale_fields_lookup(v, d->locales[i], &current);
   if (current_status < 0) return true;
   if (current_status == 1 && locale_fields_equal(&current, &wanted)) { out->i = (int32_t)i; break; }
  }
  return true;
 }
 if (LOCALE_IS("getFirstMatch", "([Ljava/lang/String;)Ljava/util/Locale;") && n == 1) {
  dvm_ref array = a[0].l;
  if (!array) { dvm__throw(v, "java/lang/NullPointerException", "supportedLocales"); return true; }
  struct dvm_object *ao = dvm__obj(v, array);
  if (!ao || ao->kind != DVM_OBJ_ARRAY || !ao->cls || !ao->cls->desc || strcmp(ao->cls->desc, "[Ljava/lang/String;")) {
   dvm__throw(v, "java/lang/IllegalArgumentException", "getFirstMatch requires String[]"); return true;
  }
  if (!d->count) { out->l = 0; return true; }
  const dvm_ref *supported = ao->data;
  for (uint32_t i = 0; i < ao->length; i++) {
   if (!supported[i]) { dvm__throw(v, "java/lang/NullPointerException", "supportedLocales[%u]", i); return true; }
   const char *text = dvm_string_utf8(v, supported[i]);
   size_t length = dvm_string_utf8_length(v, supported[i]);
   if (!text || strlen(text) != length) { dvm__throw(v, "java/lang/IllegalArgumentException", "supportedLocales[%u]", i); return true; }
   locale_fields desired;
   if (!locale_parse_tag(text, &desired)) memset(&desired, 0, sizeof desired);
   bool matched = false;
   for (uint32_t j = 0; j < d->count; j++) {
    locale_fields candidate;
    int status = locale_fields_lookup(v, d->locales[j], &candidate);
    if (status < 0) return true;
    if (status == 1 && locale_fields_matches(&desired, &candidate)) { out->l = d->locales[j]; matched = true; break; }
   }
   if (matched) return true;
  }
  out->l = d->locales[0]; return true; /* AOSP computeFirstMatch fallback */
 }
 return false;
}
bool locale_runtime_call(struct dvm *v, const char *c, const char *m, const char *sig,
                         dvm_ref self, const union dvm_value *a, int n,
                         union dvm_value *out) {
 if (!strcmp(c, "java/util/Locale")) return locale_runtime_locale_call(v, m, sig, self, a, n, out);
 if (!strcmp(c, "android/os/LocaleList")) return locale_runtime_locale_list_call(v, m, sig, self, a, n, out);
 return false;
}

/* ---- Selftest ------------------------------------------------------------ */
static bool locale_exception_named(struct dvm *v, const char *name) {
 dvm_ref exception = dvm_exception(v);
 struct dvm_object *o = dvm__obj(v, exception);
 if (!o || !o->cls) return false;
 for (struct dvm_class *k = o->cls; k; k = k->super) if (!strcmp(k->name, name)) return true;
 return false;
}
static bool locale_test_fail(struct dvm *v, const char *stage, int line) {
 char why[512];
 if (dvm_exception(v)) {
  dvm_describe_exception(v, dvm_exception(v), why, sizeof why);
  sdk_trace_fprintf(stderr, "LOCALE_SELFTEST FAIL stage=%s line=%d exception=%s\n", stage, line, why);
  dvm_clear_exception(v);
 } else sdk_trace_fprintf(stderr, "LOCALE_SELFTEST FAIL stage=%s line=%d\n", stage, line);
 return false;
}
bool locale_runtime_selftest(struct dvm *v) {
 char saved[LOCALE_HOST_CSV_CAP];
 bool saved_present = locale_runtime_get_system_locales(saved, sizeof saved);
 bool ok = true; const char *stage = NULL; int line = 0;
 dvm_ref list = 0, second = 0, list_from_default = 0, got0 = 0;
 dvm_ref array = 0, empty_array = 0, null_element_array = 0;
 dvm_ref null_locale_array = 0; (void)null_locale_array;
 dvm_ref configuration = 0;
 union dvm_value out = {0};
#define LT(cond,label) do { if (!(cond)) { stage=(label); line=__LINE__; ok=false; goto done; } } while (0)
 if (!locale_runtime_set_system_locales("zh-Hans-CN,en-US")) { stage="set_fixture"; line=__LINE__; ok=false; goto done; }
 locale_fields zh, en, zh_dup;
 memset(&zh, 0, sizeof zh); memcpy(zh.language, "zh", 3); memcpy(zh.script, "Hans", 5); memcpy(zh.region, "CN", 3);
 en = zh; memset(en.script, 0, sizeof en.script); memset(en.region, 0, sizeof en.region); memcpy(en.language, "en", 3); memcpy(en.region, "US", 3);
 zh_dup = zh;
 dvm_ref zhl = locale_create(v, &zh), enl = locale_create(v, &en), zhdupl = locale_create(v, &zh_dup);
 LT(zhl && enl && zhdupl, "locale_create");
 array = dvm_new_array(v, 'L', "Ljava/util/Locale;", 3);
 LT(array, "array_new");
 { dvm_ref *refs = dvm_array_data(v, array); refs[0] = zhl; refs[1] = enl; refs[2] = zhdupl; }
 list = dvm_new_object(v, dvm_find_class_or_external(v, "android/os/LocaleList"));
 LT(list, "list_new");
 { union dvm_value arg = { .l = array }; LT(platform_runtime_call(v, "android/os/LocaleList", "<init>", "([Ljava/util/Locale;)V", list, &arg, 1, &out) && !dvm_exception(v), "list_ctor"); }
 LT(platform_runtime_call(v, "android/os/LocaleList", "size", "()I", list, NULL, 0, &out) && !dvm_exception(v) && out.i == 2, "list_size_dedup");
 LT(platform_runtime_call(v, "android/os/LocaleList", "isEmpty", "()Z", list, NULL, 0, &out) && !dvm_exception(v) && out.i == 0, "list_empty_false");
 /* AOSP clones each Locale in the constructor, so get(0) is a real Locale with
  * the same fields, not the caller's object identity. */
 LT(platform_runtime_call(v, "android/os/LocaleList", "get", "(I)Ljava/util/Locale;", list, (union dvm_value[]){ {.i=0} }, 1, &out) && !dvm_exception(v) && (got0 = out.l) != 0, "list_get0");
 LT(platform_runtime_call(v, "java/util/Locale", "getLanguage", "()Ljava/lang/String;", got0, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), "zh"), "list_language");
 LT(platform_runtime_call(v, "java/util/Locale", "getCountry", "()Ljava/lang/String;", got0, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), "CN"), "list_country");
 LT(platform_runtime_call(v, "java/util/Locale", "getScript", "()Ljava/lang/String;", got0, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), "Hans"), "list_script");
 LT(platform_runtime_call(v, "java/util/Locale", "toLanguageTag", "()Ljava/lang/String;", got0, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), "zh-Hans-CN"), "locale_tag");
 LT(platform_runtime_call(v, "android/os/LocaleList", "toLanguageTags", "()Ljava/lang/String;", list, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), "zh-Hans-CN,en-US"), "list_tags");
 LT(platform_runtime_call(v, "android/os/LocaleList", "toString", "()Ljava/lang/String;", list, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), "[zh_CN_#Hans, en_US]"), "list_display");
 LT(platform_runtime_call(v, "android/os/LocaleList", "getFirst", "()Ljava/util/Locale;", list, NULL, 0, &out) && !dvm_exception(v) && out.l && out.l == got0, "list_get_first");
 LT(platform_runtime_call(v, "android/os/LocaleList", "hashCode", "()I", list, NULL, 0, &out) && !dvm_exception(v), "list_hash");
 int32_t list_hash = out.i;
 second = dvm_new_object(v, dvm_find_class_or_external(v, "android/os/LocaleList"));
 LT(second, "second_list_new");
 { union dvm_value arg = { .l = array }; LT(platform_runtime_call(v, "android/os/LocaleList", "<init>", "([Ljava/util/Locale;)V", second, &arg, 1, &out) && !dvm_exception(v), "second_list_ctor"); }
 { union dvm_value arg = { .l = second }; LT(platform_runtime_call(v, "android/os/LocaleList", "equals", "(Ljava/lang/Object;)Z", list, &arg, 1, &out) && !dvm_exception(v) && out.i == 1, "list_equals"); }
 LT(platform_runtime_call(v, "android/os/LocaleList", "hashCode", "()I", second, NULL, 0, &out) && !dvm_exception(v) && out.i == list_hash, "list_hash_equal");
 { union dvm_value arg = { .i = 2 }; bool rejected = platform_runtime_call(v, "android/os/LocaleList", "get", "(I)Ljava/util/Locale;", list, &arg, 1, &out) && locale_exception_named(v, "java/lang/IndexOutOfBoundsException"); dvm_clear_exception(v); LT(rejected, "list_oob"); }
 { union dvm_value arg = { .l = 0 }; bool rejected = platform_runtime_call(v, "android/os/LocaleList", "<init>", "([Ljava/util/Locale;)V", second, &arg, 1, &out) && locale_exception_named(v, "java/lang/NullPointerException"); dvm_clear_exception(v); LT(rejected, "list_null_array"); }
 null_element_array = dvm_new_array(v, 'L', "Ljava/util/Locale;", 1);
 LT(null_element_array, "null_element_array_new");
 { union dvm_value arg = { .l = null_element_array }; bool rejected = platform_runtime_call(v, "android/os/LocaleList", "<init>", "([Ljava/util/Locale;)V", second, &arg, 1, &out) && locale_exception_named(v, "java/lang/NullPointerException"); dvm_clear_exception(v); LT(rejected, "list_null_element"); }
 empty_array = dvm_new_array(v, 'L', "Ljava/util/Locale;", 0);
 LT(empty_array, "empty_array_new");
 { union dvm_value arg = { .l = empty_array }; LT(platform_runtime_call(v, "android/os/LocaleList", "<init>", "([Ljava/util/Locale;)V", second, &arg, 1, &out) && !dvm_exception(v), "empty_list_ctor"); }
 LT(platform_runtime_call(v, "android/os/LocaleList", "size", "()I", second, NULL, 0, &out) && !dvm_exception(v) && out.i == 0, "empty_list_size");
 LT(platform_runtime_call(v, "android/os/LocaleList", "getFirst", "()Ljava/util/Locale;", second, NULL, 0, &out) && !dvm_exception(v) && out.l == 0, "empty_list_first");
 LT(platform_runtime_call(v, "android/os/LocaleList", "toString", "()Ljava/lang/String;", second, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), "[]"), "empty_list_display");
 LT(platform_runtime_call(v, "android/os/LocaleList", "toLanguageTags", "()Ljava/lang/String;", second, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), ""), "empty_list_tags");
 configuration = dvm_new_object(v, dvm_find_class_or_external(v, "android/content/res/Configuration"));
 LT(configuration, "config_new");
 LT(platform_runtime_call(v, "android/content/res/Configuration", "<init>", "()V", configuration, NULL, 0, &out) && !dvm_exception(v), "config_init");
 LT(platform_runtime_call(v, "android/content/res/Configuration", "getLocales", "()Landroid/os/LocaleList;", configuration, NULL, 0, &out) && !dvm_exception(v) && out.l, "config_locales");
 dvm_ref config_list = out.l;
 LT(platform_runtime_call(v, "android/content/res/Configuration", "getLocales", "()Landroid/os/LocaleList;", configuration, NULL, 0, &out) && !dvm_exception(v) && out.l == config_list, "config_locales_identity");
 LT(platform_runtime_call(v, "android/os/LocaleList", "size", "()I", config_list, NULL, 0, &out) && !dvm_exception(v) && out.i == 2, "config_locales_size");
 LT(platform_runtime_call(v, "android/os/LocaleList", "toLanguageTags", "()Ljava/lang/String;", config_list, NULL, 0, &out) && !dvm_exception(v) && out.l && !strcmp(dvm_string_utf8(v, out.l), "zh-Hans-CN,en-US"), "config_locales_tags");
 LT(platform_runtime_call(v, "android/os/LocaleList", "getDefault", "()Landroid/os/LocaleList;", 0, NULL, 0, &out) && !dvm_exception(v) && out.l, "list_get_default");
 list_from_default = out.l;
 LT(platform_runtime_call(v, "android/os/LocaleList", "size", "()I", list_from_default, NULL, 0, &out) && !dvm_exception(v) && out.i >= 1, "list_default_size");
 LT(platform_runtime_call(v, "android/os/LocaleList", "getAdjustedDefault", "()Landroid/os/LocaleList;", 0, NULL, 0, &out) && !dvm_exception(v) && out.l, "list_adjusted_default");
 LT(locale_runtime_set_system_locales(NULL), "clear_host");
 configuration = dvm_new_object(v, dvm_find_class_or_external(v, "android/content/res/Configuration"));
 LT(configuration, "fallback_config_new");
 LT(platform_runtime_call(v, "android/content/res/Configuration", "<init>", "()V", configuration, NULL, 0, &out) && !dvm_exception(v), "fallback_config_init");
 LT(platform_runtime_call(v, "android/content/res/Configuration", "getLocales", "()Landroid/os/LocaleList;", configuration, NULL, 0, &out) && !dvm_exception(v) && out.l, "fallback_locales");
 dvm_ref fallback_list = out.l;
 LT(platform_runtime_call(v, "android/os/LocaleList", "size", "()I", fallback_list, NULL, 0, &out) && !dvm_exception(v) && out.i == 1, "fallback_size");
 LT(platform_runtime_call(v, "android/os/LocaleList", "get", "(I)Ljava/util/Locale;", fallback_list, (union dvm_value[]){ {.i=0} }, 1, &out) && !dvm_exception(v) && out.l, "fallback_get0");
 dvm_ref fallback_locale = out.l;
 LT(platform_runtime_call(v, "java/util/Locale", "getDefault", "()Ljava/util/Locale;", 0, NULL, 0, &out) && !dvm_exception(v) && out.l, "fallback_default");
 dvm_ref runtime_default = out.l;
 LT(platform_runtime_call(v, "java/util/Locale", "getLanguage", "()Ljava/lang/String;", runtime_default, NULL, 0, &out) && !dvm_exception(v) && out.l, "fallback_default_language");
 dvm_ref default_language = out.l;
 LT(platform_runtime_call(v, "java/util/Locale", "getLanguage", "()Ljava/lang/String;", fallback_locale, NULL, 0, &out) && !dvm_exception(v) && out.l, "fallback_locale_language");
 LT(!strcmp(dvm_string_utf8(v, out.l), dvm_string_utf8(v, default_language)), "fallback_language_equal");
 LT(platform_runtime_call(v, "java/util/Locale", "getCountry", "()Ljava/lang/String;", runtime_default, NULL, 0, &out) && !dvm_exception(v) && out.l, "fallback_default_country");
 dvm_ref default_country = out.l;
 LT(platform_runtime_call(v, "java/util/Locale", "getCountry", "()Ljava/lang/String;", fallback_locale, NULL, 0, &out) && !dvm_exception(v) && out.l, "fallback_locale_country");
 LT(!strcmp(dvm_string_utf8(v, out.l), dvm_string_utf8(v, default_country)), "fallback_country_equal");
 LT(platform_runtime_call(v, "android/content/res/Configuration", "getLocales", "()Landroid/os/LocaleList;", configuration, NULL, 0, &out) && !dvm_exception(v) && out.l == fallback_list, "fallback_identity");
 LT(locale_runtime_set_system_locales("fr-FR"), "set_fr");
 LT(!locale_runtime_set_system_locales("not a tag!"), "reject_invalid");
 { char current[LOCALE_HOST_CSV_CAP]; LT(locale_runtime_get_system_locales(current, sizeof current) && !strcmp(current, "fr-FR"), "preserve_after_invalid"); }
done:
 if (saved_present) locale_runtime_set_system_locales(saved);
 else locale_runtime_set_system_locales(NULL);
 if (!ok) return locale_test_fail(v, stage, line);
 sdk_trace_fprintf(stderr, "LOCALE_SELFTEST PASS host_bridge=1 LocaleList_subset=1 Configuration_locales=1 fallback_default=1 invalid_rejected=1\n");
 return true;
#undef LT
}
bool locale_runtime_vm_start(struct dvm *v) {
 (void)v;
 locale_default_ref = 0;
 locale_list_default_ref = 0;
 locale_list_adjusted_ref = 0;
 return true;
}
