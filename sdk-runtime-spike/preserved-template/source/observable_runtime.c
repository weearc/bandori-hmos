/*
 * java.util.Observable: a real observer registry.  UserInfoServiceImpl
 * extends it during SDK initialization; notifyObservers dispatches the
 * original Observer.update(Observable,Object) through the DVM.
 */
#include "observable_runtime.h"
#include "dvm/dvm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

typedef struct {
    dvm_ref observer;
} observer_entry;

typedef struct {
    struct dvm *vm;
    observer_entry *items;
    size_t count;
    size_t capacity;
    int changed;
} observable_data;

static void observable_free(void *p) {
    observable_data *o = p;
    if (!o) return;
    free(o->items);
    free(o);
}

static observable_data *observable_of(struct dvm *v, dvm_ref self) {
    struct dvm_object *obj = dvm__obj(v, self);
    return obj && obj->data_destroy == observable_free ? (observable_data *)obj->data : NULL;
}

static bool observable_class(struct dvm *v, dvm_ref self, const char *c) {
    if (c && !strcmp(c, "java/util/Observable")) return true;
    struct dvm_object *obj = dvm__obj(v, self);
    for (struct dvm_class *k = obj ? obj->cls : NULL; k; k = k->super)
        if (k->name && !strcmp(k->name, "java/util/Observable")) return true;
    return false;
}

static bool observable_append(struct dvm *v, observable_data *o, dvm_ref observer) {
    if (o->count == o->capacity) {
        size_t next = o->capacity ? o->capacity * 2 : 4;
        if (next < o->capacity || next > SIZE_MAX / sizeof *o->items) {
            dvm__throw(v, "java/lang/OutOfMemoryError", "Observable observers");
            return false;
        }
        observer_entry *grown = realloc(o->items, next * sizeof *grown);
        if (!grown) {
            dvm__throw(v, "java/lang/OutOfMemoryError", "Observable observers");
            return false;
        }
        o->items = grown;
        o->capacity = next;
    }
    o->items[o->count++].observer = observer;
    return true;
}

static bool observable_delete(struct dvm *v, observable_data *o, dvm_ref observer) {
    for (size_t i = 0; i < o->count; ++i) {
        if (o->items[i].observer == observer) {
            o->items[i] = o->items[--o->count];
            return true;
        }
    }
    return true;
}

static bool observable_notify(struct dvm *v, observable_data *o, dvm_ref self,
                              dvm_ref value, union dvm_value *out) {
    (void)out;
    if (!o->changed) return true;
    size_t count = o->count;
    dvm_ref *snapshot = count ? malloc(count * sizeof *snapshot) : NULL;
    if (count && !snapshot) {
        dvm__throw(v, "java/lang/OutOfMemoryError", "Observable notify snapshot");
        return false;
    }
    for (size_t i = 0; i < count; ++i) snapshot[i] = o->items[i].observer;
    /* Java clears changed before invoking observers; a concurrent add/delete
     * therefore does not affect this notification pass. */
    o->changed = 0;
    for (size_t i = 0; i < count; ++i) {
        struct dvm_object *observer = dvm__obj(v, snapshot[i]);
        struct dvm_method *update = observer ? dvm_find_method(
            v, observer->cls, "update", "(Ljava/util/Observable;Ljava/lang/Object;)V") : NULL;
        if (!update) {
            free(snapshot);
            dvm__throw(v, "java/lang/AbstractMethodError", "Observer.update");
            return false;
        }
        union dvm_value args[2] = {{.l = self}, {.l = value}}, result = {0};
        if (!dvm_call(v, update, snapshot[i], args, 2, &result) || dvm_exception(v)) {
            free(snapshot);
            return false;
        }
    }
    free(snapshot);
    return true;
}

bool observable_runtime_call(struct dvm *v, const char *c, const char *m,
                             const char *sig, dvm_ref self,
                             const union dvm_value *a, int n,
                             union dvm_value *out) {
    memset(out, 0, sizeof *out);
    if (!v || !c || !m || !sig) return false;
    if (!observable_class(v, self, c)) return false;

    if (!strcmp(m, "<init>") && !strcmp(sig, "()V") && n == 0) {
        struct dvm_object *obj = dvm__obj(v, self);
        if (!obj) { dvm__throw(v, "java/lang/NullPointerException", "Observable receiver"); return true; }
        if (obj->data) { dvm__throw(v, "java/lang/IllegalStateException", "Observable already initialized"); return true; }
        observable_data *data = calloc(1, sizeof *data);
        if (!data) { dvm__throw(v, "java/lang/OutOfMemoryError", "Observable"); return true; }
        data->vm = v;
        obj->data = data;
        obj->data_destroy = observable_free;
        return true;
    }

    observable_data *data = observable_of(v, self);
    if (!data) {
        struct dvm_object *dbg = dvm__obj(v, self);
        sdk_trace_fprintf(stderr,
                          "OBSERVABLE_UNINITIALIZED c=%s m=%s sig=%s self_class=%s\n",
                          c ? c : "?", m ? m : "?", sig ? sig : "?",
                          dbg && dbg->cls && dbg->cls->name ? dbg->cls->name : "?");
        dvm__throw(v, "java/lang/IllegalStateException", "Observable not initialized");
        return true;
    }
    if (!strcmp(m, "addObserver") && !strcmp(sig, "(Ljava/util/Observer;)V") && n == 1) {
        if (!a || !a[0].l) { dvm__throw(v, "java/lang/NullPointerException", "Observer"); return true; }
        if (!observable_append(v, data, a[0].l)) return true;
        return true;
    }
    if (!strcmp(m, "deleteObserver") && !strcmp(sig, "(Ljava/util/Observer;)V") && n == 1) {
        return observable_delete(v, data, a && a[0].l ? a[0].l : 0);
    }
    if (!strcmp(m, "deleteObservers") && !strcmp(sig, "()V") && n == 0) {
        data->count = 0; return true;
    }
    if (!strcmp(m, "countObservers") && !strcmp(sig, "()I") && n == 0) {
        out->i = (int32_t)data->count; return true;
    }
    if (!strcmp(m, "setChanged") && !strcmp(sig, "()V") && n == 0) {
        data->changed = 1; return true;
    }
    if (!strcmp(m, "clearChanged") && !strcmp(sig, "()V") && n == 0) {
        data->changed = 0; return true;
    }
    if (!strcmp(m, "hasChanged") && !strcmp(sig, "()Z") && n == 0) {
        out->i = data->changed; return true;
    }
    if (!strcmp(m, "notifyObservers") && !strcmp(sig, "()V") && n == 0)
        return observable_notify(v, data, self, 0, out);
    if (!strcmp(m, "notifyObservers") && !strcmp(sig, "(Ljava/lang/Object;)V") && n == 1)
        return observable_notify(v, data, self, a && a[0].l ? a[0].l : 0, out);
    return false;
}
