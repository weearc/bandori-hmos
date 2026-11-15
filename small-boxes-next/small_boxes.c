#include "gc_runtime.h"
#include "small_boxes.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

/* The interpreter serializes calls under its VM lock. Never infer ownership
 * from data alone: other builtins also attach payloads to dvm_object. */
struct small_box { int32_t value; };
static struct { struct dvm *vm; dvm_ref byte[256], short_[256]; } cache;
static bool small_boxes_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
    (void)context;if(!dvm_gil_held(v)||cache.vm!=v)return false;
    for(unsigned i=0;i<256;i++){if(cache.byte[i])visit(&cache.byte[i],user);if(cache.short_[i])visit(&cache.short_[i],user);}return true;
}
static void box_destroy(void *p) { free(p); }
static int type_of(const char *c) {
    if (!strcmp(c, "java/lang/Byte")) return 1;
    if (!strcmp(c, "java/lang/Short")) return 2;
    return 0;
}
static struct small_box *box_of(struct dvm *vm, dvm_ref ref, int type) {
    struct dvm_object *o = dvm__obj(vm, ref);
    if (!o || !o->cls || type_of(o->cls->name) != type ||
        o->data_destroy != box_destroy) return NULL;
    return o->data;
}
static bool fail(struct dvm *vm, const char *kind, const char *message) {
    dvm__throw(vm, kind, "%s", message);
    return true;
}
static bool install(struct dvm *vm, dvm_ref ref, int type, int32_t value) {
    struct dvm_object *o = dvm__obj(vm, ref);
    if (!o) { fail(vm, "java/lang/NullPointerException", "small box receiver"); return false; }
    if (!o->cls || type_of(o->cls->name) != type)
        { fail(vm, "java/lang/IllegalArgumentException", "small box receiver type"); return false; }
    if (o->data && o->data_destroy != box_destroy)
        { fail(vm, "java/lang/IllegalStateException", "foreign small box payload"); return false; }
    struct small_box *p = malloc(sizeof *p);
    if (!p) { fail(vm, "java/lang/OutOfMemoryError", "small box payload"); return false; }
    p->value = value;
    if (o->data) box_destroy(o->data);
    o->data = p;
    o->data_destroy = box_destroy;
    return true;
}
void small_boxes_vm_end(struct dvm *vm) {
    if (cache.vm != vm) return;
    dvm_gc_root_provider_remove(vm,"small-boxes/cache",&cache);
    for (int i = 0; i < 256; ++i) {
        if (cache.byte[i]) dvm_unpin(vm, cache.byte[i]);
        if (cache.short_[i]) dvm_unpin(vm, cache.short_[i]);
    }
    memset(&cache, 0, sizeof cache);
}
void small_boxes_vm_start(struct dvm *vm) {
    if (cache.vm && cache.vm != vm) small_boxes_vm_end(cache.vm);
    if (cache.vm == vm) return;
    memset(&cache, 0, sizeof cache);
    cache.vm = vm;
    if(!dvm_gc_root_provider_add(vm,"small-boxes/cache",DVM_GC_ROOT_STRONG,small_boxes_gc_roots,&cache)){dvm__throw(vm,"java/lang/OutOfMemoryError","small box GC roots");return;}
    struct dvm_class *number = dvm_find_class_or_external(vm, "java/lang/Number");
    for (int t = 1; t <= 2; ++t) {
        struct dvm_class *c = dvm_find_class_or_external(vm,
            t == 1 ? "java/lang/Byte" : "java/lang/Short");
        if (c && c->external) c->super = number;
    }
}
#define IS(m,s) (!strcmp(method,m) && !strcmp(sig,s))
bool small_boxes_call(struct dvm *vm, const char *cls, const char *method,
                      const char *sig, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out) {
    int type = type_of(cls);
    if(!type&&self&&strcmp(method,"<init>")&&strcmp(method,"valueOf")&&(!strcmp(cls,"java/lang/Number")||!strcmp(cls,"java/lang/Object")||!strcmp(cls,"java/lang/Comparable"))){
        struct dvm_object *o=dvm__obj(vm,self);if(o&&o->cls&&o->data_destroy==box_destroy)type=type_of(o->cls->name);
    }
    if (!type) return false;
    const char *prim = type == 1 ? "B" : "S";
    const char *name = type == 1 ? "Byte" : "Short";
    char ctor[5], valueof[32], compare[32];
    snprintf(ctor, sizeof ctor, "(%s)V", prim);
    snprintf(valueof, sizeof valueof, "(%s)Ljava/lang/%s;", prim, name);
    snprintf(compare, sizeof compare, "(Ljava/lang/%s;)I", name);
    bool is_ctor = IS("<init>", ctor), is_factory = IS("valueOf", valueof);
    bool is_value = IS("byteValue", "()B") || IS("shortValue", "()S") ||
        IS("intValue", "()I") || IS("longValue", "()J") ||
        IS("floatValue", "()F") || IS("doubleValue", "()D");
    bool is_hash = IS("hashCode", "()I"), is_text = IS("toString", "()Ljava/lang/String;");
    bool is_equals = IS("equals", "(Ljava/lang/Object;)Z");
    bool is_compare = IS("compareTo", compare);
    if (!(is_ctor || is_factory || is_value || is_hash || is_text || is_equals || is_compare)) return false;
    memset(out, 0, sizeof *out);
    if ((is_ctor || is_factory || is_equals || is_compare) && (nargs != 1 || !args))
        return fail(vm, "java/lang/IllegalArgumentException", "small box arguments");
    if (is_ctor || is_factory) {
        int32_t v = type == 1 ? (int8_t)args[0].i : (int16_t)args[0].i;
        if (is_ctor) { install(vm, self, type, v); return true; }
        if (cache.vm != vm) small_boxes_vm_start(vm);
        dvm_ref *slot = type == 1 ? &cache.byte[(uint8_t)v] :
            (v >= -128 && v <= 127 ? &cache.short_[v + 128] : NULL);
        if (slot && *slot) { out->l = *slot; return true; }
        struct dvm_class *c = dvm_find_class_or_external(vm, cls);
        dvm_ref ref = c ? dvm_new_object(vm, c) : 0;
        if (!ref) return fail(vm, "java/lang/OutOfMemoryError", "small box object");
        if (!install(vm, ref, type, v)) return true;
        if (slot) { *slot = ref; dvm_pin(vm, ref); }
        out->l = ref;
        return true;
    }
    struct small_box *p = box_of(vm, self, type);
    if (!p) return fail(vm, self ? "java/lang/IllegalStateException" : "java/lang/NullPointerException", "small box receiver");
    int32_t v = p->value;
    if (is_value || is_hash) {
        if (IS("byteValue", "()B")) out->i = (int8_t)v;
        else if (IS("shortValue", "()S")) out->i = (int16_t)v;
        else if (IS("longValue", "()J")) out->j = v;
        else if (IS("floatValue", "()F")) out->f = (float)v;
        else if (IS("doubleValue", "()D")) out->d = (double)v;
        else out->i = v;
        return true;
    }
    if (is_equals) {
        struct small_box *other = box_of(vm, args[0].l, type);
        out->i = other && other->value == v;
        return true;
    }
    if (is_compare) {
        if (!args[0].l) return fail(vm, "java/lang/NullPointerException", "small box compareTo");
        struct small_box *other = box_of(vm, args[0].l, type);
        if (!other) return fail(vm, "java/lang/ClassCastException", "small box compareTo");
        out->i = (v > other->value) - (v < other->value);
        return true;
    }
    char text[16];
    snprintf(text, sizeof text, "%d", v);
    out->l = dvm_new_string(vm, text);
    if (!out->l) return fail(vm, "java/lang/OutOfMemoryError", "small box string");
    return true;
}

bool small_boxes_selftest(struct dvm *vm) {
    small_boxes_vm_start(vm);
    bool ok = true;
    const char *names[] = {"java/lang/Byte", "java/lang/Short"};
    const char *factory[] = {"(B)Ljava/lang/Byte;", "(S)Ljava/lang/Short;"};
    const char *ctor[] = {"(B)V", "(S)V"};
    const char *compare[] = {"(Ljava/lang/Byte;)I", "(Ljava/lang/Short;)I"};
    const int values[][4] = {{-128, -1, 0, 127}, {-32768, -129, 127, 32767}};
#define CHECK(x) do { if (!(x)) ok = false; } while (0)
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < 4; ++i) {
            union dvm_value arg = {.i = values[t][i]}, a = {0}, b = {0}, r = {0};
            CHECK(small_boxes_call(vm, names[t], "valueOf", factory[t], 0, &arg, 1, &a));
            CHECK(small_boxes_call(vm, names[t], "valueOf", factory[t], 0, &arg, 1, &b));
            CHECK(a.l && b.l);
            if (t == 0 || (arg.i >= -128 && arg.i <= 127)) CHECK(a.l == b.l);
            CHECK(small_boxes_call(vm, names[t], "intValue", "()I", a.l, NULL, 0, &r) && r.i == arg.i);
            CHECK(small_boxes_call(vm, names[t], "hashCode", "()I", a.l, NULL, 0, &r) && r.i == arg.i);
            CHECK(small_boxes_call(vm, names[t], "longValue", "()J", a.l, NULL, 0, &r) && r.j == arg.i);
            CHECK(small_boxes_call(vm, names[t], "byteValue", "()B", a.l, NULL, 0, &r) && r.i == (int8_t)arg.i);
            CHECK(small_boxes_call(vm, names[t], "shortValue", "()S", a.l, NULL, 0, &r) && r.i == (int16_t)arg.i);
            CHECK(small_boxes_call(vm, names[t], "floatValue", "()F", a.l, NULL, 0, &r) && r.f == (float)arg.i);
            CHECK(small_boxes_call(vm, names[t], "doubleValue", "()D", a.l, NULL, 0, &r) && r.d == arg.i);
            CHECK(small_boxes_call(vm, names[t], "toString", "()Ljava/lang/String;", a.l, NULL, 0, &r) && r.l);
            char expected[16]; snprintf(expected, sizeof expected, "%d", arg.i);
            struct dvm_object *str = dvm__obj(vm, r.l);
            CHECK(str && str->utf8 && !strcmp(str->utf8, expected));
            dvm_ref fresh = dvm_new_object(vm, dvm_find_class_or_external(vm, names[t]));
            CHECK(fresh && fresh != a.l);
            CHECK(small_boxes_call(vm, names[t], "<init>", ctor[t], fresh, &arg, 1, &r));
            union dvm_value other = {.l = fresh};
            CHECK(small_boxes_call(vm, names[t], "equals", "(Ljava/lang/Object;)Z", a.l, &other, 1, &r) && r.i == 1);
            CHECK(small_boxes_call(vm, names[t], "compareTo", compare[t], a.l, &other, 1, &r) && r.i == 0);
            union dvm_value next = {.i = arg.i == (t ? 32767 : 127) ? arg.i - 1 : arg.i + 1}, nextbox = {0};
            CHECK(small_boxes_call(vm, names[t], "valueOf", factory[t], 0, &next, 1, &nextbox));
            other.l = nextbox.l;
            CHECK(small_boxes_call(vm, names[t], "compareTo", compare[t], a.l, &other, 1, &r) &&
                  r.i == (arg.i > next.i ? 1 : -1));
            other.l = 0;
            CHECK(small_boxes_call(vm, names[t], "equals", "(Ljava/lang/Object;)Z", a.l, &other, 1, &r) && r.i == 0);
            CHECK(small_boxes_call(vm, names[t], "compareTo", compare[t], a.l, &other, 1, &r) && dvm_exception(vm));
            dvm_clear_exception(vm);
        }
    }
    union dvm_value v = {.i = 7}, byte = {0}, short_ = {0}, r = {0};
    CHECK(small_boxes_call(vm, names[0], "valueOf", factory[0], 0, &v, 1, &byte));
    CHECK(small_boxes_call(vm, names[1], "valueOf", factory[1], 0, &v, 1, &short_));
    CHECK(small_boxes_call(vm,"java/lang/Number","intValue","()I",byte.l,NULL,0,&r)&&r.i==7);
    CHECK(small_boxes_call(vm,"java/lang/Object","toString","()Ljava/lang/String;",short_.l,NULL,0,&r)&&r.l&&!strcmp(dvm_string_utf8(vm,r.l),"7"));
    union dvm_value other = {.l = short_.l};
    CHECK(small_boxes_call(vm, names[0], "equals", "(Ljava/lang/Object;)Z", byte.l, &other, 1, &r) && r.i == 0);
    CHECK(small_boxes_call(vm, names[0], "compareTo", compare[0], byte.l, &other, 1, &r) && dvm_exception(vm));
    dvm_clear_exception(vm);
    sdk_trace_fprintf(stderr,"SMALL_BOXES_SELFTEST %s byte_short=1 signed_bounds=1 cached_identity=1 constructor_identity=1 wrong_type=1\n",ok?"PASS":"FAIL");
    return ok;
#undef CHECK
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int small_boxes_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;(void)visit;(void)user;return o->data_destroy==box_destroy?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
