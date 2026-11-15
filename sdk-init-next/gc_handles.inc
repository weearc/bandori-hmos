/* GIL-owned identity handles. Tokens never contain guest body addresses.
 * Generation allocation is process-wide to reject cross-VM stale tokens. */
struct dvm_gc_handle_slot {
   dvm_ref ref;
   uint32_t generation,next_free,flags,weak_next;
   uint64_t identity;
   bool live;
};
struct dvm_gc_provider {
   char *name;unsigned kind;dvm_gc_root_fn fn;void *context;
};
struct leak_retired_pair {uint32_t array,box;uint64_t array_id,box_id;};
struct dvm_gc_state {
   struct dvm_gc_handle_slot *slots;
   uint32_t count,capacity,free_head,live,strong,borrows;
   struct dvm_gc_provider *providers;
   uint32_t providers_count,providers_capacity;
   bool visiting;
   struct dvm_gc_native_scope *scopes;
   dvm_ref *satb_queue; uint32_t satb_capacity,satb_head,satb_count;
   uint64_t satb_enqueued,satb_overflows; bool satb_active,satb_failed;
   struct leak_retired_pair leak_pairs[32];unsigned leak_pairs_count;
};
static _Thread_local struct dvm_gc_native_scope *gc_current_scope;
static _Thread_local uint32_t gc_guest_execution_depth;
static bool gc_core_roots(struct dvm *,dvm_gc_visit_fn,void *);
static void gc_root_failure(const char *provider,const char *detail){
   FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/gc-root-failures.csv","a+");
   if(!f)return;fseek(f,0,SEEK_END);if(!ftell(f))fputs("pid,provider,detail\n",f);
   char line[768];snprintf(line,sizeof line,"%d,%s,%s\n",getpid(),provider,detail?detail:"unknown");fputs(line,f);fclose(f);
}
static _Atomic uint64_t gc_handle_generation=1;

void dvm_gc_store_ref(struct dvm *vm,dvm_ref *slot,dvm_ref value,const char *site)
{
   if(!slot)return;
   struct dvm_gc_state *s=vm?vm->gc:NULL;
   if(s&&s->satb_active&&!s->satb_failed&&*slot){
      if(!s->satb_queue||s->satb_count>=s->satb_capacity){
         /* Invalidate collection, never silently discard the guest write.
          * No real incremental coordinator is enabled by this stage. */
         s->satb_overflows++;s->satb_failed=true;
         fprintf(stderr,"GC7_SATB_OVERFLOW site=%s capacity=%u collection_must_abort=1\n",site?site:"unknown",s->satb_capacity);
      } else {
         s->satb_queue[(s->satb_head+s->satb_count)%s->satb_capacity]=*slot;
         s->satb_count++;s->satb_enqueued++;
      }
   }
   *slot=value;
}

/* Collector bookkeeping must not create a new C borrow as a side effect. */
static struct dvm_object *gc_peek_object(struct dvm *vm,dvm_ref ref)
{
   if(!vm||!ref||ref>vm->heap_size)return NULL;
   struct dvm_object *o=heap_slot(vm,ref);
   /* Freed slots have a cleared header, not a corrupt forwarding pointer. */
   if(!o->live)return NULL;
   o=dvm_gc_resolve(vm,o);
   return o&&o->live?o:NULL;
}
/* Joint retirement avoids freeing the Long while a borrowed array still
 * contains it. Slots/payload never move; no tracing or STW is started here.
 * At admission no JNI local/global/strong handle may outlive the proof.
 * Raw borrows made by our readonly adapters may drain later. */
bool dvm_leak_retire_pair(struct dvm *vm,uint32_t array,uint32_t box,uint64_t ai,uint64_t bi){
 struct dvm_gc_state *s=vm?vm->gc:NULL;
 if(!s||!dvm_gil_held(vm)||!dvm_leak_collection_off(vm)||s->leak_pairs_count>=32)return false;
 struct dvm_object *a=gc_peek_object(vm,array),*b=gc_peek_object(vm,box);
 if(!a||!b||a->identity_generation!=ai||b->identity_generation!=bi||
    a->pins!=1||b->pins||a->gc_strong_handles||b->gc_strong_handles||
    a->monitor_depth||b->monitor_depth||a->kind!=DVM_OBJ_ARRAY||
    a->elem_kind!='L'||a->length!=1||!a->data||((uint32_t*)a->data)[0]!=box)return false;
 for(unsigned i=0;i<32;i++)if(!s->leak_pairs[i].array){
  ++a->pins;++b->pins;
  s->leak_pairs[i]=(struct leak_retired_pair){array,box,ai,bi};++s->leak_pairs_count;return true;
 }
 return false;
}
void dvm_leak_reap_pairs(struct dvm *vm){
 struct dvm_gc_state *s=vm?vm->gc:NULL;
 if(!s||!dvm_gil_held(vm)||!s->leak_pairs_count||!dvm_leak_collection_off(vm))return;
 for(unsigned i=0;i<32;i++){
  struct leak_retired_pair *p=&s->leak_pairs[i];if(!p->array)continue;
  struct dvm_object *a=gc_peek_object(vm,p->array),*b=gc_peek_object(vm,p->box);
  if(!a||!b||a->identity_generation!=p->array_id||b->identity_generation!=p->box_id||
     a->pins!=1||b->pins!=1||a->gc_strong_handles||b->gc_strong_handles||
     a->gc_body_borrows||a->gc_payload_borrows||b->gc_body_borrows||b->gc_payload_borrows||
     a->monitor_depth||b->monitor_depth)continue;
  uint32_t array=p->array,box=p->box;
  --a->pins;--b->pins;*p=(struct leak_retired_pair){0};--s->leak_pairs_count;
  heap_free(vm,array);heap_free(vm,box);leak_scope_reaped();
 }
}
static bool gc_access_ok(struct dvm *vm)
{
   return vm && (vm->gc_initializing || vm->gc_destroying || dvm_gil_held(vm));
}
static struct dvm_gc_state *gc_state_ensure(struct dvm *vm)
{
   if (!gc_access_ok(vm)) return NULL;
   if (!vm->gc) vm->gc=calloc(1,sizeof *vm->gc);
   if (!vm->gc && !dvm_exception(vm))
      dvm__throw(vm,"java/lang/OutOfMemoryError","GC handle registry");
   return vm->gc;
}

struct dvm_object *dvm_gc_resolve(struct dvm *vm,struct dvm_object *o)
{
   if (!vm || !o) return NULL;
   struct dvm_object *next=atomic_load_explicit(&o->gc_self,memory_order_acquire);
   if (next==o) return o;
   uint32_t hops=0;
   while(next && next!=o) {
      if (++hops>vm->heap_size) {
         fprintf(stderr,"GC_CORRUPTION reason=forwarding_cycle max_hops=%u\n",vm->heap_size);
         return NULL;
      }
      o=next;next=atomic_load_explicit(&o->gc_self,memory_order_acquire);
   }
   if (!next) fprintf(stderr,"GC_CORRUPTION reason=null_forwarding_pointer\n");
   return next?o:NULL;
}

dvm_gc_handle dvm_gc_handle_new(struct dvm *vm,uint32_t ref,unsigned flags)
{
   if(leak_scope_current&&(flags&DVM_GC_HANDLE_WEAK)&&leak_scope_mentions(vm,ref,NULL,0))leak_scope_cancel(vm,LEAK_STORE);
   if (!gc_access_ok(vm) || !ref) return 0;
   if ((flags&~15u) || !(flags&3u) || (flags&3u)==3u ||
       ((flags&12u)&&!(flags&DVM_GC_HANDLE_STRONG))) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","GC handle flags");return 0;
   }
   struct dvm_object *o=dvm__obj(vm,ref);
   if (!o) {dvm__throw(vm,"java/lang/IllegalArgumentException","GC handle dead identity");return 0;}
   struct dvm_gc_state *s=gc_state_ensure(vm);if(!s)return 0;
   if ((flags&1u)&&o->gc_strong_handles==UINT32_MAX) goto overflow;
   if ((flags&4u)&&o->gc_body_borrows==UINT32_MAX) goto overflow;
   if ((flags&8u)&&o->gc_payload_borrows==UINT32_MAX) goto overflow;
   if (!s->free_head && s->count==s->capacity) {
      uint32_t cap=s->capacity?s->capacity*2u:64u;
      if (cap<s->capacity || (uint64_t)cap*sizeof *s->slots>SIZE_MAX) goto overflow;
      void *slots=realloc(s->slots,(size_t)cap*sizeof *s->slots);
      if(!slots)goto oom;
      s->slots=slots;memset(s->slots+s->capacity,0,(cap-s->capacity)*sizeof *s->slots);s->capacity=cap;
   }
   uint64_t generation=atomic_fetch_add(&gc_handle_generation,1);
   if (!generation || generation>UINT32_MAX) goto overflow;
   uint32_t index;
   if (s->free_head) {index=s->free_head-1;s->free_head=s->slots[index].next_free;}
   else index=s->count++;
   s->slots[index]=(struct dvm_gc_handle_slot){.ref=ref,.generation=(uint32_t)generation,
      .flags=flags,.identity=o->identity_generation,.live=true};
   if(flags&2u){s->slots[index].weak_next=o->gc_weak_head;o->gc_weak_head=index+1u;}
   ++s->live;
   if(flags&1u){++s->strong;++o->gc_strong_handles;}
   if(flags&4u)++o->gc_body_borrows;
   if(flags&8u)++o->gc_payload_borrows;
   if(flags&12u)++s->borrows;
   return (generation<<32)|(uint64_t)(index+1u);
overflow:
   dvm__throw(vm,"java/lang/OutOfMemoryError","GC handle identity/counter exhausted");return 0;
oom:
   dvm__throw(vm,"java/lang/OutOfMemoryError","GC handle slots");return 0;
}
static struct dvm_gc_handle_slot *gc_handle_slot(struct dvm *vm,dvm_gc_handle h)
{
   if (!gc_access_ok(vm) || !vm->gc || !h) return NULL;
   uint32_t index=(uint32_t)h;
   if (!index || index>vm->gc->count) return NULL;
   struct dvm_gc_handle_slot *p=&vm->gc->slots[index-1];
   return p->live && p->generation==(uint32_t)(h>>32)?p:NULL;
}
uint32_t dvm_gc_handle_ref(struct dvm *vm,dvm_gc_handle h)
{
   struct dvm_gc_handle_slot *p=gc_handle_slot(vm,h);if(!p)return 0;
   struct dvm_object *o=dvm__obj(vm,p->ref);
   return o && o->identity_generation==p->identity?p->ref:0;
}
static bool gc_weak_unlink(struct dvm_gc_state *s,struct dvm_object *o,uint32_t index)
{
   uint32_t *link=&o->gc_weak_head,hops=0;
   while(*link && *link!=index){
      if(*link>s->count || ++hops>s->count)goto corrupt;
      struct dvm_gc_handle_slot *p=&s->slots[*link-1];
      if(!p->live || !(p->flags&DVM_GC_HANDLE_WEAK) || p->identity!=o->identity_generation)goto corrupt;
      link=&p->weak_next;
   }
   if(!*link || index>s->count)goto corrupt;
   *link=s->slots[index-1].weak_next;s->slots[index-1].weak_next=0;return true;
corrupt:
   fprintf(stderr,"GC_CORRUPTION reason=weak_handle_chain\n");return false;
}
bool dvm_gc_handle_drop(struct dvm *vm,dvm_gc_handle h)
{
   struct dvm_gc_handle_slot *p=gc_handle_slot(vm,h);if(!p)return false;
   struct dvm_gc_state *s=vm->gc;struct dvm_object *o=gc_peek_object(vm,p->ref);
   if (o && o->identity_generation==p->identity) {
      if(p->flags&1u)--o->gc_strong_handles;
      if(p->flags&4u)--o->gc_body_borrows;
      if(p->flags&8u)--o->gc_payload_borrows;
      if((p->flags&2u)&&!gc_weak_unlink(s,o,(uint32_t)h))return false;
   } else if(p->flags&1u) {
      fprintf(stderr,"GC_CORRUPTION reason=strong_handle_lost_identity\n");return false;
   }
   if(p->flags&1u)--s->strong;if(p->flags&12u)--s->borrows;
   --s->live;uint32_t index=(uint32_t)h;
   memset(p,0,sizeof *p);p->next_free=s->free_head;s->free_head=index;return true;
}

bool dvm_gc_root_provider_add(struct dvm *vm,const char *name,unsigned kind,dvm_gc_root_fn fn,void *context)
{
   if (!name || !*name || !fn || (kind!=1u&&kind!=2u)) return false;
   struct dvm_gc_state *s=gc_state_ensure(vm);if(!s||s->visiting)return false;
   for(uint32_t i=0;i<s->providers_count;++i){struct dvm_gc_provider *p=&s->providers[i];
      if(p->kind==kind&&p->context==context&&!strcmp(p->name,name))return p->fn==fn;
   }
   if(s->providers_count==s->providers_capacity){
      uint32_t cap=s->providers_capacity?s->providers_capacity*2u:16u;
      if(cap<s->providers_capacity)goto oom;
      void *p=realloc(s->providers,(size_t)cap*sizeof *s->providers);if(!p)goto oom;
      s->providers=p;s->providers_capacity=cap;
   }
   char *copy=strdup(name);if(!copy)goto oom;
   s->providers[s->providers_count++]=(struct dvm_gc_provider){copy,kind,fn,context};return true;
oom:
   if(!dvm_exception(vm))dvm__throw(vm,"java/lang/OutOfMemoryError","GC root providers");return false;
}
void dvm_gc_root_provider_remove(struct dvm *vm,const char *name,void *context)
{
   if(!gc_access_ok(vm)||!vm->gc||!name)return;struct dvm_gc_state *s=vm->gc;
   if(s->visiting){fprintf(stderr,"GC_REGISTRY_ERROR reason=remove_during_visit name=%s\n",name);return;}
   for(uint32_t i=0;i<s->providers_count;){struct dvm_gc_provider *p=&s->providers[i];
      if(p->context==context&&!strcmp(p->name,name)){
         free(p->name);*p=s->providers[--s->providers_count];
      }else ++i;
   }
}
bool dvm_gc_visit_roots(struct dvm *vm,unsigned kind,dvm_gc_visit_fn visit,void *user)
{
   if(!gc_access_ok(vm)||!visit||(kind!=1u&&kind!=2u))return false;
   struct dvm_gc_state *s=vm->gc;if(!s)return kind==1u?gc_core_roots(vm,visit,user):true;if(s->visiting)return false;
   s->visiting=true;bool ok=true;
   if(kind==1u&&!gc_core_roots(vm,visit,user))ok=false;
   if(kind==1u)for(struct dvm_gc_native_scope *q=s->scopes;q;q=q->next_vm){
      if(q->failed){ok=false;gc_root_failure("native_scope","allocation_or_capture_failed");}
      for(uint32_t i=0;i<q->count;++i){struct dvm_gc_scope_ref *r=&q->refs[i];
         struct dvm_object *o=dvm__obj(vm,r->ref);
         if(o&&o->identity_generation==r->identity)visit(&r->ref,user);
      }
   }
   for(uint32_t i=0;i<s->count;++i){struct dvm_gc_handle_slot *p=&s->slots[i];
      if(!p->live || !(p->flags&kind))continue;
      struct dvm_object *o=dvm__obj(vm,p->ref);
      if(!o||o->identity_generation!=p->identity){if(kind==1u){ok=false;gc_root_failure("handle_table","strong_identity_invalid");}else p->ref=0;continue;}
      dvm_ref original=p->ref;
      visit(&p->ref,user);
      /* Guest refs are stable IDs, not movable body addresses. A weak pass
       * may clear an ID; it cannot replace it without relinking identities. */
      if(p->ref!=original && (kind!=DVM_GC_ROOT_WEAK || p->ref)){
         fprintf(stderr,"GC_ROOT_FAILURE provider=handle_table reason=stable_id_rewrite\n");
         p->ref=original;ok=false;
      }
      /* Clearing a weak root must detach its slot before that token can be
       * dropped/reused, even if the observed object remains alive. */
      if(kind==DVM_GC_ROOT_WEAK&&!p->ref&&!gc_weak_unlink(s,o,i+1u))ok=false;
   }
   for(uint32_t i=0;i<s->providers_count;++i){struct dvm_gc_provider *p=&s->providers[i];
      if(p->kind==kind&&!p->fn(vm,visit,user,p->context)){ok=false;gc_root_failure(p->name,"provider_returned_false");fprintf(stderr,"GC_ROOT_FAILURE provider=%s kind=%u\n",p->name,kind);}
   }
   s->visiting=false;return ok;
}
void dvm_gc_forget_identity(struct dvm *vm,uint32_t ref,uint64_t identity)
{
   /* Weak slots are identity checked on resolution/root visitation. Eagerly
    * clear our own table before the ID can be reused, without calling foreign
    * providers under an arbitrary destructor's lock. Providers do the same
    * generation check before exposing or using their weak IDs. */
   struct dvm_gc_state *s=vm->gc;if(!s)return;
   struct dvm_object *o=gc_peek_object(vm,ref);
   if(!o||o->identity_generation!=identity)return;
   uint32_t at=o->gc_weak_head;o->gc_weak_head=0;
   while(at){struct dvm_gc_handle_slot *p=&s->slots[at-1];uint32_t next=p->weak_next;
      if(p->live&&(p->flags&2u)&&p->ref==ref&&p->identity==identity){p->ref=0;p->weak_next=0;}
      at=next;
   }
}
void dvm_gc_handle_counts(struct dvm *vm,uint32_t *live,uint32_t *strong,uint32_t *borrows)
{
   struct dvm_gc_state *s=vm?vm->gc:NULL;
   if(live)*live=s?s->live:0;if(strong)*strong=s?s->strong:0;if(borrows)*borrows=s?s->borrows:0;
}
void dvm_gc_handles_destroy(struct dvm *vm)
{
   struct dvm_gc_state *s=vm?vm->gc:NULL;if(!s)return;
   for(unsigned i=0;i<32;i++){
      struct leak_retired_pair *p=&s->leak_pairs[i];
      struct dvm_object *a=gc_peek_object(vm,p->array),*b=gc_peek_object(vm,p->box);
      if(a&&a->identity_generation==p->array_id&&a->pins)--a->pins;
      if(b&&b->identity_generation==p->box_id&&b->pins)--b->pins;
   }
   for(uint32_t i=0;i<s->count;++i)if(s->slots[i].live)
      dvm_gc_handle_drop(vm,((uint64_t)s->slots[i].generation<<32)|(i+1u));
   for(uint32_t i=0;i<s->providers_count;++i)free(s->providers[i].name);
   free(s->satb_queue);free(s->providers);free(s->slots);free(s);vm->gc=NULL;
}

void dvm_gc_native_touch(struct dvm *vm,uint32_t ref,struct dvm_object *o)
{
   struct dvm_gc_native_scope *q=gc_current_scope;
   if(!q||q->vm!=vm||q->guest_depth!=gc_guest_execution_depth||!q->active||q->failed||!ref||!o||
      (vm->gc&&vm->gc->visiting))return;
   for(uint32_t i=0;i<q->count;++i)
      if(q->refs[i].ref==ref&&q->refs[i].identity==o->identity_generation)return;
   if(q->count==q->capacity){
      uint32_t cap=q->capacity*2u;
      if(cap<q->capacity)goto oom;
      struct dvm_gc_scope_ref *p;
      if(q->refs==q->inline_refs){p=malloc((size_t)cap*sizeof *p);if(p)memcpy(p,q->refs,q->count*sizeof *p);}
      else p=realloc(q->refs,(size_t)cap*sizeof *p);
      if(!p)goto oom;q->refs=p;q->capacity=cap;
   }
   if(o->gc_body_borrows==UINT32_MAX || o->gc_payload_borrows==UINT32_MAX)goto oom;
   /* C runtimes can retain the returned body and any payload/slots address
    * until scope exit, including across a reentrant guest call or GIL yield.
    * Pin only the touched objects, never the entire heap or all C activity. */
   ++o->gc_body_borrows;++o->gc_payload_borrows;
   q->refs[q->count++]=(struct dvm_gc_scope_ref){ref,o->identity_generation};return;
oom:
   q->failed=true;
   if(!dvm_exception(vm))dvm__throw(vm,"java/lang/OutOfMemoryError","GC native temporary roots");
}
bool dvm_gc_native_enter(struct dvm *vm,struct dvm_gc_native_scope *q,uint32_t self,const char *sig,const uint32_t *slots,int nslots)
{
   if(!q)return false;memset(q,0,sizeof *q);
   struct dvm_gc_state *s=gc_state_ensure(vm);
   if(!s||!q||!sig||sig[0]!='('||nslots<0||(!slots&&nslots))return false;
   q->vm=vm;q->refs=q->inline_refs;q->capacity=32;
   q->guest_depth=gc_guest_execution_depth;
   q->previous_thread=gc_current_scope;q->next_vm=s->scopes;
   s->scopes=q;gc_current_scope=q;q->active=true;
   if(self)dvm__obj(vm,self);
   const char *p=sig+1;int index=0;
   while(*p&&*p!=')'){
      char kind=*p;bool reference=kind=='L'||kind=='[';
      if(kind=='['){while(*p=='[')++p;if(*p=='L'){while(*p&&*p!=';')++p;if(*p)++p;}else if(*p)++p;}
      else if(kind=='L'){while(*p&&*p!=';')++p;if(*p)++p;}
      else ++p;
      if(index>=nslots){q->failed=true;break;}
      if(reference&&slots[index])dvm__obj(vm,slots[index]);
      index+=(kind=='J'||kind=='D')?2:1;
   }
   return !q->failed;
}
void dvm_gc_native_leave(struct dvm_gc_native_scope *q)
{
   if(!q||!q->active)return;struct dvm_gc_state *s=q->vm->gc;
   struct dvm_gc_native_scope **p=&s->scopes;while(*p&&*p!=q)p=&(*p)->next_vm;
   if(*p)*p=q->next_vm;
   if(gc_current_scope!=q)fprintf(stderr,"GC_SCOPE_ERROR reason=non_lifo\n");
   else gc_current_scope=q->previous_thread;
   /* No dvm__obj here: that would register cleanup accesses in the parent
    * scope and turn nested cleanup into an ever-growing borrow chain. */
   for(uint32_t i=0;i<q->count;++i){
      struct dvm_gc_scope_ref *r=&q->refs[i];
      struct dvm_object *o=r->ref&&r->ref<=q->vm->heap_size?
         dvm_gc_resolve(q->vm,heap_slot(q->vm,r->ref)):NULL;
      if(!o||!o->live||o->identity_generation!=r->identity ||
         !o->gc_body_borrows||!o->gc_payload_borrows){
         fprintf(stderr,"GC_CORRUPTION reason=native_borrow_lost_identity\n");continue;
      }
      --o->gc_body_borrows;--o->gc_payload_borrows;
      if(!o->gc_body_borrows&&!o->gc_payload_borrows&&
         (o->gc_flags&DVM_GC_DEFERRED_FREE))heap_free(q->vm,r->ref);
   }
   if(q->refs!=q->inline_refs)free(q->refs);q->active=false;
   if(s->leak_pairs_count)dvm_leak_reap_pairs(q->vm);
}
bool dvm_gc_has_native_scopes(struct dvm *vm)
{
   return vm&&vm->gc&&vm->gc->scopes;
}

/* A C caller can keep the returned reference across a later guest call. Hand
 * ownership to its restored scope, rather than dropping the child's last root. */
void dvm_gc_native_leave_result(struct dvm_gc_native_scope *q,const char *sig,uint32_t ref)
{
   struct dvm *vm=q?q->vm:NULL;
   const char *ret=sig?strchr(sig,')'):NULL;
   bool reference=ret&&(ret[1]=='L'||ret[1]=='[');
   bool same_c_parent=vm&&q->active&&q->previous_thread&&q->previous_thread->vm==vm&&
      q->previous_thread->guest_depth==gc_guest_execution_depth;
   if(vm&&reference&&ref&&!same_c_parent){
      /* A returned object escapes to guest registers or the host/JNI caller.
       * A formerly local discard is no longer valid: leave it for tracing GC
       * instead of freeing an escaped object when its final C borrow ends.
       * This cancels only explicit temporary cleanup, never marking/finalize. */
      struct dvm_object *o=gc_peek_object(vm,ref);
      if(o)o->gc_flags&=~DVM_GC_DEFERRED_FREE;
   }
   /* Transfer before releasing the child's borrow: a deferred discard must
    * not reclaim an object in the handoff window. */
   if(vm&&reference&&ref&&same_c_parent){
      struct dvm_gc_native_scope *saved=gc_current_scope;
      gc_current_scope=q->previous_thread;dvm__obj(vm,ref);gc_current_scope=saved;
   }
   dvm_gc_native_leave(q);
   if(vm&&reference&&ref)dvm__obj(vm,ref);
}

struct gc2_root_check {struct dvm *vm;dvm_ref wanted;unsigned seen,invalid;};
static void gc2_root_check_visit(dvm_ref *slot,void *user)
{
   struct gc2_root_check *s=user;
   if(!slot){++s->invalid;return;}if(!*slot)return;
   if(!dvm__obj(s->vm,*slot)){++s->invalid;return;}
   if(*slot==s->wanted)++s->seen;
}
struct gc2_weak_clear {dvm_ref *target;unsigned cleared;};
static void gc2_weak_clear_visit(dvm_ref *slot,void *user)
{
   struct gc2_weak_clear *s=user;
   if(slot==s->target){*slot=0;++s->cleared;}
}
static bool gc2_scope_contains(const struct dvm_gc_native_scope *scope,dvm_ref ref)
{
   for(uint32_t i=0;i<scope->count;++i)if(scope->refs[i].ref==ref)return true;
   return false;
}
static struct dvm_method *gc2_platform_method(struct dvm *vm,struct dvm_class *cls,
   const char *name,const char *sig,char ret,int count,struct dvm_method *storage,FILE *log)
{
   struct dvm_method *m=cls?dvm_find_method(vm,cls,name,sig):NULL;
   dprintf(fileno(log),"GC2_METHOD_RESOLVE class=%s method=%s sig=%s class_found=%d lookup_found=%d platform_descriptor=%d\n",
      cls&&cls->name?cls->name:"?",name,sig,cls!=NULL,m!=NULL,cls&&!m);
   if(m||!cls)return m;
   /* Boot methods need not have a guest method-table entry. This descriptor
    * still runs dvm_call -> dvm__call_out -> the real platform dispatcher. */
   *storage=(struct dvm_method){.cls=cls,.name=name,.sig=(char *)sig,.ret_kind=ret,
      .access=1,.arg_count=count,.arg_slots=count};
   return storage;
}
static bool gc2_call_logged(struct dvm *vm,struct dvm_method *method,dvm_ref self,
   const union dvm_value *args,int count,union dvm_value *out,FILE *log,const char *stage)
{
   memset(out,0,sizeof *out);
   bool ok=method&&dvm_call(vm,method,self,args,count,out);
   dprintf(fileno(log),"GC2_CALL stage=%s method_found=%d ok=%d value_i=%d value_ref=%u exception=%u\n",
      stage,method!=NULL,ok,out->i,out->l,dvm_exception(vm));
   return ok&&!dvm_exception(vm);
}
bool dvm_gc_handles_selftest(struct dvm *vm,const char *path)
{
   extern bool dvm_gc_execution_roots_selftest(struct dvm *);
   extern bool dvm_gc_external_static_selftest(struct dvm *);
   if(!gc_access_ok(vm)||dvm_exception(vm))return false;
   FILE *f=fopen(path,"w");if(!f)return false;
   unsigned checks=0;const char *stage="entry";bool ok=false;
   dvm_ref ref=0,temp=0,reused=0;dvm_gc_handle a=0,b=0,w=0,w2=0;
   dvm_ref returned=0,primitive=0,field_owner=0,field_ref=0,static_ref=0,builder=0,builder_result=0;
   struct dvm_gc_native_scope scope={0},child_scope={0};
   struct dvm_class field_class={.name="gc2/selftest/FieldOwner",.init_state=2,.islots=2,.nsslots=2};
   struct dvm_field instance_fields[2]={
      {.cls=&field_class,.name="reference",.type="Ljava/lang/String;",.kind='L',.width=1,.slot=0},
      {.cls=&field_class,.name="primitive",.type="I",.kind='I',.width=1,.slot=1}};
   struct dvm_field static_fields[2]={
      {.cls=&field_class,.name="reference",.type="Ljava/lang/String;",.kind='L',.width=1,.slot=0,.access=DEX_ACC_STATIC},
      {.cls=&field_class,.name="primitive",.type="I",.kind='I',.width=1,.slot=1,.access=DEX_ACC_STATIC}};
   union dvm_value field_statics[2]={{0},{0}},value={0};
   field_class.ifields=instance_fields;field_class.nifields=2;
   field_class.sfields=static_fields;field_class.nsfields=2;field_class.sslots=field_statics;
#define GC2_CHECK(label,test) do{stage=label;if(!(test))goto done;++checks;dprintf(fileno(f),"PASS %s\n",stage);}while(0)
   ref=dvm_new_array(vm,'B',"B",8);
   GC2_CHECK("actual_vm_object",ref!=0);
   struct dvm_object *o=dvm__obj(vm,ref);uint64_t identity=o->identity_generation;
   uint32_t oldpins=o->pins;
   a=dvm_gc_handle_new(vm,ref,DVM_GC_HANDLE_STRONG);
   b=dvm_gc_handle_new(vm,ref,DVM_GC_HANDLE_STRONG|DVM_GC_HANDLE_BORROW_PAYLOAD);
   w=dvm_gc_handle_new(vm,ref,DVM_GC_HANDLE_WEAK);
   GC2_CHECK("strong_weak_borrow_identity_handles",a&&b&&w&&dvm_gc_handle_ref(vm,a)==ref&&dvm_gc_handle_ref(vm,b)==ref&&dvm_gc_handle_ref(vm,w)==ref);
   GC2_CHECK("liveness_separate_from_movement_borrow",o->gc_strong_handles==2&&o->gc_payload_borrows==1&&!o->gc_body_borrows&&o->pins==oldpins);
   heap_free(vm,ref);
   GC2_CHECK("strong_handle_prevents_temporary_free",dvm__obj(vm,ref)==o);
   struct gc2_root_check roots={.vm=vm,.wanted=ref};
   GC2_CHECK("strong_root_enumeration",dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&roots.seen>=2&&!roots.invalid);
   w2=dvm_gc_handle_new(vm,ref,DVM_GC_HANDLE_WEAK);
   GC2_CHECK("weak_chain_two_live_handles",w2&&o->gc_weak_head==(uint32_t)w2&&gc_handle_slot(vm,w2)->weak_next==(uint32_t)w);
   struct gc2_weak_clear clear={.target=&gc_handle_slot(vm,w)->ref};
   GC2_CHECK("weak_visitor_clears_and_unlinks_tail",dvm_gc_visit_roots(vm,DVM_GC_ROOT_WEAK,gc2_weak_clear_visit,&clear)&&clear.cleared==1&&!dvm_gc_handle_ref(vm,w)&&dvm_gc_handle_ref(vm,w2)==ref&&!gc_handle_slot(vm,w2)->weak_next);
   uint32_t cleared_index=(uint32_t)w;
   GC2_CHECK("weak_visitor_cleared_token_drop",dvm_gc_handle_drop(vm,w));w=0;
   w=dvm_gc_handle_new(vm,ref,DVM_GC_HANDLE_WEAK);
   GC2_CHECK("weak_cleared_slot_reuse_has_no_cycle",w&&(uint32_t)w==cleared_index&&o->gc_weak_head==(uint32_t)w&&gc_handle_slot(vm,w)->weak_next==(uint32_t)w2&&!gc_handle_slot(vm,w2)->weak_next);
   clear=(struct gc2_weak_clear){.target=&gc_handle_slot(vm,w)->ref};
   GC2_CHECK("weak_visitor_clears_and_unlinks_head",dvm_gc_visit_roots(vm,DVM_GC_ROOT_WEAK,gc2_weak_clear_visit,&clear)&&clear.cleared==1&&o->gc_weak_head==(uint32_t)w2&&dvm_gc_handle_ref(vm,w2)==ref);
   GC2_CHECK("weak_head_cleared_token_drop",dvm_gc_handle_drop(vm,w));w=0;
   GC2_CHECK("weak_remaining_chain_drop",dvm_gc_handle_drop(vm,w2)&&!o->gc_weak_head);w2=0;
   w=dvm_gc_handle_new(vm,ref,DVM_GC_HANDLE_WEAK);
   GC2_CHECK("weak_recreated_for_identity_reuse",w&&o->gc_weak_head==(uint32_t)w&&!gc_handle_slot(vm,w)->weak_next);
   GC2_CHECK("release_borrow",dvm_gc_handle_drop(vm,b)&&!o->gc_payload_borrows&&o->gc_strong_handles==1);b=0;
   dvm_gc_handle stale=a;
   GC2_CHECK("release_strong",dvm_gc_handle_drop(vm,a)&&!o->gc_strong_handles);a=0;
   GC2_CHECK("stale_token_rejected",!dvm_gc_handle_ref(vm,stale)&&!dvm_gc_handle_drop(vm,stale));
   heap_free(vm,ref);reused=dvm_new_array(vm,'B',"B",8);
   GC2_CHECK("weak_identity_cleared_before_reuse",reused==ref&&dvm__obj(vm,reused)->identity_generation!=identity&&!dvm_gc_handle_ref(vm,w));ref=0;
   GC2_CHECK("cleared_weak_handle_releasable",dvm_gc_handle_drop(vm,w));w=0;
   GC2_CHECK("native_scope_enter",dvm_gc_native_enter(vm,&scope,0,"()V",NULL,0));
   temp=dvm_new_string(vm,"native-scope-root");
   GC2_CHECK("new_temporary_auto_registered",temp&&scope.count&&dvm_gc_has_native_scopes(vm));
   roots=(struct gc2_root_check){.vm=vm,.wanted=temp};
   GC2_CHECK("native_scope_root_enumeration",dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&roots.seen&&!roots.invalid);
   GC2_CHECK("native_scope_temporary_borrows_body_and_payload",gc_peek_object(vm,temp)->gc_body_borrows==1&&gc_peek_object(vm,temp)->gc_payload_borrows==1);
   dvm_gc_native_leave(&scope);
   GC2_CHECK("native_scope_exit_releases_temporary_borrows",gc_peek_object(vm,temp)&&!gc_peek_object(vm,temp)->gc_body_borrows&&!gc_peek_object(vm,temp)->gc_payload_borrows);
   GC2_CHECK("return_parent_scope_enter",dvm_gc_native_enter(vm,&scope,0,"()V",NULL,0));
   GC2_CHECK("return_child_scope_enter",dvm_gc_native_enter(vm,&child_scope,0,"()Ljava/lang/String;",NULL,0));
   returned=dvm_new_string(vm,"gc2-child-return");
   GC2_CHECK("return_child_owns_new_string",returned&&gc2_scope_contains(&child_scope,returned)&&!gc2_scope_contains(&scope,returned));
   heap_free(vm,returned);
   GC2_CHECK("child_return_discard_deferred_while_borrowed",gc_peek_object(vm,returned)&&(gc_peek_object(vm,returned)->gc_flags&DVM_GC_DEFERRED_FREE)&&gc_peek_object(vm,returned)->gc_body_borrows==1&&gc_peek_object(vm,returned)->gc_payload_borrows==1);
   dvm_gc_native_leave_result(&child_scope,"()Ljava/lang/String;",returned);
   GC2_CHECK("reference_return_handed_to_parent",gc2_scope_contains(&scope,returned));
   GC2_CHECK("reference_return_handoff_keeps_one_parent_borrow",gc_peek_object(vm,returned)&&gc_peek_object(vm,returned)->gc_body_borrows==1&&gc_peek_object(vm,returned)->gc_payload_borrows==1);
   /* A second real guest call must not erase the prior native return root. */
   struct dvm_class *string_class=dvm_find_class_or_external(vm,"java/lang/String");
   struct dvm_method length_storage;
   struct dvm_method *length_method=gc2_platform_method(vm,string_class,"length","()I",'I',0,&length_storage,f);
   GC2_CHECK("nested_real_guest_call_after_return",gc2_call_logged(vm,length_method,temp,NULL,0,&value,f,"nested_after_return")&&value.i==17);
   roots=(struct gc2_root_check){.vm=vm,.wanted=returned};
   GC2_CHECK("reference_return_survives_nested_call_root_walk",dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&roots.seen&&!roots.invalid);
   GC2_CHECK("primitive_child_scope_enter",dvm_gc_native_enter(vm,&child_scope,0,"()I",NULL,0));
   primitive=dvm_new_string(vm,"gc2-primitive-bits");
   GC2_CHECK("primitive_bits_start_only_in_child",primitive&&gc2_scope_contains(&child_scope,primitive)&&!gc2_scope_contains(&scope,primitive));
   uint32_t before_primitive=scope.count;
   dvm_gc_native_leave_result(&child_scope,"()I",primitive);
   GC2_CHECK("primitive_return_does_not_root_matching_object_id",scope.count==before_primitive&&!gc2_scope_contains(&scope,primitive));
   GC2_CHECK("primitive_return_releases_child_borrows",gc_peek_object(vm,primitive)&&!gc_peek_object(vm,primitive)->gc_body_borrows&&!gc_peek_object(vm,primitive)->gc_payload_borrows);
   roots=(struct gc2_root_check){.vm=vm,.wanted=primitive};
   GC2_CHECK("primitive_return_absent_from_root_walk",dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&!roots.seen&&!roots.invalid);
   /* The class is deliberately not registered: static/instance metadata does
    * not itself supply these values as VM roots and cannot mask getter touch. */
   GC2_CHECK("field_value_setup_scope_enter",dvm_gc_native_enter(vm,&child_scope,0,"()V",NULL,0));
   field_owner=dvm_new_object(vm,&field_class);
   field_ref=dvm_new_string(vm,"gc2-instance-field");static_ref=dvm_new_string(vm,"gc2-static-field");
   GC2_CHECK("real_vm_reference_and_primitive_fields",field_owner&&field_ref&&static_ref&&
      dvm_set_field(vm,field_owner,"reference","Ljava/lang/String;",(union dvm_value){.l=field_ref})&&
      dvm_set_field(vm,field_owner,"primitive","I",(union dvm_value){.i=(int32_t)primitive})&&
      dvm_set_static(vm,&field_class,"reference","Ljava/lang/String;",(union dvm_value){.l=static_ref})&&
      dvm_set_static(vm,&field_class,"primitive","I",(union dvm_value){.i=(int32_t)primitive}));
   dvm_gc_native_leave_result(&child_scope,"()V",0);
   GC2_CHECK("field_setup_does_not_leak_child_roots",!gc2_scope_contains(&scope,field_ref)&&!gc2_scope_contains(&scope,static_ref));
   GC2_CHECK("get_field_registers_extracted_reference",dvm_get_field(vm,field_owner,"reference","Ljava/lang/String;",&value)&&value.l==field_ref&&gc2_scope_contains(&scope,field_ref));
   GC2_CHECK("get_static_registers_extracted_reference",dvm_get_static(vm,&field_class,"reference","Ljava/lang/String;",&value)&&value.l==static_ref&&gc2_scope_contains(&scope,static_ref));
   GC2_CHECK("extracted_fields_have_parent_body_payload_borrows",gc_peek_object(vm,field_ref)->gc_body_borrows==1&&gc_peek_object(vm,field_ref)->gc_payload_borrows==1&&gc_peek_object(vm,static_ref)->gc_body_borrows==1&&gc_peek_object(vm,static_ref)->gc_payload_borrows==1);
   GC2_CHECK("get_field_primitive_does_not_register_reference",dvm_get_field(vm,field_owner,"primitive","I",&value)&&value.i==(int32_t)primitive&&!gc2_scope_contains(&scope,primitive));
   GC2_CHECK("get_static_primitive_does_not_register_reference",dvm_get_static(vm,&field_class,"primitive","I",&value)&&value.i==(int32_t)primitive&&!gc2_scope_contains(&scope,primitive));
   roots=(struct gc2_root_check){.vm=vm,.wanted=field_ref};
   GC2_CHECK("extracted_instance_field_visible_to_root_walk",dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&roots.seen&&!roots.invalid);
   roots=(struct gc2_root_check){.vm=vm,.wanted=static_ref};
   GC2_CHECK("extracted_static_field_visible_to_root_walk",dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&roots.seen&&!roots.invalid);
   /* Exercise dispatch-owned scopes as well as the explicit leave_result API. */
   struct dvm_class *builder_class=dvm_find_class_or_external(vm,"java/lang/StringBuilder");
   struct dvm_method ctor_storage,append_storage,string_storage;
   struct dvm_method *builder_ctor=gc2_platform_method(vm,builder_class,"<init>","()V",'V',0,&ctor_storage,f);
   struct dvm_method *builder_append=gc2_platform_method(vm,builder_class,"append","(Ljava/lang/String;)Ljava/lang/StringBuilder;",'L',1,&append_storage,f);
   struct dvm_method *builder_string=gc2_platform_method(vm,builder_class,"toString","()Ljava/lang/String;",'L',0,&string_storage,f);
   builder=builder_class?dvm_new_object(vm,builder_class):0;
   GC2_CHECK("real_stringbuilder_constructor",builder&&gc2_call_logged(vm,builder_ctor,builder,NULL,0,&value,f,"builder_ctor"));
   union dvm_value append_arg={.l=returned};
   GC2_CHECK("real_stringbuilder_append",gc2_call_logged(vm,builder_append,builder,&append_arg,1,&value,f,"builder_append")&&value.l==builder);
   GC2_CHECK("real_stringbuilder_reference_return",gc2_call_logged(vm,builder_string,builder,NULL,0,&value,f,"builder_toString")&&value.l);
   builder_result=value.l;
   GC2_CHECK("dvm_call_reference_return_owned_by_parent",gc2_scope_contains(&scope,builder_result));
   GC2_CHECK("nested_call_preserves_stringbuilder_return",gc2_call_logged(vm,length_method,temp,NULL,0,&value,f,"nested_after_builder")&&value.i==17);
   roots=(struct gc2_root_check){.vm=vm,.wanted=builder_result};
   GC2_CHECK("stringbuilder_return_visible_after_nested_call",dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&roots.seen&&!roots.invalid);
   heap_free(vm,field_owner);
   GC2_CHECK("borrowed_synthetic_owner_discard_is_deferred",gc_peek_object(vm,field_owner)&&(gc_peek_object(vm,field_owner)->gc_flags&DVM_GC_DEFERRED_FREE));
   dvm_gc_native_leave(&scope);
   bool return_discarded=!gc_peek_object(vm,returned),owner_discarded=!gc_peek_object(vm,field_owner);
   if(return_discarded)returned=0;
   if(owner_discarded)field_owner=0;
   GC2_CHECK("parent_exit_completes_deferred_return_and_owner_discard",return_discarded&&owner_discarded);
   GC2_CHECK("parent_exit_releases_extracted_field_borrows",gc_peek_object(vm,field_ref)&&gc_peek_object(vm,static_ref)&&!gc_peek_object(vm,field_ref)->gc_body_borrows&&!gc_peek_object(vm,field_ref)->gc_payload_borrows&&!gc_peek_object(vm,static_ref)->gc_body_borrows&&!gc_peek_object(vm,static_ref)->gc_payload_borrows);
   /* This is a barrier unit test, not a claimed collector/heap movement. */
   o=dvm__obj(vm,reused);struct dvm_object shadow;
   memcpy(&shadow,o,sizeof shadow);atomic_init(&shadow.gc_self,&shadow);
   atomic_store_explicit(&o->gc_self,&shadow,memory_order_release);
   bool resolved=dvm__obj(vm,reused)==&shadow;
   atomic_store_explicit(&o->gc_self,o,memory_order_release);
   GC2_CHECK("brooks_barrier_forwarding_unit",resolved&&dvm__obj(vm,reused)==o);
   GC2_CHECK("execution_frame_tstate_roots",dvm_gc_execution_roots_selftest(vm));
   GC2_CHECK("external_static_hook_reference_and_exception",dvm_gc_external_static_selftest(vm));
   extern bool java_basics_global_roots_selftest(struct dvm *);
   extern bool thread_extras_gc_selftest(struct dvm *);
   extern bool weak_runtime_gc_selftest(struct dvm *);
   GC2_CHECK("java_constant_cache_holder_roots",java_basics_global_roots_selftest(vm));
   GC2_CHECK("thread_default_root_owner_identity",thread_extras_gc_selftest(vm));
   GC2_CHECK("reference_payload_generation",weak_runtime_gc_selftest(vm));
   GC2_CHECK("ui_side_table_generation",dvm_gc_ui_side_identity_selftest(vm));
   extern bool activity_runtime_gc_identity_selftest(struct dvm *);
   extern bool view_state_runtime_gc_identity_selftest(struct dvm *);
   extern bool android_basics_gc_identity_selftest(struct dvm *);
   GC2_CHECK("activity_shadow_generation",activity_runtime_gc_identity_selftest(vm));
   GC2_CHECK("view_lookup_generation",view_state_runtime_gc_identity_selftest(vm));
   GC2_CHECK("phone_listener_generation",android_basics_gc_identity_selftest(vm));
   extern bool io_runtime_baos_selftest(struct dvm *);
   GC2_CHECK("baos_scoped_backing_no_persistent_pointer",io_runtime_baos_selftest(vm));
   GC2_CHECK("thread_native_registry",thread_runtime_gc_roots_selftest(vm));
   GC2_CHECK("executor_native_registry",executor_runtime_gc_roots_selftest(vm));
   ok=true;
done:
   dvm_gc_native_leave(&child_scope);
   dvm_gc_native_leave(&scope);
   if(a)dvm_gc_handle_drop(vm,a);if(b)dvm_gc_handle_drop(vm,b);if(w)dvm_gc_handle_drop(vm,w);if(w2)dvm_gc_handle_drop(vm,w2);
   heap_free(vm,ref);heap_free(vm,reused);heap_free(vm,temp);
   heap_free(vm,field_owner);heap_free(vm,field_ref);heap_free(vm,static_ref);
   heap_free(vm,builder);heap_free(vm,builder_result);heap_free(vm,returned);heap_free(vm,primitive);
   dvm_gc_layout_destroy(&field_class);
   dprintf(fileno(f),"GC2_HANDLES_SELFTEST %s pid=%d checks=%u failed_stage=%s actual_VM=1 collector_movement_test=0\n",ok?"PASS":"FAIL",getpid(),checks,ok?"none":stage);
   bool io_ok=!ferror(f);if(fclose(f))io_ok=false;return ok&&io_ok&&!dvm_exception(vm);
#undef GC2_CHECK
}
