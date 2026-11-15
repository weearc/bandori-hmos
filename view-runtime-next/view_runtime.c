#include "gc_runtime.h"
/*
 * Minimal android.view.View / TextView / GradientDrawable value facade.
 *
 * The diagnostic HAP has no Android view tree.  These objects preserve the
 * original call sequence and store text/background properties, but layout,
 * measurement and drawing are not claimed.  UI rendering remains a host
 * bridge task.
 */
#include "view_runtime.h"
#include "dvm/dvm_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct { dvm_ref text; dvm_ref background; int color; float text_size; } text_view_data;
static void text_view_free(void *p) { free(p); }

static text_view_data *tv_of(struct dvm *v, dvm_ref self) {
    struct dvm_object *o = dvm__obj(v, self);
    return o && o->data_destroy == text_view_free ? (text_view_data *)o->data : NULL;
}

bool view_runtime_call(struct dvm *v, const char *c, const char *m,
                       const char *sig, dvm_ref self,
                       const union dvm_value *a, int n,
                       union dvm_value *out) {
    (void)out;
    if (!v || !c || !m || !sig) return false;

    if (!strcmp(c, "android/graphics/drawable/GradientDrawable")) {
        if (!strcmp(m, "<init>") && !strcmp(sig, "()V")) return true;
        if (!strcmp(m, "setColor") && !strcmp(sig, "(I)V") && n == 1) return true;
        if (!strcmp(m, "setCornerRadius") && !strcmp(sig, "(F)V") && n == 1) return true;
        return false;
    }

    if (!strcmp(c, "android/widget/TextView") || !strcmp(c, "android/view/View")) {
        if (!strcmp(m, "<init>") && !strcmp(sig, "(Landroid/content/Context;)V") && n == 1) {
            struct dvm_object *o = dvm__obj(v, self);
            if (!o) { dvm__throw(v, "java/lang/NullPointerException", "View receiver"); return true; }
            if (strcmp(c, "android/widget/TextView")) return true;
            if (!o->data) {
                text_view_data *data = calloc(1, sizeof *data);
                if (!data) { dvm__throw(v, "java/lang/OutOfMemoryError", "TextView"); return true; }
                o->data = data;
                o->data_destroy = text_view_free;
            }
            return true;
        }
        text_view_data *data = tv_of(v, self);
        if (!data) { dvm__throw(v, "java/lang/IllegalStateException", "TextView not initialized"); return true; }
        if (!strcmp(m, "setPadding") && !strcmp(sig, "(IIII)V") && n == 4) return true;
        if (!strcmp(m, "setBackground") && !strcmp(sig, "(Landroid/graphics/drawable/Drawable;)V") && n == 1) {
            data->background = a && a[0].l ? a[0].l : 0; return true;
        }
        if (!strcmp(m, "setText") && !strcmp(sig, "(Ljava/lang/CharSequence;)V") && n == 1) {
            data->text = a && a[0].l ? a[0].l : 0; return true;
        }
        if (!strcmp(m, "getText") && !strcmp(sig, "()Ljava/lang/CharSequence;") && n == 0) {
            out->l = data->text; return true;
        }
        if (!strcmp(m, "setTextSize") && !strcmp(sig, "(IF)V") && n == 2) {
            data->text_size = a[1].f; return true;
        }
        if (!strcmp(m, "setTextColor") && !strcmp(sig, "(I)V") && n == 1) {
            data->color = a[0].i; return true;
        }
        if (!strcmp(m, "setGravity") && !strcmp(sig, "(I)V") && n == 1) return true;
        if (!strcmp(m, "setVisibility") && !strcmp(sig, "(I)V") && n == 1) return true;
        if (!strcmp(m, "setLayoutParams") && !strcmp(sig, "(Landroid/view/ViewGroup$LayoutParams;)V") && n == 1) return true;
        return false;
    }
    return false;
}

int text_view_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 (void)v;if(o->data_destroy!=text_view_free)return DVM_GC_PAYLOAD_UNKNOWN;
 if(visit){text_view_data *p=o->data;visit(&p->text,u);visit(&p->background,u);}return DVM_GC_PAYLOAD_REFS;
}
