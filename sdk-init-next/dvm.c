#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
extern void android_util_vm_end(struct dvm *vm);
extern void android_os_vm_end(struct dvm *vm);
extern void thread_extras_vm_end(struct dvm *vm) __attribute__((weak));
extern void weak_runtime_vm_end(struct dvm *vm) __attribute__((weak));
#include "battery_runtime.h"
#include "small_boxes.h"
#include "discovery_runtime.h"
#include "security_provider.h"
#include "ssl_runtime.h"
#include "socket_runtime.h"
#include "reflection_runtime.h"
#include "executor_runtime.h"
#include "thread_runtime.h"
#include "intent_filter_runtime.h"
/* Runtime-level class contracts the application DEX cannot declare; the
 * implementation lives in runtime-contract-next/runtime_contract.c. */
extern bool runtime_contract_vm_start(struct dvm *vm);
extern bool string_runtime_httpurl_ascii(struct dvm *,const uint32_t *,int,union dvm_value *);
extern bool string_runtime_header_value_ascii(struct dvm *,const uint32_t *,int);
extern bool string_runtime_header_absent(struct dvm *,dvm_ref,dvm_ref);
extern bool string_runtime_json_quoted(struct dvm *,dvm_ref,uint16_t,bool,union dvm_value *);
extern bool string_runtime_json_core(struct dvm *,dvm_ref,const char *,const char *,const uint32_t *,int,union dvm_value *);
extern bool string_runtime_json_nonwhite(struct dvm *,dvm_ref,union dvm_value *);
extern bool string_runtime_json_structure(struct dvm *,dvm_ref,union dvm_value *);
extern bool exception_hierarchy_class_exists(const char *desc);
/* I/O declaration inventory is shared with the actual native dispatcher. */
extern bool io_runtime_method_supported(const char *,const char *,const char *) __attribute__((weak));
extern void thread_runtime_vsync_tick(struct dvm *vm);
/* Optional overlay: retire view identities before heap/class storage can be
 * reused. Older builds without this module have no corresponding state. */
extern void view_state_runtime_vm_shutdown(struct dvm *vm) __attribute__((weak));
extern void window_attributes_vm_shutdown(struct dvm *vm) __attribute__((weak));
extern void display_runtime_vm_shutdown(struct dvm *vm) __attribute__((weak));
extern void task6j_diagnostic_catch(struct dvm *,struct dvm_method *,uint32_t,uint32_t) __attribute__((weak));
extern void surface_view_runtime_vm_shutdown(struct dvm *vm) __attribute__((weak));
extern void theme_runtime_vm_shutdown(struct dvm *vm) __attribute__((weak));
extern void layout_params_runtime_vm_shutdown(struct dvm *vm) __attribute__((weak));
/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Dalvik interpreter proper: heap, class loading, resolution and the
 * instruction loop.  See dvm.h for why this exists.
 */

#include "dvm/dvm_internal.h"
#include "next_arena.h"
#include "native_scratch.h"
#include "surface_runtime.h"
#include "perf_attr.h"
#include "leak_scope.h"
#include "page_geometry.h"
#include "java_basics.h"
#include <fcntl.h>







#include "portable.h"
#include <errno.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
/* Bounded diagnostic guard.  The old aggregate 100000 cap was below the
 * declared Application.onCreate stage budget (2000000) and was exhausted by
 * normal OkHttp/Activity work.  This is still a hard bound, not an infinite
 * loop escape; the trace makes the observed steps and limit auditable. */
#ifndef DVM_AGGREGATE_STEP_LIMIT
#define DVM_AGGREGATE_STEP_LIMIT 2000000ull
#endif
extern int sdk_trace_fprintf(FILE*,const char*,...);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define fprintf sdk_trace_fprintf
#define fputc sdk_trace_fputc

/* GIL-serialized, argument-free call ring. One spare slot prevents a signal
 * interrupting the writer from observing its partially written next entry. */
static char lifetime_calls[31][512];
static unsigned lifetime_lengths[31];
static atomic_uint lifetime_sequence;
static atomic_uint lifetime_dumping;
static unsigned large_after;
void dvm_large_allocation_note(size_t bytes,uintptr_t caller)
{
   static size_t sizes[12];static unsigned count;
   if(bytes<VM_EXTERNAL_THRESHOLD)return;
   for(unsigned i=0;i<count;i++)if(sizes[i]==bytes)return;
   if(count==12)return;sizes[count++]=bytes;
   FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/large-alloc.txt","a");
   if(!f)return;
   dprintf(fileno(f),"LARGE_ALLOC pid=%d mono_ms=%llu bytes=%zu caller=%p arguments=redacted\n",getpid(),(unsigned long long)dvm__now_ms(),bytes,(void *)caller);
   unsigned seq=atomic_load(&lifetime_sequence),first=seq>3?seq-3:0;
   for(unsigned i=first;i<seq;i++)(void)write(fileno(f),lifetime_calls[i%31],lifetime_lengths[i%31]);
   fclose(f);large_after=3;
}
void dvm_lifetime_signal_dump(int fd)
{
   atomic_fetch_add_explicit(&lifetime_dumping,1,memory_order_acq_rel);
   unsigned seq=atomic_load_explicit(&lifetime_sequence,memory_order_acquire);
   unsigned first=seq>30?seq-30:0;
   for(unsigned i=first;i<seq;i++){
      unsigned slot=i%31,len=lifetime_lengths[slot];
      if(len)(void)write(fd,lifetime_calls[slot],len);
   }
   atomic_fetch_sub_explicit(&lifetime_dumping,1,memory_order_release);
}
void dvm_lifetime_note(struct dvm *vm,const char *event)
{
   int saved=errno;
   FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/vm-lifetime.txt","a");
   if(f){
      struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
      dprintf(fileno(f),"VM_LIFETIME wall=%lld mono_ms=%llu pid=%d tid=%ld vm=%p steps=%llu event=%s caller=%p\n",
         (long long)time(NULL),(unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000,
         getpid(),(long)syscall(SYS_gettid),(void*)vm,vm?(unsigned long long)vm->steps:0,event,__builtin_return_address(0));
      dvm_lifetime_signal_dump(fileno(f));fclose(f);
   }
   errno=saved;
}
static void lifetime_call(struct dvm *vm,const struct dvm_method *m)
{
   /* No lock or allocation in a signal. An already-active writer can finish
    * its spare slot; later writers skip until all snapshot readers finish. */
   if(atomic_load_explicit(&lifetime_dumping,memory_order_acquire))return;
   unsigned seq=atomic_load_explicit(&lifetime_sequence,memory_order_relaxed),slot=seq%31;
   const char *owner=m&&m->cls?m->cls->name:"?",*name=m?m->name:"?";
   int n=snprintf(lifetime_calls[slot],sizeof lifetime_calls[slot],
      "GUEST_CALL seq=%u tid=%ld guest_thread=%u method=%s.%s%s\n",seq,
      (long)syscall(SYS_gettid),vm->cur_thread,owner,name,m&&m->sig?m->sig:"");
   lifetime_lengths[slot]=n<0?0:(unsigned)n<sizeof lifetime_calls[slot]?(unsigned)n:sizeof lifetime_calls[slot]-1;
   atomic_store_explicit(&lifetime_sequence,seq+1,memory_order_release);
   if(large_after){FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/large-alloc.txt","a");if(f){dprintf(fileno(f),"AFTER ");(void)write(fileno(f),lifetime_calls[slot],lifetime_lengths[slot]);fclose(f);}--large_after;}
   if((!strcmp(name,"exit")||!strcmp(name,"halt"))&&
         (!strcmp(owner,"java/lang/System")||!strcmp(owner,"java/lang/Runtime")))dvm_lifetime_note(vm,"guest_exit_api");
   if(!strcmp(name,"finish")||!strcmp(name,"finishAffinity")||
         (!strcmp(owner,"android/os/Process")&&!strcmp(name,"killProcess")))dvm_lifetime_note(vm,"guest_finish_or_kill_api");
}

#ifdef LUNARIA_UNITY_PLAYER_PROBE
/* Exclusive DEX instruction counts, local to the executing host thread.
 * A decreasing task budget identifies a new execution slice. Native work
 * contributes zero DEX instructions; no budget or business result changes. */
#define JSON_HOT_CAP 4096
#include "task_step_profile.inc"
struct json_hot_entry { const struct dvm_method *method; uint64_t steps; };
static _Thread_local struct {struct dvm *vm;dvm_ref thread;uint64_t last;unsigned overflow;struct json_hot_entry entries[JSON_HOT_CAP];} json_hot;
static void json_hot_tick(struct dvm *v,const struct dvm_method *m){
 if(json_hot.vm!=v||json_hot.thread!=v->cur_thread||v->call_steps<json_hot.last){memset(&json_hot,0,sizeof json_hot);json_hot.vm=v;json_hot.thread=v->cur_thread;}
 json_hot.last=v->call_steps;
 unsigned slot=(unsigned)(((uintptr_t)m>>4)*2654435761u)&(JSON_HOT_CAP-1);
 for(unsigned n=0;n<JSON_HOT_CAP;n++,slot=(slot+1)&(JSON_HOT_CAP-1)){
  struct json_hot_entry *e=&json_hot.entries[slot];if(!e->method||e->method==m){e->method=m;e->steps++;return;}
 }
 json_hot.overflow++;
}
static void json_hot_report(unsigned id,const struct json_hot_entry *before){
 int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/json-hotspots.txt","a");if(!f)return;
 dprintf(fileno(f),"JSON_HOT_REPORT pid=%d id=%u mono_ms=%llu\n",getpid(),id,(unsigned long long)dvm__now_ms());
 for(int scope=0;scope<2;scope++){
  unsigned picked[5];int count=0;
  for(int rank=0;rank<5;rank++){
   int best=-1;uint64_t maximum=0;
   for(unsigned i=0;i<JSON_HOT_CAP;i++){
    bool seen=false;for(int j=0;j<count;j++)if(picked[j]==i)seen=true;
    uint64_t steps=json_hot.entries[i].steps;
    if(scope&&before&&before[i].method==json_hot.entries[i].method)steps-=before[i].steps;
    if(!seen&&steps>maximum){maximum=steps;best=(int)i;}
   }
   if(best<0)break;picked[count++]=(unsigned)best;const struct dvm_method *m=json_hot.entries[best].method;
   dprintf(fileno(f),"SDK_JSON_HOT id=%u scope=%s rank=%d method=%s.%s%s steps=%llu thread=%u overflow=%u exclusive=1\n",id,scope?"parse":"task",rank+1,m->cls->name,m->name,m->sig,(unsigned long long)maximum,json_hot.thread,json_hot.overflow);
  }
 }
 fclose(f);errno=saved;
}
#endif

/* getenv() is a linear walk of the environment, comparing every entry: this
 * process runs with a large one, and the interpreter used to call it on paths
 * taken once per method call and once per string created.  Host profiles of a
 * Cross Worlds load screen found the emulator inside getenv() as often as
 * inside the bytecode loop itself.
 *
 * The answer cannot change while the process runs, so each site looks it up
 * once into an atomic slot.  The sentinel is the address of an object rather
 * than a cast integer, so the slot has a constant initialiser and NULL stays
 * available as a real answer ("the variable is not set").  Two threads racing
 * on a cold slot both call getenv() and store the same pointer, so neither
 * side needs a lock; reading a warm slot costs one relaxed load. */
static const char dvm__env_unread_marker;
#define DVM_ENV_UNREAD (&dvm__env_unread_marker)

static const char *dvm__env_once(_Atomic(const char *) *slot, const char *name)
{
   const char *v = atomic_load_explicit(slot, memory_order_relaxed);
   if (v == DVM_ENV_UNREAD) {
      v = getenv(name);
      atomic_store_explicit(slot, v, memory_order_relaxed);
   }
   return v;
}

/* The same idea for call sites that name the variable with a literal and have
 * no slot of their own: a table keyed by the literal's address.  Only for
 * the trace and profiling switches, which nothing changes after start-up;
 * the ANDROID_* launch settings are rewritten while an app runs (and by the
 * tests) and keep calling getenv().  Two threads racing on an empty entry both store the
 * same pointers; the key is published last, so a reader never sees a value
 * that is not its own. */
#define DVM_ENV_SLOTS 256u
static struct { _Atomic(const char *) key; _Atomic(const char *) val; } g_env_cache[DVM_ENV_SLOTS];

const char *dvm__env(const char *name)
{
   unsigned i = (unsigned)(((uintptr_t)name >> 3) * 2654435761u >> 8) & (DVM_ENV_SLOTS - 1u);
   for (unsigned n = 0; n < DVM_ENV_SLOTS; ++n, i = (i + 1u) & (DVM_ENV_SLOTS - 1u)) {
      const char *k = atomic_load_explicit(&g_env_cache[i].key, memory_order_acquire);
      if (k == name) return atomic_load_explicit(&g_env_cache[i].val, memory_order_relaxed);
      if (!k) {
         const char *v = getenv(name);
         atomic_store_explicit(&g_env_cache[i].val, v, memory_order_relaxed);
         atomic_store_explicit(&g_env_cache[i].key, name, memory_order_release);
         return v;
      }
   }
   return getenv(name);
}

/* The three the hot paths ask for. */
static const char *dvm__env_strwatch(void)
{
   static _Atomic(const char *) slot = DVM_ENV_UNREAD;
   return dvm__env_once(&slot, "LUNARIA_DVM_STRWATCH");
}
static const char *dvm__env_outtrace(void)
{
   static _Atomic(const char *) slot = DVM_ENV_UNREAD;
   return dvm__env_once(&slot, "LUNARIA_DVM_OUTTRACE");
}
static const char *dvm__env_trace_class(void)
{
   static _Atomic(const char *) slot = DVM_ENV_UNREAD;
   return dvm__env_once(&slot, "LUNARIA_DVM_TRACE_CLASS");
}

/* ------------------------------------------------------------------------ *
 * Descriptors
 * ------------------------------------------------------------------------ */

char dvm__kind_of(const char *desc)
{
   if (!desc || !*desc) return 'V';
   switch (*desc) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
      case 'J': case 'F': case 'D': case 'V':
         return *desc;
      default:
         return 'L';   /* object or array */
   }
}

int dvm__slots_of(char kind)
{
   return (kind == 'J' || kind == 'D') ? 2 : (kind == 'V' ? 0 : 1);
}

/* Walks one type descriptor, returning the position just past it. */
static const char *desc_skip(const char *p)
{
   while (*p == '[') ++p;
   if (*p == 'L') {
      while (*p && *p != ';') ++p;
      if (*p == ';') ++p;
   } else if (*p) {
      ++p;
   }
   return p;
}

int dvm_sig_arg_slots(const char *sig)
{
   if (!sig || *sig != '(') return -1;
   int n = 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      const char *q = desc_skip(p);
      if (q == p) return -1;
      n += dvm__slots_of(dvm__kind_of(p));
      p = q;
   }
   return n;
}

int dvm_sig_arg_count(const char *sig)
{
   if (!sig || *sig != '(') return -1;
   int n = 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      const char *q = desc_skip(p);
      if (q == p) return -1;
      ++n;
      p = q;
   }
   return n;
}

char dvm_sig_return_kind(const char *sig)
{
   const char *p = sig ? strchr(sig, ')') : NULL;
   return p ? dvm__kind_of(p + 1) : 'V';
}

/* Parameter descriptor `idx` of a signature, into `buf`. */
bool dvm__sig_param(const char *sig, int idx, char *buf, size_t sz)
{
   if (!sig || *sig != '(') return false;
   const char *p = sig + 1;
   for (int i = 0; *p && *p != ')'; ++i) {
      const char *q = desc_skip(p);
      if (i == idx) {
         size_t l = (size_t)(q - p);
         if (l + 1 > sz) return false;
         memcpy(buf, p, l);
         buf[l] = '\0';
         return true;
      }
      p = q;
   }
   return false;
}

/* JNI form ("com/foo/Bar") into a descriptor ("Lcom/foo/Bar;"). */
static void name_to_desc(const char *name, char *buf, size_t sz)
{
   if (!sz) return;
   if (!name) { buf[0] = '\0'; return; }
   const size_t n = strlen(name);
   const bool wrap = !((name[0] == 'L' && name[n - 1] == ';') || name[0] == '[' ||
                       (n == 1 && strchr("ZBCSIJFDV", name[0])));
   const size_t need = n + (wrap ? 2 : 0);
   if (need >= sz) {   /* truncate the way snprintf did */
      size_t room = sz - 1, k = 0;
      if (wrap && room) { buf[k++] = 'L'; --room; }
      if (room > n) room = n;
      memcpy(buf + k, name, room);
      buf[k + room] = '\0';
   } else if (wrap) {
      buf[0] = 'L';
      memcpy(buf + 1, name, n);
      buf[n + 1] = ';';
      buf[n + 2] = '\0';
   } else {
      memcpy(buf, name, n + 1);
   }
   /* Java's dotted form shows up in Class.forName() and in strings the app
    * hands us; the dex always uses slashes. */
   for (char *q = buf; *q; ++q)
      if (*q == '.') *q = '/';
}

/* ------------------------------------------------------------------------ *
 * Heap
 * ------------------------------------------------------------------------ */

/* The slot a reference names.  Objects live in fixed-size blocks and the
 * blocks are never moved or freed while the VM runs.
 *
 * The heap used to be one array grown with realloc(), which is fine while a
 * single thread interprets: nobody can allocate between one statement of a
 * built-in method and the next.  With bytecode running on host threads that
 * stops being true — a built-in holding `struct dvm_object *o` across a call
 * that lets another thread run would find `o` pointing into a freed block the
 * moment that thread allocated past the capacity.  Growing by block keeps
 * every pointer ever handed out valid for the life of the VM, which is what
 * makes those built-ins correct without auditing every one of them.
 *
 * Only the block table moves, and no caller ever holds a pointer into it. */
static struct dvm_object *heap_slot(struct dvm *vm, dvm_ref ref)
{
   uint32_t i = ref - 1u;
   return &vm->heap_blocks[i / DVM_HEAP_BLOCK][i % DVM_HEAP_BLOCK];
}

struct dvm_object *dvm__obj(struct dvm *vm, dvm_ref ref)
{
   if (!ref || ref > vm->heap_size) return NULL;
   struct dvm_object *o = heap_slot(vm, ref);
   if (!o->live) return NULL;
   o=dvm_gc_resolve(vm,o);
   if(o&&o->live)dvm_gc_native_touch(vm,ref,o);
   return o && o->live ? o : NULL;
}

/* Metadata-only, synchronous GIL-protected query. No allocation, safepoint,
 * payload access or returned object pointer: unlike dvm__obj(), it must not
 * add every heap object to a native payload-borrow scope (quadratic lookup).
 * Forwarding resolution and live checks remain intact. */
static uint32_t heap_header_count_class(struct dvm *vm,const char *name){
 uint32_t count=0;
 for(uint32_t r=1;r<=vm->heap_size;r++){
  struct dvm_object *o=heap_slot(vm,r);if(!o->live)continue;
  o=dvm_gc_resolve(vm,o);if(!o||!o->live)continue;
  for(struct dvm_class *c=o->cls;c;c=c->super)if(c->name&&!strcmp(c->name,name)){count++;break;}
 }
 return count;
}
bool dvm_heap_header_count_class(struct dvm *vm,const char *name,uint32_t *count){
 extern int dvm_gil_deep_service_scan_enabled(void);
 if(!dvm_gil_deep_service_scan_enabled())return false;
 if(!vm||!name||!count||!dvm_gil_held(vm))abort();
 *count=heap_header_count_class(vm,name);return true;
}

static void gc_fast_allocation_poll(struct dvm *,bool);
static dvm_ref heap_alloc(struct dvm *vm)
{
   gc_fast_allocation_poll(vm,false);
   if (vm->free_head) {
      dvm_ref r = vm->free_head;
      struct dvm_object *o = heap_slot(vm, r);
      vm->free_head = o->next_free;
      memset(o, 0, sizeof *o);
      atomic_init(&o->gc_self, o);
      if (!vm_arena_object_start_set(o,true)) {
         o->next_free=vm->free_head;vm->free_head=r;
         fprintf(stderr,"GC1_ALLOCATION_FAILED reason=object_start_registration ref=%u\n",r);
         return 0;
      }
      o->identity_generation = ++vm->next_object_identity;
      o->live = true;
      dvm_gc_native_touch(vm,r,o);
      return r;
   }
   if (vm->heap_size == vm->heap_cap) {
      struct dvm_object **t =
         realloc(vm->heap_blocks, (size_t)(vm->heap_nblocks + 1) * sizeof *t);
      if (!t) return 0;
      vm->heap_blocks = t;
      struct dvm_object *blk = calloc(DVM_HEAP_BLOCK, sizeof *blk);
      if (!blk) return 0;
      vm->heap_blocks[vm->heap_nblocks++] = blk;
      vm->heap_cap += DVM_HEAP_BLOCK;
   }
   dvm_ref r = ++vm->heap_size;
   atomic_init(&heap_slot(vm, r)->gc_self, heap_slot(vm, r));
   if (!vm_arena_object_start_set(heap_slot(vm,r),true)) {
      --vm->heap_size;
      fprintf(stderr,"GC1_ALLOCATION_FAILED reason=object_start_registration ref=%u\n",r);
      return 0;
   }
   heap_slot(vm, r)->identity_generation = ++vm->next_object_identity;
   heap_slot(vm, r)->live = true;
   dvm_gc_native_touch(vm,r,heap_slot(vm,r));
   return r;
}

void dvm_pin(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   if (o) ++o->pins;
}

void dvm_unpin(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   if (o && o->pins) --o->pins;
}

/* No collector.  The VM runs short bursts of app glue code (an activity
 * callback, a lifecycle hook) between returns to native code, and the heap is
 * freed with the VM.  A collector would have to trace the guest's JNI handle
 * table as well, which the stub layer does not model; getting that wrong
 * would free objects the game still holds.  Objects are reclaimed only where
 * the VM can prove it: string temporaries created and dropped inside one
 * builtin. */
static void object_data_destroy(struct dvm_object *o)
{
   if (o->data_destroy) o->data_destroy(o->data_owner ? o->data_owner : o->data);
   else free(o->data);
   o->data = NULL;
   o->data_destroy = NULL;
   o->data_owner = NULL;
}

static void heap_free(struct dvm *vm, dvm_ref ref)
{
   /* Cleanup is not a new raw-address borrow. Otherwise cleanup itself would
    * retain every temporary until the caller's outermost scope returned. */
   struct dvm_object *o = ref&&ref<=vm->heap_size?dvm_gc_resolve(vm,heap_slot(vm,ref)):NULL;
   if (!o || !o->live || o->pins || o->gc_strong_handles) return;
   if(o->gc_body_borrows || o->gc_payload_borrows){o->gc_flags|=DVM_GC_DEFERRED_FREE;return;}
   dvm_gc_forget_identity(vm,ref,o->identity_generation);
   /* A runtime payload pointer that has drifted into a class name/desc
    * buffer is the signature of the corruption this build is chasing; report
    * it here as the object is reclaimed (cheap: frees are rare) so the guest
    * class/ref and the current caller are captured even if no later
    * class_lookup notices the damage. */
   if (o->data && dvm__name_range_hit(vm, o->data, 8))
      fprintf(stderr, "FREE_BAD_DATA vm=%p ref=%x cls=%s data=%p caller=%s.%s\n",
              (void *)vm, ref, o->cls && o->cls->name ? o->cls->name : "?",
              o->data,
              vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?",
              vm->cur_method ? vm->cur_method->name : "?");
   free(o->utf8);
   free(o->string_units);
   object_data_destroy(o);
   free(o->slots);
   vm_arena_object_start_set(o,false);
   memset(o, 0, sizeof *o);
   o->next_free = vm->free_head;
   vm->free_head = ref;
}

void dvm__discard_temporary(struct dvm *vm,dvm_ref ref)
{
   heap_free(vm,ref);
}

#include "gc_layout.inc"
#include "gc_handles.inc"
#include "gc_foundation.inc"

/* Opt-in bounded creation attribution. Cumulative requested bytes are not
 * retained bytes and do not prove guest reachability. Serialized by the GIL. */
/* Read-only, opt-in idle census. Never called by nativeRender/vsync or by
 * allocations. A positive incoming edge is evidence; absence is NEVER proof
 * of non-escape (native payloads, roots and inter-slice stores are excluded). */
static atomic_bool mem_escape_enabled;
static pthread_mutex_t mem_escape_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
 uint64_t slices,max_ns,cycles,long_small,long_wide,arrays,edges,budget_overruns;
 dvm_ref candidates[64],owner,last_target;uint64_t identities[64];
 unsigned count,next;uint32_t cursor,bound;int phase;char owner_class[128];
} mem_escape;
void dvm_mem_idle_configure(int enabled){if(pthread_mutex_trylock(&mem_escape_lock))return;atomic_store(&mem_escape_enabled,enabled&&!mem_escape.budget_overruns);pthread_mutex_unlock(&mem_escape_lock);}
void dvm_mem_idle_observe(struct dvm *v){
 if(!mem_escape_enabled||!v||!dvm_gil_held(v)||perf_frame_in_progress())return;
 static uint64_t last_ns;uint64_t begin=perf_now();if(begin-last_ns<100000000ull)return;last_ns=begin;
 if(pthread_mutex_trylock(&mem_escape_lock))return;
 if(!mem_escape.cursor){mem_escape.cursor=1;mem_escape.bound=v->heap_size;}
 while(mem_escape.cursor<=mem_escape.bound&&perf_now()-begin<200000ull){
  dvm_ref ref=mem_escape.cursor++;struct dvm_object *o=heap_slot(v,ref);
  if(!o||!o->live||!o->cls)continue;
  if(!mem_escape.phase){
   if(o->cls->name&&!strcmp(o->cls->name,"java/lang/Long")&&o->data){int64_t value;memcpy(&value,o->data,sizeof value);if(value>=-128&&value<=127)mem_escape.long_small++;else mem_escape.long_wide++;}
   if(o->kind==DVM_OBJ_ARRAY&&o->elem_kind=='L'&&o->length==1&&o->data){
    dvm_ref elem=((dvm_ref*)o->data)[0];struct dvm_object *e=elem&&elem<=v->heap_size?heap_slot(v,elem):NULL;
    if(e&&e->live&&e->cls&&e->cls->name&&!strcmp(e->cls->name,"java/lang/Long")){
     unsigned n=mem_escape.next++%64;mem_escape.candidates[n]=ref;mem_escape.identities[n]=o->identity_generation;if(mem_escape.count<64)mem_escape.count++;mem_escape.arrays++;
    }
   }
  }else{
   /* Bounded declared-reference fields / array prefix only; no native graph walk. */
   dvm_ref refs[64];unsigned n=0;
   if(o->kind==DVM_OBJ_ARRAY&&o->elem_kind=='L'&&o->data){n=o->length<64?o->length:64;memcpy(refs,o->data,n*sizeof *refs);}
   else if(o->slots){unsigned depth=0;for(struct dvm_class *c=o->cls;c&&n<64&&depth++<32;c=c->super)
    for(int i=0;i<c->nifields&&n<64;i++)if(c->ifields[i].kind=='L'&&c->ifields[i].slot<o->cls->islots)refs[n++]=o->slots[c->ifields[i].slot].l;}
   for(unsigned i=0;i<n;i++)if(refs[i])for(unsigned j=0;j<mem_escape.count;j++)if(refs[i]==mem_escape.candidates[j]){
    struct dvm_object *target=heap_slot(v,refs[i]);if(!target->live||target->identity_generation!=mem_escape.identities[j])continue;
    mem_escape.edges++;mem_escape.owner=ref;mem_escape.last_target=refs[i];snprintf(mem_escape.owner_class,sizeof mem_escape.owner_class,"%.120s",o->cls->name?o->cls->name:"?");
   }
  }
 }
 if(mem_escape.cursor>mem_escape.bound){mem_escape.cursor=0;if(mem_escape.phase){mem_escape.cycles++;mem_escape.phase=0;mem_escape.count=mem_escape.next=0;}else mem_escape.phase=1;}
 uint64_t elapsed=perf_now()-begin;mem_escape.slices++;if(elapsed>mem_escape.max_ns)mem_escape.max_ns=elapsed;
 if(elapsed>500000ull){mem_escape.budget_overruns++;mem_escape_enabled=false;}
 pthread_mutex_unlock(&mem_escape_lock);
}
void dvm_mem_idle_export(void){
 if(pthread_mutex_trylock(&mem_escape_lock))return;
 __typeof__(mem_escape) copy=mem_escape;pthread_mutex_unlock(&mem_escape_lock);
 if(!copy.slices)return;
 int fd=open("/data/storage/el2/base/haps/entry/files/sdkdex/mem-escape.txt",O_WRONLY|O_CREAT|O_TRUNC,0666);
 if(fd>=0){dprintf(fd,"pid=%d slices=%llu max_ns=%llu cycles=%llu sampled_Long_small=%llu sampled_Long_wide=%llu candidate_arrays=%llu incoming_edges=%llu last_owner=%u last_owner_class=%s last_target=%u budget_overruns=%llu proxy_origin_proven=0 complete_roots=0 native_payloads_scanned=0 pooling_enabled=0 negative_result_proves_no_escape=0\n",getpid(),(unsigned long long)copy.slices,(unsigned long long)copy.max_ns,(unsigned long long)copy.cycles,(unsigned long long)copy.long_small,(unsigned long long)copy.long_wide,(unsigned long long)copy.arrays,(unsigned long long)copy.edges,copy.owner,copy.owner_class,copy.last_target,(unsigned long long)copy.budget_overruns);close(fd);}
}
static bool heap_creation_observe;
static struct {const struct dvm_class *type;const struct dvm_method *method;size_t count,bytes;char label[512];} heap_creation[256];
static unsigned heap_creation_used;static size_t heap_creation_overflow;static uint64_t heap_creation_last;
static void heap_creation_note(struct dvm *vm,struct dvm_class *cls,size_t bytes){
 if(!heap_creation_observe)return;
 if(!cls||!cls->name||(strcmp(cls->name,"java/lang/Long")&&strcmp(cls->name,"[Ljava/lang/Object;")&&strcmp(cls->name,"[Ljava/lang/Class;")&&strcmp(cls->name,"com/gsc/cobbler/patch/PatchProxyResult")))return;
 const struct dvm_method *m=vm->invoke_frame?vm->invoke_frame->method:vm->cur_method;
 unsigned i=0;for(;i<heap_creation_used;i++)if(heap_creation[i].type==cls&&heap_creation[i].method==m)break;
 if(i==heap_creation_used){if(i==256){heap_creation_overflow++;return;}heap_creation[i].type=cls;heap_creation[i].method=m;snprintf(heap_creation[i].label,sizeof heap_creation[i].label,"%s.%s%s",m&&m->cls?m->cls->name:"?",m&&m->name?m->name:"?",m&&m->sig?m->sig:"");heap_creation_used++;}
 heap_creation[i].count++;heap_creation[i].bytes+=bytes;
 uint64_t now=dvm__now_ms();if(now-heap_creation_last<10000)return;heap_creation_last=now;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/heap-creation.txt","a");if(!f)return;
 dprintf(fileno(f),"CREATION pid=%d mono_ms=%llu sites=%u overflow_events=%zu accounting=cumulative_partial_requested_not_live\n",getpid(),(unsigned long long)now,heap_creation_used,heap_creation_overflow);
 for(unsigned j=0;j<heap_creation_used;j++)dprintf(fileno(f),"site=%u count=%zu bytes=%zu type=%s method=%s\n",j,heap_creation[j].count,heap_creation[j].bytes,heap_creation[j].type?heap_creation[j].type->name:"?",heap_creation[j].label);
 fclose(f);
}
dvm_ref dvm_new_object(struct dvm *vm, struct dvm_class *cls)
{
   if (!cls) return 0;
   dvm_ref r = heap_alloc(vm);
   if (!r) return 0;
   struct dvm_object *o = heap_slot(vm, r);
   o->cls = cls;
   o->kind = DVM_OBJ_PLAIN;
   heap_creation_note(vm,cls,sizeof *o+(size_t)cls->islots*sizeof(union dvm_value));
   if (cls->islots > 0) {
      o->slots = calloc((size_t)cls->islots, sizeof *o->slots);
      if (!o->slots) { heap_free(vm, r); return 0; }
   }
   return r;
}

dvm_ref dvm_new_string_n(struct dvm *vm, const char *utf8, size_t len)
{
   struct dvm_class *cls = dvm__class_by_desc(vm, "Ljava/lang/String;");
   dvm_ref r = heap_alloc(vm);
   if (!r) return 0;
   struct dvm_object *o = heap_slot(vm, r);
   o->cls = cls;
   o->kind = DVM_OBJ_STRING;
   heap_creation_note(vm,cls,sizeof *o+len+1);
   o->utf8 = malloc(len + 1);
   if (!o->utf8) { heap_free(vm, r); return 0; }
   if (len) memcpy(o->utf8, utf8, len);
   o->utf8[len] = '\0';
   o->utf8_len = (uint32_t)len;
   {
      const char *want = dvm__env_strwatch();
      if (want && strstr(o->utf8, want))
         fprintf(stderr, "[dvm] new-string \"%.200s\" in %s.%s\n", o->utf8,
                 vm->cur_method && vm->cur_method->cls
                    ? vm->cur_method->cls->name : "?",
                 vm->cur_method ? vm->cur_method->name : "?");
   }
   return r;
}

dvm_ref dvm_new_string(struct dvm *vm, const char *utf8)
{
   return dvm_new_string_n(vm, utf8 ? utf8 : "", utf8 ? strlen(utf8) : 0);
}

dvm_ref dvm_new_array(struct dvm *vm, char elem, const char *elem_desc, uint32_t length)
{
   char desc[256];
   snprintf(desc, sizeof desc, "[%s", elem_desc ? elem_desc : "I");
   struct dvm_class *cls = dvm__class_by_desc(vm, desc);
   size_t bytes = (size_t)length * (size_t)dvm__elem_width(elem);
   if(bytes>=VM_EXTERNAL_THRESHOLD)gc_fast_allocation_poll(vm,true);
   dvm_ref r = heap_alloc(vm);
   if (!r) return 0;
   struct dvm_object *o = heap_slot(vm, r);
   o->cls = cls;
   o->kind = DVM_OBJ_ARRAY;
   heap_creation_note(vm,cls,sizeof *o+bytes);
   o->length = length;
   o->elem_kind = elem;
   if(bytes>=VM_EXTERNAL_THRESHOLD){
      dvm_large_allocation_note(bytes,(uintptr_t)__builtin_return_address(0));
      o->data=vm_external_calloc(bytes);
      FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/large-alloc.txt","a");
      if(f){struct vm_external_snapshot s;vm_external_snapshot(&s);dprintf(fileno(f),"ARRAY_RESULT pid=%d bytes=%zu length=%u elem=%c success=%d external_payload_bytes=%zu external_payload_peak=%zu external_limit=%zu\n",getpid(),bytes,length,elem,o->data!=NULL,s.bytes,s.peak,s.limit);fclose(f);}
   }else o->data = calloc(bytes ? bytes : 1, 1);
   if (!o->data) { heap_free(vm, r); return 0; }
   return r;
}

struct dvm_class *dvm_object_class(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return o ? o->cls : NULL;
}

const char *dvm_string_utf8(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return (o && o->kind == DVM_OBJ_STRING) ? o->utf8 : NULL;
}

uint32_t dvm_string_utf8_length(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return (o && o->kind == DVM_OBJ_STRING) ? o->utf8_len : 0;
}

uint32_t dvm_array_length(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return (o && o->kind == DVM_OBJ_ARRAY) ? o->length : 0;
}

void *dvm_array_data(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return (o && o->kind == DVM_OBJ_ARRAY) ? o->data : NULL;
}

dvm_ref dvm_wrap_external(struct dvm *vm, const char *class_name, uint32_t host_handle)
{
   char desc[512];
   name_to_desc(class_name, desc, sizeof desc);
   struct dvm_class *cls = dvm__class_by_desc(vm, desc);
   dvm_ref r = heap_alloc(vm);
   if (!r) return 0;
   struct dvm_object *o = heap_slot(vm, r);
   o->cls = cls;
   o->kind = DVM_OBJ_EXTERNAL;
   o->host_handle = host_handle;
   if (cls && cls->islots > 0)
      o->slots = calloc((size_t)cls->islots, sizeof *o->slots);
   return r;
}

uint32_t dvm_external_handle(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return o ? o->host_handle : 0;
}

/* --- interning ---------------------------------------------------------- */

struct dvm_intern {
   /* Points at the pinned String object's immutable utf8 storage.  Keeping a
    * second strdup here served no ownership purpose and, worse, its unchecked
    * allocation failure published a NULL key: the next intern operation then
    * crashed the host in strcmp(). */
   const char *key;
   dvm_ref ref;
   uint32_t hash;
};

/* Execution roots are process-visible while the GIL is handed to another
 * thread. Stack-backed records are linked only for their exact C lifetime.
 * Suspended TLS records contain copied refs, never pointers into dead TLS. */
struct frame;
struct gc_exec_entry {
   struct dvm *vm;dvm_ref self;const char *sig;const uint32_t *slots;int nslots;
   struct gc_exec_entry *next;
};
struct gc_exec_frame {struct dvm *vm;struct frame *frame;struct gc_exec_frame *next;};
struct gc_tstate_hold {
   struct dvm *vm;uint64_t serial;dvm_ref refs[3];bool suspended;
   struct gc_tstate_hold *next;
};
static struct gc_exec_entry *gc_exec_entries;
static struct gc_exec_frame *gc_exec_frames;
static struct gc_tstate_hold *gc_tstate_holds;
static pthread_mutex_t gc_tstate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t gc_tstate_key;
static pthread_once_t gc_tstate_once=PTHREAD_ONCE_INIT;
static int gc_tstate_key_error;
struct gc_exec_counts {size_t entries,typed_slots,frames,registers,results,tstates,refs;};
static bool gc_execution_roots(struct dvm *,dvm_gc_visit_fn,void *,struct gc_exec_counts *);
static void gc_tstate_unlink_locked(struct gc_tstate_hold *r){
   struct gc_tstate_hold **p=&gc_tstate_holds;while(*p&&*p!=r)p=&(*p)->next;
   if(*p)*p=r->next;r->next=NULL;r->vm=NULL;
}
static void gc_tstate_destructor(void *data){
   struct gc_tstate_hold *r=data;if(!r)return;
   pthread_mutex_lock(&gc_tstate_lock);gc_tstate_unlink_locked(r);pthread_mutex_unlock(&gc_tstate_lock);free(r);
}
static void gc_tstate_key_init(void){gc_tstate_key_error=pthread_key_create(&gc_tstate_key,gc_tstate_destructor);}
static void gc_tstate_copy(struct gc_tstate_hold *r,struct dvm *vm,const struct dvm_tstate *t){
   r->vm=vm;r->serial=vm->serial;r->refs[0]=t->exception;r->refs[1]=t->exc_ref;r->refs[2]=t->cur_thread;
}
static void gc_tstate_local_enter(struct gc_tstate_hold *r,struct dvm *vm,const struct dvm_tstate *t){
   memset(r,0,sizeof *r);gc_tstate_copy(r,vm,t);
   pthread_mutex_lock(&gc_tstate_lock);r->next=gc_tstate_holds;gc_tstate_holds=r;pthread_mutex_unlock(&gc_tstate_lock);
}
static void gc_tstate_local_leave(struct gc_tstate_hold *r){
   pthread_mutex_lock(&gc_tstate_lock);gc_tstate_unlink_locked(r);pthread_mutex_unlock(&gc_tstate_lock);
}
static bool gc_tstate_resume(void){
   pthread_once(&gc_tstate_once,gc_tstate_key_init);if(gc_tstate_key_error)return false;
   struct gc_tstate_hold *r=pthread_getspecific(gc_tstate_key);if(!r)return true;
   int error=pthread_setspecific(gc_tstate_key,NULL);
   if(error){pthread_mutex_lock(&gc_tstate_lock);if(r->vm)r->vm->gc_execution_incomplete=true;pthread_mutex_unlock(&gc_tstate_lock);fprintf(stderr,"GC_ROOT_FAILURE provider=suspended_tstate reason=pthread_setspecific_clear error=%d vm_serial=%llu\n",error,(unsigned long long)r->serial);return false;}
   gc_tstate_destructor(r);return true;
}
static void gc_tstate_suspend(struct dvm *vm,const struct dvm_tstate *t){
   if(!gc_tstate_resume()){vm->gc_execution_incomplete=true;fprintf(stderr,"GC_ROOT_FAILURE provider=suspended_tstate reason=key_resume error=%d vm_serial=%llu\n",gc_tstate_key_error,(unsigned long long)vm->serial);return;}
   struct gc_tstate_hold *r=calloc(1,sizeof *r);if(!r){vm->gc_execution_incomplete=true;fprintf(stderr,"GC_ROOT_FAILURE provider=suspended_tstate reason=native_allocation bytes=%zu vm_serial=%llu\n",sizeof *r,(unsigned long long)vm->serial);return;}
   gc_tstate_copy(r,vm,t);r->suspended=true;
   int key_error=pthread_setspecific(gc_tstate_key,r);
   if(key_error){free(r);vm->gc_execution_incomplete=true;fprintf(stderr,"GC_ROOT_FAILURE provider=suspended_tstate reason=pthread_setspecific error=%d vm_serial=%llu\n",key_error,(unsigned long long)vm->serial);return;}
   pthread_mutex_lock(&gc_tstate_lock);r->next=gc_tstate_holds;gc_tstate_holds=r;pthread_mutex_unlock(&gc_tstate_lock);
}
static void gc_execution_vm_end(struct dvm *vm){
   pthread_mutex_lock(&gc_tstate_lock);
   struct gc_tstate_hold **p=&gc_tstate_holds;
   while(*p){struct gc_tstate_hold *r=*p;if(r->vm==vm){*p=r->next;r->next=NULL;r->vm=NULL;memset(r->refs,0,sizeof r->refs);}else p=&r->next;}
   pthread_mutex_unlock(&gc_tstate_lock);
}

static bool gc_core_roots(struct dvm *vm,dvm_gc_visit_fn visit,void *user)
{
   dvm_ref *direct[]={&vm->runtime_instance,&vm->shutdown_hooks_lock,&vm->pending_intents,
      &vm->content_providers,&vm->content_observers,&vm->exception,&vm->exc_ref,
      &vm->resource_bundle_cache,&vm->cur_thread,&vm->default_trust_manager,
      &vm->keystore_entries,&vm->accepted_issuers,&vm->main_thread,
      &vm->main_thread_group,&vm->system_thread_group};
   for(size_t i=0;i<sizeof direct/sizeof direct[0];++i)if(*direct[i])visit(direct[i],user);
   for(size_t i=0;i<vm->shutdown_hook_count;++i)if(vm->shutdown_hooks[i])visit(&vm->shutdown_hooks[i],user);
   for(size_t i=0;i<vm->nnetwork_callbacks;++i){
      if(vm->network_callbacks[i].callback)visit(&vm->network_callbacks[i].callback,user);
      if(vm->network_callbacks[i].network)visit(&vm->network_callbacks[i].network,user);
   }
   for(size_t i=0;i<vm->nlayout_observers;++i)if(vm->layout_observers[i])visit(&vm->layout_observers[i],user);
   for(size_t i=0;i<vm->nactive_object_animators;++i)if(vm->active_object_animators[i])visit(&vm->active_object_animators[i],user);
   for(unsigned i=0;i<vm->velocity_pool_count&&i<2;++i)if(vm->velocity_pool[i])visit(&vm->velocity_pool[i],user);
   for(unsigned i=0;i<256;++i)if(vm->native_threads[i].thread)visit(&vm->native_threads[i].thread,user);
   for(int i=0;i<vm->npending;++i){
      if(vm->pending_threads[i])visit(&vm->pending_threads[i],user);
      if(vm->pending_looper&&vm->pending_looper[i])visit(&vm->pending_looper[i],user);
      if(vm->pending_owner&&vm->pending_owner[i])visit(&vm->pending_owner[i],user);
      if(vm->pending_token&&vm->pending_token[i])visit(&vm->pending_token[i],user);
   }
   for(int i=0;i<vm->interns_cap;++i)if(vm->interns[i].ref)visit(&vm->interns[i].ref,user);
   for(int i=0;i<vm->ndexes;++i){struct dvm_dex *d=vm->dexes[i];
      if(d->string_cache)for(uint32_t j=0;j<d->file.string_ids_size;++j)if(d->string_cache[j])visit(&d->string_cache[j],user);
   }
   bool ok=gc_execution_roots(vm,visit,user,NULL);
   for(int i=0;i<vm->nclasses;++i){struct dvm_class *c=vm->classes[i];
      if(c->class_object)visit(&c->class_object,user);
      if(c->defining_loader)visit(&c->defining_loader,user);
      if(c->type_parameters)visit(&c->type_parameters,user);
      if(!dvm_gc_visit_static_refs(vm,c,visit,user)){
         gc_root_failure("class_static",c->name);
         fprintf(stderr,"GC_ROOT_FAILURE provider=class_static class=%s\n",c->name?c->name:"?");ok=false;
      }
   }
   return ok;
}

/* ------------------------------------------------------------------------ *
 * Class registry
 * ------------------------------------------------------------------------ */

static uint32_t str_hash(const char *s)
{
   uint32_t h = 2166136261u;
   for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
   return h;
}

dvm_ref dvm__intern(struct dvm *vm, const char *utf8)
{
   const uint32_t hash = str_hash(utf8);
   if (vm->interns_cap) {
      unsigned slot = hash & (unsigned)(vm->interns_cap - 1);
      while (vm->interns[slot].ref) {
         const struct dvm_intern *entry = vm->interns + slot;
         if (entry->hash == hash && !strcmp(entry->key, utf8)) return entry->ref;
         slot = (slot + 1u) & (unsigned)(vm->interns_cap - 1);
      }
   }

   dvm_ref r = dvm_new_string(vm, utf8);
   if (!r) return 0;
   dvm_pin(vm, r);

   /* Literals remain pinned for the VM's lifetime, so the pool has no deletes.
    * A half-full open-addressed table avoids a scan of every literal on each
    * const-string and String.intern call, without another allocation per key. */
   if (vm->ninterns >= vm->interns_cap / 2) {
      if (vm->interns_cap > INT32_MAX / 2) return r;
      int cap = vm->interns_cap ? vm->interns_cap * 2 : 64;
      struct dvm_intern *n = calloc((size_t)cap, sizeof *n);
      if (!n) return r;
      for (int i = 0; i < vm->interns_cap; ++i) {
         if (!vm->interns[i].ref) continue;
         unsigned slot = vm->interns[i].hash & (unsigned)(cap - 1);
         while (n[slot].ref) slot = (slot + 1u) & (unsigned)(cap - 1);
         n[slot] = vm->interns[i];
      }
      free(vm->interns);
      vm->interns = n;
      vm->interns_cap = cap;
   }
   /* r was pinned above, so its backing object and utf8 allocation live until
    * dvm_destroy().  Use that storage as the table key: Java intern strings
    * are immutable, and one allocation per value is both sufficient and the
    * only failure-atomic representation. */
   const char *key = dvm_string_utf8(vm, r);
   if (!key) return r; /* defensive: never publish a NULL comparison key */
   unsigned slot = hash & (unsigned)(vm->interns_cap - 1);
   while (vm->interns[slot].ref)
      slot = (slot + 1u) & (unsigned)(vm->interns_cap - 1);
   vm->interns[slot] = (struct dvm_intern){ .key = key, .ref = r, .hash = hash };
   ++vm->ninterns;
   return r;
}

/* The class registry (hash chains, vm->classes growth) is mutated while guest
 * worker threads resolve classes concurrently with the main thread.  It has
 * no other lock, and dvm__class_by_desc() recurses through superclasses and
 * array component types, so the registry uses one recursive mutex. */
static pthread_mutex_t class_registry_lock;
static pthread_once_t class_registry_once = PTHREAD_ONCE_INIT;
static void class_registry_lock_init(void)
{
   pthread_mutexattr_t attr;
   if (pthread_mutexattr_init(&attr) == 0) {
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init(&class_registry_lock, &attr);
      pthread_mutexattr_destroy(&attr);
   } else {
      pthread_mutex_init(&class_registry_lock, NULL);
   }
}
static void class_registry_enter(void)
{
   pthread_once(&class_registry_once, class_registry_lock_init);
   pthread_mutex_lock(&class_registry_lock);
}
static void class_registry_leave(void)
{
   pthread_mutex_unlock(&class_registry_lock);
}

/* --- class-name write guard (diagnostic) --------------------------------
 *
 * 72ca captured the first-class corruption scene: java/lang/ClassCastException's
 * name buffer held cd 0c 00 00 01 00 00 00 ('g/ClassCastException' survives
 * from offset 8), owned=1 (same VM), caller DiskLogAdapter initialValue.  An
 * 8-byte field/union write is the shape that fits.  Every field write path
 * already checks slot < islots, so the guard records each class name
 * allocation and checks the destination address of every instance/static
 * field write against those ranges; a hit prints the exact field, value,
 * caller and destination instead of silently corrupting the name.
 *
 * Ranges are kept sorted by address and probed with a binary search, so the
 * cost on a field write is a handful of comparisons.  The table and the
 * guard are diagnostics; they are never used to alter behavior. */
struct name_range { char *base; size_t len; struct dvm *vm; uint64_t serial; bool is_desc; };
static struct name_range g_name_ranges[20000];
static size_t g_name_range_count;
static pthread_mutex_t g_name_range_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_name_guard_hits;

static int name_range_cmp(const void *x, const void *y)
{
   const struct name_range *a = x, *b = y;
   return (a->base < b->base) ? -1 : (a->base > b->base) ? 1 : 0;
}

static void name_range_add(struct dvm *vm, char *base, size_t len, bool is_desc)
{
   if (!base || !len) return;
   pthread_mutex_lock(&g_name_range_lock);
   if (g_name_range_count < sizeof g_name_ranges / sizeof g_name_ranges[0]) {
      g_name_ranges[g_name_range_count].base = base;
      g_name_ranges[g_name_range_count].len = len;
      g_name_ranges[g_name_range_count].vm = vm;
      g_name_ranges[g_name_range_count].serial = vm ? vm->serial : 0;
      g_name_ranges[g_name_range_count].is_desc = is_desc;
      ++g_name_range_count;
      qsort(g_name_ranges, g_name_range_count, sizeof g_name_ranges[0],
            name_range_cmp);
   }
   pthread_mutex_unlock(&g_name_range_lock);
}

/* Only ranges belonging to this VM can be hit: malloc reuses a destroyed VM's
 * name buffers for the next VM's slots arrays (72cb's first report was
 * exactly that false positive), and a hit in another VM's range is not the
 * corruption this guard is looking for. */
static bool name_range_hit(struct dvm *vm, const void *p, size_t n,
                           char **hit_base, size_t *hit_len, bool *hit_desc)
{
   const char *a = p;
   bool hit = false;
   if (hit_base) *hit_base = NULL;
   if (hit_len) *hit_len = 0;
   if (hit_desc) *hit_desc = false;
   pthread_mutex_lock(&g_name_range_lock);
   size_t lo = 0, hi = g_name_range_count;
   while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (g_name_ranges[mid].base <= a) lo = mid + 1; else hi = mid;
   }
   if (lo) {
      const struct name_range *r = &g_name_ranges[lo - 1];
      hit = vm && r->serial == vm->serial && (uintptr_t)a >= (uintptr_t)r->base &&
            (uintptr_t)a - (uintptr_t)r->base <= r->len &&
            n <= r->len - ((uintptr_t)a - (uintptr_t)r->base);
      if (hit) {
         if (hit_base) *hit_base = r->base;
         if (hit_len) *hit_len = r->len;
         if (hit_desc) *hit_desc = r->is_desc;
      }
   }
   pthread_mutex_unlock(&g_name_range_lock);
   return hit;
}

/* Exported for the collection runtimes: a map's payload pointer (o->data)
 * must never point into a class name/desc buffer.  When it does, the guest
 * ref named a different object than the caller assumed (heap slot reuse), and
 * the next entry write would land in the class record. */
bool dvm__name_range_hit(struct dvm *vm, const void *p, size_t n)
{
   return name_range_hit(vm, p, n, NULL, NULL, NULL);
}

static void name_guard_report(const char *site, struct dvm *vm,
                              struct dvm_class *owner, const char *field,
                              int slot, union dvm_value val, const void *dst,
                              const char *range_base, size_t range_len,
                              bool range_is_desc)
{
   int hits = atomic_fetch_add(&g_name_guard_hits, 1) + 1;
   if (hits > 40) return;   /* bounded: the first hits identify the writer */
   fprintf(stderr, "NAME_OVERWRITE site=%s vm=%p owner=%s field=%s slot=%d dst=%p value=%llx range=%p+%zu kind=%s caller=%s.%s\n",
           site, (void *)vm, owner && owner->name ? owner->name : "?",
           field ? field : "?", slot, dst, (unsigned long long)val.ju,
           (void *)range_base, range_len, range_is_desc ? "desc" : "name",
           vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?",
           vm->cur_method ? vm->cur_method->name : "?");
}

/* --- class name/desc write-protect trap (diagnostic) --------------------
 *
 * The 8-byte pattern in ClassCastException's/RuntimeException's name buffer
 * and the 2-byte pattern in java/lang/Object's desc did not come through any
 * field-write site the range guard covers, so waiting for the ~1-in-3
 * corrupted run is not enough.  Each class name/desc buffer is therefore
 * allocated on its own page and sealed read-only once initialized; the next
 * write faults, the handler records the faulting address, the owning class
 * and kind, then grants write access so execution continues unchanged.  This
 * is a detector only: it never repairs or hides anything, and it is off when
 * LUNARIA_CLASS_BUF_PROTECT=0. */
/* Initialized before publishing protected buffers; signal handler never calls sysconf. */
static size_t class_buf_page;
struct class_buf_map { void *page; const char *label; uint64_t serial; bool is_desc; };
static struct class_buf_map g_class_bufs[16384];
static size_t g_class_buf_count;
static pthread_mutex_t g_class_buf_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_class_buf_faults;
static _Atomic bool g_class_buf_protect;

static void class_buf_handler(int sig, siginfo_t *info, void *ctx)
{
   (void)sig; (void)ctx;
   uintptr_t addr = (uintptr_t)(info ? info->si_addr : NULL);
   const char *label = "?"; bool is_desc = false; uint64_t serial = 0;
   bool found = false;
   /* Lock-free scan: the registry only grows during class registration, and
    * taking a mutex inside a signal handler can deadlock against the thread
    * the signal interrupted.  A torn read here is a diagnostic miss, not a
    * correctness problem. */
   size_t count = __atomic_load_n(&g_class_buf_count, __ATOMIC_ACQUIRE);
   for (size_t i = 0; i < count; ++i) {
      uintptr_t base = (uintptr_t)g_class_bufs[i].page;
      if (addr >= base && addr < base + class_buf_page) {
         label = g_class_bufs[i].label; is_desc = g_class_bufs[i].is_desc;
         serial = g_class_bufs[i].serial; found = true;
         break;
      }
   }
   /* Faulting thread and instruction pointer: the PC identifies the exact
    * store instruction (and therefore the runtime/guest frame) that wrote the
    * class buffer; the value itself is still in a register and is recovered
    * from the disassembly of that instruction if needed. */
   unsigned long tid = 0;
   uintptr_t pc = 0;
   {
      ucontext_t *uc = (ucontext_t *)ctx;
      if (uc) pc = (uintptr_t)uc->uc_mcontext.pc;
#ifdef SYS_gettid
      tid = (unsigned long)syscall(SYS_gettid);
#endif
   }
   char out[384]; int n = 0;
   if (found)
      n = snprintf(out, sizeof out,
                   "CLASS_BUF_WRITE addr=0x%llx label=%s kind=%s serial=%llu tid=%lu pc=0x%llx\n",
                   (unsigned long long)addr, label ? label : "?",
                   is_desc ? "desc" : "name", (unsigned long long)serial,
                   tid, (unsigned long long)pc);
   else
      n = snprintf(out, sizeof out,
                   "CLASS_BUF_WRITE addr=0x%llx label=? kind=? tid=%lu pc=0x%llx (unregistered page)\n",
                   (unsigned long long)addr, tid, (unsigned long long)pc);
   if (n > 0) { ssize_t wrote = write(2, out, (size_t)n); (void)wrote; }
   atomic_fetch_add(&g_class_buf_faults, 1);
   /* Let the write through so the run continues exactly as before this
    * detector existed; the page stays writable and later writes to it are
    * not re-reported. */
   mprotect((void *)(addr & ~(uintptr_t)(class_buf_page - 1u)),
            class_buf_page, PROT_READ | PROT_WRITE);
}

static void class_buf_enable(void)
{
   if (atomic_load(&g_class_buf_protect)) return;
   /* Opt-in only: the dedicated-page layout changed the malloc layout enough
    * that the corruption stopped reproducing after 72cf (72ca–72ce hit about
    * one run in three; 16+ later runs hit none).  Keep the trap available for
    * explicit debugging via LUNARIA_CLASS_BUF_PROTECT=1, but default to the
    * original malloc layout so the checkpoints below can observe a live
    * corruption. */
   const char *env = getenv("LUNARIA_CLASS_BUF_PROTECT");
   if (!env || env[0] != '1') return;
   class_buf_page=host_page_size();
   if(!class_buf_page)return;
   struct sigaction sa;
   memset(&sa, 0, sizeof sa);
   sa.sa_sigaction = class_buf_handler;
   sa.sa_flags = SA_SIGINFO | SA_NODEFER;
   sigemptyset(&sa.sa_mask);
   sigaction(SIGSEGV, &sa, NULL);
   atomic_store(&g_class_buf_protect, true);
}

static char *class_buf_alloc(size_t len, const char *label, bool is_desc,
                             uint64_t serial)
{
   if (!atomic_load(&g_class_buf_protect) || len > class_buf_page) {
      char *p = malloc(len);
      return p;
   }
   void *m = mmap(NULL, class_buf_page, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (m == MAP_FAILED) return NULL;
   pthread_mutex_lock(&g_class_buf_lock);
   if (g_class_buf_count < sizeof g_class_bufs / sizeof g_class_bufs[0]) {
      g_class_bufs[g_class_buf_count].page = m;
      g_class_bufs[g_class_buf_count].label = label;
      g_class_bufs[g_class_buf_count].serial = serial;
      g_class_bufs[g_class_buf_count].is_desc = is_desc;
      __atomic_store_n(&g_class_buf_count, g_class_buf_count + 1, __ATOMIC_RELEASE);
   }
   pthread_mutex_unlock(&g_class_buf_lock);
   return m;
}

static void class_buf_seal(char *buf)
{
   if (!atomic_load(&g_class_buf_protect) || !buf) return;
   if (((uintptr_t)buf % class_buf_page) != 0) return;  /* malloc fallback */
   mprotect(buf, class_buf_page, PROT_READ);
}

static void class_buf_free(char *buf)
{
   if (!buf) return;
   if (!atomic_load(&g_class_buf_protect) || ((uintptr_t)buf % class_buf_page) != 0) {
      free(buf);
      return;
   }
   pthread_mutex_lock(&g_class_buf_lock);
   for (size_t i = 0; i < g_class_buf_count; ++i) {
      if (g_class_bufs[i].page == buf) {
         g_class_bufs[i] = g_class_bufs[--g_class_buf_count];
         break;
      }
   }
   pthread_mutex_unlock(&g_class_buf_lock);
   munmap(buf, class_buf_page);
}

/* class->name is derived state: for Lname; descriptors it is desc with the
 * L and ; removed, and for every other descriptor it is desc verbatim.  The
 * desc buffer is authoritative and is what class_lookup matches on.  One
 * corrupted class record (72bv/72bw: java/lang/Throwable's name buffer held
 * the two bytes cd 0c while its desc was intact) made derives() by name fail
 * for every exception method, and produced "�\x0c.getMessage()" instead of
 * the real class.  Rebuilding the name from desc is only a stopgap so the
 * rest of the run stays usable: it is NOT the root-cause fix.  The first
 * corruption is copied out in full (bytes, pointers, ownership) before the
 * repair and is never overwritten, so the original scene survives for the
 * ownership / VM-lifetime / out-of-bounds investigation. */
struct class_corruption_first {
   bool captured;
   struct dvm *vm;
   struct dvm_class *cls;
   const char *desc;
   char *name_ptr;
   size_t expected_len;
   size_t bytes_len;
   unsigned char bytes[64];
   char caller_class[96];
   char caller_method[96];
};
static struct class_corruption_first g_first_corruption;

static bool class_owned_by_vm(struct dvm *vm, struct dvm_class *c)
{
   for (int i = 0; i < vm->nclasses; ++i)
      if (vm->classes[i] == c) return true;
   return false;
}

static void class_name_self_heal(struct dvm *vm, struct dvm_class *c)
{
   if (!c || !c->desc) return;
   size_t dn = strlen(c->desc);
   const char *expect = c->desc;
   size_t el = dn;
   if (dn >= 2 && c->desc[0] == 'L' && c->desc[dn - 1] == ';') {
      expect = c->desc + 1;
      el = dn - 2;
   }
   if (el >= 512) return;
   if (c->name && !strncmp(c->name, expect, el) && !c->name[el]) return;
   /* Ownership: a class reached through this VM's hash table must also be a
    * member of vm->classes[].  owned=0 means the record belongs to another
    * VM (a destroyed one whose memory was reused, or a live sibling), which
    * is a lifetime bug rather than an overwrite inside this VM. */
   bool owned = class_owned_by_vm(vm, c);
   size_t dump_len = el < sizeof g_first_corruption.bytes ? el : sizeof g_first_corruption.bytes;
   char old_hex[3 * 128 + 1];
   size_t at = 0;
   for (size_t i = 0; i < dump_len; ++i)
      at += (size_t)snprintf(old_hex + at, sizeof old_hex - at, "%02x",
                             c->name ? (unsigned char)c->name[i] : 0);
   old_hex[at] = '\0';
   const char *cc = vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?";
   const char *cm = vm->cur_method ? vm->cur_method->name : "?";
   if (!g_first_corruption.captured) {
      g_first_corruption.captured = true;
      g_first_corruption.vm = vm;
      g_first_corruption.cls = c;
      g_first_corruption.desc = c->desc;
      g_first_corruption.name_ptr = c->name;
      g_first_corruption.expected_len = el;
      g_first_corruption.bytes_len = dump_len;
      for (size_t i = 0; i < dump_len; ++i)
         g_first_corruption.bytes[i] = c->name ? (unsigned char)c->name[i] : 0;
      snprintf(g_first_corruption.caller_class, sizeof g_first_corruption.caller_class, "%s", cc);
      snprintf(g_first_corruption.caller_method, sizeof g_first_corruption.caller_method, "%s", cm);
      fprintf(stderr, "CLASS_CORRUPTION_FIRST vm=%p owned=%d cls=%p desc=%s name_ptr=%p bytes=%s caller=%s.%s\n",
              (void *)vm, owned ? 1 : 0, (void *)c, c->desc, (void *)c->name, old_hex, cc, cm);
   } else {
      fprintf(stderr, "CLASS_NAME_REPAIRED vm=%p owned=%d cls=%p desc=%s name_ptr=%p bytes=%s caller=%s.%s first_cls=%p\n",
              (void *)vm, owned ? 1 : 0, (void *)c, c->desc, (void *)c->name, old_hex, cc, cm,
              (void *)g_first_corruption.cls);
   }
   char *fixed = class_buf_alloc(el + 1, c->desc, false, vm->serial);
   if (!fixed) return;
   memcpy(fixed, expect, el);
   fixed[el] = '\0';
   class_buf_seal(fixed);
   class_buf_free(c->name);
   c->name = fixed;
   name_range_add(vm, fixed, el + 1, false);
}

static struct dvm_class *class_lookup(struct dvm *vm, const char *desc)
{
   class_registry_enter();
   uint32_t h = str_hash(desc) % DVM_CLASS_HASH;
   struct dvm_class *found = NULL;
   for (struct dvm_class *c = vm->class_hash[h]; c; c = c->hash_next)
      if (!strcmp(c->desc, desc)) { found = c; break; }
   if (found) class_name_self_heal(vm, found);
   class_registry_leave();
   return found;
}

static struct dvm_class *class_register(struct dvm *vm, const char *desc)
{
   class_registry_enter();
   /* A later registration deliberately wins over an earlier placeholder:
    * dvm__register_builtin() is installed after an external class of the same
    * descriptor may already exist, and the newest entry at the head of the
    * hash chain is the one dvm__class_by_desc() must return.  Keep that
    * replacement semantics; the lock only serialises the mutation itself. */
   struct dvm_class *c = calloc(1, sizeof *c);
   if (!c) { class_registry_leave(); return NULL; }
   size_t n = strlen(desc);
   /* desc and name are the buffers the corruption writes into; allocate them
    * on dedicated pages so the diagnostic write trap can name the writer.
    * class_buf_alloc falls back to malloc when protection is off. */
   c->desc = class_buf_alloc(n + 1, desc, true, vm->serial);
   if (!c->desc) { free(c); class_registry_leave(); return NULL; }
   memcpy(c->desc, desc, n + 1);
   class_buf_seal(c->desc);

   size_t name_bytes = 0;
   if (desc[0] == 'L' && n >= 2 && desc[n - 1] == ';') {
      c->name = class_buf_alloc(n - 1, c->desc, false, vm->serial);
      if (c->name) {
         memcpy(c->name, desc + 1, n - 2);
         c->name[n - 2] = '\0';
         class_buf_seal(c->name);
      }
      name_bytes = n - 1;
   } else {
      c->name = class_buf_alloc(n + 1, c->desc, false, vm->serial);
      if (c->name) { memcpy(c->name, desc, n + 1); class_buf_seal(c->name); }
      name_bytes = n + 1;
   }
   if (!c->name) { class_buf_free(c->desc); free(c); class_registry_leave(); return NULL; }
   name_range_add(vm, c->name, name_bytes, false);
   name_range_add(vm, c->desc, n + 1, true);

   uint32_t h = str_hash(desc) % DVM_CLASS_HASH;
   c->hash_next = vm->class_hash[h];
   vm->class_hash[h] = c;

   if (vm->nclasses == vm->classes_cap) {
      int cap = vm->classes_cap ? vm->classes_cap * 2 : 256;
      struct dvm_class **nn = realloc(vm->classes, (size_t)cap * sizeof *nn);
      if (nn) { vm->classes = nn; vm->classes_cap = cap; }
   }
   if (vm->nclasses < vm->classes_cap)
      vm->classes[vm->nclasses++] = c;
   class_registry_leave();
   return c;
}

/* dvm_runtime.c builds its classes through this so the registry (hash chain,
 * teardown list) stays owned by one file. */
struct dvm_class *dvm__register_builtin(struct dvm *vm, const char *desc)
{
   return class_register(vm, desc);
}

struct dvm_class *dvm__define_primitive(struct dvm *vm, const char *desc)
{
   struct dvm_class *c = class_lookup(vm, desc);
   if (c) return c;
   c = class_register(vm, desc);
   if (c) { c->is_primitive = true; c->init_state = 2; }
   return c;
}

static bool class_load_from_dex(struct dvm *vm, struct dvm_class *c,
                                struct dvm_dex *dd, uint32_t class_def_idx);

/* --- external / native dispatch ----------------------------------------- */

static void note_missing(struct dvm *vm, const char *what)
{
   for (int i = 0; i < vm->nmissing; ++i)
      if (!strcmp(vm->missing[i], what)) return;
   if (vm->nmissing == vm->missing_cap) {
      int cap = vm->missing_cap ? vm->missing_cap * 2 : 64;
      char **n = realloc(vm->missing, (size_t)cap * sizeof *n);
      if (!n) return;
      vm->missing = n;
      vm->missing_cap = cap;
   }
   vm->missing[vm->nmissing++] = strdup(what);
   fprintf(stderr, "[dvm] WARNING: %s\n", what);
}

/* Descriptors the boot class loader defines: java/, javax/, android/ and the
 * runtime's own dalvik/ and libcore/.  androidx/ is application code. */
static bool desc_is_boot_class(const char *desc)
{
   static const char *const ns[] = {
      "Ljava/", "Ljavax/", "Landroid/", "Ldalvik/", "Llibcore/",
   };
   for (size_t i = 0; i < sizeof ns / sizeof ns[0]; ++i)
      if (!strncmp(desc, ns[i], strlen(ns[i]))) return true;
   return false;
}

static struct dvm_class *class_by_desc_unlocked(struct dvm *vm, const char *desc)
{
   if (!desc || !*desc) return NULL;

   struct dvm_class *c = class_lookup(vm, desc);
   if (c) return c;

   if (desc[0] == '[') {
      c = class_register(vm, desc);
      if (!c) return NULL;
      c->is_array = true;
      c->init_state = 2;
      c->elem_kind = dvm__kind_of(desc + 1);
      c->elem = dvm__class_by_desc(vm, desc + 1);
      c->super = dvm__class_by_desc(vm, "Ljava/lang/Object;");
      return c;
   }
   if (desc[1] == '\0' && strchr("ZBCSIJFDV", desc[0]))
      return dvm__define_primitive(vm, desc);

   /* The boot classpath wins for the namespaces the platform owns: ART's
    * application loader asks its parent first, and java.* may not be defined
    * by an app at all.  APKs do carry android.* classes (R8 API-modelling
    * stubs for APIs newer than minSdk, e.g. android.os.LocaleList) that must
    * never shadow the real class, which this device has.  A platform class
    * the runtime does not model yet falls through to the dex definition, then
    * to the host-backed stub below. */
   if (desc_is_boot_class(desc)) {
      c = dvm_runtime_define(vm, desc);
      if (c) return c;
   }
   /* Otherwise a dex definition wins; then a built-in; then a host stub. */
   for (int i = 0; i < vm->ndexes; ++i) {
      int idx = dex_find_class(&vm->dexes[i]->file, desc);
      if (idx < 0) continue;
      c = class_register(vm, desc);
      if (!c) return NULL;
      if (!class_load_from_dex(vm, c, vm->dexes[i], (uint32_t)idx)) {
         c->external = true;
         c->init_state = 2;
      }
      return c;
   }

   c = dvm_runtime_define(vm, desc);
   if (c) return c;

   /* Every real class descriptor starts with L, [ or one primitive letter.
    * A synthesized class with any other first byte (for example the
    * intermittent cobbler ͌.<init>()V name in 72bi/72bs) is a corruption
    * signal, not a platform stub: record its bytes and the method that asked
    * for it so the caller can be identified instead of guessed at. */
   if (desc[0] != 'L' && desc[0] != '[' && !(desc[1] == '\0' && strchr("ZBCSIJFDV", desc[0]))) {
      char hex[3 * 32 + 1]; size_t at = 0;
      for (size_t i = 0; i < 32 && desc[i]; ++i)
         at += (size_t)snprintf(hex + at, sizeof hex - at, "%02x", (unsigned char)desc[i]);
      hex[at] = '\0';
      fprintf(stderr, "SYNTH_CLASS_ODD desc_hex=%s caller=%s.%s\n", hex,
              vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?",
              vm->cur_method ? vm->cur_method->name : "?");
   }
   c = class_register(vm, desc);
   if (!c) return NULL;
   c->external = true;
   c->synthesized = true;
   c->init_state = 2;
    if (!strcmp(desc, "Landroid/location/LocationListener;") ||
        !strcmp(desc, "Landroid/media/AudioManager$OnAudioFocusChangeListener;") ||
        !strcmp(desc, "Landroid/view/View$OnSystemUiVisibilityChangeListener;") ||
        !strcmp(desc, "Landroid/view/View$OnLayoutChangeListener;") ||
        !strcmp(desc, "Landroid/content/SharedPreferences;") ||
        !strcmp(desc, "Landroid/content/SharedPreferences$Editor;") ||
        /* InvocationHandler 是 proxy 契约接口；handler 实现类来自 guest
         * DEX，接口本身由 native FindClass 解析时需要接口身份。 */
        !strcmp(desc, "Ljava/lang/reflect/InvocationHandler;")) {
      c->is_interface = true;
      c->access = DEX_ACC_PUBLIC | DEX_ACC_INTERFACE | DEX_ACC_ABSTRACT;
   }
   const char *super = !strcmp(desc, "Ljava/util/NoSuchElementException;")
                        ? "Ljava/lang/RuntimeException;" : "Ljava/lang/Object;";
   c->super = strcmp(desc, "Ljava/lang/Object;") ? dvm__class_by_desc(vm, super) : NULL;
   /* AOSP BroadcastReceiver 的真实实例状态；必须在子类布局计算前声明。 */
   if(!strcmp(desc,"Landroid/content/BroadcastReceiver;")){
      struct dvm_field *fields=calloc(2,sizeof *fields);
      if(!fields){dvm__throw(vm,"java/lang/OutOfMemoryError","BroadcastReceiver fields");return NULL;}
      unsigned base=c->super?c->super->islots:0;
      fields[0]=(struct dvm_field){.name="mPendingResult",.type="Landroid/content/BroadcastReceiver$PendingResult;",.access=DEX_ACC_PRIVATE,.cls=c,.slot=(uint16_t)base,.width=1,.kind='L'};
      fields[1]=(struct dvm_field){.name="mDebugUnregister",.type="Z",.access=DEX_ACC_PRIVATE,.cls=c,.slot=(uint16_t)(base+1),.width=1,.kind='Z'};
      c->ifields=fields;c->nifields=2;c->islots=base+2;
   }
   /* Exact implemented Bundle signatures: expose existing value backend to
    * JNI and reflection without accepting arbitrary names or signatures. */
   if(!strcmp(desc,"Landroid/os/Bundle;")){
      static const struct {const char *name,*sig;} entries[]={
         {"<init>","()V"},
         {"<init>","(Landroid/os/Bundle;)V"},
         {"<init>","(I)V"},
         {"putAll","(Landroid/os/Bundle;)V"},
         {"size","()I"},
         {"isEmpty","()Z"},
         {"containsKey","(Ljava/lang/String;)Z"},
         {"get","(Ljava/lang/String;)Ljava/lang/Object;"},
         {"remove","(Ljava/lang/String;)V"},
         {"clear","()V"},
         {"keySet","()Ljava/util/Set;"},
         {"getString","(Ljava/lang/String;)Ljava/lang/String;"},
         {"getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"},
         {"getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;"},
         {"getCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;"},
         {"getInt","(Ljava/lang/String;)I"},
         {"getInt","(Ljava/lang/String;I)I"},
         {"getLong","(Ljava/lang/String;)J"},
         {"getLong","(Ljava/lang/String;J)J"},
         {"getBoolean","(Ljava/lang/String;)Z"},
         {"getBoolean","(Ljava/lang/String;Z)Z"},
         {"getDouble","(Ljava/lang/String;)D"},
         {"getDouble","(Ljava/lang/String;D)D"},
         {"getFloat","(Ljava/lang/String;)F"},
         {"getFloat","(Ljava/lang/String;F)F"},
         {"getShort","(Ljava/lang/String;)S"},
         {"getShort","(Ljava/lang/String;S)S"},
         {"getByte","(Ljava/lang/String;)B"},
         {"getByte","(Ljava/lang/String;B)B"},
         {"getChar","(Ljava/lang/String;)C"},
         {"getChar","(Ljava/lang/String;C)C"},
         {"getStringArray","(Ljava/lang/String;)[Ljava/lang/String;"},
         {"getIntArray","(Ljava/lang/String;)[I"},
         {"getByteArray","(Ljava/lang/String;)[B"},
         {"getCharArray","(Ljava/lang/String;)[C"},
         {"getCharSequenceArray","(Ljava/lang/String;)[Ljava/lang/CharSequence;"},
         {"getBundle","(Ljava/lang/String;)Landroid/os/Bundle;"},
         {"getParcelable","(Ljava/lang/String;)Landroid/os/Parcelable;"},
         {"getSerializable","(Ljava/lang/String;)Ljava/io/Serializable;"},
         {"getStringArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;"},
         {"getIntegerArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;"},
         {"getParcelableArrayList","(Ljava/lang/String;)Ljava/util/ArrayList;"},
         {"putString","(Ljava/lang/String;Ljava/lang/String;)V"},
         {"putInt","(Ljava/lang/String;I)V"},
         {"putLong","(Ljava/lang/String;J)V"},
         {"putBoolean","(Ljava/lang/String;Z)V"},
         {"putDouble","(Ljava/lang/String;D)V"},
         {"putFloat","(Ljava/lang/String;F)V"},
         {"putShort","(Ljava/lang/String;S)V"},
         {"putByte","(Ljava/lang/String;B)V"},
         {"putChar","(Ljava/lang/String;C)V"},
         {"putCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)V"},
         {"putStringArray","(Ljava/lang/String;[Ljava/lang/String;)V"},
         {"putIntArray","(Ljava/lang/String;[I)V"},
         {"putByteArray","(Ljava/lang/String;[B)V"},
         {"putCharArray","(Ljava/lang/String;[C)V"},
         {"putCharSequenceArray","(Ljava/lang/String;[Ljava/lang/CharSequence;)V"},
         {"putBundle","(Ljava/lang/String;Landroid/os/Bundle;)V"},
         {"putParcelable","(Ljava/lang/String;Landroid/os/Parcelable;)V"},
         {"putSerializable","(Ljava/lang/String;Ljava/io/Serializable;)V"},
         {"putStringArrayList","(Ljava/lang/String;Ljava/util/ArrayList;)V"},
         {"putIntegerArrayList","(Ljava/lang/String;Ljava/util/ArrayList;)V"},
         {"putParcelableArrayList","(Ljava/lang/String;Ljava/util/ArrayList;)V"},
      };
      unsigned count=sizeof entries/sizeof entries[0];c->methods=calloc(count,sizeof *c->methods);
      if(!c->methods){dvm__throw(vm,"java/lang/OutOfMemoryError","Bundle method metadata");return NULL;}
      for(unsigned i=0;i<count;i++){struct dvm_method *m=&c->methods[i];m->cls=c;m->name=entries[i].name;m->sig=strdup(entries[i].sig);if(!m->sig){dvm__throw(vm,"java/lang/OutOfMemoryError","Bundle signature");return NULL;}m->access=DEX_ACC_PUBLIC;m->arg_slots=dvm_sig_arg_slots(m->sig);m->arg_count=dvm_sig_arg_count(m->sig);m->ret_kind=dvm_sig_return_kind(m->sig);}c->nmethods=count;
   }
   /* Notification value objects only: no delivery, permission grant or UI. */
   if(!strcmp(desc,"Landroid/app/Notification;")||!strcmp(desc,"Landroid/app/Notification$Builder;")){
      bool builder=strstr(desc,"$Builder;")!=NULL;
      static const struct {const char *name,*type;} fs[]={
         {"extras","Landroid/os/Bundle;"},{"channelId","Ljava/lang/String;"},
         {"contentTitle","Ljava/lang/CharSequence;"},{"contentText","Ljava/lang/CharSequence;"},
         {"color","I"},{"number","I"},{"when","J"},{"icon","I"},{"flags","I"},
         {"autoCancel","Z"},{"showWhen","Z"},{"usesChronometer","Z"},
         {"group","Ljava/lang/String;"},{"groupSummary","Z"},{"sortKey","Ljava/lang/String;"},
         {"priority","I"},{"defaults","I"},{"visibility","I"},{"vibrate","[J"},{"style","Landroid/app/Notification$Style;"},
         {"groupAlertBehavior","I"},{"colorized","Z"}
      };
      unsigned count=sizeof fs/sizeof fs[0],base=c->super?c->super->islots:0,slot=base;
      c->ifields=calloc(count,sizeof *c->ifields);if(!c->ifields){dvm__throw(vm,"java/lang/OutOfMemoryError","Notification fields");return NULL;}
      for(unsigned i=0;i<count;i++){unsigned width=1;c->ifields[i]=(struct dvm_field){.name=fs[i].name,.type=fs[i].type,.access=!strcmp(fs[i].name,"extras")||!strcmp(fs[i].name,"color")||(!builder&&!strcmp(fs[i].name,"flags"))?DEX_ACC_PUBLIC:DEX_ACC_PRIVATE,.cls=c,.slot=(uint16_t)slot,.width=width,.kind=dvm__kind_of(fs[i].type)};slot+=width;}
      c->nifields=count;c->islots=slot;
      if(!builder){
         static const struct {const char *name,*value;} constants[]={
            {"EXTRA_TITLE","android.title"},{"EXTRA_TEXT","android.text"},{"EXTRA_INFO_TEXT","android.infoText"},
            {"EXTRA_SUB_TEXT","android.subText"},{"EXTRA_SUMMARY_TEXT","android.summaryText"},{"EXTRA_TITLE_BIG","android.title.big"},
            {"EXTRA_BIG_TEXT","android.bigText"},{"EXTRA_TEXT_LINES","android.textLines"},{"EXTRA_TEMPLATE","android.template"},
            {"EXTRA_SHOW_WHEN","android.showWhen"},{"EXTRA_SHOW_CHRONOMETER","android.showChronometer"},
            {"EXTRA_CHRONOMETER_COUNT_DOWN","android.chronometerCountDown"},{"EXTRA_LARGE_ICON","android.largeIcon"},
            {"EXTRA_LARGE_ICON_BIG","android.largeIcon.big"},{"EXTRA_SMALL_ICON","android.icon"},{"EXTRA_PICTURE","android.picture"},
            {"EXTRA_PROGRESS","android.progress"},{"EXTRA_PROGRESS_MAX","android.progressMax"},
            {"EXTRA_PROGRESS_INDETERMINATE","android.progressIndeterminate"},{"EXTRA_PEOPLE","android.people"}
         };
         static const struct {const char *name;int value;} ints[]={
            {"FLAG_SHOW_LIGHTS",1},{"FLAG_ONGOING_EVENT",2},{"FLAG_INSISTENT",4},{"FLAG_ONLY_ALERT_ONCE",8},
            {"FLAG_AUTO_CANCEL",16},{"FLAG_NO_CLEAR",32},{"FLAG_FOREGROUND_SERVICE",64},{"FLAG_HIGH_PRIORITY",128},
            {"FLAG_LOCAL_ONLY",256},{"FLAG_GROUP_SUMMARY",512},{"DEFAULT_ALL",-1},{"DEFAULT_SOUND",1},{"DEFAULT_VIBRATE",2},{"DEFAULT_LIGHTS",4},
            {"PRIORITY_MIN",-2},{"PRIORITY_LOW",-1},{"PRIORITY_DEFAULT",0},{"PRIORITY_HIGH",1},{"PRIORITY_MAX",2},
            {"VISIBILITY_SECRET",-1},{"VISIBILITY_PRIVATE",0},{"VISIBILITY_PUBLIC",1},
            {"GROUP_ALERT_ALL",0},{"GROUP_ALERT_SUMMARY",1},{"GROUP_ALERT_CHILDREN",2}
         };
         unsigned nc=sizeof constants/sizeof constants[0],ni=sizeof ints/sizeof ints[0];c->sfields=calloc(nc+ni,sizeof *c->sfields);c->sslots=calloc(nc+ni,sizeof *c->sslots);
         if(!c->sfields||!c->sslots){dvm__throw(vm,"java/lang/OutOfMemoryError","Notification constants");return NULL;}
         c->nsfields=c->nsslots=nc+ni;
         for(unsigned i=0;i<nc;i++){c->sfields[i]=(struct dvm_field){.name=constants[i].name,.type="Ljava/lang/String;",.access=0x19,.cls=c,.slot=(uint16_t)i,.width=1,.kind='L'};c->sslots[i].l=dvm_new_string(vm,constants[i].value);if(!c->sslots[i].l)return NULL;}
         for(unsigned j=0;j<ni;j++){unsigned i=nc+j;c->sfields[i]=(struct dvm_field){.name=ints[j].name,.type="I",.access=0x19,.cls=c,.slot=(uint16_t)i,.width=1,.kind='I'};c->sslots[i].i=ints[j].value;}
      }
      static const struct {const char *name,*sig;} bs[]={
         {"<init>","(Landroid/content/Context;)V"},{"<init>","(Landroid/content/Context;Ljava/lang/String;)V"},
         {"getExtras","()Landroid/os/Bundle;"},{"build","()Landroid/app/Notification;"},
#define NS(n,t) {"set" n,"(" t ")Landroid/app/Notification$Builder;"}
         NS("ContentTitle","Ljava/lang/CharSequence;"),NS("ContentText","Ljava/lang/CharSequence;"),
         NS("Color","I"),NS("Number","I"),NS("When","J"),NS("SmallIcon","I"),
         NS("AutoCancel","Z"),NS("Ongoing","Z"),NS("Style","Landroid/app/Notification$Style;"),NS("ShowWhen","Z"),NS("UsesChronometer","Z"),
         NS("Group","Ljava/lang/String;"),NS("GroupSummary","Z"),NS("SortKey","Ljava/lang/String;"),
         NS("Priority","I"),NS("Defaults","I"),NS("Visibility","I"),NS("Vibrate","[J"),
         NS("GroupAlertBehavior","I"),NS("Colorized","Z")
#undef NS
      };
      static const struct {const char *name,*sig;} ns[]={{"<init>","()V"},{"getChannelId","()Ljava/lang/String;"},{"getGroup","()Ljava/lang/String;"},{"getSortKey","()Ljava/lang/String;"},{"getGroupAlertBehavior","()I"}};
      unsigned nm=builder?sizeof bs/sizeof bs[0]:sizeof ns/sizeof ns[0];c->methods=calloc(nm,sizeof *c->methods);
      if(!c->methods){dvm__throw(vm,"java/lang/OutOfMemoryError","Notification methods");return NULL;}
      for(unsigned i=0;i<nm;i++){struct dvm_method *m=&c->methods[i];m->cls=c;m->name=builder?bs[i].name:ns[i].name;m->sig=strdup(builder?bs[i].sig:ns[i].sig);if(!m->sig){dvm__throw(vm,"java/lang/OutOfMemoryError","Notification signature");return NULL;}m->access=DEX_ACC_PUBLIC;m->arg_slots=dvm_sig_arg_slots(m->sig);m->arg_count=dvm_sig_arg_count(m->sig);m->ret_kind=dvm_sig_return_kind(m->sig);}c->nmethods=nm;
   }
   return c;
}

struct dvm_class *dvm__class_by_desc(struct dvm *vm, const char *desc)
{
   class_registry_enter();
   struct dvm_class *c = class_by_desc_unlocked(vm, desc);
   class_registry_leave();
   return c;
}

/* Is this descriptor in one of the namespaces a device provides?
 *
 * The stub above is how framework classes the emulator has not modelled yet
 * still answer calls, and for java/, android/ and their neighbours that is a
 * reasonable fiction: the class exists on a device whether or not this
 * emulator knows it.  For a class in the *application's* own namespace it is
 * not: the APK is the only place such a class can come from, so if no dex
 * defines it, it does not exist — and pretending otherwise changes what the
 * app does.  See the NoClassDefFoundError in new-instance. */
static bool desc_is_platform(const char *desc)
{
   static const char *const ns[] = {
      "Ljava/", "Ljavax/", "Landroid/", "Landroidx/", "Ldalvik/", "Llibcore/",
      "Lsun/", "Lcom/android/", "Lorg/apache/", "Lorg/json/", "Lorg/w3c/",
      "Lorg/xml/", "Lorg/xmlpull/", "Ljunit/",
   };
   if (!desc) return true;
   if (desc[0] == '[') return true;             /* arrays are synthesised */
   for (size_t i = 0; i < sizeof ns / sizeof ns[0]; ++i)
      if (!strncmp(desc, ns[i], strlen(ns[i]))) return true;
   return false;
}

/* The class the dex names is nowhere: not in the APK, not in the platform,
 * not implemented here.  Reported once per name, because the app's own
 * handling of it (a try/catch around an optional plugin) is normal and this
 * is only interesting when it is not. */
static bool class_absent(struct dvm *vm, struct dvm_class *c)
{
   if (!c || !c->synthesized || desc_is_platform(c->desc)) return false;
   /* A class whose name is in a platform namespace but whose desc is not
    * platform-shaped (or the reverse) means the record was overwritten: the
    * name/desc pair no longer describes the same class.  The 72by run failed
    * with NoClassDefFoundError: java/lang/Object, which this function can
    * only return for a non-platform descriptor, so the pair had diverged.
    * Print both fields, the pointer and the caller before treating it as an
    * ordinary absent optional class. */
   if (c->name && (!strncmp(c->name, "java/", 5) || !strncmp(c->name, "android/", 8)))
      fprintf(stderr, "ABSENT_ODD vm=%p cls=%p name=%s desc=%s synthesized=%d external=%d caller=%s.%s\n",
              (void *)vm, (void *)c, c->name, c->desc ? c->desc : "?",
              c->synthesized ? 1 : 0, c->external ? 1 : 0,
              vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?",
              vm->cur_method ? vm->cur_method->name : "?");
   char what[512];
   snprintf(what, sizeof what, "class not found in the APK or the platform, stubbed: %s",
            c->name ? c->name : c->desc);
   note_missing(vm, what);
   return true;
}

struct dvm_class *dvm_find_class(struct dvm *vm, const char *name)
{
   char desc[512];
   name_to_desc(name, desc, sizeof desc);
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   return (c && !c->external) ? c : NULL;
}

struct dvm_class *dvm_find_class_or_external(struct dvm *vm, const char *name)
{
   char desc[512];
   name_to_desc(name, desc, sizeof desc);
   return dvm__class_by_desc(vm, desc);
}

bool dvm_class_is_known(struct dvm *vm, const char *name)
{
   char desc[512];
   name_to_desc(name, desc, sizeof desc);
   for (int i = 0; i < vm->ndexes; ++i)
      if (dex_find_class(&vm->dexes[i]->file, desc) >= 0) return true;
   struct dvm_class *c = class_lookup(vm, desc);
   return c && !c->external;
}

/* Existence comes from the app's DEX, provided VM classes, or the platform's
 * class inventory. A package prefix is not a class definition: Android's
 * Support Library, for example, ships in the app rather than the boot loader.
 * An external placeholder created while resolving a reference is not proof
 * that the referenced class exists. */
static const struct wrapper_shape {const char *name;char primitive;const char *unbox;} wrapper_shapes[]={
 {"java/lang/Boolean",'Z',"booleanValue"},{"java/lang/Byte",'B',"byteValue"},
 {"java/lang/Character",'C',"charValue"},{"java/lang/Short",'S',"shortValue"},
 {"java/lang/Integer",'I',"intValue"},{"java/lang/Long",'J',"longValue"},
 {"java/lang/Float",'F',"floatValue"},{"java/lang/Double",'D',"doubleValue"}};
extern bool content_runtime_class_supported(const char *) __attribute__((weak));
extern bool content_runtime_method_supported(const char *,const char *,const char *) __attribute__((weak));
bool dvm_class_exists(struct dvm *vm, const char *name)
{
   if (!name || !*name) return false;
   if(content_runtime_class_supported){char plain[512];const char *candidate=name;size_t length=strlen(name);
      if(name[0]=='L'&&length>2&&name[length-1]==';'&&length<sizeof plain){memcpy(plain,name+1,length-2);plain[length-2]=0;candidate=plain;}
      if(content_runtime_class_supported(candidate))return true;
   }
   char desc[512];
   name_to_desc(name, desc, sizeof desc);
   /* reflection_runtime installs the real singleton field and allocation
    * builtin. Discovery must expose that platform implementation to Gson. */
   if (!strcmp(desc,"Lsun/misc/Unsafe;")) return true;
   if(!strcmp(desc,"Ljava/io/CharArrayWriter;")||!strcmp(desc,"Ljava/io/CharArrayReader;"))return true;
   if(!strcmp(desc,"Ljava/security/spec/EncodedKeySpec;")||!strcmp(desc,"Ljava/security/spec/X509EncodedKeySpec;")||
      !strcmp(desc,"Ljava/security/spec/KeySpec;")||!strcmp(desc,"Ljava/security/spec/InvalidKeySpecException;")||
      !strcmp(desc,"Ljava/security/KeyFactory;")||!strcmp(desc,"Ljava/security/Key;")||!strcmp(desc,"Ljava/security/PublicKey;")||
      !strcmp(desc,"Ljava/security/interfaces/RSAPublicKey;")||!strcmp(desc,"Ljava/security/Signature;")||
      !strcmp(desc,"Ljava/security/SignatureException;")||!strcmp(desc,"Ljavax/crypto/Cipher;"))return true;
   if (!strcmp(desc,"Landroid/view/LayoutInflater;")||!strcmp(desc,"Landroid/webkit/CookieManager;")||!strcmp(desc,"Landroid/webkit/CookieSyncManager;")||!strcmp(desc,"Landroid/webkit/ValueCallback;")||!strcmp(desc,"Landroid/webkit/WebSettings$ZoomDensity;")||!strcmp(desc,"Landroid/webkit/WebSettings$PluginState;")||!strcmp(desc,"Landroid/webkit/WebSettings$LayoutAlgorithm;")||!strcmp(desc,"Landroid/webkit/WebView;")||!strcmp(desc,"Landroid/webkit/WebSettings;")||!strcmp(desc,"Landroid/webkit/WebViewClient;")||!strcmp(desc,"Landroid/webkit/WebChromeClient;")||!strcmp(desc,"Landroid/app/DialogFragment;")||!strcmp(desc,"Landroid/app/Fragment;")||!strcmp(desc,"Lorg/json/JSONTokener;")||!strcmp(desc,"Lorg/json/JSONArray;")) return true;
   if (!strcmp(desc,"Ljava/util/Set;")) return true;
   if (!strcmp(desc,"Ljava/util/Vector;")||!strcmp(desc,"Ljava/util/Stack;")||!strcmp(desc,"Ljava/util/WeakHashMap;")) return true;
   /* Use the same exact inventory that installs the real exception hierarchy.
    * A runtime-created Error must also be discoverable by native catch code. */
   if (exception_hierarchy_class_exists(desc)) return true;
   /* Existing ssl_runtime/cert_runtime payloads, discovered by Unity TLS. */
   if(!strcmp(desc,"Ljavax/net/ssl/TrustManagerFactory;")||
      !strcmp(desc,"Ljavax/net/ssl/TrustManager;")||!strcmp(desc,"Ljavax/net/ssl/X509TrustManager;")||
      !strcmp(desc,"Ljavax/net/ssl/DefaultX509TrustManager;")||
      !strcmp(desc,"Ljava/security/cert/Certificate;")||!strcmp(desc,"Ljava/security/cert/X509Certificate;")||!strcmp(desc,"Ljavax/security/auth/x500/X500Principal;"))return true;
   for(size_t i=0;i<sizeof wrapper_shapes/sizeof wrapper_shapes[0];i++){
      size_t n=strlen(wrapper_shapes[i].name);
      if(desc[0]=='L'&&strlen(desc)==n+2&&!memcmp(desc+1,wrapper_shapes[i].name,n)&&desc[n+1]==';')return true;
   }
   /* These exact platform types have host runtimes even when the strict
    * inventory has not listed them. This does not validate arbitrary stubs. */
   if (!strcmp(desc, "Ljava/lang/StringBuilder;") ||
       !strcmp(desc, "Ljava/util/TimeZone;") ||
       !strcmp(desc, "Ljava/lang/StringBuffer;") ||
       /* Implemented by android_basics/system_runtime, reflection_runtime,
        * throwable_reference and java_log_runtime respectively. Native
        * discovery must see the same types as the existing dispatchers. */
       !strcmp(desc, "Ljava/lang/System;") ||
        !strcmp(desc, "Ljava/lang/reflect/Field;") ||
        !strcmp(desc, "Ljava/lang/reflect/Modifier;") ||
        !strcmp(desc, "Ljava/lang/reflect/Type;") ||
        !strcmp(desc, "Ljava/lang/reflect/ParameterizedType;") ||
        /* 反射三件套的其余两类与 proxy 契约接口：方法体均由
         * reflection_runtime_call 分派（Method 8 个方法 /
         * Constructor.newInstance 族）；72qa 实测 native 侧
         * FindClass(java/lang/reflect/Method) 失败留 pending=1229 不消，
         * 打断 ToReflectedMethod 之后的帧回调链。 */
        !strcmp(desc, "Ljava/lang/reflect/Method;") ||
        !strcmp(desc, "Ljava/lang/reflect/Constructor;") ||
        !strcmp(desc, "Ljava/lang/reflect/InvocationHandler;") ||
       !strcmp(desc, "Ljava/lang/Throwable;") ||
       !strcmp(desc, "Landroid/util/Log;") ||
       !strcmp(desc, "Lorg/json/JSONObject;") ||
       !strcmp(desc, "Lorg/json/JSONException;") ||
       !strcmp(desc, "Ljava/lang/ClassLoader;") ||
       /* Core interface the engine resolves from native code before posting
        * the resume Runnable to the main Looper.  It has no host payload and
        * needs no fields; the DEX class implementing it carries the code. */
       !strcmp(desc, "Ljava/lang/Runnable;") ||
       !strcmp(desc, "Landroid/app/Activity;") ||
       !strcmp(desc, "Landroid/app/Notification;") ||
       !strcmp(desc, "Landroid/app/NotificationChannel;") ||
       !strcmp(desc, "Landroid/app/NotificationManager;") ||
       !strcmp(desc, "Landroid/os/Vibrator;") ||
       !strcmp(desc, "Landroid/app/Notification$Builder;") ||
       !strcmp(desc, "Landroid/app/Notification$Style;") ||
       !strcmp(desc, "Landroid/content/Context;") ||
       !strcmp(desc, "Landroid/content/Intent;") ||
       !strcmp(desc, "Landroid/view/ContextThemeWrapper;") ||
       !strcmp(desc, "Landroid/os/Build;") ||
       /* Build$VERSION 由 android_basics_vm_start 建立真实静态字段
        * （SDK_INT/RELEASE/…），forName 需要把它视为存在的宿主类。 */
       !strcmp(desc, "Landroid/os/Build$VERSION;") ||
       !strcmp(desc, "Landroid/view/WindowManager;") ||
       !strcmp(desc, "Landroid/os/Bundle;") ||
       !strcmp(desc, "Landroid/content/res/AssetManager;") ||
       !strcmp(desc, "Landroid/content/res/Resources;") ||
       !strcmp(desc, "Landroid/hardware/display/DisplayManager;") ||
       !strcmp(desc, "Landroid/view/Display;") ||
       !strcmp(desc, "Landroid/net/Uri;") ||
       !strcmp(desc, "Landroid/net/Uri$Builder;") ||
       !strcmp(desc, "Landroid/view/animation/DecelerateInterpolator;") ||
       !strcmp(desc, "Landroid/view/animation/AccelerateInterpolator;") ||
       !strcmp(desc, "Ljava/util/Locale;") ||
       !strcmp(desc, "Landroid/util/DisplayMetrics;") ||
       !strcmp(desc, "Landroid/util/TypedValue;") ||
       !strcmp(desc, "Landroid/view/SurfaceView;") ||
       !strcmp(desc, "Landroid/os/Process;") ||
        /* Environment 的静态方法由 android_basics 分发到宿主沙箱真实目录。 */
        !strcmp(desc, "Landroid/os/Environment;") ||
        !strcmp(desc, "Landroid/os/Looper;") ||
        !strcmp(desc, "Landroid/os/MessageQueue;") ||
        /* HandlerThread 有 thread_runtime 的真实 pthread worker 后端
         * （构造/start/getLooper/quit）；android_basics_vm_start 把它的
         * super 链接到 Thread。72pr 实测 FindClass 缺这一行会在
         * nativeRender 尾部留下 pending=1225 的 NoClassDefFoundError
         * 不消，主循环停摆。 */
        !strcmp(desc, "Landroid/os/HandlerThread;") ||
        /* Handler 的构造/投递/移除由 android_basics（主 Looper）与
         * thread_runtime（worker Looper）分派；72pu 实测 nativeRender
         * 尾部 FindClass(Handler) 失败留 pending=1226 不消（Handler$Callback
         * 接口已在下方白名单，本体此前缺失）。 */
        !strcmp(desc, "Landroid/os/Handler;") ||
        /* java/lang/Thread 的方法体由 thread_runtime 分派（构造/start/
         * 名称/优先级/currentThread/getStackTrace）；72pv 实测 nativeRender
         * 尾部 FindClass(java/lang/Thread) 失败留 pending=1223 不消。 */
        !strcmp(desc, "Ljava/lang/Thread;") ||
        /* android/os/Message 的 obtain/sendToTarget 由 thread_runtime 分派；
         * 72px 实测 native 侧 FindClass(Message) 失败留 pending=1226 不消。 */
        !strcmp(desc, "Landroid/os/Message;") ||
        /* CRI 媒体路径 FindClass 的 android/media/MediaFormat（72pu 实测
         * pending=881 NoClassDefFoundError）；类存在性之外的方法仍按各
         * dispatcher fail closed。 */
        !strcmp(desc, "Landroid/media/MediaFormat;") ||
        /* MediaCodec 的公开内部类 BufferInfo（72pv 实测 pending=877
         * NoClassDefFoundError）；存在性之外的方法仍 fail closed。 */
        !strcmp(desc, "Landroid/media/MediaCodec$BufferInfo;") ||
        /* MediaCodecInfo（72pw 实测 pending=877 NoClassDefFoundError）；
         * 存在性之外的方法仍 fail closed。 */
        !strcmp(desc, "Landroid/media/MediaCodecInfo;") ||
       /* 引擎 native 的错误框通过该接口创建点击监听器；接口本身无宿主负载，
        * 实现方是 guest DEX 中的匿名/lambda 类。 */
       !strcmp(desc, "Landroid/content/DialogInterface$OnClickListener;") ||
       !strcmp(desc, "Landroid/content/DialogInterface$OnKeyListener;") ||
       !strcmp(desc, "Landroid/content/DialogInterface$OnCancelListener;") ||
       !strcmp(desc, "Landroid/content/DialogInterface$OnDismissListener;") ||
       !strcmp(desc, "Landroid/content/DialogInterface$OnShowListener;") ||
       !strcmp(desc, "Landroid/widget/CompoundButton$OnCheckedChangeListener;") ||
       /* 错误框组件：Builder/AlertDialog/Dialog 由 android_basics 分发最小真实
        * 语义（show 即触发 positive 回调，等价用户点 Ok）。 */
       !strcmp(desc, "Landroid/app/AlertDialog$Builder;") ||
       !strcmp(desc, "Landroid/app/AlertDialog;") ||
       !strcmp(desc, "Landroid/app/Dialog;") ||
       /* 引擎启动/音频/输入/显示路径通过 FindClass 直接解析的平台类型。
        * 存在性只让类可被解析；未实现的方法仍按各自 dispatcher fail closed。 */
       !strcmp(desc, "Landroid/view/View;") ||
       !strcmp(desc, "Landroid/view/MotionEvent;") ||
       !strcmp(desc, "Landroid/view/InputEvent;") ||
       /* nativeInjectEvent 首先 FindClass(android/view/KeyEvent) 判断事件
        * 类别；类不存在时 FindClass 抛异常并直接返回 false（72rd 实测
        * JNI_LOOKUP kind=class class=android/view/KeyEvent success=0）。 */
       !strcmp(desc, "Landroid/view/KeyEvent;") ||
       !strcmp(desc, "Landroid/app/ActivityManager;") ||
       !strcmp(desc, "Landroid/os/StatFs;") ||
       !strcmp(desc, "Landroid/app/ActivityManager$MemoryInfo;") ||
        !strcmp(desc, "Landroid/view/InputDevice;") ||
        !strcmp(desc, "Landroid/hardware/input/InputManager$InputDeviceListener;") ||
        /* Context.getSystemService("input") 的返回类型；管理器对象由
         * network_runtime 提供，方法在后端可用前 fail closed。 */
        !strcmp(desc, "Landroid/hardware/input/InputManager;") ||
       !strcmp(desc, "Landroid/hardware/display/DisplayManager$DisplayListener;") ||
       !strcmp(desc, "Landroid/content/pm/ActivityInfo;") ||
       !strcmp(desc, "Landroid/media/AudioManager;") ||
       !strcmp(desc, "Landroid/media/MediaCodec;") ||
       !strcmp(desc, "Landroid/media/MediaRouter;") ||
       /* UnityPlayer 的 Handler 回调路径 FindClass 该接口；pending 异常
        * 不清除会让主循环停摆（72pm 实测 pending=1209 持续）。 */
       !strcmp(desc, "Landroid/os/Handler$Callback;") ||
       /* 渲染循环：UnityPlayer 原生侧 FindClass Choreographer 及其帧回调接口
        * （72pn 实测该 FindClass 失败后 pending=1209 持续、主循环停摆）。 */
       !strcmp(desc, "Landroid/view/Choreographer;") ||
       !strcmp(desc, "Landroid/view/Choreographer$FrameCallback;") ||
       !strcmp(desc, "Ljava/lang/ref/SoftReference;") ||
       /* File paths and ApplicationInfo fields are provided by file_runtime
        * and reviewed_metadata; existence does not add unsupported methods. */
       !strcmp(desc, "Ljava/io/File;") ||
       !strcmp(desc, "Ljava/io/IOException;") ||
       !strcmp(desc, "Ljava/io/InputStream;") ||
       !strcmp(desc, "Ljava/io/ByteArrayInputStream;") ||
       !strcmp(desc, "Ljava/io/InputStreamReader;") ||
       !strcmp(desc, "Ljava/io/PrintWriter;") ||
       !strcmp(desc, "Landroid/location/LocationListener;") ||
       !strcmp(desc, "Landroid/media/AudioManager$OnAudioFocusChangeListener;") ||
       !strcmp(desc, "Landroid/view/View$OnSystemUiVisibilityChangeListener;") ||
       !strcmp(desc, "Landroid/view/View$OnLayoutChangeListener;") ||
       !strcmp(desc, "Landroid/content/SharedPreferences;") ||
       !strcmp(desc, "Landroid/content/SharedPreferences$Editor;") ||
       !strcmp(desc, "Ljava/util/NoSuchElementException;") ||
       !strcmp(desc, "Ljava/util/Scanner;") ||
       !strcmp(desc, "Landroid/content/pm/ApplicationInfo;") ||
       !strcmp(desc, "Landroid/content/pm/PackageInfo;") ||
       !strcmp(desc, "Landroid/content/pm/PackageManager;")) return true;
   if (desc[0] == '[') {
      const char *component = desc;
      while (*component == '[') ++component;
      if (component - desc > 255) return false;
      if (*component == 'L') return dvm_class_exists(vm, component);
      return component[0] && !component[1] && strchr("ZBCSIJFD", component[0]);
   }
   size_t length = strlen(desc);
   if (length < 3 || desc[0] != 'L' || desc[length - 1] != ';') return false;
   if (dvm_class_is_known(vm, desc)) return true;
   return dvm_platform_class_exists(desc);
}

const char *dvm_class_name(const struct dvm_class *c) { return c ? c->name : "?"; }
const char *dvm_class_super_name(const struct dvm_class *c)
{
   return (c && c->super) ? c->super->name : NULL;
}
const char *dvm_method_name(const struct dvm_method *m) { return m ? m->name : "?"; }
const char *dvm_method_sig(const struct dvm_method *m) { return m ? m->sig : "?"; }
bool dvm_method_is_static(const struct dvm_method *m)
{
   return m && (m->access & DEX_ACC_STATIC);
}

/* --- loading ------------------------------------------------------------ */

static void method_finish(struct dvm_method *m)
{
   m->arg_slots = dvm_sig_arg_slots(m->sig);
   if (m->arg_slots < 0) m->arg_slots = 0;
   m->arg_count = dvm_sig_arg_count(m->sig);
   if (m->arg_count < 0) m->arg_count = 0;
   m->ret_kind = dvm_sig_return_kind(m->sig);
}

static bool annotation_element_builtin(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out);

static bool class_load_from_dex(struct dvm *vm, struct dvm_class *c,
                                struct dvm_dex *dd, uint32_t class_def_idx)
{
   struct dex_file *d = &dd->file;
   struct dex_class_def cd;
   if (!dex_class_def(d, class_def_idx, &cd)) return false;

   c->dex = d;
   c->class_def_idx = class_def_idx;
   c->access = cd.access_flags;
   c->is_interface = (cd.access_flags & DEX_ACC_INTERFACE) != 0;
   c->static_values_off = cd.static_values_off;

   if (cd.superclass_idx != DEX_NO_INDEX) {
      const char *sd = dex_type(d, cd.superclass_idx);
      if (sd && strcmp(sd, c->desc))
         c->super = dvm__class_by_desc(vm, sd);
   }

   int ni = dex_interfaces(d, cd.interfaces_off, NULL, 0);
   if (ni > 0) {
      uint16_t *ifaces = malloc((size_t)ni * sizeof *ifaces);
      c->ifaces = calloc((size_t)ni, sizeof *c->ifaces);
      if (!ifaces || !c->ifaces) {
         free(ifaces);
         free(c->ifaces);
         c->ifaces = NULL;
         dvm__throw(vm, "java/lang/OutOfMemoryError", "class interfaces");
         return false;
      }
      (void)dex_interfaces(d, cd.interfaces_off, ifaces, ni);
      for (int i = 0; i < ni; ++i) {
         const char *t = dex_type(d, ifaces[i]);
         if (t) c->ifaces[c->nifaces++] = dvm__class_by_desc(vm, t);
      }
      free(ifaces);
   }

   struct dex_class_data data;
   if (!dex_class_data(d, cd.class_data_off, &data)) {
      /* A class_def with no class_data is legal (a marker interface).  It
       * still has a name, a super and interfaces, so it is fully usable. */
      c->islots = c->super ? c->super->islots : 0;
      return true;
   }

   /* Instance fields sit after the superclass's, so a subclass reference can
    * be read through a superclass field index. */
   int base = c->super ? c->super->islots : 0;
   c->nifields = (int)data.instance_fields_size;
   c->ifields = c->nifields ? calloc((size_t)c->nifields, sizeof *c->ifields) : NULL;
   if (c->nifields && !c->ifields) { dvm__throw(vm,"java/lang/OutOfMemoryError","class fields"); return false; }
   int slot = base;
   for (int i = 0; i < c->nifields; ++i) {
      struct dex_field_id fid;
      if (!dex_field_id(d, data.instance_fields[i].field_idx, &fid)) continue;
      struct dvm_field *f = &c->ifields[i];
      f->cls = c;
      f->name = dex_string(d, fid.name_idx);
      f->type = dex_type(d, fid.type_idx);
      f->access = data.instance_fields[i].access_flags;
      f->kind = dvm__kind_of(f->type);
      f->width = 1;   /* one union slot holds a wide value */
      f->slot = (uint16_t)slot++;
   }
   c->islots = slot;

   c->nsfields = (int)data.static_fields_size;
   c->sfields = c->nsfields ? calloc((size_t)c->nsfields, sizeof *c->sfields) : NULL;
   if (c->nsfields && !c->sfields) { dvm__throw(vm,"java/lang/OutOfMemoryError","class fields"); return false; }
   for (int i = 0; i < c->nsfields; ++i) {
      struct dex_field_id fid;
      if (!dex_field_id(d, data.static_fields[i].field_idx, &fid)) continue;
      struct dvm_field *f = &c->sfields[i];
      f->cls = c;
      f->name = dex_string(d, fid.name_idx);
      f->type = dex_type(d, fid.type_idx);
      f->access = data.static_fields[i].access_flags;
      f->kind = dvm__kind_of(f->type);
      f->width = 1;
      f->slot = (uint16_t)i;
   }
   c->nsslots = c->nsfields;
   c->sslots = c->nsslots ? calloc((size_t)c->nsslots, sizeof *c->sslots) : NULL;

   int nm = (int)(data.direct_methods_size + data.virtual_methods_size);
   c->methods = nm ? calloc((size_t)nm, sizeof *c->methods) : NULL;
   c->nmethods = 0;
   for (int pass = 0; pass < 2; ++pass) {
      struct dex_encoded_method *list = pass ? data.virtual_methods : data.direct_methods;
      uint32_t n = pass ? data.virtual_methods_size : data.direct_methods_size;
      for (uint32_t i = 0; i < n && c->methods; ++i) {
         struct dex_method_id mid;
         if (!dex_method_id(d, list[i].method_idx, &mid)) continue;
         struct dvm_method *m = &c->methods[c->nmethods];
         m->cls = c;
         m->name = dex_string(d, mid.name_idx);
         m->shorty = dex_proto_shorty(d, mid.proto_idx);
         m->access = list[i].access_flags;
         m->dex = d;
         m->sig = dex_proto_signature_alloc(d, mid.proto_idx);
         if (!m->sig) continue;
         if (list[i].code_off && dex_code(d, list[i].code_off, &m->code))
            m->has_code = true;
         method_finish(m);
         /* An annotation type's element declarations are abstract interface
          * methods in DEX, but a materialised annotation instance must answer
          * them from its encoded element map.  Bind every zero-argument
          * element here, so this works for arbitrary application annotations
          * rather than a Gson-specific proxy class. */
         if ((c->access & DEX_ACC_ANNOTATION) && m->arg_count == 0 &&
             strcmp(m->name, "<clinit>") && strcmp(m->name, "<init>"))
            m->builtin = annotation_element_builtin;
         ++c->nmethods;
      }
   }

   dex_class_data_release(&data);
   return true;
}

/* ------------------------------------------------------------------------ *
 * Resolution
 * ------------------------------------------------------------------------ */

static struct dvm_method *class_own_method(struct dvm_class *c, const char *name,
                                           const char *sig)
{
   for (int i = 0; i < c->nmethods; ++i) {
      if (!c->methods[i].name || strcmp(c->methods[i].name, name)) continue;
      if (sig && c->methods[i].sig && strcmp(c->methods[i].sig, sig)) continue;
      return &c->methods[i];
   }
   return NULL;
}

/* --- resolved-method cache ------------------------------------------------
 *
 * dvm_find_method() walks the superclass chain and then every interface,
 * comparing a name and a signature with strcmp at each step.  virtual_target()
 * calls it for every invoke-virtual whose receiver is not exactly the class
 * the call site named — which, in ordinary object-oriented code, is most of
 * them — so the walk ran millions of times a second and showed up in host
 * profiles as strcmp.  A device resolves a virtual call through a vtable slot
 * decided once; this is the same idea, one table per class.
 *
 * Only methods that were found are remembered.  A lookup that fails is the
 * path that ends in the host stubs, and a class can still be given its
 * superclass link after it has been registered, so a remembered "no such
 * method" could outlive the reason it was true.  A method array, by contrast,
 * is allocated once when the class is built and never grows, so a cached
 * pointer into it stays valid for the life of the class.
 *
 * Like every other mutation of a class this happens under the interpreter
 * lock, so the table needs no synchronisation of its own. */
static uint32_t mcache_hash(const char *name, const char *sig)
{
   uint32_t h = 2166136261u;
   for (const char *p = name; *p; ++p) h = (h ^ (unsigned char)*p) * 16777619u;
   if (sig) {
      h = (h ^ 0xffu) * 16777619u;
      for (const char *p = sig; *p; ++p) h = (h ^ (unsigned char)*p) * 16777619u;
   }
   return h | 1u;   /* zero means "empty slot" */
}

static bool mcache_key_eq(const struct dvm_mcache_slot *s, uint32_t h,
                          const char *name, const char *sig)
{
   if (s->hash != h) return false;
   if (strcmp(s->name, name)) return false;
   if ((sig == NULL) != (s->sig == NULL)) return false;
   return sig == NULL || !strcmp(s->sig, sig);
}

static struct dvm_method *mcache_get(const struct dvm_class *c, uint32_t h,
                                     const char *name, const char *sig)
{
   if (!c->mcache) return NULL;
   const uint32_t mask = c->mcache_cap - 1u;
   for (uint32_t i = h & mask, n = 0; n <= mask; i = (i + 1u) & mask, ++n) {
      const struct dvm_mcache_slot *s = &c->mcache[i];
      if (!s->hash) return NULL;                 /* probe ends at a free slot */
      if (mcache_key_eq(s, h, name, sig)) return s->m;
   }
   return NULL;
}

static void mcache_insert(struct dvm_mcache_slot *tab, uint32_t cap,
                          const struct dvm_mcache_slot *e)
{
   const uint32_t mask = cap - 1u;
   uint32_t i = e->hash & mask;
   while (tab[i].hash) i = (i + 1u) & mask;
   tab[i] = *e;
}

/* The key is stored as the strings the *found method* owns, never as the
 * caller's.  A call site resolves its signature into a scratch buffer on the
 * interpreter's C stack, and remembering that pointer would leave the table
 * comparing against a stack frame that has since been reused.  The method's
 * own name and signature are allocated with the class, compare equal to the
 * query by construction, and live exactly as long as the entry does.  A
 * lookup with no signature (the wildcard) is stored as one. */
static void mcache_put(struct dvm_class *c, uint32_t h, const char *name,
                       const char *sig, struct dvm_method *m)
{
   /* Equal by content to the query — class_own_method() matched them with
    * strcmp — so the hash computed from the query still describes the key.
    * The one exception is a method declared with no signature at all, which
    * matches any query: there is nothing to key that on, so it is not cached. */
   if (!m->name || (sig && !m->sig)) return;
   name = m->name;
   sig  = sig ? m->sig : NULL;
   if (c->mcache_len + 1u > c->mcache_cap - c->mcache_cap / 4u) {
      const uint32_t cap = c->mcache_cap ? c->mcache_cap * 2u : 16u;
      struct dvm_mcache_slot *tab = calloc(cap, sizeof *tab);
      if (!tab) return;                          /* a cache, not a ledger */
      for (uint32_t i = 0; i < c->mcache_cap; ++i)
         if (c->mcache[i].hash) mcache_insert(tab, cap, &c->mcache[i]);
      free(c->mcache);
      c->mcache = tab;
      c->mcache_cap = cap;
   }
   struct dvm_mcache_slot e = { h, name, sig, m };
   mcache_insert(c->mcache, c->mcache_cap, &e);
   ++c->mcache_len;
}

struct dvm_method *dvm_find_method_by_params(struct dvm *vm, struct dvm_class *cls,
                                             const char *name, const char *sig)
{
   struct dvm_method *m = dvm_find_method(vm, cls, name, sig);
   if (m || !cls || !sig) return m;
   const char *close = strchr(sig, ')');
   if (!close) return NULL;
   const size_t plen = (size_t)(close - sig) + 1;
   for (struct dvm_class *c = cls; c; c = c->super)
      for (int i = 0; i < c->nmethods; ++i) {
         const struct dvm_method *cand = &c->methods[i];
         if (cand->name && cand->sig && !strcmp(cand->name, name) &&
             !strncmp(cand->sig, sig, plen))
            return &c->methods[i];
      }
   return NULL;
}

/* Declarations backed by the platform dispatcher but absent from the small
 * boot metadata. Allocated separately so resolved JNI method IDs stay valid. */
static bool wrapper_method_supported(const char *cls,const char *name,const char *sig){
   if(!strcmp(cls,"java/lang/Integer")&&!strcmp(name,"valueOf")&&(!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Integer;")||!strcmp(sig,"(Ljava/lang/String;I)Ljava/lang/Integer;")))return true;
   if(!strcmp(cls,"java/util/TimeZone"))return
       (!strcmp(name,"getDefault")&&!strcmp(sig,"()Ljava/util/TimeZone;"))||
       (!strcmp(name,"getTimeZone")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/util/TimeZone;"))||
       (!strcmp(name,"getRawOffset")&&!strcmp(sig,"()I"))||
       (!strcmp(name,"getID")&&!strcmp(sig,"()Ljava/lang/String;"))||
       (!strcmp(name,"clone")&&!strcmp(sig,"()Ljava/lang/Object;"));
   if(!strcmp(cls,"java/lang/Thread")&&!strcmp(name,"sleep")&&(!strcmp(sig,"(J)V")||!strcmp(sig,"(JI)V")))return true;
   for(size_t i=0;i<sizeof wrapper_shapes/sizeof wrapper_shapes[0];i++){
      const struct wrapper_shape *w=&wrapper_shapes[i];if(strcmp(cls,w->name))continue;
      char expected[96];
      if(!strcmp(name,"<init>")){snprintf(expected,sizeof expected,"(%c)V",w->primitive);return !strcmp(sig,expected);}
      if(!strcmp(name,"valueOf")){snprintf(expected,sizeof expected,"(%c)L%s;",w->primitive,w->name);return !strcmp(sig,expected);}
      if(!strcmp(name,w->unbox)){snprintf(expected,sizeof expected,"()%c",w->primitive);return !strcmp(sig,expected);}
      if(w->primitive!='Z'&&w->primitive!='C'){
         static const char *const getters[]={"byteValue","shortValue","intValue","longValue","floatValue","doubleValue"};
         static const char types[]="BSIJFD";
         for(size_t j=0;j<6;j++)if(!strcmp(name,getters[j])){snprintf(expected,sizeof expected,"()%c",types[j]);return !strcmp(sig,expected);}
      }
      return false;
   }
   return false;
}
static bool platform_extra_supported(const char *cls, const char *name, const char *sig)
{
   if(content_runtime_method_supported&&content_runtime_method_supported(cls,name,sig))return true;
   if(io_runtime_method_supported && io_runtime_method_supported(cls,name,sig))return true;
   if(!strcmp(cls,"java/lang/StringBuffer")){const char *suffix=strstr(sig,")Ljava/lang/StringBuffer;");char mapped[192];if(suffix){size_t n=(size_t)(suffix-sig)+1;if(n+strlen("Ljava/lang/StringBuilder;")+1>sizeof mapped)return false;memcpy(mapped,sig,n);strcpy(mapped+n,"Ljava/lang/StringBuilder;");sig=mapped;}return platform_extra_supported("java/lang/StringBuilder",name,sig);}
   if(!strcmp(name,"toHexString"))return (!strcmp(cls,"java/lang/Integer")&&!strcmp(sig,"(I)Ljava/lang/String;"))||(!strcmp(cls,"java/lang/Long")&&!strcmp(sig,"(J)Ljava/lang/String;"));
   if(!strcmp(cls,"java/nio/charset/Charset"))
      return (!strcmp(name,"forName")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/nio/charset/Charset;"))||
             (!strcmp(name,"name")&&!strcmp(sig,"()Ljava/lang/String;"));
   if(!strcmp(cls,"java/security/Signature"))
      return (!strcmp(name,"getInstance")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/security/Signature;"))||
             (!strcmp(name,"initVerify")&&!strcmp(sig,"(Ljava/security/PublicKey;)V"))||
             (!strcmp(name,"update")&&!strcmp(sig,"([B)V"))||(!strcmp(name,"verify")&&!strcmp(sig,"([B)Z"))||
             (!strcmp(name,"getAlgorithm")&&!strcmp(sig,"()Ljava/lang/String;"));
   if(!strcmp(cls,"java/security/spec/X509EncodedKeySpec"))
      return (!strcmp(name,"<init>")&&(!strcmp(sig,"([B)V")||!strcmp(sig,"([BLjava/lang/String;)V")))||
             (!strcmp(name,"getEncoded")&&!strcmp(sig,"()[B"))||
             ((!strcmp(name,"getFormat")||!strcmp(name,"getAlgorithm"))&&!strcmp(sig,"()Ljava/lang/String;"));
   if(!strcmp(cls,"java/security/KeyFactory"))
      return (!strcmp(name,"getInstance")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/security/KeyFactory;"))||
             (!strcmp(name,"generatePublic")&&!strcmp(sig,"(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;"));
   if(!strcmp(cls,"javax/crypto/Cipher"))
      return (!strcmp(name,"getInstance")&&!strcmp(sig,"(Ljava/lang/String;)Ljavax/crypto/Cipher;"))||
             (!strcmp(name,"init")&&(!strcmp(sig,"(ILjava/security/Key;)V")||!strcmp(sig,"(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V")))||
             (!strcmp(name,"doFinal")&&!strcmp(sig,"([B)[B"))||
             (!strcmp(name,"getBlockSize")&&!strcmp(sig,"()I"))||
             (!strcmp(name,"getOutputSize")&&!strcmp(sig,"(I)I"))||
             (!strcmp(name,"getAlgorithm")&&!strcmp(sig,"()Ljava/lang/String;"));
   if(!strcmp(cls,"host/security/RsaPublicKey")||!strcmp(cls,"java/security/Key")||!strcmp(cls,"java/security/PublicKey")||!strcmp(cls,"java/security/spec/EncodedKeySpec"))
      return (!strcmp(name,"getEncoded")&&!strcmp(sig,"()[B"))||
             ((!strcmp(name,"getFormat")||!strcmp(name,"getAlgorithm"))&&!strcmp(sig,"()Ljava/lang/String;"));
   if(wrapper_method_supported(cls,name,sig))return true;
   if(!strcmp(cls,"java/lang/Boolean")&&!strcmp(name,"toString")&&!strcmp(sig,"(Z)Ljava/lang/String;"))return true;
   if (!cls || !name || !sig) return false;
   if(!strcmp(cls,"java/util/Set")&&((!strcmp(name,"iterator")&&!strcmp(sig,"()Ljava/util/Iterator;"))||(!strcmp(name,"size")&&!strcmp(sig,"()I"))||(!strcmp(name,"isEmpty")&&!strcmp(sig,"()Z"))||((!strcmp(name,"add")||!strcmp(name,"contains")||!strcmp(name,"remove"))&&!strcmp(sig,"(Ljava/lang/Object;)Z"))))return true;
   if(!strcmp(cls,"android/content/Context")&&!strcmp(name,"getClassLoader")&&!strcmp(sig,"()Ljava/lang/ClassLoader;"))return true;
   if((!strcmp(cls,"java/util/Iterator")||!strcmp(cls,"java/util/ArrayList$Itr"))&&
      ((!strcmp(name,"hasNext")&&!strcmp(sig,"()Z"))||(!strcmp(name,"next")&&!strcmp(sig,"()Ljava/lang/Object;"))||(!strcmp(name,"remove")&&!strcmp(sig,"()V"))))return true;
   if(!strcmp(cls,"android/view/View")&&!strcmp(name,"getDisplay")&&!strcmp(sig,"()Landroid/view/Display;"))return true;
   if(!strcmp(cls,"android/util/DisplayMetrics")&&!strcmp(name,"<init>")&&!strcmp(sig,"()V"))return true;
   if(!strcmp(cls,"android/util/TypedValue")&&!strcmp(name,"applyDimension")&&!strcmp(sig,"(IFLandroid/util/DisplayMetrics;)F"))return true;
   if((!strcmp(cls,"android/view/View")||!strcmp(cls,"android/webkit/WebView")||!strncmp(cls,"android/widget/",15))&&!strcmp(name,"onFinishInflate")&&!strcmp(sig,"()V"))return true;
   if((!strcmp(cls,"android/view/View")||!strcmp(cls,"android/webkit/WebView")||!strncmp(cls,"android/widget/",15))&&((!strcmp(name,"setVisibility")&&!strcmp(sig,"(I)V"))||(!strcmp(name,"getVisibility")&&!strcmp(sig,"()I"))))return true;
   if((!strcmp(cls,"android/view/View")||!strncmp(cls,"android/widget/",15))&&!strcmp(name,"inflate")&&!strcmp(sig,"(Landroid/content/Context;ILandroid/view/ViewGroup;)Landroid/view/View;"))return true;
   if(!strcmp(cls,"android/view/LayoutInflater"))return (!strcmp(name,"from")&&!strcmp(sig,"(Landroid/content/Context;)Landroid/view/LayoutInflater;"))||(!strcmp(name,"inflate")&&(!strcmp(sig,"(ILandroid/view/ViewGroup;)Landroid/view/View;")||!strcmp(sig,"(ILandroid/view/ViewGroup;Z)Landroid/view/View;")));
   if((!strcmp(cls,"android/view/View")||!strncmp(cls,"android/widget/",15))&&((!strcmp(name,"<init>")&&(!strcmp(sig,"(Landroid/content/Context;)V")||!strcmp(sig,"(Landroid/content/Context;Landroid/util/AttributeSet;)V")||!strcmp(sig,"(Landroid/content/Context;Landroid/util/AttributeSet;I)V")))||(!strcmp(name,"setOnClickListener")&&!strcmp(sig,"(Landroid/view/View$OnClickListener;)V"))||(!strcmp(name,"setOnTouchListener")&&!strcmp(sig,"(Landroid/view/View$OnTouchListener;)V"))||(!strcmp(name,"setClickable")&&!strcmp(sig,"(Z)V"))||(!strcmp(name,"isClickable")&&!strcmp(sig,"()Z"))||(!strcmp(name,"findViewById")&&!strcmp(sig,"(I)Landroid/view/View;"))))return true;
   if(!strcmp(cls,"android/webkit/WebView")&&(((!strcmp(name,"setLongClickable")||!strcmp(name,"setScrollbarFadingEnabled")||!strcmp(name,"setDrawingCacheEnabled")||!strcmp(name,"clearCache"))&&!strcmp(sig,"(Z)V"))||(!strcmp(name,"setScrollBarStyle")&&!strcmp(sig,"(I)V"))||(!strcmp(name,"clearHistory")&&!strcmp(sig,"()V"))||(!strcmp(name,"loadDataWithBaseURL")&&!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"))))return true;
   if(!strcmp(cls,"android/webkit/WebView")&&((!strcmp(name,"setInitialScale")&&!strcmp(sig,"(I)V"))||(!strcmp(name,"removeJavascriptInterface")&&!strcmp(sig,"(Ljava/lang/String;)V"))||(!strcmp(name,"<init>")&&!strcmp(sig,"(Landroid/content/Context;Landroid/util/AttributeSet;I)V"))))return true;
   if (!strcmp(cls,"android/webkit/WebView")&&!strcmp(name,"destroy")&&!strcmp(sig,"()V")) return true;
   if(!strcmp(cls,"android/webkit/CookieManager"))return
       (!strcmp(name,"getInstance")&&!strcmp(sig,"()Landroid/webkit/CookieManager;")) ||
       (!strcmp(name,"setCookie")&&(!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)V")||!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;Landroid/webkit/ValueCallback;)V"))) ||
       (!strcmp(name,"getCookie")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;")) ||
       ((!strcmp(name,"removeAllCookies")||!strcmp(name,"removeSessionCookies"))&&!strcmp(sig,"(Landroid/webkit/ValueCallback;)V")) ||
       ((!strcmp(name,"removeAllCookie")||!strcmp(name,"removeSessionCookie")||!strcmp(name,"flush"))&&!strcmp(sig,"()V")) ||
       ((!strcmp(name,"hasCookies")||!strcmp(name,"acceptCookie"))&&!strcmp(sig,"()Z")) ||
       (!strcmp(name,"setAcceptCookie")&&!strcmp(sig,"(Z)V"));
   if(!strcmp(cls,"android/webkit/CookieSyncManager"))return
       (!strcmp(name,"getInstance")&&!strcmp(sig,"()Landroid/webkit/CookieSyncManager;")) ||
       (!strcmp(name,"createInstance")&&!strcmp(sig,"(Landroid/content/Context;)Landroid/webkit/CookieSyncManager;")) ||
       ((!strcmp(name,"sync")||!strcmp(name,"startSync")||!strcmp(name,"stopSync")||!strcmp(name,"resetSync"))&&!strcmp(sig,"()V"));
   if(!strcmp(cls,"android/webkit/ValueCallback"))return !strcmp(name,"onReceiveValue")&&!strcmp(sig,"(Ljava/lang/Object;)V");
   if(!strcmp(cls,"android/webkit/WebView")&&(!strcmp(name,"setHorizontalScrollBarEnabled")||!strcmp(name,"setVerticalScrollBarEnabled"))&&!strcmp(sig,"(Z)V"))return true;
   if(!strcmp(cls,"android/webkit/WebSettings")&&!strcmp(name,"getDefaultUserAgent")&&!strcmp(sig,"(Landroid/content/Context;)Ljava/lang/String;"))return true;
   if(!strcmp(cls,"android/webkit/WebSettings"))return (!strcmp(name,"setAppCacheMaxSize")&&!strcmp(sig,"(J)V"))||(!strncmp(name,"set",3)&&(!strcmp(sig,"(Z)V")||!strcmp(sig,"(I)V")||!strcmp(sig,"(Ljava/lang/String;)V")||!strcmp(sig,"(Landroid/webkit/WebSettings$ZoomDensity;)V")||!strcmp(sig,"(Landroid/webkit/WebSettings$LayoutAlgorithm;)V")||!strcmp(sig,"(Landroid/webkit/WebSettings$PluginState;)V")))||(!strncmp(name,"get",3)&&(!strcmp(sig,"()Z")||!strcmp(sig,"()I")||!strcmp(sig,"()Ljava/lang/String;")));
   if((!strcmp(cls,"android/webkit/WebViewClient")||!strcmp(cls,"android/webkit/WebChromeClient"))&&!strcmp(name,"<init>")&&!strcmp(sig,"()V"))return true;
   if(!strcmp(cls,"android/webkit/WebViewClient"))return (!strcmp(name,"onPageFinished")&&!strcmp(sig,"(Landroid/webkit/WebView;Ljava/lang/String;)V"))||(!strcmp(name,"onReceivedError")&&!strcmp(sig,"(Landroid/webkit/WebView;ILjava/lang/String;Ljava/lang/String;)V"));
   if(!strcmp(cls,"android/webkit/WebView")&&((!strcmp(name,"getSettings")&&!strcmp(sig,"()Landroid/webkit/WebSettings;"))||(!strcmp(name,"addJavascriptInterface")&&!strcmp(sig,"(Ljava/lang/Object;Ljava/lang/String;)V"))||(!strcmp(name,"setWebViewClient")&&!strcmp(sig,"(Landroid/webkit/WebViewClient;)V"))||(!strcmp(name,"setWebChromeClient")&&!strcmp(sig,"(Landroid/webkit/WebChromeClient;)V"))||(!strcmp(name,"setWebContentsDebuggingEnabled")&&!strcmp(sig,"(Z)V"))||(!strcmp(name,"<init>")&&!strcmp(sig,"(Landroid/content/Context;Landroid/util/AttributeSet;)V"))))return true;
   if (!strcmp(cls,"android/webkit/WebView"))
      return (!strcmp(name,"<init>")&&!strcmp(sig,"(Landroid/content/Context;)V"))||(!strcmp(name,"loadUrl")&&(!strcmp(sig,"(Ljava/lang/String;)V")||!strcmp(sig,"(Ljava/lang/String;Ljava/util/Map;)V")))||(!strcmp(name,"getUrl")&&!strcmp(sig,"()Ljava/lang/String;"))||(!strcmp(name,"getContext")&&!strcmp(sig,"()Landroid/content/Context;"));
   if (!strcmp(cls,"android/app/Fragment")&&
       ((!strcmp(name,"<init>")&&!strcmp(sig,"()V"))||(!strcmp(name,"setArguments")&&!strcmp(sig,"(Landroid/os/Bundle;)V"))||(!strcmp(name,"getArguments")&&!strcmp(sig,"()Landroid/os/Bundle;")))) return true;
   if (!strcmp(cls,"android/app/DialogFragment"))
      return (!strcmp(name,"<init>")&&!strcmp(sig,"()V"))||((!strcmp(name,"setShowsDialog")||!strcmp(name,"setCancelable"))&&!strcmp(sig,"(Z)V"))||((!strcmp(name,"getShowsDialog")||!strcmp(name,"isCancelable"))&&!strcmp(sig,"()Z"));
   if (!strcmp(cls,"org/json/JSONTokener"))
      return (!strcmp(name,"<init>")&&!strcmp(sig,"(Ljava/lang/String;)V"))||(!strcmp(name,"nextClean")&&!strcmp(sig,"()C"))||(!strcmp(name,"nextValue")&&!strcmp(sig,"()Ljava/lang/Object;"));
   if (!strcmp(cls,"android/view/animation/DecelerateInterpolator")||!strcmp(cls,"android/view/animation/AccelerateInterpolator"))
      return (!strcmp(name,"<init>")&&(!strcmp(sig,"()V")||!strcmp(sig,"(F)V"))) ||
             (!strcmp(name,"getInterpolation")&&!strcmp(sig,"(F)F"));
   if (!strcmp(cls,"java/net/URI"))
      return ((!strcmp(name,"getAuthority")||!strcmp(name,"getRawAuthority")||!strcmp(name,"toString")) && !strcmp(sig,"()Ljava/lang/String;")) ||
             (!strcmp(name,"<init>") && (!strcmp(sig,"(Ljava/lang/String;)V")||!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")));
   if (!strcmp(cls,"android/net/Uri"))
      return (!strcmp(name,"parse") && !strcmp(sig,"(Ljava/lang/String;)Landroid/net/Uri;")) ||
             (!strcmp(name,"getQueryParameterNames") && !strcmp(sig,"()Ljava/util/Set;")) ||
             (!strcmp(name,"buildUpon") && !strcmp(sig,"()Landroid/net/Uri$Builder;")) ||
             ((!strcmp(name,"toString")||!strcmp(name,"getPath")||!strcmp(name,"getScheme")||
               !strcmp(name,"getHost")||!strcmp(name,"getQuery")||!strcmp(name,"getFragment")||
               !strcmp(name,"getEncodedPath")||!strcmp(name,"getEncodedQuery")||!strcmp(name,"getEncodedFragment")) &&
              !strcmp(sig,"()Ljava/lang/String;"));
   if (!strcmp(cls,"android/net/Uri$Builder"))
      return (!strcmp(name,"<init>") && !strcmp(sig,"()V")) ||
             (!strcmp(name,"clearQuery") && !strcmp(sig,"()Landroid/net/Uri$Builder;")) ||
             ((!strcmp(name,"path")||!strcmp(name,"encodedPath")) && !strcmp(sig,"(Ljava/lang/String;)Landroid/net/Uri$Builder;")) ||
             (!strcmp(name,"build") && !strcmp(sig,"()Landroid/net/Uri;"));
   if (!strcmp(cls,"java/util/Locale"))
      return (!strcmp(name,"getDefault") && !strcmp(sig,"()Ljava/util/Locale;")) ||
             ((!strcmp(name,"getLanguage")||!strcmp(name,"getCountry")||!strcmp(name,"toString")||!strcmp(name,"toLanguageTag")) &&
              !strcmp(sig,"()Ljava/lang/String;"));
   if (!strcmp(cls,"java/lang/ClassLoader")&&!strcmp(name,"loadClass")&&
       !strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Class;")) return true;
   if (!strcmp(cls,"java/lang/ClassLoader")&&!strcmp(name,"findLibrary")&&
       !strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;")) return true;
   if (!strcmp(cls,"android/content/pm/PackageManager")&&!strcmp(name,"getPackageInfo")&&
       !strcmp(sig,"(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;")) return true;
   if (!strcmp(cls,"java/lang/Object") && !strcmp(name,"getClass") &&
       !strcmp(sig,"()Ljava/lang/Class;")) return true;
   if (!strcmp(cls,"java/lang/Class"))
      return (!strcmp(name,"forName") && !strcmp(sig,"(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;")) ||
             (!strcmp(name,"toString") && !strcmp(sig,"()Ljava/lang/String;")) ||
             (!strcmp(name,"getDeclaredMethods") && !strcmp(sig,"()[Ljava/lang/reflect/Method;")) ||
             ((!strcmp(name,"getConstructors")||!strcmp(name,"getDeclaredConstructors")) &&
              !strcmp(sig,"()[Ljava/lang/reflect/Constructor;")) ||
             (!strcmp(name,"getClassLoader") && !strcmp(sig,"()Ljava/lang/ClassLoader;")) ||
             /* Metadata-backed class predicates; the dispatcher reads the same
              * primitive/access facts the DEX already carries. */
             ((!strcmp(name,"isPrimitive")||!strcmp(name,"isEnum")||!strcmp(name,"isMemberClass")||
               !strcmp(name,"isAnnotation")||!strcmp(name,"isSynthetic")) &&
              !strcmp(sig,"()Z")) ||
             (!strcmp(name,"asSubclass") && !strcmp(sig,"(Ljava/lang/Class;)Ljava/lang/Class;")) ||
             (!strcmp(name,"getName") && !strcmp(sig,"()Ljava/lang/String;")) ||
             (!strcmp(name,"getSuperclass") && !strcmp(sig,"()Ljava/lang/Class;")) ||
             (!strcmp(name,"isInterface") && !strcmp(sig,"()Z")) ||
             (!strcmp(name,"isInstance") && !strcmp(sig,"(Ljava/lang/Object;)Z")) ||
             (!strcmp(name,"getComponentType") && !strcmp(sig,"()Ljava/lang/Class;")) ||
             (!strcmp(name,"getModifiers") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"cast") && !strcmp(sig,"(Ljava/lang/Object;)Ljava/lang/Object;"));
   /* Bodies are implemented by reflection_runtime_call, but JNI GetMethodID
    * must also resolve their real public instance declarations. */
   if (!strcmp(cls,"java/lang/reflect/Constructor"))
      return (!strcmp(name,"getParameterTypes") && !strcmp(sig,"()[Ljava/lang/Class;")) ||
             (!strcmp(name,"getModifiers") && !strcmp(sig,"()I"));
   if (!strcmp(cls,"java/lang/reflect/Field") && !strcmp(name,"getDeclaringClass") &&
       !strcmp(sig,"()Ljava/lang/Class;")) return true;
   /* java.lang.System 的原生库加载入口：android_basics 用宿主 dlopen（含 HAP
    * native 目录回退）真实实现，缺失时抛 UnsatisfiedLinkError。DEX 清单没有
    * System.load 的声明，引擎加载游戏自己的 so 时必须能解析到它。 */
   if (!strcmp(cls,"java/lang/System"))
      return ((!strcmp(name,"load") || !strcmp(name,"loadLibrary")) &&
              !strcmp(sig,"(Ljava/lang/String;)V")) ||
             (!strcmp(name,"identityHashCode") && !strcmp(sig,"(Ljava/lang/Object;)I")) ||
             (!strcmp(name,"nanoTime") && !strcmp(sig,"()J")) ||
             (!strcmp(name,"currentTimeMillis") && !strcmp(sig,"()J")) ||
             (!strcmp(name,"getProperties") && !strcmp(sig,"()Ljava/util/Properties;")) ||
             (!strcmp(name,"getProperty") &&
              (!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;") ||
               !strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")));
   /* java.util.Properties：System.getProperties 返回的空表的读取入口。 */
   if (!strcmp(cls,"java/util/Properties"))
      return (!strcmp(name,"getProperty") &&
              (!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;") ||
               !strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"))) ||
             (!strcmp(name,"setProperty") &&
              !strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;"));
   /* java.lang.ref.SoftReference：DEX 没有该类定义，B 站渠道悬浮球 SDK
    * 用 SoftReference 缓存 Activity；分派由 throwable_reference 提供。 */
   if (!strcmp(cls,"java/lang/ref/SoftReference"))
      return (!strcmp(name,"<init>") && !strcmp(sig,"(Ljava/lang/Object;)V")) ||
             (!strcmp(name,"get") && !strcmp(sig,"()Ljava/lang/Object;")) ||
             (!strcmp(name,"clear") && !strcmp(sig,"()V")) ||
             (!strcmp(name,"refersTo") && !strcmp(sig,"(Ljava/lang/Object;)Z"));
   /* android.view.MotionEvent / InputEvent：触摸注入的 guest 载体。Unity 的
    * nativeInjectEvent(InputEvent,int) 在 native 侧用这些 getter 读取动作/
    * 坐标/时间；快照对象与读数由 view_state_runtime 提供。 */
   if (!strcmp(cls,"android/view/MotionEvent")) {
      bool ok=(!strcmp(name,"getAction") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getActionMasked") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getActionIndex") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getPointerCount") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getPointerId") && !strcmp(sig,"(I)I")) ||
             (!strcmp(name,"findPointerIndex") && !strcmp(sig,"(I)I")) ||
             (!strcmp(name,"getEventTimeNano") && !strcmp(sig,"()J")) ||
             ((!strcmp(name,"getX")||!strcmp(name,"getY")) &&
              (!strcmp(sig,"()F")||!strcmp(sig,"(I)F"))) ||
             ((!strcmp(name,"getRawX")||!strcmp(name,"getRawY")) && !strcmp(sig,"()F")) ||
             ((!strcmp(name,"getPressure")||!strcmp(name,"getSize")) &&
              (!strcmp(sig,"()F")||!strcmp(sig,"(I)F"))) ||
             (!strcmp(name,"getEventTime") && !strcmp(sig,"()J")) ||
             (!strcmp(name,"getDownTime") && !strcmp(sig,"()J")) ||
             (!strcmp(name,"getSource") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getDeviceId") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getMetaState") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getFlags") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"obtain") && !strcmp(sig,"(JJIFFI)Landroid/view/MotionEvent;")) ||
             (!strcmp(name,"obtain") && !strcmp(sig,"(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;")) ||
             (!strcmp(name,"getToolType") && !strcmp(sig,"(I)I")) ||
             (!strcmp(name,"getHistorySize") && !strcmp(sig,"()I")) ||
             ((!strcmp(name,"getTouchMajor")||!strcmp(name,"getTouchMinor")||!strcmp(name,"getOrientation")) &&
              (!strcmp(sig,"()F")||!strcmp(sig,"(I)F"))) ||
             (!strcmp(name,"recycle") && !strcmp(sig,"()V"));
      if(!ok){static int logged;if(logged<64){logged++;FILE*mlf=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(mlf){fprintf(mlf,"MOTIONEVENT_UNSUPPORTED name=%s sig=%s\n",name,sig);fclose(mlf);}}}
      return ok;
   }
   if (!strcmp(cls,"android/view/InputEvent"))
      return (!strcmp(name,"getSource") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getDeviceId") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"getEventTime") && !strcmp(sig,"()J"));
   /* Android 内存查询：Unity 每帧 FindClass(ActivityManager$MemoryInfo) 并调用
    * ActivityManager.getMemoryInfo 填 availMem/totalMem（72qy 实测 186 次
    * ClassNotFoundException）。 */
   if (!strcmp(cls,"android/app/ActivityManager"))
      return !strcmp(name,"getMemoryInfo") &&
             !strcmp(sig,"(Landroid/app/ActivityManager$MemoryInfo;)V");
   /* Unity's AndroidJavaObject creates new MemoryInfo() before calling
    * getMemoryInfo; without <init>()V it throws NoSuchMethodError and the
    * master data load aborts (72rf: ActivityManager$MemoryInfo.<init>()V). */
   if (!strcmp(cls,"android/app/ActivityManager$MemoryInfo"))
      return !strcmp(name,"<init>") && !strcmp(sig,"()V");
   if(!strcmp(cls,"javax/net/ssl/TrustManagerFactory"))
      return (!strcmp(name,"getDefaultAlgorithm")&&!strcmp(sig,"()Ljava/lang/String;"))||
       (!strcmp(name,"getInstance")&&!strcmp(sig,"(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;"))||
       (!strcmp(name,"init")&&!strcmp(sig,"(Ljava/security/KeyStore;)V"))||
       (!strcmp(name,"getTrustManagers")&&!strcmp(sig,"()[Ljavax/net/ssl/TrustManager;"));
   if(!strcmp(cls,"javax/net/ssl/X509TrustManager")||!strcmp(cls,"javax/net/ssl/DefaultX509TrustManager"))
      return (!strcmp(name,"getAcceptedIssuers")&&!strcmp(sig,"()[Ljava/security/cert/X509Certificate;"))||
       (!strcmp(name,"checkServerTrusted")&&(!strcmp(sig,"([Ljava/security/cert/X509Certificate;Ljava/lang/String;)V")||!strcmp(sig,"([Ljava/security/cert/X509Certificate;Ljava/lang/String;Ljava/lang/String;)V")));
   if(!strcmp(cls,"java/security/cert/Certificate")||!strcmp(cls,"java/security/cert/X509Certificate"))
      return (!strcmp(name,"getEncoded")&&!strcmp(sig,"()[B"))||
       ((!strcmp(name,"getSubjectX500Principal")||!strcmp(name,"getIssuerX500Principal"))&&!strcmp(sig,"()Ljavax/security/auth/x500/X500Principal;"));
   if(!strcmp(cls,"javax/security/auth/x500/X500Principal"))
      return (!strcmp(name,"getEncoded")&&!strcmp(sig,"()[B"))||(!strcmp(name,"equals")&&!strcmp(sig,"(Ljava/lang/Object;)Z"))||(!strcmp(name,"hashCode")&&!strcmp(sig,"()I"));
   if (!strcmp(cls,"android/os/StatFs"))
      return ((!strcmp(name,"<init>") || !strcmp(name,"restat")) && !strcmp(sig,"(Ljava/lang/String;)V")) ||
             ((!strcmp(name,"getBlockSizeLong") || !strcmp(name,"getBlockCountLong") ||
               !strcmp(name,"getAvailableBlocksLong") || !strcmp(name,"getFreeBlocksLong") ||
               !strcmp(name,"getAvailableBytes") || !strcmp(name,"getFreeBytes") ||
               !strcmp(name,"getTotalBytes")) && !strcmp(sig,"()J")) ||
             ((!strcmp(name,"getBlockSize") || !strcmp(name,"getBlockCount") ||
               !strcmp(name,"getAvailableBlocks") || !strcmp(name,"getFreeBlocks")) && !strcmp(sig,"()I"));
   /* java.lang.reflect.Modifier: every public predicate is a pure bit test on
    * the int already published by Field/Method.getModifiers().  Declared as a
    * group so the engine's ReflectionHelper can classify members. */
   if (!strcmp(cls,"java/lang/reflect/Modifier") && !strcmp(sig,"(I)Z"))
      return (!strcmp(name,"isPublic")||!strcmp(name,"isPrivate")||
              !strcmp(name,"isProtected")||!strcmp(name,"isStatic")||
              !strcmp(name,"isFinal")||!strcmp(name,"isSynchronized")||
              !strcmp(name,"isVolatile")||!strcmp(name,"isTransient")||
              !strcmp(name,"isNative")||!strcmp(name,"isInterface")||
              !strcmp(name,"isAbstract")||!strcmp(name,"isStrict"));
   if (!strcmp(cls,"android/util/Log") && !strcmp(name,"getStackTraceString") &&
       !strcmp(sig,"(Ljava/lang/Throwable;)Ljava/lang/String;")) return true;
   if ((!strcmp(cls,"java/lang/Object") || !strcmp(cls,"java/lang/Throwable")) &&
       !strcmp(name,"toString") && !strcmp(sig,"()Ljava/lang/String;")) return true;
   if (!strcmp(cls,"android/app/Activity"))
      return (!strcmp(name,"getIntent") && !strcmp(sig,"()Landroid/content/Intent;")) ||
             (!strcmp(name,"getLastNonConfigurationInstance") && !strcmp(sig,"()Ljava/lang/Object;")) ||
             (!strcmp(name,"runOnUiThread") && !strcmp(sig,"(Ljava/lang/Runnable;)V")) ||
             (!strcmp(name,"findViewById") && !strcmp(sig,"(I)Landroid/view/View;"));
   /* Unity's native engine thread resolves Context.getPackageCodePath through
    * JNI at startup; the dispatcher (activity_runtime/android_basics) returns
    * the real sdkdex/classes.dex path and is only reachable once this
    * declaration exists.  Do not add methods here whose dispatcher has no
    * implementation. */
   if (!strcmp(cls,"android/content/Context"))
      return ((!strcmp(name,"getFilesDir") || !strcmp(name,"getCacheDir") || !strcmp(name,"getExternalCacheDir")) &&
              !strcmp(sig,"()Ljava/io/File;")) ||
             (!strcmp(name,"getExternalFilesDir") &&
              !strcmp(sig,"(Ljava/lang/String;)Ljava/io/File;")) ||
             (!strcmp(name,"getAssets") && !strcmp(sig,"()Landroid/content/res/AssetManager;")) ||
             (!strcmp(name,"getResources") && !strcmp(sig,"()Landroid/content/res/Resources;")) ||
             (!strcmp(name,"getPackageManager") && !strcmp(sig,"()Landroid/content/pm/PackageManager;")) ||
             (!strcmp(name,"getPackageCodePath") && !strcmp(sig,"()Ljava/lang/String;")) ||
             (!strcmp(name,"getPackageName") && !strcmp(sig,"()Ljava/lang/String;")) ||
             (!strcmp(name,"getObbDirs") && !strcmp(sig,"()[Ljava/io/File;")) ||
             (!strcmp(name,"getSystemService") && !strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Object;")) ||
             (!strcmp(name,"getSharedPreferences") &&
              !strcmp(sig,"(Ljava/lang/String;I)Landroid/content/SharedPreferences;")) ||
             /* package-metadata-next/reviewed_metadata.c implements this and
              * its selftest covers the real host-backed ApplicationInfo; the
              * native engine resolves it through JNI during startup. */
             (!strcmp(name,"getApplicationInfo") &&
              !strcmp(sig,"()Landroid/content/pm/ApplicationInfo;"));
   if (!strcmp(cls,"android/view/ContextThemeWrapper"))
      return !strcmp(name,"getResources") &&
             !strcmp(sig,"()Landroid/content/res/Resources;");
   if (!strcmp(cls,"android/content/res/Resources"))
      return !strcmp(name,"getIdentifier") &&
             !strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
   if (!strcmp(cls,"android/hardware/display/DisplayManager"))
      return !strcmp(name,"getDisplay") && !strcmp(sig,"(I)Landroid/view/Display;");
   if (!strcmp(cls,"android/view/Display"))
      return (!strcmp(name,"getAppVsyncOffsetNanos")&&!strcmp(sig,"()J"))||((!strcmp(name,"getMetrics")||!strcmp(name,"getRealMetrics"))&&!strcmp(sig,"(Landroid/util/DisplayMetrics;)V"))||(!strcmp(name,"getDisplayId")||!strcmp(name,"getRotation")||
              !strcmp(name,"getWidth")||!strcmp(name,"getHeight")) && !strcmp(sig,"()I");
   if (!strcmp(cls,"android/os/Process"))
      return !strcmp(name,"setThreadPriority") && (!strcmp(sig,"(I)V") || !strcmp(sig,"(II)V"));
   if (!strcmp(cls,"android/os/Environment"))
      return (!strcmp(name,"getExternalStorageDirectory") && !strcmp(sig,"()Ljava/io/File;")) ||
             (!strcmp(name,"getDataDirectory") && !strcmp(sig,"()Ljava/io/File;")) ||
             (!strcmp(name,"getRootDirectory") && !strcmp(sig,"()Ljava/io/File;")) ||
             (!strcmp(name,"getExternalStoragePublicDirectory") && !strcmp(sig,"(Ljava/lang/String;)Ljava/io/File;")) ||
             (!strcmp(name,"getExternalStorageState") && !strcmp(sig,"()Ljava/lang/String;")) ||
             (!strcmp(name,"isExternalStorageEmulated") &&
              (!strcmp(sig,"()Z") || !strcmp(sig,"(Ljava/io/File;)Z"))) ||
             (!strcmp(name,"isExternalStorageRemovable") &&
              (!strcmp(sig,"()Z") || !strcmp(sig,"(Ljava/io/File;)Z"))) ||
             (!strcmp(name,"isExternalStorageManager") && !strcmp(sig,"()Z"));
   if (!strcmp(cls,"android/content/res/AssetManager"))
      return !strcmp(name,"open") && !strcmp(sig,"(Ljava/lang/String;)Ljava/io/InputStream;");
   if (!strcmp(cls,"java/lang/String"))
      return (!strcmp(name,"<init>") && !strcmp(sig,"([BLjava/lang/String;)V")) ||
             (!strcmp(name,"getBytes") && !strcmp(sig,"(Ljava/nio/charset/Charset;)[B")) ||
             (!strcmp(name,"toCharArray") && !strcmp(sig,"()[C")) ||
             (!strcmp(name,"toString") && !strcmp(sig,"()Ljava/lang/String;")) ||
             (!strcmp(name,"charAt") && !strcmp(sig,"(I)C")) ||
             (!strcmp(name,"equals") && !strcmp(sig,"(Ljava/lang/Object;)Z")) ||
             (!strcmp(name,"hashCode") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"length") && !strcmp(sig,"()I"));
   if (!strcmp(cls,"android/content/DialogInterface$OnClickListener"))
      return !strcmp(name,"onClick") &&
             !strcmp(sig,"(Landroid/content/DialogInterface;I)V");
   if (!strcmp(cls,"android/content/DialogInterface$OnKeyListener"))
      return !strcmp(name,"onKey") &&
             !strcmp(sig,"(Landroid/content/DialogInterface;ILandroid/view/KeyEvent;)Z");
   if (!strcmp(cls,"android/content/DialogInterface$OnCancelListener"))
      return !strcmp(name,"onCancel") &&
             !strcmp(sig,"(Landroid/content/DialogInterface;)V");
   if (!strcmp(cls,"android/content/DialogInterface$OnDismissListener"))
      return !strcmp(name,"onDismiss") &&
             !strcmp(sig,"(Landroid/content/DialogInterface;)V");
   if (!strcmp(cls,"android/content/DialogInterface$OnShowListener"))
      return !strcmp(name,"onShow") &&
             !strcmp(sig,"(Landroid/content/DialogInterface;)V");
   if (!strcmp(cls,"android/widget/CompoundButton$OnCheckedChangeListener"))
      return !strcmp(name,"onCheckedChanged") &&
             !strcmp(sig,"(Landroid/widget/CompoundButton;Z)V");
   if (!strcmp(cls,"android/app/AlertDialog$Builder"))
      return (!strcmp(name,"<init>") && !strcmp(sig,"(Landroid/content/Context;)V")) ||
             ((!strcmp(name,"setTitle") || !strcmp(name,"setMessage")) &&
              !strcmp(sig,"(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;")) ||
             ((!strcmp(name,"setPositiveButton") || !strcmp(name,"setNegativeButton")) &&
              !strcmp(sig,"(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;")) ||
             (!strcmp(name,"setCancelable") &&
              !strcmp(sig,"(Z)Landroid/app/AlertDialog$Builder;")) ||
             (!strcmp(name,"create") && !strcmp(sig,"()Landroid/app/AlertDialog;")) ||
             (!strcmp(name,"show") && !strcmp(sig,"()Landroid/app/AlertDialog;"));
   if (!strcmp(cls,"android/app/AlertDialog") || !strcmp(cls,"android/app/Dialog"))
      return (!strcmp(name,"show") && !strcmp(sig,"()V")) ||
             (!strcmp(name,"dismiss") && !strcmp(sig,"()V")) ||
             (!strcmp(name,"isShowing") && !strcmp(sig,"()Z"));
   if (!strcmp(cls,"android/content/Intent"))
      return !strcmp(name,"getExtras") && !strcmp(sig,"()Landroid/os/Bundle;");
   if (!strcmp(cls,"android/os/Bundle"))
      return (!strcmp(name,"<init>") && (!strcmp(sig,"()V") || !strcmp(sig,"(Landroid/os/Bundle;)V"))) ||
             (!strcmp(name,"putInt") && !strcmp(sig,"(Ljava/lang/String;I)V")) ||
             (!strcmp(name,"getInt") && !strcmp(sig,"(Ljava/lang/String;)I")) ||
             (!strcmp(name,"containsKey") && !strcmp(sig,"(Ljava/lang/String;)Z")) ||
             (!strcmp(name,"getString") &&
              (!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;") ||
               !strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")));
   if (!strcmp(cls,"java/io/File"))
      return ((!strcmp(name,"getPath") || !strcmp(name,"getAbsolutePath") || !strcmp(name,"getCanonicalPath") ||
               !strcmp(name,"getParent")) &&
              !strcmp(sig,"()Ljava/lang/String;"));
   if (!strcmp(cls,"java/io/ByteArrayInputStream"))
      return (!strcmp(name,"<init>") && (!strcmp(sig,"([B)V") || !strcmp(sig,"([BII)V"))) ||
             (!strcmp(name,"read") && (!strcmp(sig,"()I") || !strcmp(sig,"([B)I") || !strcmp(sig,"([BII)I"))) ||
             (!strcmp(name,"available") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"skip") && !strcmp(sig,"(J)J")) ||
             (!strcmp(name,"mark") && !strcmp(sig,"(I)V")) ||
             (!strcmp(name,"reset") && !strcmp(sig,"()V")) ||
             (!strcmp(name,"markSupported") && !strcmp(sig,"()Z")) ||
             (!strcmp(name,"close") && !strcmp(sig,"()V"));
   if (!strcmp(cls,"java/io/InputStream"))
      return (!strcmp(name,"read") &&
              (!strcmp(sig,"()I") || !strcmp(sig,"([B)I") || !strcmp(sig,"([BII)I"))) ||
             (!strcmp(name,"available") && !strcmp(sig,"()I")) ||
             (!strcmp(name,"skip") && !strcmp(sig,"(J)J")) ||
             (!strcmp(name,"mark") && !strcmp(sig,"(I)V")) ||
             (!strcmp(name,"reset") && !strcmp(sig,"()V")) ||
             (!strcmp(name,"markSupported") && !strcmp(sig,"()Z")) ||
             (!strcmp(name,"close") && !strcmp(sig,"()V"));
   /* Legacy listener callbacks resolve as declarations. Implementations
    * belong to the guest listener or its InvocationHandler. */
   if (!strcmp(cls,"android/location/LocationListener"))
      return (!strcmp(name,"onLocationChanged") && !strcmp(sig,"(Landroid/location/Location;)V")) ||
             ((!strcmp(name,"onProviderEnabled") || !strcmp(name,"onProviderDisabled")) &&
              !strcmp(sig,"(Ljava/lang/String;)V")) ||
             (!strcmp(name,"onStatusChanged") && !strcmp(sig,"(Ljava/lang/String;ILandroid/os/Bundle;)V"));
   if (!strcmp(cls,"android/view/View$OnSystemUiVisibilityChangeListener"))
      return !strcmp(name,"onSystemUiVisibilityChange") && !strcmp(sig,"(I)V");
   if (!strcmp(cls,"android/view/View$OnLayoutChangeListener"))
      return !strcmp(name,"onLayoutChange") &&
             !strcmp(sig,"(Landroid/view/View;IIIIIIII)V");
   if (!strcmp(cls,"android/media/AudioManager$OnAudioFocusChangeListener"))
      return !strcmp(name,"onAudioFocusChange") && !strcmp(sig,"(I)V");
   if (!strcmp(cls,"android/content/SharedPreferences")){
      bool supported=(!strcmp(name,"edit") && !strcmp(sig,"()Landroid/content/SharedPreferences$Editor;")) ||
             (!strcmp(name,"getAll") && !strcmp(sig,"()Ljava/util/Map;")) ||
             (!strcmp(name,"contains") && !strcmp(sig,"(Ljava/lang/String;)Z")) ||
             (!strcmp(name,"getBoolean") && !strcmp(sig,"(Ljava/lang/String;Z)Z")) ||
             (!strcmp(name,"getInt") && !strcmp(sig,"(Ljava/lang/String;I)I")) ||
             (!strcmp(name,"getString") && !strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")) ||
             (!strcmp(name,"getStringSet") && !strcmp(sig,"(Ljava/lang/String;Ljava/util/Set;)Ljava/util/Set;"));
      static unsigned extra_log;
      if(extra_log<16){extra_log++;FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
       if(f){fprintf(f,"PLATFORM_EXTRA_SP name=%s sig=%s result=%d\n",name,sig,(int)supported);fclose(f);}}
      return supported;
   }
   if (!strcmp(cls,"android/content/SharedPreferences$Editor"))
      return (!strcmp(name,"apply") && !strcmp(sig,"()V")) ||
             (!strcmp(name,"commit") && !strcmp(sig,"()Z")) ||
             (!strcmp(name,"clear") && !strcmp(sig,"()Landroid/content/SharedPreferences$Editor;")) ||
             (!strcmp(name,"remove") && !strcmp(sig,"(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;")) ||
             (!strcmp(name,"putBoolean") && !strcmp(sig,"(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;")) ||
             (!strcmp(name,"putString") && !strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;")) ||
             (!strcmp(name,"putStringSet") && !strcmp(sig,"(Ljava/lang/String;Ljava/util/Set;)Landroid/content/SharedPreferences$Editor;"));
   if (!strcmp(cls,"java/util/Scanner"))
      return (!strcmp(name,"<init>") && !strcmp(sig,"(Ljava/io/InputStream;Ljava/lang/String;)V")) ||
             (!strcmp(name,"useDelimiter") && !strcmp(sig,"(Ljava/lang/String;)Ljava/util/Scanner;")) ||
             (!strcmp(name,"next") && !strcmp(sig,"()Ljava/lang/String;")) ||
             (!strcmp(name,"close") && !strcmp(sig,"()V"));
   if (strcmp(cls,"java/lang/StringBuilder")) return false;
   if(!strcmp(name,"reverse"))return !strcmp(sig,"()Ljava/lang/StringBuilder;");
   if(!strcmp(name,"length"))return !strcmp(sig,"()I");
   if(!strcmp(name,"charAt"))return !strcmp(sig,"(I)C");
   if(!strcmp(name,"setLength"))return !strcmp(sig,"(I)V");
   if(!strcmp(name,"substring"))return !strcmp(sig,"(I)Ljava/lang/String;")||!strcmp(sig,"(II)Ljava/lang/String;");
   if(!strcmp(name,"deleteCharAt"))return !strcmp(sig,"(I)Ljava/lang/StringBuilder;");
   if (!strcmp(name,"<init>")) return !strcmp(sig,"()V") || !strcmp(sig,"(I)V") || !strcmp(sig,"(Ljava/lang/String;)V");
   if (!strcmp(name,"toString")) return !strcmp(sig,"()Ljava/lang/String;");
   if (!strcmp(name,"append")) return !strcmp(sig,"([C)Ljava/lang/StringBuilder;") || !strcmp(sig,"([CII)Ljava/lang/StringBuilder;") || !strcmp(sig,"(Ljava/lang/String;)Ljava/lang/StringBuilder;") ||
      !strcmp(sig,"(Ljava/lang/Object;)Ljava/lang/StringBuilder;") || !strcmp(sig,"(I)Ljava/lang/StringBuilder;") ||
      !strcmp(sig,"(J)Ljava/lang/StringBuilder;") || !strcmp(sig,"(Z)Ljava/lang/StringBuilder;") ||
      !strcmp(sig,"(C)Ljava/lang/StringBuilder;");
   return false;
}

static struct dvm_method *platform_extra_method(struct dvm_class *cls,
                                                const char *name, const char *sig)
{
   if (!cls || !cls->name || !sig) return NULL;
   bool for_name = !strcmp(cls->name, "java/lang/Class") &&
       !strcmp(name, "forName") &&
       !strcmp(sig, "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
   if (!platform_extra_supported(cls->name, name, sig)) {
      if (!strcmp(name,"getDeclaringClass")||!strcmp(name,"getStackTraceString")) {
         static unsigned extra_probe;
         if (extra_probe < 8) { extra_probe++;
            FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
            if(f){fprintf(f,"DM_EXTRA supported=0 cls=%s name=%s sig=%s\n",cls->name,name,sig);fclose(f);}
         }
      }
      return NULL;
   }
   if (!strcmp(name,"getDeclaringClass")||!strcmp(name,"getStackTraceString")) {
      static unsigned extra_hit;
      if (extra_hit < 8) { extra_hit++;
         FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
         if(f){fprintf(f,"DM_EXTRA supported=1 cls=%s name=%s sig=%s\n",cls->name,name,sig);fclose(f);}
      }
   }
   const uint32_t h = mcache_hash(name, sig);
   struct dvm_method *hit = mcache_get(cls, h, name, sig);
   if (hit && hit->cls == cls) return hit;
   struct dvm_method *m = calloc(1, sizeof *m);
   if (!m) return NULL;
   m->cls = cls;
   m->name = strdup(name);
   m->sig = strdup(sig);
   if (!m->name || !m->sig) { free((void *)m->name); free(m->sig); free(m); return NULL; }
   /* android.os.Environment 的方法全部是 JNI GetStaticMethodID 解析的静态方法；
    * 其余 platform 额外方法保持实例语义，避免把真实实例 API 误标为 static。 */
   const bool is_static_method = for_name ||
       ((!strcmp(cls->name,"android/provider/Settings$Secure")||!strcmp(cls->name,"android/provider/Settings$System"))&&content_runtime_method_supported&&content_runtime_method_supported(cls->name,name,sig)) ||
       (!strcmp(cls->name,"android/util/TypedValue")&&!strcmp(name,"applyDimension")) ||
       ((!strcmp(cls->name,"android/view/View")||!strncmp(cls->name,"android/widget/",15))&&!strcmp(name,"inflate")&&!strcmp(sig,"(Landroid/content/Context;ILandroid/view/ViewGroup;)Landroid/view/View;")) ||
       (!strcmp(name,"toHexString")&&(!strcmp(cls->name,"java/lang/Integer")||!strcmp(cls->name,"java/lang/Long"))) ||
       (!strcmp(cls->name,"android/view/LayoutInflater")&&!strcmp(name,"from")) ||
       (!strcmp(cls->name,"android/webkit/CookieManager")&&!strcmp(name,"getInstance")) ||
       (!strcmp(cls->name,"android/webkit/CookieSyncManager")&&(!strcmp(name,"getInstance")||!strcmp(name,"createInstance"))) ||
       (!strcmp(cls->name,"android/webkit/WebSettings")&&!strcmp(name,"getDefaultUserAgent")) ||
       (!strcmp(cls->name,"android/webkit/WebView")&&!strcmp(name,"setWebContentsDebuggingEnabled")) ||
       (!strcmp(cls->name,"java/nio/charset/Charset")&&!strcmp(name,"forName")) ||
       (!strcmp(cls->name,"java/security/KeyFactory")&&!strcmp(name,"getInstance")) ||
       (!strcmp(cls->name,"javax/crypto/Cipher")&&!strcmp(name,"getInstance")) ||
       (!strcmp(cls->name,"java/security/Signature")&&!strcmp(name,"getInstance")) ||
       (!strcmp(cls->name,"java/lang/Boolean")&&!strcmp(name,"toString")&&!strcmp(sig,"(Z)Ljava/lang/String;")) ||
       (!strcmp(cls->name,"java/lang/Thread")&&!strcmp(name,"sleep")) ||
       (!strcmp(name,"valueOf")&&wrapper_method_supported(cls->name,name,sig)) ||
       (!strcmp(cls->name,"android/net/Uri")&&!strcmp(name,"parse")) ||
       (!strcmp(cls->name,"java/util/Locale")&&!strcmp(name,"getDefault")) ||
       (!strcmp(cls->name,"java/util/TimeZone")&&(!strcmp(name,"getDefault")||!strcmp(name,"getTimeZone"))) ||
       (!strcmp(cls->name,"javax/net/ssl/TrustManagerFactory")&&
        (!strcmp(name,"getDefaultAlgorithm")||!strcmp(name,"getInstance"))) ||
       /* android.util.Log 的公开入口（getStackTraceString 等）全部是 static；
        * 引擎用 GetStaticMethodID 解析，标成实例方法会 static mismatch。 */
       !strcmp(cls->name, "android/util/Log") ||
       !strcmp(cls->name, "java/lang/reflect/Modifier") ||
       /* java.lang.System 只有静态方法（load/loadLibrary/nanoTime/...）。 */
       !strcmp(cls->name, "java/lang/System") ||
       /* MotionEvent.obtain 是静态工厂；getter 为实例方法。 */
       (!strcmp(cls->name, "android/view/MotionEvent") && !strcmp(name, "obtain")) ||
       !strcmp(cls->name, "android/os/Environment") ||
       !strcmp(cls->name, "android/os/Process");
   m->access = DEX_ACC_PUBLIC | (is_static_method ? DEX_ACC_STATIC : 0);
   if (cls->is_interface)
      m->access |= DEX_ACC_ABSTRACT;
   method_finish(m);
   mcache_put(cls, h, name, sig, m);
   if (mcache_get(cls, h, name, sig) != m) {
      free((void *)m->name); free(m->sig); free(m); return NULL;
   }
   return m;
}

struct dvm_method *dvm_find_method(struct dvm *vm, struct dvm_class *cls,
                                   const char *name, const char *sig)
{
   if (!cls) return NULL;
   const uint32_t h = mcache_hash(name, sig);
   struct dvm_method *hit = mcache_get(cls, h, name, sig);
   if (hit) return hit;

   struct dvm_method *m = class_own_method(cls, name, sig);
   if (m) { mcache_put(cls, h, name, sig, m); return m; }
   m = platform_extra_method(cls, name, sig);
   if (m) return m;

   /* Constructors and class initialisers are never inherited.  Treating
    * <init> like an ordinary virtual method made a missing constructor on a
    * subclass resolve to Object.<init>(); reflective construction then
    * returned an object without ever running the subclass initialisation.
    * Apart from violating the VM method-resolution rules, that leaves any
    * registrations performed by the real constructor silently absent. */
   if (!strcmp(name, "<init>") || !strcmp(name, "<clinit>")) return NULL;

   for (struct dvm_class *c = cls->super; c; c = c->super) {
      m = class_own_method(c, name, sig);
      if (!m) m = platform_extra_method(c, name, sig);
      if (m) { mcache_put(cls, h, name, sig, m); return m; }
   }
   /* Default methods live on the interface. */
   struct dvm_method *declaration = NULL;
   for (struct dvm_class *c = cls; c; c = c->super) {
      for (int i = 0; i < c->nifaces; ++i) {
         if (!c->ifaces[i]) continue;
         struct dvm_method *m = dvm_find_method(vm, c->ifaces[i], name, sig);
         if (m && (m->has_code || m->builtin)) {
            mcache_put(cls, h, name, sig, m);
            return m;
         }
         if (m && !declaration) declaration = m;
      }
   }
   /* An inherited abstract declaration still resolves. Invocation decides
    * whether the receiver implements it; generated proxies dispatch it to
    * InvocationHandler. Prefer a concrete default above when one exists. */
   if (declaration) {
      mcache_put(cls, h, name, sig, declaration);
      return declaration;
   }
   return NULL;
}

struct dvm_method *dvm_lookup(struct dvm *vm, const char *class_name,
                              const char *method, const char *sig)
{
   char desc[512];
   name_to_desc(class_name, desc, sizeof desc);
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   if (!c || c->external) return NULL;
   return dvm_find_method(vm, c, method, sig);
}

static struct dvm_field *class_find_field_slow(struct dvm_class *cls, const char *name,
                                               const char *type, bool statics,
                                               struct dvm_class **owner, int *index)
{
   for (struct dvm_class *c = cls; c; c = c->super) {
      struct dvm_field *list = statics ? c->sfields : c->ifields;
      int n = statics ? c->nsfields : c->nifields;
      for (int i = 0; i < n; ++i) {
         if (!list[i].name || strcmp(list[i].name, name)) continue;
         if (type && list[i].type && strcmp(list[i].type, type)) continue;
         *owner = c; *index = i;
         return &list[i];
      }
      if (statics) {
         /* Interface constants. */
         for (int i = 0; i < c->nifaces; ++i) {
            if (!c->ifaces[i]) continue;
            struct dvm_field *f = class_find_field_slow(c->ifaces[i], name, type, true,
                                                        owner, index);
            if (f) return f;
         }
      }
   }
   return NULL;
}

/* The runtime stubs look fields up by name on every call, and a view tree walk
 * does it hundreds of times per frame.  Remember where a hit was found, keyed
 * by the caller's name pointer (a literal at nearly every call site) and
 * validated against the field list itself, so a reused pointer or a field list
 * that grew since can only cost a slow search, never a wrong answer. */
struct field_hit { struct dvm_class *cls, *owner; const char *name, *type, *field_name, *field_type; int index; bool statics; bool miss; unsigned shape, namehash; };

/* What a negative answer depends on: how many fields (and interfaces, for the
 * constants) every class in the chain declares.  A class gets its field lists
 * when it is defined, so the sum only moves if the chain does; integer reads,
 * no string compares. */
static unsigned class_chain_shape(const struct dvm_class *cls)
{
   unsigned sum = 0;
   for (const struct dvm_class *c = cls; c; c = c->super)
      sum = sum * 31u + (unsigned)c->nifields + 7u * (unsigned)c->nsfields + 13u * (unsigned)c->nifaces;
   return sum;
}

static unsigned name_hash(const char *s)
{
   unsigned h = 2166136261u;
   for (; *s; ++s) h = (h ^ (unsigned char)*s) * 16777619u;
   return h;
}

static struct dvm_field *class_find_field(struct dvm_class *cls, const char *name,
                                          const char *type, bool statics)
{
   static _Thread_local struct field_hit cache[2048];
   /* The type is part of the key: ui_int() asks for the same name as "I" and
    * then "Z", and two entries fighting over one slot is a slow search each. */
   const uintptr_t key = (((uintptr_t)cls >> 4) * 2654435761u ^ ((uintptr_t)name >> 3) ^
                          ((uintptr_t)type >> 2) * 40503u ^ (statics ? 1u : 0u)) * 2246822519u >> 7;
   struct field_hit *h = &cache[key & 2047u];
   if (h->cls == cls && h->name == name && h->statics == statics) {
      if (h->miss) {
         /* Probing for a field the class does not have (a view helper asking
          * every drawable for "shape") used to walk the whole chain with a
          * strcmp per field, every time. */
         if (h->type == type && h->shape == class_chain_shape(cls) &&
             h->namehash == name_hash(name)) return NULL;
      } else if (h->owner) {
         struct dvm_field *list = statics ? h->owner->sfields : h->owner->ifields;
         int n = statics ? h->owner->nsfields : h->owner->nifields;
         if (h->index < n) {
            struct dvm_field *f = &list[h->index];
            /* The same field entry, asked for with the same literals, as when it was
             * found: nothing to compare. */
            if (h->type == type && f->name == h->field_name && f->type == h->field_type) return f;
            if (f->name && !strcmp(f->name, name) &&
                (!type || !f->type || !strcmp(f->type, type)))
               return f;
         }
      }
   }
   struct dvm_class *owner = NULL; int index = 0;
   struct dvm_field *f = class_find_field_slow(cls, name, type, statics, &owner, &index);
   if (f) *h = (struct field_hit){ .cls = cls, .owner = owner, .name = name, .type = type,
                                   .field_name = f->name, .field_type = f->type,
                                   .index = index, .statics = statics };
   else   *h = (struct field_hit){ .cls = cls, .name = name, .type = type, .statics = statics,
                                   .miss = true, .shape = class_chain_shape(cls),
                                   .namehash = name_hash(name) };
   return f;
}

/* ------------------------------------------------------------------------ *
 * Exceptions
 * ------------------------------------------------------------------------ */

/* How many exceptions the VM has raised.  A library that probes for something
 * optional throws once; one that is stuck in a retry loop throws thousands of
 * times a second, and the two are indistinguishable from a profile alone. */
static unsigned long long g_throws;

/* And what they are.  The class alone is not enough — a NullPointerException
 * says nothing about which of the app's methods keeps hitting it — so the
 * throwing method is part of the key.  A fixed table: this is a tally for the
 * profile report, not a log. */
#define DVM_THROW_TALLY 64
static struct {
   const char *cls;
   const char *method;
   const char *in;
   unsigned long long n;
} g_throw_tally[DVM_THROW_TALLY];

static void throw_tally(struct dvm *vm, const char *class_name)
{
   const struct dvm_method *m =
      atomic_load_explicit(&vm->cur_method, memory_order_relaxed);
   const char *meth = m ? m->name : "?";
   const char *in   = (m && m->cls) ? m->cls->name : "?";
   for (int i = 0; i < DVM_THROW_TALLY; ++i) {
      if (!g_throw_tally[i].cls) {
         g_throw_tally[i].cls = class_name;
         g_throw_tally[i].method = meth;
         g_throw_tally[i].in = in;
         g_throw_tally[i].n = 1;
         return;
      }
      if (g_throw_tally[i].cls == class_name &&
          g_throw_tally[i].method == meth && g_throw_tally[i].in == in) {
         ++g_throw_tally[i].n;
         return;
      }
   }
}

/* An exception storm reports itself.
 *
 * A library that probes for something optional throws once and carries on; one
 * whose retry path this emulator has broken throws thousands of times a second
 * and looks, from outside, exactly like a slow loading screen.  The AppsFlyer
 * SDK spent an entire Cross Worlds load doing that because the VM's regex
 * engine rejected \p{C}, and nothing said so.
 *
 * So the rate is watched with no environment variable to remember: one counter
 * on the throw path, checked every 1024 throws, and a report — with the top
 * throwers — when it is high enough that something is wrong.  Reports are
 * rate-limited to one per ten seconds so the storm cannot become its own.
 * LUNARIA_DVM_THROWS=1 prints every throw instead, for when the summary has
 * named the culprit and the detail is what is wanted. */
/* Set by the storm report, consumed by the next throw. */
static bool g_throw_want_stack;

static void throw_storm_watch(void)
{
   static unsigned long long since;
   static double last_check, last_report;
   static bool started;
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   const double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
   if (!started) { started = true; last_check = last_report = now; return; }

   if (++since < 1024u) return;
   const double window = now - last_check;
   const double rate = window > 0.0 ? (double)since / window : 0.0;
   since = 0;
   last_check = now;
   if (rate < 200.0 || now - last_report < 10.0) return;
   last_report = now;

   fprintf(stderr, "[dvm] exception storm: %.0f/s — a retry loop, not "
           "ordinary error handling.  Most frequent since the last report:\n",
           rate);
   /* And who is driving it.  The tally names the method that throws; the loop
    * is whatever keeps calling it, which only the call stack shows.  Asking
    * for the next throw's stack costs one flag test on the throw path and one
    * print per report. */
   g_throw_want_stack = true;
   for (int shown = 0; shown < 3; ++shown) {
      int best = -1;
      for (int i = 0; i < DVM_THROW_TALLY; ++i)
         if (g_throw_tally[i].cls && g_throw_tally[i].n &&
             (best < 0 || g_throw_tally[i].n > g_throw_tally[best].n))
            best = i;
      if (best < 0) break;
      fprintf(stderr, "[dvm]   %8llu  %s thrown in %s.%s\n",
              g_throw_tally[best].n, g_throw_tally[best].cls,
              g_throw_tally[best].in, g_throw_tally[best].method);
      g_throw_tally[best].n = 0;
   }
   memset(g_throw_tally, 0, sizeof g_throw_tally);
}

static const char *dvm__env_throws(void)
{
   static _Atomic(const char *) slot = DVM_ENV_UNREAD;
   return dvm__env_once(&slot, "LUNARIA_DVM_THROWS");
}

static struct dvm *jni_sweep_vm;
static struct {char error[96],detail[401],caller[300];unsigned count;} jni_sweep_rows[256];
static unsigned jni_sweep_used,jni_sweep_overflow;
void jni_sweep_start(struct dvm *vm){jni_sweep_vm=vm;jni_sweep_used=jni_sweep_overflow=0;memset(jni_sweep_rows,0,sizeof jni_sweep_rows);}
void jni_sweep_record(struct dvm *vm,const char *error,const char *detail){
 if(!vm||vm!=jni_sweep_vm)return;
 const char *types[]={"java/lang/NoSuchMethodError","java/lang/NoSuchFieldError","java/lang/UnsupportedOperationException","java/lang/NoClassDefFoundError","java/lang/ClassNotFoundException","java/lang/AbstractMethodError","java/lang/IncompatibleClassChangeError","android/content/res/Resources$NotFoundException"};
 unsigned t=0;for(;t<sizeof types/sizeof types[0];t++)if(!strcmp(error,types[t]))break;if(t==sizeof types/sizeof types[0])return;
 char caller[300]="<native-no-DEX-frame>";const struct dvm_call_frame *fr=vm->invoke_frame;const struct dvm_method *m=fr?fr->method:NULL;if(m&&m->cls)snprintf(caller,sizeof caller,"%s.%s%s",m->cls->name,m->name,m->sig);
 unsigned i=0;for(;i<jni_sweep_used;i++)if(!strcmp(jni_sweep_rows[i].error,error)&&!strcmp(jni_sweep_rows[i].detail,detail)&&!strcmp(jni_sweep_rows[i].caller,caller))break;
 if(i==jni_sweep_used){if(i==256){jni_sweep_overflow++;return;}jni_sweep_used++;snprintf(jni_sweep_rows[i].error,sizeof jni_sweep_rows[i].error,"%s",error);snprintf(jni_sweep_rows[i].detail,sizeof jni_sweep_rows[i].detail,"%.400s",detail);snprintf(jni_sweep_rows[i].caller,sizeof jni_sweep_rows[i].caller,"%s",caller);}jni_sweep_rows[i].count++;
 int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-sweep-missing.txt","w");if(f){dprintf(fileno(f),"pid=%d unique=%u overflow=%u scope=application_after_HAP_selftests\n",getpid(),jni_sweep_used,jni_sweep_overflow);for(unsigned j=0;j<jni_sweep_used;j++)dprintf(fileno(f),"error=%s count=%u detail=%s top=%s\n",jni_sweep_rows[j].error,jni_sweep_rows[j].count,jni_sweep_rows[j].detail,jni_sweep_rows[j].caller);fclose(f);}errno=saved;
}
void dvm__throw(struct dvm *vm, const char *class_name, const char *fmt, ...)
{
   if(leak_scope_current)leak_scope_cancel(vm,LEAK_EXCEPTION);
   vm_arena_emergency_enter();
   ++g_throws;
   if (vm) throw_tally(vm, class_name);
   throw_storm_watch();
   char msg[512];
   if (fmt) {
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(msg, sizeof msg, fmt, ap);
      va_end(ap);
   } else {
      msg[0] = '\0';
   }

#ifdef LUNARIA_TASK7_LOGIN_PANEL
   /* Sweep only real application execution: the HAP selftests deliberately
    * throw negatives and run before jni_sweep_vm is set. Aggregate under GIL. */
   extern void jni_sweep_record(struct dvm *,const char *,const char *);
   jni_sweep_record(vm,class_name,msg);
   /* Bounded, argument-free API-failure evidence before wrappers can mask it. */
   static unsigned task6c_throw_count;
   if(vm&&task6c_throw_count<96&&(!strcmp(class_name,"java/lang/NullPointerException")||!strcmp(class_name,"java/lang/NoSuchMethodError")||!strcmp(class_name,"java/lang/OutOfMemoryError")||!strcmp(class_name,"java/lang/ClassCastException"))){
      ++task6c_throw_count;int saved_errno=errno;
      FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/vm-api-failures.txt","a");
      if(f){dprintf(fileno(f),"VM_API_FAILURE pid=%d thread=%u class=%s detail=%.400s\n",getpid(),vm->cur_thread,class_name,msg);
         const struct dvm_call_frame *frame=vm->invoke_frame;
         for(unsigned i=0;frame&&i<12;frame=frame->previous,i++){const struct dvm_method *m=frame->method;if(m&&m->cls)dprintf(fileno(f),"VM_API_CALLER depth=%u class=%s method=%s sig=%s\n",i,m->cls->name,m->name,m->sig);}
         fclose(f);}
      errno=saved_errno;
   }
#endif

   if (g_throw_want_stack && vm) {
      g_throw_want_stack = false;
      fprintf(stderr, "[dvm]   the loop, innermost first:\n");
      for (int i = vm->ncallstack - 1, shown = 0; i >= 0 && shown < 10;
           --i, ++shown) {
         const struct dvm_method *f = vm->callstack[i];
         if (!f) continue;
         fprintf(stderr, "[dvm]     #%d %s.%s%s\n", shown,
                 f->cls ? f->cls->name : "?", f->name, f->sig ? f->sig : "");
      }
   }

   /* LUNARIA_DVM_THROW_STACK=<text>: print the Java call chain of every throw
    * whose class or message contains <text> — which caller passed the null. */
   static const char *stack_filter = (const char *)-1;
   if (stack_filter == (const char *)-1) stack_filter = getenv("LUNARIA_DVM_THROW_STACK");
   if (stack_filter && *stack_filter && vm &&
       (strstr(msg, stack_filter) || strstr(class_name, stack_filter))) {
      fprintf(stderr, "[dvm] throw %s: %s — call chain, innermost first:\n", class_name, msg);
      const struct dvm_call_frame *f = vm->invoke_frame;
      for (int shown = 0; f && shown < 14; f = f->previous, ++shown) {
         const struct dvm_method *fm = f->method;
         if (fm) fprintf(stderr, "[dvm]     #%d %s.%s%s\n", shown,
                         fm->cls ? fm->cls->name : "?", fm->name, fm->sig ? fm->sig : "");
      }
   }

   if (dvm__env_throws()) {
      const struct dvm_method *m =
         vm ? atomic_load_explicit(&vm->cur_method, memory_order_relaxed) : NULL;
      fprintf(stderr, "[dvm] throw %s: %s (in %s.%s)\n", class_name, msg,
              (m && m->cls && m->cls->name) ? m->cls->name : "?",
              m ? m->name : "?");
   }

   char desc[256];
   name_to_desc(class_name, desc, sizeof desc);
   struct dvm_class *cls = dvm__class_by_desc(vm, desc);
   dvm_ref e = dvm_new_object(vm, cls);
   if (e) {
      union dvm_value v = { .l = msg[0] ? dvm_new_string(vm, msg) : 0 };
      (void)dvm_set_field(vm, e, "detailMessage", "Ljava/lang/String;", v);
      struct dvm_object *o = dvm__obj(vm, e);
      if (o && !o->utf8 && msg[0]) o->utf8 = strdup(msg);   /* always readable */
   }
   vm->exception = e;
   vm->exc_ref = e;
   vm->nexc_trace = 0;   /* a fresh throw starts a fresh unwind path */
   if (vm->cur_method) {
      vm->exc_trace[vm->nexc_trace++] = vm->cur_method;
      vm->exc_pc = vm->cur_pc;
   }
   if (vm->trace)
      fprintf(stderr, "[dvm] throw %s: %s\n", class_name, msg);
   vm_arena_emergency_leave();
}

dvm_ref dvm_exception(struct dvm *vm) { return vm->exception; }
void dvm_clear_exception(struct dvm *vm) { vm->exception = 0; vm->parked = false; }

void dvm_describe_exception(struct dvm *vm, dvm_ref exc, char *buf, size_t sz)
{
   /* Follow the cause chain.  Wrapping is how library code reports a failure —
    * `catch (Throwable t) { throw new RuntimeException(t); }` — and the
    * wrapper carries no message at all, so stopping at the outermost throwable
    * prints "java/lang/RuntimeException" and says nothing about what went
    * wrong.  The cause is the part worth reading. */
   size_t at = 0;
   for (int hop = 0; hop < 8 && exc; ++hop) {
      struct dvm_object *o = dvm__obj(vm, exc);
      if (!o) break;
      const char *cn = o->cls ? o->cls->name : "java/lang/Throwable";
      union dvm_value msg = { 0 };
      const char *text = NULL;
      if (dvm_get_field(vm, exc, "detailMessage", "Ljava/lang/String;", &msg) &&
          msg.l)
         text = dvm_string_utf8(vm, msg.l);
      if (!text) text = o->utf8;
      if (at >= sz) return;
      at += (size_t)snprintf(buf + at, sz - at, "%s%s%s%s",
                             hop ? " <- " : "", cn,
                             text ? ": " : "", text ? text : "");
      union dvm_value cause = { 0 };
      if (!dvm_get_field(vm, exc, "cause", "Ljava/lang/Throwable;", &cause) ||
          cause.l == exc)
         return;
      exc = cause.l;
   }
   if (!at && at < sz) snprintf(buf, sz, "(no exception)");
}

/* An installed UncaughtExceptionHandler receives the Throwable as an ordinary
 * Java argument: by then vm->exception has been cleared because the throw was
 * caught by Thread/Unity and deliberately forwarded to the handler.  Logging
 * only dvm_call() failures therefore misses exactly the process-ending
 * exception Crashlytics is meant to catch.  Describe it at that one semantic
 * boundary, before the handler can wrap or discard it. */
static void log_forwarded_uncaught(struct dvm *vm, dvm_ref exc,
                                   const struct dvm_method *handler)
{
   char why[1024];
   dvm_describe_exception(vm, exc, why, sizeof why);
   fprintf(stderr, "[dvm] forwarded uncaught %s to %s.%s\n", why,
           handler && handler->cls ? handler->cls->name : "?",
           handler ? handler->name : "?");

   /* Throwable.fillInStackTrace()/setStackTrace() store the actual throw-side
    * trace on the object.  Print it when present; do not synthesize the
    * handler's current stack, which would point at Crashlytics instead of the
    * failure. */
   union dvm_value trace = { 0 };
   if (!dvm_get_field(vm, exc, "stackTrace", "[Ljava/lang/StackTraceElement;",
                      &trace) || !trace.l)
      return;
   struct dvm_object *array = dvm__obj(vm, trace.l);
   if (!array || array->kind != DVM_OBJ_ARRAY || array->elem_kind != 'L' ||
       !array->data)
      return;
   const dvm_ref *items = array->data;
   uint32_t n = array->length < 32u ? array->length : 32u;
   for (uint32_t i = 0; i < n; ++i) {
      union dvm_value cls = { 0 }, method = { 0 };
      if (!items[i]) continue;
      (void)dvm_get_field(vm, items[i], "declaringClass", "Ljava/lang/String;",
                          &cls);
      (void)dvm_get_field(vm, items[i], "methodName", "Ljava/lang/String;",
                          &method);
      fprintf(stderr, "[dvm]   at %s.%s\n",
              cls.l ? dvm_string_utf8(vm, cls.l) : "?",
              method.l ? dvm_string_utf8(vm, method.l) : "?");
   }
}

/* ------------------------------------------------------------------------ *
 * Fields
 * ------------------------------------------------------------------------ */

bool dvm__get_field_from(struct dvm *vm, dvm_ref obj, struct dvm_class *owner,
                         const char *name, const char *type,
                         union dvm_value *out)
{
   struct dvm_object *o = dvm__obj(vm, obj);
   if (!o || !o->cls) return false;
   struct dvm_field *f = class_find_field(owner ? owner : o->cls, name, type, false);
   if (!f || !o->slots || f->slot >= o->cls->islots) return false;
   *out = o->slots[f->slot];
   if(type && (type[0]=='L'||type[0]=='[') && out->l)dvm__obj(vm,out->l);
   return true;
}

bool dvm__set_field_from(struct dvm *vm, dvm_ref obj, struct dvm_class *owner,
                         const char *name, const char *type,
                         union dvm_value val)
{
   if(leak_scope_current&&(type&&(type[0]=='L'||type[0]=='['))&&leak_scope_mentions(vm,val.l,NULL,0))leak_scope_cancel(vm,LEAK_STORE);
   struct dvm_object *o = dvm__obj(vm, obj);
   if (!o || !o->cls) return false;
   struct dvm_field *f = class_find_field(owner ? owner : o->cls, name, type, false);
   if (!f || !o->slots || f->slot >= o->cls->islots) return false;
   {
      char *rb = NULL; size_t rl = 0; bool rd = false;
      if (name_range_hit(vm, &o->slots[f->slot], sizeof(union dvm_value), &rb, &rl, &rd))
         name_guard_report("set_field", vm, o->cls, f->name, f->slot, val,
                           &o->slots[f->slot], rb, rl, rd);
   }
   if (f->kind == 'L') GC_STORE_REF(vm,&o->slots[f->slot].l,val.l,"runtime_set_field");
   else o->slots[f->slot] = val;
   return true;
}

bool dvm_get_field(struct dvm *vm, dvm_ref obj, const char *name,
                   const char *type, union dvm_value *out)
{
   return dvm__get_field_from(vm, obj, NULL, name, type, out);
}

bool dvm_set_field(struct dvm *vm, dvm_ref obj, const char *name,
                   const char *type, union dvm_value val)
{
   return dvm__set_field_from(vm, obj, NULL, name, type, val);
}

bool dvm_get_static(struct dvm *vm, struct dvm_class *cls, const char *name,
                    const char *type, union dvm_value *out)
{
   struct dvm_field *f = class_find_field(cls, name, type, true);
   if (!f || !f->cls->sslots || f->slot >= f->cls->nsslots) return false;
   if (!dvm_init_class(vm, f->cls)) return false;
   *out = f->cls->sslots[f->slot];
   if(type && (type[0]=='L'||type[0]=='[') && out->l)dvm__obj(vm,out->l);
   return true;
}

bool dvm_set_static(struct dvm *vm, struct dvm_class *cls, const char *name,
                    const char *type, union dvm_value val)
{
   if(leak_scope_current&&(type&&(type[0]=='L'||type[0]=='['))&&leak_scope_mentions(vm,val.l,NULL,0))leak_scope_cancel(vm,LEAK_STORE);
   struct dvm_field *f = class_find_field(cls, name, type, true);
   if (!f || !f->cls->sslots || f->slot >= f->cls->nsslots) return false;
   {
      char *rb = NULL; size_t rl = 0; bool rd = false;
      if (name_range_hit(vm, &f->cls->sslots[f->slot], sizeof(union dvm_value), &rb, &rl, &rd))
         name_guard_report("set_static", vm, f->cls, f->name, f->slot, val,
                           &f->cls->sslots[f->slot], rb, rl, rd);
   }
   if (f->kind == 'L') GC_STORE_REF(vm,&f->cls->sslots[f->slot].l,val.l,"runtime_set_static");
   else f->cls->sslots[f->slot] = val;
   return true;
}

/* ------------------------------------------------------------------------ *
 * Class initialisation
 * ------------------------------------------------------------------------ */

static bool invoke(struct dvm *vm, struct dvm_method *m, dvm_ref self,
                   const uint32_t *slots, int nslots, union dvm_value *out);

bool dvm_init_class(struct dvm *vm, struct dvm_class *cls)
{
   if (!cls) return true;
   if (cls->init_state == 2) return true;
   /* Only the initialising Java thread may recursively use partial state.
    * Other threads wait without GIL; the owner must be able to finish. */
   uint64_t me = 0;
   if (cls->init_state != 3) {
      me = dvm__current_thread_id(vm);
      if (!me) return false;
      while (cls->init_state == 1 && cls->init_owner != me) {
         if (dvm_is_stopping(vm)) return false;
         dvm_gil_wait_for_ns(vm, (uintptr_t)&cls->init_owner, UINT64_MAX);
      }
   }
   /* A class whose <clinit> threw stays erroneous: every later use has to fail
    * too, because its static fields were never assigned.  Reporting success
    * once the first attempt is over hands out nulls that look like data. */
   if (cls->init_state == 3) {
      dvm__throw(vm, "java/lang/NoClassDefFoundError",
                 "%s failed to initialise", cls->name);
      return false;
   }
   if (cls->init_state) return true;
   cls->init_state = 1;
   cls->init_owner = me;

   if (cls->super && !dvm_init_class(vm, cls->super)) goto failed;

   /* Constant static fields come from the encoded array, in declaration
    * order, before <clinit> runs. */
   if (cls->static_values_off && cls->dex && cls->nsslots) {
      struct dex_value *vals = calloc((size_t)cls->nsslots, sizeof *vals);
      if (vals) {
         int n = dex_static_values(cls->dex, cls->static_values_off, vals, cls->nsslots);
         if (n > cls->nsslots) n = cls->nsslots;
         for (int i = 0; i < n; ++i) {
            union dvm_value v = { 0 };
            switch (vals[i].type) {
               case DEX_VALUE_STRING: {
                  const char *s = dex_string(cls->dex, (uint32_t)vals[i].bits);
                  v.l = s ? dvm__intern(vm, s) : 0;
                  break;
               }
               case DEX_VALUE_TYPE: {
                  const char *t = dex_type(cls->dex, (uint32_t)vals[i].bits);
                  struct dvm_class *tc = t ? dvm__class_by_desc(vm, t) : NULL;
                  v.l = tc ? dvm_new_object(vm, dvm__class_by_desc(vm, "Ljava/lang/Class;")) : 0;
                  if (v.l) {
                     struct dvm_object *co = dvm__obj(vm, v.l);
                     co->kind = DVM_OBJ_CLASS;
                     co->klass = tc;
                  }
                  break;
               }
               case DEX_VALUE_NULL:
                  break;
               default:
                  v.ju = vals[i].bits;
                  break;
            }
            if (vals[i].type == DEX_VALUE_STRING || vals[i].type == DEX_VALUE_TYPE || vals[i].type == DEX_VALUE_NULL)
               GC_STORE_REF(vm,&cls->sslots[i].l,v.l,"encoded_static_reference");
            else cls->sslots[i] = v;
         }
         free(vals);
      }
   }

   struct dvm_method *clinit = class_own_method(cls, "<clinit>", "()V");
   if (clinit && (clinit->has_code || clinit->builtin)) {
      if (vm->trace) fprintf(stderr, "[dvm] <clinit> %s\n", cls->name);
      union dvm_value ret;
      if (!invoke(vm, clinit, 0, NULL, 0, &ret)) {
         char why[512] = "";
         if (vm->exception)
            dvm_describe_exception(vm, vm->exception, why, sizeof why);
         fprintf(stderr, "[dvm] <clinit> %s failed: %s\n", cls->name,
                 why[0] ? why : "no exception");
         {static unsigned logged;if(logged++<32){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){dprintf(fileno(f),"SDK_NOTIFY_CLINIT_FAILURE class=%s detail=%s\n",cls->name,why[0]?why:"no exception");fclose(f);}errno=saved;}}
         goto failed;
      }
   }
   cls->init_state = 2;
   cls->init_owner = 0;
   dvm_gil_notify_for((uintptr_t)&cls->init_owner);
   return true;
failed:
   cls->init_state = 3;
   cls->init_owner = 0;
   dvm_gil_notify_for((uintptr_t)&cls->init_owner);
   return false;
}

/* ------------------------------------------------------------------------ *
 * Argument marshalling
 * ------------------------------------------------------------------------ */

/* Register slots (Dalvik layout) into one union per declared parameter. */
static int slots_to_values(const char *sig, const uint32_t *slots, int nslots,
                           union dvm_value *out, int max)
{
   int n = 0, s = 0;
   if (!sig || *sig != '(') return 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      const char *q = desc_skip(p);
      char k = dvm__kind_of(p);
      if (n >= max) break;
      if (k == 'J' || k == 'D') {
         uint64_t lo = (s < nslots) ? slots[s] : 0;
         uint64_t hi = (s + 1 < nslots) ? slots[s + 1] : 0;
         out[n].ju = lo | (hi << 32);
         s += 2;
      } else {
         out[n].u = (s < nslots) ? slots[s] : 0;
         ++s;
      }
      ++n;
      p = q;
   }
   return n;
}

static int values_to_slots(const char *sig, const union dvm_value *vals, int nvals,
                           uint32_t *out, int max)
{
   int s = 0, n = 0;
   if (!sig || *sig != '(') return 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      const char *q = desc_skip(p);
      char k = dvm__kind_of(p);
      union dvm_value v = (n < nvals) ? vals[n] : (union dvm_value){ 0 };
      if (k == 'J' || k == 'D') {
         if (s >= max - 1) break;
         out[s++] = (uint32_t)v.ju;
         out[s++] = (uint32_t)(v.ju >> 32);
      } else {
         if (s >= max) break;
         out[s++] = v.u;
      }
      ++n;
      p = q;
   }
   return s;
}

/* ------------------------------------------------------------------------ *
 * The interpreter
 * ------------------------------------------------------------------------ */

#define A_HI(u) ((uint8_t)((u) >> 12))
#define OPCODE(u) ((uint8_t)((u) & 0xff))
#define AA(u) ((uint8_t)((u) >> 8))
#define A4(u) ((uint8_t)(((u) >> 8) & 0x0f))
#define B4(u) ((uint8_t)(((u) >> 12) & 0x0f))

struct frame {
   struct dvm_method *m;
   uint32_t *regs;
   uint32_t nregs;
   uint32_t pc;
   uint64_t result;   /* the pseudo-register move-result reads */
};

static bool gc_exec_ref(struct dvm *vm,dvm_ref *slot,bool typed,dvm_gc_visit_fn visit,void *user,struct gc_exec_counts *count){
   if(!*slot)return true;
   if(*slot>vm->heap_size){if(typed)gc_root_failure("execution_frames","typed_ref_above_heap");return !typed;}
   struct dvm_object *o=heap_slot(vm,*slot);
   if(!o->live||!(o=dvm_gc_resolve(vm,o))||!o->live){if(typed)gc_root_failure("execution_frames","typed_ref_not_live");return !typed;}
   /* A conservative register may be an integer equal to a live stable ID.
    * Expose only a copy: collectors must never rewrite that primitive. */
   if(typed)visit(slot,user);
   else {
      dvm_ref candidate=*slot;visit(&candidate,user);
      if(candidate!=*slot){fprintf(stderr,"GC_ROOT_FAILURE provider=execution_frames reason=conservative_id_rewrite original=%u replacement=%u\n",*slot,candidate);return false;}
   }
   if(count)++count->refs;return true;
}
static bool gc_execution_roots(struct dvm *vm,dvm_gc_visit_fn visit,void *user,struct gc_exec_counts *count){
   struct gc_exec_counts ignored={0};if(!count)count=&ignored;
   bool ok=!vm->gc_execution_incomplete;size_t guard=0;
   if(!ok)gc_root_failure("execution_frames","latched_incomplete");
   for(struct gc_exec_entry *r=gc_exec_entries;r;r=r->next){
      if(++guard>1000000){ok=false;break;}if(r->vm!=vm)continue;++count->entries;
      if(r->self)++count->typed_slots;
      if(!gc_exec_ref(vm,&r->self,true,visit,user,count))ok=false;
      const char *p=r->sig;int at=0;
      if(!p||*p++!='('||r->nslots<0||(!r->slots&&r->nslots)){ok=false;continue;}
      while(*p&&*p!=')'){
         char kind=*p;bool ref=kind=='L'||kind=='[';
         if(kind=='['){while(*p=='[')++p;if(*p=='L'){while(*p&&*p!=';')++p;if(*p)++p;}else if(*p&&strchr("ZBCSIJFD",*p))++p;else{ok=false;break;}}
         else if(kind=='L'){while(*p&&*p!=';')++p;if(*p)++p;}
         else if(strchr("ZBCSIJFD",kind))++p;else{ok=false;break;}
         int width=kind=='J'||kind=='D'?2:1;if(at>r->nslots-width){ok=false;break;}
         if(ref){++count->typed_slots;dvm_ref value=r->slots[at];if(!gc_exec_ref(vm,&value,true,visit,user,count))ok=false;}
         at+=width;
      }
      if(*p!=')'||at!=r->nslots){char detail[384];snprintf(detail,sizeof detail,"signature_slots sig=%s expected=%d actual=%d",r->sig?r->sig:"null",at,r->nslots);gc_root_failure("execution_entries",detail);ok=false;}
   }
   guard=0;
   for(struct gc_exec_frame *r=gc_exec_frames;r;r=r->next){
      if(++guard>1000000){ok=false;break;}if(r->vm!=vm)continue;++count->frames;
      struct frame *f=r->frame;if(!f||!f->m||(!f->regs&&f->nregs)||f->nregs>65535){ok=false;continue;}
      for(uint32_t i=0;i<f->nregs;++i){++count->registers;if(!gc_exec_ref(vm,&f->regs[i],false,visit,user,count))ok=false;}
      if(f->result<=UINT32_MAX){dvm_ref value=(dvm_ref)f->result;size_t before=count->refs;if(!gc_exec_ref(vm,&value,false,visit,user,count))ok=false;if(before!=count->refs)++count->results;}
   }
   if(pthread_mutex_trylock(&gc_tstate_lock)){gc_root_failure("suspended_tstate","lock_busy");return false;}
   guard=0;
   for(struct gc_tstate_hold *r=gc_tstate_holds;r;r=r->next){
      if(++guard>1000000){ok=false;break;}if(r->vm!=vm)continue;
      if(r->serial!=vm->serial){ok=false;continue;}++count->tstates;
      for(unsigned i=0;i<3;++i)if(!gc_exec_ref(vm,&r->refs[i],true,visit,user,count))ok=false;
   }
   pthread_mutex_unlock(&gc_tstate_lock);return ok;
}
static void gc_exec_entry_leave(struct gc_exec_entry *r){
   struct gc_exec_entry **p=&gc_exec_entries;while(*p&&*p!=r)p=&(*p)->next;if(*p)*p=r->next;
}
static void gc_exec_frame_leave(struct gc_exec_frame *r){
   struct gc_exec_frame **p=&gc_exec_frames;while(*p&&*p!=r)p=&(*p)->next;if(*p)*p=r->next;
}
static void gc_exec_count_visit(dvm_ref *slot,void *user){(void)slot;(void)user;}
bool dvm_gc_execution_root_counts(struct dvm *vm,uint64_t *typed_entries,uint64_t *typed_slots,uint64_t *conservative_frames,uint64_t *conservative_slots,uint64_t *tstates,uint64_t *result_candidates){
   if(!vm||!dvm_gil_held(vm))return false;
   struct gc_exec_counts count={0};bool ok=gc_execution_roots(vm,gc_exec_count_visit,NULL,&count);
   if(typed_entries)*typed_entries=count.entries;if(typed_slots)*typed_slots=count.typed_slots;
   if(conservative_frames)*conservative_frames=count.frames;if(conservative_slots)*conservative_slots=count.registers;
   if(tstates)*tstates=count.tstates;if(result_candidates)*result_candidates=count.results;
   return ok;
}
static _Thread_local void (*gc_exec_test_probe)(struct dvm *);

static void gil_observe_poll(struct dvm *);
static bool gil_short_enabled(void);
struct gil_site_scope {int previous;};
static struct gil_site_scope gil_site_enter(const char *);
static void gil_site_leave(struct gil_site_scope *);
static void gil_site_resume(void);
static void gil_site_pause(void);
struct gil_deep_scope {struct gil_deep_scope *previous;struct dvm *vm;struct dvm_method *method;uint32_t pc,last_pc;uint64_t samples,repeated;bool tracked;};
static void gil_deep_enter(struct gil_deep_scope *,struct dvm *,struct dvm_method *);
static void gil_deep_leave(struct gil_deep_scope *);
static void gil_deep_tick(uint32_t);
static void gil_deep_pause(void);
static void gil_deep_resume(void);
static void gil_deep_hold_end(struct dvm *,uint64_t);
static void gil_deep_write(void);
struct emu_profile_scope {int index,fast;uint64_t start;};
static struct emu_profile_scope emu_profile_enter(struct dvm_method *);
static void emu_profile_leave(struct emu_profile_scope *);
static void emu_profile_write(void);
#include "gc_fast.inc"

static inline uint64_t rw(const uint32_t *r, int i)
{
   return (uint64_t)r[i] | ((uint64_t)r[i + 1] << 32);
}

static inline void sw(uint32_t *r, int i, uint64_t v)
{
   r[i] = (uint32_t)v;
   r[i + 1] = (uint32_t)(v >> 32);
}

static inline float bits_to_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f_to_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double bits_to_d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t d_to_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static bool class_assignable(struct dvm *vm, struct dvm_class *from,
                             struct dvm_class *to);

static bool iface_assignable(struct dvm *vm, struct dvm_class *from,
                             struct dvm_class *to)
{
   for (struct dvm_class *c = from; c; c = c->super)
      for (int i = 0; i < c->nifaces; ++i)
         /* A class that lists itself teaches this walk nothing and never ends:
          * the recursion comes straight back here and the process dies of a
          * stack overflow rather than of a wrong answer.  Java has no such
          * declaration, but a builtin table can be written with one. */
         if (c->ifaces[i] && c->ifaces[i] != c && c->ifaces[i] != from &&
             class_assignable(vm, c->ifaces[i], to))
            return true;
   return false;
}

bool dvm__class_assignable(struct dvm *vm, struct dvm_class *from,
                           struct dvm_class *to)
{
   return class_assignable(vm, from, to);
}

static bool class_assignable(struct dvm *vm, struct dvm_class *from,
                             struct dvm_class *to)
{
   if (!from || !to) return true;   /* unknown: do not block the app */
   if (from == to || !strcmp(from->desc, to->desc)) return true;
   /* Primitive values have no reference widening conversion.  This check
    * must precede Object: array covariance recurses on the component type,
    * and treating byte as an Object made byte[] pass `instanceof Object[]`.
    * Primitive arrays are Objects, Cloneable and Serializable, but are never
    * arrays of references; only identical primitive component types match. */
   if (from->is_primitive || to->is_primitive) return false;
   if (!strcmp(to->desc, "Ljava/lang/Object;")) return true;
   for (struct dvm_class *c = from; c; c = c->super)
      if (c == to || !strcmp(c->desc, to->desc)) return true;
   if (iface_assignable(vm, from, to)) return true;
   if (from->is_array && to->is_array)
      return class_assignable(vm, from->elem, to->elem);
   /* Unknown hierarchy is not evidence of assignability.  Returning true for
    * every external class makes `x instanceof InternalSentinel` succeed for
    * arbitrary objects; lock-free queues then mistake real tasks for their
    * private marker objects and spin forever.  Exact classes and every known
    * superclass/interface were handled above, so the only sound answer left
    * is false. */
   return false;
}

/* Resolves the target of an invoke-virtual / -interface at the receiver's
 * actual class. */
static struct dvm_method *virtual_target(struct dvm *vm, dvm_ref self,
                                         struct dvm_method *decl)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o || !o->cls || o->cls == decl->cls) return decl;
   /* Inline cache on the declared method: a call site sees the same few
    * receiver classes over and over, and the lookup below hashes the name and
    * the signature on every call.  Method tables are fixed once a class is
    * built, so a (receiver class, declared method) answer never goes stale. */
   struct dvm_class *rc = o->cls;
   if (decl->vt_cls[0] == rc) return decl->vt_m[0];
   if (decl->vt_cls[1] == rc) return decl->vt_m[1];
   struct dvm_method *m = dvm_find_method(vm, rc, decl->name, decl->sig);
   if (!m) m = decl;
   decl->vt_cls[1] = decl->vt_cls[0]; decl->vt_m[1] = decl->vt_m[0];
   decl->vt_cls[0] = rc; decl->vt_m[0] = m;
   return m;
}

static bool gc_call_out_impl(struct dvm *,struct dvm_class *,const char *,const char *,bool,bool,dvm_ref,const uint32_t *,int,union dvm_value *);
bool dvm__call_out(struct dvm *vm, struct dvm_class *cls, const char *name,
                   const char *sig, bool is_static, bool is_native,
                   dvm_ref self, const uint32_t *slots, int nslots,
                   union dvm_value *out)
{
   struct dvm_gc_native_scope scope;
   if(!dvm_gc_native_enter(vm,&scope,self,sig,slots,nslots)){
      dvm_gc_native_leave(&scope);return false;
   }
   bool ok=gc_call_out_impl(vm,cls,name,sig,is_static,is_native,self,slots,nslots,out);
   dvm_gc_native_leave_result(&scope,sig,ok&&out?out->l:0);return ok;
}
static bool gc_call_out_impl(struct dvm *vm, struct dvm_class *cls, const char *name,
                   const char *sig, bool is_static, bool is_native,
                   dvm_ref self, const uint32_t *slots, int nslots,
                   union dvm_value *out)
{
   union dvm_value args[64];
   int nargs = slots_to_values(sig, slots, nslots, args, 64);
   memset(out, 0, sizeof *out);

   /* A class name carrying control or non-ASCII bytes is not a DEX name and
    * not a name any runtime installs: it means the class record or its name
    * storage was overwritten.  Print the raw bytes and both pointers once so
    * the corrupted object can be matched against the heap and the caller,
    * instead of observing only the MISSING_API line it produces (72bv saw
    * "�\x0c.getMessage()" from IdReader.doRead and Bugly's crash handler). */
   if (cls && cls->name) {
      const unsigned char *n = (const unsigned char *)cls->name;
      bool odd = false;
      for (size_t i = 0; n[i]; ++i)
         if (n[i] < 0x20 || n[i] >= 0x7f) { odd = true; break; }
      if (odd) {
         char nh[3 * 48 + 1], dh[3 * 48 + 1]; size_t na = 0, da = 0;
         for (size_t i = 0; i < 48 && n[i]; ++i)
            na += (size_t)snprintf(nh + na, sizeof nh - na, "%02x", n[i]);
         nh[na] = '\0';
         if (cls->desc)
            for (size_t i = 0; i < 48 && cls->desc[i]; ++i)
               da += (size_t)snprintf(dh + da, sizeof dh - da, "%02x",
                                      (unsigned char)cls->desc[i]);
         dh[da] = '\0';
         fprintf(stderr, "CALLOUT_ODD vm=%p owned=%d cls=%p desc_ptr=%p name_hex=%s desc_hex=%s caller=%s.%s\n",
                 (void *)vm, class_owned_by_vm(vm, cls) ? 1 : 0, (void *)cls,
                 (void *)cls->desc, nh, dh,
                 vm->cur_method && vm->cur_method->cls ? vm->cur_method->cls->name : "?",
                 vm->cur_method ? vm->cur_method->name : "?");
      }
   }

   if (dvm__env_outtrace())
      fprintf(stderr, "[out] %s.%s%s%s\n", cls->name, name, sig,
              is_native ? " (native)" : "");

   /* Interface / abstract methods on a Proxy instance never have bytecode;
    * they must reach the InvocationHandler rather than the unresolved stub. */
   if (!is_static && self &&
       dvm_proxy_try_invoke(vm, self, cls, name, sig, args, nargs, out))
      return !vm->exception;

   if (is_native && vm->hooks.call_native) {
      if(leak_scope_current){
         extern bool jni_bridge_call_native(void *,struct dvm *,const char *,const char *,const char *,bool,dvm_ref,const union dvm_value *,int,union dvm_value *) __attribute__((weak));
         extern dvm_ref jni_bridge_take_pending_exception(void *,struct dvm *) __attribute__((weak));
         if(vm->hooks.call_native!=jni_bridge_call_native||vm->hooks.take_pending_exception!=jni_bridge_take_pending_exception)leak_scope_cancel(vm,LEAK_UNKNOWN_CALL);
      }
      /* The bridge converts VM objects before and after guest execution.
       * Those conversions require GIL just like bytecode. The bridge drops
       * it only around actual native execution, preserving GIL -> AEL order. */
      bool ok = vm->hooks.call_native(vm->hooks.user, vm, cls->name, name, sig,
                                      is_static, self, args, nargs, out);
      if (ok) {
         if (vm->hooks.take_pending_exception) {
            dvm_ref raised = vm->hooks.take_pending_exception(vm->hooks.user, vm);
            if (raised) {
               memset(out, 0, sizeof *out);
               vm->exception = raised;
               return false;
            }
         }
         return true;
      }
   }

   if(leak_scope_current&&leak_scope_mentions(vm,self,slots,nslots)){
      /* The only external read allowed to observe our private Long is the
       * real immutable primitive accessor. Do not delegate that capability
       * to an unknown external hook, even if its name looks familiar. */
      if(!is_native&&leak_scope_box(vm,self)&&!strcmp(cls->name,"java/lang/Long")&&
         !strcmp(name,"longValue")&&!strcmp(sig,"()J")&&nslots==0&&
         java_basics_call(vm,cls->name,name,sig,self,args,nargs,out))return !vm->exception;
      leak_scope_cancel(vm,LEAK_UNKNOWN_CALL);
   }
   if(leak_scope_current&&!leak_scope_external_allowed(vm,cls->name,name,sig,self,slots,nslots))leak_scope_cancel(vm,LEAK_UNKNOWN_CALL);
   if (vm->hooks.call_external &&
       vm->hooks.call_external(vm->hooks.user, vm, cls->name, name, sig,
                               self, args, nargs, out))
      return true;
   /* A dispatched guest call may fail with its original pending exception. */
   if (dvm_exception(vm)) return false;

   /* Nothing implements this and the guest is calling it *now*: the zero
    * below is about to be used as an answer.  Report the caller with it —
    * which app method acted on the missing result is what separates a
    * harmless optional-plugin probe from the cause of the next failure. */
   char what[512];
   struct dvm_method *from = vm->cur_method;
   /* The declared class is what the dex names; the receiver's own class is
    * what has to gain the method, and the two differ for every interface. */
   struct dvm_class *receiver = self ? dvm_object_class(vm, self) : NULL;
   snprintf(what, sizeof what,
            "unsupported method, strict runner throws: %s.%s%s (receiver %s, called from %s.%s)",
            cls->name, name, sig,
            receiver && receiver->name ? receiver->name : "-",
            from && from->cls && from->cls->name ? from->cls->name : "?",
            from && from->name ? from->name : "?");
   note_missing(vm, what);
   dvm__throw(vm, "java/lang/NoSuchMethodError", "%s.%s%s", cls->name, name, sig);
   return false;
}

/* --- the loop ----------------------------------------------------------- */

/* Finds the catch handler for `pc`, or -1.  Sets *out_pc to the handler
 * address on success. */
static int find_handler(struct dvm *vm, struct dvm_method *m, uint32_t pc,
                        struct dvm_class *exc_cls, uint32_t *out_pc)
{
   const struct dex_code *code = &m->code;
   if (!code->tries_size || !code->tries || !m->dex) return -1;

   for (uint32_t i = 0; i < code->tries_size; ++i) {
      const uint8_t *t = code->tries + (size_t)i * 8u;
      uint32_t start, count16;
      memcpy(&start, t, 4);
      uint16_t cnt, hoff;
      memcpy(&cnt, t + 4, 2);
      memcpy(&hoff, t + 6, 2);
      count16 = cnt;
      if (pc < start || pc >= start + count16) continue;

      size_t p = (size_t)(code->handlers - m->dex->p) + hoff;
      int32_t size;
      p = dex_sleb(m->dex, p, &size);
      int npairs = size < 0 ? -size : size;
      for (int k = 0; k < npairs; ++k) {
         uint32_t type_idx, addr;
         p = dex_uleb(m->dex, p, &type_idx);
         p = dex_uleb(m->dex, p, &addr);
         const char *td = dex_type(m->dex, type_idx);
         struct dvm_class *tc = td ? dvm__class_by_desc(vm, td) : NULL;
         if (tc && class_assignable(vm, exc_cls, tc)) {
            *out_pc = addr;
            return 0;
         }
      }
      if (size <= 0) {
         uint32_t all;
         p = dex_uleb(m->dex, p, &all);
         *out_pc = all;
         return 0;
      }
   }
   return -1;
}

/* A cooperative park is an interpreter unwind token, not a Java
 * InterruptedException.  It must bypass typed catch clauses, but it still has
 * to execute encoded catch-all handlers: javac/d8 use those for finally, and
 * skipping them leaves executor locks/active-task slots permanently held. */
static int find_catchall_handler(struct dvm_method *m, uint32_t pc,
                                 uint32_t *out_pc)
{
   const struct dex_code *code = &m->code;
   if (!code->tries_size || !code->tries || !m->dex) return -1;
   for (uint32_t i = 0; i < code->tries_size; ++i) {
      const uint8_t *t = code->tries + (size_t)i * 8u;
      uint32_t start;
      uint16_t count, hoff;
      memcpy(&start, t, 4);
      memcpy(&count, t + 4, 2);
      memcpy(&hoff, t + 6, 2);
      if (pc < start || pc >= start + count) continue;
      size_t p = (size_t)(code->handlers - m->dex->p) + hoff;
      int32_t size;
      p = dex_sleb(m->dex, p, &size);
      int npairs = size < 0 ? -size : size;
      for (int k = 0; k < npairs; ++k) {
         uint32_t ignored;
         p = dex_uleb(m->dex, p, &ignored);
         p = dex_uleb(m->dex, p, &ignored);
      }
      if (size <= 0) {
         p = dex_uleb(m->dex, p, out_pc);
         (void)p;
         return 0;
      }
   }
   return -1;
}

/* Reads the 35c/3rc argument registers into `slots`. */
static int gather_args(const uint16_t *insns, uint32_t nins, uint32_t pc, bool range,
                       uint32_t *regs, uint32_t nregs, uint32_t *slots)
{
   uint16_t u0 = insns[pc];
   uint16_t cccc = (pc + 2 < nins) ? insns[pc + 2] : 0;
   if (range) {
      int n = AA(u0);
      uint16_t first = cccc;
      for (int i = 0; i < n; ++i)
         slots[i] = (uint32_t)(first + i) < nregs ? regs[first + i] : 0;
      return n;
   }
   int n = B4(u0);
   uint16_t g = A4(u0);
   uint16_t fedc = cccc;
   uint16_t idx[5] = {
      (uint16_t)(fedc & 0xf), (uint16_t)((fedc >> 4) & 0xf),
      (uint16_t)((fedc >> 8) & 0xf), (uint16_t)((fedc >> 12) & 0xf), g
   };
   if (n > 5) n = 5;
   for (int i = 0; i < n; ++i)
      slots[i] = idx[i] < nregs ? regs[idx[i]] : 0;
   return n;
}

static struct dvm_class *resolve_type(struct dvm *vm, struct dvm_dex *dd, uint32_t idx)
{
   if (dd->type_cache && idx < dd->file.type_ids_size && dd->type_cache[idx])
      return dd->type_cache[idx];
   const char *t = dex_type(&dd->file, idx);
   if (!t) return NULL;
   struct dvm_class *c = dvm__class_by_desc(vm, t);
   if (dd->type_cache && idx < dd->file.type_ids_size)
      dd->type_cache[idx] = c;
   return c;
}

static struct dvm_dex *dex_of(struct dvm *vm, struct dex_file *f)
{
   for (int i = 0; i < vm->ndexes; ++i)
      if (&vm->dexes[i]->file == f) return vm->dexes[i];
   return NULL;
}

static struct dvm_method *resolve_method(struct dvm *vm, struct dvm_dex *dd,
                                         uint32_t idx, struct dvm_class **out_cls,
                                         const char **out_name, const char **out_sig)
{
   /* The cache is consulted first, because everything below it is the
    * expensive half: dex_proto_signature() builds the descriptor string out of
    * the proto's parameter type ids, and dvm__class_by_desc() hashes and
    * compares the class descriptor.  Both used to run on every single invoke
    * instruction and only then was the cached answer returned, so the cache
    * saved the lookup and paid for the parsing — profiles of a Cross Worlds
    * load screen sat in dex_proto_signature() through the interpreter.
    *
    * A resolved method already knows the three things the caller wanted out of
    * the constant pool: its class, its name and its signature.  The referenced
    * class may be a subclass of the declaring one, which is what the caller
    * wants anyway — dispatch is on the receiver and the error messages name
    * the method that was actually found. */
   if (dd->method_cache && idx < dd->file.method_ids_size && dd->method_cache[idx]) {
      struct dvm_method *m = dd->method_cache[idx];
      *out_cls  = m->cls;
      *out_name = m->name;
      *out_sig = m->sig ? m->sig : "";
      return m;
   }

   struct dex_method_id mid;
   if (!dex_method_id(&dd->file, idx, &mid)) return NULL;
   const char *cd = dex_type(&dd->file, mid.class_idx);
   const char *name = dex_string(&dd->file, mid.name_idx);
   if (!cd || !name) return NULL;
   if (!dd->proto_cache || mid.proto_idx >= dd->file.proto_ids_size) {
      dvm__throw(vm, "java/lang/VerifyError", "invalid method prototype");
      return NULL;
   }
   if (!dd->proto_cache[mid.proto_idx])
      dd->proto_cache[mid.proto_idx] = dex_proto_signature_alloc(&dd->file, mid.proto_idx);
   *out_sig = dd->proto_cache[mid.proto_idx];
   if (!*out_sig) {
      *out_sig = "";
      dvm__throw(vm, "java/lang/VerifyError", "cannot read method descriptor");
      return NULL;
   }

   struct dvm_class *cls = dvm__class_by_desc(vm, cd);
   *out_cls = cls;
   *out_name = name;
   if (!cls) return NULL;

   struct dvm_method *m = dvm_find_method(vm, cls, name, *out_sig);
   if (m && dd->method_cache && idx < dd->file.method_ids_size)
      dd->method_cache[idx] = m;
   return m;
}

static struct dvm_field *resolve_field(struct dvm *vm, struct dvm_dex *dd,
                                       uint32_t idx, bool statics,
                                       struct dvm_class **out_cls,
                                       const char **out_name, const char **out_type)
{
   if (dd->field_cache && idx < dd->file.field_ids_size && dd->field_cache[idx]) {
      struct dvm_field *f = dd->field_cache[idx];
      *out_cls = f->cls;
      *out_name = f->name;
      *out_type = f->type;
      return f;
   }
   struct dex_field_id fid;
   if (!dex_field_id(&dd->file, idx, &fid)) return NULL;
   const char *cd = dex_type(&dd->file, fid.class_idx);
   *out_name = dex_string(&dd->file, fid.name_idx);
   *out_type = dex_type(&dd->file, fid.type_idx);
   if (!cd || !*out_name) return NULL;
   struct dvm_class *cls = dvm__class_by_desc(vm, cd);
   *out_cls = cls;
   if (!cls) return NULL;
   struct dvm_field *f = class_find_field(cls, *out_name, *out_type, statics);
   if (f && dd->field_cache && idx < dd->file.field_ids_size)
      dd->field_cache[idx] = f;
   return f;
}

static int dex_index(struct dvm *vm, struct dvm_dex *dd)
{
   for (int i = 0; i < vm->ndexes; ++i)
      if (vm->dexes[i] == dd) return i;
   return -1;
}

static dvm_ref const_string(struct dvm *vm, struct dvm_dex *dd, uint32_t idx)
{
   if (dd->string_cache && idx < dd->file.string_ids_size && dd->string_cache[idx])
      return dd->string_cache[idx];
   const char *s = dex_string(&dd->file, idx);
   {
      const char *want = dvm__env_strwatch();
      if (want && s && strstr(s, want))
         fprintf(stderr, "[dvm] const-string[%u] = \"%s\" in %s.%s (dex %d)\n",
                 idx, s,
                 vm->cur_method && vm->cur_method->cls
                    ? vm->cur_method->cls->name : "?",
                 vm->cur_method ? vm->cur_method->name : "?",
                 dex_index(vm, dd));
   }
   dvm_ref r = dvm__intern(vm, s ? s : "");
   if (dd->string_cache && idx < dd->file.string_ids_size)
      dd->string_cache[idx] = r;
   return r;
}

static dvm_ref class_object(struct dvm *vm, struct dvm_class *cls)
{
   if (!cls) return 0;
   if (cls->class_object) return cls->class_object;
   dvm_ref r = dvm_new_object(vm, dvm__class_by_desc(vm, "Ljava/lang/Class;"));
   struct dvm_object *o = dvm__obj(vm, r);
   if (o) {
      o->kind = DVM_OBJ_CLASS;
      o->klass = cls;
      ++o->pins;
   }
   cls->class_object = r;
   return r;
}

dvm_ref dvm_class_object(struct dvm *vm, struct dvm_class *cls)
{
   return class_object(vm, cls);
}

/* Array element access shared by aget/aput. */
static bool array_check(struct dvm *vm, struct dvm_object *a, uint32_t idx)
{
   if (!a || a->kind != DVM_OBJ_ARRAY) {
      dvm__throw(vm, "java/lang/NullPointerException", "array is null");
      return false;
   }
   if (idx >= a->length) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException",
                 "length=%u index=%u", a->length, idx);
      return false;
   }
   return true;
}

static bool gc_external_static(struct dvm *vm,const char *cls,const char *field,
                               const char *type,union dvm_value *out)
{
   struct dvm_gc_native_scope scope={0};
   if(!dvm_gc_native_enter(vm,&scope,0,"()V",NULL,0)){
      dvm_gc_native_leave(&scope);return false;
   }
   bool ok=vm->hooks.get_external_static(vm->hooks.user,vm,cls,field,type,out);
   dvm_gc_native_leave_result(&scope,type&&(type[0]=='L'||type[0]=='[')?
      "()Ljava/lang/Object;":"()V",ok&&out?out->l:0);
   return ok&&!vm->exception;
}

static bool execute(struct dvm *vm, struct frame *fr, union dvm_value *out)
{
   if(gc_exec_test_probe)gc_exec_test_probe(vm);
   struct dvm_method *m = fr->m;
   const char *trace_class = dvm__env_trace_class();
   bool trace_this = !trace_class ||
      (m->cls && m->cls->name && strstr(m->cls->name, trace_class));
   const uint16_t *insns = m->code.insns;
   uint32_t nins = m->code.insns_size;
   uint32_t nregs = fr->nregs;
   uint32_t *r = fr->regs;
   struct dvm_dex *dd = dex_of(vm, m->dex);
   if (m->dex && !dd) {
      /* Every constant-pool index in this method is meaningless without its
       * own dex; resolving against another one silently returns the wrong
       * class and method.  Fail loudly instead. */
      static bool warned;
      if (!warned) {
         warned = true;
         fprintf(stderr, "[dvm] %s.%s: dex not registered — constant pool "
                 "cannot be resolved\n", m->cls ? m->cls->name : "?", m->name);
      }
   }
   const char *sigbuf = "";

   memset(out, 0, sizeof *out);

/* Operand fetch.  A truncated method would otherwise read past the end of the
 * instruction array; the dex is third-party input, so read zero instead. */
#define IU(k) ((pc + (uint32_t)(k) < nins) ? insns[pc + (uint32_t)(k)] : (uint16_t)0)
#define REQ(reg) do { if ((uint32_t)(reg) >= nregs) { \
      dvm__throw(vm, "java/lang/VerifyError", "%s.%s register %u of %u", \
                 m->cls->name, m->name, (unsigned)(reg), nregs); \
      goto exception; } } while (0)

   for (;;) {
      if (fr->pc >= nins) {
         dvm__throw(vm, "java/lang/VerifyError", "%s.%s ran past the end",
                    m->cls->name, m->name);
         goto exception;
      }
      if((vm->steps&4095u)==0)gc_fast_debug_poll(vm);
#ifdef LUNARIA_UNITY_PLAYER_PROBE
      json_hot_tick(vm,m);
#endif
      ++vm->steps;
      if((vm->steps&4095u)==0)gil_deep_tick(fr->pc);
#ifndef LUNARIA_TASK7_LOGIN_PANEL
      if (vm->steps>DVM_AGGREGATE_STEP_LIMIT) {
         sdk_trace_fprintf(stderr,
          "BUDGET_EXHAUSTED stage=interpreter total_steps=%llu aggregate_limit=%llu thread=%llu method=%s.%s pc=%u\n",
          (unsigned long long)vm->steps,
          (unsigned long long)DVM_AGGREGATE_STEP_LIMIT,
          (unsigned long long)(uintptr_t)pthread_self(),
          m->cls && m->cls->name ? m->cls->name : "?",
          m->name ? m->name : "?", (unsigned)fr->pc);
         dvm__throw(vm,"java/lang/Error","aggregate diagnostic bytecode budget");
         dvm_request_stop(vm); return false;
      }
#endif
      /* Offer the lock up periodically.  Nothing preempts bytecode, so without
       * this a thread in a long loop starves every other one for as long as it
       * runs.  The top of the dispatch loop is the one place in a call where
       * nothing but frames, registers and dex mappings are live, and none of
       * those move when another thread allocates. */
      if ((vm->steps & (8192u - 1)) == 0 &&
          (gil_short_enabled() || (vm->steps & (DVM_GIL_YIELD_STEPS - 1)) == 0)) {
         if (luna_guest_paused()) {
            unsigned ael = arm_lock_unlock_all();
            unsigned gil = dvm_gil_unlock_all(vm);
            const struct timespec interval = {0, 10000000};
            while (luna_guest_paused() && !dvm_is_stopping(vm)) {
               arm_exec_pause_poll();
               nanosleep(&interval, NULL);
            }
            dvm_gil_relock(vm, gil);
            arm_lock_relock(ael);
         }
         dvm_gil_yield(vm);
         if (dvm_is_stopping(vm)) return false;
      }
      /* And the *execution* lock with it, more often.
       *
       * Java reached from guest native code (dvm_jni_invoke) runs with the ARM
       * execution lock still held, because the SVC handler that called in is
       * holding it.  Every guest thread in the process is therefore stopped
       * for as long as that Java call runs -- measured on Cross Worlds: a
       * single JNI call into the security module's Java side held it for
       * 1037 ms, and the main-Looper tick that the frame pump drives (and
       * with it that module's own heartbeat callback) came 691 ms late.  On a
       * device a JNI transition stops nothing.
       *
       * Lock order here is interpreter-outside, execution-inside, so giving
       * the inner lock back while holding the outer one is the safe
       * direction, and the top of the dispatch loop is the same safe point
       * the GIL yield above uses.  arm_lock_yield() is one relaxed load when
       * nobody is waiting, which is why this can afford the shorter
       * interval. */
      if ((vm->steps & (DVM_AEL_YIELD_STEPS - 1)) == 0) arm_lock_yield();
      if (vm->step_limit && ++vm->call_steps > vm->step_limit) {
#ifdef LUNARIA_UNITY_PLAYER_PROBE
         json_hot_report(0,NULL);
         task_profile_report(vm,"step_limit");
#endif
         /* Report-visible diagnostic before the original [dvm] lines.
          * validate_probe_device.py keeps only selected prefixes (EXCEPTION
          * among them), so these lines survive the report filter while the
          * [dvm] lines below do not.  The step limit and the thrown
          * java/lang/Error are unchanged: this only exposes the failing
          * method and up to 12 callers, matching the [dvm] stack depth. */
         sdk_trace_fprintf(stderr,
                 "EXCEPTION STEP_LIMIT where=step_limit method=%s.%s%s "
                 "depth=%d limit=%llu steps=%llu pc=%u\n",
                 m->cls && m->cls->name ? m->cls->name : "?",
                 m->name ? m->name : "?", m->sig ? m->sig : "",
                 vm->depth, (unsigned long long)vm->step_limit,
                 (unsigned long long)vm->call_steps, (unsigned)fr->pc);
         for (int i = vm->ncallstack - 1, shown = 0; i >= 0 && shown < 12; --i, ++shown) {
            const struct dvm_method *f = vm->callstack[i];
            if (!f) continue;
            sdk_trace_fprintf(stderr,
                    "EXCEPTION STEP_LIMIT_FRAME index=%d method=%s.%s%s\n",
                    shown, f->cls && f->cls->name ? f->cls->name : "?",
                    f->name ? f->name : "?", f->sig ? f->sig : "");
         }
         fprintf(stderr, "[dvm] step limit reached in %s.%s — aborting the call "
                 "(%llu steps, depth=%d, limit=%llu); call stack:\n",
                 m->cls->name, m->name, (unsigned long long)vm->call_steps,
                 vm->depth, (unsigned long long)vm->step_limit);
         for (int i = vm->ncallstack - 1, shown = 0; i >= 0 && shown < 12; --i, ++shown) {
            const struct dvm_method *f = vm->callstack[i];
            if (!f) continue;
            fprintf(stderr, "[dvm]   #%d %s.%s%s\n", shown,
                    f->cls ? f->cls->name : "?", f->name, f->sig ? f->sig : "");
         }
         dvm__throw(vm, "java/lang/Error", "dvm step limit");
         goto exception;
      }

      uint32_t pc = fr->pc;
#ifdef LUNARIA_UNITY_PLAYER_PROBE
      task_profile_tick(vm,m);
#endif
      uint16_t u0 = insns[pc];
      uint8_t op = OPCODE(u0);
      if(leak_scope_current)leak_scope_opcode(vm,op);
      vm->cur_method = m;
      vm->cur_pc = pc;

      if (vm->trace >= 2 && trace_this)
         fprintf(stderr, "[dvm]   %s.%s @%04x op=%02x words=%04x,%04x,%04x\n",
                 m->cls->name, m->name, pc, op, u0, IU(1), IU(2));
      if (vm->trace >= 3 && trace_this) {
         fprintf(stderr, "[dvm]     regs");
         for (uint32_t i = 0; i < nregs; ++i)
            fprintf(stderr, " v%u=%08x", i, r[i]);
         fputc('\n', stderr);
      }

      switch (op) {
      case 0x00: /* nop, and the payload pseudo-ops when reached by accident */
         fr->pc += 1;
         break;

      /* --- moves --- */
      case 0x01: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = r[B4(u0)]; fr->pc += 1; break;
      case 0x07: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = r[B4(u0)]; fr->pc += 1; break;
      case 0x02: case 0x08:
         REQ(AA(u0)); REQ(IU(1));
         r[AA(u0)] = r[IU(1)];
         fr->pc += 2;
         break;
      case 0x03: case 0x09:
         REQ(IU(1)); REQ(IU(2));
         r[IU(1)] = r[IU(2)];
         fr->pc += 3;
         break;
      case 0x04:
         REQ(A4(u0) + 1); REQ(B4(u0) + 1);
         sw(r, A4(u0), rw(r, B4(u0)));
         fr->pc += 1;
         break;
      case 0x05:
         REQ(AA(u0) + 1); REQ(IU(1) + 1);
         sw(r, AA(u0), rw(r, IU(1)));
         fr->pc += 2;
         break;
      case 0x06:
         REQ(IU(1) + 1); REQ(IU(2) + 1);
         sw(r, IU(1), rw(r, IU(2)));
         fr->pc += 3;
         break;

      case 0x0a: /* move-result */
      case 0x0c: /* move-result-object */
         REQ(AA(u0));
         r[AA(u0)] = (uint32_t)fr->result;
         fr->pc += 1;
         break;
      case 0x0b: /* move-result-wide */
         REQ(AA(u0) + 1);
         sw(r, AA(u0), fr->result);
         fr->pc += 1;
         break;
      case 0x0d: /* move-exception */
         REQ(AA(u0));
         /* The handler entry already cleared the pending exception; the
          * object itself is kept in exc_ref for exactly this instruction. */
         r[AA(u0)] = vm->exception ? vm->exception : vm->exc_ref;
         vm->exception = 0;
         fr->pc += 1;
         break;

      /* --- returns --- */
      case 0x0e:
         return true;
      case 0x0f: case 0x11:
         REQ(AA(u0));
         out->u = r[AA(u0)];
         return true;
      case 0x10:
         REQ(AA(u0) + 1);
         out->ju = rw(r, AA(u0));
         return true;

      /* --- constants --- */
      case 0x12: { /* const/4 */
         REQ(A4(u0));
         int32_t v = (int32_t)(int8_t)(uint8_t)(B4(u0) << 4) >> 4;
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 1;
         break;
      }
      case 0x13: /* const/16 */
         REQ(AA(u0));
         r[AA(u0)] = (uint32_t)(int32_t)(int16_t)IU(1);
         fr->pc += 2;
         break;
      case 0x14: /* const */
         REQ(AA(u0));
         r[AA(u0)] = (uint32_t)IU(1) | ((uint32_t)IU(2) << 16);
         fr->pc += 3;
         break;
      case 0x15: /* const/high16 */
         REQ(AA(u0));
         r[AA(u0)] = (uint32_t)IU(1) << 16;
         fr->pc += 2;
         break;
      case 0x16: /* const-wide/16 */
         REQ(AA(u0) + 1);
         sw(r, AA(u0), (uint64_t)(int64_t)(int16_t)IU(1));
         fr->pc += 2;
         break;
      case 0x17: /* const-wide/32 */
         REQ(AA(u0) + 1);
         sw(r, AA(u0), (uint64_t)(int64_t)(int32_t)
            ((uint32_t)IU(1) | ((uint32_t)IU(2) << 16)));
         fr->pc += 3;
         break;
      case 0x18: { /* const-wide */
         REQ(AA(u0) + 1);
         uint64_t v = 0;
         for (int i = 0; i < 4; ++i) v |= (uint64_t)IU(1 + i) << (16 * i);
         sw(r, AA(u0), v);
         fr->pc += 5;
         break;
      }
      case 0x19: /* const-wide/high16 */
         REQ(AA(u0) + 1);
         sw(r, AA(u0), (uint64_t)IU(1) << 48);
         fr->pc += 2;
         break;
      case 0x1a: /* const-string */
         REQ(AA(u0));
         r[AA(u0)] = dd ? const_string(vm, dd, IU(1)) : 0;
         fr->pc += 2;
         break;
      case 0x1b: /* const-string/jumbo */
         REQ(AA(u0));
         r[AA(u0)] = dd ? const_string(vm, dd,
                        (uint32_t)IU(1) | ((uint32_t)IU(2) << 16)) : 0;
         fr->pc += 3;
         break;
      case 0x1c: { /* const-class */
         REQ(AA(u0));
         /* A type the loader cannot produce is NoClassDefFoundError, not a
          * null Class.  Handing back null turned "this optional component is
          * not in the package" into a NullPointerException several frames
          * later, inside whatever the caller did with the Class object —
          * CrashSight's Singleton.getSingleton(clazz) reports the NPE from
          * clazz.newInstance() and never mentions the class that is missing.
          * new-instance has always answered this way (case 0x22); this makes
          * the two agree. */
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         if (!t || class_absent(vm, t)) {
            dvm__throw(vm, "java/lang/NoClassDefFoundError", "%s",
                       t && t->name ? t->name : "?");
            goto exception;
         }
         r[AA(u0)] = class_object(vm, t);
         fr->pc += 2;
         break;
      }

      case 0x1d: /* monitor-enter */
         REQ(AA(u0));
         if (!dvm__monitor_enter(vm, r[AA(u0)])) goto exception;
         fr->pc += 1;
         break;
      case 0x1e: /* monitor-exit */
         REQ(AA(u0));
         if (!dvm__monitor_exit(vm, r[AA(u0)])) goto exception;
         fr->pc += 1;
         break;

      case 0x1f: { /* check-cast */
         REQ(AA(u0));
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         struct dvm_object *o = dvm__obj(vm, r[AA(u0)]);
         if (o && t && !class_assignable(vm, o->cls, t)) {
            fprintf(stderr, "[dvm] check-cast at %s.%s%s pc=%u: %s to %s\n",
                    m->cls->name, m->name, m->sig ? m->sig : "",
                    fr->pc, o->cls ? o->cls->name : "?", t->name);
            if (getenv("LUNARIA_DVM_CASTVAL") && o->cls && !strcmp(o->cls->name, "java/lang/String")) {
               const char *sv = dvm_string_utf8(vm, r[AA(u0)]);
               fprintf(stderr, "[dvm]   value=%.200s\n", sv ? sv : "?");
            }
            dvm__throw(vm, "java/lang/ClassCastException", "%s to %s",
                       o->cls ? o->cls->name : "?", t->name);
            goto exception;
         }
         fr->pc += 2;
         break;
      }
      case 0x20: { /* instance-of */
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         struct dvm_object *o = dvm__obj(vm, r[B4(u0)]);
         r[A4(u0)] = (o && t && class_assignable(vm, o->cls, t)) ? 1u : 0u;
         fr->pc += 2;
         break;
      }
      case 0x21: { /* array-length */
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_object *o = dvm__obj(vm, r[B4(u0)]);
         if (!o || o->kind != DVM_OBJ_ARRAY) {
            dvm__throw(vm, "java/lang/NullPointerException", "array-length");
            goto exception;
         }
         r[A4(u0)] = o->length;
         fr->pc += 1;
         break;
      }
      case 0x22: { /* new-instance */
         REQ(AA(u0));
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         if (!t || class_absent(vm, t)) {
            dvm__throw(vm, "java/lang/NoClassDefFoundError", "%s",
                       t && t->name ? t->name : "?");
            goto exception;
         }
         if (!dvm_init_class(vm, t)) goto exception;
         if (t->external && vm->hooks.new_external) {
            struct dvm_gc_native_scope scope={0};
            if(!dvm_gc_native_enter(vm,&scope,0,"()V",NULL,0)){
               dvm_gc_native_leave(&scope);goto exception;
            }
            uint32_t h = vm->hooks.new_external(vm->hooks.user, vm, t->name);
            dvm_gc_native_leave_result(&scope,"()Ljava/lang/Object;",h);
            if(vm->exception)goto exception;
            r[AA(u0)] = h ? h : dvm_new_object(vm, t);
         } else {
            r[AA(u0)] = dvm_new_object(vm, t);
         }
         fr->pc += 2;
         break;
      }
      case 0x23: { /* new-array */
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         int32_t len = (int32_t)r[B4(u0)];
         if (len < 0) {
            dvm__throw(vm, "java/lang/NegativeArraySizeException", "%d", len);
            goto exception;
         }
         const char *ed = (t && t->desc[0] == '[') ? t->desc + 1 : "I";
         r[A4(u0)] = dvm_new_array(vm, dvm__kind_of(ed), ed, (uint32_t)len);
         fr->pc += 2;
         break;
      }
      case 0x24: case 0x25: { /* filled-new-array[/range] */
         bool range = (op == 0x25);
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         uint32_t slots[256];
         int n = gather_args(insns, nins, pc, range, r, nregs, slots);
         const char *ed = (t && t->desc[0] == '[') ? t->desc + 1 : "I";
         char kind = dvm__kind_of(ed);
         dvm_ref arr = dvm_new_array(vm, kind, ed, (uint32_t)n);
         struct dvm_object *ao = dvm__obj(vm, arr);
         if (ao) {
            for (int i = 0; i < n; ++i) {
               switch (dvm__elem_width(kind)) {
                  case 1: ((uint8_t *)ao->data)[i] = (uint8_t)slots[i]; break;
                  case 2: ((uint16_t *)ao->data)[i] = (uint16_t)slots[i]; break;
                  default:
                     if (kind == 'L') GC_STORE_REF(vm,&((dvm_ref *)ao->data)[i],slots[i],"filled_new_reference_array");
                     else ((uint32_t *)ao->data)[i] = slots[i];
                     break;
               }
            }
         }
         fr->result = arr;
         fr->pc += 3;
         break;
      }
      case 0x26: { /* fill-array-data */
         REQ(AA(u0));
         int32_t off = (int32_t)((uint32_t)IU(1) | ((uint32_t)IU(2) << 16));
         uint32_t poff = pc + (uint32_t)off;
         struct dvm_object *ao = dvm__obj(vm, r[AA(u0)]);
         if (!ao || ao->kind != DVM_OBJ_ARRAY) {
            dvm__throw(vm, "java/lang/NullPointerException", "fill-array-data");
            goto exception;
         }
         if (poff + 4 <= nins && insns[poff] == 0x0300) {
            uint16_t width = insns[poff + 1];
            uint32_t count = (uint32_t)insns[poff + 2] | ((uint32_t)insns[poff + 3] << 16);
            uint32_t n = count < ao->length ? count : ao->length;
            /* The payload is `count * width` bytes; a truncated one would
             * read past the code array. */
            uint64_t units = ((uint64_t)count * width + 1u) / 2u;
            if (poff + 4u + units <= nins && width == (uint16_t)dvm__elem_width(ao->elem_kind))
               memcpy(ao->data, (const uint8_t *)&insns[poff + 4], (size_t)n * width);
         }
         fr->pc += 3;
         break;
      }
      case 0x27: /* throw */
         REQ(AA(u0));
         if (!r[AA(u0)]) {
            dvm__throw(vm, "java/lang/NullPointerException", "throw null");
         } else {
            /* An explicit throw skips dvm__throw, so stamp the origin here or
             * the backtrace would attribute it to whichever frame unwinds
             * first.  A rethrow of the same object — the `move-exception;
             * monitor-exit; throw` shape a synchronized block compiles to —
             * must keep the origin it already has, or the frame that really
             * failed is lost. */
            vm->exception = r[AA(u0)];
            if (vm->exc_ref != r[AA(u0)]) {
               /* LUNARIA_DVM_THROWS covers the exceptions app code throws
                * itself too, not only the VM's own: a library that turns a
                * platform gap into its own exception (Gson's
                * JsonSyntaxException, a Kotlin check()) is otherwise
                * invisible between the gap and the catch that swallows it. */
               if (dvm__env_throws()) {
                  char desc[384];
                  dvm_describe_exception(vm, r[AA(u0)], desc, sizeof desc);
                  fprintf(stderr, "[dvm] throw %s (in %s.%s @%04x)\n", desc,
                          m->cls && m->cls->name ? m->cls->name : "?",
                          m->name, pc * 2);
               }
               {
                  /* LUNARIA_DVM_THROW_STACK=<text>: the call chain of an app throw
                   * whose description contains <text> (see dvm__throw). */
                  static const char *stack_filter = (const char *)-1;
                  if (stack_filter == (const char *)-1) stack_filter = getenv("LUNARIA_DVM_THROW_STACK");
                  char desc[384];
                  if (stack_filter && *stack_filter) {
                     dvm_describe_exception(vm, r[AA(u0)], desc, sizeof desc);
                     if (strstr(desc, stack_filter)) {
                        fprintf(stderr, "[dvm] throw %s — call chain, innermost first:\n", desc);
                        const struct dvm_call_frame *f = vm->invoke_frame;
                        for (int shown = 0; f && shown < 14; f = f->previous, ++shown)
                           if (f->method)
                              fprintf(stderr, "[dvm]     #%d %s.%s%s\n", shown,
                                      f->method->cls ? f->method->cls->name : "?", f->method->name,
                                      f->method->sig ? f->method->sig : "");
                     }
                  }
               }
               vm->exc_ref = r[AA(u0)];
               vm->nexc_trace = 0;
               vm->exc_trace[vm->nexc_trace++] = m;
               vm->exc_pc = pc;
            } else if (vm->nexc_trace <
                       (int)(sizeof vm->exc_trace / sizeof vm->exc_trace[0]) &&
                       (vm->nexc_trace == 0 ||
                        vm->exc_trace[vm->nexc_trace - 1] != m)) {
               vm->exc_trace[vm->nexc_trace++] = m;
            }
         }
         goto exception;

      /* --- branches --- */
      case 0x28: { /* goto */
         int32_t off = (int8_t)AA(u0);
         if (!off) { dvm__throw(vm, "java/lang/VerifyError", "goto 0"); goto exception; }
         fr->pc = (uint32_t)((int32_t)pc + off);
         break;
      }
      case 0x29:
         fr->pc = (uint32_t)((int32_t)pc + (int16_t)IU(1));
         break;
      case 0x2a:
         fr->pc = (uint32_t)((int32_t)pc + (int32_t)
                  ((uint32_t)IU(1) | ((uint32_t)IU(2) << 16)));
         break;

      case 0x2b: case 0x2c: { /* packed-switch / sparse-switch */
         REQ(AA(u0));
         int32_t off = (int32_t)((uint32_t)IU(1) | ((uint32_t)IU(2) << 16));
         uint32_t poff = pc + (uint32_t)off;
         int32_t val = (int32_t)r[AA(u0)];
         uint32_t target = pc + 3;
         if (poff + 2 <= nins) {
            uint16_t ident = insns[poff];
            uint16_t size = insns[poff + 1];
            if (ident == 0x0100 && op == 0x2b &&
                (uint64_t)poff + 4u + (uint64_t)size * 2u <= nins) {
               int32_t first = (int32_t)((uint32_t)insns[poff + 2] |
                                         ((uint32_t)insns[poff + 3] << 16));
               int64_t k = (int64_t)val - first;
               if (k >= 0 && k < size) {
                  uint32_t e = poff + 4 + (uint32_t)k * 2u;
                  target = (uint32_t)((int32_t)pc + (int32_t)
                     ((uint32_t)insns[e] | ((uint32_t)insns[e + 1] << 16)));
               }
            } else if (ident == 0x0200 && op == 0x2c &&
                       (uint64_t)poff + 2u + (uint64_t)size * 4u <= nins) {
               for (uint16_t k = 0; k < size; ++k) {
                  uint32_t ke = poff + 2 + (uint32_t)k * 2u;
                  int32_t key = (int32_t)((uint32_t)insns[ke] |
                                          ((uint32_t)insns[ke + 1] << 16));
                  if (key != val) continue;
                  uint32_t te = poff + 2 + (uint32_t)size * 2u + (uint32_t)k * 2u;
                  target = (uint32_t)((int32_t)pc + (int32_t)
                     ((uint32_t)insns[te] | ((uint32_t)insns[te + 1] << 16)));
                  break;
               }
            }
         }
         fr->pc = target;
         break;
      }

      /* --- comparisons --- */
      case 0x2d: case 0x2e: { /* cmpl-float / cmpg-float */
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         float a = bits_to_f(r[bc & 0xff]), b = bits_to_f(r[bc >> 8]);
         int32_t v;
         if (isunordered(a, b)) v = (op == 0x2d) ? -1 : 1;   /* NaN */
         else if (isgreater(a, b)) v = 1;
         else if (isless(a, b)) v = -1;
         else v = 0;
         r[AA(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }
      case 0x2f: case 0x30: { /* cmpl-double / cmpg-double */
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ((bc & 0xff) + 1); REQ((bc >> 8) + 1);
         double a = bits_to_d(rw(r, bc & 0xff)), b = bits_to_d(rw(r, bc >> 8));
         int32_t v;
         if (isunordered(a, b)) v = (op == 0x2f) ? -1 : 1;   /* NaN */
         else if (isgreater(a, b)) v = 1;
         else if (isless(a, b)) v = -1;
         else v = 0;
         r[AA(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }
      case 0x31: { /* cmp-long */
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ((bc & 0xff) + 1); REQ((bc >> 8) + 1);
         int64_t a = (int64_t)rw(r, bc & 0xff), b = (int64_t)rw(r, bc >> 8);
         r[AA(u0)] = (uint32_t)(a > b ? 1 : (a == b ? 0 : -1));
         fr->pc += 2;
         break;
      }

      case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
         REQ(A4(u0)); REQ(B4(u0));
         int32_t a = (int32_t)r[A4(u0)], b = (int32_t)r[B4(u0)];
         bool t;
         switch (op) {
            case 0x32: t = (a == b); break;
            case 0x33: t = (a != b); break;
            case 0x34: t = (a < b); break;
            case 0x35: t = (a >= b); break;
            case 0x36: t = (a > b); break;
            default:   t = (a <= b); break;
         }
         fr->pc = t ? (uint32_t)((int32_t)pc + (int16_t)IU(1)) : pc + 2;
         break;
      }
      case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
         REQ(AA(u0));
         int32_t a = (int32_t)r[AA(u0)];
         bool t;
         switch (op) {
            case 0x38: t = (a == 0); break;
            case 0x39: t = (a != 0); break;
            case 0x3a: t = (a < 0); break;
            case 0x3b: t = (a >= 0); break;
            case 0x3c: t = (a > 0); break;
            default:   t = (a <= 0); break;
         }
         if (m && m->name && !strcmp(m->name,"updateDisplayInternal")) {
            char line[160];
            int len = snprintf(line,sizeof line,"IF_UPDATE op=0x%x pc=0x%x a=0x%x t=%d target=0x%x\n",
                               op,(unsigned)pc,(unsigned)a,t?1:0,
                               t?(unsigned)((int32_t)pc + (int16_t)IU(1)):(unsigned)(pc + 2));
            FILE *df = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
            if (df) { if (len > 0) fwrite(line,1,(size_t)len < sizeof line ? (size_t)len : sizeof line - 1,df); fclose(df); }
         }
         fr->pc = t ? (uint32_t)((int32_t)pc + (int16_t)IU(1)) : pc + 2;
         break;
      }

      /* --- arrays --- */
      case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         struct dvm_object *a = dvm__obj(vm, r[bc & 0xff]);
         uint32_t idx = r[bc >> 8];
         if (!array_check(vm, a, idx)) goto exception;
         switch (op) {
            case 0x44: r[AA(u0)] = ((uint32_t *)a->data)[idx]; break;
            case 0x45: REQ(AA(u0) + 1); sw(r, AA(u0), ((uint64_t *)a->data)[idx]); break;
            case 0x46: r[AA(u0)] = ((uint32_t *)a->data)[idx]; break;
            case 0x47: r[AA(u0)] = ((uint8_t *)a->data)[idx] ? 1u : 0u; break;
            case 0x48: r[AA(u0)] = (uint32_t)(int32_t)(int8_t)((uint8_t *)a->data)[idx]; break;
            case 0x49: r[AA(u0)] = ((uint16_t *)a->data)[idx]; break;
            default:   r[AA(u0)] = (uint32_t)(int32_t)(int16_t)((uint16_t *)a->data)[idx]; break;
         }
         fr->pc += 2;
         break;
      }
      case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         struct dvm_object *a = dvm__obj(vm, r[bc & 0xff]);
         uint32_t idx = r[bc >> 8];
         if (!array_check(vm, a, idx)) goto exception;
         switch (op) {
            case 0x4b: ((uint32_t *)a->data)[idx] = r[AA(u0)]; break;
            case 0x4c: REQ(AA(u0) + 1); ((uint64_t *)a->data)[idx] = rw(r, AA(u0)); break;
            case 0x4d: /* DEX aput-object; primitive aput is 0x4b. */
               GC_STORE_REF(vm,&((dvm_ref *)a->data)[idx],r[AA(u0)],"aput-object"); break;
            case 0x4e: ((uint8_t *)a->data)[idx] = r[AA(u0)] ? 1u : 0u; break;
            case 0x4f: ((uint8_t *)a->data)[idx] = (uint8_t)r[AA(u0)]; break;
            case 0x50: ((uint16_t *)a->data)[idx] = (uint16_t)r[AA(u0)]; break;
            default:   ((uint16_t *)a->data)[idx] = (uint16_t)r[AA(u0)]; break;
         }
         fr->pc += 2;
         break;
      }

      /* --- instance fields --- */
      case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: {
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_class *fc; const char *fn, *ft;
         struct dvm_field *f = dd ? resolve_field(vm, dd, IU(1), false, &fc, &fn, &ft)
                                  : NULL;
         struct dvm_object *o = dvm__obj(vm, r[B4(u0)]);
         if (!o) {
            /* Name the field's declaring class as well.  Obfuscated code
             * reuses one identifier for dozens of unrelated fields, so
             * "iget getMonetizationNetwork" fits every link of a chain like
             * a.b.c.d and says nothing about which one is null. */
            /* The bytecode offset as well: one method can read the same
             * field name from several places, and the offset is what maps the
             * report back to a single instruction in the dex. */
            dvm__throw(vm, "java/lang/NullPointerException",
                       "iget %s.%s:%s at %s.%s@%04x",
                       fc && fc->name ? fc->name : "?", fn ? fn : "?",
                       ft ? ft : "?", m->cls ? m->cls->name : "?", m->name, pc);
            goto exception;
         }
         /* A field the object does not have belongs to a host-backed class;
          * those carry no state on this side, so it reads as zero — the same
          * answer the stub layer gave before this module existed. */
         union dvm_value v = { 0 };
         if (!f) {
            sdk_trace_unresolved_field(fc ? fc->name : NULL, fn, ft);
            dvm__throw(vm, "java/lang/NoSuchFieldError", "%s.%s:%s", fc ? fc->name : "?", fn ? fn : "?", ft ? ft : "?");
            goto exception;
         }
         if (f && o->slots && f->slot < (o->cls ? o->cls->islots : 0))
            v = o->slots[f->slot];
         if (f && f->name && !strcmp(f->name,"a") && fc && fc->name && !strcmp(fc->name,"com/unity3d/player/U0") && op == 0x54) {
            char line[200];
            int len = snprintf(line,sizeof line,
                               "IGET_U0_A receiver=0x%x slot=%u islots=%d value=0x%x method=%s pc=0x%x\n",
                               (unsigned)r[B4(u0)],(unsigned)f->slot,
                               o->cls ? (int)o->cls->islots : -1,(unsigned)v.u,
                               m && m->name ? m->name : "?", (unsigned)pc);
            FILE *df = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
            if (df) { if (len > 0) fwrite(line,1,(size_t)len < sizeof line ? (size_t)len : sizeof line - 1,df); fclose(df); }
         }
         if (vm->trace && trace_this && op == 0x54)
            fprintf(stderr, "[dvm] %*siget-object @%x.%s:%s -> @%x\n",
                    vm->depth * 2, "", r[B4(u0)], fn ? fn : "?",
                    ft ? ft : "?", v.l);
         if (op == 0x53) { REQ(A4(u0) + 1); sw(r, A4(u0), v.ju); }
         else r[A4(u0)] = v.u;
         fr->pc += 2;
         break;
      }
      case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_class *fc; const char *fn, *ft;
         struct dvm_field *f = dd ? resolve_field(vm, dd, IU(1), false, &fc, &fn, &ft)
                                  : NULL;
         struct dvm_object *o = dvm__obj(vm, r[B4(u0)]);
         if (!o) {
            dvm__throw(vm, "java/lang/NullPointerException",
                       "iput %s.%s:%s at %s.%s@%04x",
                       fc && fc->name ? fc->name : "?", fn ? fn : "?",
                       ft ? ft : "?", m->cls ? m->cls->name : "?", m->name, pc);
            goto exception;
         }
         union dvm_value v = { 0 };
         if (op == 0x5a) { REQ(A4(u0) + 1); v.ju = rw(r, A4(u0)); }
         else v.u = r[A4(u0)];
         if (!f) {
            sdk_trace_unresolved_field(fc ? fc->name : NULL, fn, ft);
            dvm__throw(vm, "java/lang/NoSuchFieldError", "%s.%s:%s", fc ? fc->name : "?", fn ? fn : "?", ft ? ft : "?");
            goto exception;
         }
         if (f && o->slots && f->slot < (o->cls ? o->cls->islots : 0)) {
            char *rb = NULL; size_t rl = 0; bool rd = false;
            if (name_range_hit(vm, &o->slots[f->slot], sizeof(union dvm_value), &rb, &rl, &rd))
               name_guard_report("iput", vm, o->cls, fn, f->slot, v,
                                 &o->slots[f->slot], rb, rl, rd);
            if (f->kind == 'L') GC_STORE_REF(vm,&o->slots[f->slot].l,v.l,"iput-object");
            else o->slots[f->slot] = v;
         }
         if (vm->trace && trace_this && op == 0x5b)
            fprintf(stderr, "[dvm] %*siput-object @%x.%s:%s <- @%x\n",
                    vm->depth * 2, "", r[B4(u0)], fn ? fn : "?",
                    ft ? ft : "?", v.l);
         fr->pc += 2;
         break;
      }

      /* --- static fields --- */
      case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: {
         REQ(AA(u0));
         struct dvm_class *fc = NULL; const char *fn = NULL, *ft = NULL;
         struct dvm_field *f = dd ? resolve_field(vm, dd, IU(1), true, &fc, &fn, &ft)
                                  : NULL;
         union dvm_value v = { 0 };
         if (f) {
            /* A <clinit> that throws leaves the class erroneous and its static
             * fields unset; reading one anyway hands the app a null it can
             * only fail on later, far from the cause. */
            if (!dvm_init_class(vm, f->cls)) goto exception;
            if (f->cls->sslots && f->slot < f->cls->nsslots)
               v = f->cls->sslots[f->slot];
            /* Dex static that the host publishes after attach (e.g.
             * UnityPlayer.currentActivity).  A null sslot here is not "no
             * field" — ask the external hook before handing out null. */
            if (op == 0x62 && !v.l && fc && fn && ft &&
                !strcmp(fn, "currentActivity") &&
                vm->hooks.get_external_static) {
               union dvm_value host = { 0 };
               if (gc_external_static(vm,fc->name,fn,ft,&host) && host.l) {
                  v = host;
                  if (f->cls->sslots && f->slot < f->cls->nsslots) {
                     char *rb = NULL; size_t rl = 0; bool rd = false;
                     if (name_range_hit(vm, &f->cls->sslots[f->slot], sizeof(union dvm_value), &rb, &rl, &rd))
                        name_guard_report("sget_host_backfill", vm, f->cls, fn, f->slot, v,
                                          &f->cls->sslots[f->slot], rb, rl, rd);
                     GC_STORE_REF(vm,&f->cls->sslots[f->slot].l,v.l,"external_static_backfill");
                  }
               }
               if(vm->exception)goto exception;
            }
         } else if (fc && fn && vm->hooks.get_external_static) {
            /* Nothing defines this class — not a dex, not a builtin, not the
             * host stubs.  A device raises NoClassDefFoundError here, and app
             * code guards for it: AppsFlyer probes for Xiaomi's install
             * referrer exactly this way.  Answering null instead turns a
             * handled absence into an NPE one call later. */
            if (!gc_external_static(vm,fc->name,fn,ft,&v)) {
               if(!vm->exception)dvm__throw(vm, "java/lang/NoClassDefFoundError", "%s.%s",
                          fc->name, fn);
               goto exception;
            }
         }
         if (vm->trace && trace_this && op == 0x62)
            fprintf(stderr, "[dvm] %*ssget-object %s.%s:%s -> @%x\n",
                    vm->depth * 2, "", fc ? fc->name : "?", fn ? fn : "?",
                    ft ? ft : "?", v.l);
         if (op == 0x61) { REQ(AA(u0) + 1); sw(r, AA(u0), v.ju); }
         else r[AA(u0)] = v.u;
         fr->pc += 2;
         break;
      }
      case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: {
         REQ(AA(u0));
         struct dvm_class *fc; const char *fn, *ft;
         struct dvm_field *f = dd ? resolve_field(vm, dd, IU(1), true, &fc, &fn, &ft)
                                  : NULL;
         union dvm_value v = { 0 };
         if (op == 0x68) { REQ(AA(u0) + 1); v.ju = rw(r, AA(u0)); }
         else v.u = r[AA(u0)];
         if (f) {
            if (!dvm_init_class(vm, f->cls)) goto exception;
            if (f->cls->sslots && f->slot < f->cls->nsslots) {
               char *rb = NULL; size_t rl = 0; bool rd = false;
               if (name_range_hit(vm, &f->cls->sslots[f->slot], sizeof(union dvm_value), &rb, &rl, &rd))
                  name_guard_report("sput", vm, f->cls, fn, f->slot, v,
                                    &f->cls->sslots[f->slot], rb, rl, rd);
               if (f->kind == 'L') GC_STORE_REF(vm,&f->cls->sslots[f->slot].l,v.l,"sput-object");
               else f->cls->sslots[f->slot] = v;
            }
         }
         if (!f) {
            sdk_trace_unresolved_field(fc ? fc->name : NULL, fn, ft);
            dvm__throw(vm, "java/lang/NoSuchFieldError", "%s.%s:%s", fc ? fc->name : "?", fn ? fn : "?", ft ? ft : "?");
            goto exception;
         }
         if (vm->trace && trace_this && op == 0x69)
            fprintf(stderr, "[dvm] %*ssput-object %s.%s:%s <- @%x\n",
                    vm->depth * 2, "", fc ? fc->name : "?", fn ? fn : "?",
                    ft ? ft : "?", v.l);
         fr->pc += 2;
         break;
      }

      /* --- invokes --- */
      case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
      case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
         bool range = (op >= 0x74);
         uint8_t kind = range ? (uint8_t)(op - 0x74 + 0x6e) : op;
         uint32_t midx = IU(1);

         struct dvm_class *cls = NULL;
         const char *name = NULL;
         struct dvm_method *target =
            dd ? resolve_method(vm, dd, midx, &cls, &name, &sigbuf) : NULL;

         if (vm->trace >= 2 && trace_this)
            fprintf(stderr, "[dvm]     invoke %s.%s%s\n",
                    cls ? cls->name : "?", name ? name : "?", sigbuf);

         uint32_t slots[256];
         int n = gather_args(insns, nins, pc, range, r, nregs, slots);

         bool is_static = (kind == 0x71);
         dvm_ref self = 0;
         const uint32_t *argslots = slots;
         int nargslots = n;
         if (!is_static) {
            if (n < 1) {
               dvm__throw(vm, "java/lang/VerifyError", "invoke with no receiver");
               goto exception;
            }
            self = slots[0];
            argslots = slots + 1;
            nargslots = n - 1;
            if (!self) {
               /* The method index pins which constant-pool entry resolved to
                * this name, which matters when the resolution itself looks
                * wrong. */
               dvm__throw(vm, "java/lang/NullPointerException",
                          "receiver %s.%s meth@%u (insns@0x%lx w0=%04x w1=%04x)",
                          cls ? cls->name : "?", name ? name : "?", midx,
                          dd ? (unsigned long)((const uint8_t *)insns - dd->file.p)
                             : 0UL,
                          (unsigned)insns[pc], (unsigned)IU(1));
               goto exception;
            }
         }

         /* invoke-super searches from the calling class's direct superclass,
          * even when the method reference names a more distant ancestor.
          * Interface super calls retain their declared interface target. */
         if (kind == 0x6f && cls && !(cls->access & DEX_ACC_INTERFACE) &&
             m->cls && m->cls->super)
            target = dvm_find_method(vm, m->cls->super, name, sigbuf);

         /* invoke-virtual / -interface re-dispatch on the receiver. */
         if (target && (kind == 0x6e || kind == 0x72))
            target = virtual_target(vm, self, target);
         else if (!target && self && (kind == 0x6e || kind == 0x72)) {
            /* The declared type carries no such method — an interface whose
             * definition is not in the dex, or one modelled here without it.
             * Dispatch is on the receiver, so look there before giving up and
             * handing the call to the host stubs, which would answer null.
             * Iterable.iterator() reached this path constantly: the sequence
             * types Kotlin's split() builds implement it, but the interface
             * itself declares nothing the VM can see. */
            struct dvm_object *ro = dvm__obj(vm, self);
            if (ro && ro->cls)
               target = dvm_find_method(vm, ro->cls, name, sigbuf);
         }

         union dvm_value ret;
         bool ok;
         if (target) {
            ok = invoke(vm, target, self, argslots, nargslots, &ret);
         } else if (cls && class_absent(vm, cls)) {
            /* Same as new-instance: the call names a class the APK does not
             * carry and the platform does not have.  Answering it with a stub
             * that returns zero tells the app the optional component is there
             * and it keeps using it. */
            dvm__throw(vm, "java/lang/NoClassDefFoundError", "%s",
                       cls->name ? cls->name : "?");
            goto exception;
         } else if (cls && name) {
            ++vm->depth;
            ok = dvm__call_out(vm, cls, name, sigbuf,
                          is_static, false, self, argslots, nargslots, &ret);
            --vm->depth;
            ok = ok && !vm->exception;
         } else {
            memset(&ret, 0, sizeof ret);
            ok = true;
         }
         if (!ok || vm->exception) goto exception;

         fr->result = ret.ju;
         fr->pc += 3;   /* 35c and 3rc are both three code units */
         break;
      }

      /* --- unary --- */
      case 0x7b: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = (uint32_t)(-(int32_t)r[B4(u0)]); fr->pc += 1; break;
      case 0x7c: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = ~r[B4(u0)]; fr->pc += 1; break;
      case 0x7d: REQ(A4(u0) + 1); REQ(B4(u0) + 1); sw(r, A4(u0), (uint64_t)(-(int64_t)rw(r, B4(u0)))); fr->pc += 1; break;
      case 0x7e: REQ(A4(u0) + 1); REQ(B4(u0) + 1); sw(r, A4(u0), ~rw(r, B4(u0))); fr->pc += 1; break;
      case 0x7f: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = f_to_bits(-bits_to_f(r[B4(u0)])); fr->pc += 1; break;
      case 0x80: REQ(A4(u0) + 1); REQ(B4(u0) + 1); sw(r, A4(u0), d_to_bits(-bits_to_d(rw(r, B4(u0))))); fr->pc += 1; break;

      case 0x81: REQ(A4(u0) + 1); REQ(B4(u0)); sw(r, A4(u0), (uint64_t)(int64_t)(int32_t)r[B4(u0)]); fr->pc += 1; break;
      case 0x82: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = f_to_bits((float)(int32_t)r[B4(u0)]); fr->pc += 1; break;
      case 0x83: REQ(A4(u0) + 1); REQ(B4(u0)); sw(r, A4(u0), d_to_bits((double)(int32_t)r[B4(u0)])); fr->pc += 1; break;
      case 0x84: REQ(A4(u0)); REQ(B4(u0) + 1); r[A4(u0)] = (uint32_t)rw(r, B4(u0)); fr->pc += 1; break;
      case 0x85: REQ(A4(u0)); REQ(B4(u0) + 1); r[A4(u0)] = f_to_bits((float)(int64_t)rw(r, B4(u0))); fr->pc += 1; break;
      case 0x86: REQ(A4(u0) + 1); REQ(B4(u0) + 1); sw(r, A4(u0), d_to_bits((double)(int64_t)rw(r, B4(u0)))); fr->pc += 1; break;
      case 0x87: { /* float-to-int, with Java's saturating semantics */
         REQ(A4(u0)); REQ(B4(u0));
         float f = bits_to_f(r[B4(u0)]);
         int32_t v;
         if (isnan(f)) v = 0;
         else if (f >= 2147483647.0f) v = INT32_MAX;
         else if (f <= -2147483648.0f) v = INT32_MIN;
         else v = (int32_t)f;
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 1;
         break;
      }
      case 0x88: {
         REQ(A4(u0) + 1); REQ(B4(u0));
         float f = bits_to_f(r[B4(u0)]);
         int64_t v;
         if (isnan(f)) v = 0;
         else if (f >= 9223372036854775807.0f) v = INT64_MAX;
         else if (f <= -9223372036854775808.0f) v = INT64_MIN;
         else v = (int64_t)f;
         sw(r, A4(u0), (uint64_t)v);
         fr->pc += 1;
         break;
      }
      case 0x89: REQ(A4(u0) + 1); REQ(B4(u0)); sw(r, A4(u0), d_to_bits((double)bits_to_f(r[B4(u0)]))); fr->pc += 1; break;
      case 0x8a: {
         REQ(A4(u0)); REQ(B4(u0) + 1);
         double f = bits_to_d(rw(r, B4(u0)));
         int32_t v;
         if (isnan(f)) v = 0;
         else if (f >= 2147483647.0) v = INT32_MAX;
         else if (f <= -2147483648.0) v = INT32_MIN;
         else v = (int32_t)f;
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 1;
         break;
      }
      case 0x8b: {
         REQ(A4(u0) + 1); REQ(B4(u0) + 1);
         double f = bits_to_d(rw(r, B4(u0)));
         int64_t v;
         if (isnan(f)) v = 0;
         else if (f >= 9223372036854775807.0) v = INT64_MAX;
         else if (f <= -9223372036854775808.0) v = INT64_MIN;
         else v = (int64_t)f;
         sw(r, A4(u0), (uint64_t)v);
         fr->pc += 1;
         break;
      }
      case 0x8c: REQ(A4(u0)); REQ(B4(u0) + 1); r[A4(u0)] = f_to_bits((float)bits_to_d(rw(r, B4(u0)))); fr->pc += 1; break;
      case 0x8d: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = (uint32_t)(int32_t)(int8_t)r[B4(u0)]; fr->pc += 1; break;
      case 0x8e: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = (uint16_t)r[B4(u0)]; fr->pc += 1; break;
      case 0x8f: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = (uint32_t)(int32_t)(int16_t)r[B4(u0)]; fr->pc += 1; break;

      /* --- binary op vAA, vBB, vCC --- */
      case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
      case 0x96: case 0x97: case 0x98: case 0x99: case 0x9a: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         int32_t a = (int32_t)r[bc & 0xff], b = (int32_t)r[bc >> 8];
         int32_t v = 0;
         switch (op) {
            case 0x90: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0x91: v = (int32_t)((uint32_t)a - (uint32_t)b); break;
            case 0x92: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0x93:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
               break;
            case 0x94:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? 0 : a % b;
               break;
            case 0x95: v = a & b; break;
            case 0x96: v = a | b; break;
            case 0x97: v = a ^ b; break;
            case 0x98: v = (int32_t)((uint32_t)a << (b & 31)); break;
            case 0x99: v = a >> (b & 31); break;
            default:   v = (int32_t)((uint32_t)a >> (b & 31)); break;
         }
         r[AA(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }
      case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: case 0xa0:
      case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: {
         REQ(AA(u0) + 1);
         uint16_t bc = IU(1);
         bool shift = (op >= 0xa3);
         REQ((bc & 0xff) + 1);
         REQ(shift ? (uint32_t)(bc >> 8) : (uint32_t)((bc >> 8) + 1));
         int64_t a = (int64_t)rw(r, bc & 0xff);
         int64_t b = shift ? (int32_t)r[bc >> 8] : (int64_t)rw(r, bc >> 8);
         int64_t v = 0;
         switch (op) {
            case 0x9b: v = (int64_t)((uint64_t)a + (uint64_t)b); break;
            case 0x9c: v = (int64_t)((uint64_t)a - (uint64_t)b); break;
            case 0x9d: v = (int64_t)((uint64_t)a * (uint64_t)b); break;
            case 0x9e:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT64_MIN && b == -1) ? INT64_MIN : a / b;
               break;
            case 0x9f:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT64_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xa0: v = a & b; break;
            case 0xa1: v = a | b; break;
            case 0xa2: v = a ^ b; break;
            case 0xa3: v = (int64_t)((uint64_t)a << (b & 63)); break;
            case 0xa4: v = a >> (b & 63); break;
            default:   v = (int64_t)((uint64_t)a >> (b & 63)); break;
         }
         sw(r, AA(u0), (uint64_t)v);
         fr->pc += 2;
         break;
      }
      case 0xa6: case 0xa7: case 0xa8: case 0xa9: case 0xaa: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         float a = bits_to_f(r[bc & 0xff]), b = bits_to_f(r[bc >> 8]), v;
         switch (op) {
            case 0xa6: v = a + b; break;
            case 0xa7: v = a - b; break;
            case 0xa8: v = a * b; break;
            case 0xa9: v = a / b; break;
            default:   v = fmodf(a, b); break;
         }
         r[AA(u0)] = f_to_bits(v);
         fr->pc += 2;
         break;
      }
      case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: {
         REQ(AA(u0) + 1);
         uint16_t bc = IU(1);
         REQ((bc & 0xff) + 1); REQ((bc >> 8) + 1);
         double a = bits_to_d(rw(r, bc & 0xff)), b = bits_to_d(rw(r, bc >> 8)), v;
         switch (op) {
            case 0xab: v = a + b; break;
            case 0xac: v = a - b; break;
            case 0xad: v = a * b; break;
            case 0xae: v = a / b; break;
            default:   v = fmod(a, b); break;
         }
         sw(r, AA(u0), d_to_bits(v));
         fr->pc += 2;
         break;
      }

      /* --- binary op/2addr --- */
      case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5:
      case 0xb6: case 0xb7: case 0xb8: case 0xb9: case 0xba: {
         REQ(A4(u0)); REQ(B4(u0));
         int32_t a = (int32_t)r[A4(u0)], b = (int32_t)r[B4(u0)];
         int32_t v = 0;
         switch (op) {
            case 0xb0: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0xb1: v = (int32_t)((uint32_t)a - (uint32_t)b); break;
            case 0xb2: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0xb3:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
               break;
            case 0xb4:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xb5: v = a & b; break;
            case 0xb6: v = a | b; break;
            case 0xb7: v = a ^ b; break;
            case 0xb8: v = (int32_t)((uint32_t)a << (b & 31)); break;
            case 0xb9: v = a >> (b & 31); break;
            default:   v = (int32_t)((uint32_t)a >> (b & 31)); break;
         }
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 1;
         break;
      }
      case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: case 0xc0:
      case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: {
         bool shift = (op >= 0xc3);
         REQ(A4(u0) + 1);
         REQ(shift ? (uint32_t)B4(u0) : (uint32_t)(B4(u0) + 1));
         int64_t a = (int64_t)rw(r, A4(u0));
         int64_t b = shift ? (int32_t)r[B4(u0)] : (int64_t)rw(r, B4(u0));
         int64_t v = 0;
         switch (op) {
            case 0xbb: v = (int64_t)((uint64_t)a + (uint64_t)b); break;
            case 0xbc: v = (int64_t)((uint64_t)a - (uint64_t)b); break;
            case 0xbd: v = (int64_t)((uint64_t)a * (uint64_t)b); break;
            case 0xbe:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT64_MIN && b == -1) ? INT64_MIN : a / b;
               break;
            case 0xbf:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT64_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xc0: v = a & b; break;
            case 0xc1: v = a | b; break;
            case 0xc2: v = a ^ b; break;
            case 0xc3: v = (int64_t)((uint64_t)a << (b & 63)); break;
            case 0xc4: v = a >> (b & 63); break;
            default:   v = (int64_t)((uint64_t)a >> (b & 63)); break;
         }
         sw(r, A4(u0), (uint64_t)v);
         fr->pc += 1;
         break;
      }
      case 0xc6: case 0xc7: case 0xc8: case 0xc9: case 0xca: {
         REQ(A4(u0)); REQ(B4(u0));
         float a = bits_to_f(r[A4(u0)]), b = bits_to_f(r[B4(u0)]), v;
         switch (op) {
            case 0xc6: v = a + b; break;
            case 0xc7: v = a - b; break;
            case 0xc8: v = a * b; break;
            case 0xc9: v = a / b; break;
            default:   v = fmodf(a, b); break;
         }
         r[A4(u0)] = f_to_bits(v);
         fr->pc += 1;
         break;
      }
      case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: {
         REQ(A4(u0) + 1); REQ(B4(u0) + 1);
         double a = bits_to_d(rw(r, A4(u0))), b = bits_to_d(rw(r, B4(u0))), v;
         switch (op) {
            case 0xcb: v = a + b; break;
            case 0xcc: v = a - b; break;
            case 0xcd: v = a * b; break;
            case 0xce: v = a / b; break;
            default:   v = fmod(a, b); break;
         }
         sw(r, A4(u0), d_to_bits(v));
         fr->pc += 1;
         break;
      }

      /* --- literal ops --- */
      case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4:
      case 0xd5: case 0xd6: case 0xd7: {
         REQ(A4(u0)); REQ(B4(u0));
         int32_t a = (int32_t)r[B4(u0)];
         int32_t b = (int16_t)IU(1);
         int32_t v = 0;
         switch (op) {
            case 0xd0: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0xd1: v = (int32_t)((uint32_t)b - (uint32_t)a); break;  /* rsub */
            case 0xd2: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0xd3:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
               break;
            case 0xd4:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xd5: v = a & b; break;
            case 0xd6: v = a | b; break;
            default:   v = a ^ b; break;
         }
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }
      case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd:
      case 0xde: case 0xdf: case 0xe0: case 0xe1: case 0xe2: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff);
         int32_t a = (int32_t)r[bc & 0xff];
         int32_t b = (int8_t)(uint8_t)(bc >> 8);
         int32_t v = 0;
         switch (op) {
            case 0xd8: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0xd9: v = (int32_t)((uint32_t)b - (uint32_t)a); break;
            case 0xda: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0xdb:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
               break;
            case 0xdc:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xdd: v = a & b; break;
            case 0xde: v = a | b; break;
            case 0xdf: v = a ^ b; break;
            case 0xe0: v = (int32_t)((uint32_t)a << (b & 31)); break;
            case 0xe1: v = a >> (b & 31); break;
            default:   v = (int32_t)((uint32_t)a >> (b & 31)); break;
         }
         r[AA(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }

      /* --- invoke-polymorphic / -custom, const-method-*: these need a
       * MethodHandle runtime.  Java 8 lambdas in an APK are desugared by d8
       * into ordinary classes, so this only shows up in code that reflects on
       * method handles.  Skip the call and yield zero rather than derail the
       * whole method. */
      case 0xfa: case 0xfb: fr->pc += 4; fr->result = 0; break;
      case 0xfc: case 0xfd: fr->pc += 3; fr->result = 0; break;
      case 0xfe: case 0xff: REQ(AA(u0)); r[AA(u0)] = 0; fr->pc += 2; break;

      default:
         dvm__throw(vm, "java/lang/VerifyError", "%s.%s: opcode %02x at %04x",
                    m->cls->name, m->name, op, pc);
         goto exception;
      }
      continue;

exception:
      {
         /* Process death is not a Java exception that guest code can catch
          * and restart.  Unwind to the host thread owner for joining. */
         if (dvm_is_stopping(vm)) return false;
         if (!vm->exception)
            dvm__throw(vm, "java/lang/Error", "internal: no exception object");
         struct dvm_object *eo = dvm__obj(vm, vm->exception);
         uint32_t handler;
         /* A parked thread is not a failure the app can handle: it is a
          * thread that would be waiting inside the platform right now.  The
          * unwind therefore passes catch clauses by and ends the thread — a
          * dispatcher's `catch (InterruptedException e) { continue; }` would
          * otherwise put it straight back on the empty queue it just parked
          * on, and spin for the whole time slice. */
         if (vm->parked) {
            if (find_catchall_handler(m, fr->pc, &handler) == 0) {
               vm->exc_ref = vm->exception;
               vm->exception = 0;
               fr->pc = handler;
               continue;
            }
            return false;
         }
         if (find_handler(vm, m, fr->pc, eo ? eo->cls : NULL, &handler) == 0) {
            if(task6j_diagnostic_catch)task6j_diagnostic_catch(vm,m,fr->pc,handler);
#ifdef LUNARIA_UNITY_PLAYER_PROBE
            if(m->cls&&!strcmp(m->cls->name,"com/gsc/base/d")&&!strcmp(m->name,"h")){static unsigned caught_routes;if(caught_routes++<8){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){char desc[512];dvm_describe_exception(vm,vm->exception,desc,sizeof desc);dprintf(fileno(f),"SDK_NOTIFY_ROUTE_CAUGHT pc=%u handler=%u detail=%s\n",fr->pc,handler,desc);for(int i=0;i<vm->nexc_trace&&i<8;i++){struct dvm_method *em=vm->exc_trace[i];dprintf(fileno(f),"SDK_NOTIFY_ROUTE_FRAME index=%d class=%s method=%s sig=%s\n",i,em&&em->cls?em->cls->name:"?",em?em->name:"?",em&&em->sig?em->sig:"?");}fclose(f);}errno=saved;}}
#endif
            /* Dalvik clears the pending exception when control reaches the
             * handler; move-exception only *retrieves* it, and dx omits that
             * instruction entirely when the catch variable is unused.  Leaving
             * the exception set meant such a handler ran with the throw still
             * pending, so the first call it made returned "exception" and the
             * caught error escaped anyway — `catch (ClassNotFoundException e)`
             * blocks that merely log were the common shape.  Keep the object
             * reachable for a move-exception that does follow. */
            vm->exc_ref = vm->exception;
            vm->exception = 0;
            fr->pc = handler;
            continue;
         }
         return false;
      }
   }
#undef REQ
#undef THROW_AND_DISPATCH
}
static _Thread_local struct dvm_method *g_builtin_method;

static bool annotation_value_to_dvm(struct dvm *vm, struct dex_file *dex,
                                    const struct dex_value *value,
                                    const char *expected,
                                    union dvm_value *out)
{
   memset(out, 0, sizeof *out);
   if (!value) return false;
   switch (value->type) {
      case DEX_VALUE_NULL:
         return true;
      case DEX_VALUE_BOOLEAN: case DEX_VALUE_BYTE: case DEX_VALUE_SHORT:
      case DEX_VALUE_CHAR: case DEX_VALUE_INT: case DEX_VALUE_LONG:
      case DEX_VALUE_FLOAT: case DEX_VALUE_DOUBLE:
         out->ju = value->bits;
         return true;
      case DEX_VALUE_STRING: {
         const char *s = dex_string(dex, (uint32_t)value->bits);
         out->l = s ? dvm__intern(vm, s) : 0;
         return s != NULL;
      }
      case DEX_VALUE_TYPE: {
         const char *desc = dex_type(dex, (uint32_t)value->bits);
         out->l = desc ? class_object(vm, dvm__class_by_desc(vm, desc)) : 0;
         return desc != NULL;
      }
      case DEX_VALUE_ENUM: {
         struct dex_field_id field;
         if (!dex_field_id(dex, (uint32_t)value->bits, &field)) return false;
         const char *desc = dex_type(dex, field.class_idx);
         const char *name = dex_string(dex, field.name_idx);
         struct dvm_class *cls = desc ? dvm__class_by_desc(vm, desc) : NULL;
         if (!cls || !name) return false;
         if (!dvm_init_class(vm, cls)) return false;
         return dvm_get_static(vm, cls, name, desc, out);
      }
      case DEX_VALUE_ANNOTATION: {
         if (value->bits > UINT32_MAX) return false;
         const char *desc = dex_annotation_type(dex, (uint32_t)value->bits);
         struct dvm_class *cls = desc ? dvm__class_by_desc(vm, desc) : NULL;
         dvm_ref ref = cls ? dvm_new_object(vm, cls) : 0;
         struct dvm_object *obj = dvm__obj(vm, ref);
         if (!obj) return false;
         obj->annotation_dex = dex;
         obj->annotation_off = (uint32_t)value->bits;
         out->l = ref;
         return true;
      }
      case DEX_VALUE_ARRAY: {
         if (!expected || expected[0] != '[') return false;
         int count = dex_value_array(dex, value, NULL, 0);
         if (count < 0) return false;
         struct dex_value *items = count
            ? calloc((size_t)count, sizeof *items) : NULL;
         if (count && !items) return false;
         if (count && dex_value_array(dex, value, items, count) != count) {
            free(items);
            return false;
         }
         char element_kind = expected[1] == '[' || expected[1] == 'L'
            ? 'L' : expected[1];
         dvm_ref array = dvm_new_array(vm, element_kind, expected + 1,
                                       (uint32_t)count);
         struct dvm_object *obj = dvm__obj(vm, array);
         if (!obj) { free(items); return false; }
         for (int i = 0; i < count; ++i) {
            union dvm_value item;
            if (!annotation_value_to_dvm(vm, dex, &items[i], expected + 1,
                                         &item)) {
               free(items);
               return false;
            }
            switch (element_kind) {
               case 'Z': case 'B':
                  ((uint8_t *)obj->data)[i] = (uint8_t)item.u;
                  break;
               case 'C': case 'S':
                  ((uint16_t *)obj->data)[i] = (uint16_t)item.u;
                  break;
               case 'J': case 'D':
                  ((uint64_t *)obj->data)[i] = item.ju;
                  break;
               default:
                  ((uint32_t *)obj->data)[i] = item.u;
                  break;
            }
         }
         free(items);
         out->l = array;
         return true;
      }
      default:
         return false;
   }
}

static bool annotation_element_builtin(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)args;
   (void)nargs;
   struct dvm_method *method = g_builtin_method;
   struct dvm_object *obj = dvm__obj(vm, self);
   struct dex_value value;
   if (!method || !method->name || !method->sig || !obj ||
       !obj->annotation_dex || !obj->annotation_off) {
      dvm__throw(vm, "java/lang/annotation/IncompleteAnnotationException",
                 "annotation element has no encoded value");
      return false;
   }
   /* An encoded value is offsets into the dex it was read from.  An element
    * the use site left out takes the default declared on the annotation
    * type, which may live in another dex of a multidex APK; decoding it
    * against the use site's dex read unrelated bytes as nested arrays. */
   struct dex_file *value_dex = obj->annotation_dex;
   bool found = dex_annotation_element(obj->annotation_dex,
                                       obj->annotation_off,
                                       method->name, &value);
   if (!found && method->cls && method->cls->dex) {
      found = dex_annotation_default(method->cls->dex,
                                     method->cls->class_def_idx,
                                     method->name, &value);
      value_dex = method->cls->dex;
   }
   if (!found) {
      dvm__throw(vm, "java/lang/annotation/IncompleteAnnotationException",
                 "%s.%s", method->cls && method->cls->name
                    ? method->cls->name : "?", method->name);
      return false;
   }
   const char *returns = strchr(method->sig, ')');
   if (!returns || !annotation_value_to_dvm(vm, value_dex, &value,
                                            returns + 1, out)) {
      dvm__throw(vm, "java/lang/AnnotationFormatError", "%s.%s",
                 method->cls && method->cls->name ? method->cls->name : "?",
                 method->name);
      return false;
   }
   return true;
}

static uint64_t monitor_thread_token(struct dvm *vm)
{
   /* Guest threads can share a host executor or migrate between executors.
    * Ownership belongs to the Java Thread identity, never the host TLS. */
   return dvm__current_thread_id(vm);
}

static uintptr_t monitor_channel(dvm_ref ref, unsigned kind)
{
   return ((uintptr_t)ref << 2) | (uintptr_t)kind;
}

static uint64_t monitor_now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

bool dvm__monitor_try_enter(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   if (!o) {
      dvm__throw(vm, "java/lang/NullPointerException", "monitor-enter null");
      return false;
   }
   const uint64_t me = monitor_thread_token(vm);
   if (!me) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "monitor thread identity");
      return false;
   }
   if (o->monitor_owner && o->monitor_owner != me) return false;
   o->monitor_owner = me;
   ++o->monitor_depth;
   return true;
}

bool dvm__monitor_enter(struct dvm *vm, dvm_ref ref)
{
   if(leak_scope_current)leak_scope_monitor(vm,ref);
   for (;;) {
      if (dvm_is_stopping(vm)) return false;
      if (dvm__monitor_try_enter(vm, ref)) return true;
      if (vm->exception) return false;
      /* Notification is the mechanism.  The long timeout only permits state
       * validation if a producer disappears; it never grants ownership. */
      dvm_ref thread = dvm__current_thread(vm);
      dvm__thread_set_state(vm, thread, DVM_THREAD_BLOCKED);
      dvm_gil_wait_for(vm, monitor_channel(ref, 1), 60000u);
      dvm__thread_set_state(vm, thread, DVM_THREAD_RUNNABLE);
   }
}

bool dvm__monitor_exit(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   const uint64_t me = monitor_thread_token(vm);
   if (!o || o->monitor_owner != me || !o->monitor_depth) {
      dvm__throw(vm, "java/lang/IllegalMonitorStateException",
                 "monitor-exit without ownership");
      return false;
   }
   if (--o->monitor_depth) return true;
   o->monitor_owner = 0;
   dvm_gil_notify_one_for(monitor_channel(ref, 1));
   return true;
}

bool dvm__monitor_wait(struct dvm *vm, dvm_ref ref, uint64_t timeout_ms,
                       bool *notified)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   const uint64_t me = monitor_thread_token(vm);
   if (!o || o->monitor_owner != me || !o->monitor_depth) {
      dvm__throw(vm, "java/lang/IllegalMonitorStateException",
                 "wait without monitor ownership");
      return false;
   }
   if (dvm__thread_take_interrupt(vm)) {
      dvm__throw(vm, "java/lang/InterruptedException", "wait interrupted");
      return false;
   }
   const uint32_t saved_depth = o->monitor_depth;
   const uint64_t seen = o->monitor_seq;
   const uint64_t deadline = timeout_ms ? monitor_now_ms() + timeout_ms : 0;
   o->monitor_owner = 0;
   o->monitor_depth = 0;
   dvm_gil_notify_one_for(monitor_channel(ref, 1));

   bool signalled = false, interrupted = false;
   for (;;) {
      if (dvm_is_stopping(vm)) return false;
      uint64_t now = monitor_now_ms();
      if (deadline && now >= deadline) break;
      uint64_t left = deadline ? deadline - now : 60000u;
      if (left > 60000u) left = 60000u;
      dvm_ref thread = dvm__current_thread(vm);
      dvm__thread_set_state(vm, thread, deadline ? DVM_THREAD_TIMED_WAITING : DVM_THREAD_WAITING);
      dvm__thread_set_wait(vm, monitor_channel(ref, 2));
      dvm_gil_wait_for(vm, monitor_channel(ref, 2), (unsigned)left);
      dvm__thread_set_wait(vm, 0);
      dvm__thread_set_state(vm, thread, DVM_THREAD_RUNNABLE);
      o = dvm__obj(vm, ref);
      if (!o) {
         dvm__throw(vm, "java/lang/NullPointerException", "waited object freed");
         return false;
      }
      if (o->monitor_seq != seen) { signalled = true; break; }
      if (dvm__thread_take_interrupt(vm)) { interrupted = true; break; }
   }
   if (!dvm__monitor_enter(vm, ref)) return false;
   o = dvm__obj(vm, ref);
   o->monitor_depth = saved_depth;
   if (notified) *notified = signalled;
   /* The monitor is owned again before the interrupt is reported, as
    * Object.wait() specifies. */
   if (interrupted) {
      dvm__throw(vm, "java/lang/InterruptedException", "wait interrupted");
      return false;
   }
   return true;
}

bool dvm__monitor_notify(struct dvm *vm, dvm_ref ref, bool all)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   if (!o || o->monitor_owner != monitor_thread_token(vm) || !o->monitor_depth) {
      dvm__throw(vm, "java/lang/IllegalMonitorStateException",
                 "notify without monitor ownership");
      return false;
   }
   ++o->monitor_seq;
   if (all) dvm_gil_notify_for(monitor_channel(ref, 2));
   else     dvm_gil_notify_one_for(monitor_channel(ref, 2));
   return true;
}

bool dvm__monitor_state(struct dvm *vm, dvm_ref ref, bool current,
                        uint32_t *depth)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   if (depth) *depth = o ? o->monitor_depth : 0;
   if (!o || !o->monitor_owner) return false;
   return !current || o->monitor_owner == monitor_thread_token(vm);
}

struct dvm_method *dvm__builtin_method(void) { return g_builtin_method; }

void dvm__warn_placeholder(struct dvm *vm)
{
   struct dvm_method *m = g_builtin_method;
   if (!m || m->placeholder_warned) return;
   m->placeholder_warned = true;
   /* Name the caller too.  "Paint.<init> is a no-op" is a fact about the
    * emulator; "…and app code X called it" is the only half that can explain
    * what X does next, and it is the reason this is reported at call time
    * rather than when the method table was built. */
   struct dvm_method *from = vm ? vm->cur_method : NULL;
   fprintf(stderr, "[dvm] placeholder no-op invoked: %s.%s%s (from %s.%s)\n",
           m->cls && m->cls->name ? m->cls->name : "?",
           m->name ? m->name : "?", m->sig ? m->sig : "",
           from && from->cls && from->cls->name ? from->cls->name : "?",
           from && from->name ? from->name : "?");
}

static bool invoke_body(struct dvm *vm, struct dvm_method *m, dvm_ref self,
                        const uint32_t *slots, int nslots, union dvm_value *out);


/* LUNARIA_DVM_TRACE: what a traced method returned, next to the "call" line
 * that printed its arguments.  A String shows its text, any other object its
 * class, and a collection its size — enough to tell an empty result from a
 * missing one without single-stepping the callee. */
static void trace_return(struct dvm *vm, const struct dvm_method *m, char kind,
                         const union dvm_value *v)
{
   const char *cls = m && m->cls && m->cls->name ? m->cls->name : "?";
   const char *name = m && m->name ? m->name : "?";
   if (kind == 'V') {
      fprintf(stderr, "[dvm] %*sreturn %s.%s\n", vm->depth * 2, "", cls, name);
      return;
   }
   if (kind == 'J') {
      fprintf(stderr, "[dvm] %*sreturn %s.%s -> %lld (wide)\n", vm->depth * 2,
              "", cls, name, (long long)v->j);
      return;
   }
   if (strchr("ZBCSI", kind)) {
      fprintf(stderr, "[dvm] %*sreturn %s.%s -> %d\n", vm->depth * 2, "", cls,
              name, v->i);
      return;
   }
   if (kind == 'F' || kind == 'D') {
      fprintf(stderr, "[dvm] %*sreturn %s.%s -> %.17g (%s)\n", vm->depth * 2,
              "", cls, name, kind == 'F' ? (double)v->f : v->d,
              kind == 'F' ? "float" : "double");
      return;
   }
   dvm_ref r = (dvm_ref)v->u;
   struct dvm_object *o = r ? dvm__obj(vm, r) : NULL;
   if (!o) {
      fprintf(stderr, "[dvm] %*sreturn %s.%s -> null\n", vm->depth * 2, "",
              cls, name);
   } else if (o->kind == DVM_OBJ_STRING) {
      fprintf(stderr, "[dvm] %*sreturn %s.%s -> \"%.200s\" (%u bytes)\n",
              vm->depth * 2, "", cls, name, o->utf8 ? o->utf8 : "",
              (unsigned)o->utf8_len);
   } else {
      fprintf(stderr, "[dvm] %*sreturn %s.%s -> @%x %s\n", vm->depth * 2, "",
              cls, name, (unsigned)r,
              o->cls && o->cls->name ? o->cls->name : "?");
   }
}

static bool trace_call_enabled(struct dvm *vm, const struct dvm_method *m,
                                const struct dvm_call_frame *caller)
{
   if (!vm->trace) return false;
   const char *filter = dvm__env_trace_class();
   if (!filter) return true;
   if (m && m->cls && m->cls->name && strstr(m->cls->name, filter)) return true;
   return caller && caller->method && caller->method->cls &&
      caller->method->cls->name && strstr(caller->method->cls->name, filter);
}

static void call_histogram(const struct dvm_method *m);
extern void task6_trace_event(struct dvm *,struct dvm_method *,dvm_ref,const uint32_t *,int,const union dvm_value *,int) __attribute__((weak));
extern void login_panel_trace(struct dvm *,struct dvm_method *,dvm_ref,const uint32_t *,int,const union dvm_value *,int) __attribute__((weak));
extern void login_panel_before_call(struct dvm *,struct dvm_method *,dvm_ref,const uint32_t *,int) __attribute__((weak));

/* Independent finite JSON construction and parsing operations. Delegating
 * overloads share their nearest same-kind ancestor's slice; no VM ABI field
 * or process-global ownership flag is needed. */
static int json_method_slice_kind(const struct dvm_method *m)
{
   if(!m||!m->cls||!m->name||!m->sig||strcmp(m->cls->name,"copy/google/json/JSON"))return 0;
   if(!strcmp(m->name,"<init>")&&(!strcmp(m->sig,"()V")||
      !strcmp(m->sig,"(Lcopy/google/json/internal/Excluder;Lcopy/google/json/FieldNamingStrategy;Ljava/util/Map;ZZZZZZZLcopy/google/json/LongSerializationPolicy;Ljava/lang/String;IILjava/util/List;Ljava/util/List;Ljava/util/List;)V")))return 1;
   if(!strcmp(m->name,"fromJson")&&(!strcmp(m->sig,"(Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/Object;")||
      !strcmp(m->sig,"(Ljava/lang/String;Ljava/lang/reflect/Type;)Ljava/lang/Object;")||
      !strcmp(m->sig,"(Ljava/io/Reader;Ljava/lang/Class;)Ljava/lang/Object;")))return 2;
   return 0;
}

static bool invoke(struct dvm *vm, struct dvm_method *m, dvm_ref self,
                   const uint32_t *slots, int nslots, union dvm_value *out)
{
   struct gil_site_scope gs __attribute__((cleanup(gil_site_leave)))=gil_site_enter(m&&m->cls?m->cls->name:NULL);
   struct gil_deep_scope ds __attribute__((cleanup(gil_deep_leave)))={0};gil_deep_enter(&ds,vm,m);
   struct emu_profile_scope es __attribute__((cleanup(emu_profile_leave)))=emu_profile_enter(m);
   struct perf_scope perf_scope __attribute__((cleanup(perf_vm_leave)))=perf_vm_enter(m&&m->cls?m->cls->name:NULL,m?m->name:NULL,m?m->sig:NULL);
   perf_vm_install(&perf_scope);
   struct gc_exec_entry entry={.vm=vm,.self=self,.sig=m?m->sig:NULL,.slots=slots,.nslots=nslots,.next=gc_exec_entries};
   gc_exec_entries=&entry;
   if(leak_scope_current)leak_scope_method(vm,m,self,slots,nslots);
   /* Every pre/post raw-reference callback needs its own identity/shape check. */
   /* Each actual observer is certified independently at its call boundary. */
   lifetime_call(vm,m);
   if(task6_trace_event){leak_observer_check(vm,LEAK_TASK_PRE,(uintptr_t)task6_trace_event,m,self,slots,nslots,(const void*)vm->hooks.call_external,NULL);task6_trace_event(vm,m,self,slots,nslots,NULL,1);}
   if(login_panel_before_call){leak_observer_check(vm,LEAK_PANEL_PRE,(uintptr_t)login_panel_before_call,m,self,slots,nslots,(const void*)vm->hooks.call_external,NULL);login_panel_before_call(vm,m,self,slots,nslots);}
#ifdef LUNARIA_UNITY_PLAYER_PROBE
   static unsigned sdk_notify_serial;
   static bool sdk_notify_login_started;
   unsigned sdk_notify_id=0;
   const char *sdk_owner=m&&m->cls?m->cls->name:NULL;
   const char *sdk_method=m?m->name:NULL;
   static unsigned json_parse_serial;unsigned json_parse_id=0;uint64_t json_parse_start=vm->call_steps;
   struct json_hot_entry *json_hot_before=NULL;
   if(sdk_owner&&sdk_method&&!strcmp(sdk_owner,"copy/google/json/JSON")&&!strcmp(sdk_method,"fromJson")&&m->sig&&!strcmp(m->sig,"(Ljava/lang/String;Ljava/lang/reflect/Type;)Ljava/lang/Object;")&&nslots==2&&json_parse_serial<48){json_parse_id=++json_parse_serial;int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){struct dvm_object *type=dvm__obj(vm,slots[1]);const char *name=type&&type->kind==DVM_OBJ_CLASS&&type->klass?type->klass->name:type&&type->cls?type->cls->name:"null";const char *data=dvm_string_utf8(vm,slots[0]);dprintf(fileno(f),"SDK_NOTIFY_JSON id=%u stage=enter bytes=%zu type=%s steps=%llu thread=%u text=<redacted>\n",json_parse_id,(size_t)dvm_string_utf8_length(vm,slots[0]),name,(unsigned long long)vm->call_steps,vm->cur_thread);fclose(f);}errno=saved;}
   static unsigned canonicalize_serial;unsigned canonicalize_id=0;
   if(sdk_owner&&sdk_method&&!strcmp(sdk_owner,"okhttp3/HttpUrl")&&!strcmp(sdk_method,"canonicalize")&&nslots==9&&canonicalize_serial<48){canonicalize_id=++canonicalize_serial;int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){dprintf(fileno(f),"SDK_CANON id=%u stage=enter ref=%u utf8_length=%zu start=%u end=%u call_steps=%llu thread=%u\n",canonicalize_id,slots[0],(size_t)dvm_string_utf8_length(vm,slots[0]),slots[1],slots[2],(unsigned long long)vm->call_steps,vm->cur_thread);fclose(f);}errno=saved;}
   bool sdk_init_event=sdk_owner&&sdk_method&&((!strcmp(sdk_owner,"com/gsc/pub/GSCPubCommon")&&(!strcmp(sdk_method,"init")||!strcmp(sdk_method,"a")||!strcmp(sdk_method,"initInternal")||!strcmp(sdk_method,"initInternalV2")))||(!strncmp(sdk_owner,"com/gsc/pub/GSCPubCommon$",sizeof("com/gsc/pub/GSCPubCommon$")-1)&&(!strcmp(sdk_method,"onSuccess")||!strcmp(sdk_method,"onFailed")||!strcmp(sdk_method,"onError")||!strcmp(sdk_method,"onInitSuccess")||!strcmp(sdk_method,"onInitFailed")))||(!strcmp(sdk_owner,"java/util/concurrent/atomic/AtomicBoolean")&&!strcmp(sdk_method,"set")));
   if(sdk_owner&&sdk_method&&!strcmp(sdk_method,"login")&&(!strcmp(sdk_owner,"com/bilibili/star/bili/Sdk")||!strcmp(sdk_owner,"com/bilibili/star/bili/UnityAgent")))sdk_notify_login_started=true;
   sdk_init_event=sdk_init_event||(sdk_owner&&sdk_method&&((!strcmp(sdk_owner,"com/gsc/base/c"))||(!strncmp(sdk_owner,"com/gsc/base/c$",sizeof("com/gsc/base/c$")-1))||(strstr(sdk_owner,"CallbackManager")!=NULL)||(!strcmp(sdk_owner,"com/gsc/pub/GSCPubCommon$b")&&!strcmp(sdk_method,"callback"))||(!strncmp(sdk_owner,"com/gsc/base/service/b",sizeof("com/gsc/base/service/b")-1))||(!strncmp(sdk_owner,"com/base/router/",16)&&(!strcmp(sdk_method,"navigation")||!strcmp(sdk_method,"_navigation")||!strcmp(sdk_method,"startActivity")||!strcmp(sdk_method,"runInMainThread")))||(!strcmp(sdk_owner,"com/base/annotation/model/RouteMeta")&&!strcmp(sdk_method,"getDestination"))));
   sdk_init_event=sdk_init_event||(sdk_owner&&strstr(sdk_owner,"InitAgreementRouteService")!=NULL);
   bool sdk_notify=sdk_owner&&sdk_method&&sdk_notify_serial<384&&(sdk_init_event||(sdk_notify_login_started&&
      ((!strcmp(sdk_owner,"com/bilibili/star/bili/Sdk")&&
        (!strcmp(sdk_method,"initialize")||!strcmp(sdk_method,"login")||!strcmp(sdk_method,"accountCallBack")||!strcmp(sdk_method,"buglyInit")))||
       (!strcmp(sdk_owner,"com/bilibili/star/bili/ASdk")&&(!strcmp(sdk_method,"initialize")||!strcmp(sdk_method,"login")))||
       (!strcmp(sdk_owner,"com/gsc/pub/GSCPubCommon")&&(!strcmp(sdk_method,"init")||!strcmp(sdk_method,"login")||!strcmp(sdk_method,"applicationAttach")||!strcmp(sdk_method,"a")))||
       (!strcmp(sdk_owner,"com/base/cachelib/CacheInstaller")&&(!strcmp(sdk_method,"init")||!strcmp(sdk_method,"getInstance")||!strcmp(sdk_method,"getGlobeContext")))||
       (!strcmp(sdk_owner,"com/gsc/pub/utils/b")&&!strcmp(sdk_method,"c"))||
       (!strcmp(sdk_owner,"com/gsc/pub/b")&&(!strcmp(sdk_method,"onError")||!strcmp(sdk_method,"onSuccess")))||
       (!strncmp(sdk_owner,"com/base/router/",16)&&(!strcmp(sdk_method,"navigation")||!strcmp(sdk_method,"_navigation")||!strcmp(sdk_method,"startActivity")||!strcmp(sdk_method,"runInMainThread")||!strcmp(sdk_method,"onLost")||!strcmp(sdk_method,"onInterrupt")||!strcmp(sdk_method,"onArrival")))||
       (!strcmp(sdk_owner,"com/base/annotation/model/RouteMeta")&&(!strcmp(sdk_method,"getDestination")||!strcmp(sdk_method,"getType")))||
       (!strcmp(sdk_owner,"com/gsc/base/d")&&(!strcmp(sdk_method,"onFinish")||!strcmp(sdk_method,"h")))||
       (!strncmp(sdk_owner,"com/bilibili/star/bili/Sdk$",27)&&!strcmp(sdk_method,"run"))||
       (!strcmp(sdk_owner,"com/bilibili/star/bili/UnityAgent")&&
        (!strcmp(sdk_method,"login")||!strcmp(sdk_method,"SendMessageToUnity")||!strcmp(sdk_method,"unity3dSendMessage")))||
       (!strcmp(sdk_owner,"com/unity3d/player/UnityPlayer")&&
        (!strcmp(sdk_method,"UnitySendMessage")||!strcmp(sdk_method,"nativeUnitySendMessage"))))));
   if(sdk_notify){
      sdk_notify_id=++sdk_notify_serial;int saved=errno;
      FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
      if(f&&!strcmp(sdk_owner,"com/gsc/route_service/service/InitAgreementRouteService")&&!strcmp(sdk_method,"onFailure")&&nslots==2){const char *why=dvm_string_utf8(vm,slots[0]);dprintf(fileno(f),"SDK_NOTIFY_AGREEMENT_FAILURE code=%d message=%.512s\n",(int32_t)slots[1],why?why:"null");}
      if(f){dprintf(fileno(f),"SDK_NOTIFY id=%u stage=enter class=%s method=%s sig=%s self=%u java_thread=%u mono_ms=%llu exception=%u\n",sdk_notify_id,sdk_owner,sdk_method,m->sig?m->sig:"?",self,vm->cur_thread,(unsigned long long)dvm__now_ms(),vm->exception);fclose(f);}errno=saved;
   }
#endif
#ifdef LUNARIA_UNITY_PLAYER_PROBE
   if(json_parse_id){json_hot_before=malloc(sizeof json_hot.entries);if(json_hot_before)memcpy(json_hot_before,json_hot.entries,sizeof json_hot.entries);}
#endif
   /* Give real JSON setup/parsing a bounded independent operation slice.
    * Calls of the same category under an existing frame share its budget.
    * Construction and parsing are separate categories and may nest. */
   uint64_t parser_outer_steps=vm->call_steps,parser_outer_limit=vm->step_limit;
   int json_slice_kind=json_method_slice_kind(m);
   bool json_slice_shared=false;
   for(const struct dvm_call_frame *f=vm->invoke_frame;json_slice_kind&&f;f=f->previous)
      if(json_method_slice_kind(f->method)==json_slice_kind){json_slice_shared=true;break;}
   bool parser_slice=json_slice_kind&&!json_slice_shared&&parser_outer_limit;
   const char *json_slice_label=json_slice_kind==1?"ctor":"parse";
   if(parser_slice)vm->step_limit=parser_outer_steps>UINT64_MAX-200000?UINT64_MAX:parser_outer_steps+200000;
   if(parser_slice){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/json-method-slices.txt","a");if(f){dprintf(fileno(f),"JSON_METHOD_SLICE pid=%d stage=enter kind=%s method=%s sig=%s outer_steps=%llu outer_limit=%llu allowance=200000 thread=%u\n",getpid(),json_slice_label,m->name,m->sig,(unsigned long long)parser_outer_steps,(unsigned long long)parser_outer_limit,vm->cur_thread);fclose(f);}errno=saved;}
   call_histogram(m);
#ifdef LUNARIA_UNITY_PLAYER_PROBE
   struct task_profile_frame profile_frame;
   task_profile_enter(vm,m,&profile_frame);
#endif
   struct dvm_call_frame frame = { .method = m, .previous = vm->invoke_frame };
   struct dvm_method *caller = vm->cur_method;
   const uint32_t caller_pc = vm->cur_pc;
   vm->invoke_frame = &frame;
#ifdef LUNARIA_UNITY_PLAYER_PROBE
   bool profile_response=m->cls&&!strcmp(m->cls->name,"okhttp3/ResponseBody")&&!strcmp(m->name,"string")&&!strcmp(m->sig,"()Ljava/lang/String;");
   if(profile_response)task_profile_report(vm,"response_body_string_enter");
#endif
   extern bool string_runtime_okio_buffer(struct dvm *,dvm_ref,const char *,const char *,const uint32_t *,int,union dvm_value *);
   extern bool string_runtime_okio_read_buffer(struct dvm *,dvm_ref,const char *,const char *,const uint32_t *,int,union dvm_value *);
   extern bool string_runtime_response_read(struct dvm *,const char *,dvm_ref,const char *,const char *,const uint32_t *,int,union dvm_value *);
   struct dvm_gc_native_scope fast_scope={0};
   bool possible_fast=m->cls&&(!strcmp(m->cls->name,"okio/Buffer")||
      !strcmp(m->cls->name,"okio/RealBufferedSource")||!strcmp(m->cls->name,"okhttp3/ResponseBody")||
      !strcmp(m->cls->name,"okhttp3/HttpUrl")||!strcmp(m->cls->name,"okhttp3/Headers")||
      !strcmp(m->cls->name,"okhttp3/Headers$Builder")||!strcmp(m->cls->name,"copy/google/json/stream/JsonReader"));
   bool fast_scope_ok=!possible_fast||dvm_gc_native_enter(vm,&fast_scope,self,m->sig,slots,nslots);
   bool okio_read=fast_scope_ok&&m->cls&&!strcmp(m->cls->name,"okio/Buffer")&&string_runtime_okio_read_buffer(vm,self,m->name,m->sig,slots,nslots,out);
   bool response_read=fast_scope_ok&&!okio_read&&m->cls&&string_runtime_response_read(vm,m->cls->name,self,m->name,m->sig,slots,nslots,out);
   if(okio_read||response_read){static _Thread_local unsigned read_logs;unsigned hit=++read_logs;if(hit<=96||hit%1024==0){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/okio-read-native.txt","a");if(f){dprintf(fileno(f),"OKIO_READ_NATIVE pid=%d thread=%u owner=%s method=%s%s calls=%u exception=%u call_steps=%llu\n",getpid(),vm->cur_thread,m->cls->name,m->name,m->sig,hit,vm->exception,(unsigned long long)vm->call_steps);fclose(f);}errno=saved;}}
   bool okio_buffer=fast_scope_ok&&m->cls&&!strcmp(m->cls->name,"okio/Buffer")&&string_runtime_okio_buffer(vm,self,m->name,m->sig,slots,nslots,out);
   if(okio_buffer){static struct {const struct dvm_method *method;unsigned hits;} counters[16];unsigned i;for(i=0;i<16;i++)if(!counters[i].method||counters[i].method==m)break;if(i<16){counters[i].method=m;unsigned hit=++counters[i].hits;if(hit<=4||hit%1024==0){FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/okio-native.txt","a");if(f){fprintf(f,"OKIO_NATIVE pid=%d method=%s%s count=%u self=%u thread=%u exception=%u steps=%llu\n",getpid(),m->name,m->sig,hit,self,vm->cur_thread,vm->exception,(unsigned long long)vm->call_steps);fclose(f);}}}}
   extern bool string_runtime_okio_utf8(struct dvm *,dvm_ref,const uint32_t *,int,union dvm_value *);
   extern bool string_runtime_okio_byte(struct dvm *,dvm_ref,const uint32_t *,int,union dvm_value *);
   bool okio_byte=fast_scope_ok&&m->cls&&!strcmp(m->cls->name,"okio/Buffer")&&!strcmp(m->name,"writeByte")&&!strcmp(m->sig,"(I)Lokio/Buffer;")&&string_runtime_okio_byte(vm,self,slots,nslots,out);
   bool okio_utf8=fast_scope_ok&&!okio_buffer&&m->cls&&!strcmp(m->cls->name,"okio/Buffer")&&!strcmp(m->name,"writeUtf8")&&!strcmp(m->sig,"(Ljava/lang/String;II)Lokio/Buffer;")&&string_runtime_okio_utf8(vm,self,slots,nslots,out);
   bool url_ascii=fast_scope_ok&&m->cls&&!strcmp(m->cls->name,"okhttp3/HttpUrl")&&!strcmp(m->name,"canonicalize")&&!strcmp(m->sig,"(Ljava/lang/String;IILjava/lang/String;ZZZZLjava/nio/charset/Charset;)Ljava/lang/String;")&&string_runtime_httpurl_ascii(vm,slots,nslots,out);
   bool header_ascii=fast_scope_ok&&m->cls&&!strcmp(m->cls->name,"okhttp3/Headers")&&!strcmp(m->name,"checkValue")&&!strcmp(m->sig,"(Ljava/lang/String;Ljava/lang/String;)V")&&string_runtime_header_value_ascii(vm,slots,nslots);
   union dvm_value header_list={0};bool header_absent=m->cls&&!strcmp(m->cls->name,"okhttp3/Headers$Builder")&&!strcmp(m->name,"removeAll")&&!strcmp(m->sig,"(Ljava/lang/String;)Lokhttp3/Headers$Builder;")&&nslots==1&&dvm_get_field(vm,self,"namesAndValues","Ljava/util/List;",&header_list)&&string_runtime_header_absent(vm,header_list.l,slots[0]);
   if(header_absent)out->l=self;
   bool json_core=fast_scope_ok&&m->cls&&!strcmp(m->cls->name,"copy/google/json/stream/JsonReader")&&string_runtime_json_core(vm,self,m->name,m->sig,slots,nslots,out);
   bool json_quoted=fast_scope_ok&&!json_core&&m->cls&&!strcmp(m->cls->name,"copy/google/json/stream/JsonReader")&&nslots==1&&((!strcmp(m->name,"nextQuotedValue")&&!strcmp(m->sig,"(C)Ljava/lang/String;"))||(!strcmp(m->name,"skipQuotedValue")&&!strcmp(m->sig,"(C)V")))&&string_runtime_json_quoted(vm,self,(uint16_t)slots[0],!strcmp(m->name,"skipQuotedValue"),out);
   bool json_nonwhite=fast_scope_ok&&m->cls&&!strcmp(m->cls->name,"copy/google/json/stream/JsonReader")&&nslots==1&&!strcmp(m->name,"nextNonWhitespace")&&!strcmp(m->sig,"(Z)I")&&string_runtime_json_nonwhite(vm,self,out);
   bool json_structure=fast_scope_ok&&m->cls&&!strcmp(m->cls->name,"copy/google/json/stream/JsonReader")&&nslots==0&&!strcmp(m->name,"doPeek")&&!strcmp(m->sig,"()I")&&string_runtime_json_structure(vm,self,out);
   bool handled=okio_read||response_read||okio_buffer||okio_byte||okio_utf8||url_ascii||header_ascii||header_absent||json_core||json_quoted||json_nonwhite||json_structure;
   dvm_gc_native_leave_result(&fast_scope,m->sig,handled&&out?out->l:0);
   bool ok = fast_scope_ok && (handled?!vm->exception:invoke_body(vm, m, self, slots, nslots, out));
#ifdef LUNARIA_UNITY_PLAYER_PROBE
   if(json_parse_id){json_hot_report(json_parse_id,json_hot_before);free(json_hot_before);}
   if(json_parse_id){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){char detail[512]={0};if(vm->exception)dvm_describe_exception(vm,vm->exception,detail,sizeof detail);dprintf(fileno(f),"SDK_NOTIFY_JSON id=%u stage=exit ok=%d steps=%llu delta=%llu thread=%u exception=%.512s\n",json_parse_id,ok,(unsigned long long)vm->call_steps,(unsigned long long)(vm->call_steps-json_parse_start),vm->cur_thread,detail);fclose(f);}errno=saved;}
   {static unsigned http_logged;bool response=sdk_owner&&sdk_method&&!strcmp(sdk_owner,"okhttp3/Response$Builder")&&!strcmp(sdk_method,"build");bool body=sdk_owner&&sdk_method&&!strcmp(sdk_owner,"okhttp3/ResponseBody")&&!strcmp(sdk_method,"string");if(ok&&!vm->exception&&(response||body)&&http_logged++<32){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){if(response){union dvm_value code={0},request={0},url={0},text={0};dvm_get_field(vm,out->l,"code","I",&code);dvm_get_field(vm,out->l,"request","Lokhttp3/Request;",&request);if(request.l)dvm_get_field(vm,request.l,"url","Lokhttp3/HttpUrl;",&url);if(url.l)dvm_get_field(vm,url.l,"url","Ljava/lang/String;",&text);const char *address=dvm_string_utf8(vm,text.l);dprintf(fileno(f),"NET_JAVA_HTTP status=%d response=%u url_length=%zu thread=%u\n",code.i,out->l,address?strlen(address):0,vm->cur_thread);}else{const char *text=dvm_string_utf8(vm,out->l);dprintf(fileno(f),"NET_JAVA_HTTP_BODY ref=%u bytes=%zu text=<redacted> thread=%u\n",self,(size_t)dvm_string_utf8_length(vm,out->l),vm->cur_thread);}fclose(f);}errno=saved;}}
   if(canonicalize_id){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){dprintf(fileno(f),"SDK_CANON id=%u stage=exit ok=%d exception=%u call_steps=%llu thread=%u\n",canonicalize_id,ok,vm->exception,(unsigned long long)vm->call_steps,vm->cur_thread);fclose(f);}errno=saved;}
   {static unsigned types_logged;bool type_call=sdk_owner&&sdk_method&&((!strcmp(sdk_owner,"copy/google/json/internal/$Gson$Types")&&!strcmp(sdk_method,"getCollectionElementType"))||(!strcmp(sdk_owner,"java/lang/Class")&&(!strcmp(sdk_method,"getGenericInterfaces")||!strcmp(sdk_method,"getGenericSuperclass"))));if(type_call&&types_logged++<48){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){struct dvm_object *receiver=dvm__obj(vm,self),*result=dvm__obj(vm,out->l);dprintf(fileno(f),"SDK_NOTIFY_GENERIC owner=%s method=%s self=%u represented=%s input0=%u input1=%u ok=%d exception=%u result=%u result_class=%s represented_result=%s array_length=%u\n",sdk_owner,sdk_method,self,receiver&&receiver->kind==DVM_OBJ_CLASS&&receiver->klass?receiver->klass->name:"-",nslots?slots[0]:0,nslots>1?slots[1]:0,ok,vm->exception,out->l,result&&result->cls?result->cls->name:"-",result&&result->kind==DVM_OBJ_CLASS&&result->klass?result->klass->name:"-",result&&result->kind==DVM_OBJ_ARRAY?dvm_array_length(vm,out->l):0);fclose(f);}errno=saved;}}
   if(sdk_notify){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
      if(f&&ok&&!strcmp(sdk_owner,"com/base/annotation/model/RouteMeta")&&!strcmp(sdk_method,"getDestination")){struct dvm_object *dest=dvm__obj(vm,out->l);dprintf(fileno(f),"SDK_NOTIFY_DEST id=%u ref=%u class=%s\n",sdk_notify_id,out->l,dest&&dest->kind==DVM_OBJ_CLASS&&dest->klass?dest->klass->name:"null_or_nonclass");}
      if(f&&!ok&&vm->exception){char desc[512];dvm_describe_exception(vm,vm->exception,desc,sizeof desc);dprintf(fileno(f),"SDK_NOTIFY_EXCEPTION id=%u detail=%s\n",sdk_notify_id,desc);for(int i=0;i<vm->nexc_trace&&i<8;i++){struct dvm_method *em=vm->exc_trace[i];dprintf(fileno(f),"SDK_NOTIFY_FRAME id=%u index=%d class=%s method=%s sig=%s pc=%u\n",sdk_notify_id,i,em&&em->cls?em->cls->name:"?",em?em->name:"?",em&&em->sig?em->sig:"?",vm->exc_pc);}}
      if(f){dprintf(fileno(f),"SDK_NOTIFY id=%u stage=exit class=%s method=%s ok=%d exception=%u bool_result=%d mono_ms=%llu\n",sdk_notify_id,sdk_owner,sdk_method,ok,vm->exception,m->sig&&strstr(m->sig,")Z")?(int)out->i:-1,(unsigned long long)dvm__now_ms());if(!strcmp(sdk_owner,"com/base/cachelib/CacheInstaller"))dprintf(fileno(f),"SDK_NOTIFY_CACHE pid=%d vm_serial=%llu method=%s self=%u arg0=%u result_ref=%u mono_ms=%llu\n",(int)getpid(),(unsigned long long)vm->serial,sdk_method,self,nslots?slots[0]:0,out->l,(unsigned long long)dvm__now_ms());fclose(f);}errno=saved;}
#endif
   if (ok && m && trace_call_enabled(vm, m, frame.previous))
      trace_return(vm, m, m->ret_kind, out);
   vm->invoke_frame = frame.previous;
   /* A returned bytecode frame is no longer the current frame. Retaining it
    * misattributes later builtin exceptions and can outlive the method. */
   vm->cur_method = caller;
   vm->cur_pc = caller_pc;
   /* Recheck post identity and exact shape; pre success grants no exemption. */
   if(task6_trace_event){leak_observer_check(vm,LEAK_TASK_POST,(uintptr_t)task6_trace_event,m,self,slots,nslots,(const void*)vm->hooks.call_external,NULL);task6_trace_event(vm,m,self,slots,nslots,out,ok?0:-1);}
   if(login_panel_trace){leak_observer_check(vm,LEAK_PANEL_POST,(uintptr_t)login_panel_trace,m,self,slots,nslots,(const void*)vm->hooks.call_external,NULL);login_panel_trace(vm,m,self,slots,nslots,out,ok&&!vm->exception);}
   if(parser_slice){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/json-method-slices.txt","a");if(f){dprintf(fileno(f),"JSON_METHOD_SLICE pid=%d stage=return kind=%s method=%s sig=%s used=%llu ok=%d exception=%u thread=%u\n",getpid(),json_slice_label,m->name,m->sig,(unsigned long long)(vm->call_steps-parser_outer_steps),ok,vm->exception,vm->cur_thread);fclose(f);}errno=saved;vm->call_steps=parser_outer_steps;vm->step_limit=parser_outer_limit;}
#ifdef LUNARIA_UNITY_PLAYER_PROBE
   task_profile_leave(vm,&profile_frame);
#endif
   gc_exec_entry_leave(&entry);
   return ok;
}

static bool invoke_body(struct dvm *vm, struct dvm_method *m, dvm_ref self,
                        const uint32_t *slots, int nslots, union dvm_value *out)
{
   memset(out, 0, sizeof *out);
   if (dvm_is_stopping(vm)) return false;
   if (!m) return true;

   /* The return slot is defined before the callee runs.
    *
    * Dalvik's result register always holds a value: a void method leaves it
    * zero, and a method returning int leaves the upper half of the wide view
    * zero rather than whatever was there.  Here it is a `union dvm_value ret`
    * declared on the interpreter's own stack at every call site, and the
    * builtin path writes only as many bytes as the return type has -- RETI
    * writes four, RETV writes none -- so without this the caller's
    * `fr->result` carries the bytes that happened to be in that stack slot.
    * move-result truncates and hides it; anything that reads the wide value,
    * or a builtin that returns nothing at all, does not.  That is how a 400
    * from getResponseCode() reached Volley as 0xb021_0000_0190.
    *
    * dvm__call_out() already did this for the calls that leave the VM; doing
    * it here covers every callee kind at the one point they share. */
   memset(out, 0, sizeof *out);

   /* Java and native calls share this host thread's stack. Leave room to
    * construct the exception and unwind rather than limiting Java depth. */
   size_t stack_remaining = luna_os_stack_remaining();
   if (stack_remaining < 64u * 1024u ||
       (stack_remaining == SIZE_MAX && vm->depth >= DVM_MAX_FRAMES)) {
      if (getenv("LUNARIA_DVM_STACK")) {
         unsigned depth = 0;
         for (const struct dvm_call_frame *f = vm->invoke_frame;
              f && depth <= (unsigned)vm->depth + 1u; f = f->previous, ++depth) {
            const struct dvm_method *method = f->method;
            fprintf(stderr, "[dvm-stack] %u %s.%s%s\n", depth,
               method && method->cls ? method->cls->name : "?",
               method ? method->name : "?", method && method->sig ? method->sig : "");
         }
      }
      dvm__throw(vm, "java/lang/StackOverflowError", "%d frames", vm->depth);
      return false;
   }

   /* Every call passes through here: test the first character before comparing names. */
   if (nslots >= 2 && m->name && m->name[0] == 'u' && m->sig &&
       !strcmp(m->name, "uncaughtException") &&
       !strcmp(m->sig, "(Ljava/lang/Thread;Ljava/lang/Throwable;)V"))
      log_forwarded_uncaught(vm, (dvm_ref)slots[1], m);

   bool is_static = (m->access & DEX_ACC_STATIC) != 0;
   if (!is_static && !self && !(m->access & DEX_ACC_NATIVE)) {
      /* "self" marks the callee-side check, so a null receiver can be told
       * apart from the call-site one below. */
      dvm__throw(vm, "java/lang/NullPointerException", "self %s.%s%s",
                 m->cls->name, m->name, m->sig ? m->sig : "");
      return false;
   }

   /* A class is initialised by its first static call or its first
    * construction.  A host that builds an object through JNI (AllocObject +
    * constructor, as the process Application is) never executes new-instance,
    * so its <clinit> -- System.loadLibrary of the app's own natives, say --
    * has to run here, before the constructor. */
   if (m->cls && (is_static || (m->name && m->name[0] == '<' && !strcmp(m->name, "<init>"))) &&
       !dvm_init_class(vm, m->cls)) return false;

   const bool synchronized = (m->access & DEX_ACC_SYNCHRONIZED) != 0;
   dvm_ref monitor = 0;
   if (synchronized) {
      monitor = is_static ? class_object(vm, m->cls) : self;
      if (!dvm__monitor_enter(vm, monitor)) return false;
   }

   bool trace_this = trace_call_enabled(vm, m,
      vm->invoke_frame ? vm->invoke_frame->previous : NULL);
   if (vm->trace && trace_this) {
      fprintf(stderr, "[dvm] %*scall %s.%s%s", vm->depth * 2, "",
              m->cls ? m->cls->name : "?", m->name, m->sig ? m->sig : "");
      if (self) fprintf(stderr, " on @%x", (unsigned)self);
      fputc('\n', stderr);
      /* What the reference arguments are: the declared type says little
       * when the caller passes a subclass (which Throwable reached an error
       * handler, which listener was registered). */
      char one[256];
      for (int i = 0, slot = 0; m->sig && slot < nslots &&
           dvm__sig_param(m->sig, i, one, sizeof one); ++i) {
         const bool wide = one[0] == 'J' || one[0] == 'D';
         if ((one[0] == 'L' || one[0] == '[') && slots[slot]) {
            struct dvm_object *o = dvm__obj(vm, (dvm_ref)slots[slot]);
            char desc[384] = "";
            if (o && o->cls && o->kind != DVM_OBJ_STRING)
               dvm_describe_exception(vm, (dvm_ref)slots[slot], desc,
                                      sizeof desc);
            fprintf(stderr, "[dvm] %*s  arg%d = %s%s%s\n", vm->depth * 2, "", i,
                    o && o->kind == DVM_OBJ_STRING ? "\""
                       : o && o->cls && o->cls->name ? o->cls->name : "?",
                    o && o->kind == DVM_OBJ_STRING
                       ? (dvm_string_utf8(vm, (dvm_ref)slots[slot]) ?: "") : "",
                    o && o->kind == DVM_OBJ_STRING ? "\"" : "");
            if (desc[0] && (strstr(desc, "Exception") || strstr(desc, "Error")))
               fprintf(stderr, "[dvm] %*s    (%s)\n", vm->depth * 2, "", desc);
         } else if (one[0] != 'L' && one[0] != '[') {
            union dvm_value value = {0};
            value.ju = slots[slot];
            if (wide && slot + 1 < nslots) value.ju |= (uint64_t)slots[slot + 1] << 32;
            if (one[0] == 'F')
               fprintf(stderr, "[dvm] %*s  arg%d = %.9g (float)\n", vm->depth * 2, "", i, (double)value.f);
            else if (one[0] == 'D')
               fprintf(stderr, "[dvm] %*s  arg%d = %.17g (double)\n", vm->depth * 2, "", i, value.d);
            else if (one[0] == 'J')
               fprintf(stderr, "[dvm] %*s  arg%d = %lld (wide)\n", vm->depth * 2, "", i, (long long)value.j);
            else
               fprintf(stderr, "[dvm] %*s  arg%d = %d\n", vm->depth * 2, "", i, value.i);
         }
         slot += wide ? 2 : 1;
      }
   }

   if (m->builtin) {
      if(leak_scope_current)leak_scope_cancel(vm,LEAK_UNKNOWN_CALL);
      union dvm_value args[64];
      int nargs = slots_to_values(m->sig, slots, nslots, args, 64);
      ++vm->depth;
      struct dvm_method *previous_builtin = g_builtin_method;
      g_builtin_method = m;
      struct dvm_gc_native_scope scope;
      bool ok=dvm_gc_native_enter(vm,&scope,self,m->sig,slots,nslots);
      if(ok)ok=m->builtin(vm, self, args, nargs, out);
      dvm_gc_native_leave_result(&scope,m->sig,ok&&out?out->l:0);
      g_builtin_method = previous_builtin;
      --vm->depth;
      if (synchronized && !dvm__monitor_exit(vm, monitor)) ok = false;
      return ok && !vm->exception;
   }

   if (!m->has_code) {
      bool native = (m->access & DEX_ACC_NATIVE) != 0;
      ++vm->depth;
      bool ok = dvm__call_out(vm, m->cls, m->name, m->sig, is_static, native,
                         self, slots, nslots, out);
      --vm->depth;
      if (synchronized && !dvm__monitor_exit(vm, monitor)) ok = false;
      return ok && !vm->exception;
   }

   uint16_t nregs = m->code.registers_size;
   uint16_t nins = m->code.ins_size;
   if (nregs < nins) nregs = nins;

   uint32_t stackbuf[64];
   uint32_t *regs = nregs <= 64 ? stackbuf : native_scratch_acquire((size_t)nregs * sizeof *regs);
   if (!regs) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "%u registers", nregs);
      if (synchronized) (void)dvm__monitor_exit(vm, monitor);
      return false;
   }
   memset(regs, 0, (size_t)nregs * sizeof *regs);

   /* Parameters occupy the last ins_size registers, `this` first. */
   int base = (int)nregs - (int)nins;
   int w = base;
   if (!is_static && w < nregs) regs[w++] = self;
   for (int i = 0; i < nslots && w < nregs; ++i)
      regs[w++] = slots[i];

   struct frame fr = { .m = m, .regs = regs, .nregs = nregs, .pc = 0 };
   struct gc_exec_frame gc_frame={.vm=vm,.frame=&fr,.next=gc_exec_frames};gc_exec_frames=&gc_frame;
   ++vm->depth;
   bool pushed = vm->ncallstack <
                 (int)(sizeof vm->callstack / sizeof vm->callstack[0]);
   if (pushed) vm->callstack[vm->ncallstack++] = m;
   /* Guest bytecode must not append all its transient reads to a suspended
    * outer C caller (Looper.loop may remain on that C stack for hours).
    * Existing C borrows stay published; new C callees acquire their own scope
    * at this guest depth. Interpreter registers are separate execution roots. */
   ++gc_guest_execution_depth;
   bool ok = execute(vm, &fr, out);
   --gc_guest_execution_depth;
   gc_exec_frame_leave(&gc_frame);
   if (pushed) --vm->ncallstack;
   --vm->depth;

   /* Record the frames the exception passes on its way out, innermost first. */
   if (!ok && vm->exception && m != vm->exc_trace[0] &&
       vm->nexc_trace < (int)(sizeof vm->exc_trace / sizeof vm->exc_trace[0]))
      vm->exc_trace[vm->nexc_trace++] = m;

   if (regs != stackbuf) native_scratch_release(regs);
   if (synchronized && !dvm__monitor_exit(vm, monitor)) ok = false;
   return ok;
}

/* ------------------------------------------------------------------------ *
 * Public entry points
 * ------------------------------------------------------------------------ */

/* Runs the threads bytecode started during the call that just finished.  The
 * slice is deliberately small: a dispatcher loop that has drained its queue
 * would otherwise spin against the ordinary step limit and stall the frame. */
uint64_t dvm__now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Scheduler-only tracing.  LUNARIA_DVM_TRACE prints every bytecode call, which
 * is far too slow to reach a startup step several minutes in; this answers the
 * one question a blocked wait raises — what the queue was allowed to run. */
bool dvm__sched_trace(void)
{
   static int on = -1;
   if (on < 0) {
      const char *s = getenv("LUNARIA_DVM_SCHED");
      on = s && *s && strcmp(s, "0") ? 1 : 0;
   }
   return on == 1;
}

/* Volley parks its dispatchers on an empty queue and re-queues them every few
 * milliseconds, so an unfiltered scheduler trace is six hundred thousand lines
 * of the same two classes — enough fprintf to change what the run does, which
 * is the one thing a diagnostic must not do.  LUNARIA_DVM_SCHED=<substring>
 * keeps only the classes whose name contains it; =1 keeps everything. */
bool dvm__sched_trace_for(const char *class_name)
{
   if (!dvm__sched_trace()) return false;
   static const char *filter = (const char *)-1;
   if (filter == (const char *)-1) {
      const char *s = getenv("LUNARIA_DVM_SCHED");
      filter = (s && strcmp(s, "1") && strcmp(s, "on")) ? s : NULL;
   }
   if (!filter) return true;
   return class_name && strstr(class_name, filter) != NULL;
}

/* `nested` separates the two callers.  The drain at a call boundary is
 * housekeeping and must stay flat: letting it re-enter turns every Runnable
 * that starts another into unbounded recursion.  A blocking wait is the
 * opposite case for cooperative threads: their work may be in this queue.
 * Handler callbacks still belong to the waiting main thread and must remain
 * queued until its current dispatch returns to Looper.loop(). */
/* How late a due Runnable may run before it is worth reporting.  Two frames:
 * below that it is indistinguishable from a device's own scheduling jitter. */
#define DVM_LOOPER_LATE_MS 32u

static int drain_pending(struct dvm *vm, bool nested)
{
   if (dvm_is_stopping(vm)) return 0;
   gc_fast_debug_poll(vm);
   dvm_nsd_poll(vm);
   dvm_alarm_poll(vm);
   if (!vm->npending) return 0;
   if (!nested && vm->drain_depth) return 0;
   if (vm->drain_depth >= DVM_DRAIN_MAX_DEPTH) {
      if (dvm__sched_trace())
         fprintf(stderr, "[sched] drain refused: depth=%d pending=%d\n",
                 vm->drain_depth, vm->npending);
      return 0;
   }
   int ran = 0;
   ++vm->drain_depth;
   uint64_t saved_limit = vm->step_limit;
   bool saved_quiet = vm->quiet_uncaught;

   /* One entry is taken from the queue at a time, immediately before it runs.
    *
    * This used to detach every due entry up front and then run the batch.  A
    * Runnable that blocks then made the rest of its own batch unreachable: the
    * entries were already off the queue, so the nested drain its wait performs
    * could not see them.  Volley's NetworkDispatcher sat in exactly that hole
    * — dispatcher and requester were picked up by the same drain, the
    * requester ran first and waited, and the dispatcher that would have served
    * it was invisible until the wait had timed out (15 s, every time).
    *
    * `runs` bounds the work one drain does so a runnable that re-queues itself
    * with no delay cannot spin here forever. */
   for (int runs = 0; runs < DVM_DRAIN_MAX_RUNS; ++runs) {
      /* Oldest due entry first.  Taking one at a time means the choice is a
       * scheduling policy rather than an ordering within a batch, and
       * newest-first starves under it: a daemon that parks is re-queued at the
       * tail and would be picked again immediately, forever, while the work it
       * is waiting for sits in front of it.  Idle daemons carry
       * DVM_PARK_RETRY_MS, so they are not due most of the time and cannot
       * crowd out freshly started work either.  Entries whose delay has not
       * elapsed stay queued for a later drain. */
      uint64_t now = dvm__now_ms();
      int pick = -1;
      for (int i = 0; i < vm->npending; ++i) {
         /* Background Looper.loop() owns its tagged entries. */
         if (!vm->pending_is_thread[i] && vm->pending_looper[i])
            continue;
         /* The rest are the main Looper's, and only the main thread runs
          * them — whatever wait or call boundary brought another thread
          * here.  (Queued green threads belong to no Looper.) */
         if (!vm->pending_is_thread[i] && !dvm_on_main_thread())
            continue;
         if (!vm->pending_is_thread[i] && vm->main_transaction_depth)
            continue;
         /* sleep/yield/park do not run the caller's next Looper message.
          * A nested wait may advance cooperative threads, never Handler work
          * on the main thread whose current call is still on the stack. */
         if (nested && !vm->pending_is_thread[i])
            continue;
         if (vm->pending_due_ms[i] <= now) { pick = i; break; }
      }
      if (pick < 0) break;
      ++ran;

      dvm_ref entry = vm->pending_threads[pick];
      bool entry_is_thread = vm->pending_is_thread[pick];
      const uint64_t entry_due = vm->pending_due_ms[pick];
      for (int i = pick; i + 1 < vm->npending; ++i) {
         vm->pending_threads[i] = vm->pending_threads[i + 1];
         vm->pending_is_thread[i] = vm->pending_is_thread[i + 1];
         vm->pending_due_ms[i] = vm->pending_due_ms[i + 1];
         vm->pending_looper[i] = vm->pending_looper[i + 1];
         vm->pending_owner[i] = vm->pending_owner[i + 1];
         vm->pending_token[i] = vm->pending_token[i + 1];
      }
      --vm->npending;

      dvm_ref list[1] = { entry };
      bool is_thread[1] = { entry_is_thread };
      {
         const int i = 0;
         struct dvm_class *c = dvm_object_class(vm, list[i]);
         struct dvm_method *run = c ? dvm_find_method(vm, c, "run", "()V") : NULL;
         bool proxy_run = !run && c && c->super && c->super->name &&
                          !strcmp(c->super->name, "java/lang/reflect/Proxy");
         /* How late this looper is.
          *
          * A Handler.postDelayed() callback is a deadline the app may be
          * relying on, not a hint.  Cross Worlds' anti-tamper module arms one
          * (NmssSa.TDThreadWait -> Handler(mainLooper).postDelayed(->
          * nmssNativeSignalToTDThread, ms)) and its native side treats a
          * signal that does not arrive before its own timer expires as the app
          * having been frozen.  On a device the main looper is a thread of its
          * own and fires within a millisecond or two of the due time; here the
          * queue is drained once per pump frame and only when the guest is
          * between slices, so a long stretch of native code makes every armed
          * callback late.  That lateness is invisible unless it is measured,
          * and it is the difference between an emulator that a timing check
          * accepts and one it does not. */
         if (entry_due && now > entry_due + DVM_LOOPER_LATE_MS) {
            static int late_warned;
            if (late_warned++ < 64 || dvm__sched_trace())
               fprintf(stderr, "[sched] %s.run() ran %llu ms after its due "
                       "time (main-looper callback)\n",
                       run && run->cls ? run->cls->name : "?",
                       (unsigned long long)(now - entry_due));
         }
         if ((run && (run->has_code || run->builtin)) || proxy_run) {
            union dvm_value ret;
            uint64_t before = vm->steps;
            struct timespec looper_t0;
            clock_gettime(CLOCK_MONOTONIC, &looper_t0);
            /* The Runnable gets a slice of its own, so it starts from zero.
             * call_steps is only reset by dvm_call() at depth 0, and a drain
             * runs nested inside whatever call asked to wait — so without this
             * the Runnable inherits every step that call had already spent and
             * blows the slice on its first instruction.  It surfaced once
             * Thread.sleep() stopped holding the ARM execution lock: the Java
             * wait loops around it started really turning, and each turn
             * drains, so a long-running JNI call reached the limit within a
             * couple of Runnables. */
            uint64_t saved_steps = vm->call_steps;
            vm->call_steps = 0;
            /* Only a thread that fell back to this queue has a slice: it is the
             * cooperative scheduler's way of noticing that a green thread is
             * spinning rather than getting anywhere.  A main-Looper callback
             * is not a thread — on a device it simply runs on the main thread
             * until it returns, however long that takes.  Giving it the slice
             * killed ordinary work: the HoYoverse SDK's language table is a
             * Gson parse of a large JSON file, took more than two million
             * instructions, and was aborted with java.lang.Error, so every
             * SDK string lookup failed ("load resource failed"). */
            vm->step_limit = is_thread[i] ? DVM_THREAD_SLICE : 0;
            vm->quiet_uncaught = true;
            /* A Runnable handed to Handler.post() runs on the main thread; one
             * started with Thread.start() runs as itself.  App code asserts on
             * the difference, so Thread.currentThread() has to follow it. */
            dvm_ref saved_thread = vm->cur_thread;
            vm->cur_thread = is_thread[i] ? list[i] : vm->main_thread;
            bool ok;
            if (!is_thread[i]) dvm_runtime_looper_log(vm, 0, true, list[i]);
            if (proxy_run) {
               struct dvm_class *iface =
                  dvm__class_by_desc(vm, "Ljava/lang/Runnable;");
               ok = dvm_proxy_try_invoke(vm, list[i], iface, "run", "()V",
                                         NULL, 0, &ret) && !vm->exception;
            } else {
               ok = dvm_call(vm, run, list[i], NULL, 0, &ret);
            }
            vm->cur_thread = saved_thread;
            vm->quiet_uncaught = saved_quiet;
            vm->step_limit = saved_limit;
            vm->call_steps = saved_steps;
            /* LUNARIA_TRACE_LOOPER: where the main looper's time goes, by
             * callback class, every five seconds. */
            if (!is_thread[i]) {
               static int looper_trace = -1;
               static struct { const char *name; unsigned runs; uint64_t ns, steps; } top[24];
               static struct timespec looper_last;
               if (looper_trace < 0) looper_trace = getenv("LUNARIA_TRACE_LOOPER") != NULL;
               if (looper_trace) {
                  struct timespec t1;
                  clock_gettime(CLOCK_MONOTONIC, &t1);
                  const char *cname = c && c->name ? c->name : "?";
                  int slot = -1;
                  for (int k = 0; k < 24 && slot < 0; ++k)
                     if (top[k].name && !strcmp(top[k].name, cname)) slot = k;
                  for (int k = 0; k < 24 && slot < 0; ++k)
                     if (!top[k].name) { top[k].name = cname; slot = k; }
                  if (slot >= 0) {
                     ++top[slot].runs;
                     top[slot].ns += (uint64_t)(t1.tv_sec - looper_t0.tv_sec) * 1000000000ull +
                                     (uint64_t)(t1.tv_nsec - looper_t0.tv_nsec);
                     top[slot].steps += vm->steps - before;
                  }
                  if (t1.tv_sec - looper_last.tv_sec >= 5) {
                     for (int k = 0; k < 24; ++k)
                        if (top[k].name && top[k].ns > 20000000ull)
                           fprintf(stderr, "[looper] %6.0f ms %6u runs %10llu steps  %s\n", top[k].ns / 1e6,
                                   top[k].runs, (unsigned long long)top[k].steps, top[k].name);
                     memset(top, 0, sizeof top);
                     looper_last = t1;
                  }
               }
            }
            char how[256];
            how[0] = '\0';
            if (!ok && vm->exception)
               dvm_describe_exception(vm, vm->exception, how, sizeof how);
            /* A thread that parked has not finished: it is blocked inside the
             * platform on something another thread must produce — a Volley
             * dispatcher sitting in BlockingQueue.take() before the first
             * request is added is the standard shape.  Dropping it ended the
             * thread for good, so every request queued afterwards had no
             * consumer and the caller waiting on the response timed out.  Put
             * it back, so a later drain retries the wait the way the blocked
             * thread would have resumed on a real runtime. */
            const bool parked_here = vm->parked;
            if (entry_is_thread && !parked_here) {
               (void)dvm_set_field(vm, entry, "alive", "Z", (union dvm_value){.i = 0});
               dvm__thread_set_state(vm, entry, DVM_THREAD_TERMINATED);
               dvm_gil_notify_for(entry);
            }
            dvm_clear_exception(vm);
            if (parked_here) {
               /* The exception only exists to unwind the wait. It is not a
                * failure, and reporting it would print one line per park. */
               how[0] = '\0';
               bool requeued = dvm__queue_runnable_at(vm, list[i], is_thread[i],
                                                      DVM_PARK_RETRY_MS);
               if (dvm__sched_trace_for(c ? c->name : NULL))
                  fprintf(stderr, "[sched] parked %s.run() depth=%d %s\n",
                          c && c->name ? c->name : "?", vm->drain_depth,
                          requeued ? "requeued" : "DROPPED (queue full)");
            } else if (dvm__sched_trace_for(c ? c->name : NULL)) {
               fprintf(stderr, "[sched] ran %s.run() depth=%d steps=%llu\n",
                       c && c->name ? c->name : "?", vm->drain_depth,
                       (unsigned long long)(vm->steps - before));
            }
            static int log_n = 0;
            /* The cap keeps startup readable; a trace run wants every one of
               them, because a runnable that keeps re-queueing is the symptom.
               An exception is not startup noise: it is dropped together with
               the rest of the Runnable (Volley's Request.finish never runs),
               and once the first two dozen callbacks have passed it used to
               vanish entirely. Cross Worlds' sign-in delivery is one of those. */
            const bool failed = how[0] != '\0';
            const bool report = failed || log_n < 24 || vm->trace;
            if (!failed) ++log_n;
            if (report)
               fprintf(stderr, "[dvm] thread %s.run() ran %llu steps%s%s\n",
                       c && c->name ? c->name : "?",
                       (unsigned long long)(vm->steps - before),
                       how[0] ? " — " : "", how[0] ? how : "");
            /* A Runnable that died of an exception is the interesting case, and
             * the class of the Runnable says nothing about where it died — the
             * throw is usually several frames down inside an SDK.  Print the
             * unwind path for it exactly as dvm_call() does for an uncaught
             * exception on the main thread. */
            if (report && how[0])
               for (int f_i = 0; f_i < vm->nexc_trace; ++f_i) {
                  struct dvm_method *f = vm->exc_trace[f_i];
                  if (!f) continue;
                  if (f_i == 0)
                     fprintf(stderr, "[dvm]   at %s.%s%s +0x%x\n",
                             f->cls ? f->cls->name : "?", f->name,
                             f->sig ? f->sig : "", vm->exc_pc * 2);
                  else
                     fprintf(stderr, "[dvm]   at %s.%s%s\n",
                             f->cls ? f->cls->name : "?", f->name,
                             f->sig ? f->sig : "");
               }
            vm->nexc_trace = 0;
            if (!is_thread[i]) dvm_runtime_looper_log(vm, 0, false, list[i]);
         }
         dvm_unpin(vm, list[i]);
      }
   }

   vm->step_limit = saved_limit;
   vm->quiet_uncaught = saved_quiet;
   --vm->drain_depth;
   return ran;
}

void dvm__run_pending_threads(struct dvm *vm)
{
   (void)drain_pending(vm, false);
   /* Only at the top: a nested drain is inside somebody's Runnable, and a
    * click callback dispatched there would run underneath a wait. */
   if (vm->drain_depth == 0) dvm__ui_tick(vm);
}
int dvm__drain_for_wait(struct dvm *vm) { return drain_pending(vm, true); }

/* One turn of the main thread's Looper, for the frame pump to call.
 *
 * On a device the main thread sits in Looper.loop(): a Handler callback whose
 * delay has elapsed runs within a millisecond or two of its due time, whatever
 * the game's own threads are doing.  Here the queue was drained only where a
 * dvm_call happened to unwind to depth 0 — that is, only when the *guest*
 * called into Java of its own accord.  Between two such calls nothing ran the
 * looper at all, so an armed callback waited for the guest rather than for its
 * deadline: Cross Worlds' anti-tamper heartbeat
 * (NmssSa.TDThreadWait -> Handler(mainLooper).postDelayed(->
 * nmssNativeSignalToTDThread, ms)) came in 6.7 seconds late, and its native
 * side reads a signal that late as the process having been frozen.
 *
 * The pump's frame is this emulator's main-thread loop, so this belongs in it
 * next to the other things a device's main thread does every turn. */
void dvm_main_looper_tick(struct dvm *vm)
{
   /* Not only when a message is pending: input the emulator's own windows
    * took (a tap on a guest dialog) and their redraws are the main thread's
    * work too, and they arrive with nothing queued. */
   if (!vm) return;
   /* Same contract as dvm_media_pump_active(): called from the frame pump,
    * which does not carry the interpreter lock. */
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   if (vm->main_transaction_depth) {
      dvm_gil_leave_to_guest(vm, cookie);
      return;
   }
   struct dvm_gc_native_scope turn_scope={0};
   if(!dvm_gc_native_enter(vm,&turn_scope,0,"()V",NULL,0)){
      dvm_gc_native_leave(&turn_scope);dvm_gil_leave_to_guest(vm,cookie);return;
   }
   /* A pump turn belongs to Android's main Looper, not to the guest JNI
    * invocation that last used this host thread. That invocation may have
    * parked inside a modal wait with drain_depth > 0. Borrowing its state
    * suppresses UI redraws and clicks forever, while preserving it lets the
    * suspended invocation resume with its own stack and exception intact. */
   struct dvm_tstate saved, main_turn = { 0 };
   dvm__tstate_save(vm, &saved);
   struct gc_tstate_hold gc_saved;gc_tstate_local_enter(&gc_saved,vm,&saved);
   main_turn.cur_thread = vm->main_thread;
   dvm__tstate_load(vm, &main_turn);
   extern __thread dvm_ref g_current_looper;
   const dvm_ref saved_looper = g_current_looper;
   g_current_looper = 0;
   thread_runtime_vsync_tick(vm);
   dvm__network_tick(vm);
   bool dispatched = false;
   const uint64_t now = dvm__now_ms();
   for (int i = 0; i < vm->npending; ++i)
      if (!vm->pending_is_thread[i] && !vm->pending_looper[i] &&
          (!vm->pending_due_ms[i] || vm->pending_due_ms[i] <= now)) { dispatched = true; break; }
   dvm__run_pending_threads(vm);
   dvm__main_queue_idle(vm, dispatched);
   dvm_gc_native_leave(&turn_scope);
   g_current_looper = saved_looper;
   dvm__tstate_load(vm, &saved);
   gc_tstate_local_leave(&gc_saved);
   dvm_gil_leave_to_guest(vm, cookie);
}


/* ------------------------------------------------------------------------ *
 * The global interpreter lock, and the thread state it hands over
 *
 * See the commentary above dvm_gil_acquire() in dvm.h for why this exists.
 * ------------------------------------------------------------------------ */

#define TS_COPY(dst, src, f) ((dst)->f = (src)->f)

void dvm__tstate_save(struct dvm *vm, struct dvm_tstate *t)
{
   TS_COPY(t, vm, exception);
   TS_COPY(t, vm, nexc_trace);
   TS_COPY(t, vm, exc_ref);
   TS_COPY(t, vm, exc_pc);
   TS_COPY(t, vm, cur_thread);
   TS_COPY(t, vm, drain_depth);
   TS_COPY(t, vm, quiet_uncaught);
   TS_COPY(t, vm, parked);
   TS_COPY(t, vm, cur_method);
   TS_COPY(t, vm, cur_pc);
   TS_COPY(t, vm, ncallstack);
   TS_COPY(t, vm, invoke_frame);
   TS_COPY(t, vm, depth);
   TS_COPY(t, vm, call_steps);
   TS_COPY(t, vm, step_limit);
   memcpy(t->exc_trace, vm->exc_trace, sizeof t->exc_trace);
   /* Only the live part: the array is 128 entries and this copy happens on
    * every handover, so copying the tail would cost more than the switch. */
   int n = vm->ncallstack;
   if (n < 0) n = 0;
   if (n > (int)(sizeof t->callstack / sizeof t->callstack[0]))
      n = (int)(sizeof t->callstack / sizeof t->callstack[0]);
   memcpy(t->callstack, vm->callstack, (size_t)n * sizeof t->callstack[0]);
}

void dvm__tstate_load(struct dvm *vm, const struct dvm_tstate *t)
{
   TS_COPY(vm, t, exception);
   TS_COPY(vm, t, nexc_trace);
   TS_COPY(vm, t, exc_ref);
   TS_COPY(vm, t, exc_pc);
   TS_COPY(vm, t, cur_thread);
   TS_COPY(vm, t, drain_depth);
   TS_COPY(vm, t, quiet_uncaught);
   TS_COPY(vm, t, parked);
   TS_COPY(vm, t, cur_method);
   TS_COPY(vm, t, cur_pc);
   TS_COPY(vm, t, ncallstack);
   TS_COPY(vm, t, invoke_frame);
   TS_COPY(vm, t, depth);
   TS_COPY(vm, t, call_steps);
   TS_COPY(vm, t, step_limit);
   memcpy(vm->exc_trace, t->exc_trace, sizeof vm->exc_trace);
   int n = t->ncallstack;
   if (n < 0) n = 0;
   if (n > (int)(sizeof t->callstack / sizeof t->callstack[0]))
      n = (int)(sizeof t->callstack / sizeof t->callstack[0]);
   memcpy(vm->callstack, t->callstack, (size_t)n * sizeof t->callstack[0]);
}

#undef TS_COPY

/* Per-thread copy of the above.  A thread that has never held the lock has no
 * saved state, and inheriting the previous holder's would give it that
 * thread's call stack and pending exception. */
static _Thread_local struct dvm_tstate g_tstate;
static _Thread_local bool g_tstate_valid;
static _Thread_local struct dvm *g_tstate_vm;
static _Thread_local uint64_t g_tstate_serial;

/* Whether this host thread exists to run bytecode, as opposed to being the one
 * that drives the frame pump and the guest CPU.  The difference matters
 * wherever a wait has to be kept short so the emulator keeps running: on a
 * thread of its own, a Thread.sleep(500) can simply take 500 ms. */
static _Thread_local bool g_is_bytecode_thread;

bool dvm_on_bytecode_thread(void) { return g_is_bytecode_thread; }
void dvm__mark_bytecode_thread(void) { g_is_bytecode_thread = true; }

/* Android's main thread.  See dvm_on_main_thread() in dvm.h. */
static _Thread_local bool g_is_main_thread;

bool dvm_on_main_thread(void)
{
   return g_is_main_thread && dvm__logical_main_thread(dvm_current());
}

void dvm__tstate_reset(struct dvm *vm)
{
   /* Zero throughout, including the step limit: that budget exists to bound a
    * call the VM cannot preempt, and a thread with its own stack is preempted
    * every DVM_GIL_YIELD_STEPS instructions instead.  A caller that wants a
    * budget sets one after this returns.
    *
    * Installed into the VM as well as saved, because the fields still hold
    * whatever the previous holder of the lock left in them — this thread has
    * no saved state to load, which is exactly why it is calling this. */
   memset(&g_tstate, 0, sizeof g_tstate);
   g_tstate_valid = true;
   g_tstate_vm=vm;g_tstate_serial=vm?vm->serial:0;
   gc_tstate_resume();
   if (vm) dvm__tstate_load(vm, &g_tstate);
}

static struct {
   pthread_mutex_t m;
   pthread_cond_t cv;
   pthread_t owner;
   _Atomic unsigned depth; /* 0 = nobody holds it */
   /* Threads blocked in acquire.  dvm_gil_yield() is called from the
    * interpreter's dispatch loop and from the guest scheduler's spin, both of
    * them hot; with nobody waiting there is nothing to hand the lock to, and
    * the handover would cost a thread-state save and load for nothing. */
   _Atomic unsigned waiters;
   /* Handed out in the order it was asked for, for the same reason the ARM
    * execution lock is (src/arm.c).  Every wait in this VM is "give the lock
    * up and take it again until somebody else makes my condition true" — a
    * java.util.concurrent queue's take(), a Thread.sleep() poll loop.  With an
    * unfair lock the thread that just released it is the one already running,
    * so it wins the race back every time and the thread it is waiting for
    * never gets in: a UE modal box looping `Thread.sleep(100)` on an engine
    * worker held the interpreter for good while every bytecode thread sat in
    * dvm_gil_acquire(), and the frame pump waited on the worker.
    *
    * The queue is explicit rather than a ticket counter over a shared condvar:
    * with a shared one every release has to be a broadcast, because only the
    * holder of the next ticket may proceed, so each hand-off woke every waiter
    * and all but one went straight back to sleep.  One condvar per waiter lets
    * the releaser signal exactly the thread whose turn it is.
    *
    * Recursion depth is a thread-local, so a recursive acquire and "do I hold
    * it?" - both on the hot JNI path - cost nothing at all. */
} g_gil = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 };

struct dvm_gil_waiter {
   pthread_cond_t         cv;
   struct dvm_gil_waiter *next;
   long tid;
   uint64_t enqueued_ns;
   bool main_thread;
   bool                   go;   /* the lock has been handed to me */
};
static struct dvm_gil_waiter *g_gil_head, *g_gil_tail;
static __thread unsigned      t_gil_depth;

/* The one VM in the process.  Callers outside this module — the frame pump,
 * the guest scheduler — have to offer the lock up without having been handed a
 * `struct dvm *` to do it with. */
static struct dvm *g_gil_vm;

/* Only written while g_gil.m is held, so plain counters are enough.
 *
 * Two different questions, and the first one alone is misleading.  The summed
 * wait is over every thread, so eight idle consumers correctly blocked on
 * empty queues add eight seconds of "wait" per second of wall time and nothing
 * is wrong.  What says whether the interpreter is actually the bottleneck is
 * how much of the wall clock *somebody holds the lock* — at 100% the VM is
 * saturated and every waiter is a real queue; at 20% the waiters are idle and
 * the sum is just counting their idleness. */
static _Atomic unsigned long long g_gil_wait_ns;     /* summed over threads */
static _Atomic unsigned long long g_gil_held_ns;     /* wall time with an owner */
static _Atomic unsigned long long g_gil_max_wait_ns; /* worst single wait */
static struct timespec    g_gil_held_since;
/* Who holds the interpreter lock, and what for.
 *
 * The lock's cost is visible in the slice report ("interpreter lock: held 30%
 * of the last 5.0s, 7653.0 ms summed wait, worst single wait 570.9 ms" while
 * the execution lock was at 1% and 0.4 ms) but the report cannot say *who*,
 * and that is the whole question: a convoy of short JNI calls and one holder
 * sitting on it across a long host operation look identical in the totals.
 *
 * Measured on Cross Worlds: the numbers above appear the moment a 228 MB
 * background download starts calling BackgroundDownload.onProgressUpdate()
 * into native, and guest execution falls from ~109 Mips to a few hundred
 * instructions per five seconds.  A waiter that has been kept out for longer
 * than a frame deserves to name the thread keeping it out. */
static unsigned           g_gil_owner_id;
static const char        *g_gil_owner_what = "?";
static __thread unsigned  t_gil_id;
static __thread const char *t_gil_what = "?";
static _Atomic unsigned   g_gil_next_id;

#include "gc_stw.inc"
static void gil_perf_controls(void);
static void gil_perf_write(FILE *);
#pragma push_macro("fprintf")
#pragma push_macro("fputc")
#undef fprintf
#undef fputc
#include "gil_observe.inc"
#include "gil_perf.inc"
#include "gil_deep.inc"
#pragma pop_macro("fputc")
#pragma pop_macro("fprintf")

/* Called from inside gil_tryacquire()/dvm_gil_acquire(), which already hold
 * g_gil.m — so it must not take it.  An atomic counter needs no lock. */
static unsigned gil_self_id(void)
{
   if (!t_gil_id)
      t_gil_id = atomic_fetch_add_explicit(&g_gil_next_id, 1u,
                                           memory_order_relaxed) + 1u;
   return t_gil_id;
}

/* Long enough that a healthy JNI call never trips it; short enough that a
 * holder that is doing host I/O under the lock always does. */
#define GIL_SLOW_WAIT_NS 50000000ull   /* 50 ms */
unsigned long long dvm_gil_wait_ns(void)
{
   return atomic_load_explicit(&g_gil_wait_ns, memory_order_relaxed);
}
unsigned long long dvm_gil_held_ns(void)
{
   return atomic_load_explicit(&g_gil_held_ns, memory_order_relaxed);
}
unsigned long long dvm_gil_max_wait_ns(void)
{
   return atomic_exchange_explicit(&g_gil_max_wait_ns, 0,
                                   memory_order_relaxed);
}
static unsigned long long gil_ns_since(const struct timespec *a,
                                       const struct timespec *b)
{
   return (unsigned long long)(b->tv_sec - a->tv_sec) * 1000000000ull
        + (unsigned long long)(b->tv_nsec - a->tv_nsec);
}

/* Signalled whenever a thread changes something another thread might be
 * blocked on: a queue gains an item, a latch is counted down, a permit is
 * handed over.  See dvm_gil_wait(). */
static pthread_cond_t g_gil_change = PTHREAD_COND_INITIALIZER;
/* Protected by g_gil.m.  The sequence turns the condvar into an event: a
 * notification that lands between dropping GIL and entering timedwait is
 * observed instead of being lost. */
static unsigned long long g_gil_change_seq;

/* Targeted waiters keep independent queues from waking one another.  A Timer
 * per in-flight operation is common Java code; broadcasting one global event
 * to all of them turns a harmless enqueue into an O(thread count) GIL storm.
 *
 * Waiters live on their sleeping thread's stack.  Registration, notification
 * and removal all happen under g_gil.m, and the notifier only sets `ready`, so
 * it never retains a stack pointer after the waiter removes itself. */
struct dvm_event_waiter {
   pthread_cond_t cv;
   struct dvm_event_waiter *next;
   uintptr_t channel;
   bool ready;
   char where[480];      /* the Java frames the waiter is parked in (diagnostics) */
};
static struct dvm_event_waiter *g_event_waiters;

/* LUNARIA_DVM_WAITDUMP=1: SIGUSR2 prints every Java thread parked on a channel and the
 * Java frames it is parked in.  A stall in which every thread is idle and nothing is
 * runnable (the process "spins" at 0 Mips) has no other witness: the host stacks show
 * only the VM's own wait primitive.  Windows has no SIGUSR2, so the dump stays a
 * POSIX diagnostic. */
#ifndef _WIN32
static void dvm_waitdump(int sig)
{
   (void)sig;
   fprintf(stderr, "[waitdump] parked Java threads:\n");
   for (struct dvm_event_waiter *w = g_event_waiters; w; w = w->next)
      fprintf(stderr, "[waitdump]   channel=%lu ready=%d  %s\n",
              (unsigned long)w->channel, (int)w->ready, w->where);
   if (g_gil_vm) {
      struct dvm *vm = g_gil_vm;
      const uint64_t now = dvm__now_ms();
      fprintf(stderr, "[waitdump] pending Runnables: %d\n", vm->npending);
      for (int i = 0; i < vm->npending; ++i) {
         struct dvm_class *c = vm->pending_threads[i] ? dvm_object_class(vm, vm->pending_threads[i]) : NULL;
         fprintf(stderr, "[waitdump]   #%d %s thread=%d looper=%u due=%+lldms\n", i,
                 c && c->name ? c->name : "?", (int)vm->pending_is_thread[i],
                 (unsigned)vm->pending_looper[i],
                 vm->pending_due_ms[i] ? (long long)vm->pending_due_ms[i] - (long long)now : 0ll);
      }
   }
}
#endif

void dvm_gil_notify(void)
{
   pthread_mutex_lock(&g_gil.m);
   ++g_gil_change_seq;
   pthread_cond_broadcast(&g_gil_change);
   pthread_mutex_unlock(&g_gil.m);
}

bool dvm_is_stopping(struct dvm *vm)
{
   return vm && atomic_load_explicit(&vm->stopping, memory_order_relaxed);
}

bool dvm_process_exited(struct dvm *vm, int *status)
{
   if (!vm || atomic_load_explicit(&vm->process_exit_phase,memory_order_acquire)!=2) return false;
   if (status) *status=atomic_load_explicit(&vm->process_exit_status,memory_order_relaxed);
   return true;
}

void dvm_exit_process(struct dvm *vm, int status)
{
   if (!vm) return;
   dvm_lifetime_note(vm,"dvm_exit_process");
   int live=0;
   if (!atomic_compare_exchange_strong_explicit(&vm->process_exit_phase,&live,1,
         memory_order_relaxed,memory_order_relaxed)) return;
   atomic_store_explicit(&vm->process_exit_status,status,memory_order_relaxed);
   atomic_store_explicit(&vm->process_exit_phase,2,memory_order_release);
   dvm_request_stop(vm);
}

void dvm_request_stop(struct dvm *vm)
{
   if (!vm) return;
   if (!atomic_exchange_explicit(&vm->stopping, true, memory_order_relaxed)) {
      dvm_lifetime_note(vm,"dvm_request_stop");
      const char wake = 1;
      /* One byte, never drained: every concurrent network waiter wakes. */
      luna_socket_send(vm->io_cancel[1], &wake, 1, LUNA_SEND_NOSIGNAL);
   }
   pthread_mutex_lock(&g_gil.m);
   ++g_gil_change_seq;
   pthread_cond_broadcast(&g_gil_change);
   for (struct dvm_event_waiter *w = g_event_waiters; w; w = w->next) {
      w->ready = true;
      pthread_cond_signal(&w->cv);
   }
   pthread_mutex_unlock(&g_gil.m);
}

void dvm_gil_notify_for(uintptr_t channel)
{
   if (!channel) { dvm_gil_notify(); return; }
   pthread_mutex_lock(&g_gil.m);
   for (struct dvm_event_waiter *w = g_event_waiters; w; w = w->next) {
      if (w->channel != channel) continue;
      w->ready = true;
      pthread_cond_signal(&w->cv);
   }
   pthread_mutex_unlock(&g_gil.m);
}

void dvm_gil_notify_one_for(uintptr_t channel)
{
   if (!channel) { dvm_gil_notify(); return; }
   pthread_mutex_lock(&g_gil.m);
   for (struct dvm_event_waiter *w = g_event_waiters; w; w = w->next) {
      if (w->channel != channel) continue;
      w->ready = true;
      pthread_cond_signal(&w->cv);
      break;
   }
   pthread_mutex_unlock(&g_gil.m);
}

/* Wait for such a change, or for `ms` to pass, without holding the
 * interpreter lock.
 *
 * The blocking primitives used to do this by hand: drop the lock, usleep(1000),
 * take it again, re-check.  With one bytecode thread that is a poll; with the
 * eight a UE title's Java side runs it is eight threads taking a *global* lock
 * a thousand times a second each, and the lock is handed out in ticket order,
 * so a JNI call arriving from guest code queues behind all of them.  Measured
 * on Cross Worlds: 22.6 seconds of accumulated wait in a 5-second window, and
 * 4.1 ms for an average CallIntMethodV.
 *
 * The timeout stays as a backstop rather than the mechanism — a notify can be
 * missed in the window between releasing the lock and taking g_gil.m, and the
 * callers all re-check their own condition — but it is 20 ms rather than 1,
 * so an idle waiter costs fifty lock acquisitions a second instead of a
 * thousand, and a waiter with a producer is woken at once instead of on the
 * next tick. */
/* Take the interpreter lock from a context that is holding the ARM execution
 * lock — every JNI entry point reached from a guest SVC is one.
 *
 * Lock order in the emulator is interpreter-outside, execution-inside (see the
 * call_native boundary above).  A thread that already holds the execution lock
 * therefore cannot simply block on the interpreter lock: it would hold the two
 * in the opposite order to a bytecode thread and the pair deadlocks.  So the
 * execution lock is given up first, the interpreter lock is taken, and the
 * execution lock is taken again — which is also exactly what a real JNI
 * transition does when it re-enters the VM. */
/* Take the lock only if it is free.  Never blocks, so it cannot invert the
 * lock order and needs no dance with the execution lock. */
static bool gil_tryacquire(struct dvm *vm)
{
   if (t_gil_depth) { ++t_gil_depth; return true; }
   gc_stw_thread_waiting(vm);
   bool got = false;
   if (pthread_mutex_trylock(&g_gil.m)) return false;
   /* Free *and* nobody queued: barging past a waiter would undo the fairness
    * the queue exists for. */
   if (!atomic_load_explicit(&g_gil.depth, memory_order_relaxed) &&
       !g_gil_head) {
      atomic_store_explicit(&g_gil.depth, 1, memory_order_relaxed);
      g_gil.owner = pthread_self();
      clock_gettime(CLOCK_MONOTONIC, &g_gil_held_since);
      g_gil_owner_id = gil_self_id();
      g_gil_owner_what = t_gil_what;
      gil_obs_acquired();
      got = true;
   }
   pthread_mutex_unlock(&g_gil.m);
   if (got) {
      t_gil_depth = 1;
      gc_fast_gil_acquired(vm);
      if (!g_tstate_valid||g_tstate_vm!=vm||g_tstate_serial!=vm->serial) dvm__tstate_reset(vm);
      else                 dvm__tstate_load(vm, &g_tstate);
      gc_tstate_resume();
   }
   return got;
}

bool dvm_gil_try_acquire(struct dvm *vm){return gil_tryacquire(vm);}

unsigned dvm_gil_enter_from_guest(struct dvm *vm)
{
   t_gil_what = "guest JNI";
   /* No VM at all — a title with no classes*.dex runs with bytecode emulation
    * off, and its JNI entry points still come through here. */
   if (!vm) return 0u;
   if (dvm_gil_held(vm)) return 0u;      /* already ours: nothing to order */
   /* The common case by far: no Java thread is inside the VM, so the lock is
    * free and this costs one mutex.  Dropping and re-taking the execution lock
    * unconditionally would be three lock operations per JNI call — and the
    * execution lock is a ticket, so re-taking it means going to the back of
    * the queue and sleeping.  UnitySampleGame, which calls into Java every
    * frame, lost four fifths of its frame rate to exactly that. */
   if (gil_tryacquire(vm)) return 1u;
   unsigned ael = arm_lock_unlock_all();
   dvm_gil_acquire(vm);
   arm_lock_relock(ael);
   return 1u;
}

void dvm_gil_leave_to_guest(struct dvm *vm, unsigned cookie)
{
   /* Releasing never blocks, so the execution lock can stay where it is. */
   if (!vm || !cookie) return;
   dvm_gil_release(vm);
}

extern int arm_exec_host_ui_thread(void) __attribute__((weak));
extern void arm_exec_service_host_ui(void) __attribute__((weak));
extern int arm_exec_main_looper_enter(void) __attribute__((weak));
extern void arm_exec_main_looper_leave(void) __attribute__((weak));

/* A non-main guest can wait on the host executor that normally pumps UI.
 * Main Looper work has an independent identity; a wait by the main Java
 * thread itself must not re-enter that thread's Handler callbacks. */
static void ui_before_platform_wait(struct dvm *vm)
{
   static _Thread_local bool active;
   if (active || !arm_exec_host_ui_thread || !arm_exec_host_ui_thread()) return;
   active = true;
   struct dvm_gc_native_scope turn_scope={0};
   if(!dvm_gc_native_enter(vm,&turn_scope,0,"()V",NULL,0)){
      dvm_gc_native_leave(&turn_scope);active=false;return;
   }
   if (!dvm__logical_main_thread(vm) && arm_exec_main_looper_enter &&
       arm_exec_main_looper_leave && arm_exec_main_looper_enter()) {
      /* The host executor is borrowed by a suspended non-main guest. Run
       * the main Looper as its own Java/native identity, then restore it. */
      dvm_main_looper_tick(vm);
      arm_exec_main_looper_leave();
      if (arm_exec_service_host_ui) arm_exec_service_host_ui();
      dvm_gc_native_leave(&turn_scope);
      active = false;
      return;
   }
   struct dvm_tstate saved, main_turn = { 0 };
   dvm__tstate_save(vm, &saved);
   struct gc_tstate_hold gc_saved;gc_tstate_local_enter(&gc_saved,vm,&saved);
   main_turn.cur_thread = vm->main_thread;
   dvm__tstate_load(vm, &main_turn);
   dvm__ui_tick(vm);
   dvm__tstate_load(vm, &saved);
   gc_tstate_local_leave(&gc_saved);
   if (arm_exec_service_host_ui) arm_exec_service_host_ui();
   dvm_gc_native_leave(&turn_scope);
   active = false;
}

void dvm_gil_wait(struct dvm *vm, unsigned ms)
{
   if (dvm_is_stopping(vm)) return;
   ui_before_platform_wait(vm);
   /* Snapshot while still owning GIL.  A producer cannot change a VM
    * condition until this thread releases GIL, so any later condition change
    * also advances the sequence. */
   pthread_mutex_lock(&g_gil.m);
   unsigned long long seen = g_gil_change_seq;
   pthread_mutex_unlock(&g_gil.m);

   unsigned d = dvm_gil_unlock_all(vm);
   /* A blocking VM wait reached from a guest JNI call still owns the ARM
    * execution lock, and what it is waiting for almost always needs it: the
    * joined Java thread's own native calls, and the pump that drains the
    * pending-Runnable queue.  Holding it here stopped every guest thread --
    * and the pump with them -- for the whole wait, which is the one thing that
    * guarantees the condition never becomes true.  Give it up for the
    * duration, exactly as dvm_gil_enter_from_guest() does when GIL is
    * contended, and take it back in lock order (GIL, then AEL). */
   unsigned ael = arm_lock_unlock_all();
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec  += (time_t)(ms / 1000u);
   ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
   if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ++ts.tv_sec; }
   pthread_mutex_lock(&g_gil.m);
   while (g_gil_change_seq == seen && !dvm_is_stopping(vm)) {
      int rc = pthread_cond_timedwait(&g_gil_change, &g_gil.m, &ts);
      if (rc != 0) break;
   }
   pthread_mutex_unlock(&g_gil.m);
   dvm_gil_relock(vm, d);
   arm_lock_relock(ael);
}

void dvm_gil_wait_for(struct dvm *vm, uintptr_t channel, unsigned ms)
{
   if (!channel) { dvm_gil_wait(vm, ms); return; }
   dvm_gil_wait_for_ns(vm, channel, (uint64_t)ms * 1000000ull);
}

void dvm_gil_wait_for_ns(struct dvm *vm, uintptr_t channel, uint64_t ns)
{
   if (dvm_is_stopping(vm)) return;

   struct dvm_event_waiter w;
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   clockid_t clock = CLOCK_REALTIME;
#if defined(__linux__)
   if (!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) clock = CLOCK_MONOTONIC;
#endif
   pthread_cond_init(&w.cv, &attr);
   pthread_condattr_destroy(&attr);
   w.channel = channel;
   w.ready = false;
   {
      static int dump_on = -1;
      if (dump_on < 0) {
         const char *e = getenv("LUNARIA_DVM_WAITDUMP");
         dump_on = e && *e && *e != '0';
#ifndef _WIN32
         if (dump_on) signal(SIGUSR2, dvm_waitdump);
#endif
      }
      w.where[0] = 0;
      if (dump_on && vm) {
         size_t n = 0;
         for (int i = vm->ncallstack - 1, shown = 0; i >= 0 && shown < 5 && n + 2 < sizeof w.where; --i, ++shown) {
            const struct dvm_method *f = vm->callstack[i];
            if (!f) continue;
            n += (size_t)snprintf(w.where + n, sizeof w.where - n, "%s%s.%s", shown ? " <- " : "",
                                  f->cls && f->cls->name ? f->cls->name : "?", f->name ? f->name : "?");
            if (n >= sizeof w.where) { n = sizeof w.where - 1; break; }
         }
      }
   }

   /* Register before dropping GIL.  A producer cannot change the protected VM
    * state before GIL is released, so it cannot notify in the registration
    * gap.  This gives condvars event semantics without a polling timeout. */
   pthread_mutex_lock(&g_gil.m);
   w.next = g_event_waiters;
   g_event_waiters = &w;
   pthread_mutex_unlock(&g_gil.m);

   /* Main work can notify this very waiter. Register first so a notification
    * delivered while the borrowed host executor services UI is not lost. */
   ui_before_platform_wait(vm);

   unsigned d = dvm_gil_unlock_all(vm);
   /* See dvm_gil_wait(): the execution lock cannot be held across a wait for
    * something that needs it to happen. */
   unsigned ael = arm_lock_unlock_all();
   struct timespec ts;
   if (ns != UINT64_MAX) {
      clock_gettime(clock, &ts);
      ts.tv_sec  += (time_t)(ns / 1000000000ull);
      ts.tv_nsec += (long)(ns % 1000000000ull);
      if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ++ts.tv_sec; }
   }

   pthread_mutex_lock(&g_gil.m);
   while (!w.ready && !dvm_is_stopping(vm)) {
      int rc = ns == UINT64_MAX ? pthread_cond_wait(&w.cv, &g_gil.m)
                               : pthread_cond_timedwait(&w.cv, &g_gil.m, &ts);
      if (rc != 0) break;
   }
   struct dvm_event_waiter **link = &g_event_waiters;
   while (*link && *link != &w) link = &(*link)->next;
   if (*link) *link = w.next;
   pthread_mutex_unlock(&g_gil.m);

   pthread_cond_destroy(&w.cv);
   dvm_gil_relock(vm, d);
   arm_lock_relock(ael);
}

struct dvm *dvm_current(void) { return g_gil_vm; }

bool dvm_gil_held(struct dvm *vm)
{
   (void)vm;
   return t_gil_depth != 0u;
}

void dvm_gil_acquire(struct dvm *vm)
{
   if (t_gil_depth) { ++t_gil_depth; return; }

   /* Keep the global lock order GIL -> AEL at the primitive boundary.
    *
    * Most callers are pure bytecode threads and do not own the ARM execution
    * lock.  A Java -> native -> Java callback is different: the native half
    * can return to dvm__call_out() while its callback SVC still owns AEL, and
    * the old dvm_gil_relock() then waited here without dropping AEL.  At the
    * same time the pump could own GIL and be re-taking AEL after a JNI call:
    * a real ABBA deadlock.
    *
    * Preserve the uncontended hot path.  Taking a free GIL cannot wait and is
    * therefore safe while AEL is held.  On contention, release every recursive
    * AEL level before joining the FIFO GIL queue and restore it only after GIL
    * ownership has been handed to us.  Putting this here, rather than in each
    * callback path, makes the ordering invariant apply to every current and
    * future GIL acquisition. */
   if (gil_tryacquire(vm)) return;
   unsigned ael_depth = arm_lock_unlock_all();

   pthread_mutex_lock(&g_gil.m);
   if (!atomic_load_explicit(&g_gil.depth, memory_order_relaxed) &&
       !g_gil_head) {
      atomic_store_explicit(&g_gil.depth, 1, memory_order_relaxed);
   } else {
      /* The waiter lives on this thread's stack.  Safe: the releaser only
       * touches it under g_gil.m, and this thread does not leave the wait
       * until it has been dequeued under the same mutex. */
      struct dvm_gil_waiter w;
      pthread_cond_init(&w.cv, NULL);
      w.next = NULL;
      w.go   = false;
      w.tid=gil_tid();
      w.enqueued_ns=gc_fast_ns();w.main_thread=gil_is_main();
      if(atomic_load(&gil_obs_enabled)){struct gil_obs_row *r=gil_row(w.tid);if(r)r->since=gc_fast_ns();}
      if (g_gil_tail) g_gil_tail->next = &w; else g_gil_head = &w;
      g_gil_tail = &w;
      atomic_fetch_add_explicit(&g_gil.waiters, 1, memory_order_relaxed);
      unsigned qn=atomic_load(&g_gil.waiters);gil_queue_hist[qn<129?qn:128]++;
      struct timespec w0, w1;
      clock_gettime(CLOCK_MONOTONIC, &w0);
      while (!w.go) {
         pthread_cond_wait(atomic_load(&gil_broadcast)?&g_gil.cv:&w.cv, &g_gil.m);
         gil_wake_returns++;if(!w.go)gil_retries++;
      }
      clock_gettime(CLOCK_MONOTONIC, &w1);
      atomic_fetch_sub_explicit(&g_gil.waiters, 1, memory_order_relaxed);
      pthread_cond_destroy(&w.cv);
      const unsigned long long d = gil_ns_since(&w0, &w1);
      atomic_fetch_add_explicit(&g_gil_wait_ns, d, memory_order_relaxed);
      unsigned long long old = atomic_load_explicit(
         &g_gil_max_wait_ns, memory_order_relaxed);
      while (old < d && !atomic_compare_exchange_weak_explicit(
               &g_gil_max_wait_ns, &old, d,
               memory_order_relaxed, memory_order_relaxed)) {}
      if (d >= GIL_SLOW_WAIT_NS) {
         static unsigned said;
         if (said < 64u || (said % 200u) == 0u)
            fprintf(stderr, "[gil] thread %u (%s) waited %.1f ms for the "
                    "interpreter lock; it was held by thread %u (%s)\n",
                    gil_self_id(), t_gil_what, (double)d / 1e6,
                    g_gil_owner_id, g_gil_owner_what);
         ++said;
      }
      /* g_gil.depth stayed 1: the releaser handed ownership straight over. */
   }
   g_gil.owner = pthread_self();
   clock_gettime(CLOCK_MONOTONIC, &g_gil_held_since);
   g_gil_owner_id = gil_self_id();
   g_gil_owner_what = t_gil_what;
      gil_obs_acquired();
   pthread_mutex_unlock(&g_gil.m);
   t_gil_depth = 1;
   gc_fast_gil_acquired(vm);
   /* Nobody else can be interpreting now, so the VM's per-thread fields are
    * free for this thread to install its own into.
    *
    * A thread that has never held the lock has nothing saved, and the fields
    * still hold the previous holder's — its call stack, its pending exception
    * and the steps it has already spent.  Leaving them was survivable while a
    * new thread always called dvm__tstate_reset() before it first interpreted;
    * it stopped being so once threads take the lock more often, and a freshly
    * started java/lang/Thread inherited a call_steps that was already at the
    * limit, so its first method aborted with "step limit reached". */
   if (!g_tstate_valid||g_tstate_vm!=vm||g_tstate_serial!=vm->serial) dvm__tstate_reset(vm);
   else                 dvm__tstate_load(vm, &g_tstate);

   gc_tstate_resume();
   arm_lock_relock(ael_depth);
}

void dvm_gil_release(struct dvm *vm)
{
   if (!t_gil_depth) return;      /* not ours to release */
   if (--t_gil_depth) return;     /* still held by an outer acquire */
   gil_deep_hold_end(vm,gc_fast_ns()-((uint64_t)g_gil_held_since.tv_sec*1000000000+g_gil_held_since.tv_nsec));
   /* Still the owner here — g_gil.depth is only cleared below — so this reads
    * a VM no other thread can be touching. */
   dvm__tstate_save(vm, &g_tstate);
   g_tstate_valid = true;
   g_tstate_vm=vm;g_tstate_serial=vm->serial;
   gc_tstate_suspend(vm,&g_tstate);
   gc_stw_thread_parked(vm);
   pthread_mutex_lock(&g_gil.m);
   {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      atomic_fetch_add_explicit(&g_gil_held_ns,
                                gil_ns_since(&g_gil_held_since, &now),
                                memory_order_relaxed);
   }
   gil_obs_released(gc_fast_ns()-((uint64_t)g_gil_held_since.tv_sec*1000000000+g_gil_held_since.tv_nsec));
   struct dvm_gil_waiter *w = gil_choose_waiter(gc_fast_ns());
   if (w) {
      gil_handoff=w->tid;gil_handoff_since=gc_fast_ns();
      w->go = true;
      /* Wake both modes so a live switch cannot strand an existing waiter. */
      pthread_cond_broadcast(&g_gil.cv);
      pthread_cond_signal(&w->cv);
   } else {
      atomic_store_explicit(&g_gil.depth, 0, memory_order_release);
   }
   pthread_mutex_unlock(&g_gil.m);
}

unsigned dvm_gil_unlock_all(struct dvm *vm)
{
   unsigned d = t_gil_depth;
   if (d) {
      t_gil_depth = 1u;
      dvm_gil_release(vm);
   }
   return d;
}

void dvm_gil_relock(struct dvm *vm, unsigned depth)
{
   if (!depth) return;
   dvm_gil_acquire(vm);
   t_gil_depth = depth;
}

void dvm_gil_yield(struct dvm *vm)
{
   if (!atomic_load_explicit(&g_gil.waiters, memory_order_relaxed)) return;
   unsigned d = dvm_gil_unlock_all(vm);
   if (!d) return;
   /* No sched_yield(): dvm_gil_acquire() queues this thread behind everyone
    * who was already waiting, which is the hand-off this call is asking for. */
   dvm_gil_relock(vm, d);
}

/* LUNARIA_CALL_HISTO=1: which Java methods native code (or the scheduler)
 * calls most, top 12 every 10 seconds.  The counts are advisory: a lost
 * increment under a race costs a diagnostic one tick. */
static void call_histogram(const struct dvm_method *m)
{
   static int enabled = -1;
   static struct { const struct dvm_method *m; uint64_t n; } slot[512];
   static uint64_t last_report;
   if (enabled < 0) enabled = getenv("LUNARIA_CALL_HISTO") != NULL;
   if (!enabled) return;
   size_t h = ((uintptr_t)m >> 4) & 511;
   for (size_t k = 0; k < 512; ++k, h = (h + 1) & 511) {
      if (slot[h].m == m) { ++slot[h].n; break; }
      if (!slot[h].m) { slot[h].m = m; slot[h].n = 1; break; }
   }
   const uint64_t now = dvm__now_ms();
   if (!last_report) last_report = now;
   if (now - last_report < 10000) return;
   last_report = now;
   for (int pass = 0; pass < 12; ++pass) {
      size_t best = 512;
      for (size_t i = 0; i < 512; ++i)
         if (slot[i].m && (best == 512 || slot[i].n > slot[best].n)) best = i;
      if (best == 512 || !slot[best].n) break;
      fprintf(stderr, "[call-histo] %8llu %s.%s%s\n", (unsigned long long)slot[best].n,
              slot[best].m->cls ? slot[best].m->cls->name : "?", slot[best].m->name,
              slot[best].m->sig ? slot[best].m->sig : "");
      slot[best].n = 0;
   }
   fprintf(stderr, "[call-histo] ----\n");
}

bool dvm_call(struct dvm *vm, struct dvm_method *m, dvm_ref self,
              const union dvm_value *args, int nargs, union dvm_value *out)
{
   union dvm_value dummy;
   if (!out) out = &dummy;
   memset(out, 0, sizeof *out);
   if (!m || dvm_is_stopping(vm)) return false;

   uint32_t slots[256];
   int n = values_to_slots(m->sig, args, nargs, slots, 256);
   vm->exception = 0;
   /* Cleared on the way in, not on the way out: the unwind path belongs to the
    * call that just failed, and the caller (dvm__run_pending_threads(), which
    * reports a Runnable that died) needs to read it after invoke() returns. */
   vm->nexc_trace = 0;
   if (!vm->depth) { vm->call_steps = 0; vm->parked = false; }
   bool ok = invoke(vm, m, self, slots, n, out);
   /* The frame has returned, but pending dispatch can execute arbitrary Java
    * before the native caller gets the result. Root only that typed result;
    * wrapping the whole interpreted call in a C borrow would retain every
    * object touched by bytecode and prevent useful movement during waits. */
   const char *return_type=strchr(m->sig,')');
   dvm_gc_handle result_root=0;
   if(ok&&!vm->exception&&return_type&&(return_type[1]=='L'||return_type[1]=='[')&&out->l){
      result_root=dvm_gc_handle_new(vm,out->l,DVM_GC_HANDLE_STRONG);
      if(!result_root)ok=false;
   }
   if (!ok && vm->exception && !vm->quiet_uncaught) {
      char buf[512];
      dvm_describe_exception(vm, vm->exception, buf, sizeof buf);
      fprintf(stderr, "[dvm] uncaught %s in %s.%s\n", buf,
              m->cls ? m->cls->name : "?", m->name);
      for (int i = 0; i < vm->nexc_trace; ++i) {
         struct dvm_method *f = vm->exc_trace[i];
         if (i == 0)
            fprintf(stderr, "[dvm]   at %s.%s%s +0x%x\n",
                    f->cls ? f->cls->name : "?", f->name,
                    f->sig ? f->sig : "", vm->exc_pc * 2);
         else
            fprintf(stderr, "[dvm]   at %s.%s%s\n",
                    f->cls ? f->cls->name : "?", f->name, f->sig ? f->sig : "");
      }
   }
   /* The main Looper's queue is the main thread's to run.  A JNI call that a
    * guest's own native thread makes returns to that thread, not to a
    * Looper: running Handler messages there executed them on whichever
    * thread happened to call into Java — androidx.sqlite's ProcessLock then
    * re-entered itself from inside Thread.sleep and deadlocked the process on
    * its own file lock, and Play services' checkMainThread() failed. */
   /* A failed synchronous call returns its exception to its caller. Handler
    * dispatch invokes other Java methods, which clear the exception on entry;
    * it must wait until the caller has handled the pending exception. */
   if (!vm->depth && dvm_on_main_thread() && !vm->exception) {
      /* vm->parked describes the call that just returned — "this thread is
       * blocked inside the platform", which its caller has to act on.  The
       * drain below runs other Runnables and each clears the flag on its way
       * out, so a thread that parked was reported as having finished normally
       * and was dropped instead of re-queued.  Volley's NetworkDispatchers
       * disappeared exactly this way whenever the drain had anything else to
       * run, leaving later requests with no consumer. */
      const bool parked_here = vm->parked;
      dvm__run_pending_threads(vm);
      vm->parked = parked_here;
   }
   if(result_root){
      out->l=dvm_gc_handle_ref(vm,result_root);
      dvm_gc_handle_drop(vm,result_root);
   }
   return ok;
}

/* ------------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------------ */

/* Is anybody inside the VM at this instant?  Read without the mutex: the
 * profiler below only needs to tell work from idleness, and a sample that
 * lands exactly on a hand-off may go either way without changing the shape of
 * the answer. */
static bool gil_busy(void)
{
   return atomic_load_explicit(&g_gil.depth, memory_order_acquire) != 0u;
}

/* ---- Where the Java side spends its time (LUNARIA_DVM_PROF) --------------
 *
 * The interpreter-lock line in the [slice] report says how much of the wall
 * clock the VM is busy.  It does not say what it is busy with, and "the VM is
 * busy 98% of a loading screen" is a claim about the app's threads that has to
 * be checked before anything can be fixed.
 *
 * A sampler thread reads the method the interpreter is standing on every few
 * milliseconds and counts it.  The interpreter pays nothing for this beyond
 * the store to cur_method it already did, and only the sampler touches the
 * table, so there is no lock anywhere.  Methods and classes are freed only in
 * dvm_destroy(), so a sampled pointer stays valid for as long as the sampler
 * runs.
 *
 * A sample taken while nobody holds the interpreter lock is idle time: the
 * method pointer is then whatever ran last, and attributing to it would make
 * an idle VM look like a busy one.  Those are counted separately, so the
 * report distinguishes "Java is the bottleneck" from "Java is asleep".
 *
 * LUNARIA_DVM_PROF=<seconds between reports> (=1 means the 5 s default). */
#define DVM_PROF_SLOTS 4096u
static struct { struct dvm_method *m; unsigned long long n; } g_prof[DVM_PROF_SLOTS];
static unsigned long long g_prof_busy, g_prof_idle;

static void prof_count(struct dvm_method *m)
{
   size_t h = ((size_t)(uintptr_t)m >> 4) & (DVM_PROF_SLOTS - 1u);
   for (unsigned i = 0; i < DVM_PROF_SLOTS; ++i) {
      size_t k = (h + i) & (DVM_PROF_SLOTS - 1u);
      if (g_prof[k].m == m || !g_prof[k].m) {
         g_prof[k].m = m;
         ++g_prof[k].n;
         return;
      }
   }
   /* Table full: the count is a profile, not a ledger — drop the sample. */
}

static int prof_cmp(const void *a, const void *b)
{
   unsigned long long x = ((const struct { struct dvm_method *m;
                                           unsigned long long n; } *)a)->n;
   unsigned long long y = ((const struct { struct dvm_method *m;
                                           unsigned long long n; } *)b)->n;
   return x < y ? 1 : x > y ? -1 : 0;
}

static void prof_report(void)
{
   const unsigned long long total = g_prof_busy + g_prof_idle;
   if (!total) return;
   qsort(g_prof, DVM_PROF_SLOTS, sizeof g_prof[0], prof_cmp);
   {
      static unsigned long long steps_last, throws_last;
      const struct dvm *vm = g_gil_vm;
      const unsigned long long steps = vm ? vm->steps : 0ull;
      fprintf(stderr, "[dvmprof] %llu samples: %.0f%% in the VM, %.0f%% idle, "
              "%.1fM bytecodes/s, %llu exceptions\n",
              total, 100.0 * (double)g_prof_busy / (double)total,
              100.0 * (double)g_prof_idle / (double)total,
              (double)(steps - steps_last) / 1e6 /
                 ((double)total * 0.002),
              g_throws - throws_last);
      steps_last = steps;
      throws_last = g_throws;
      for (int shown = 0; shown < 5; ++shown) {
         int best = -1;
         for (int i = 0; i < DVM_THROW_TALLY; ++i)
            if (g_throw_tally[i].cls && g_throw_tally[i].n &&
                (best < 0 || g_throw_tally[i].n > g_throw_tally[best].n))
               best = i;
         if (best < 0) break;
         fprintf(stderr, "[dvmprof]   %8llu  %s thrown in %s.%s\n",
                 g_throw_tally[best].n, g_throw_tally[best].cls,
                 g_throw_tally[best].in, g_throw_tally[best].method);
         g_throw_tally[best].n = 0;
      }
      memset(g_throw_tally, 0, sizeof g_throw_tally);
   }
   for (unsigned i = 0; i < DVM_PROF_SLOTS && i < 15u; ++i) {
      if (!g_prof[i].m || !g_prof[i].n) break;
      const struct dvm_method *m = g_prof[i].m;
      fprintf(stderr, "[dvmprof]   %5.1f%%  %s.%s\n",
              100.0 * (double)g_prof[i].n / (double)total,
              m->cls && m->cls->name ? m->cls->name : "?", m->name);
   }
   memset(g_prof, 0, sizeof g_prof);
   g_prof_busy = g_prof_idle = 0;
}

static void *prof_thread(void *arg)
{
   const double every = *(const double *)arg;
   free(arg);
   struct timespec tick = { 0, 2 * 1000 * 1000 };   /* 2 ms */
   double waited = 0.0;
   for (;;) {
      nanosleep(&tick, NULL);
      waited += 0.002;
      struct dvm *vm = g_gil_vm;
      struct dvm_method *m =
         vm ? atomic_load_explicit(&vm->cur_method, memory_order_relaxed) : NULL;
      if (m && gil_busy()) { ++g_prof_busy; prof_count(m); }
      else                      ++g_prof_idle;
      if (waited >= every) { prof_report(); waited = 0.0; }
   }
   return NULL;
}

static void prof_start(void)
{
   const char *e = getenv("LUNARIA_DVM_PROF");
   if (!e || !*e || !strcmp(e, "0")) return;
   double every = atof(e);
   if (every < 0.5) every = 5.0;
   double *arg = malloc(sizeof *arg);
   if (!arg) return;
   *arg = every;
   pthread_t th;
   if (pthread_create(&th, NULL, prof_thread, arg) != 0) { free(arg); return; }
   pthread_detach(th);
   fprintf(stderr, "[dvmprof] sampling the interpreter every 2 ms, "
                   "reporting every %.1f s\n", every);
}

static pthread_once_t gc1_arena_test_once=PTHREAD_ONCE_INIT;
static bool gc1_arena_test_ok;
static void gc1_test_arena_once(void) { gc1_arena_test_ok=vm_arena_selftest(); }

struct dvm *dvm_create(const struct dvm_hooks *hooks)
{
   pthread_once(&gc1_arena_test_once,gc1_test_arena_once);
   if (!gc1_arena_test_ok) return NULL;
   /* Install the class-buffer write trap before the first class is
    * registered.  Idempotent; LUNARIA_CLASS_BUF_PROTECT=0 disables it. */
   class_buf_enable();
   if (!vm_arena_begin(vm_arena_configured_limit())) return NULL;
   FILE *heap_flag=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/heap-observe","r");int heap_on=0;
   if(heap_flag){if(fscanf(heap_flag,"%d",&heap_on)!=1)heap_on=0;fclose(heap_flag);}
   FILE *escape_flag=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/mem-escape-observe","r");int escape_on=0;
   if(escape_flag){(void)fscanf(escape_flag,"%d",&escape_on);fclose(escape_flag);}mem_escape_enabled=escape_on==1;pthread_mutex_lock(&mem_escape_lock);memset(&mem_escape,0,sizeof mem_escape);pthread_mutex_unlock(&mem_escape_lock);
   heap_creation_observe=false; /* Per-allocation diagnostic IO is forbidden on hot paths. */heap_creation_used=0;heap_creation_overflow=0;heap_creation_last=0;memset(heap_creation,0,sizeof heap_creation);
   struct dvm *vm = calloc(1, sizeof *vm);
   if (!vm) { vm_arena_end(); return NULL; }
   vm->gc_initializing=true;
   vm->process_start_elapsed_ms = (int64_t)(luna_os_boottime_ns() / 1000000ULL);
   vm->process_start_uptime_ms = (int64_t)(luna_os_uptime_ns() / 1000000ULL);
   vm->main_host_thread=luna_os_thread_token();
   atomic_init(&vm->stopping, false);
   atomic_init(&vm->process_exit_phase, 0);
   atomic_init(&vm->process_exit_status, 0);
   vm->io_cancel[0] = vm->io_cancel[1] = -1;
   if (luna_socket_pair(AF_UNIX, SOCK_STREAM, 0, vm->io_cancel) < 0) {
      free(vm);
      vm_arena_end();
      return NULL;
   }
   if (hooks) vm->hooks = *hooks;
   /* The thread that creates the VM is the process's main thread: it goes on
    * to launch the Activity and to drive the frame pump. */
   g_is_main_thread = true;
   /* No budget: a call on the main thread runs until it returns, as it does
    * on a device.  dvm_set_step_limit() sets one for diagnosis. */
   vm->step_limit = 0;

   const char *t = getenv("LUNARIA_DVM_TRACE");
   if (t) vm->trace = atoi(t);

   dvm_runtime_install(vm);
   /* Install class contracts after every runtime installer has created or
    * resolved its external classes.  This must not invent failure: the VM is
    * still fully usable if an optional contract cannot be confirmed. */
   if (!runtime_contract_vm_start(vm))
      fprintf(stderr, "[dvm] runtime contract not installed\n");
   g_gil_vm = vm;
   /* Candidate disables optional profiler workers; VM execution stays serialized. */
   /* The thread that creates the VM is the one the emulator runs its frame
    * pump and its guest CPU on.  It used to take the interpreter lock here and
    * keep it, giving it up only at chosen points — which bought the lock order
    * (interpreter outside, execution inside) for free, but meant the lock was
    * held 99-100% of the wall clock while the ARM JIT ran, so a Java thread
    * only ever ran in the gaps and a JNI call from the guest waited behind
    * every one of them (worst single wait measured: 416 ms).
    *
    * Now nobody holds it by default: each entry into the VM takes it for its
    * own duration (dvm_gil_enter_from_guest, which drops the execution lock
    * first so the order still holds).  The Java side and the guest CPU then
    * run at the same time on different host threads.
    * LUNARIA_DVM_GIL_HOLD=1 restores the old behaviour for comparison. */
   if (getenv("LUNARIA_DVM_GIL_HOLD")) {
      fprintf(stderr, "[dvm] pump holds the interpreter lock "
                      "(LUNARIA_DVM_GIL_HOLD)\n");
      dvm_gil_acquire(vm);
   }
   dvm_gil_acquire(vm);
   dvm__tstate_reset(vm);
   vm->cur_thread=thread_runtime_current(vm);
   /* VM lifetime serial for the corruption investigation.  malloc reuses an
    * address after dvm_destroy, so two VM_CREATE lines with the same vm=%p
    * are different VMs; class pointers cached across them belong to the
    * earlier instance.  The serial is printed, never stored, so no structure
    * layout changes. */
   {
      static uint64_t vm_serial;
      vm->serial = ++vm_serial;
      g_tstate_serial=vm->serial;
      fprintf(stderr, "VM_CREATE vm=%p serial=%llu\n", (void *)vm,
              (unsigned long long)vm->serial);
   }
   leak_observer_bootstrap(); /* before any guest execution */
   vm->gc_initializing=false;
   return vm;
}

struct gc_exec_selftest {
   struct dvm *vm;dvm_ref wanted;bool seen,ok;struct gc_exec_counts count;
};
static FILE *e1_root_log;
#define E1_ROOT_LOG(...) do {if(e1_root_log){dprintf(fileno(e1_root_log),__VA_ARGS__);fflush(e1_root_log);}}while(0)
static _Thread_local struct gc_exec_selftest *gc_exec_test;
static struct dvm_method gc_exec_fixture;
static void gc_exec_selftest_visit(dvm_ref *slot,void *user){
   struct gc_exec_selftest *s=user;if(slot&&*slot==s->wanted)s->seen=true;
}
static void gc_exec_selftest_probe(struct dvm *vm){
   struct gc_exec_selftest *s=gc_exec_test;
   if(!s||!gc_exec_frames||gc_exec_frames->frame->m!=&gc_exec_fixture)return;
   gc_exec_frames->frame->result=s->wanted;
   s->ok=gc_execution_roots(vm,gc_exec_selftest_visit,s,&s->count)&&s->seen&&
      s->count.entries&&s->count.frames&&s->count.registers&&s->count.results;
}
static void *gc_exec_selftest_handoff(void *data){
   struct gc_exec_selftest *s=data;dvm_gil_acquire(s->vm);
   s->ok=gc_execution_roots(s->vm,gc_exec_selftest_visit,s,&s->count)&&s->seen&&s->count.tstates;
   dvm_gil_release(s->vm);return NULL;
}
struct gc_depth_test {
   struct dvm_gc_native_scope *outer;uint32_t depth;dvm_ref nested;bool nested_ok,probe_ok;
   dvm_ref deferred;bool deferred_set,deferred_cleared,deferred_probe_ok;
};
static _Thread_local struct gc_depth_test *gc_depth_test;
static struct dvm_method gc_depth_guest,gc_depth_nested;
static struct dvm_method gc_depth_deferred_guest,gc_depth_deferred_builtin;
static bool gc_depth_return_deferred(struct dvm *vm,dvm_ref self,const union dvm_value *args,int n,union dvm_value *out){
   (void)self;(void)args;(void)n;struct gc_depth_test *test=gc_depth_test;if(!test)return false;
   test->deferred=dvm_new_array(vm,'B',"B",7);if(!test->deferred)return false;
   dvm__discard_temporary(vm,test->deferred);struct dvm_object *o=gc_peek_object(vm,test->deferred);
   test->deferred_set=o&&(o->gc_flags&DVM_GC_DEFERRED_FREE)&&o->gc_body_borrows&&o->gc_payload_borrows;
   out->l=test->deferred;return test->deferred_set;
}
static bool gc_depth_nested_builtin(struct dvm *vm,dvm_ref self,const union dvm_value *args,int n,union dvm_value *out){
   (void)self;(void)args;(void)n;(void)out;struct gc_depth_test *test=gc_depth_test;if(!test)return false;
   test->nested=dvm_new_array(vm,'B',"B",8);
   struct dvm_object *o=dvm__obj(vm,test->nested);
   test->nested_ok=o&&gc_current_scope!=test->outer&&gc_current_scope->guest_depth==gc_guest_execution_depth&&
      gc_guest_execution_depth==test->depth+1&&o->gc_body_borrows==1&&o->gc_payload_borrows==1;
   return test->nested_ok;
}
static void gc_depth_guest_probe(struct dvm *vm){
   struct gc_depth_test *test=gc_depth_test;
   if(test&&gc_exec_frames&&gc_exec_frames->frame->m==&gc_depth_deferred_guest){
      unsigned before=test->outer->count;union dvm_value out={0};
      bool called=dvm_call(vm,&gc_depth_deferred_builtin,0,NULL,0,&out);
      struct dvm_object *o=gc_peek_object(vm,out.l);
      test->deferred_cleared=o&&!(o->gc_flags&DVM_GC_DEFERRED_FREE)&&!o->gc_body_borrows&&!o->gc_payload_borrows;
      test->deferred_probe_ok=called&&test->deferred_set&&test->deferred_cleared&&before==test->outer->count;
      gc_exec_frames->frame->regs[1]=out.l;return;
   }
   if(!test||!gc_exec_frames||gc_exec_frames->frame->m!=&gc_depth_guest)return;
   unsigned before=test->outer->count;union dvm_value ignored={0};
   bool called=dvm_call(vm,&gc_depth_nested,0,NULL,0,&ignored);
   struct dvm_object *o=test->nested?dvm_gc_resolve(vm,heap_slot(vm,test->nested)):NULL;
   test->probe_ok=called&&test->nested_ok&&o&&o->live&&!o->gc_body_borrows&&!o->gc_payload_borrows&&
      test->outer->count==before;
}
static bool gc_depth_boundary_selftest(struct dvm *vm){
   if(gc_exec_test_probe||gc_depth_test||dvm_gc_has_native_scopes(vm)){E1_ROOT_LOG("DEPTH_GUARD probe=%d test=%d native_scopes=%d\n",gc_exec_test_probe!=NULL,gc_depth_test!=NULL,dvm_gc_has_native_scopes(vm));return false;}
   /* Real guest code: allocate int[1], read its length, repeat 32 times,
    * return the last array. The no-DEX new-array path uses real int arrays. */
   static const uint16_t insns[]={0x0012,0x1112,0x1223,0x0000,0x2321,0x00d8,0x0100,0x4034,0xfffb,0x0211};
   struct dvm_class *cls=dvm_find_class_or_external(vm,"java/lang/Object");
   gc_depth_guest=(struct dvm_method){.cls=cls,.name="gcDepthGuestAllocateRead",.sig="(I)[I",.access=DEX_ACC_STATIC,.has_code=true,.ret_kind='L'};
   gc_depth_guest.code.registers_size=5;gc_depth_guest.code.ins_size=1;gc_depth_guest.code.insns=insns;gc_depth_guest.code.insns_size=sizeof insns/sizeof insns[0];
   gc_depth_nested=(struct dvm_method){.cls=cls,.name="gcDepthNestedBorrow",.sig="()V",.access=DEX_ACC_STATIC,.ret_kind='V',.builtin=gc_depth_nested_builtin};
   if(!cls||!dvm_init_class(vm,cls))return false;
   dvm_ref old=dvm_new_array(vm,'B',"B",8);struct dvm_gc_native_scope outer={0};
   if(!old||!dvm_gc_native_enter(vm,&outer,0,"()V",NULL,0)){dvm_gc_native_leave(&outer);return false;}
   unsigned char *old_pointer=dvm_array_data(vm,old);if(old_pointer)old_pointer[0]=0x5a;
   unsigned before=outer.count;uint32_t heap_before=vm->heap_size;
   struct gc_depth_test test={.outer=&outer,.depth=gc_guest_execution_depth};gc_depth_test=&test;gc_exec_test_probe=gc_depth_guest_probe;
   union dvm_value arg={.i=32},result={0};bool called=dvm_call(vm,&gc_depth_guest,0,&arg,1,&result);
   static const uint16_t length_insns[]={0x1021,0x000f}; /* array-length v0,v1; return v0 */
   gc_depth_deferred_guest=(struct dvm_method){.cls=cls,.name="gcDeferredGuestArrayLength",.sig="()I",.access=DEX_ACC_STATIC,.has_code=true,.ret_kind='I'};
   gc_depth_deferred_guest.code.registers_size=2;gc_depth_deferred_guest.code.insns=length_insns;gc_depth_deferred_guest.code.insns_size=2;
   gc_depth_deferred_builtin=(struct dvm_method){.cls=cls,.name="gcDeferredNativeReturn",.sig="()[B",.access=DEX_ACC_STATIC,.ret_kind='L',.builtin=gc_depth_return_deferred};
   union dvm_value length={0};bool deferred_called=dvm_call(vm,&gc_depth_deferred_guest,0,NULL,0,&length);
   gc_exec_test_probe=NULL;gc_depth_test=NULL;
   struct dvm_object *kept=dvm_gc_resolve(vm,heap_slot(vm,old));
   struct dvm_object *returned=result.l?dvm_gc_resolve(vm,heap_slot(vm,result.l)):NULL;
   bool ok=called&&deferred_called&&length.i==7&&test.deferred_probe_ok&&!dvm_exception(vm)&&test.probe_ok&&before==1&&outer.count==before+1&&
      gc2_scope_contains(&outer,result.l)&&kept&&kept->gc_body_borrows==1&&kept->gc_payload_borrows==1&&
      old_pointer&&old_pointer[0]==0x5a&&returned&&returned->kind==DVM_OBJ_ARRAY&&returned->length==1&&
      returned->gc_body_borrows==1&&returned->gc_payload_borrows==1&&gc_guest_execution_depth==test.depth;
   E1_ROOT_LOG("DEPTH_BEFORE_EXIT ok=%d called=%d deferred_called=%d length=%d deferred_probe=%d nested_probe=%d before=%u after=%u result=%u returned_length=%u kept_body=%u kept_payload=%u returned_body=%u returned_payload=%u actual_depth=%u expected_depth=%u exception=%u\n",ok,called,deferred_called,length.i,test.deferred_probe_ok,test.probe_ok,before,outer.count,result.l,returned?returned->length:0,kept?kept->gc_body_borrows:0,kept?kept->gc_payload_borrows:0,returned?returned->gc_body_borrows:0,returned?returned->gc_payload_borrows:0,gc_guest_execution_depth,test.depth,dvm_exception(vm));
   for(uint32_t i=0;i<outer.count;i++){struct dvm_object *member=gc_peek_object(vm,outer.refs[i].ref);E1_ROOT_LOG("DEPTH_PARENT_MEMBER ref=%u old=%d returned=%d nested=%d deferred=%d class=%s\n",outer.refs[i].ref,outer.refs[i].ref==old,outer.refs[i].ref==result.l,outer.refs[i].ref==test.nested,outer.refs[i].ref==test.deferred,member&&member->cls?member->cls->name:"?");}
   unsigned after=outer.count;uint32_t heap_after=vm->heap_size;
   /* Same-depth handoff keeps deferred cleanup live until final C owner exit. */
   struct dvm_gc_native_scope child={0};bool child_ok=dvm_gc_native_enter(vm,&child,0,"()V",NULL,0);
   dvm_ref same=child_ok?dvm_new_array(vm,'B',"B",3):0;if(same)dvm__discard_temporary(vm,same);
   dvm_gc_native_leave_result(&child,"()[B",same);struct dvm_object *same_object=gc_peek_object(vm,same);
   bool same_deferred=same_object&&(same_object->gc_flags&DVM_GC_DEFERRED_FREE)&&same_object->gc_body_borrows&&gc2_scope_contains(&outer,same);
   dvm_gc_native_leave(&outer);
   E1_ROOT_LOG("DEPTH_AFTER_EXIT child_ok=%d same_deferred=%d same_live=%d kept_body=%u kept_payload=%u returned_body=%u returned_payload=%u\n",child_ok,same_deferred,gc_peek_object(vm,same)!=NULL,kept?kept->gc_body_borrows:0,kept?kept->gc_payload_borrows:0,returned?returned->gc_body_borrows:0,returned?returned->gc_payload_borrows:0);
   ok=ok&&child_ok&&same_deferred&&!gc_peek_object(vm,same)&&!kept->gc_body_borrows&&!kept->gc_payload_borrows&&returned&&!returned->gc_body_borrows&&!returned->gc_payload_borrows;
   dvm__discard_temporary(vm,old);if(test.nested)dvm__discard_temporary(vm,test.nested);if(test.deferred)dvm__discard_temporary(vm,test.deferred);if(result.l)dvm__discard_temporary(vm,result.l);
   fprintf(stderr,"GC2_GUEST_DEPTH_SELFTEST %s actual_dvm_call=1 guest_allocations=32 guest_array_length_reads=32 nested_c_borrow_released=%d outer_before=%u outer_after=%u expected_after=2 raw_pointer_preserved=1 returned_ref_handoff=1 heap_slots_delta=%u\n",ok?"PASS":"FAIL",test.probe_ok,before,after,heap_after-heap_before);
   fprintf(stderr,"GC2_DEFERRED_RETURN_SELFTEST %s discarded_inside_native=%d cross_depth_clear=%d real_guest_array_length=%d expected_length=7 same_depth_deferred_preserved=%d final_c_leave_reclaimed=%d\n",ok?"PASS":"FAIL",test.deferred_set,test.deferred_cleared,length.i,same_deferred,!gc_peek_object(vm,same));
   return ok;
}
#include "gc_e1_matrix.inc"
bool dvm_gc_execution_roots_selftest(struct dvm *vm){
   if(!vm||!dvm_gil_held(vm)||gc_exec_test_probe)return false;
   e1_root_log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/e1-execution-roots.txt","w");
   E1_ROOT_LOG("E1_ROOTS pid=%d real_vm_collection_enabled=0 real_vm_moving_enabled=0\n",getpid());
   struct gc_exec_counts before={0},after={0};struct gc_exec_selftest test={.vm=vm};
   bool ok=gc_execution_roots(vm,gc_exec_selftest_visit,&test,&before);
   dvm_ref object=dvm_new_object(vm,dvm_find_class_or_external(vm,"java/lang/Object"));
   static const uint16_t insns[]={0x0011}; /* real interpreter: return-object v0 */
   gc_exec_fixture=(struct dvm_method){.cls=dvm_object_class(vm,object),.name="gcExecutionRootsFixture",
      .sig="(Ljava/lang/Object;)Ljava/lang/Object;",.access=DEX_ACC_STATIC,.has_code=true,.ret_kind='L'};
   gc_exec_fixture.code.registers_size=1;gc_exec_fixture.code.ins_size=1;
   gc_exec_fixture.code.insns=insns;gc_exec_fixture.code.insns_size=1;
   test.wanted=object;gc_exec_test=&test;gc_exec_test_probe=gc_exec_selftest_probe;
   union dvm_value arg={.l=object},out={0};
   bool called=object&&dvm_call(vm,&gc_exec_fixture,0,&arg,1,&out);
   gc_exec_test_probe=NULL;gc_exec_test=NULL;
   ok=ok&&called&&out.l==object&&test.ok;
   E1_ROOT_LOG("FRAME_ASSERT ok=%d called=%d out=%u expected=%u seen=%d probe_ok=%d entries=%zu frames=%zu registers=%zu results=%zu\n",ok,called,out.l,object,test.seen,test.ok,test.count.entries,test.count.frames,test.count.registers,test.count.results);
   struct dvm_tstate saved;dvm__tstate_save(vm,&saved);
   struct gc_tstate_hold local;gc_tstate_local_enter(&local,vm,&saved);
   vm->exc_ref=object;
   struct gc_exec_selftest handoff={.vm=vm,.wanted=object};pthread_t thread;
   int created=pthread_create(&thread,NULL,gc_exec_selftest_handoff,&handoff);
   if(!created){unsigned depth=dvm_gil_unlock_all(vm);pthread_join(thread,NULL);dvm_gil_relock(vm,depth);}else ok=false;
   dvm__tstate_load(vm,&saved);gc_tstate_local_leave(&local);
   ok=ok&&!created&&handoff.ok;
   E1_ROOT_LOG("HANDOFF_ASSERT created=%d ok=%d seen=%d tstates=%zu\n",created,handoff.ok,handoff.seen,handoff.count.tstates);
   test.seen=false;
   ok=gc_execution_roots(vm,gc_exec_selftest_visit,&test,&after)&&ok&&
      before.entries==after.entries&&before.frames==after.frames&&before.tstates==after.tstates;
   fprintf(stderr,"GC2_EXECUTION_ROOTS_SELFTEST %s typed_entries=%zu actual_frames=%zu registers=%zu result_roots=%zu suspended_tstates=%zu handoff_seen=%d lifecycle_restored=%d conservative_regs=1\n",
      ok?"PASS":"FAIL",test.count.entries,test.count.frames,test.count.registers,test.count.results,
      handoff.count.tstates,handoff.seen,before.entries==after.entries&&before.frames==after.frames&&before.tstates==after.tstates);
   E1_ROOT_LOG("BALANCE_ASSERT before_entries=%zu after_entries=%zu before_frames=%zu after_frames=%zu before_tstates=%zu after_tstates=%zu ok=%d\n",before.entries,after.entries,before.frames,after.frames,before.tstates,after.tstates,ok);
   bool depth_ok=gc_depth_boundary_selftest(vm);
   bool matrix_ok=e1_nested_matrix_selftest(vm);
   E1_ROOT_LOG("E1_EXECUTION_ROOTS %s frame_tstate_ok=%d depth_ok=%d\n",ok&&depth_ok?"PASS":"FAIL",ok,depth_ok);
   if(e1_root_log){fclose(e1_root_log);e1_root_log=NULL;}
   return depth_ok&&ok&&matrix_ok;
}

/* Isolated hook fixtures: neither the installed application hook nor its user
 * context is changed after this selftest returns. */
struct gc_static_hook_test {
   dvm_ref object,raised;bool fail,nested_seen,nested_ok;unsigned calls;
};
static _Thread_local struct gc_static_hook_test *gc_static_hook_test;
static void gc_static_hook_visit(dvm_ref *slot,void *user)
{
   struct gc_static_hook_test *t=user;
   if(slot&&*slot==t->object)t->nested_seen=true;
}
static bool gc_static_hook_nested(struct dvm *vm,dvm_ref self,
   const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)args;(void)nargs;memset(out,0,sizeof *out);
   struct gc_static_hook_test *t=gc_static_hook_test;
   if(!t)return false;
   t->nested_ok=dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc_static_hook_visit,t)&&t->nested_seen;
   return t->nested_ok;
}
static bool gc_static_hook_fixture(void *user,struct dvm *vm,const char *cls,
   const char *field,const char *type,union dvm_value *out)
{
   (void)user;(void)cls;(void)field;(void)type;
   struct gc_static_hook_test *t=gc_static_hook_test;if(!t)return false;
   ++t->calls;
   if(t->fail){
      dvm__throw(vm,"java/lang/RuntimeException","gc static hook original exception");
      t->raised=dvm_exception(vm);return false;
   }
   t->object=dvm_new_string(vm,"gc-static-hook-reference");
   struct dvm_method nested={.cls=dvm_find_class_or_external(vm,"java/lang/Object"),
      .name="gcStaticHookNested",.sig="()V",.access=DEX_ACC_STATIC,
      .ret_kind='V',.builtin=gc_static_hook_nested};
   union dvm_value ignored={0};
   if(!t->object||!dvm_call(vm,&nested,0,NULL,0,&ignored))return false;
   out->l=t->object;return true;
}
bool dvm_gc_external_static_selftest(struct dvm *vm)
{
   if(!vm||!dvm_gil_held(vm)||dvm_exception(vm)||gc_static_hook_test)return false;
   struct dvm_hooks saved_hooks=vm->hooks;
   struct dvm_tstate saved_state;dvm__tstate_save(vm,&saved_state);
   struct gc_tstate_hold saved_hold;gc_tstate_local_enter(&saved_hold,vm,&saved_state);
   struct gc_static_hook_test test={0};struct dvm_gc_native_scope parent={0};
   union dvm_value out={0};bool returned=false,parent_seen=false,exception_preserved=false,sget_called=false;
   int saved_depth=vm->depth;
   /* The nested fixture is synchronous: do not run unrelated main-queue work
    * while the deliberately temporary test hook is installed. */
   ++vm->depth;
   gc_static_hook_test=&test;vm->hooks.get_external_static=gc_static_hook_fixture;
   bool entered=dvm_gc_native_enter(vm,&parent,0,"()V",NULL,0);
   if(entered){
      returned=gc_external_static(vm,"java/lang/Object","gcStaticMissing","Ljava/lang/String;",&out)&&out.l==test.object;
      test.nested_seen=false;
      parent_seen=returned&&dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc_static_hook_visit,&test)&&test.nested_seen;
   }
   bool nested_seen=test.nested_ok;
   dvm_gc_native_leave(&parent);
   /* Real sget-object dispatch through a minimal in-memory DEX field table.
    * Object has no such field, so this exercises the external-static failure
    * branch that used to overwrite the original exception with NoClassDef. */
   static const uint8_t field_image[16]={0,0,0,0,1,0,0,0,0,0,1,0,2,0,0,0};
   const char *strings[]={"Ljava/lang/Object;","Ljava/lang/String;","gcStaticMissing"};
   struct dvm_dex dex={0};
   dex.file=(struct dex_file){.p=field_image,.len=sizeof field_image,.strings=strings,
      .string_ids_size=3,.type_ids_size=2,.type_ids_off=0,.field_ids_size=1,.field_ids_off=8};
   struct dvm_dex **saved_dexes=vm->dexes;
   int saved_ndexes=vm->ndexes,saved_capacity=vm->dexes_cap;
   struct dvm_dex **temporary=malloc(((size_t)saved_ndexes+1u)*sizeof *temporary);
   if(temporary){
      if(saved_ndexes)memcpy(temporary,saved_dexes,(size_t)saved_ndexes*sizeof *temporary);
      temporary[saved_ndexes]=&dex;vm->dexes=temporary;
      vm->ndexes=saved_ndexes+1;vm->dexes_cap=saved_ndexes+1;
      static const uint16_t insns[]={0x0062,0x0000,0x0011};
      struct dvm_method getter={.cls=dvm_find_class_or_external(vm,"java/lang/Object"),
         .name="gcStaticMissingFixture",.sig="()Ljava/lang/String;",.access=DEX_ACC_STATIC,
         .dex=&dex.file,.has_code=true,.ret_kind='L'};
      getter.code.registers_size=1;getter.code.insns=insns;getter.code.insns_size=3;
      test.fail=true;vm->quiet_uncaught=true;
      sget_called=true;
      bool called=dvm_call(vm,&getter,0,NULL,0,&out);
      struct dvm_object *raised=gc_peek_object(vm,dvm_exception(vm));
      exception_preserved=!called&&test.raised&&dvm_exception(vm)==test.raised&&raised&&raised->cls&&
         !strcmp(raised->cls->name,"java/lang/RuntimeException");
      vm->dexes=saved_dexes;vm->ndexes=saved_ndexes;vm->dexes_cap=saved_capacity;free(temporary);
   }
   vm->hooks=saved_hooks;gc_static_hook_test=NULL;vm->depth=saved_depth;
   dvm__tstate_load(vm,&saved_state);gc_tstate_local_leave(&saved_hold);
   heap_free(vm,test.object);heap_free(vm,test.raised);
   bool ok=entered&&returned&&nested_seen&&parent_seen&&exception_preserved&&test.calls==2;
   fprintf(stderr,"GC2_EXTERNAL_STATIC_SELFTEST %s hook_calls=%u nested_root=%d parent_return_root=%d original_runtime_exception=%d hooks_restored=%d actual_sget=%d\n",
      ok?"PASS":"FAIL",test.calls,nested_seen,parent_seen,exception_preserved,
      vm->hooks.get_external_static==saved_hooks.get_external_static&&vm->hooks.user==saved_hooks.user,sget_called);
   return ok;
}

void dvm_destroy(struct dvm *vm)
{
   gil_observe_shutdown();
   dvm_lifetime_note(vm,"dvm_destroy");
   if (!vm) return;
   vm->gc_destroying=true;
   dvm_gc_fast_shutdown(vm);
   fprintf(stderr, "VM_DESTROY vm=%p nclasses=%d ndexes=%d\n", (void *)vm,
           vm->nclasses, vm->ndexes);
   executor_runtime_shutdown(vm,100);
   thread_runtime_shutdown(vm,100);
   dvm_request_stop(vm);
   dvm_threads_finish(vm);
   android_util_vm_end(vm);
   android_os_vm_end(vm);
   battery_runtime_finish(vm);
   small_boxes_vm_end(vm);
   extern void java_basics_integer_vm_end(struct dvm *);
   java_basics_integer_vm_end(vm);
   discovery_runtime_vm_end(vm);
   security_provider_vm_end(vm);
   extern void asset_reader_vm_end(struct dvm *);
   extern void locale_runtime_vm_end(struct dvm *);
   extern void platform_runtime_gc_vm_end(struct dvm *);
   extern void android_basics_gc_vm_end(struct dvm *);
   extern void package_metadata_gc_vm_end(struct dvm *);
   extern void activity_runtime_gc_vm_end(struct dvm *);
   extern void application_lifecycle_gc_vm_end(struct dvm *);
   extern void content_runtime_gc_vm_end(struct dvm *);
   extern void network_runtime_gc_vm_end(struct dvm *);
   asset_reader_vm_end(vm);
   locale_runtime_vm_end(vm);
   platform_runtime_gc_vm_end(vm);
   android_basics_gc_vm_end(vm);
   package_metadata_gc_vm_end(vm);
   activity_runtime_gc_vm_end(vm);
   application_lifecycle_gc_vm_end(vm);
   content_runtime_gc_vm_end(vm);
   network_runtime_gc_vm_end(vm);
   surface_runtime_vm_end(vm);
   ssl_runtime_cleanup();
   socket_runtime_cleanup();
   if(theme_runtime_vm_shutdown)theme_runtime_vm_shutdown(vm);
   if(surface_view_runtime_vm_shutdown)surface_view_runtime_vm_shutdown(vm);
   if(view_state_runtime_vm_shutdown)view_state_runtime_vm_shutdown(vm);
   if(layout_params_runtime_vm_shutdown)layout_params_runtime_vm_shutdown(vm);
   if(window_attributes_vm_shutdown)window_attributes_vm_shutdown(vm);
   if(display_runtime_vm_shutdown)display_runtime_vm_shutdown(vm);
   intent_filter_runtime_vm_end(vm);
   free(vm->shutdown_hooks);
   free(vm->network_callbacks);
   /* A pending apply() must reach the disk before the process goes away. */
   dvm_prefs_finish(vm);
   dvm_nsd_finish(vm);
   dvm_alarm_finish(vm);
   reflection_runtime_vm_end(vm);
   if(thread_extras_vm_end)thread_extras_vm_end(vm);
   if(weak_runtime_vm_end)weak_runtime_vm_end(vm);
   dvm_glsurface_finish(vm);
   luna_fd_close(vm->io_cancel[0]);
   luna_fd_close(vm->io_cancel[1]);

   free(vm->pending_threads);
   free(vm->pending_is_thread);
   free(vm->pending_due_ms);
   free(vm->pending_looper);
   free(vm->pending_owner);
   free(vm->pending_token);
   free(vm->active_object_animators);
   free(vm->layout_observers);

   gc_execution_vm_end(vm);
   dvm_gc_handles_destroy(vm);
   for (uint32_t i = 0; i < vm->heap_size; ++i) {
      struct dvm_object *o = heap_slot(vm, i + 1u);
      free(o->utf8);
      free(o->string_units);
      object_data_destroy(o);
      free(o->slots);
   }
   for (uint32_t b = 0; b < vm->heap_nblocks; ++b) free(vm->heap_blocks[b]);
   free(vm->heap_blocks);

   for (int i = 0; i < vm->nclasses; ++i) {
      struct dvm_class *c = vm->classes[i];
      dvm_gc_layout_destroy(c);
      /* Standalone declarations belong to their declaring class, never to
       * caches on inheriting classes. Free before the class's method array. */
      for (uint32_t k = 0; k < c->mcache_cap; ++k) {
         struct dvm_method *extra = c->mcache[k].hash ? c->mcache[k].m : NULL;
         if (extra && extra->cls == c && extra->sig &&
             platform_extra_supported(c->name, extra->name, extra->sig) &&
             !class_own_method(c, extra->name, extra->sig)) {
            free((void *)extra->name); free(extra->sig); free(extra);
         }
      }
      for (int k = 0; k < c->nmethods; ++k)
         free(c->methods[k].sig);
      free(c->methods);
      free(c->mcache);
      free(c->ifields);
      free(c->sfields);
      free(c->sslots);
      free(c->ifaces);
      class_buf_free(c->desc);
      class_buf_free(c->name);
      free(c);
   }
   free(vm->classes);

   for (int i = 0; i < vm->ndexes; ++i) {
      free(vm->dexes[i]->type_cache);
      free(vm->dexes[i]->method_cache);
      if (vm->dexes[i]->proto_cache)
         for (uint32_t j = 0; j < vm->dexes[i]->file.proto_ids_size; ++j)
            free(vm->dexes[i]->proto_cache[j]);
      free(vm->dexes[i]->proto_cache);
      free(vm->dexes[i]->field_cache);
      free(vm->dexes[i]->string_cache);
      dex_close(&vm->dexes[i]->file);
      free(vm->dexes[i]);
   }
   free(vm->dexes);

   /* intern keys alias their pinned String objects' utf8 storage. */
   free(vm->interns);

   for (int i = 0; i < vm->nmissing; ++i)
      free(vm->missing[i]);
   free(vm->missing);

   if (g_gil_vm == vm) g_gil_vm = NULL;
   dvm_gil_release(vm);
   free(vm);
   vm_arena_end();
}

bool dvm_add_dex(struct dvm *vm, const char *path)
{
   if (vm->ndexes == vm->dexes_cap) {
      int cap = vm->dexes_cap ? vm->dexes_cap * 2 : 8;
      struct dvm_dex **n = realloc(vm->dexes, (size_t)cap * sizeof *n);
      if (!n) return false;
      vm->dexes = n;
      vm->dexes_cap = cap;
   }
   struct dvm_dex *dd = calloc(1, sizeof *dd);
   if (!dd) return false;
   if (!dex_open(&dd->file, path)) {
      free(dd);
      return false;
   }

   dd->type_cache   = calloc(dd->file.type_ids_size + 1u, sizeof *dd->type_cache);
   dd->method_cache = calloc(dd->file.method_ids_size + 1u, sizeof *dd->method_cache);
   dd->proto_cache = calloc(dd->file.proto_ids_size + 1u, sizeof *dd->proto_cache);
   dd->field_cache  = calloc(dd->file.field_ids_size + 1u, sizeof *dd->field_cache);
   dd->string_cache = calloc(dd->file.string_ids_size + 1u, sizeof *dd->string_cache);
   if (!dd->type_cache || !dd->method_cache || !dd->proto_cache || !dd->field_cache || !dd->string_cache) { free(dd->type_cache); free(dd->method_cache); free(dd->proto_cache); free(dd->field_cache); free(dd->string_cache); dex_close(&dd->file); free(dd); return false; }

   vm->dexes[vm->ndexes++] = dd;
   fprintf(stderr, "[dvm] loaded %s: %u classes, %u methods\n",
           path, dd->file.class_defs_size, dd->file.method_ids_size);
   /* The boot card's only phase with a real denominator.  Reported after the
    * parse, because that is the work the person watching was waiting on. */
   {
      struct stat st;
      luna_boot_dex_loaded(path, dd->file.class_defs_size,
                           dd->file.method_ids_size,
                           stat(path, &st) == 0 ? (uint64_t)st.st_size : 0);
      (void)arm_exec_boot_present();
   }
   return true;
}

bool dvm_add_dex_memory(struct dvm *vm, const void *data, size_t len,
                        const char *name)
{
   if (!vm || !data || len < 112u /* dex header */) return false;
   if (memcmp(data, "dex\n", 4) != 0) {
      fprintf(stderr, "[dvm] %s: not a dex (magic %.4s)\n",
              name ? name : "in-memory dex", (const char *)data);
      return false;
   }
   if (vm->ndexes == vm->dexes_cap) {
      int cap = vm->dexes_cap ? vm->dexes_cap * 2 : 8;
      struct dvm_dex **n = realloc(vm->dexes, (size_t)cap * sizeof *n);
      if (!n) return false;
      vm->dexes = n;
      vm->dexes_cap = cap;
   }
   struct dvm_dex *dd = calloc(1, sizeof *dd);
   if (!dd) return false;
   /* The buffer belongs to whoever handed it over (guest memory, for an
    * InMemoryDexClassLoader), so keep our own copy for dex_close() to free. */
   uint8_t *copy = malloc(len);
   if (!copy) { free(dd); return false; }
   memcpy(copy, data, len);
   if (!dex_open_memory(&dd->file, copy, len, name ? name : "in-memory dex")) {
      free(copy);
      free(dd);
      return false;
   }
   dd->file.owned = copy;

   dd->type_cache   = calloc(dd->file.type_ids_size + 1u, sizeof *dd->type_cache);
   dd->method_cache = calloc(dd->file.method_ids_size + 1u, sizeof *dd->method_cache);
   dd->proto_cache = calloc(dd->file.proto_ids_size + 1u, sizeof *dd->proto_cache);
   dd->field_cache  = calloc(dd->file.field_ids_size + 1u, sizeof *dd->field_cache);
   dd->string_cache = calloc(dd->file.string_ids_size + 1u, sizeof *dd->string_cache);
   if (!dd->type_cache || !dd->method_cache || !dd->proto_cache || !dd->field_cache || !dd->string_cache) { free(dd->type_cache); free(dd->method_cache); free(dd->proto_cache); free(dd->field_cache); free(dd->string_cache); dex_close(&dd->file); free(dd); return false; }

   vm->dexes[vm->ndexes++] = dd;
   fprintf(stderr, "[dvm] loaded %s (%zu bytes): %u classes, %u methods\n",
           name ? name : "in-memory dex", len,
           dd->file.class_defs_size, dd->file.method_ids_size);
   return true;
}

int dvm_add_apk_dir(struct dvm *vm, const char *dir)
{
   static const char *const subdirs[] = { "", "base/" };
   int loaded = 0;
   /* A first pass that only stats.  The boot card needs the total before the
    * first file is parsed, and the files differ by megabytes, so a bar
    * counting files rather than bytes would move in uneven jumps. */
   {
      int files = 0;
      uint64_t bytes = 0;
      for (size_t s = 0; s < sizeof subdirs / sizeof subdirs[0]; ++s) {
         for (int i = 0; i < 64; ++i) {
            char path[1024];
            struct stat st;
            if (i == 0) snprintf(path, sizeof path, "%s/%sclasses.dex", dir, subdirs[s]);
            else snprintf(path, sizeof path, "%s/%sclasses%d.dex", dir, subdirs[s], i + 1);
            if (stat(path, &st) != 0) {
               if (i == 0) continue;
               break;
            }
            ++files;
            bytes += (uint64_t)st.st_size;
         }
      }
      luna_boot_dex_total(files, bytes);
      (void)arm_exec_boot_present();
   }
   for (size_t s = 0; s < sizeof subdirs / sizeof subdirs[0]; ++s) {
      for (int i = 0; i < 64; ++i) {
         char path[1024];
         if (i == 0) snprintf(path, sizeof path, "%s/%sclasses.dex", dir, subdirs[s]);
         else snprintf(path, sizeof path, "%s/%sclasses%d.dex", dir, subdirs[s], i + 1);
         if (!dvm_add_dex(vm, path)) {
            if (i == 0) continue;   /* some APKs start at classes2.dex */
            break;
         }
         ++loaded;
      }
   }
   return loaded;
}

void dvm_set_trace(struct dvm *vm, int level) { if (vm) vm->trace = level; }
uint64_t dvm_instructions(const struct dvm *vm) { return vm ? vm->steps : 0; }
void dvm_set_step_limit(struct dvm *vm, uint64_t limit) { if (vm) vm->step_limit = limit; }

/* Fixed interpreter microbenchmark shared by GC stages. This measures a
 * synthetic guest integer loop, never Unity frame rate or collection pause.
 * Keep the bytecode and 8 x 2000 workload identical between stage labels. */
bool dvm_gc_microbenchmark(struct dvm *vm,const char *path,const char *stage)
{
   if(!vm||!path||!stage||!dvm_gil_held(vm)||vm->depth||vm->exception)return false;
   for(const char *p=stage;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return false;
   /* v0=0; do { ++v0; } while(v0<v1); return v0. v1 is argument 2000.
    * 4002 executed DEX instructions per call; no C fast path implements it. */
   static const uint16_t insns[]={0x0012,0x00d8,0x0100,0x1034,0xfffe,0x000f};
   static struct dvm_method method;
   method=(struct dvm_method){.cls=dvm_find_class_or_external(vm,"java/lang/Object"),
      .name="gcIntegerLoopMicrobenchmarkV1",.sig="(I)I",.access=DEX_ACC_STATIC,.has_code=true,.ret_kind='I'};
   method.code.registers_size=2;method.code.ins_size=1;method.code.insns_size=sizeof insns/sizeof insns[0];method.code.insns=insns;
   if(!method.cls||!dvm_init_class(vm,method.cls))return false;
   struct vm_arena_region_snapshot memory={0};if(!vm_arena_region_snapshot(&memory)||!memory.active)return false;
   size_t sampled_live_peak=memory.live_bytes;
   uint64_t saved_limit=vm->step_limit,saved_charged=vm->call_steps;
   /* Respect a smaller caller budget; an unlimited caller is tightened.
    * No quota is increased, and all caller accounting is restored below. */
   vm->step_limit=saved_limit&&saved_limit<100000?saved_limit:100000;
   uint64_t benchmark_limit=vm->step_limit,executed_before=vm->steps,charged=0;
   unsigned calls=0;bool ok=true;struct timespec begin,end;
   if(clock_gettime(CLOCK_MONOTONIC,&begin)){vm->step_limit=saved_limit;return false;}
   for(unsigned i=0;i<8;++i){
      union dvm_value arg={.i=2000},out={0};bool called=dvm_call(vm,&method,0,&arg,1,&out);
      charged+=vm->call_steps;++calls;
      bool within=vm->call_steps<100000;
      if(!called||vm->exception||out.i!=2000||!within){ok=false;fprintf(stderr,"GC_MICROBENCH_FAILURE stage=%s call=%u called=%d result=%d expected=2000 charged_steps=%llu limit=%llu exception=%u\n",stage,calls,called,out.i,(unsigned long long)vm->call_steps,(unsigned long long)benchmark_limit,vm->exception);break;}
      if(!vm_arena_region_snapshot(&memory)||!memory.active){ok=false;break;}
      if(memory.live_bytes>sampled_live_peak)sampled_live_peak=memory.live_bytes;
   }
   if(clock_gettime(CLOCK_MONOTONIC,&end)){vm->step_limit=saved_limit;vm->call_steps=saved_charged;return false;}
   uint64_t executed=vm->steps-executed_before;
   double seconds=(double)(end.tv_sec-begin.tv_sec)+(double)(end.tv_nsec-begin.tv_nsec)/1000000000.0;
   vm->step_limit=saved_limit;vm->call_steps=saved_charged;
   FILE *file=fopen(path,"a+");if(!file)return false;
   bool written=fseek(file,0,SEEK_END)==0;
   if(written&&ftell(file)==0)written=dprintf(fileno(file),"pid,stage,workload,calls,iterations_per_call,expected_dex,executed_dex,charged_steps,wall_seconds,heap_peak,heap_peak_basis,sampled_heap_peak_live_bytes,pause_ms,ok,unity_frames_measured\n")>=0;
   if(written)written=dprintf(fileno(file),"%d,%s,dex_int_loop_v1_8x2000,%u,2000,32016,%llu,%llu,%.9f,%zu,process_peak_committed_bytes,%zu,NA,%u,0\n",getpid(),stage,calls,(unsigned long long)executed,(unsigned long long)charged,seconds,memory.peak_committed_bytes,sampled_live_peak,(unsigned)(ok&&calls==8))>=0;
   if(fclose(file))written=false;
   fprintf(stderr,"GC_MICROBENCH %s stage=%s calls=%u iterations_per_call=2000 executed_dex=%llu charged_steps=%llu wall_seconds=%.9f heap_peak=%zu heap_peak_basis=process_peak_committed_bytes pause_ms=NA unity_frames_measured=0\n",ok&&calls==8&&written?"PASS":"FAIL",stage,calls,(unsigned long long)executed,(unsigned long long)charged,seconds,memory.peak_committed_bytes);
   return ok&&calls==8&&written;
}
