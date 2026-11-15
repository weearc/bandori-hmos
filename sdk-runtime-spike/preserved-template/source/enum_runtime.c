#include "enum_runtime.h"
#include <stdlib.h>
#include <string.h>

typedef struct { struct dvm *vm; dvm_ref name; int ordinal; struct dvm_class *declaring; } enum_data;
static bool eqs(const char *a,const char *b){return a&&strcmp(a,b)==0;}
static void destroy(void *p){enum_data *d=p;if(d){if(d->name)dvm_unpin(d->vm,d->name);free(d);}}
static enum_data *payload(struct dvm_object *o){return o&&o->data_destroy==destroy?(enum_data *)o->data:NULL;}
bool enum_runtime_call(struct dvm *v,const char *c,const char *m,const char *s,
                       dvm_ref self,const union dvm_value *a,int n,
                       union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);if(!o)return false;
 enum_data *d=payload(o);
 if(eqs(c,"java/lang/Enum")&&eqs(m,"<init>")&&eqs(s,"(Ljava/lang/String;I)V")&&n==2){
   if(!a||!a[0].l||o->data){dvm__throw(v,"java/lang/IllegalArgumentException","invalid Enum constructor state");return true;}
   d=calloc(1,sizeof *d);if(!d){dvm__throw(v,"java/lang/OutOfMemoryError","Enum metadata");return true;}
   d->vm=v;d->name=a[0].l;d->ordinal=a[1].i;
   d->declaring=o->cls;
   while(d->declaring&&d->declaring->super&&!eqs(d->declaring->super->name,"java/lang/Enum"))d->declaring=d->declaring->super;
   if(!d->declaring||!d->declaring->super){free(d);dvm__throw(v,"java/lang/ClassCastException","receiver is not an enum");return true;}
   dvm_pin(v,d->name);o->data=d;o->data_destroy=destroy;return true;
 }
 if(!d)return false;
 if(eqs(m,"name")&&eqs(s,"()Ljava/lang/String;")&&n==0){out->l=d->name;return true;}
 if(eqs(m,"ordinal")&&eqs(s,"()I")&&n==0){out->i=d->ordinal;return true;}
 if(eqs(m,"toString")&&eqs(s,"()Ljava/lang/String;")&&n==0){out->l=d->name;return true;}
 if(eqs(m,"equals")&&eqs(s,"(Ljava/lang/Object;)Z")&&n==1){out->i=self==a[0].l;return true;}
 if(eqs(m,"hashCode")&&eqs(s,"()I")&&n==0){out->i=(int)self;return true;}
 if(eqs(m,"getDeclaringClass")&&eqs(s,"()Ljava/lang/Class;")&&n==0){
   out->l=dvm_class_object(v,d->declaring);return true;
 }
 if(eqs(m,"compareTo")&&eqs(s,"(Ljava/lang/Enum;)I")&&n==1){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","enum comparison");return true;}
   struct dvm_object *other=dvm__obj(v,a[0].l);enum_data *od=payload(other);
   if(!od||od->declaring!=d->declaring){dvm__throw(v,"java/lang/ClassCastException","different enum type");return true;}
   out->i=(int32_t)((uint32_t)d->ordinal-(uint32_t)od->ordinal);return true;
 }
 return false;
}
