/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The part of the class library the interpreter has to own.
 *
 * An APK ships its own classes, not the platform's, so `java/lang/String` and
 * friends have no bytecode anywhere in the dex.  Routing those to the JNI stub
 * layer would not work either: `StringBuilder.append` is what every string
 * concatenation in the app compiles to, and it has to return the *same*
 * builder so the next append chains.  So the VM implements them directly.
 *
 * Scope is set by what app glue code actually uses: strings and builders,
 * boxing, Math, System, the exception hierarchy, and small collections.
 * Anything else stays an external class and goes to the host stubs.
 */
/* Extracted and adapted by extract_collections.py; see collections-provenance.json. */
struct rt_list { uint32_t cap, size, mod_count; dvm_ref items[]; };
struct rt_map {
   uint32_t cap, size, ci, sorted, mod_count;
   struct dvm_class *enum_type;
   dvm_ref views[3];
   struct { dvm_ref k, v, node; } e[];
};

static bool list_contains(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool key_eq(struct dvm *v,dvm_ref a,dvm_ref b){return java_basics_equal(v,a,b);}
/* Sorted/enum map classes are not exposed by this module. Never substitute an
 * invented comparator if their internal paths are reached. */
static bool map_compare_key(struct dvm *v,dvm_ref s,dvm_ref a,dvm_ref b,int32_t *r){(void)s;(void)a;(void)b;(void)r;dvm__throw(v,"java/lang/UnsupportedOperationException","sorted maps not installed");return false;}
static bool tree_map_lower_bound(struct dvm *v,dvm_ref s,dvm_ref k,uint32_t *i,bool *e){(void)s;(void)k;(void)i;(void)e;dvm__throw(v,"java/lang/UnsupportedOperationException","sorted maps not installed");return false;}

static struct rt_list *list_of(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (o && o->cls && !strcmp(o->cls->name, "java/util/ArrayList$SubList")) return NULL;
   return o ? o->data : NULL;
}

static bool list_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETV();
   free(o->data);
   o->data = calloc(1, sizeof(struct rt_list) + 8 * sizeof(dvm_ref));
   if (o->data) ((struct rt_list *)o->data)->cap = 8;
   if (!o->data) { dvm__throw(vm,"java/lang/OutOfMemoryError","collection storage"); return false; }
   RETV();
}

static bool list_add(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETI(0);
   struct rt_list *l = o->data;
   if (!l) { list_init(vm, self, NULL, 0, out); l = o->data; }
   if (!l) { dvm__throw(vm,"java/lang/OutOfMemoryError","list storage"); return false; }
   struct dvm_class *set_class =
      dvm__class_by_desc(vm, "Ljava/util/Set;");
   if (set_class && dvm__class_assignable(vm, o->cls, set_class)) {
      union dvm_value present = { 0 };
      (void)list_contains(vm, self, args, 1, &present);
      if (present.i) RETI(0);
   }
   if (l->size >= 1000000) { dvm__throw(vm,"java/lang/OutOfMemoryError","diagnostic collection limit"); return false; }
   if (l->size == l->cap) {
      uint32_t cap = l->cap ? l->cap * 2 : 8;
      struct rt_list *n = realloc(l, sizeof *n + (size_t)cap * sizeof(dvm_ref));
      if (!n) { dvm__throw(vm,"java/lang/OutOfMemoryError","list growth"); return false; }
      n->cap = cap;
      o->data = l = n;
   }
   l->items[l->size++] = ARG(0).l;
   ++l->mod_count;
   if (dvm__env("LUNARIA_TRACE_CLASS_FLOW") && vm->cur_method &&
       vm->cur_method->cls &&
       strstr(vm->cur_method->cls->name, "ReflectionHelper")) {
      struct dvm_object *a = dvm__obj(vm, ARG(0).l);
      /* When the element is a String, its text says which of the parser's
       * return paths produced it — the descriptor it was reading, or the
       * name it was about to hand to Class.forName. */
      const char *txt = (a && a->cls && a->cls->name &&
                         !strcmp(a->cls->name, "java/lang/String"))
                        ? dvm_string_utf8(vm, ARG(0).l) : NULL;
      fprintf(stderr, "[classflow] list.add ref=%x kind=%d cls=%s caller=%s%s%s\n",
              ARG(0).l, a ? (int)a->kind : -1,
              a && a->cls ? a->cls->name : "null", vm->cur_method->name,
              txt ? " text=" : "", txt ? txt : "");
   }
   dvm_pin(vm, ARG(0).l);
   RETI(1);
}

static bool list_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, self);
   int32_t i = ARG(0).i;
   if (!l || i < 0 || (uint32_t)i >= l->size) {
      dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "index %d size %u",
                 i, l ? l->size : 0);
      return false;
   }
   RETL(l->items[i]);
}

static bool list_size(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_list *l = list_of(vm, self);
   RETI(l ? (int32_t)l->size : 0);
}

static bool list_isEmpty(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_list *l = list_of(vm, self);
   RETI(!l || !l->size);
}

static bool list_clear(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct rt_list *l = list_of(vm, self);
   if (l) { l->size = 0; ++l->mod_count; }
   RETV();
}

static bool list_contains(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, self);
   for (uint32_t i = 0; l && i < l->size; ++i) {
      bool hit = key_eq(vm, ARG(0).l, l->items[i]);
      if (dvm_exception(vm)) return false;
      if (hit) RETI(1);
   }
   RETI(0);
}

static bool list_remove(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, self);
   int32_t i = ARG(0).i;
   if (!l || (uint32_t)i >= l->size) { dvm__throw(vm,"java/lang/IndexOutOfBoundsException","list index"); return false; }
   dvm_ref old = l->items[i];
   memmove(&l->items[i], &l->items[i + 1], (l->size - (uint32_t)i - 1) * sizeof(dvm_ref));
   --l->size;
   ++l->mod_count;
   RETL(old);
}

static bool list_add_at(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETV();
   struct rt_list *l = o->data;
   if (!l) { union dvm_value ig = { 0 }; list_init(vm, self, NULL, 0, &ig); l = o->data; }
   if (!l) { dvm__throw(vm, "java/lang/OutOfMemoryError", "list storage"); return false; }
   int32_t i = ARG(0).i;
   if (i < 0 || (uint32_t)i > l->size) {
      dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "index %d size %u",
                 i, l->size);
      return false;
   }
   if (l->size >= 1000000) { dvm__throw(vm,"java/lang/OutOfMemoryError","diagnostic collection limit"); return false; }
   if (l->size == l->cap) {
      uint32_t cap = l->cap ? l->cap * 2 : 8;
      struct rt_list *n = realloc(l, sizeof *n + (size_t)cap * sizeof(dvm_ref));
      if (!n) { dvm__throw(vm, "java/lang/OutOfMemoryError", "list storage"); return false; }
      n->cap = cap;
      o->data = l = n;
   }
   memmove(&l->items[i + 1], &l->items[i], (l->size - (uint32_t)i) * sizeof(dvm_ref));
   l->items[i] = ARG(1).l;
   ++l->size;
   ++l->mod_count;
   dvm_pin(vm, ARG(1).l);
   RETV();
}

static bool list_remove_obj(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, self);
   for (uint32_t i = 0; l && i < l->size; ++i) {
      bool hit = key_eq(vm, ARG(0).l, l->items[i]);
      if (dvm_exception(vm)) return false;
      if (!hit) continue;
      memmove(&l->items[i], &l->items[i + 1],
              (l->size - i - 1) * sizeof(dvm_ref));
      --l->size;
      ++l->mod_count;
      RETI(1);
   }
   RETI(0);
}

static bool list_toArray(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   struct rt_list *l = list_of(vm, self);
   uint32_t n = l ? l->size : 0;
   /* toArray(T[]) returns an array of the argument's component type, and
    * callers cast the result straight back to T[]. */
   const char *ed = "Ljava/lang/Object;";
   struct dvm_object *proto = (nargs > 0 && ARG(0).l) ? dvm__obj(vm, ARG(0).l)
                                                      : NULL;
   if (proto && proto->kind == DVM_OBJ_ARRAY && proto->cls &&
       proto->cls->desc && proto->cls->desc[0] == '[')
      ed = proto->cls->desc + 1;
   if (nargs > 0 && !ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "toArray");
      return false;
   }
   dvm_ref a = proto && proto->length >= n ? ARG(0).l
                                           : dvm_new_array(vm, 'L', ed, n);
   struct dvm_object *ao = a ? dvm__obj(vm, a) : NULL;
   struct dvm_class *element = dvm__class_by_desc(vm, ed);
   for (uint32_t i = 0; ao && ao->data && i < n; ++i) {
      struct dvm_class *actual = dvm_object_class(vm, l->items[i]);
      if (l->items[i] && element &&
          !dvm__class_assignable(vm, actual, element)) {
         dvm__throw(vm, "java/lang/ArrayStoreException", "toArray: %s", ed);
         return false;
      }
      ((uint32_t *)ao->data)[i] = l->items[i];
   }
   if (ao && ao->data && ao->length > n)
      ((uint32_t *)ao->data)[n] = 0;
   RETL(a);
}

static bool list_set(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, self);
   uint32_t i = (uint32_t)ARG(0).i;
   if (!l || (uint32_t)i >= l->size) { dvm__throw(vm,"java/lang/IndexOutOfBoundsException","list index"); return false; }
   dvm_ref old = l->items[i];
   l->items[i] = ARG(1).l;
   RETL(old);
}

static bool map_key_eq(struct dvm *vm, const struct rt_map *mp, dvm_ref a,
                       dvm_ref b)
{
   if (mp && mp->enum_type) return a == b;
   if (mp && mp->ci && a && b) {
      const char *x = dvm_string_utf8(vm, a), *y = dvm_string_utf8(vm, b);
      if (x && y) return !strcasecmp(x, y);
   }
   return key_eq(vm, a, b);
}

static int64_t map_find_key(struct dvm *vm, dvm_ref self, dvm_ref key)
{
   {
      struct dvm_object *object = dvm__obj(vm, self);
      struct rt_map *map = object ? object->data : NULL;
      if (map && map->sorted) {
         uint32_t at;
         bool exact;
         if (!tree_map_lower_bound(vm, self, key, &at, &exact)) return -1;
         return exact ? (int64_t)at : -1;
      }
   }
   for (uint32_t i = 0;; ++i) {
      struct dvm_object *object = dvm__obj(vm, self);
      struct rt_map *map = object ? object->data : NULL;
      if (!map || i >= map->size) return -1;
      dvm_ref candidate = map->e[i].k;
      bool equal;
      if (map->sorted) {
         int32_t comparison;
         if (!map_compare_key(vm, self, key, candidate, &comparison)) return -1;
         equal = comparison == 0;
      } else equal = map_key_eq(vm, map, key, candidate);
      if (dvm_exception(vm)) return -1;
      object = dvm__obj(vm, self);
      map = object ? object->data : NULL;
      if (!map) return -1;
      if (!equal) continue;
      /* A removal may have shifted the entry while equals() ran. */
      if (i < map->size && map->e[i].k == candidate) return i;
      for (uint32_t j = 0; j < map->size; ++j)
         if (map->e[j].k == candidate) return j;
   }
}

static void map_destroy(void *data) { free(data); }

static bool map_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETV();
   free(o->data);
   o->data = calloc(1, sizeof(struct rt_map) + 8 * sizeof(*((struct rt_map *)0)->e));
   o->data_destroy = map_destroy;
   if (o->data) ((struct rt_map *)o->data)->cap = 8;
   if (!o->data) { dvm__throw(vm,"java/lang/OutOfMemoryError","collection storage"); return false; }
   RETV();
}

static bool map_put(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   if (dvm__env("LUNARIA_TRACE_MAP")) {
      const char *key = dvm_string_utf8(vm, ARG(0).l);
      fprintf(stderr, "[dvm-map] put map=%x class=%s key=%x%s%s value=%x caller=%s.%s\n",
              self, o && o->cls ? o->cls->name : "?", ARG(0).l,
              key ? " text=" : "", key ? key : "", ARG(1).l,
              vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?",
              vm->cur_method ? vm->cur_method->name : "?");
   }
   if (!o) RETL(0);
   struct rt_map *mp = o->data;
   if (!mp) { map_init(vm, self, NULL, 0, out); mp = o->data; }
   if (!mp) { dvm__throw(vm,"java/lang/OutOfMemoryError","map storage"); return false; }
   if (mp->enum_type) {
      if (!ARG(0).l) {
         dvm__throw(vm, "java/lang/NullPointerException", "null enum key");
         return false;
      }
      struct dvm_class *key = dvm_object_class(vm, ARG(0).l);
      if (key != mp->enum_type && (!key || key->super != mp->enum_type)) {
         dvm__throw(vm, "java/lang/ClassCastException", "wrong enum key type");
         return false;
      }
   }
   if (mp->sorted && !mp->size) {
      int32_t comparison;
      if (!map_compare_key(vm, self, ARG(0).l, ARG(0).l, &comparison)) return false;
   }
   int64_t found = map_find_key(vm, self, ARG(0).l);
   if (dvm_exception(vm)) return false;
   o = dvm__obj(vm, self);
   mp = o ? o->data : NULL;
   if (!mp) { dvm__throw(vm,"java/lang/OutOfMemoryError","map storage"); return false; }
   if (found >= 0) {
      dvm_ref old = mp->e[found].v;
      mp->e[found].v = ARG(1).l;
      if (mp->e[found].node)
         (void)dvm_set_field(vm, mp->e[found].node, "value", "Ljava/lang/Object;", ARG(1));
      dvm_pin(vm, ARG(1).l);
      RETL(old);
   }
   uint32_t position = mp->size;
   if (mp->sorted) {
      /* The key is absent (found < 0 above), so the first entry not less than
       * it is where it goes. */
      bool exact;
      if (!tree_map_lower_bound(vm, self, ARG(0).l, &position, &exact)) return false;
      o = dvm__obj(vm, self);
      mp = o ? o->data : NULL;
      if (!mp || position > mp->size) {
         dvm__throw(vm, "java/util/ConcurrentModificationException", "sorted map changed during comparison");
         return false;
      }
   }
   if (mp->size >= 1000000) { dvm__throw(vm,"java/lang/OutOfMemoryError","diagnostic collection limit"); return false; }
   if (mp->size == mp->cap) {
      uint32_t cap = mp->cap ? mp->cap * 2 : 8;
      struct rt_map *n = realloc(mp, sizeof *n + (size_t)cap * sizeof n->e[0]);
      if (!n) { dvm__throw(vm,"java/lang/OutOfMemoryError","map growth"); return false; }
      n->cap = cap;
      o->data = mp = n;
   }
   /* The payload pointer must not land in a class name/desc buffer: that
    * means the guest ref resolved to a different object than the caller
    * assumed (heap slot reuse), and the entry write below would corrupt the
    * class record.  Sampled every 16th put (and on every growth above) so the
    * range-table probe and its lock do not slow the guest's map traffic into
    * the 150 s diagnostic window; a corrupted payload is persistent, so a
    * 1-in-16 probe still observes it. */
   static unsigned map_guard_tick;
   if (((++map_guard_tick & 15u) == 0) && dvm__name_range_hit(vm, mp, sizeof *mp))
      fprintf(stderr, "MAP_BAD_PAYLOAD vm=%p self=%x map=%p size=%u cap=%u caller=%s.%s\n",
              (void *)vm, self, (void *)mp, mp->size, mp->cap,
              vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?",
              vm->cur_method ? vm->cur_method->name : "?");
   if (mp->enum_type) {
      union dvm_value ordinal = { 0 }, other = { 0 };
      (void)dvm_get_field(vm, ARG(0).l, "ordinal", "I", &ordinal);
      for (position = 0; position < mp->size; ++position) {
         (void)dvm_get_field(vm, mp->e[position].k, "ordinal", "I", &other);
         if (other.i > ordinal.i) break;
      }
   }
   memmove(mp->e + position + 1, mp->e + position,
           (mp->size - position) * sizeof mp->e[0]);
   mp->e[position].k = ARG(0).l;
   mp->e[position].v = ARG(1).l;
   mp->e[position].node = 0;
   ++mp->size;
   ++mp->mod_count;
   dvm_pin(vm, ARG(0).l);
   dvm_pin(vm, ARG(1).l);
   RETL(0);
}

static bool map_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   if (dvm__env("LUNARIA_TRACE_MAP")) {
      const char *key = dvm_string_utf8(vm, ARG(0).l);
      fprintf(stderr, "[dvm-map] get map=%x class=%s key=%x%s%s caller=%s.%s\n",
              self, o && o->cls ? o->cls->name : "?", ARG(0).l,
              key ? " text=" : "", key ? key : "",
              vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?",
              vm->cur_method ? vm->cur_method->name : "?");
   }
   int64_t found = map_find_key(vm, self, ARG(0).l);
   if (dvm_exception(vm)) return false;
   o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   RETL(mp && found >= 0 ? mp->e[found].v : 0);
}

static bool map_containsKey(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs;
   int64_t found = map_find_key(vm, self, ARG(0).l);
   if (dvm_exception(vm)) return false;
   RETI(found >= 0);
}

static dvm_ref map_erase_at(struct dvm *vm, dvm_ref self, uint32_t index)
{
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   if (mp && index < mp->size) {
      dvm_ref old = mp->e[index].v;
      if (mp->e[index].node) {
         (void)dvm_set_field(vm, mp->e[index].node, "owner", "Ljava/lang/Object;", (union dvm_value){0});
         dvm_unpin(vm, mp->e[index].node);
      }
      memmove(mp->e + index, mp->e + index + 1,
              (mp->size - index - 1) * sizeof mp->e[0]);
      --mp->size;
      ++mp->mod_count;
      return old;
   }
   return 0;
}

static bool map_remove(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)nargs;
   int64_t found = map_find_key(vm, self, ARG(0).l);
   if (dvm_exception(vm)) return false;
   RETL(found >= 0 ? map_erase_at(vm, self, (uint32_t)found) : 0);
}

static bool map_size(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   RETI(mp ? (int32_t)mp->size : 0);
}

static bool map_clear(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   if (mp) {
      for (uint32_t i = 0; i < mp->size; ++i) if (mp->e[i].node) {
         (void)dvm_set_field(vm, mp->e[i].node, "owner", "Ljava/lang/Object;", (union dvm_value){0});
         dvm_unpin(vm, mp->e[i].node);
      }
      mp->size = 0;
      ++mp->mod_count;
   }
   RETV();
}

static bool map_isEmpty(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   RETI(!mp || mp->size == 0);
}
