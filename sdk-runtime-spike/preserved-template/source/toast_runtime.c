/*
 * android.widget.Toast: value object + explicit show boundary.
 *
 * The HAP has no Android window system.  show() records the requested toast
 * (text/duration) and does not pretend that a UI was displayed; a host toast
 * bridge can replace this boundary later.  Constructor/makeText/setText are
 * real object operations so the caller's control flow is preserved.
 */
#include "toast_runtime.h"
#include "dvm/dvm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);

typedef struct {
    dvm_ref context;
    dvm_ref text;
    int duration;
    int gravity;
    int xoff;
    int yoff;
    dvm_ref view;
} toast_data;

static void toast_free(void *p) { free(p); }

static toast_data *toast_of(struct dvm *v, dvm_ref self) {
    struct dvm_object *o = dvm__obj(v, self);
    return o && o->data_destroy == toast_free ? (toast_data *)o->data : NULL;
}

static bool toast_init_data(struct dvm *v, dvm_ref self, dvm_ref context,
                            dvm_ref text, int duration) {
    struct dvm_object *o = dvm__obj(v, self);
    if (!o) { dvm__throw(v, "java/lang/NullPointerException", "Toast receiver"); return false; }
    if (o->data) { dvm__throw(v, "java/lang/IllegalStateException", "Toast already initialized"); return false; }
    toast_data *data = calloc(1, sizeof *data);
    if (!data) { dvm__throw(v, "java/lang/OutOfMemoryError", "Toast"); return false; }
    data->context = context;
    data->text = text;
    data->duration = duration;
    o->data = data;
    o->data_destroy = toast_free;
    return true;
}

static bool toast_new(struct dvm *v, dvm_ref context, dvm_ref text, int duration,
                      union dvm_value *out) {
    struct dvm_class *cls = dvm_find_class_or_external(v, "android/widget/Toast");
    dvm_ref ref = cls ? dvm_new_object(v, cls) : 0;
    if (!ref) {
        dvm__throw(v, "java/lang/OutOfMemoryError", "Toast");
        return false;
    }
    if (!toast_init_data(v, ref, context, text, duration)) return false;
    out->l = ref;
    return true;
}

bool toast_runtime_call(struct dvm *v, const char *c, const char *m,
                        const char *sig, dvm_ref self,
                        const union dvm_value *a, int n,
                        union dvm_value *out) {
    (void)out;
    if (!v || !c || !m || !sig) return false;
    if (strcmp(c, "android/widget/Toast")) return false;
    sdk_trace_fprintf(stderr, "TOAST_CALL m=%s sig=%s n=%d self_class=%s\n",
                      m, sig, n,
                      dvm__obj(v, self) && dvm__obj(v, self)->cls && dvm__obj(v, self)->cls->name
                          ? dvm__obj(v, self)->cls->name : "?");

    if (!strcmp(m, "makeText") && !strcmp(sig, "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;") && n == 3) {
        if (!a || !a[0].l) { dvm__throw(v, "java/lang/NullPointerException", "Toast context"); return true; }
        return toast_new(v, a[0].l, a[1].l, a[2].i, out) ? true : (dvm_exception(v) != 0);
    }
    if (!strcmp(m, "<init>") && !strcmp(sig, "(Landroid/content/Context;)V") && n == 1) {
        if (!a || !a[0].l) { dvm__throw(v, "java/lang/NullPointerException", "Toast context"); return true; }
        bool ok=toast_init_data(v, self, a[0].l, 0, 0);
        sdk_trace_fprintf(stderr, "TOAST_CTOR ok=%d self=0x%x\n", ok?1:0, (unsigned)self);
        return ok ? true : (dvm_exception(v) != 0);
    }
    toast_data *data = toast_of(v, self);
    if (!data) {
        struct dvm_object *dbg = dvm__obj(v, self);
        sdk_trace_fprintf(stderr, "TOAST_UNINIT method=%s sig=%s self_class=%s\n",
                          m ? m : "?", sig ? sig : "?",
                          dbg && dbg->cls && dbg->cls->name ? dbg->cls->name : "?");
        dvm__throw(v, "java/lang/IllegalStateException", "Toast not initialized");
        return true;
    }
    if (!strcmp(m, "setText") && !strcmp(sig, "(Ljava/lang/CharSequence;)V") && n == 1) {
        data->text = a && a[0].l ? a[0].l : 0;
        return true;
    }
    if (!strcmp(m, "setDuration") && !strcmp(sig, "(I)V") && n == 1) {
        data->duration = a[0].i;
        return true;
    }
    if (!strcmp(m, "getDuration") && !strcmp(sig, "()I") && n == 0) {
        out->i = data->duration;
        return true;
    }
    if (!strcmp(m, "setGravity") && !strcmp(sig, "(III)V") && n == 3) {
        data->gravity = a[0].i; data->xoff = a[1].i; data->yoff = a[2].i;
        return true;
    }
    if (!strcmp(m, "setView") && !strcmp(sig, "(Landroid/view/View;)V") && n == 1) {
        data->view = a && a[0].l ? a[0].l : 0;
        return true;
    }
    if (!strcmp(m, "cancel") && !strcmp(sig, "()V") && n == 0) return true;
    if (!strcmp(m, "show") && !strcmp(sig, "()V") && n == 0) {
        const char *text = data->text ? dvm_string_utf8(v, data->text) : "";
        sdk_trace_fprintf(stderr,
                          "TOAST_BOUNDARY show duration=%d gravity=%d text=%s\n",
                          data->duration, data->gravity, text ? text : "");
        return true;
    }
    if (!strcmp(m, "getView") && !strcmp(sig, "()Landroid/view/View;") && n == 0) {
        out->l = 0;
        return true;
    }
    return false;
}
