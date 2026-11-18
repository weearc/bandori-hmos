/* Precise declaration layouts. Included by dvm.c after dvm_internal.h.
 * This enumerates guest reference SLOTS, never pointer-shaped primitive bits.
 * Native payload edges require an independent registered visitor. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

struct dvm_gc_field_snapshot {
   char *type;
   uint32_t access;
   uint16_t slot;
   uint8_t width;
   char kind;
};
struct dvm_gc_level_snapshot {
   struct dvm_class *klass, *super;
   int islots, nifields, nsfields, nsslots;
   struct dvm_gc_field_snapshot *instance, *statics;
};
#include "gc_layout_types.h"
/* Only active while the owning DVM keeps the GIL continuously. Metadata
 * cannot change during tracing; invalidation destroys the stamped layout. */
static _Thread_local uint64_t gc_layout_stw_epoch;
static uint64_t gc_layout_next_stw_epoch;

static void dvm_gc_layout_free(struct dvm_gc_layout *layout)
{
   if (!layout) return;
   for (unsigned n=0; n<layout->levels; ++n) {
      struct dvm_gc_level_snapshot *s=&layout->chain[n];
      if (s->instance) for (int i=0;i<s->nifields;++i) free(s->instance[i].type);
      if (s->statics) for (int i=0;i<s->nsfields;++i) free(s->statics[i].type);
      free(s->instance); free(s->statics);
   }
   free(layout->chain); free(layout->instance_slots);
   free(layout->static_slots); free(layout);
}

void dvm_gc_layout_destroy(struct dvm_class *klass)
{
   if (!klass) return;
   dvm_gc_layout_free(klass->gc_layout);
   klass->gc_layout=NULL;
}

void dvm_gc_layout_invalidate(struct dvm_class *klass)
{
   dvm_gc_layout_destroy(klass);
}

/* Bounds are metadata safety limits, not collection truncation: exceeding
 * them returns false. A failed layout must never be treated as an empty one. */
static bool dvm_gc_field_valid(const struct dvm_field *f, int slots)
{
   /* dvm.c class_load_from_dex sets width=1 for EVERY field (including J/D),
    * increments slot once, and iget/iput read a single union dvm_value.
    * The internal header's old "1 or 2 slots" comment describes register
    * widths, not this union-slot storage ABI. Do not reserve a fictitious
    * second slot for J/D or accept a width=2 producer we cannot interpret. */
   if (!f->type || !f->type[0] || slots<0 || f->slot>=slots || f->width!=1)
      return false;
   char c=f->type[0];
   if (c=='L') {
      size_t n=strlen(f->type);
      return n>=3 && f->type[n-1]==';' && f->kind=='L';
   }
   if (c=='[') {
      const char *p=f->type;
      while (*p=='[') ++p;
      if (*p=='L') {
         size_t n=strlen(p);
         return n>=3 && p[n-1]==';' && f->kind=='L';
      }
      return p[0] && !p[1] && strchr("ZBCSIJFD",p[0]) && f->kind=='L';
   }
   return !f->type[1] && strchr("ZBCSIJFD",c) && f->kind==c;
}

static bool dvm_gc_fields_match(const struct dvm_gc_field_snapshot *saved,
                                const struct dvm_field *fields, int count)
{
   if (count && (!saved || !fields)) return false;
   for (int i=0;i<count;++i) {
      const struct dvm_gc_field_snapshot *s=&saved[i];
      const struct dvm_field *f=&fields[i];
      if (!f->type || strcmp(s->type,f->type) || s->slot!=f->slot ||
          s->width!=f->width || s->kind!=f->kind || s->access!=f->access)
         return false;
   }
   return true;
}

static bool dvm_gc_layout_matches(const struct dvm_gc_layout *layout,
                                  struct dvm_class *klass)
{
   if (!layout) return false;
   for (unsigned i=0;i<layout->levels;++i) {
      const struct dvm_gc_level_snapshot *s=&layout->chain[i];
      if (!klass || klass!=s->klass || klass->super!=s->super ||
          klass->islots!=s->islots || klass->nifields!=s->nifields ||
          klass->nsfields!=s->nsfields || klass->nsslots!=s->nsslots ||
          !dvm_gc_fields_match(s->instance,klass->ifields,s->nifields) ||
          !dvm_gc_fields_match(s->statics,klass->sfields,s->nsfields))
         return false;
      klass=klass->super;
   }
   return klass==NULL;
}

static bool dvm_gc_snapshot_fields(struct dvm_gc_field_snapshot **dest,
                                   const struct dvm_field *fields,
                                   int count, int slots)
{
   if (!count) return true;
   if (!fields) return false;
   *dest=calloc((size_t)count,sizeof **dest);
   if (!*dest) return false;
   for (int i=0;i<count;++i) {
      if (!dvm_gc_field_valid(&fields[i],slots)) return false;
      (*dest)[i]=(struct dvm_gc_field_snapshot){
         .access=fields[i].access,.slot=fields[i].slot,
         .width=fields[i].width,.kind=fields[i].kind};
      (*dest)[i].type=strdup(fields[i].type);
      if (!(*dest)[i].type) return false;
   }
   return true;
}

bool dvm_gc_layout_ensure(struct dvm *vm, struct dvm_class *klass)
{
   (void)vm;
   if (!klass) return false;
   if (gc_layout_stw_epoch && klass->gc_layout &&
       klass->gc_layout->stw_validated_epoch==gc_layout_stw_epoch) return true;
   if (dvm_gc_layout_matches(klass->gc_layout,klass)) {
      klass->gc_layout->stw_validated_epoch=gc_layout_stw_epoch;return true;
   }
   struct dvm_class *chain[256];unsigned levels=0;
   for (struct dvm_class *c=klass;c;c=c->super) {
      if (levels==256 || c->islots<0 || c->islots>65536 ||
          c->nifields<0 || c->nifields>65536 || c->nsfields<0 ||
          c->nsfields>65536 || c->nsslots<0 || c->nsslots>65536)
         return false;
      for (unsigned i=0;i<levels;++i) if (chain[i]==c) return false;
      chain[levels++]=c;
   }
   struct dvm_gc_layout *layout=calloc(1,sizeof *layout);
   if (!layout) return false;
   layout->chain=calloc(levels,sizeof *layout->chain);
   if (!layout->chain) { dvm_gc_layout_free(layout); return false; }
   layout->levels=levels;
   /* Byte maps deduplicate inherited absolute slot indices. Metadata from a
    * child must not overlap its superclass slots. */
   unsigned char *seen=calloc(klass->islots? (size_t)klass->islots:1,1);
   unsigned char *statics=calloc(klass->nsslots? (size_t)klass->nsslots:1,1);
   if (!seen || !statics) goto fail;
   for (unsigned n=0;n<levels;++n) {
      struct dvm_class *c=chain[n];struct dvm_gc_level_snapshot *s=&layout->chain[n];
      *s=(struct dvm_gc_level_snapshot){.klass=c,.super=c->super,
         .islots=c->islots,.nifields=c->nifields,.nsfields=c->nsfields,
         .nsslots=c->nsslots};
      if (c->islots>klass->islots || (c->super && c->super->islots>c->islots) ||
          !dvm_gc_snapshot_fields(&s->instance,c->ifields,c->nifields,c->islots) ||
          !dvm_gc_snapshot_fields(&s->statics,c->sfields,c->nsfields,c->nsslots))
         goto fail;
      for (int i=0;i<c->nifields;++i) {
         struct dvm_field *f=&c->ifields[i];
         if ((c->super && f->slot<c->super->islots) || seen[f->slot]) goto fail;
         seen[f->slot]=f->kind=='L'?2:1;
         if (f->kind=='L') ++layout->instance_count;
      }
      if (!n) for (int i=0;i<c->nsfields;++i) {
         struct dvm_field *f=&c->sfields[i];
         if (statics[f->slot]) goto fail;
         statics[f->slot]=f->kind=='L'?2:1;
         if (f->kind=='L') ++layout->static_count;
      }
   }
   if (layout->instance_count) {
      layout->instance_slots=malloc(layout->instance_count*sizeof(uint32_t));
      if (!layout->instance_slots) goto fail;
   }
   if (layout->static_count) {
      layout->static_slots=malloc(layout->static_count*sizeof(uint32_t));
      if (!layout->static_slots) goto fail;
   }
   size_t at=0;
   for (int i=0;i<klass->islots;++i) if (seen[i]==2) layout->instance_slots[at++]=(uint32_t)i;
   at=0;
   for (int i=0;i<klass->nsslots;++i) if (statics[i]==2) layout->static_slots[at++]=(uint32_t)i;
   free(seen);free(statics);
   layout->stw_validated_epoch=gc_layout_stw_epoch;
   dvm_gc_layout_destroy(klass);klass->gc_layout=layout;return true;
fail:
   free(seen);free(statics);dvm_gc_layout_free(layout);return false;
}

bool dvm_gc_payload_requires_visitor(struct dvm_object *obj)
{
   /* utf8/string_units are separate atomic buffers. Arbitrary non-array data
    * is UNKNOWN, including scalar boxes until registered explicitly. */
   return obj && obj->data && obj->kind!=DVM_OBJ_ARRAY;
}

extern int network_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int json_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int collections_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int java_basics_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int reflection_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int io_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int set_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int list_views_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int small_boxes_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int enum_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int file_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int prefs_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int uri_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int bundle_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int intent_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int sqlite_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int platform_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int cert_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int socket_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int ssl_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int response_fixture_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int thread_runtime_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int android_basics_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int executor_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int weak_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int throwable_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int asset_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int metadata_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int surface_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int raf_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int lru_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int concurrent_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int lock_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int discovery_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int locale_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int filter_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int text_view_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int battery_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int observable_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int content_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
extern int view_motion_gc_payload(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *) __attribute__((weak));
int dvm_gc_payload_visit(struct dvm *vm,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 if(!o)return DVM_GC_PAYLOAD_INVALID;
 if(!dvm_gc_payload_requires_visitor(o))return DVM_GC_PAYLOAD_ATOMIC;
 int (*providers[])(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *)={battery_gc_payload,observable_gc_payload,content_gc_payload,view_motion_gc_payload,locale_gc_payload,filter_gc_payload,text_view_gc_payload,concurrent_gc_payload,lock_gc_payload,discovery_gc_payload,android_basics_gc_payload,executor_gc_payload,weak_gc_payload,throwable_gc_payload,asset_gc_payload,metadata_gc_payload,surface_gc_payload,raf_gc_payload,lru_gc_payload,thread_runtime_gc_payload,network_gc_payload,json_gc_payload,collections_gc_payload,java_basics_gc_payload,reflection_gc_payload,io_gc_payload,set_gc_payload,list_views_gc_payload,small_boxes_gc_payload,enum_gc_payload,file_gc_payload,prefs_gc_payload,uri_gc_payload,bundle_gc_payload,intent_gc_payload,sqlite_gc_payload,platform_gc_payload,cert_gc_payload,socket_gc_payload,ssl_gc_payload,response_fixture_gc_payload};
 /* These three visitors recognize only their own file-local destructor
  * functions, and still validate the payload on every invocation. Prioritize
  * common producers; no class/category-only cache can bypass validation. */
 int (*fast[])(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *)={java_basics_gc_payload,reflection_gc_payload,collections_gc_payload};
 for(unsigned i=0;i<3;i++)if(fast[i]){int kind=fast[i](vm,o,visit,user);if(kind!=DVM_GC_PAYLOAD_UNKNOWN)return kind;}
 for(size_t i=0;i<sizeof providers/sizeof providers[0];i++)if(providers[i]){int kind=providers[i](vm,o,visit,user);if(kind!=DVM_GC_PAYLOAD_UNKNOWN)return kind;}
 return DVM_GC_PAYLOAD_UNKNOWN;
}

bool dvm_gc_visit_declared_refs(struct dvm *vm, dvm_ref ref,
                                void (*visit)(dvm_ref *,void *),void *user)
{
   struct dvm_object *obj=dvm__obj(vm,ref);
   if (!obj || !obj->cls || !visit) return false;
   if (obj->kind<DVM_OBJ_PLAIN || obj->kind>DVM_OBJ_EXTERNAL) return false;
   if (obj->kind==DVM_OBJ_ARRAY) {
      if (!obj->cls->is_array || obj->elem_kind!=obj->cls->elem_kind ||
          !obj->elem_kind || !strchr("ZBCSIJFDL",obj->elem_kind) ||
          (!obj->data && obj->length)) return false;
      if (obj->elem_kind=='L') {
         if (sizeof(dvm_ref)!=4) return false;
#if SIZE_MAX < UINT64_MAX
         if (obj->length>SIZE_MAX/sizeof(dvm_ref)) return false;
#endif
         dvm_ref *items=obj->data;
         for (uint32_t i=0;i<obj->length;++i) visit(&items[i],user);
      }
      return true;
   }
   if (!dvm_gc_layout_ensure(vm,obj->cls)) return false;
   struct dvm_gc_layout *layout=obj->cls->gc_layout;
   if (layout->instance_count && !obj->slots) return false;
   for (size_t i=0;i<layout->instance_count;++i)
      visit(&obj->slots[layout->instance_slots[i]].l,user);
   return true; /* native payload coverage is checked separately */
}

bool dvm_gc_visit_static_refs(struct dvm *vm,struct dvm_class *klass,
                             void (*visit)(dvm_ref *,void *),void *user)
{
   if (!visit || !dvm_gc_layout_ensure(vm,klass)) return false;
   struct dvm_gc_layout *layout=klass->gc_layout;
   if (layout->static_count && !klass->sslots) return false;
   for (size_t i=0;i<layout->static_count;++i)
      visit(&klass->sslots[layout->static_slots[i]].l,user);
   return true; /* class_object/loader/type_parameters are separate roots */
}

/* Narrow metadata-only check. Does not allocate guest objects or collect.
 * The integration driver must separately test real instance/array visitation.
 * Returns false on any mismatch; caller owns reporting the result. */
bool dvm_gc_layout_metadata_selftest(void)
{
   struct dvm_class base={0},child={0};bool ok=false;
   struct dvm_field parent_fields[3]={
      {.type="J",.kind='J',.width=1,.slot=0},
      {.type="Ljava/lang/Object;",.kind='L',.width=1,.slot=1},
      {.type="D",.kind='D',.width=1,.slot=2}};
   struct dvm_field child_fields[2]={
      {.type="[J",.kind='L',.width=1,.slot=3},
      {.type="I",.kind='I',.width=1,.slot=4}};
   struct dvm_field statics[3]={
      {.type="D",.kind='D',.width=1,.slot=0},
      {.type="[Ljava/lang/Object;",.kind='L',.width=1,.slot=1},
      {.type="J",.kind='J',.width=1,.slot=2}};
   base.ifields=parent_fields;base.nifields=3;base.islots=3;
   child.super=&base;child.ifields=child_fields;child.nifields=2;child.islots=5;
   child.sfields=statics;child.nsfields=3;child.nsslots=3;
   if (!dvm_gc_layout_ensure(NULL,&child) ||
       child.gc_layout->instance_count!=2 ||
       child.gc_layout->instance_slots[0]!=1 ||
       child.gc_layout->instance_slots[1]!=3 ||
       child.gc_layout->static_count!=1 || child.gc_layout->static_slots[0]!=1)
      goto done;
   /* Same field count/slot count, new type: cached descendants must rebuild. */
   parent_fields[1].type="I";parent_fields[1].kind='I';
   if (!dvm_gc_layout_ensure(NULL,&child) ||
       child.gc_layout->instance_count!=1 || child.gc_layout->instance_slots[0]!=3)
      goto done;
   parent_fields[1].type="Ljava/lang/Object;";parent_fields[1].kind='L';
   /* Same-class duplicate slot and inherited-slot overlap are both invalid. */
   child_fields[1].slot=3;
   if (dvm_gc_layout_ensure(NULL,&child)) goto done;
   child_fields[1].slot=1;
   if (dvm_gc_layout_ensure(NULL,&child)) goto done;
   child_fields[1].slot=4;
   parent_fields[0].width=2;
   if (dvm_gc_layout_ensure(NULL,&child)) goto done;
   parent_fields[0].width=1;
   if (!dvm_gc_layout_ensure(NULL,&child) || child.gc_layout->instance_count!=2)
      goto done;
   dvm_gc_layout_invalidate(&child);
   if (child.gc_layout || !dvm_gc_layout_ensure(NULL,&child)) goto done;
   gc_layout_stw_epoch=++gc_layout_next_stw_epoch;
   if(!dvm_gc_layout_ensure(NULL,&child)||child.gc_layout->stw_validated_epoch!=gc_layout_stw_epoch)goto done;
   struct dvm_gc_layout *same=child.gc_layout;
   if(!dvm_gc_layout_ensure(NULL,&child)||child.gc_layout!=same)goto done;
   dvm_gc_layout_invalidate(&child);
   if(child.gc_layout||!dvm_gc_layout_ensure(NULL,&child)||child.gc_layout->stw_validated_epoch!=gc_layout_stw_epoch)goto done;
   gc_layout_stw_epoch=0;
   parent_fields[1].type="I";parent_fields[1].kind='I';
   if(!dvm_gc_layout_ensure(NULL,&child)||child.gc_layout->instance_count!=1)goto done;
   ok=true;
done:
   gc_layout_stw_epoch=0;
   dvm_gc_layout_destroy(&child);dvm_gc_layout_destroy(&base);return ok;
}

uint32_t dvm_gc_ref_of(struct dvm *v,struct dvm_object *o){
 return gc_rev_ref_of(v,o);
}
