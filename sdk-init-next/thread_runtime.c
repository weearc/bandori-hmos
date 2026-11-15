/* SPDX-License-Identifier: MPL-2.0
 * Adapted from pinned Lunaria bytecode_thread_main/ht_run/looper_loop boundary;
 * see thread-runtime-provenance.json and generated/LICENSE.
 * Minimal actual HandlerThread/Looper worker. VM entry follows upstream
 * bytecode_thread_main: mark -> GIL acquire -> TLS reset -> cur_thread -> call.
 * Queue ownership, publication and termination are guarded independently;
 * never acquire GIL while holding the queue mutex. Workers are owned by the
 * VM and joined before VM teardown. No main/UI event loop is provided. */
#include "thread_runtime.h"
#include "executor_runtime.h"
#include "portable.h"
#include "gc_runtime.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/syscall.h>
extern bool android_basics_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
struct jni_bridge;
extern struct jni_bridge *jni_bridge_for_vm(struct dvm *) __attribute__((weak));
extern bool jni_bridge_thread_is_attached(struct jni_bridge *) __attribute__((weak));
extern bool jni_bridge_detach_current(struct jni_bridge *) __attribute__((weak));
static void detach_worker_jni(struct dvm *v){
 if(jni_bridge_for_vm&&jni_bridge_thread_is_attached&&jni_bridge_detach_current){
  struct jni_bridge *b=jni_bridge_for_vm(v);
  if(b&&jni_bridge_thread_is_attached(b)) (void)jni_bridge_detach_current(b);
 }
}
struct handler{dvm_ref looper,callback;bool async;};
struct task{dvm_ref handler,runnable,message;uint64_t due,sequence;struct task *next;};
struct worker{struct dvm *vm;dvm_ref self,name,looper;uint64_t looper_identity;pthread_t thread;bool started,ready,alive,quit,joined,active,plain,loop_failed;int priority;bool priority_attempted,priority_applied;int priority_errno,priority_nice;struct task *head,*gc_inflight,*gc_retiring;unsigned queued,executed,cancelled;struct worker *next;};
struct plain_thread { struct dvm *vm; dvm_ref self,target,name,group; uint64_t identity; bool daemon,started,alive; int priority; struct plain_thread *next; };
static void worker_payload_release(void *data){struct worker *w=data;if(w&&!w->started)free(w);}
static struct plain_thread *plain_threads;
static uint64_t thread_ref_identity(struct dvm *v,dvm_ref ref){struct dvm_object *o=dvm__obj(v,ref);return o?o->identity_generation:0;}
static struct plain_thread *plain_find(struct dvm *v,dvm_ref self){uint64_t identity=thread_ref_identity(v,self);for(struct plain_thread *t=plain_threads;t;t=t->next)if(t->vm==v&&t->self==self&&identity&&t->identity==identity)return t;return NULL;}
bool thread_runtime_executor_bind(struct dvm *v,dvm_ref self,dvm_ref target){
 struct plain_thread *t=plain_find(v,self);struct dvm_object *o=dvm__obj(v,self);
 if(!t||!o||strcmp(o->cls->name,"java/lang/Thread")||t->started||t->target!=target){dvm__throw(v,"java/lang/UnsupportedOperationException","factory must return a NEW plain Thread with the worker target");return false;}
 t->started=t->alive=true;return true;
}
void thread_runtime_executor_finished(struct dvm *v,dvm_ref self){struct plain_thread *t=plain_find(v,self);if(t)t->alive=false;}
static struct {struct dvm *vm;pthread_t main_host;dvm_ref main_java;struct worker *workers;uint64_t sequence;} run;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
enum message_lifetime {MESSAGE_OWNED,MESSAGE_IN_USE,MESSAGE_POOLED,MESSAGE_RETIRED};
struct message_lifetime_state {enum message_lifetime state;};
static struct {struct dvm *vm;dvm_ref refs[50];unsigned count;uint64_t hits,created,recycled;} message_pool;
static void message_state_free(void *p){free(p);}
static struct message_lifetime_state *message_state(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||strcmp(o->cls->name,"android/os/Message")){dvm__throw(v,"java/lang/IllegalArgumentException","Message receiver");return NULL;}
 if(o->data_destroy==message_state_free)return o->data;
 if(o->data){dvm__throw(v,"java/lang/IllegalStateException","Message payload ownership");return NULL;}
 struct message_lifetime_state *s=calloc(1,sizeof *s);
 if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","Message lifetime");return NULL;}
 o->data=s;o->data_destroy=message_state_free;return s;
}
static dvm_ref message_obtain(struct dvm *v){
 if(message_pool.vm!=v){memset(&message_pool,0,sizeof message_pool);message_pool.vm=v;}
 if(message_pool.count){dvm_ref ref=message_pool.refs[--message_pool.count];struct message_lifetime_state *s=message_state(v,ref);if(!s)return 0;s->state=MESSAGE_OWNED;dvm_unpin(v,ref);message_pool.hits++;return ref;}
 dvm_ref ref=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Message"));
 if(!ref||!message_state(v,ref)){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Message");return 0;}
 message_pool.created++;return ref;
}
/* Looper owns an enqueued Message until dispatch/cancellation ends. Android
 * permits recycling these actual objects; it does not permit destroying an
 * arbitrary escaped guest reference. Only slots are cleared, without guest
 * calls, so cleanup cannot overwrite the callback's pending exception. */
static void message_recycle_unchecked(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);if(!o||o->data_destroy!=message_state_free)return;
 struct message_lifetime_state *s=o->data;if(s->state==MESSAGE_POOLED||s->state==MESSAGE_RETIRED)return;
 if(s->state==MESSAGE_IN_USE)dvm_unpin(v,ref);
 if(o->slots)memset(o->slots,0,(size_t)o->cls->islots*sizeof *o->slots);
 s->state=MESSAGE_RETIRED;
 if(message_pool.vm==v&&message_pool.count<50){s->state=MESSAGE_POOLED;message_pool.refs[message_pool.count++]=ref;dvm_pin(v,ref);}
 message_pool.recycled++;
 if((message_pool.recycled%4096)==1){FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){fprintf(f,"ENGINE_LOG_MESSAGE_POOL pid=%d created=%llu hits=%llu recycled=%llu pooled=%u limit=50\n",getpid(),(unsigned long long)message_pool.created,(unsigned long long)message_pool.hits,(unsigned long long)message_pool.recycled,message_pool.count);fclose(f);}}
}
static void choreographer_install(struct dvm *v);
static void choreographer_shutdown(struct dvm *v);
static bool choreographer_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static struct timespec deadline(unsigned ms){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_nsec+=(long)(ms%1000)*1000000;t.tv_sec+=ms/1000+t.tv_nsec/1000000000;t.tv_nsec%=1000000000;return t;}
/* Android's Process.setThreadPriority and HandlerThread(String,int) pass their
 * int argument through to Linux nice unchanged.  setpriority(PRIO_PROCESS,0,)
 * targets the calling thread on Linux/OHOS.  Android rejects values outside
 * the kernel's [-20,19] range before calling the kernel; Linux may silently
 * clamp them, so validate here and verify the observed value after the call. */
#define THREAD_PRIORITY_MIN (-20)
#define THREAD_PRIORITY_MAX 19
static bool thread_priority_valid(int priority){return priority>=THREAD_PRIORITY_MIN&&priority<=THREAD_PRIORITY_MAX;}
static bool thread_priority_get_for(id_t tid,int *priority,int *error){
 errno=0;int got=getpriority(PRIO_PROCESS,tid);
 if(got==-1&&errno){*error=errno?errno:EINVAL;return false;}
 *priority=got;*error=0;return true;
}
static bool thread_priority_get(int *priority,int *error){
 return thread_priority_get_for(0,priority,error);
}
static bool thread_priority_apply_for(id_t tid,int priority,int *error,int *observed){
 errno=0;
 if(setpriority(PRIO_PROCESS,tid,priority)!=0){*error=errno?errno:EINVAL;return false;}
 return thread_priority_get_for(tid,observed,error);
}
static bool thread_priority_apply(int priority,int *error,int *observed){
 return thread_priority_apply_for(0,priority,error,observed);
}
/* Java Thread priority (1..10) -> Linux nice, matching ART's kNiceValues
 * table in Thread.cc: LOWEST(19), BACKGROUND+6/3/0 (16/13/10), NORMAL(0),
 * NORMAL-2/-4 (-2/-4), URGENT_DISPLAY+3/+2/0 (-5/-6/-8).  Thread.setPriority()
 * stores the Java value even when the process may not lower its nice; Android
 * ignores the kernel's EPERM here, so executor workers log and keep running. */
static int java_priority_nice(int priority){
 static const int nice[10]={19,16,13,10,0,-2,-4,-5,-6,-8};
 return priority>=1&&priority<=10?nice[priority-1]:0;
}
/* Called by executor_worker_main after a ThreadFactory-created Thread has been
 * bound.  Returns false with *java_priority==0 when the Thread has no plain
 * runtime state (the internal default worker), and false with a non-zero Java
 * priority when the kernel refused the mapped nice value. */
bool thread_runtime_executor_apply_priority(struct dvm *v,dvm_ref self,int *java_priority,int *error,int *observed){
 struct plain_thread *t=plain_find(v,self);
 if(!t){if(java_priority)*java_priority=0;return false;}
 int priority=(t->priority>=1&&t->priority<=10)?t->priority:5;
 if(java_priority)*java_priority=priority;
 return thread_priority_apply(java_priority_nice(priority),error,observed);
}
static bool is_thread(struct dvm *v,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)if(!strcmp(c->name,"android/os/HandlerThread"))return true;return false;}
static void message_field(struct dvm_class *c,int index,const char *name,const char *type){struct dvm_field *f=&c->ifields[index];f->cls=c;f->name=name;f->type=type;f->slot=(uint16_t)index;f->width=1;f->kind=type[0]=='L'?'L':type[0];}
/* Thread.getStackTrace() is capped at 64 entries: one synthetic
 * Thread.getStackTrace frame plus at most 63 real interpreter frames. */
#define THREAD_STACK_TRACE_MAX 64
#define THREAD_STACK_TRACE_REAL (THREAD_STACK_TRACE_MAX-1)
static bool stack_trace_element_install(struct dvm_class *c,int index,const char *name,const char *sig,dvm_builtin_fn fn,char ret,int args){
 struct dvm_method *m=&c->methods[index];
 m->cls=c;m->name=name;m->sig=strdup(sig);m->builtin=fn;m->ret_kind=ret;m->arg_count=args;m->arg_slots=args;
 return m->sig!=NULL;
}
static bool stack_trace_element_get(struct dvm *v,dvm_ref self,const char *name,const char *type,union dvm_value *out){
 if(!self){dvm__throw(v,"java/lang/NullPointerException","StackTraceElement receiver");return false;}
 if(!dvm_get_field(v,self,name,type,out)){dvm__throw(v,"java/lang/IllegalStateException","StackTraceElement.%s missing",name);return false;}
 return true;
}
static bool ste_getClassName(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)stack_trace_element_get(v,self,"declaringClass","Ljava/lang/String;",out);return true;
}
static bool ste_getMethodName(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)stack_trace_element_get(v,self,"methodName","Ljava/lang/String;",out);return true;
}
static bool ste_getFileName(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)stack_trace_element_get(v,self,"fileName","Ljava/lang/String;",out);return true;
}
static bool ste_getLineNumber(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)stack_trace_element_get(v,self,"lineNumber","I",out);return true;
}
static bool ste_toString(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;
 union dvm_value klass={0},method={0},file={0},line={0};
 if(!stack_trace_element_get(v,self,"declaringClass","Ljava/lang/String;",&klass)||
    !stack_trace_element_get(v,self,"methodName","Ljava/lang/String;",&method)||
    !stack_trace_element_get(v,self,"fileName","Ljava/lang/String;",&file)||
    !stack_trace_element_get(v,self,"lineNumber","I",&line))return true;
 const char *cn=dvm_string_utf8(v,klass.l);if(!cn)cn="null";
 const char *mn=dvm_string_utf8(v,method.l);if(!mn)mn="null";
 const char *fn=file.l?dvm_string_utf8(v,file.l):NULL;
 size_t need=strlen(cn)+strlen(mn)+(fn?strlen(fn):0)+32;
 char *buf=malloc(need);
 if(!buf){dvm__throw(v,"java/lang/OutOfMemoryError","StackTraceElement.toString");return true;}
 int written;
 if(line.i==-2)written=snprintf(buf,need,"%s.%s(Native Method)",cn,mn);
 else if(fn&&line.i>=0)written=snprintf(buf,need,"%s.%s(%s:%d)",cn,mn,fn,line.i);
 else if(fn)written=snprintf(buf,need,"%s.%s(%s)",cn,mn,fn);
 else written=snprintf(buf,need,"%s.%s(Unknown Source)",cn,mn);
 dvm_ref s=written>=0&&(size_t)written<need?dvm_new_string(v,buf):0;
 free(buf);
 if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","StackTraceElement.toString result");return true;}
 out->l=s;return true;
}
static bool ste_init(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 if(!self){dvm__throw(v,"java/lang/NullPointerException","StackTraceElement receiver");return true;}
 if(n!=4||!a){dvm__throw(v,"java/lang/IllegalArgumentException","StackTraceElement constructor arity %d",n);return true;}
 /* Java permits a null fileName but constructing an element without a
  * declaring class or method name is a NullPointerException. */
 if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","declaringClass");return true;}
 if(!a[1].l){dvm__throw(v,"java/lang/NullPointerException","methodName");return true;}
 union dvm_value val;bool ok=true;
 val.l=a[0].l;ok=ok&&dvm_set_field(v,self,"declaringClass","Ljava/lang/String;",val);
 val.l=a[1].l;ok=ok&&dvm_set_field(v,self,"methodName","Ljava/lang/String;",val);
 val.l=a[2].l;ok=ok&&dvm_set_field(v,self,"fileName","Ljava/lang/String;",val);
 val.i=a[3].i;ok=ok&&dvm_set_field(v,self,"lineNumber","I",val);
 if(!ok){dvm__throw(v,"java/lang/IllegalStateException","StackTraceElement fields unavailable");return true;}
 return true;
}
static dvm_ref stack_trace_element_new(struct dvm *v,dvm_ref declaring,dvm_ref method,dvm_ref file,int32_t line){
 struct dvm_class *c=dvm__class_by_desc(v,"Ljava/lang/StackTraceElement;");
 if(!c){dvm__throw(v,"java/lang/IllegalStateException","StackTraceElement class unavailable");return 0;}
 dvm_ref e=dvm_new_object(v,c);
 if(!e){dvm__throw(v,"java/lang/OutOfMemoryError","StackTraceElement");return 0;}
 union dvm_value val;bool ok=true;
 val.l=declaring;ok=ok&&dvm_set_field(v,e,"declaringClass","Ljava/lang/String;",val);
 val.l=method;ok=ok&&dvm_set_field(v,e,"methodName","Ljava/lang/String;",val);
 val.l=file;ok=ok&&dvm_set_field(v,e,"fileName","Ljava/lang/String;",val);
 val.i=line;ok=ok&&dvm_set_field(v,e,"lineNumber","I",val);
 if(!ok){dvm__discard_temporary(v,e);dvm__throw(v,"java/lang/IllegalStateException","StackTraceElement fields unavailable");return 0;}
 return e;
}
/* A real frame keeps only the class/method the VM actually executed.  The DEX
 * carries no source-file or line mapping in this runtime, so fileName stays
 * null and lineNumber -1 (StackTraceElement's "unavailable" value); neither is
 * invented from the dex index. */
static dvm_ref thread_stack_element(struct dvm *v,const char *class_name,const char *method_name){
 if(!class_name||!method_name)return 0;
 size_t n=strlen(class_name);
 char *dotted=malloc(n+1);
 if(!dotted){dvm__throw(v,"java/lang/OutOfMemoryError","stack frame class name");return 0;}
 for(size_t i=0;i<n;i++)dotted[i]=class_name[i]=='/'?'.':class_name[i];
 dotted[n]='\0';
 dvm_ref cn=dvm_new_string(v,dotted);free(dotted);
 dvm_ref mn=dvm_new_string(v,method_name);
 if(!cn||!mn){dvm__throw(v,"java/lang/OutOfMemoryError","stack frame names");return 0;}
 return stack_trace_element_new(v,cn,mn,0,-1);
}
static bool thread_stack_receiver_is_thread(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)
  if(c->name&&!strcmp(c->name,"java/lang/Thread"))return true;
 return false;
}
/* vm->callstack[] is outermost first and holds bytecode frames only (builtins
 * are not pushed).  Report it innermost first, after the synthetic frame for
 * this builtin call. */
static dvm_ref thread_stack_trace_current(struct dvm *v){
 struct dvm_method *frames[THREAD_STACK_TRACE_REAL];
 int n=0;
 for(int i=v->ncallstack-1;i>=0&&n<THREAD_STACK_TRACE_REAL;--i){
  struct dvm_method *m=v->callstack[i];
  if(m&&m->cls&&m->cls->name&&m->name)frames[n++]=m;
 }
 dvm_ref arr=dvm_new_array(v,'L',"Ljava/lang/StackTraceElement;",(uint32_t)n+1u);
 if(!arr){dvm__throw(v,"java/lang/OutOfMemoryError","Thread.getStackTrace array");return 0;}
 dvm_ref *slots=dvm_array_data(v,arr);
 if(!slots){dvm__throw(v,"java/lang/IllegalStateException","Thread.getStackTrace array data");return 0;}
 slots[0]=thread_stack_element(v,"java/lang/Thread","getStackTrace");
 if(!slots[0])return 0;
 for(int i=0;i<n;i++){
  slots[i+1]=thread_stack_element(v,frames[i]->cls->name,frames[i]->name);
  if(!slots[i+1])return 0;
 }
 return arr;
}
static dvm_ref thread_stack_trace_empty(struct dvm *v){
 dvm_ref arr=dvm_new_array(v,'L',"Ljava/lang/StackTraceElement;",0);
 if(!arr)dvm__throw(v,"java/lang/OutOfMemoryError","Thread.getStackTrace empty array");
 return arr;
}
static void stack_trace_element_vm_start(struct dvm *v){
 struct dvm_class *c=dvm__register_builtin(v,"Ljava/lang/StackTraceElement;");
 if(!c){dvm__throw(v,"java/lang/OutOfMemoryError","StackTraceElement class");return;}
 c->super=dvm_find_class_or_external(v,"java/lang/Object");
 c->ifields=calloc(4,sizeof *c->ifields);
 if(!c->ifields){dvm__throw(v,"java/lang/OutOfMemoryError","StackTraceElement fields");return;}
 c->nifields=c->islots=4;
 message_field(c,0,"declaringClass","Ljava/lang/String;");
 message_field(c,1,"methodName","Ljava/lang/String;");
 message_field(c,2,"fileName","Ljava/lang/String;");
 message_field(c,3,"lineNumber","I");
 c->methods=calloc(6,sizeof *c->methods);
 if(!c->methods){dvm__throw(v,"java/lang/OutOfMemoryError","StackTraceElement methods");return;}
 int n=0;
 if(!stack_trace_element_install(c,n,"<init>","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V",ste_init,'V',4))goto fail;++n;
 if(!stack_trace_element_install(c,n,"getClassName","()Ljava/lang/String;",ste_getClassName,'L',0))goto fail;++n;
 if(!stack_trace_element_install(c,n,"getMethodName","()Ljava/lang/String;",ste_getMethodName,'L',0))goto fail;++n;
 if(!stack_trace_element_install(c,n,"getFileName","()Ljava/lang/String;",ste_getFileName,'L',0))goto fail;++n;
 if(!stack_trace_element_install(c,n,"getLineNumber","()I",ste_getLineNumber,'I',0))goto fail;++n;
 if(!stack_trace_element_install(c,n,"toString","()Ljava/lang/String;",ste_toString,'L',0))goto fail;++n;
 c->nmethods=n;
 return;
fail:
 c->nmethods=n;
 dvm__throw(v,"java/lang/OutOfMemoryError","StackTraceElement methods");
}
/* --- ReentrantReadWriteLock --------------------------------------------
 * java.util.concurrent.locks.ReentrantReadWriteLock is not modelled by the
 * boot class path in this build, but androidx/commonlib's IdExternalStorage
 * cache constructs one and guards its SQLite reads with readLock()/writeLock()
 * through the Lock interface.  Bind the outer object, both inner lock classes
 * and the Lock interface as real builtins.
 *
 * Semantics follow java.util.concurrent: writers are exclusive and
 * reentrant; readers share and are reentrant per thread; the write owner may
 * take the read lock (downgrade); read-to-write upgrade is never granted by
 * tryLock() and a blocking write acquisition waits until every reader has
 * released.  Ownership is identified by pthread_t: one guest Java thread is
 * one host thread in this runtime, and the token stays valid while the GIL is
 * dropped for a condition wait.
 *
 * Waiting must not sleep under the GIL.  The wait loop holds only the lock's
 * own mutex, drops every GIL level with dvm_gil_unlock_all(), waits on the
 * condition (releasing that mutex), drops the mutex, reacquires the GIL and
 * then the mutex before rechecking the predicate.  A poll interval bounds the
 * time to notice VM shutdown, or a selftest abort, without a broadcaster.
 * Failure to own the lock on unlock, or a wrong receiver, throws
 * IllegalStateException rather than silently succeeding.  Unknown methods
 * and signatures return false so the runtime stays fail-closed. */
#define RRW_OUTER_NAME "java/util/concurrent/locks/ReentrantReadWriteLock"
#define RRW_READ_NAME RRW_OUTER_NAME "$ReadLock"
#define RRW_WRITE_NAME RRW_OUTER_NAME "$WriteLock"
#define RRW_LOCK_NAME "java/util/concurrent/locks/Lock"
#define RRW_OUTER_DESC "L" RRW_OUTER_NAME ";"
#define RRW_READ_DESC "L" RRW_READ_NAME ";"
#define RRW_WRITE_DESC "L" RRW_WRITE_NAME ";"
#define RRW_LOCK_DESC "L" RRW_LOCK_NAME ";"
#define RRW_READ_SIG "()L" RRW_READ_NAME ";"
#define RRW_WRITE_SIG "()L" RRW_WRITE_NAME ";"
#define RRW_LOCK_RET_SIG "()L" RRW_LOCK_NAME ";"
#define RRW_COND_SIG "()Ljava/util/concurrent/locks/Condition;"
#define RRW_POLL_MS 20u
#define RRW_IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
struct rrw_reader{pthread_t thread;unsigned count;struct rrw_reader *next;};
struct rrw_lock{
 struct dvm *vm;pthread_mutex_t mutex;pthread_cond_t changed;
 bool write_owned;unsigned write_hold;pthread_t write_owner;
 unsigned read_count;struct rrw_reader *readers;
 dvm_ref self,read_lock,write_lock;
};
struct rrw_handle{struct rrw_lock *lock;bool write;};
static _Atomic(struct dvm *) rrw_shutdown_vm;
static _Atomic bool rrw_selftest_abort;
static void rrw_reader_free_all(struct rrw_lock *l){
 struct rrw_reader *r=l->readers;
 while(r){struct rrw_reader *n=r->next;free(r);r=n;}
 l->readers=NULL;
}
static void rrw_state_destroy(void *p){
 struct rrw_lock *l=p;
 if(!l)return;
 rrw_reader_free_all(l);
 pthread_cond_destroy(&l->changed);
 pthread_mutex_destroy(&l->mutex);
 free(l);
}
static void rrw_handle_destroy(void *p){free(p);}
static bool rrw_stopping(struct dvm *v){
 return dvm_is_stopping(v)||
  atomic_load_explicit(&rrw_shutdown_vm,memory_order_acquire)==v||
  atomic_load_explicit(&rrw_selftest_abort,memory_order_acquire);
}
static bool rrw_reader_add_locked(struct rrw_lock *l,pthread_t me){
 for(struct rrw_reader *r=l->readers;r;r=r->next)
  if(pthread_equal(r->thread,me)!=0){++r->count;return true;}
 struct rrw_reader *r=calloc(1,sizeof *r);
 if(!r)return false;
 r->thread=me;r->count=1;r->next=l->readers;l->readers=r;
 return true;
}
static bool rrw_reader_release_locked(struct rrw_lock *l,pthread_t me){
 struct rrw_reader **pp=&l->readers;
 while(*pp){
  struct rrw_reader *r=*pp;
  if(pthread_equal(r->thread,me)!=0){
   if(r->count>1){--r->count;return true;}
   *pp=r->next;free(r);return true;
  }
  pp=&r->next;
 }
 return false;
}
static unsigned rrw_reader_count_locked(struct rrw_lock *l,pthread_t me){
 for(struct rrw_reader *r=l->readers;r;r=r->next)
  if(pthread_equal(r->thread,me)!=0)return r->count;
 return 0;
}
static bool rrw_can_write(struct rrw_lock *l,pthread_t me){
 if(l->write_owned)return pthread_equal(l->write_owner,me)!=0;
 /* Read-to-write upgrade blocks: this thread's own read hold counts too. */
 return l->read_count==0;
}
static bool rrw_can_read(struct rrw_lock *l,pthread_t me){
 if(!l->write_owned)return true;
 /* Downgrade: the writer may take a read lock while it still owns the write
  * lock; an unrelated writer keeps readers out. */
 return pthread_equal(l->write_owner,me)!=0;
}
static void rrw_give_write_locked(struct rrw_lock *l,pthread_t me){
 if(!l->write_owned){l->write_owned=true;l->write_owner=me;}
 ++l->write_hold;
}
static bool rrw_give_read_locked(struct rrw_lock *l,pthread_t me){
 if(!rrw_reader_add_locked(l,me))return false;
 ++l->read_count;
 return true;
}
enum rrw_status{RRW_OK,RRW_STOPPING,RRW_INTERRUPTED,RRW_NO_MEMORY};
static enum rrw_status rrw_acquire(struct dvm *v,struct rrw_lock *l,bool write,bool interruptible){
 pthread_t me=pthread_self();
 pthread_mutex_lock(&l->mutex);
 for(;;){
  if(rrw_stopping(v)){pthread_mutex_unlock(&l->mutex);return RRW_STOPPING;}
  bool can=write?rrw_can_write(l,me):rrw_can_read(l,me);
  if(can){
   if(write)rrw_give_write_locked(l,me);
   else if(!rrw_give_read_locked(l,me)){pthread_mutex_unlock(&l->mutex);return RRW_NO_MEMORY;}
   pthread_mutex_unlock(&l->mutex);
   return RRW_OK;
  }
  if(interruptible&&dvm__thread_take_interrupt(v)){
   pthread_mutex_unlock(&l->mutex);
   return RRW_INTERRUPTED;
  }
  struct timespec until=deadline(RRW_POLL_MS);
  unsigned depth=dvm_gil_unlock_all(v);
  pthread_cond_timedwait(&l->changed,&l->mutex,&until);
  pthread_mutex_unlock(&l->mutex);
  dvm_gil_relock(v,depth);
  pthread_mutex_lock(&l->mutex);
 }
}
static bool rrw_do_lock(struct dvm *v,struct rrw_lock *l,bool write,bool interruptible){
 switch(rrw_acquire(v,l,write,interruptible)){
 case RRW_OK:return true;
 case RRW_INTERRUPTED:
  dvm__throw(v,"java/lang/InterruptedException","%s interrupted",write?"write lock":"read lock");
  return true;
 case RRW_NO_MEMORY:
  dvm__throw(v,"java/lang/OutOfMemoryError","%s reader state",write?"write lock":"read lock");
  return true;
 case RRW_STOPPING:break;
 }
 dvm__throw(v,"java/lang/IllegalStateException","%s wait aborted by VM shutdown",write?"write lock":"read lock");
 return true;
}
static bool rrw_do_trylock(struct dvm *v,struct rrw_lock *l,bool write,union dvm_value *out){
 pthread_t me=pthread_self();
 out->i=0;
 pthread_mutex_lock(&l->mutex);
 if(!rrw_stopping(v)){
  bool can=write?rrw_can_write(l,me):rrw_can_read(l,me);
  if(can){
   if(write){rrw_give_write_locked(l,me);out->i=1;}
   else if(rrw_give_read_locked(l,me))out->i=1;
   else{
    pthread_mutex_unlock(&l->mutex);
    dvm__throw(v,"java/lang/OutOfMemoryError","read lock reader state");
    return true;
   }
  }
 }
 pthread_mutex_unlock(&l->mutex);
 return true;
}
static bool rrw_do_unlock(struct dvm *v,struct rrw_lock *l,bool write){
 pthread_t me=pthread_self();
 bool held=false;
 pthread_mutex_lock(&l->mutex);
 if(write){
  if(l->write_owned&&pthread_equal(l->write_owner,me)!=0){
   held=true;
   if(l->write_hold>1)--l->write_hold;
   else{l->write_hold=0;l->write_owned=false;}
   pthread_cond_broadcast(&l->changed);
  }
 }else if(rrw_reader_release_locked(l,me)){
  held=true;
  if(l->read_count)--l->read_count;
  pthread_cond_broadcast(&l->changed);
 }
 pthread_mutex_unlock(&l->mutex);
 if(!held)
  dvm__throw(v,"java/lang/IllegalStateException",
             write?"unlock without holding the write lock":"unlock without holding the read lock");
 return true;
}
static bool rrw_do_get(struct dvm *v,struct rrw_lock *l,const char *m,const char *sig,union dvm_value *out){
 (void)v;
 pthread_t me=pthread_self();
 pthread_mutex_lock(&l->mutex);
 if(RRW_IS("isWriteLocked","()Z"))out->i=l->write_owned?1:0;
 else if(RRW_IS("getReadLockCount","()I"))out->i=(int32_t)l->read_count;
 else if(RRW_IS("getWriteHoldCount","()I"))
  out->i=(l->write_owned&&pthread_equal(l->write_owner,me)!=0)?(int32_t)l->write_hold:0;
 else if(RRW_IS("getReadHoldCount","()I"))out->i=(int32_t)rrw_reader_count_locked(l,me);
 else if(RRW_IS("isWriteLockedByCurrentThread","()Z"))
  out->i=(l->write_owned&&l->write_hold&&pthread_equal(l->write_owner,me)!=0)?1:0;
 else if(RRW_IS("isFair","()Z"))out->i=0;
 else{pthread_mutex_unlock(&l->mutex);return false;}
 pthread_mutex_unlock(&l->mutex);
 return true;
}
static bool rrw_make_child(struct dvm *v,struct rrw_lock *l,bool write,union dvm_value *out){
 const char *desc=write?RRW_WRITE_DESC:RRW_READ_DESC;
 const char *label=write?"WriteLock":"ReadLock";
 struct dvm_class *c=dvm__class_by_desc(v,desc);
 if(!c||c->external){
  dvm__throw(v,"java/lang/IllegalStateException","ReentrantReadWriteLock.%s class unavailable",label);
  return true;
 }
 dvm_ref *slot=write?&l->write_lock:&l->read_lock;
 struct dvm_object *o=*slot?dvm__obj(v,*slot):NULL;
 if(!o||o->cls!=c||o->data_destroy!=rrw_handle_destroy){*slot=0;o=NULL;}
 if(!o){
  dvm_ref r=dvm_new_object(v,c);
  if(!r){dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock.%s",label);return true;}
  o=dvm__obj(v,r);
  if(!o){
   dvm__discard_temporary(v,r);
   dvm__throw(v,"java/lang/IllegalStateException","ReentrantReadWriteLock.%s storage",label);
   return true;
  }
  struct rrw_handle *h=calloc(1,sizeof *h);
  if(!h){
   dvm__discard_temporary(v,r);
   dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock.%s state",label);
   return true;
  }
  h->lock=l;h->write=write;o->data=h;o->data_destroy=rrw_handle_destroy;*slot=r;
 }
 out->l=*slot;
 return true;
}
static struct rrw_handle *rrw_handle_of(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 return o&&o->data_destroy==rrw_handle_destroy?o->data:NULL;
}
static bool rrw_outer_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","ReentrantReadWriteLock receiver");return true;}
 if(!strcmp(m,"<init>")){
  if(strcmp(sig,"()V"))return false;
  if(o->data_destroy==rrw_handle_destroy||!o->cls||!o->cls->name||
     strcmp(o->cls->name,RRW_OUTER_NAME)){
   dvm__throw(v,"java/lang/IllegalStateException","ReentrantReadWriteLock constructor receiver");
   return true;
  }
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","ReentrantReadWriteLock already initialized");return true;}
  struct rrw_lock *l=calloc(1,sizeof *l);
  if(!l){dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock");return true;}
  int rc=pthread_mutex_init(&l->mutex,NULL);
  if(rc){
   free(l);
   dvm__throw(v,"java/lang/IllegalStateException","ReentrantReadWriteLock mutex: %d",rc);
   return true;
  }
  rc=pthread_cond_init(&l->changed,NULL);
  if(rc){
   pthread_mutex_destroy(&l->mutex);
   free(l);
   dvm__throw(v,"java/lang/IllegalStateException","ReentrantReadWriteLock condition: %d",rc);
   return true;
  }
  l->vm=v;l->self=self;o->data=l;o->data_destroy=rrw_state_destroy;
  return true;
 }
 if(o->data_destroy!=rrw_state_destroy||!o->data){
  dvm__throw(v,"java/lang/IllegalStateException","ReentrantReadWriteLock constructor not called");
  return true;
 }
 struct rrw_lock *l=o->data;
 if(RRW_IS("readLock",RRW_READ_SIG)||RRW_IS("readLock",RRW_LOCK_RET_SIG))
  return rrw_make_child(v,l,false,out);
 if(RRW_IS("writeLock",RRW_WRITE_SIG)||RRW_IS("writeLock",RRW_LOCK_RET_SIG))
  return rrw_make_child(v,l,true,out);
 return rrw_do_get(v,l,m,sig,out);
}
static bool rrw_child_ops(struct dvm *v,struct rrw_lock *l,bool write,const char *m,const char *sig,union dvm_value *out){
 if(RRW_IS("lock","()V"))return rrw_do_lock(v,l,write,false);
 if(RRW_IS("unlock","()V"))return rrw_do_unlock(v,l,write);
 if(RRW_IS("tryLock","()Z"))return rrw_do_trylock(v,l,write,out);
 if(RRW_IS("lockInterruptibly","()V"))return rrw_do_lock(v,l,write,true);
 if(RRW_IS("newCondition",RRW_COND_SIG)){
  dvm__throw(v,"java/lang/UnsupportedOperationException","ReentrantReadWriteLock Condition is not implemented");
  return true;
 }
 return false;
}
static bool rrw_child_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,bool write,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);
 const char *want=write?RRW_WRITE_NAME:RRW_READ_NAME;
 if(!o){dvm__throw(v,"java/lang/NullPointerException","%s receiver",want);return true;}
 if(o->data_destroy!=rrw_handle_destroy||!o->data||!o->cls||!o->cls->name||
    strcmp(o->cls->name,want)){
  dvm__throw(v,"java/lang/IllegalStateException","%s receiver is not this lock",want);
  return true;
 }
 struct rrw_handle *h=o->data;
 return rrw_child_ops(v,h->lock,write,m,sig,out);
}
static bool rrw_lock_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,union dvm_value *out){
 struct rrw_handle *h=rrw_handle_of(v,self);
 if(!h){
  if(!dvm__obj(v,self))
   dvm__throw(v,"java/lang/NullPointerException","Lock receiver");
  else
   dvm__throw(v,"java/lang/IllegalStateException","Lock receiver is not a ReentrantReadWriteLock lock");
  return true;
 }
 return rrw_child_ops(v,h->lock,h->write,m,sig,out);
}
static bool rrwlock_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;
 if(!c||!m||!sig)return false;
 if(!strcmp(c,RRW_OUTER_NAME))return rrw_outer_call(v,m,sig,self,out);
 if(!strcmp(c,RRW_READ_NAME))return rrw_child_call(v,m,sig,self,false,out);
 if(!strcmp(c,RRW_WRITE_NAME))return rrw_child_call(v,m,sig,self,true,out);
 if(!strcmp(c,RRW_LOCK_NAME))return rrw_lock_call(v,m,sig,self,out);
 return false;
}
static bool rrw_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *bm=dvm__builtin_method();
 if(!bm||!bm->cls||!bm->cls->name||!bm->name||!bm->sig)return false;
 return rrwlock_call(v,bm->cls->name,bm->name,bm->sig,self,a,n,out);
}
static bool rrw_install(struct dvm_class *c,int index,const char *name,const char *sig,char ret,int args){
 struct dvm_method *m=&c->methods[index];
 m->cls=c;m->name=name;m->sig=strdup(sig);m->builtin=rrw_builtin;
 m->ret_kind=ret;m->arg_count=args;m->arg_slots=args;
 return m->sig!=NULL;
}
static bool rrw_install_outer(struct dvm *v,struct dvm_class *c){
 c->methods=calloc(11,sizeof *c->methods);
 if(!c->methods){dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock methods");return false;}
 int n=0;
 if(!rrw_install(c,n,"<init>","()V",'V',0))goto oom;++n;
 if(!rrw_install(c,n,"readLock",RRW_READ_SIG,'L',0))goto oom;++n;
 if(!rrw_install(c,n,"readLock",RRW_LOCK_RET_SIG,'L',0))goto oom;++n;
 if(!rrw_install(c,n,"writeLock",RRW_WRITE_SIG,'L',0))goto oom;++n;
 if(!rrw_install(c,n,"writeLock",RRW_LOCK_RET_SIG,'L',0))goto oom;++n;
 if(!rrw_install(c,n,"isWriteLocked","()Z",'Z',0))goto oom;++n;
 if(!rrw_install(c,n,"getReadLockCount","()I",'I',0))goto oom;++n;
 if(!rrw_install(c,n,"getWriteHoldCount","()I",'I',0))goto oom;++n;
 if(!rrw_install(c,n,"getReadHoldCount","()I",'I',0))goto oom;++n;
 if(!rrw_install(c,n,"isWriteLockedByCurrentThread","()Z",'Z',0))goto oom;++n;
 if(!rrw_install(c,n,"isFair","()Z",'Z',0))goto oom;++n;
 c->nmethods=n;return true;
oom:
 c->nmethods=n;
 dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock methods");
 return false;
}
static bool rrw_install_child(struct dvm *v,struct dvm_class *c){
 c->methods=calloc(5,sizeof *c->methods);
 if(!c->methods){dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock lock methods");return false;}
 int n=0;
 if(!rrw_install(c,n,"lock","()V",'V',0))goto oom;++n;
 if(!rrw_install(c,n,"unlock","()V",'V',0))goto oom;++n;
 if(!rrw_install(c,n,"tryLock","()Z",'Z',0))goto oom;++n;
 if(!rrw_install(c,n,"lockInterruptibly","()V",'V',0))goto oom;++n;
 if(!rrw_install(c,n,"newCondition",RRW_COND_SIG,'L',0))goto oom;++n;
 c->nmethods=n;return true;
oom:
 c->nmethods=n;
 dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock lock methods");
 return false;
}
static bool rrw_install_lock(struct dvm *v,struct dvm_class *c){
 c->methods=calloc(5,sizeof *c->methods);
 if(!c->methods){dvm__throw(v,"java/lang/OutOfMemoryError","Lock methods");return false;}
 int n=0;
 if(!rrw_install(c,n,"lock","()V",'V',0))goto oom;++n;
 if(!rrw_install(c,n,"unlock","()V",'V',0))goto oom;++n;
 if(!rrw_install(c,n,"tryLock","()Z",'Z',0))goto oom;++n;
 if(!rrw_install(c,n,"lockInterruptibly","()V",'V',0))goto oom;++n;
 if(!rrw_install(c,n,"newCondition",RRW_COND_SIG,'L',0))goto oom;++n;
 c->nmethods=n;return true;
oom:
 c->nmethods=n;
 dvm__throw(v,"java/lang/OutOfMemoryError","Lock methods");
 return false;
}
static bool rrwlock_vm_start(struct dvm *v){
 atomic_store_explicit(&rrw_shutdown_vm,NULL,memory_order_release);
 atomic_store_explicit(&rrw_selftest_abort,false,memory_order_release);
 struct dvm_class *outer=dvm__register_builtin(v,RRW_OUTER_DESC);
 struct dvm_class *read=dvm__register_builtin(v,RRW_READ_DESC);
 struct dvm_class *write=dvm__register_builtin(v,RRW_WRITE_DESC);
 struct dvm_class *lock=dvm__register_builtin(v,RRW_LOCK_DESC);
 if(!outer||!read||!write||!lock){
  dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock classes");
  return false;
 }
 struct dvm_class *object=dvm__class_by_desc(v,"Ljava/lang/Object;");
 outer->super=object;read->super=object;write->super=object;
 lock->is_interface=true;lock->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;
 read->ifaces=calloc(1,sizeof *read->ifaces);
 write->ifaces=calloc(1,sizeof *write->ifaces);
 if(!read->ifaces||!write->ifaces){
  dvm__throw(v,"java/lang/OutOfMemoryError","ReentrantReadWriteLock interfaces");
  return false;
 }
 read->ifaces[0]=lock;write->ifaces[0]=lock;
 read->nifaces=write->nifaces=1;
 outer->init_state=read->init_state=write->init_state=lock->init_state=2;
 if(!rrw_install_outer(v,outer))return false;
 if(!rrw_install_child(v,read))return false;
 if(!rrw_install_child(v,write))return false;
 if(!rrw_install_lock(v,lock))return false;
 return true;
}
/* --- selftest ---------------------------------------------------------- */
static bool rrw_name_is(const char *got,const char *want){
 if(!got||!want)return false;
 size_t i=0;
 for(;got[i]&&want[i];++i){
  char g=got[i]=='.'?'/':got[i];
  if(g!=want[i])return false;
 }
 return got[i]=='\0'&&want[i]=='\0';
}
static void rrw_test_sleep_ms(unsigned ms){
 struct timespec t;
 t.tv_sec=(time_t)(ms/1000u);
 t.tv_nsec=(long)(ms%1000u)*1000000L;
 while(nanosleep(&t,&t)!=0&&errno==EINTR){}
}
static bool rrw_test_invoke(struct dvm *v,const char *cls,const char *name,const char *sig,dvm_ref self,union dvm_value *out){
 struct dvm_method *m=dvm_lookup(v,cls,name,sig);
 union dvm_value sink={0};
 if(!out)out=&sink;
 memset(out,0,sizeof *out);
 return m&&dvm_call(v,m,self,NULL,0,out);
}
static bool rrw_test_get(struct dvm *v,dvm_ref self,const char *name,const char *sig,int32_t want){
 union dvm_value out={0};
 struct dvm_method *m=dvm_lookup(v,RRW_OUTER_NAME,name,sig);
 if(!m)return false;
 if(!dvm_call(v,m,self,NULL,0,&out)||dvm_exception(v)){
  if(dvm_exception(v))dvm_clear_exception(v);
  return false;
 }
 return out.i==want;
}
static bool rrw_test_threw(struct dvm *v,const char *slash_name){
 dvm_ref e=dvm_exception(v);
 bool match=false;
 if(e){
  struct dvm_class *c=dvm_object_class(v,e);
  match=rrw_name_is(c?c->name:NULL,slash_name);
  dvm_clear_exception(v);
 }
 return match;
}
struct rrw_test_worker{struct dvm *v;dvm_ref write;_Atomic int attempting,acquired,failed,done;};
static void *rrw_test_worker_main(void *arg){
 struct rrw_test_worker *w=arg;
 struct dvm *v=w->v;
 dvm__mark_bytecode_thread();
 dvm_gil_acquire(v);
 atomic_store_explicit(&w->attempting,1,memory_order_release);
 union dvm_value out={0};
 bool ok=rrw_test_invoke(v,RRW_WRITE_NAME,"lock","()V",w->write,&out);
 if(!ok||dvm_exception(v)){
  if(dvm_exception(v))dvm_clear_exception(v);
  atomic_store_explicit(&w->failed,1,memory_order_release);
  atomic_store_explicit(&w->done,1,memory_order_release);
  dvm_gil_release(v);
  return NULL;
 }
 atomic_store_explicit(&w->acquired,1,memory_order_release);
 {
  unsigned depth=dvm_gil_unlock_all(v);
  rrw_test_sleep_ms(10);
  dvm_gil_relock(v,depth);
 }
 memset(&out,0,sizeof out);
 ok=rrw_test_invoke(v,RRW_WRITE_NAME,"unlock","()V",w->write,&out);
 if(!ok||dvm_exception(v)){
  if(dvm_exception(v))dvm_clear_exception(v);
  atomic_store_explicit(&w->failed,1,memory_order_release);
 }
 atomic_store_explicit(&w->done,1,memory_order_release);
 dvm_gil_release(v);
 return NULL;
}
static bool rrw_test_wait_flag(struct dvm *v,_Atomic int *flag,unsigned limit_ms){
 unsigned waited=0;
 while(!atomic_load_explicit(flag,memory_order_acquire)&&waited<limit_ms){
  unsigned depth=dvm_gil_unlock_all(v);
  rrw_test_sleep_ms(5);
  dvm_gil_relock(v,depth);
  waited+=5;
 }
 return atomic_load_explicit(flag,memory_order_acquire)!=0;
}
static void rrw_test_join(struct dvm *v,pthread_t thread){
 unsigned depth=dvm_gil_unlock_all(v);
 pthread_join(thread,NULL);
 dvm_gil_relock(v,depth);
}
static bool rrwlock_selftest(struct dvm *v){
 bool ok=true;
 union dvm_value out={0};
 dvm_ref lock=0,read_ref=0,write_ref=0;
#define RRW_CHECK(expr) do{if(!(expr)){ok=false;sdk_trace_fprintf(stderr,"RRWLOCK_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 dvm_gil_acquire(v);
 struct dvm_class *outer=dvm__class_by_desc(v,RRW_OUTER_DESC);
 RRW_CHECK(outer!=NULL&&!outer->external);
 RRW_CHECK(dvm_lookup(v,RRW_OUTER_NAME,"<init>","()V")!=NULL);
 RRW_CHECK(dvm_lookup(v,RRW_OUTER_NAME,"readLock",RRW_READ_SIG)!=NULL);
 RRW_CHECK(dvm_lookup(v,RRW_OUTER_NAME,"writeLock",RRW_WRITE_SIG)!=NULL);
 RRW_CHECK(dvm_lookup(v,RRW_READ_NAME,"lock","()V")!=NULL);
 RRW_CHECK(dvm_lookup(v,RRW_WRITE_NAME,"unlock","()V")!=NULL);
 RRW_CHECK(dvm_lookup(v,RRW_LOCK_NAME,"tryLock","()Z")!=NULL);
 if(outer){
  lock=dvm_new_object(v,outer);
  RRW_CHECK(lock!=0);
 }
 if(lock){
  RRW_CHECK(rrw_test_invoke(v,RRW_OUTER_NAME,"<init>","()V",lock,&out));
  RRW_CHECK(!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"isWriteLocked","()Z",0));
  RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",0));
  RRW_CHECK(rrw_test_get(v,lock,"getWriteHoldCount","()I",0));
  RRW_CHECK(rrw_test_get(v,lock,"getReadHoldCount","()I",0));
  RRW_CHECK(rrw_test_get(v,lock,"isWriteLockedByCurrentThread","()Z",0));
  RRW_CHECK(rrw_test_get(v,lock,"isFair","()Z",0));
  /* A second <init> must not silently replace live lock state. */
  memset(&out,0,sizeof out);
  RRW_CHECK(!rrw_test_invoke(v,RRW_OUTER_NAME,"<init>","()V",lock,&out));
  RRW_CHECK(rrw_test_threw(v,"java/lang/IllegalStateException"));
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_OUTER_NAME,"readLock",RRW_READ_SIG,lock,&out));
  read_ref=out.l;
  RRW_CHECK(read_ref!=0);
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_OUTER_NAME,"readLock",RRW_LOCK_RET_SIG,lock,&out));
  RRW_CHECK(out.l==read_ref);
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_OUTER_NAME,"writeLock",RRW_WRITE_SIG,lock,&out));
  write_ref=out.l;
  RRW_CHECK(write_ref!=0);
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_OUTER_NAME,"writeLock",RRW_LOCK_RET_SIG,lock,&out));
  RRW_CHECK(out.l==write_ref);
  if(read_ref){
   struct dvm_object *ro=dvm__obj(v,read_ref);
   RRW_CHECK(ro&&ro->cls&&rrw_name_is(ro->cls->name,RRW_READ_NAME));
  }
  if(write_ref){
   struct dvm_object *wo=dvm__obj(v,write_ref);
   RRW_CHECK(wo&&wo->cls&&rrw_name_is(wo->cls->name,RRW_WRITE_NAME));
  }
 }
 /* Write lock: reentrant, exclusive, and an unlock without ownership fails. */
 if(lock&&write_ref){
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"lock","()V",write_ref,&out)&&!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"getWriteHoldCount","()I",1));
  RRW_CHECK(rrw_test_get(v,lock,"isWriteLocked","()Z",1));
  RRW_CHECK(rrw_test_get(v,lock,"isWriteLockedByCurrentThread","()Z",1));
  RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",0));
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"lock","()V",write_ref,&out)&&!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"getWriteHoldCount","()I",2));
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"tryLock","()Z",write_ref,&out));
  RRW_CHECK(out.i==1);
  RRW_CHECK(rrw_test_get(v,lock,"getWriteHoldCount","()I",3));
  for(int i=0;i<3;++i){
   memset(&out,0,sizeof out);
   RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"unlock","()V",write_ref,&out)&&!dvm_exception(v));
  }
  RRW_CHECK(rrw_test_get(v,lock,"getWriteHoldCount","()I",0));
  RRW_CHECK(rrw_test_get(v,lock,"isWriteLocked","()Z",0));
  memset(&out,0,sizeof out);
  RRW_CHECK(!rrw_test_invoke(v,RRW_WRITE_NAME,"unlock","()V",write_ref,&out));
  RRW_CHECK(rrw_test_threw(v,"java/lang/IllegalStateException"));
 }
 /* Read lock: reentrant per thread and shared through getReadLockCount. */
 if(lock&&read_ref){
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_READ_NAME,"lock","()V",read_ref,&out)&&!dvm_exception(v));
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_READ_NAME,"lock","()V",read_ref,&out)&&!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"getReadHoldCount","()I",2));
  RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",2));
  if(write_ref){
   memset(&out,0,sizeof out);
   RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"tryLock","()Z",write_ref,&out));
   RRW_CHECK(out.i==0);
   RRW_CHECK(rrw_test_get(v,lock,"getWriteHoldCount","()I",0));
  }
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_READ_NAME,"unlock","()V",read_ref,&out)&&!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"getReadHoldCount","()I",1));
  RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",1));
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_READ_NAME,"unlock","()V",read_ref,&out)&&!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"getReadHoldCount","()I",0));
  RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",0));
  memset(&out,0,sizeof out);
  RRW_CHECK(!rrw_test_invoke(v,RRW_READ_NAME,"unlock","()V",read_ref,&out));
  RRW_CHECK(rrw_test_threw(v,"java/lang/IllegalStateException"));
 }
 /* Downgrade: write + read, drop the write lock, keep reading. */
 if(lock&&read_ref&&write_ref){
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"lock","()V",write_ref,&out)&&!dvm_exception(v));
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_READ_NAME,"lock","()V",read_ref,&out)&&!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"getWriteHoldCount","()I",1));
  RRW_CHECK(rrw_test_get(v,lock,"getReadHoldCount","()I",1));
  RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",1));
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"unlock","()V",write_ref,&out)&&!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"isWriteLocked","()Z",0));
  RRW_CHECK(rrw_test_get(v,lock,"getWriteHoldCount","()I",0));
  RRW_CHECK(rrw_test_get(v,lock,"getReadHoldCount","()I",1));
  RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",1));
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"tryLock","()Z",write_ref,&out));
  RRW_CHECK(out.i==0);
  memset(&out,0,sizeof out);
  RRW_CHECK(rrw_test_invoke(v,RRW_READ_NAME,"unlock","()V",read_ref,&out)&&!dvm_exception(v));
  RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",0));
 }
 /* The Lock interface call site resolves to the same inner-lock state. */
 if(lock&&read_ref){
  struct dvm_method *lm=dvm_lookup(v,RRW_LOCK_NAME,"lock","()V");
  struct dvm_method *um=dvm_lookup(v,RRW_LOCK_NAME,"unlock","()V");
  RRW_CHECK(lm&&um);
  if(lm&&um){
   memset(&out,0,sizeof out);
   RRW_CHECK(dvm_call(v,lm,read_ref,NULL,0,&out)&&!dvm_exception(v));
   RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",1));
   memset(&out,0,sizeof out);
   RRW_CHECK(dvm_call(v,um,read_ref,NULL,0,&out)&&!dvm_exception(v));
   RRW_CHECK(rrw_test_get(v,lock,"getReadLockCount","()I",0));
  }
 }
 /* A host worker must block while this thread owns the write lock, and the
  * wait must release the GIL (this thread keeps running). */
 if(write_ref){
  struct rrw_test_worker *tw=calloc(1,sizeof *tw);
  RRW_CHECK(tw!=NULL);
  if(tw){
   bool started=false;
   tw->v=v;tw->write=write_ref;
   memset(&out,0,sizeof out);
   bool locked=rrw_test_invoke(v,RRW_WRITE_NAME,"lock","()V",write_ref,&out)&&!dvm_exception(v);
   RRW_CHECK(locked);
   if(locked){
    pthread_t worker;
    started=pthread_create(&worker,NULL,rrw_test_worker_main,tw)==0;
    RRW_CHECK(started);
    if(started){
     RRW_CHECK(rrw_test_wait_flag(v,&tw->attempting,1000));
     {
      unsigned depth=dvm_gil_unlock_all(v);
      rrw_test_sleep_ms(100);
      dvm_gil_relock(v,depth);
     }
     RRW_CHECK(atomic_load_explicit(&tw->acquired,memory_order_acquire)==0);
    }
    memset(&out,0,sizeof out);
    RRW_CHECK(rrw_test_invoke(v,RRW_WRITE_NAME,"unlock","()V",write_ref,&out)&&!dvm_exception(v));
    if(started){
     if(!rrw_test_wait_flag(v,&tw->done,2000)){
      atomic_store_explicit(&rrw_selftest_abort,true,memory_order_release);
      ok=false;
      sdk_trace_fprintf(stderr,"RRWLOCK_SELFTEST_FAIL line=%d expression=worker_done_timeout\n",__LINE__);
     }
     rrw_test_join(v,worker);
     RRW_CHECK(atomic_load_explicit(&tw->acquired,memory_order_acquire)==1);
     RRW_CHECK(atomic_load_explicit(&tw->failed,memory_order_acquire)==0);
    }
   }
   atomic_store_explicit(&rrw_selftest_abort,false,memory_order_release);
   free(tw);
  }
 }
 /* Wrong receivers and unknown signatures stay fail-closed. */
 if(lock){
  struct dvm_class *oc=dvm__class_by_desc(v,"Ljava/lang/Object;");
  dvm_ref object=oc?dvm_new_object(v,oc):0;
  RRW_CHECK(object!=0);
  if(object){
   memset(&out,0,sizeof out);
   RRW_CHECK(thread_runtime_call(v,RRW_OUTER_NAME,"readLock",RRW_READ_SIG,object,NULL,0,&out));
   RRW_CHECK(rrw_test_threw(v,"java/lang/IllegalStateException"));
   memset(&out,0,sizeof out);
   RRW_CHECK(thread_runtime_call(v,RRW_READ_NAME,"lock","()V",object,NULL,0,&out));
   RRW_CHECK(rrw_test_threw(v,"java/lang/IllegalStateException"));
   if(write_ref){
    memset(&out,0,sizeof out);
    RRW_CHECK(thread_runtime_call(v,RRW_READ_NAME,"lock","()V",write_ref,NULL,0,&out));
    RRW_CHECK(rrw_test_threw(v,"java/lang/IllegalStateException"));
   }
  }
  memset(&out,0,sizeof out);
  RRW_CHECK(!thread_runtime_call(v,RRW_OUTER_NAME,"readLock","()V",lock,NULL,0,&out));
  RRW_CHECK(!dvm_exception(v));
  memset(&out,0,sizeof out);
  RRW_CHECK(!thread_runtime_call(v,RRW_OUTER_NAME,"noSuchMethod","()V",lock,NULL,0,&out));
  RRW_CHECK(!dvm_exception(v));
  memset(&out,0,sizeof out);
  RRW_CHECK(!thread_runtime_call(v,RRW_READ_NAME,"lockInterruptibly","()I",read_ref?read_ref:lock,NULL,0,&out));
  RRW_CHECK(!dvm_exception(v));
 }
#undef RRW_CHECK
 if(dvm_exception(v)){dvm_clear_exception(v);ok=false;}
 dvm_gil_release(v);
 sdk_trace_fprintf(stderr,"RRWLOCK_SELFTEST %s\n",ok?"PASS":"FAIL");
 return ok;
}
bool thread_runtime_stack_selftest(struct dvm *v);
static bool thread_runtime_gc_roots(struct dvm *,dvm_gc_visit_fn,void *,void *);
void thread_runtime_init(struct dvm *v){
 if(message_pool.vm!=v){memset(&message_pool,0,sizeof message_pool);message_pool.vm=v;}
 run.vm=v;run.main_host=pthread_self();run.workers=NULL;run.sequence=0;run.main_java=0;
 if(!dvm_gc_root_provider_add(v,"thread_runtime",DVM_GC_ROOT_STRONG,thread_runtime_gc_roots,NULL)||!executor_runtime_gc_install(v)){
  dvm__throw(v,"java/lang/OutOfMemoryError","thread/executor GC root registration");return;
 }
 run.main_java=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread"));
 choreographer_install(v);
  /* java/lang/Thread 的方法体同样由 thread_runtime_call 分派（plain-thread
   * 分支 + currentThread/getStackTrace）；72pv 实测 native 侧 FindClass 后
   * GetMethodID 仍需类方法表。只声明已实现的公开方法；Thread.sleep 等
   * 未实现方法继续 fail closed，不伪造。currentThread 是 static（0x9）。 */
 {
  static const struct{const char *name;const char *sig;int access;} th_methods[]={
   {"<init>","()V",0x10001},
   {"<init>","(Ljava/lang/Runnable;)V",0x10001},
   {"<init>","(Ljava/lang/Runnable;Ljava/lang/String;)V",0x10001},
   {"<init>","(Ljava/lang/ThreadGroup;Ljava/lang/Runnable;Ljava/lang/String;J)V",0x10001},
   {"start","()V",0x1},
   {"getName","()Ljava/lang/String;",0x1},
   {"setName","(Ljava/lang/String;)V",0x1},
   {"setDaemon","(Z)V",0x1},
   {"isDaemon","()Z",0x1},
   {"isAlive","()Z",0x1},
   {"getPriority","()I",0x1},
   {"setPriority","(I)V",0x1},
   {"getThreadGroup","()Ljava/lang/ThreadGroup;",0x1},
   {"getStackTrace","()[Ljava/lang/StackTraceElement;",0x1},
   {"currentThread","()Ljava/lang/Thread;",0x9},
  };
  enum {TH_N=(int)(sizeof th_methods/sizeof th_methods[0])};
  struct dvm_class *th=dvm__class_by_desc(v,"Ljava/lang/Thread;");
  if(th&&th->nmethods==0){
   struct dvm_method *ms=calloc(TH_N,sizeof *ms);
   if(ms){
    int ok=1;
    for(int i=0;i<TH_N;i++){
     struct dvm_method *m=&ms[i];
     m->cls=th;m->name=th_methods[i].name;m->sig=strdup(th_methods[i].sig);
     m->access=(uint32_t)th_methods[i].access;
     m->arg_slots=dvm_sig_arg_slots(m->sig);
     m->arg_count=dvm_sig_arg_count(m->sig);
     m->ret_kind=dvm_sig_return_kind(m->sig);
     if(!m->sig)ok=0;
    }
     if(ok){th->methods=ms;th->nmethods=TH_N;}
     else{for(int i=0;i<TH_N;i++)free(ms[i].sig);free(ms);}
    }
   }
  }
  /* HandlerThread 的方法体由 thread_runtime_call 按 receiver 类链分派
   * （下方 is_thread 分支）；JNI GetMethodID 走类方法表，类是 synthesized
   * 占位时没有声明会抛 NoSuchMethodError（72pt 实测 nativeRender 尾部
   * pending=1225 的 HandlerThread.<init>(Ljava/lang/String;)V 不消，主循环
   * 停摆）。这里只声明 thread_runtime_call 已实现的公开方法，方法体仍由
   * call 分派提供，不新增任何行为。 */
  {
  static const struct{const char *name;const char *sig;int access;} ht_methods[]={
   {"<init>","(Ljava/lang/String;)V",0x10001},
   {"<init>","(Ljava/lang/String;I)V",0x10001},
   {"start","()V",0x1},
   {"getLooper","()Landroid/os/Looper;",0x1},
   {"getName","()Ljava/lang/String;",0x1},
   {"setName","(Ljava/lang/String;)V",0x1},
   {"isAlive","()Z",0x1},
   {"quit","()Z",0x1},
  };
  enum {HT_N=(int)(sizeof ht_methods/sizeof ht_methods[0])};
  struct dvm_class *ht=dvm__class_by_desc(v,"Landroid/os/HandlerThread;");
  if(ht&&ht->nmethods==0){
   struct dvm_method *ms=calloc(HT_N,sizeof *ms);
   if(ms){
    int ok=1;
    for(int i=0;i<HT_N;i++){
     struct dvm_method *m=&ms[i];
     m->cls=ht;m->name=ht_methods[i].name;m->sig=strdup(ht_methods[i].sig);
     m->access=(uint32_t)ht_methods[i].access;
     m->arg_slots=dvm_sig_arg_slots(m->sig);
     m->arg_count=dvm_sig_arg_count(m->sig);
     m->ret_kind=dvm_sig_return_kind(m->sig);
     if(!m->sig)ok=0;
    }
    if(ok){ht->methods=ms;ht->nmethods=HT_N;}
    else{for(int i=0;i<HT_N;i++)free(ms[i].sig);free(ms);}
   }
  }
 }
 /* The currentThread() identity is a real guest Thread but was not registered
  * as a plain thread, so Thread.getThreadGroup()/getName() fell through the
  * plain-thread dispatch. Register it once per VM with a null group; callers
  * such as Router's DefaultThreadFactory only need a ThreadGroup identity. */
 if(run.main_java){struct plain_thread *t=calloc(1,sizeof *t);if(!t){dvm__throw(v,"java/lang/OutOfMemoryError","main Thread state");return;}t->vm=v;t->self=run.main_java;t->identity=thread_ref_identity(v,t->self);t->priority=5;t->daemon=false;t->name=dvm_new_string(v,"main");t->next=plain_threads;plain_threads=t;}
 stack_trace_element_vm_start(v);
 rrwlock_vm_start(v);
 if(!dvm_exception(v))rrwlock_selftest(v);
  /* 代理接口的方法声明：dvm_proxy_try_invoke 在代理携带的接口类上
   * find_method（reflection_runtime.c），接口没有方法表时即使代理对象
   * 存在也无法分派。72py 实测引擎创建
   * Proxy(Handler$Callback + Choreographer$FrameCallback) 投递到 worker
   * Looper；两个接口的声明如下。 */
 {
  static const struct{const char *desc;const char *name;const char *sig;} ifaces[]={
   {"Landroid/os/Handler$Callback;","handleMessage","(Landroid/os/Message;)Z"},
   {"Landroid/view/Choreographer$FrameCallback;","doFrame","(J)V"},
  };
  for(size_t i=0;i<sizeof ifaces/sizeof ifaces[0];i++){
   struct dvm_class *ic=dvm__class_by_desc(v,ifaces[i].desc);
   if(ic&&ic->nmethods==0){
    struct dvm_method *ms=calloc(1,sizeof *ms);
    if(ms){
     ms->cls=ic;ms->name=ifaces[i].name;ms->sig=strdup(ifaces[i].sig);
     ms->access=DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT;
     ms->arg_slots=dvm_sig_arg_slots(ms->sig);
     ms->arg_count=dvm_sig_arg_count(ms->sig);
     ms->ret_kind=dvm_sig_return_kind(ms->sig);
     if(ms->sig){ic->methods=ms;ic->nmethods=1;}
     else free(ms);
    }
   }
  }
 }
 struct dvm_class *c=dvm__class_by_desc(v,"Landroid/os/Message;");if(!c)return;
 /* Message 的方法体由 thread_runtime_call 分派（obtain 系列/sendToTarget）；
  * 72px 实测 native 侧 FindClass(Message) 之后 GetMethodID 仍需类方法表。
  * obtain 是 static（0x9）。只声明已实现的公开方法。 */
 {
  static const struct{const char *name;const char *sig;int access;} msg_methods[]={
   {"obtain","()Landroid/os/Message;",0x9},
   {"obtain","(Landroid/os/Handler;)Landroid/os/Message;",0x9},
   {"obtain","(Landroid/os/Handler;I)Landroid/os/Message;",0x9},
   {"obtain","(Landroid/os/Handler;ILjava/lang/Object;)Landroid/os/Message;",0x9},
   {"obtain","(Landroid/os/Handler;Ljava/lang/Runnable;)Landroid/os/Message;",0x9},
   {"sendToTarget","()V",0x1},
   {"recycle","()V",0x1},
  };
  enum {MSG_N=(int)(sizeof msg_methods/sizeof msg_methods[0])};
  if(c->nmethods==0){
   struct dvm_method *ms=calloc(MSG_N,sizeof *ms);
   if(ms){
    int ok=1;
    for(int i=0;i<MSG_N;i++){
     struct dvm_method *m=&ms[i];
     m->cls=c;m->name=msg_methods[i].name;m->sig=strdup(msg_methods[i].sig);
     m->access=(uint32_t)msg_methods[i].access;
     m->arg_slots=dvm_sig_arg_slots(m->sig);
     m->arg_count=dvm_sig_arg_count(m->sig);
     m->ret_kind=dvm_sig_return_kind(m->sig);
     if(!m->sig)ok=0;
    }
    if(ok){c->methods=ms;c->nmethods=MSG_N;}
    else{for(int i=0;i<MSG_N;i++)free(ms[i].sig);free(ms);}
   }
  }
 }
 c->ifields=calloc(7,sizeof *c->ifields);if(!c->ifields){dvm__throw(v,"java/lang/OutOfMemoryError","Message metadata");return;}c->nifields=c->islots=7;
 message_field(c,0,"what","I");message_field(c,1,"arg1","I");message_field(c,2,"arg2","I");message_field(c,3,"obj","Ljava/lang/Object;");message_field(c,4,"target","Landroid/os/Handler;");message_field(c,5,"when","J");message_field(c,6,"callback","Ljava/lang/Runnable;");
}
dvm_ref thread_runtime_current(struct dvm *v){
 if(v->cur_thread)return v->cur_thread;
 if(run.vm==v&&run.main_java&&pthread_equal(pthread_self(),run.main_host)){
  /* malloc reuses a destroyed dvm's address, so run.main_java can be a ref
   * from the previous instance.  Only return it when this VM's own heap still
   * owns a live object at that index (dvm__obj validates the index against
   * vm->heap_blocks); otherwise the caller sees no thread and fails closed
   * instead of acting on another VM's object. */
  struct dvm_object *o=dvm__obj(v,run.main_java);
  if(o&&o->live)return run.main_java;
 }
 return 0;
}
bool thread_runtime_is_main(struct dvm *v){return run.vm==v&&pthread_equal(pthread_self(),run.main_host);}
static struct worker *worker_for_looper(struct dvm *v,dvm_ref looper){uint64_t identity=thread_ref_identity(v,looper);for(struct worker *w=run.workers;w;w=w->next)if(w->vm==v&&w->ready&&looper==w->looper&&identity&&w->looper_identity==identity)return w;return NULL;}
static struct worker *worker_for_host(struct dvm *v,pthread_t host){for(struct worker *w=run.workers;w;w=w->next)if(w->vm==v&&w->started&&pthread_equal(host,w->thread))return w;return NULL;}
bool thread_runtime_looper(struct dvm *v,dvm_ref looper){return worker_for_looper(v,looper)!=NULL;}
static bool call_method(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *a,int n){
 struct dvm_class *c=dvm_object_class(v,self);union dvm_value out={0};
 /* bitter.jnibridge 代理对象（generated_proxy）自身没有方法表：run/
  * handleMessage 等声明在它携带的接口上，分派必须转发 InvocationHandler。
  * 72py 实测引擎把 Handler$Callback+FrameCallback 代理投递到 worker
  * Looper，此处 find_method 直接失败并打断 nativeRender 消息链。 */
 if(c&&c->generated_proxy){
  if(dvm_proxy_try_invoke(v,self,NULL,name,sig,a,n,&out))return true;
  if(dvm_exception(v))return false;
 }
 struct dvm_method *m=c?dvm_find_method(v,c,name,sig):NULL;
 if(!m){sdk_trace_missing_api(c?c->name:"?",name,sig);dvm__throw(v,"java/lang/NoSuchMethodError","async callback %s%s",name,sig);return false;}return dvm_call(v,m,self,a,n,&out);
}
static void trace_unity_message(struct dvm *v,struct handler *handler,dvm_ref message,const char *phase){
 static _Atomic unsigned count;
 if(!handler||!message)return;
 union dvm_value what={0},obj={0};
 if(!dvm_get_field(v,message,"what","I",&what)||what.i!=2269)return;
 if(atomic_fetch_add(&count,1)>=128)return;
 (void)dvm_get_field(v,message,"obj","Ljava/lang/Object;",&obj);
 struct dvm_class *cc=dvm_object_class(v,handler->callback);
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 fprintf(f,"UNITY_MESSAGE phase=%s message=%u obj=%u callback=%u class=%s exception=%u",phase,message,obj.l,handler->callback,cc&&cc->name?cc->name:"?",dvm_exception(v));
 struct dvm_class *sc=dvm_object_class(v,obj.l);
 if(sc&&sc->name&&!strcmp(sc->name,"com/unity3d/player/S0")&&sc->sslots){
  for(int i=0;i<sc->nsfields;i++){struct dvm_field *field=&sc->sfields[i];if(field->kind=='L'&&field->slot<sc->nsslots&&sc->sslots[field->slot].l==obj.l)fprintf(f," enum_field=%s",field->name);}
 }
 if(cc&&cc->name&&!strcmp(cc->name,"com/unity3d/player/T0")){
  for(int i=0;i<cc->nifields;i++){struct dvm_field *field=&cc->ifields[i];
   if(!field->type||strcmp(field->type,"Lcom/unity3d/player/U0;"))continue;
   union dvm_value owner={0};if(!dvm_get_field(v,handler->callback,field->name,field->type,&owner))continue;
   struct dvm_class *uc=dvm_object_class(v,owner.l);if(!uc)continue;
   for(int j=0;j<uc->nifields;j++){struct dvm_field *uf=&uc->ifields[j];union dvm_value value={0};if((uf->kind=='I'||uf->kind=='Z')&&dvm_get_field(v,owner.l,uf->name,uf->type,&value))fprintf(f," U0.%s=%d",uf->name,value.i);}
  }
 }
 fputc('\n',f);fclose(f);
}
static bool dispatch_body(struct worker *w,struct task *t){
 struct dvm *v=w->vm;
 if(t->runnable){
  /* Handler.post can receive a FutureTask submitted through an executor.
   * Its run() is owned by executor_runtime_call, not by a guest method that
   * dvm_find_method can see.  Without this branch the task fails with
   * NoSuchMethodError and a waiter on Future.get never completes. */
  struct dvm_object *ro=dvm__obj(v,t->runnable);
  bool future_like=false;
  for(struct dvm_class *k=ro?ro->cls:NULL;k;k=k->super)
   if(k->name&&!strcmp(k->name,"java/util/concurrent/FutureTask")){future_like=true;break;}
  if(future_like){union dvm_value ignored={0};return executor_runtime_call(v,"java/util/concurrent/FutureTask","run","()V",t->runnable,NULL,0,&ignored);}
  return call_method(v,t->runnable,"run","()V",NULL,0);
 }
 struct dvm_object *o=dvm__obj(v,t->handler);struct handler *h=o?o->data:NULL;union dvm_value arg={.l=t->message};
 trace_unity_message(v,h,t->message,"enter");
 /* AOSP Looper.dispatchMessage runs Message.callback first, then the
  * Handler.Callback, then Handler.handleMessage.  Message.obtain(handler,
  * runnable) stores its Runnable in this field (NeuronClient's
  * schedulePollAction reached this gap in 72bq). */
 dvm_ref message_callback=0;
 if(t->message){union dvm_value cv={0};if(dvm_get_field(v,t->message,"callback","Ljava/lang/Runnable;",&cv))message_callback=cv.l;}
 if(message_callback){bool cb_ok=call_method(v,message_callback,"run","()V",NULL,0);{char why[256]="-";dvm_ref ex=dvm_exception(v);if(ex)dvm_describe_exception(v,ex,why,sizeof why);FILE*df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(df){fprintf(df,"DISPATCH_CALLBACK callback=%u ok=%d exception=%u why=%s\n",(unsigned)message_callback,cb_ok?1:0,(unsigned)ex,why);fclose(df);}}return cb_ok;}
 if(h&&h->callback){union dvm_value out={0};
  /* Callback receiver 是代理对象时（72py 实测引擎用
   * Proxy.newProxyInstance(Handler$Callback,…) 创建），方法声明在接口上，
   * 必须转发 InvocationHandler；代理类自身没有 handleMessage。 */
  struct dvm_class *cb_cls=dvm_object_class(v,h->callback);
  bool invoked=false;
  if(cb_cls&&cb_cls->generated_proxy){
   invoked=dvm_proxy_try_invoke(v,h->callback,NULL,"handleMessage","(Landroid/os/Message;)Z",&arg,1,&out);
   if(dvm_exception(v))return false;
  }
  if(!invoked){
  struct dvm_method *m=dvm_find_method(v,cb_cls,"handleMessage","(Landroid/os/Message;)Z");
  if(!m){sdk_trace_missing_api("android/os/Handler$Callback","handleMessage","(Landroid/os/Message;)Z");dvm__throw(v,"java/lang/NoSuchMethodError","Handler.Callback");return false;}if(!dvm_call(v,m,h->callback,&arg,1,&out))return false;}
  if(out.i)return true;}
 struct dvm_class *c=dvm_object_class(v,t->handler);struct dvm_method *m=c?dvm_find_method(v,c,"handleMessage","(Landroid/os/Message;)V"):NULL;
 if(m){union dvm_value out={0};return dvm_call(v,m,t->handler,&arg,1,&out);}
 /* The DEX lookup returns overrides only when the boot Handler class is
  * linked into the guest hierarchy. Its inherited default is a no-op. */
 return true;
}
/* A Looper is a long-lived native scheduler nested in Thread.run, not one
 * unbounded Java call. Each scheduled message gets its own bounded budget;
 * preserve the enclosing caller's budget and the monotonic total vm.steps. */
static bool dispatch(struct worker *w,struct task *t){
 struct dvm *v=w->vm;uint64_t saved_steps=v->call_steps,saved_limit=v->step_limit;
 v->call_steps=0;dvm_set_step_limit(v,100000);
 bool ok=dispatch_body(w,t);
 uint64_t used=v->call_steps;v->call_steps=saved_steps;v->step_limit=saved_limit;
 if(w->executed==0||(!ok&&dvm_exception(v)))
  sdk_trace_fprintf(stderr,"LOOPER_TASK_BUDGET used=%llu limit=100000 outer_restored=%llu total_steps=%llu ok=%d\n",(unsigned long long)used,(unsigned long long)saved_steps,(unsigned long long)v->steps,ok?1:0);
 return ok;
}
/* Called only with the GIL: exception and Java identity are thread state. */
static void engine_thread_trace(struct dvm *v,struct worker *w,const char *event,const char *status){
 struct dvm_class *klass=dvm_object_class(v,w->self);dvm_ref ex=dvm_exception(v);char detail[512]="-";
 if(ex)dvm_describe_exception(v,ex,detail,sizeof detail);
 sdk_trace_fprintf(stderr,"ENGINE_THREAD event=%s status=%s class=%s ref=%u pthread=0x%llx cur_thread=%u exception=%u exception_text=%s looper=%u executed=%u\n",
  event,status,klass&&klass->name?klass->name:"?",(unsigned)w->self,
  (unsigned long long)(uintptr_t)pthread_self(),(unsigned)thread_runtime_current(v),(unsigned)ex,detail,(unsigned)w->looper,w->executed);
}
/* Enter with the GIL released; return with it released. */
static void worker_loop(struct worker *w){
 struct dvm *v=w->vm;
 for(;;){pthread_mutex_lock(&queue_lock);while(!w->quit&&(!w->head||w->head->due>now_ms())){struct timespec until=deadline(20);pthread_cond_timedwait(&changed,&queue_lock,&until);}if(w->quit){struct task *cancelled=w->head;w->gc_retiring=cancelled;w->head=NULL;w->queued=0;pthread_mutex_unlock(&queue_lock);dvm_gil_acquire(v);while(cancelled){struct task *next=cancelled->next;if(cancelled->message)message_recycle_unchecked(v,cancelled->message);pthread_mutex_lock(&queue_lock);w->gc_retiring=next;pthread_mutex_unlock(&queue_lock);free(cancelled);cancelled=next;w->cancelled++;}dvm_gil_release(v);break;}struct task *t=w->head;w->head=t->next;t->next=NULL;w->gc_inflight=t;--w->queued;w->active=true;pthread_mutex_unlock(&queue_lock);
  bool failed=false;dvm_gil_acquire(v);if(!dvm_is_stopping(v)){bool ok=dispatch(w,t);if(!ok||dvm_exception(v)){if(w->plain)engine_thread_trace(v,w,"dispatch","failed");{char why[512]="-";dvm_ref ex=dvm_exception(v);if(ex)dvm_describe_exception(v,ex,why,sizeof why);FILE*df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(df){dvm_ref receiver=t->runnable?t->runnable:t->handler;struct dvm_object *receiver_object=dvm__obj(v,receiver);struct dvm_class *receiver_class=receiver_object?receiver_object->cls:NULL;fprintf(df,"WORKER_DISPATCH_FAILED ok=%d plain=%d why=%s pid=%d tid=%ld worker=%u looper=%u guest_thread=%u handler=%u runnable=%u message=%u task_receiver=%u task_class=%s task_method=%s task_sig=%s total_steps=%llu outer_call_steps=%llu executed_before=%u\n",ok?1:0,w->plain?1:0,why,getpid(),(long)syscall(SYS_gettid),(unsigned)w->self,(unsigned)w->looper,(unsigned)thread_runtime_current(v),(unsigned)t->handler,(unsigned)t->runnable,(unsigned)t->message,(unsigned)receiver,receiver_class&&receiver_class->name?receiver_class->name:"?",t->runnable?"run":"handler_dispatch",t->runnable?"()V":"(Landroid/os/Message;)V",(unsigned long long)v->steps,(unsigned long long)v->call_steps,w->executed);fclose(df);}}sdk_trace_async_failure(v,"dispatch");dvm_clear_exception(v);failed=true;}}if(t->message)message_recycle_unchecked(v,t->message);pthread_mutex_lock(&queue_lock);w->gc_inflight=NULL;pthread_mutex_unlock(&queue_lock);free(t);dvm_gil_release(v);
  pthread_mutex_lock(&queue_lock);++w->executed;w->active=false;if(failed){w->quit=true;w->loop_failed=true;}pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
 }
}
static void *plain_worker_main(void *arg){
 struct worker *w=arg;struct dvm *v=w->vm;
 dvm__mark_bytecode_thread();dvm_gil_acquire(v);dvm__tstate_reset(v);v->cur_thread=w->self;v->quiet_uncaught=true;
 struct plain_thread *t=plain_find(v,w->self);struct dvm_class *klass=dvm_object_class(v,w->self);
 engine_thread_trace(v,w,"thread_enter","running");
 struct dvm_method *method=klass?dvm_find_method(v,klass,"run","()V"):NULL;
 dvm_ref receiver=w->self;
 if(!method||(!method->has_code&&!method->builtin)){
  receiver=t?t->target:0;klass=receiver?dvm_object_class(v,receiver):NULL;
  method=klass?dvm_find_method(v,klass,"run","()V"):NULL;
 }
 engine_thread_trace(v,w,"run_enter",receiver?"invoke":"empty");
 bool failed=false;
 if(receiver){union dvm_value out={0};if(!method||!dvm_call(v,method,receiver,NULL,0,&out)||dvm_exception(v)){if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","Thread target run()V");failed=true;engine_thread_trace(v,w,"run_exit","failed");sdk_trace_async_failure(v,"Thread.run");dvm_clear_exception(v);}}
 if(!failed)engine_thread_trace(v,w,"run_exit",receiver?(w->loop_failed?"loop_dispatch_failed":"returned"):"empty");
 if(t)t->alive=false;
 pthread_mutex_lock(&queue_lock);w->alive=false;w->active=false;w->quit=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
 detach_worker_jni(v);dvm_gil_release(v);return NULL;
}
static bool plain_start(struct dvm *v,struct plain_thread *t){
 if(t->started){dvm__throw(v,"java/lang/IllegalThreadStateException","Thread already started");return true;}
 struct worker *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","Thread worker");return true;}
 w->vm=v;w->self=t->self;w->name=t->name;w->started=w->alive=w->active=w->plain=true;
 pthread_mutex_lock(&queue_lock);w->next=run.workers;run.workers=w;t->started=t->alive=true;
 int rc=pthread_create(&w->thread,NULL,plain_worker_main,w);
 if(rc){run.workers=w->next;t->started=t->alive=false;}pthread_mutex_unlock(&queue_lock);
 if(rc){free(w);dvm__throw(v,"java/lang/OutOfMemoryError","pthread_create error %d",rc);}else{struct dvm_class *klass=dvm_object_class(v,t->self);sdk_trace_fprintf(stderr,"ENGINE_THREAD event=start status=created class=%s ref=%u pthread=0x%llx creator_cur_thread=%u\n",klass&&klass->name?klass->name:"?",(unsigned)t->self,(unsigned long long)(uintptr_t)w->thread,(unsigned)thread_runtime_current(v));}return true;
}
/* Real pthread + implicit Handler + queue dispatch + Looper.quit probe. */
static struct {struct dvm *vm;dvm_ref thread,looper;int run,dispatch,identity;} plain_probe;
static bool plain_probe_task(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)a;(void)n;(void)out;
 plain_probe.dispatch++;plain_probe.identity+=dvm_gil_held(v)&&thread_runtime_current(v)==plain_probe.thread;
 union dvm_value ignored={0};return thread_runtime_call(v,"android/os/Looper","quit","()V",plain_probe.looper,NULL,0,&ignored);
}
static bool plain_probe_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)out;plain_probe.run++;plain_probe.identity+=dvm_gil_held(v)&&thread_runtime_current(v)==self;
 union dvm_value r={0};
 if(!thread_runtime_call(v,"android/os/Looper","prepare","()V",0,NULL,0,&r)||dvm_exception(v))return false;
 if(!thread_runtime_call(v,"android/os/Looper","myLooper","()Landroid/os/Looper;",0,NULL,0,&r)||!r.l)return false;
 plain_probe.looper=r.l;
 struct dvm_class *hc=dvm_find_class_or_external(v,"android/os/Handler"),*tc=dvm_find_class_or_external(v,"sdktrace/tests/PlainThreadTask");
 dvm_ref handler=hc?dvm_new_object(v,hc):0,task=tc?dvm_new_object(v,tc):0;
 if(!handler||!task||!android_basics_call(v,"android/os/Handler","<init>","()V",handler,NULL,0,&r)||dvm_exception(v))return false;
 union dvm_value arg={.l=task};
 if(!thread_runtime_call(v,"android/os/Handler","post","(Ljava/lang/Runnable;)Z",handler,&arg,1,&r)||!r.i||dvm_exception(v))return false;
 return thread_runtime_call(v,"android/os/Looper","loop","()V",0,NULL,0,&r)&&!dvm_exception(v);
}
bool thread_runtime_plain_selftest(struct dvm *v){
 if(!v||!dvm_gil_held(v)||dvm_exception(v))return false;
 struct dvm_class *tc=dvm__register_builtin(v,"Lsdktrace/tests/PlainThreadTask;"),*pc=dvm__register_builtin(v,"Lsdktrace/tests/PlainThreadProbe;");
 struct dvm_class *base=dvm_find_class_or_external(v,"java/lang/Thread");
 if(!tc||!pc||!base||tc->methods||pc->methods)return false;
 tc->methods=calloc(1,sizeof *tc->methods);pc->methods=calloc(1,sizeof *pc->methods);
 if(!tc->methods||!pc->methods)return false;
 tc->nmethods=pc->nmethods=1;pc->super=base;
 if(!stack_trace_element_install(tc,0,"run","()V",plain_probe_task,'V',0)||!stack_trace_element_install(pc,0,"run","()V",plain_probe_run,'V',0))return false;
 dvm_ref thread=dvm_new_object(v,pc);union dvm_value out={0};if(!thread)return false;
 memset(&plain_probe,0,sizeof plain_probe);plain_probe.vm=v;plain_probe.thread=thread;
 union dvm_value thread_name={.l=dvm_new_string(v,"plain-thread-test")};
 if(!thread_name.l||!thread_runtime_call(v,"java/lang/Thread","<init>","(Ljava/lang/String;)V",thread,&thread_name,1,&out)||dvm_exception(v))return false;
 if(!thread_runtime_call(v,"java/lang/Thread","getName","()Ljava/lang/String;",thread,NULL,0,&out)||out.l!=thread_name.l)return false;
 if(!thread_runtime_call(v,"java/lang/Thread","start","()V",thread,NULL,0,&out)||dvm_exception(v))return false;
 if(!thread_runtime_call(v,"java/lang/Thread","start","()V",thread,NULL,0,&out)||!dvm_exception(v))return false;
 dvm_clear_exception(v);
 uint64_t end=now_ms()+2000;struct plain_thread *t=plain_find(v,thread);
 while(t&&t->alive&&now_ms()<end){unsigned depth=dvm_gil_unlock_all(v);pthread_mutex_lock(&queue_lock);struct timespec until=deadline(10);pthread_cond_timedwait(&changed,&queue_lock,&until);pthread_mutex_unlock(&queue_lock);dvm_gil_relock(v,depth);}
 bool ok=t&&!t->alive&&plain_probe.run==1&&plain_probe.dispatch==1&&plain_probe.identity==2&&!dvm_exception(v);
 struct worker *w=run.workers;while(w&&w->self!=thread)w=w->next;
 if(w&&!w->joined){pthread_mutex_lock(&queue_lock);bool busy=w->active;w->quit=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);if(busy)dvm_request_stop(v);thread_runtime_join_host(v,w->thread);w->joined=true;}
 sdk_trace_fprintf(stderr,"PLAIN_THREAD_SELFTEST %s real_pthread=1 prepare_loop=1 handler_post=1 identity_gil=%d\n",ok?"PASS":"FAIL",plain_probe.identity);
 return ok;
}
static void *worker_main(void *arg){
 struct worker *w=arg;struct dvm *v=w->vm;int priority_error=0,priority_observed=0;
 bool priority_ok=thread_priority_apply(w->priority,&priority_error,&priority_observed);
 if(!priority_ok)sdk_trace_fprintf(stderr,"THREAD_PRIORITY_FAIL priority=%d errno=%d action=continue_worker\n",w->priority,priority_error);
 dvm__mark_bytecode_thread();dvm_gil_acquire(v);dvm__tstate_reset(v);v->cur_thread=w->self;v->quiet_uncaught=true;
 dvm_ref looper=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Looper"));
 pthread_mutex_lock(&queue_lock);w->priority_attempted=true;w->priority_applied=priority_ok;w->priority_errno=priority_error;w->priority_nice=priority_observed;w->looper=looper;w->looper_identity=thread_ref_identity(v,looper);w->ready=looper!=0;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
 if(!looper){dvm__throw(v,"java/lang/OutOfMemoryError","worker Looper");sdk_trace_async_failure(v,"prepare Looper");dvm_clear_exception(v);pthread_mutex_lock(&queue_lock);w->quit=true;pthread_mutex_unlock(&queue_lock);}
 struct dvm_method *prep=dvm_find_method(v,dvm_object_class(v,w->self),"onLooperPrepared","()V");
 if(prep&&(prep->has_code||prep->builtin)){union dvm_value out={0};dvm_set_step_limit(v,100000);if(!dvm_call(v,prep,w->self,NULL,0,&out)){sdk_trace_async_failure(v,"onLooperPrepared");dvm_clear_exception(v);pthread_mutex_lock(&queue_lock);w->quit=true;pthread_mutex_unlock(&queue_lock);}}
 dvm_gil_release(v);
 worker_loop(w);
 pthread_mutex_lock(&queue_lock);w->alive=false;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
 dvm_gil_acquire(v);detach_worker_jni(v);dvm_gil_release(v);return NULL;
}
static bool start(struct dvm *v,struct worker *w){
 pthread_mutex_lock(&queue_lock);if(w->started){pthread_mutex_unlock(&queue_lock);dvm__throw(v,"java/lang/IllegalThreadStateException","HandlerThread already started");return true;}
 struct dvm_method *override=dvm_find_method(v,dvm_object_class(v,w->self),"run","()V");if(override&&(override->has_code||override->builtin)){pthread_mutex_unlock(&queue_lock);sdk_trace_missing_api(dvm_class_name(override->cls),"run","()V");dvm__throw(v,"java/lang/UnsupportedOperationException","custom Thread.run requires full worker lifecycle");return true;}
 w->started=w->alive=true;w->next=run.workers;run.workers=w;int rc=pthread_create(&w->thread,NULL,worker_main,w);if(rc){w->started=w->alive=false;run.workers=w->next;}pthread_mutex_unlock(&queue_lock);if(rc)dvm__throw(v,"java/lang/OutOfMemoryError","pthread_create error %d",rc);return true;
}
static dvm_ref await_looper(struct dvm *v,struct worker *w){
 uint64_t end=now_ms()+2000;pthread_mutex_lock(&queue_lock);
 while(w->alive&&!w->ready&&now_ms()<end){unsigned depth=dvm_gil_unlock_all(v);struct timespec until=deadline(20);pthread_cond_timedwait(&changed,&queue_lock,&until);pthread_mutex_unlock(&queue_lock);dvm_gil_relock(v,depth);pthread_mutex_lock(&queue_lock);}
 bool timeout=w->alive&&!w->ready;dvm_ref result=w->alive&&w->ready?w->looper:0;pthread_mutex_unlock(&queue_lock);if(timeout)dvm__throw(v,"java/lang/IllegalStateException","Looper publication timed out");return result;
}
static bool enqueue(struct dvm *v,dvm_ref handler,dvm_ref runnable,dvm_ref message,int64_t delay,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,handler);struct handler *h=o?o->data:NULL;struct worker *w=h?worker_for_looper(v,h->looper):NULL;
 {union dvm_value cv={0},wv={0},ov={0};bool cok=message&&dvm_get_field(v,message,"callback","Ljava/lang/Runnable;",&cv);bool wok=message&&dvm_get_field(v,message,"what","I",&wv);bool ook=message&&dvm_get_field(v,message,"obj","Ljava/lang/Object;",&ov);
  const char*oc="?";if(ov.l){struct dvm_class*ocls=dvm_object_class(v,ov.l);if(ocls&&ocls->name)oc=ocls->name;}
  FILE*df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(df){fprintf(df,"HANDLER_ENQUEUE handler=%u runnable=%u message=%u handler_state=%d worker=%d looper_ok=%d callback_read=%d callback=%u what_read=%d what=%d obj_read=%d obj=%u obj_class=%s\n",(unsigned)handler,(unsigned)runnable,(unsigned)message,h?1:0,w?1:0,h?thread_runtime_looper(v,h->looper)!=NULL:0,cok,(unsigned)cv.l,wok,wok?wv.i:-1,ook,(unsigned)ov.l,oc);fclose(df);}}
 if(!h||!w||!thread_runtime_looper(v,h->looper))return false;
 if(!runnable&&!message){dvm__throw(v,"java/lang/NullPointerException","posted task");return true;}
 struct message_lifetime_state *ms=message?message_state(v,message):NULL;if(message&&(!ms||ms->state!=MESSAGE_OWNED)){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Message already in use");return true;}
 struct task *t=calloc(1,sizeof *t);if(!t){dvm__throw(v,"java/lang/OutOfMemoryError","queued task");return true;}t->handler=handler;t->runnable=runnable;t->message=message;t->due=now_ms()+(delay>0?(uint64_t)delay:0);
 pthread_mutex_lock(&queue_lock);if(w->quit||!w->alive){pthread_mutex_unlock(&queue_lock);free(t);out->i=0;return true;}if(w->queued>=1024){pthread_mutex_unlock(&queue_lock);free(t);dvm__throw(v,"java/lang/IllegalStateException","diagnostic queue full");return true;}
 if(message){for(struct task *queued=w->head;queued;queued=queued->next)if(queued->message==message){pthread_mutex_unlock(&queue_lock);free(t);dvm__throw(v,"java/lang/IllegalStateException","Message already queued");return true;}
  union dvm_value target={.l=handler},when={.j=(int64_t)t->due};if(!dvm_set_field(v,message,"target","Landroid/os/Handler;",target)||!dvm_set_field(v,message,"when","J",when)){pthread_mutex_unlock(&queue_lock);free(t);dvm__throw(v,"java/lang/IllegalArgumentException","invalid Message object");return true;}}
 if(ms){ms->state=MESSAGE_IN_USE;dvm_pin(v,message);}
 bool front=delay==INT64_MIN;if(front)t->due=0;
 t->sequence=++run.sequence;struct task **at=&w->head;while(!front&&*at&&((*at)->due<t->due||((*at)->due==t->due&&(*at)->sequence<t->sequence)))at=&(*at)->next;t->next=*at;*at=t;++w->queued;out->i=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);{FILE*df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(df){fprintf(df,"HANDLER_ENQUEUED handler=%u message=%u queued=%u\n",(unsigned)handler,(unsigned)message,w->queued);fclose(df);}}return true;
}
static bool send_empty(struct dvm *v,dvm_ref handler,int what,int64_t delay,union dvm_value *out){
 struct dvm_object *h=dvm__obj(v,handler);if(!h||!h->data||!thread_runtime_looper(v,((struct handler *)h->data)->looper))return false;
 dvm_ref message=message_obtain(v);
 if(!message){dvm__throw(v,"java/lang/OutOfMemoryError","Message");return true;}
 union dvm_value value={.i=what};
 if(!dvm_set_field(v,message,"what","I",value)){dvm__throw(v,"java/lang/IllegalArgumentException","Message.what unavailable");return true;}
 bool handled=enqueue(v,handler,0,message,delay,out);
 /* This Message was never exposed to the caller. Failed publication must
  * return it too; an external sendMessage argument remains caller-owned. */
 struct dvm_object *mo=dvm__obj(v,message);
 if(mo&&mo->data_destroy==message_state_free&&((struct message_lifetime_state *)mo->data)->state==MESSAGE_OWNED)message_recycle_unchecked(v,message);
 return handled;
}
/* AOSP Handler.obtainMessage(...) 是 Message.obtain(this,...) 的便捷方法：
 * 构造 Message 并把 target 设为该 Handler。它不触碰任何队列，也不需要
 * Handler 已绑定 Looper。参数序 (what) (what,obj) (what,arg1,arg2)
 * (what,arg1,arg2,obj)。72pw 实测 native 侧 GetMethodID 缺 (I) 变体留
 * pending=1225 不消。 */
static bool handler_obtain(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!self){dvm__throw(v,"java/lang/NullPointerException","Handler receiver");return true;}
 dvm_ref msg=message_obtain(v);
 if(!msg){dvm__throw(v,"java/lang/OutOfMemoryError","Message");return true;}
 union dvm_value target={.l=self};
 if(!dvm_set_field(v,msg,"target","Landroid/os/Handler;",target)){dvm__throw(v,"java/lang/IllegalArgumentException","Message fields unavailable");return true;}
 if(n>=1){
  union dvm_value what={.i=a[0].i};
  if(!dvm_set_field(v,msg,"what","I",what)){dvm__throw(v,"java/lang/IllegalArgumentException","Message fields unavailable");return true;}
 }
 if(n>=3){
  union dvm_value arg1={.i=a[1].i},arg2={.i=a[2].i};
  if(!dvm_set_field(v,msg,"arg1","I",arg1)||!dvm_set_field(v,msg,"arg2","I",arg2)){dvm__throw(v,"java/lang/IllegalArgumentException","Message fields unavailable");return true;}
 }
 if(n==2||n==4){
  union dvm_value obj={.l=a[n-1].l};
  if(!dvm_set_field(v,msg,"obj","Ljava/lang/Object;",obj)){dvm__throw(v,"java/lang/IllegalArgumentException","Message fields unavailable");return true;}
 }
 out->l=msg;return true;
}
/* AOSP Handler.hasMessages(what) delegates to MessageQueue.hasMessages(handler,
 * what, null), which scans every pending Message for target==handler and a
 * matching what. post()/postDelayed() enqueue Message.obtain(handler,
 * runnable), i.e. what==0, so a queued Runnable matches what 0. The scan uses
 * the same worker queue and queue_lock as enqueue/dispatch. A Handler whose
 * Looper is not an owned worker falls through (fail-closed), never a fake 0. */
static bool task_what(struct dvm *v,const struct task *t,int what){
 if(!t->message)return what==0;
 union dvm_value value={0};
 return dvm_get_field(v,t->message,"what","I",&value)&&value.i==what;
}
static bool has_messages(struct dvm *v,dvm_ref handler,int what,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,handler);struct handler *h=o?o->data:NULL;struct worker *w=h?worker_for_looper(v,h->looper):NULL;
 if(!h||!w||!thread_runtime_looper(v,h->looper))return false;
 out->i=0;pthread_mutex_lock(&queue_lock);
 for(struct task *t=w->head;t;t=t->next)if(t->handler==handler&&task_what(v,t,what)){out->i=1;break;}
 pthread_mutex_unlock(&queue_lock);return true;
}
static bool remove_messages(struct dvm *v,dvm_ref handler,int what){
 struct dvm_object *o=dvm__obj(v,handler);struct handler *h=o?o->data:NULL;struct worker *w=h?worker_for_looper(v,h->looper):NULL;
 if(!h||!w||!thread_runtime_looper(v,h->looper))return false;
 pthread_mutex_lock(&queue_lock);struct task **at=&w->head;
 while(*at){struct task *t=*at;if(t->handler==handler&&task_what(v,t,what)){*at=t->next;--w->queued;if(t->message)message_recycle_unchecked(v,t->message);free(t);}else at=&t->next;}
 pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);return true;
}
void thread_runtime_join_host(struct dvm *v,pthread_t thread){unsigned depth=dvm_gil_unlock_all(v);pthread_join(thread,NULL);dvm_gil_relock(v,depth);}
#include "choreographer_runtime.inc"
struct thread_gc_scan {
 struct dvm *vm;dvm_gc_visit_fn visit;void *user;size_t slots;bool ok;
};
static void thread_gc_slot(struct thread_gc_scan *s,dvm_ref *slot){
 if(!slot||!*slot)return;
 if(!dvm__obj(s->vm,*slot)){s->ok=false;return;}
 ++s->slots;s->visit(slot,s->user);
 if(*slot&&!dvm__obj(s->vm,*slot))s->ok=false;
}
static void thread_gc_tasks(struct thread_gc_scan *s,struct task *head){
 size_t guard=0;
 for(struct task *t=head;t;t=t->next){
  if(++guard>1000000){s->ok=false;return;}
  thread_gc_slot(s,&t->handler);thread_gc_slot(s,&t->runnable);thread_gc_slot(s,&t->message);
 }
}
/* Exact producer identities; never classify by guest class name. */
int thread_runtime_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 if(!o||!o->data)return DVM_GC_PAYLOAD_UNKNOWN;
 if(o->data_destroy==worker_payload_release){struct worker *w=o->data;if(w->vm!=v)return DVM_GC_PAYLOAD_INVALID;if(!visit)return DVM_GC_PAYLOAD_REFS;
  if(pthread_mutex_trylock(&queue_lock))return DVM_GC_PAYLOAD_INVALID;
  struct thread_gc_scan scan={.vm=v,.visit=visit,.user=user,.ok=true};thread_gc_slot(&scan,&w->name);thread_gc_slot(&scan,&w->looper);thread_gc_tasks(&scan,w->head);thread_gc_tasks(&scan,w->gc_inflight);thread_gc_tasks(&scan,w->gc_retiring);
  pthread_mutex_unlock(&queue_lock);return scan.ok?DVM_GC_PAYLOAD_REFS:DVM_GC_PAYLOAD_INVALID;
 }
 if(o->data_destroy==message_state_free)return DVM_GC_PAYLOAD_ATOMIC; /* enum only; Message references are declared slots */
 if(o->data_destroy==choreo_request_release){
  struct choreo_request *r=o->data;if(!r->owner||r->owner->vm!=v)return DVM_GC_PAYLOAD_INVALID;
  if(!visit)return DVM_GC_PAYLOAD_REFS;
  if(pthread_mutex_trylock(&queue_lock))return DVM_GC_PAYLOAD_INVALID;
  if(!r->released){visit(&r->callback,user);visit(&r->runnable,user);}
  visit(&r->owner->self,user);visit(&r->owner->looper,user);
  if(r->task){visit(&r->task->handler,user);visit(&r->task->runnable,user);visit(&r->task->message,user);}
  pthread_mutex_unlock(&queue_lock);return DVM_GC_PAYLOAD_REFS;
 }
 if(o->data_destroy==choreo_cookie_release){
  struct choreo *c=o->data;if(c->vm!=v)return DVM_GC_PAYLOAD_INVALID;
  if(!visit)return DVM_GC_PAYLOAD_REFS;if(pthread_mutex_trylock(&queue_lock))return DVM_GC_PAYLOAD_INVALID;
  struct thread_gc_scan scan={.vm=v,.visit=visit,.user=user,.ok=true};
  thread_gc_slot(&scan,&c->looper);thread_gc_tasks(&scan,c->main_head);
  size_t guard=0;for(struct choreo_request *r=c->all;r;r=r->all_next){if(++guard>1000000){scan.ok=false;break;}if(!r->released){thread_gc_slot(&scan,&r->callback);thread_gc_slot(&scan,&r->runnable);}}
  guard=0;for(struct choreo_request *r=c->retired;r;r=r->all_next){if(++guard>1000000){scan.ok=false;break;}thread_gc_slot(&scan,&r->runnable);}
  pthread_mutex_unlock(&queue_lock);return scan.ok?DVM_GC_PAYLOAD_REFS:DVM_GC_PAYLOAD_INVALID;
 }
 if(o->data_destroy==rrw_state_destroy||o->data_destroy==rrw_handle_destroy){
  struct rrw_lock *l=o->data_destroy==rrw_state_destroy?o->data:((struct rrw_handle *)o->data)->lock;
  if(!l||l->vm!=v)return DVM_GC_PAYLOAD_INVALID;
  if(visit){visit(&l->self,user);visit(&l->read_lock,user);visit(&l->write_lock,user);}return DVM_GC_PAYLOAD_REFS;
 }
 return DVM_GC_PAYLOAD_UNKNOWN;
}
bool thread_runtime_gc_visit_owner(struct dvm *v,dvm_ref owner,dvm_gc_visit_fn visit,void *user){
 if(!v||!visit||!dvm_gil_held(v))return false;
 struct plain_thread *t=plain_find(v,owner);if(!t)return true;
 struct thread_gc_scan scan={.vm=v,.visit=visit,.user=user,.ok=true};
 thread_gc_slot(&scan,&t->target);thread_gc_slot(&scan,&t->name);thread_gc_slot(&scan,&t->group);return scan.ok;
}
/* GIL -> queue_lock matches enqueue/shutdown. Never wait if the collector
 * reenters from a native critical section: a busy lock rejects the scan.
 * The visitor must not allocate or call guest code.
 * Only active scheduler ownership is a root. Historical worker records and
 * unstarted plain Thread payloads are owner edges for the later GC stage. */
static bool thread_runtime_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 if(!v||!visit||context||!dvm_gil_held(v)||run.vm!=v)return false;
 struct thread_gc_scan s={.vm=v,.visit=visit,.user=user,.ok=true};
 if(pthread_mutex_trylock(&queue_lock))return false;
 thread_gc_slot(&s,&run.main_java);
 for(struct worker *w=run.workers;w;w=w->next){
  if(w->vm!=v)continue;
  thread_gc_slot(&s,&w->self); /* published native registry owns worker ID until shutdown */
  if(w->alive||w->gc_inflight||w->gc_retiring||w->head){
   thread_gc_slot(&s,&w->self);thread_gc_slot(&s,&w->name);thread_gc_slot(&s,&w->looper);
   thread_gc_tasks(&s,w->head);thread_gc_tasks(&s,w->gc_inflight);thread_gc_tasks(&s,w->gc_retiring);
   if(w->queued&&!w->head)s.ok=false;
  }
 }
 for(struct plain_thread *t=plain_threads;t;t=t->next)if(t->vm==v&&t->started&&t->alive){
  if(t->identity!=thread_ref_identity(v,t->self)){s.ok=false;continue;}
  thread_gc_slot(&s,&t->self);thread_gc_slot(&s,&t->target);thread_gc_slot(&s,&t->name);thread_gc_slot(&s,&t->group);
 }
 if(message_pool.vm==v){
  if(message_pool.count>50)s.ok=false;
  else for(unsigned i=0;i<message_pool.count;++i)thread_gc_slot(&s,&message_pool.refs[i]);
 }
 for(unsigned i=0;i<CHOREO_LIMIT;++i){
  struct choreo *c=&choreos[i];
  if(c->vm==v&&c->active){
   thread_gc_slot(&s,&c->self);thread_gc_slot(&s,&c->looper);thread_gc_tasks(&s,c->main_head);
   size_t guard=0;
   for(struct choreo_request *r=c->all;r;r=r->all_next){
    if(++guard>1000000){s.ok=false;break;}
    if(!r->released){thread_gc_slot(&s,&r->callback);thread_gc_slot(&s,&r->runnable);}
   }
   guard=0;
   for(struct choreo_request *r=c->retired;r;r=r->all_next){
    if(++guard>1000000){s.ok=false;break;}
    thread_gc_slot(&s,&r->runnable);
   }
  }
  struct queue_identity *q=&queue_identities[i];
  if(q->vm==v){thread_gc_slot(&s,&q->self);thread_gc_slot(&s,&q->looper);}
 }
 pthread_mutex_unlock(&queue_lock);
 return s.ok;
}
struct thread_gc_test {struct dvm *vm;size_t slots;dvm_ref main;bool found,ok;};
static void thread_gc_test_visit(dvm_ref *slot,void *user){
 struct thread_gc_test *s=user;++s->slots;
 if(!slot||!*slot||!dvm__obj(s->vm,*slot))s->ok=false;
 if(slot&&*slot==s->main)s->found=true;
}
bool thread_runtime_gc_roots_selftest(struct dvm *v){
 if(!v||run.vm!=v||!dvm_gil_held(v))return false;
 struct thread_gc_test s={.vm=v,.main=run.main_java,.ok=true};
 bool ok=thread_runtime_gc_roots(v,thread_gc_test_visit,&s,NULL)&&s.ok&&s.found&&s.slots;
 sdk_trace_fprintf(stderr,"THREAD_GC_ROOTS_SELFTEST %s slots=%zu active_registry_only=1 removed_inflight=1 payload_graph=deferred\n",ok?"PASS":"FAIL",s.slots);
 return ok;
}
void thread_runtime_shutdown(struct dvm *v,unsigned grace){
 atomic_store_explicit(&rrw_shutdown_vm,v,memory_order_release);
 for(struct worker *w=run.workers;w;w=w->next){if(w->vm!=v||!w->started||w->joined)continue;uint64_t end=now_ms()+grace;pthread_mutex_lock(&queue_lock);
  while((w->active||(w->head&&w->head->due<=now_ms()))&&now_ms()<end){unsigned depth=dvm_gil_unlock_all(v);struct timespec until=deadline(10);pthread_cond_timedwait(&changed,&queue_lock,&until);pthread_mutex_unlock(&queue_lock);dvm_gil_relock(v,depth);pthread_mutex_lock(&queue_lock);}
  w->quit=true;while(w->head){struct task *t=w->head;w->head=t->next;if(t->message)message_recycle_unchecked(v,t->message);free(t);++w->cancelled;}w->queued=0;bool busy=w->active;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);if(busy)dvm_request_stop(v);
  thread_runtime_join_host(v,w->thread);w->joined=true;sdk_trace_fprintf(stderr,"THREAD_SHUTDOWN joined=1 executed=%u cancelled=%u busy_at_deadline=%d\n",w->executed,w->cancelled,busy);
 }
 struct plain_thread **pp=&plain_threads;while(*pp){struct plain_thread *t=*pp;if(t->vm==v){*pp=t->next;free(t);}else pp=&t->next;}
 /* Address-reuse guard for the worker records only: malloc hands a later VM
  * the same pointer value, and worker_for_looper() compares w->vm.  A NULL
  * vm never matches a live VM.  run.vm/run.main_java/run.main_host are NOT
  * cleared here: activity_runtime_selftest() runs before the next VM's
  * thread_runtime_init() re-establishes them and requires
  * thread_runtime_is_main() to be true for the process-wide main thread
  * (clearing run.main_host made the selftest fail in 72bz).  The remaining
  * run.main_java staleness on address reuse is handled in
  * thread_runtime_current() by validating the object against this VM. */
 for(struct worker *w=run.workers;w;w=w->next)if(w->vm==v)w->vm=NULL;
 /* rrw_shutdown_vm is only a wake-up signal for waits that were blocked while
  * this shutdown was running.  Once every worker is joined, keeping it set
  * would make every later ReentrantReadWriteLock acquisition on the same VM
  * fail with "aborted by VM shutdown" even though the VM is live again (the
  * diagnostic dispatch selftest runs in exactly that state). */
 if(atomic_load_explicit(&rrw_shutdown_vm,memory_order_acquire)==v)
  atomic_store_explicit(&rrw_shutdown_vm,NULL,memory_order_release);
 choreographer_shutdown(v);
 if(message_pool.vm==v){while(message_pool.count)dvm_unpin(v,message_pool.refs[--message_pool.count]);message_pool.vm=NULL;}
 dvm_gc_root_provider_remove(v,"thread_runtime",NULL);
}
#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
bool thread_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!strcmp(c,"java/lang/Thread")&&(IS("sleep","(J)V")||IS("sleep","(JI)V"))){
  int nanos=n==2?a[1].i:0;
  if(a[0].j<0||nanos<0||nanos>999999){dvm__throw(v,"java/lang/IllegalArgumentException","Thread.sleep duration");return true;}
  uint64_t wait=(uint64_t)a[0].j+(nanos?1:0),start=now_ms();
  for(;;){
   if(dvm__thread_take_interrupt(v)){dvm__throw(v,"java/lang/InterruptedException","Thread.sleep interrupted");return true;}
   if(dvm_is_stopping(v)){dvm__throw(v,"java/lang/InterruptedException","VM stopping during sleep");return true;}
   uint64_t elapsed=now_ms()-start;if(elapsed>=wait)return true;
   unsigned ms=(unsigned)((wait-elapsed)>20?20:wait-elapsed);
   unsigned depth=dvm_gil_unlock_all(v);struct timespec t={.tv_sec=0,.tv_nsec=(long)ms*1000000L};while(nanosleep(&t,&t)&&errno==EINTR){}dvm_gil_relock(v,depth);
  }
 }
 if(choreographer_call(v,c,m,sig,self,a,n,out))return true;
 if(c&&m&&!strcmp(c,"android/os/Message")&&!strncmp(m,"obtain",6)){FILE*df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(df){fprintf(df,"THREAD_RUNTIME_OBTAIN sig=%s n=%d\n",sig?sig:"?",n);fclose(df);}}
 (void)n;
 if(rrwlock_call(v,c,m,sig,self,a,n,out))return true;
 if(!strcmp(c,"android/os/Process")&&(IS("setThreadPriority","(I)V")||IS("setThreadPriority","(II)V"))){
  bool explicit_tid=IS("setThreadPriority","(II)V");
  if(!a||n!=(explicit_tid?2:1)){dvm__throw(v,"java/lang/IllegalArgumentException","setThreadPriority arguments");return true;}
  int tid=explicit_tid?a[0].i:0;
  int priority=a[explicit_tid?1:0].i;int error=0,observed=0;
  if(tid<0){dvm__throw(v,"java/lang/IllegalArgumentException","Invalid thread ID: %d",tid);return true;}
  if(!thread_priority_valid(priority)){dvm__throw(v,"java/lang/IllegalArgumentException","Priority out of range: %d",priority);return true;}
  if(!thread_priority_apply_for((id_t)tid,priority,&error,&observed)){
   sdk_trace_fprintf(stderr,"THREAD_PRIORITY_FAIL priority=%d errno=%d action=throw\n",priority,error);
   if(error==EPERM||error==EACCES)dvm__throw(v,"java/lang/SecurityException","Cannot set thread priority (errno=%d)",error);
   else if(error==ESRCH)dvm__throw(v,"java/lang/IllegalArgumentException","Unknown thread ID: %d",tid);
   else if(error==EINVAL)dvm__throw(v,"java/lang/IllegalArgumentException","Priority out of range: %d",priority);
   else dvm__throw(v,"java/lang/RuntimeException","setpriority failed (errno=%d)",error);
  }
  return true;
 }
 /* A DEX subclass of Thread (Bugly's proguard.bg) invokes Thread's own
  * getName/setName/setDaemon/... with the subclass as the declared method
  * owner.  Those methods exist only in this builtin dispatch, never in the
  * subclass's method table, so the receiver's derived type must match too;
  * without this the call fell through to the host stubs as
  * NoSuchMethodError bg.setName(String) in 72bt.  Construction stays with the
  * java/lang/Thread owner because the super constructor call is declared
  * there, and a subclass constructor must not register a second state. */
 bool thread_owner=!strcmp(c,"java/lang/Thread"),thread_family=thread_owner;
 if(thread_owner&&IS("interrupted","()Z")&&n==0){out->i=dvm__thread_take_interrupt(v);return true;}
 if(!thread_family&&self&&!is_thread(v,self)){
  struct dvm_class *rc=dvm_object_class(v,self),*tc=dvm_find_class_or_external(v,"java/lang/Thread");
  thread_family=rc&&tc&&dvm__class_assignable(v,rc,tc);
 }
 if(thread_family&&self&&!is_thread(v,self)){
  struct plain_thread *t=plain_find(v,self);
  if(!strcmp(m,"<init>")){
   if(!thread_owner)return false;
   if(t){dvm__throw(v,"java/lang/IllegalThreadStateException","Thread already initialized");return true;}
   dvm_ref target=0,name=0;
   if(!strcmp(sig,"(Ljava/lang/Runnable;)V"))target=a[0].l;
   else if(!strcmp(sig,"(Ljava/lang/String;)V")){name=a[0].l;if(!name){dvm__throw(v,"java/lang/NullPointerException","Thread name");return true;}}
   else if(!strcmp(sig,"(Ljava/lang/Runnable;Ljava/lang/String;)V")){target=a[0].l;name=a[1].l;}
   else if(!strcmp(sig,"(Ljava/lang/ThreadGroup;Ljava/lang/Runnable;Ljava/lang/String;J)V")){if(a[3].j){dvm__throw(v,"java/lang/UnsupportedOperationException","custom stack size");return true;}target=a[1].l;name=a[2].l;}
   else if(!strcmp(sig,"()V")){}
   else return false;
   t=calloc(1,sizeof *t);if(!t){dvm__throw(v,"java/lang/OutOfMemoryError","Thread state");return true;}
   dvm_ref current=thread_runtime_current(v);struct plain_thread *parent=current?plain_find(v,current):NULL;
   t->priority=(parent&&parent->priority>=1&&parent->priority<=10)?parent->priority:5;
   t->daemon=parent?parent->daemon:false;
   t->vm=v;t->self=self;t->identity=thread_ref_identity(v,self);t->target=target;if(!strcmp(sig,"(Ljava/lang/ThreadGroup;Ljava/lang/Runnable;Ljava/lang/String;J)V"))t->group=a[0].l;t->name=name?name:dvm_new_string(v,"Thread");t->next=plain_threads;plain_threads=t;return true;
  }
  if(t){
   if(IS("start","()V"))return plain_start(v,t);
   if(IS("getName","()Ljava/lang/String;")){out->l=t->name;return true;}
   if(IS("setName","(Ljava/lang/String;)V")){if(!a[0].l)dvm__throw(v,"java/lang/NullPointerException","Thread name");else t->name=a[0].l;return true;}
   if(IS("setDaemon","(Z)V")){if(t->started)dvm__throw(v,"java/lang/IllegalThreadStateException","Thread already started");else t->daemon=a[0].i!=0;return true;}
   if(IS("isDaemon","()Z")){out->i=t->daemon;return true;}
   if(IS("isAlive","()Z")){out->i=t->alive;return true;}
   if(IS("getPriority","()I")){out->i=(t->priority>=1&&t->priority<=10)?t->priority:5;return true;}
   if(IS("getThreadGroup","()Ljava/lang/ThreadGroup;")){out->l=t->group;return true;}
   if(IS("setPriority","(I)V")){
    if(!a){dvm__throw(v,"java/lang/NullPointerException","Thread priority");return true;}
    int priority=a[0].i;
    if(priority<1||priority>10){dvm__throw(v,"java/lang/IllegalArgumentException","Priority out of range: %d",priority);return true;}
    t->priority=priority;
    /* ThreadFactory runs before the worker starts, so its setPriority(10)
     * is applied by executor_worker_main at thread startup.  A running
     * thread can only be adjusted when it is the caller; other live threads
     * have no exported host pthread_t in this VM. */
    if(t->started&&thread_runtime_current(v)==self){
     int error=0,observed=0;
     if(!thread_priority_apply(java_priority_nice(priority),&error,&observed))
      sdk_trace_fprintf(stderr,"THREAD_PRIORITY_FAIL java_priority=%d nice=%d errno=%d action=continue\n",priority,java_priority_nice(priority),error);
     else sdk_trace_fprintf(stderr,"THREAD_PRIORITY_APPLY java_priority=%d nice=%d\n",priority,observed);
    }
    return true;
   }
  }
 }
 /* Thread.getStackTrace(): only the receiver that is this Java thread has a
  * stack in this VM.  Another live thread's stack would require stopping it
  * and walking its interpreter frames; fabricating one is worse than the
  * empty array Java permits when a thread's stack is unavailable, so a
  * non-current receiver gets a zero-length array.  Builtins are not pushed
  * onto callstack[], hence frame 0 is the synthetic Thread.getStackTrace
  * entry and frame 1 is the innermost real guest method. */
 if(IS("getStackTrace","()[Ljava/lang/StackTraceElement;")){
  if(!thread_stack_receiver_is_thread(v,self))return false;
  dvm_ref current=thread_runtime_current(v);
  out->l=self==current?thread_stack_trace_current(v):thread_stack_trace_empty(v);
  return true;
 }
 if(is_thread(v,self)&&(!strcmp(c,"android/os/HandlerThread")||!strcmp(c,"java/lang/Thread"))){struct dvm_object *o=dvm__obj(v,self);struct worker *w=o->data;
  if(IS("<init>","(Ljava/lang/String;)V")||IS("<init>","(Ljava/lang/String;I)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","thread name");return true;}int priority=0;if(IS("<init>","(Ljava/lang/String;I)V")){priority=a[1].i;if(!thread_priority_valid(priority)){dvm__throw(v,"java/lang/IllegalArgumentException","Priority out of range: %d",priority);return true;}}w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","worker state");return true;}w->vm=v;w->self=self;w->name=a[0].l;w->priority=priority;o->data=w;o->data_destroy=worker_payload_release;return true;}
  if(!w)return false;
  if(IS("start","()V"))return start(v,w);if(IS("getLooper","()Landroid/os/Looper;")){out->l=await_looper(v,w);return true;}
  if(IS("getName","()Ljava/lang/String;")){out->l=w->name;return true;}if(IS("setName","(Ljava/lang/String;)V")){if(!a[0].l)dvm__throw(v,"java/lang/NullPointerException","thread name");else w->name=a[0].l;return true;}if(IS("isAlive","()Z")){pthread_mutex_lock(&queue_lock);out->i=w->alive;pthread_mutex_unlock(&queue_lock);return true;}
  if(IS("quit","()Z")){pthread_mutex_lock(&queue_lock);out->i=w->ready&&w->alive;if(out->i){w->quit=true;pthread_cond_broadcast(&changed);}pthread_mutex_unlock(&queue_lock);return true;}
 }
 if(!strcmp(c,"java/lang/Thread")&&IS("currentThread","()Ljava/lang/Thread;")){out->l=thread_runtime_current(v);if(!out->l)dvm__throw(v,"java/lang/IllegalStateException","host thread not attached to VM");return true;}
 if(!strcmp(c,"android/os/Looper")){
  if(IS("prepare","()V")){
   struct worker *w=worker_for_host(v,pthread_self());
   if(!w||!w->plain)return false;
   if(w->ready){dvm__throw(v,"java/lang/RuntimeException","Only one Looper may be created per thread");return true;}
   dvm_ref looper=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Looper"));
   if(!looper){dvm__throw(v,"java/lang/OutOfMemoryError","Looper.prepare");return true;}
   pthread_mutex_lock(&queue_lock);w->looper=looper;w->looper_identity=thread_ref_identity(v,looper);w->ready=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);engine_thread_trace(v,w,"looper_prepare","ready");return true;
  }
  if(IS("loop","()V")){
   struct worker *w=worker_for_host(v,pthread_self());
   if(!w)return false;
   if(!w->ready){dvm__throw(v,"java/lang/RuntimeException","No Looper prepared");return true;}
   if(w->plain)engine_thread_trace(v,w,"looper_enter","waiting");
   pthread_mutex_lock(&queue_lock);w->active=false;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
   unsigned depth=dvm_gil_unlock_all(v);worker_loop(w);dvm_gil_relock(v,depth);
   pthread_mutex_lock(&queue_lock);w->active=true;pthread_mutex_unlock(&queue_lock);if(w->plain)engine_thread_trace(v,w,"looper_exit",w->loop_failed?"dispatch_failed":w->executed?"quit_after_dispatch":"quit_no_tasks");return true;
  }
  if(IS("myLooper","()Landroid/os/Looper;")){struct worker *w=worker_for_host(v,pthread_self());if(w){out->l=w->looper;return true;}return false;}
  struct worker *w=thread_runtime_looper(v,self)?worker_for_looper(v,self):NULL;
  if(w&&IS("isCurrentThread","()Z")){out->i=pthread_equal(pthread_self(),w->thread);return true;}
  if(w&&IS("getThread","()Ljava/lang/Thread;")){out->l=w->self;return true;}
  if(w&&IS("quit","()V")){pthread_mutex_lock(&queue_lock);w->quit=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);return true;}
 }
 if(!strcmp(c,"android/os/Message")&&IS("recycle","()V")){struct message_lifetime_state *s=message_state(v,self);if(s){if(s->state!=MESSAGE_OWNED)dvm__throw(v,"java/lang/IllegalStateException","Message cannot be recycled while in use");else message_recycle_unchecked(v,self);}return true;}
 if(!strcmp(c,"android/os/Message")&&IS("obtain","()Landroid/os/Message;")){out->l=message_obtain(v);return true;}
 /* AOSP Message.obtain(Handler, Runnable) is obtain(handler, callback, 0):
  * the message carries both the target handler and the Runnable that
  * Looper.dispatchMessage runs.  NeuronClient.schedulePollAction reached
  * this gap in 72bq/72br. */
 if(!strcmp(c,"android/os/Message")&&IS("obtain","(Landroid/os/Handler;Ljava/lang/Runnable;)Landroid/os/Message;")&&n==2){
  {FILE*df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(df){fprintf(df,"MESSAGE_OBTAIN_CALLBACK handler=%u runnable=%u\n",(unsigned)(a&&n==2?a[0].l:0),(unsigned)(a&&n==2?a[1].l:0));fclose(df);}}
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Message.obtain handler");return true;}
  dvm_ref msg=message_obtain(v);
  if(!msg){dvm__throw(v,"java/lang/OutOfMemoryError","Message");return true;}
  union dvm_value target={.l=a[0].l},callback={.l=a[1].l};
  if(!dvm_set_field(v,msg,"target","Landroid/os/Handler;",target)||!dvm_set_field(v,msg,"callback","Ljava/lang/Runnable;",callback)){dvm__throw(v,"java/lang/IllegalArgumentException","Message fields unavailable");return true;}
  out->l=msg;return true;
 }
 /* AOSP convenience obtain overloads used by U0.a(S0) on the engine Looper:
  * obtain(Handler), obtain(Handler,int) and obtain(Handler,int,Object) only
  * fill the target/what/obj fields; the message is enqueued later by
  * sendToTarget() or Handler.sendMessage(). */
 if(!strcmp(c,"android/os/Message")&&IS("obtain","(Landroid/os/Handler;)Landroid/os/Message;")&&n==1){
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Message.obtain handler");return true;}
  dvm_ref msg=message_obtain(v);
  if(!msg){dvm__throw(v,"java/lang/OutOfMemoryError","Message");return true;}
  union dvm_value target={.l=a[0].l};
  if(!dvm_set_field(v,msg,"target","Landroid/os/Handler;",target)){dvm__throw(v,"java/lang/IllegalArgumentException","Message fields unavailable");return true;}
  out->l=msg;return true;
 }
 if(!strcmp(c,"android/os/Message")&&IS("obtain","(Landroid/os/Handler;I)Landroid/os/Message;")&&n==2){
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Message.obtain handler");return true;}
  dvm_ref msg=message_obtain(v);
  if(!msg){dvm__throw(v,"java/lang/OutOfMemoryError","Message");return true;}
  union dvm_value target={.l=a[0].l},what={.i=a[1].i};
  if(!dvm_set_field(v,msg,"target","Landroid/os/Handler;",target)||!dvm_set_field(v,msg,"what","I",what)){dvm__throw(v,"java/lang/IllegalArgumentException","Message fields unavailable");return true;}
  out->l=msg;return true;
 }
 if(!strcmp(c,"android/os/Message")&&IS("obtain","(Landroid/os/Handler;ILjava/lang/Object;)Landroid/os/Message;")&&n==3){
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Message.obtain handler");return true;}
  dvm_ref msg=message_obtain(v);
  if(!msg){dvm__throw(v,"java/lang/OutOfMemoryError","Message");return true;}
  union dvm_value target={.l=a[0].l},what={.i=a[1].i},obj={.l=a[2].l};
  if(!dvm_set_field(v,msg,"target","Landroid/os/Handler;",target)||!dvm_set_field(v,msg,"what","I",what)||!dvm_set_field(v,msg,"obj","Ljava/lang/Object;",obj)){dvm__throw(v,"java/lang/IllegalArgumentException","Message fields unavailable");return true;}
  out->l=msg;return true;
 }
 /* AOSP Message.sendToTarget() is target.sendMessage(this).  UnityPlayer.
  * updateDisplayInternal uses obtain(handler, runnable) followed by
  * sendToTarget() to run L0.run on the engine Looper; dispatchMessage then
  * executes the callback (thread_runtime.c dispatch_message). */
 if(!strcmp(c,"android/os/Message")&&IS("sendToTarget","()V")){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Message receiver");return true;}
  union dvm_value target={0},posted={0};
  if(!dvm_get_field(v,self,"target","Landroid/os/Handler;",&target)){dvm__throw(v,"java/lang/IllegalArgumentException","Message target field unavailable");return true;}
  if(!target.l){dvm__throw(v,"java/lang/NullPointerException","Message target");return true;}
  union dvm_value message={.l=self};
  if(!thread_runtime_call(v,"android/os/Handler","sendMessage","(Landroid/os/Message;)Z",target.l,&message,1,&posted)){
   sdk_trace_missing_api("android/os/Handler","sendMessage","(Landroid/os/Message;)Z");dvm__throw(v,"java/lang/NoSuchMethodError","Handler.sendMessage");return true;
  }
  if(!dvm_exception(v)&&!posted.i){dvm__throw(v,"java/lang/IllegalStateException","Handler rejected Message");return true;}
  return true;
 }
 if(!strcmp(c,"android/os/Handler")){
  if(IS("obtainMessage","()Landroid/os/Message;")&&n==0)return handler_obtain(v,self,NULL,0,out);
  if(IS("obtainMessage","(I)Landroid/os/Message;")&&n==1)return handler_obtain(v,self,a,n,out);
  if(IS("obtainMessage","(ILjava/lang/Object;)Landroid/os/Message;")&&n==2)return handler_obtain(v,self,a,n,out);
  if(IS("obtainMessage","(III)Landroid/os/Message;")&&n==3)return handler_obtain(v,self,a,n,out);
  if(IS("obtainMessage","(IIILjava/lang/Object;)Landroid/os/Message;")&&n==4)return handler_obtain(v,self,a,n,out);
  if(IS("post","(Ljava/lang/Runnable;)Z"))return enqueue(v,self,a[0].l,0,0,out);
  if(IS("postAtFrontOfQueue","(Ljava/lang/Runnable;)Z"))return enqueue(v,self,a[0].l,0,INT64_MIN,out);
  if(IS("postDelayed","(Ljava/lang/Runnable;J)Z"))return enqueue(v,self,a[0].l,0,a[1].j,out);
  if(IS("sendMessage","(Landroid/os/Message;)Z"))return enqueue(v,self,0,a[0].l,0,out);
  /* AOSP sendMessageAtTime(msg, uptimeMillis) enqueues with msg.when set to
   * that absolute uptime; SystemClock.uptimeMillis() and this queue share
   * CLOCK_MONOTONIC, so the delay is the positive difference (a past time
   * means "now", exactly like MessageQueue.enqueueMessage). */
  if(IS("sendMessageAtTime","(Landroid/os/Message;J)Z")&&n==2){
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","sendMessageAtTime message");return true;}
   int64_t when=a[1].j,now=(int64_t)now_ms(),delay=when>now?when-now:0;
   return enqueue(v,self,0,a[0].l,delay,out);
  }
  if(IS("sendEmptyMessage","(I)Z"))return send_empty(v,self,a[0].i,0,out);
  if(IS("sendEmptyMessageDelayed","(IJ)Z"))return send_empty(v,self,a[0].i,a[1].j,out);
  if(IS("hasMessages","(I)Z"))return has_messages(v,self,a[0].i,out);
  if(IS("removeMessages","(I)V"))return remove_messages(v,self,a[0].i);
 }
 return false;
}
static bool thread_priority_selftest(struct dvm *v){
 bool ok=true,process_positive=false,process_invalid=true,handler_ctor_invalid=false,handler_applied=false;int handler_nice=-1,error=0;
 union dvm_value out={0};
#define PRIORITY_CHECK(expr) do{if(!(expr)){ok=false;sdk_trace_fprintf(stderr,"THREAD_PRIORITY_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 /* Exercise the real Process.setThreadPriority syscall without changing the
  * host thread: setting the nice value it already has still calls setpriority
  * and is permitted to an unprivileged thread. */
 int current=0;
 if(thread_priority_get(&current,&error)){
  union dvm_value arg={.i=current};
  memset(&out,0,sizeof out);
  bool call=thread_runtime_call(v,"android/os/Process","setThreadPriority","(I)V",0,&arg,1,&out);
  process_positive=call&&!dvm_exception(v);
  if(!process_positive&&dvm_exception(v))dvm_clear_exception(v);
 }
 PRIORITY_CHECK(process_positive);
 union dvm_value bad={.i=THREAD_PRIORITY_MAX+1};
 memset(&out,0,sizeof out);
 bool high=thread_runtime_call(v,"android/os/Process","setThreadPriority","(I)V",0,&bad,1,&out);
 high=high&&rrw_test_threw(v,"java/lang/IllegalArgumentException");
 if(!high&&dvm_exception(v))dvm_clear_exception(v);
 PRIORITY_CHECK(high);process_invalid=process_invalid&&high;
 bad.i=THREAD_PRIORITY_MIN-1;
 memset(&out,0,sizeof out);
 bool low=thread_runtime_call(v,"android/os/Process","setThreadPriority","(I)V",0,&bad,1,&out);
 low=low&&rrw_test_threw(v,"java/lang/IllegalArgumentException");
 if(!low&&dvm_exception(v))dvm_clear_exception(v);
 PRIORITY_CHECK(low);process_invalid=process_invalid&&low;
 dvm_ref name=dvm_new_string(v,"priority-selftest");
 struct dvm_class *hc=dvm_find_class_or_external(v,"android/os/HandlerThread");
 dvm_ref thread=hc?dvm_new_object(v,hc):0;
 PRIORITY_CHECK(name!=0&&thread!=0);
 if(name&&thread){
  union dvm_value args[2]={{.l=name},{.i=1}};
  memset(&out,0,sizeof out);
  bool constructed=thread_runtime_call(v,"android/os/HandlerThread","<init>","(Ljava/lang/String;I)V",thread,args,2,&out)&&!dvm_exception(v);
  if(!constructed&&dvm_exception(v))dvm_clear_exception(v);
  PRIORITY_CHECK(constructed);
  struct dvm_object *o=constructed?dvm__obj(v,thread):NULL;struct worker *w=o?o->data:NULL;
  PRIORITY_CHECK(w&&w->priority==1);
  if(w){
   memset(&out,0,sizeof out);
   bool started=thread_runtime_call(v,"android/os/HandlerThread","start","()V",thread,NULL,0,&out)&&!dvm_exception(v);
   if(!started&&dvm_exception(v))dvm_clear_exception(v);
   PRIORITY_CHECK(started);
   if(started){
    union dvm_value looper={0};
    bool got=thread_runtime_call(v,"android/os/HandlerThread","getLooper","()Landroid/os/Looper;",thread,NULL,0,&looper)&&!dvm_exception(v)&&looper.l;
    if(!got&&dvm_exception(v))dvm_clear_exception(v);
    PRIORITY_CHECK(got);
    pthread_mutex_lock(&queue_lock);bool attempted=w->priority_attempted;handler_applied=w->priority_applied;int priority_error=w->priority_errno;handler_nice=w->priority_nice;pthread_mutex_unlock(&queue_lock);
    PRIORITY_CHECK(attempted);
    PRIORITY_CHECK(handler_applied&&priority_error==0&&handler_nice==1);
    pthread_mutex_lock(&queue_lock);w->quit=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
    thread_runtime_join_host(v,w->thread);w->joined=true;
    pthread_mutex_lock(&queue_lock);bool alive=w->alive;pthread_mutex_unlock(&queue_lock);
    PRIORITY_CHECK(!alive);
   }
  }
 }
 dvm_ref badname=dvm_new_string(v,"priority-selftest-bad");
 dvm_ref badthread=hc?dvm_new_object(v,hc):0;
 PRIORITY_CHECK(badname!=0&&badthread!=0);
 if(badname&&badthread){
  union dvm_value args[2]={{.l=badname},{.i=THREAD_PRIORITY_MAX+1}};
  memset(&out,0,sizeof out);
  bool call=thread_runtime_call(v,"android/os/HandlerThread","<init>","(Ljava/lang/String;I)V",badthread,args,2,&out);
  handler_ctor_invalid=call&&rrw_test_threw(v,"java/lang/IllegalArgumentException");
  if(!handler_ctor_invalid&&dvm_exception(v))dvm_clear_exception(v);
  PRIORITY_CHECK(handler_ctor_invalid);
 }
 if(dvm_exception(v)){dvm_clear_exception(v);ok=false;}
#undef PRIORITY_CHECK
 sdk_trace_fprintf(stderr,"THREAD_PRIORITY_SELFTEST %s handler_priority=1 handler_nice=%d handler_applied=%d process_positive=%d process_invalid=%d handler_ctor_invalid=%d\n",ok?"PASS":"FAIL",handler_nice,handler_applied?1:0,process_positive?1:0,process_invalid?1:0,handler_ctor_invalid?1:0);
 return ok;
}
/* Real VM calls, local fixtures only. Never invokes a game/SDK callback. */
bool thread_runtime_memory_selftest(struct dvm *v){
 dvm_ref saved=v->exception;uint64_t steps=v->call_steps;bool parked=v->parked;v->exception=0;v->call_steps=0;v->parked=false;
 bool ok=true;unsigned checks=0;
#define MP_CHECK(x) do{checks++;if(!(x)){ok=false;sdk_trace_fprintf(stderr,"MESSAGE_POOL_SELFTEST_FAIL line=%d\n",__LINE__);}}while(0)
 struct dvm_class *c=dvm_find_class_or_external(v,"android/os/Message");
 struct dvm_method *obtain=dvm_find_method(v,c,"obtain","()Landroid/os/Message;"),*recycle=dvm_find_method(v,c,"recycle","()V");union dvm_value q={0};
 MP_CHECK(obtain&&recycle);
 dvm_ref payload=dvm_new_string(v,"message-pool-local-fixture");
 if(obtain&&recycle){
  MP_CHECK(dvm_call(v,obtain,0,NULL,0,&q)&&q.l&&!v->exception);dvm_ref ref=q.l;
  union dvm_value arg={.l=payload};MP_CHECK(dvm_set_field(v,ref,"obj","Ljava/lang/Object;",arg));
  MP_CHECK(dvm_call(v,recycle,ref,NULL,0,&q)&&!v->exception);
  MP_CHECK(dvm_get_field(v,ref,"obj","Ljava/lang/Object;",&q)&&q.l==0);
  unsigned before_count=message_pool.count;
  bool returned=dvm_call(v,recycle,ref,NULL,0,&q);struct dvm_object *e=dvm__obj(v,v->exception);
  MP_CHECK(!returned&&e&&e->cls&&!strcmp(e->cls->name,"java/lang/IllegalStateException")&&message_pool.count==before_count);v->exception=0;
  MP_CHECK(dvm_call(v,obtain,0,NULL,0,&q)&&q.l==ref&&!v->exception);
  struct message_lifetime_state *s=message_state(v,ref);s->state=MESSAGE_IN_USE;dvm_pin(v,ref);
  returned=dvm_call(v,recycle,ref,NULL,0,&q);e=dvm__obj(v,v->exception);
  MP_CHECK(!returned&&e&&e->cls&&!strcmp(e->cls->name,"java/lang/IllegalStateException"));v->exception=0;
  dvm__throw(v,"java/lang/RuntimeException","message cleanup preserves original fixture exception");dvm_ref original=v->exception;
  message_recycle_unchecked(v,ref);MP_CHECK(v->exception==original);v->exception=0;
  uint32_t high=v->heap_size;uint64_t created=message_pool.created;
  bool repeated=true;for(unsigned i=0;i<2000;i++){if(!dvm_call(v,obtain,0,NULL,0,&q)||q.l!=ref||v->exception){repeated=false;break;}if(!dvm_call(v,recycle,ref,NULL,0,&q)||v->exception){repeated=false;break;}}
  MP_CHECK(repeated&&v->heap_size==high&&message_pool.created==created);
  dvm_ref refs[51]={0};bool bounded=true;for(unsigned i=0;i<51;i++){if(!dvm_call(v,obtain,0,NULL,0,&q)||!q.l){bounded=false;break;}refs[i]=q.l;}
  for(unsigned i=0;i<51;i++)if(refs[i]&&!dvm_call(v,recycle,refs[i],NULL,0,&q))bounded=false;
  MP_CHECK(bounded&&message_pool.count==50);
 }
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){fprintf(f,"ENGINE_LOG_MESSAGE_POOL_SELFTEST ok=%d checks=%u dvm_call=1 repeated_cycles=2000 no_new_objects=1 double_recycle_rejected=1 in_use_recycle_rejected=1 exception_preserved=1 capacity=50\n",ok,checks);fclose(f);}
 v->exception=saved;v->call_steps=steps;v->parked=parked;
#undef MP_CHECK
 return ok;
}
/* Handler queue semantics on an owned worker Looper. A 60s delay keeps the
 * probe pending while assertions run; the owner Handler and exact what are
 * matched, removeMessages drops it, and an absent what is a no-op. */
static bool thread_handler_queue_selftest(struct dvm *v){
 bool ok=true;union dvm_value out={0};
 extern bool android_basics_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
#define QUEUE_CHECK(expr) do{if(!(expr)){ok=false;sdk_trace_fprintf(stderr,"THREAD_HANDLER_QUEUE_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 dvm_ref name=dvm_new_string(v,"handler-queue-selftest");
 struct dvm_class *hc=dvm_find_class_or_external(v,"android/os/HandlerThread");
 dvm_ref thread=hc?dvm_new_object(v,hc):0;
 QUEUE_CHECK(name!=0&&thread!=0);
 struct worker *w=NULL;bool started_ok=false;
 if(name&&thread){
  union dvm_value args[1]={{.l=name}};
  memset(&out,0,sizeof out);
  bool constructed=thread_runtime_call(v,"android/os/HandlerThread","<init>","(Ljava/lang/String;)V",thread,args,1,&out)&&!dvm_exception(v);
  if(!constructed&&dvm_exception(v))dvm_clear_exception(v);
  QUEUE_CHECK(constructed);
  struct dvm_object *o=constructed?dvm__obj(v,thread):NULL;w=o?o->data:NULL;
  if(w){
   memset(&out,0,sizeof out);
   bool started=thread_runtime_call(v,"android/os/HandlerThread","start","()V",thread,NULL,0,&out)&&!dvm_exception(v);
   if(!started&&dvm_exception(v))dvm_clear_exception(v);
   QUEUE_CHECK(started);started_ok=started;
  }
 }
 dvm_ref looper=0;
 if(w){
  memset(&out,0,sizeof out);
  bool got=thread_runtime_call(v,"android/os/HandlerThread","getLooper","()Landroid/os/Looper;",thread,NULL,0,&out)&&!dvm_exception(v)&&out.l;
  if(!got&&dvm_exception(v))dvm_clear_exception(v);
  QUEUE_CHECK(got);looper=got?out.l:0;
 }
 struct dvm_class *handler_class=dvm_find_class_or_external(v,"android/os/Handler");
 dvm_ref handler=handler_class?dvm_new_object(v,handler_class):0;
 QUEUE_CHECK(handler!=0);
 if(handler&&looper){
  union dvm_value l={.l=looper};
  memset(&out,0,sizeof out);
  bool constructed=android_basics_call(v,"android/os/Handler","<init>","(Landroid/os/Looper;)V",handler,&l,1,&out)&&!dvm_exception(v);
  if(!constructed&&dvm_exception(v))dvm_clear_exception(v);
  QUEUE_CHECK(constructed);
 }
 if(handler&&looper){
  const int what=2814515;union dvm_value what_arg={.i=what};
  union dvm_value send_args[2]={{.i=what},{.j=60000}};
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","sendEmptyMessageDelayed","(IJ)Z",handler,send_args,2,&out)&&!dvm_exception(v)&&out.i==1);
  { /* Real pending queue, not a manually invented in-use state. */
   dvm_ref queued=0;pthread_mutex_lock(&queue_lock);for(struct task *t=w->head;t;t=t->next)if(t->handler==handler&&task_what(v,t,what)){queued=t->message;break;}pthread_mutex_unlock(&queue_lock);
   struct dvm_method *recycle=dvm_find_method(v,dvm_find_class_or_external(v,"android/os/Message"),"recycle","()V");
   bool rejected=queued&&recycle&&!dvm_call(v,recycle,queued,NULL,0,&out);struct dvm_object *error=dvm__obj(v,v->exception);
   QUEUE_CHECK(rejected&&error&&error->cls&&!strcmp(error->cls->name,"java/lang/IllegalStateException"));dvm_clear_exception(v);
   union dvm_value arg={.l=queued};thread_runtime_call(v,"android/os/Handler","sendMessage","(Landroid/os/Message;)Z",handler,&arg,1,&out);error=dvm__obj(v,v->exception);
   QUEUE_CHECK(error&&error->cls&&!strcmp(error->cls->name,"java/lang/IllegalStateException"));dvm_clear_exception(v);
  }
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","hasMessages","(I)Z",handler,&what_arg,1,&out)&&!dvm_exception(v)&&out.i==1);
  what_arg.i=what+1;
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","hasMessages","(I)Z",handler,&what_arg,1,&out)&&!dvm_exception(v)&&out.i==0);
  what_arg.i=what;
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","removeMessages","(I)V",handler,&what_arg,1,&out)&&!dvm_exception(v));
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","hasMessages","(I)Z",handler,&what_arg,1,&out)&&!dvm_exception(v)&&out.i==0);
  what_arg.i=what+2;
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","removeMessages","(I)V",handler,&what_arg,1,&out)&&!dvm_exception(v));
  union dvm_value other_args[2]={{.i=what+7},{.j=60000}};
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","sendEmptyMessageDelayed","(IJ)Z",handler,other_args,2,&out)&&!dvm_exception(v)&&out.i==1);
  what_arg.i=what;
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","hasMessages","(I)Z",handler,&what_arg,1,&out)&&!dvm_exception(v)&&out.i==0);
  what_arg.i=what+7;
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","hasMessages","(I)Z",handler,&what_arg,1,&out)&&!dvm_exception(v)&&out.i==1);
  memset(&out,0,sizeof out);
  QUEUE_CHECK(thread_runtime_call(v,"android/os/Handler","removeMessages","(I)V",handler,&what_arg,1,&out)&&!dvm_exception(v));
 }
 /* obtain(Handler,int,Object) fills the AOSP target/what/obj fields; the
  * engine Looper path (U0.a) depends on all three. */
 {
  union dvm_value oargs[3]={{.l=handler},{.i=2814516},{.l=name}};
  dvm_ref msg=0;
  memset(&out,0,sizeof out);
  bool call=thread_runtime_call(v,"android/os/Message","obtain","(Landroid/os/Handler;ILjava/lang/Object;)Landroid/os/Message;",0,oargs,3,&out)&&!dvm_exception(v);
  QUEUE_CHECK(call&&out.l!=0);msg=call?out.l:0;
  union dvm_value tv={0},wv={0},ov={0};
  QUEUE_CHECK(msg&&dvm_get_field(v,msg,"target","Landroid/os/Handler;",&tv)&&tv.l==handler);
  QUEUE_CHECK(msg&&dvm_get_field(v,msg,"what","I",&wv)&&wv.i==2814516);
  QUEUE_CHECK(msg&&dvm_get_field(v,msg,"obj","Ljava/lang/Object;",&ov)&&ov.l==name);
 }
 /* Message.sendToTarget(): a message without a target must fail closed with
  * NPE instead of silently reporting a successful post.  The real delivery
  * (obtain(handler, runnable) + sendToTarget -> Handler.sendMessage ->
  * dispatchMessage callback) is exercised by UnityPlayer.updateDisplayInternal
  * on device. */
 {
  dvm_ref msg=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Message"));
  QUEUE_CHECK(msg!=0);
  memset(&out,0,sizeof out);
  bool call=thread_runtime_call(v,"android/os/Message","sendToTarget","()V",msg,NULL,0,&out);
  QUEUE_CHECK(call&&dvm_exception(v));
  if(dvm_exception(v))dvm_clear_exception(v);
 }
 if(w&&started_ok){
  pthread_mutex_lock(&queue_lock);w->quit=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
  thread_runtime_join_host(v,w->thread);w->joined=true;
  pthread_mutex_lock(&queue_lock);bool alive=w->alive;pthread_mutex_unlock(&queue_lock);
  QUEUE_CHECK(!alive);
 }
 if(dvm_exception(v)){dvm_clear_exception(v);ok=false;}
#undef QUEUE_CHECK
 sdk_trace_fprintf(stderr,"THREAD_HANDLER_QUEUE_SELFTEST %s has_messages=1 remove_messages=1 what_match=1\n",ok?"PASS":"FAIL");
 return ok;
}
static bool stack_trace_selftest_call(struct dvm *v,dvm_ref element,const char *name,const char *sig,union dvm_value *out){
 struct dvm_method *m=dvm_lookup(v,"java/lang/StackTraceElement",name,sig);
 return m&&dvm_call(v,m,element,NULL,0,out)&&!dvm_exception(v);
}
bool thread_runtime_stack_selftest(struct dvm *v){
 bool ok=true;union dvm_value out={0};
#define STACK_CHECK(expr) do{if(!(expr)){ok=false;sdk_trace_fprintf(stderr,"THREAD_STACK_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 dvm_ref current=thread_runtime_current(v);
 STACK_CHECK(current!=0);
 if(!current){sdk_trace_fprintf(stderr,"THREAD_STACK_SELFTEST FAIL get_stack_trace=1 stack_trace_element=1\n");return false;}
 memset(&out,0,sizeof out);
 STACK_CHECK(thread_runtime_call(v,"java/lang/Thread","getStackTrace","()[Ljava/lang/StackTraceElement;",current,NULL,0,&out));
 STACK_CHECK(!dvm_exception(v));
 dvm_ref array=out.l;
 struct dvm_object *ao=array?dvm__obj(v,array):NULL;
 STACK_CHECK(ao&&ao->kind==DVM_OBJ_ARRAY&&ao->elem_kind=='L'&&ao->length>=1);
 if(ao&&ao->kind==DVM_OBJ_ARRAY&&ao->length>=1){
  dvm_ref *items=dvm_array_data(v,array);
  STACK_CHECK(items!=NULL);
  dvm_ref first=items?items[0]:0;
  STACK_CHECK(first!=0);
  if(first){
   union dvm_value r={0};
   STACK_CHECK(stack_trace_selftest_call(v,first,"getClassName","()Ljava/lang/String;",&r));
   const char *s=r.l?dvm_string_utf8(v,r.l):NULL;
   STACK_CHECK(s&&!strcmp(s,"java.lang.Thread"));
   memset(&r,0,sizeof r);
   STACK_CHECK(stack_trace_selftest_call(v,first,"getMethodName","()Ljava/lang/String;",&r));
   s=r.l?dvm_string_utf8(v,r.l):NULL;
   STACK_CHECK(s&&!strcmp(s,"getStackTrace"));
  }
  /* The first real frame, when the selftest runs from guest bytecode: the VM
   * supplied a class name and it is the dotted form, not a JNI descriptor. */
  if(ao->length>1){
   dvm_ref second=items?items[1]:0;
   STACK_CHECK(second!=0);
   if(second){
    union dvm_value r={0};
    STACK_CHECK(stack_trace_selftest_call(v,second,"getClassName","()Ljava/lang/String;",&r));
    const char *s=r.l?dvm_string_utf8(v,r.l):NULL;
    STACK_CHECK(s&&s[0]&&!strchr(s,'/'));
   }
  }
 }
 struct dvm_class *stec=dvm__class_by_desc(v,"Ljava/lang/StackTraceElement;");
 STACK_CHECK(stec&&!stec->external);
 dvm_ref element=stec?dvm_new_object(v,stec):0;
 STACK_CHECK(element!=0);
 struct dvm_method *ctor=dvm_lookup(v,"java/lang/StackTraceElement","<init>","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
 STACK_CHECK(ctor!=0);
 if(element&&ctor){
  dvm_ref cn=dvm_new_string(v,"com.example.StackProbe");
  dvm_ref mn=dvm_new_string(v,"run");
  STACK_CHECK(cn!=0&&mn!=0);
  union dvm_value args[4]={{.l=cn},{.l=mn},{.l=0},{.i=-1}};
  memset(&out,0,sizeof out);
  STACK_CHECK(dvm_call(v,ctor,element,args,4,&out));
  STACK_CHECK(!dvm_exception(v));
  union dvm_value r={0};
  STACK_CHECK(stack_trace_selftest_call(v,element,"getClassName","()Ljava/lang/String;",&r));
  const char *s=r.l?dvm_string_utf8(v,r.l):NULL;
  STACK_CHECK(s&&!strcmp(s,"com.example.StackProbe"));
  memset(&r,0,sizeof r);
  STACK_CHECK(stack_trace_selftest_call(v,element,"getMethodName","()Ljava/lang/String;",&r));
  s=r.l?dvm_string_utf8(v,r.l):NULL;
  STACK_CHECK(s&&!strcmp(s,"run"));
  memset(&r,0,sizeof r);
  STACK_CHECK(stack_trace_selftest_call(v,element,"getFileName","()Ljava/lang/String;",&r));
  STACK_CHECK(r.l==0);
  memset(&r,0,sizeof r);
  STACK_CHECK(stack_trace_selftest_call(v,element,"getLineNumber","()I",&r));
  STACK_CHECK(r.i==-1);
 }
 struct dvm_class *object_class=dvm_find_class_or_external(v,"java/lang/Object");
 dvm_ref object=object_class?dvm_new_object(v,object_class):0;
 STACK_CHECK(object!=0);
 if(object){
  memset(&out,0,sizeof out);
  STACK_CHECK(!thread_runtime_call(v,"java/lang/Thread","getStackTrace","()[Ljava/lang/StackTraceElement;",object,NULL,0,&out));
 }
 dvm_ref string=dvm_new_string(v,"not-a-thread");
 STACK_CHECK(string!=0);
 if(string){
  memset(&out,0,sizeof out);
  STACK_CHECK(!thread_runtime_call(v,"java/lang/Thread","getStackTrace","()[Ljava/lang/StackTraceElement;",string,NULL,0,&out));
 }
 STACK_CHECK(!dvm_exception(v));
 memset(&out,0,sizeof out);
 STACK_CHECK(!thread_runtime_call(v,"java/lang/Thread","getStackTrace","(I)[Ljava/lang/StackTraceElement;",current,NULL,0,&out));
 STACK_CHECK(!dvm_exception(v));
 bool handler_queue_ok=thread_handler_queue_selftest(v);
 if(!handler_queue_ok)ok=false;
 bool priority_ok=thread_priority_selftest(v);
 if(!priority_ok)ok=false;
#undef STACK_CHECK
 sdk_trace_fprintf(stderr,"THREAD_STACK_SELFTEST %s get_stack_trace=1 stack_trace_element=1 handler_queue=1\n",ok?"PASS":"FAIL");
 return ok;
}
