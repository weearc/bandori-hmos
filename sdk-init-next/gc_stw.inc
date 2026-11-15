#ifdef fprintf
#pragma push_macro("fprintf")
#undef fprintf
#define GC_STW_RESTORE_FPRINTF
#endif
/* DVM mutator rendezvous. Native-only engine/audio/network threads are not
 * DVM mutators. Their JNI access must acquire this GIL; outstanding raw payload
 * loans remain strong/pinned roots. The candidate portable ARM lock is a stub,
 * so it is deliberately NOT used as a safepoint proof. */
struct gc_stw_thread {struct dvm *vm;uint64_t serial;pthread_t thread;long tid;
 unsigned state;uint64_t last_ns,ack;char where[384];struct gc_stw_thread *next;};
enum {GC_STW_WAITING=1,GC_STW_ACTIVE=2,GC_STW_PARKED=3};
static pthread_mutex_t gc_stw_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc_stw_changed=PTHREAD_COND_INITIALIZER;
static pthread_key_t gc_stw_key;static pthread_once_t gc_stw_once=PTHREAD_ONCE_INIT;
static _Thread_local struct gc_stw_thread *gc_stw_local;
static struct gc_stw_thread *gc_stw_threads;
static int gc_stw_key_error;static bool gc_stw_registry_failed,gc_stw_requested;
static uint64_t gc_stw_epoch;static pthread_t gc_stw_owner;
static void gc_stw_thread_end(void *data){
 struct gc_stw_thread *r=data;if(!r)return;pthread_mutex_lock(&gc_stw_lock);
 if(r->state==GC_STW_ACTIVE)gc_stw_registry_failed=true;
 struct gc_stw_thread **q=&gc_stw_threads;while(*q&&*q!=r)q=&(*q)->next;if(*q)*q=r->next;
 pthread_cond_broadcast(&gc_stw_changed);pthread_mutex_unlock(&gc_stw_lock);gc_stw_local=NULL;vm_gc_scratch_free(r);
}
static void gc_stw_key_init(void){gc_stw_key_error=pthread_key_create(&gc_stw_key,gc_stw_thread_end);}
static struct gc_stw_thread *gc_stw_register(struct dvm *vm){
 if(gc_stw_local)return gc_stw_local;
 pthread_once(&gc_stw_once,gc_stw_key_init);
 struct gc_stw_thread *r=gc_stw_key_error?NULL:vm_gc_scratch_alloc(sizeof *r);
 if(r){memset(r,0,sizeof *r);r->thread=pthread_self();r->tid=(long)syscall(SYS_gettid);r->vm=vm;r->serial=vm?vm->serial:0;r->state=GC_STW_WAITING;r->last_ns=gc_fast_ns();
  if(pthread_setspecific(gc_stw_key,r)){vm_gc_scratch_free(r);r=NULL;}}
 pthread_mutex_lock(&gc_stw_lock);if(r){r->next=gc_stw_threads;gc_stw_threads=r;}else gc_stw_registry_failed=true;pthread_mutex_unlock(&gc_stw_lock);
 gc_stw_local=r;return r;
}
static void gc_stw_thread_waiting(struct dvm *vm){
 struct gc_stw_thread *r=gc_stw_register(vm);if(!r)return;
 pthread_mutex_lock(&gc_stw_lock);r->vm=vm;r->serial=vm?vm->serial:0;r->state=GC_STW_WAITING;r->last_ns=gc_fast_ns();pthread_mutex_unlock(&gc_stw_lock);
}
static void gc_stw_thread_acquired(struct dvm *vm){
 struct gc_stw_thread *r=gc_stw_register(vm);if(!r)return;
 pthread_mutex_lock(&gc_stw_lock);
 /* This must be impossible while the collector keeps the GIL. Do not silently
  * permit a new entrant should a future edit accidentally release it. */
 while(gc_stw_requested&&!pthread_equal(pthread_self(),gc_stw_owner))pthread_cond_wait(&gc_stw_changed,&gc_stw_lock);
 r->vm=vm;r->serial=vm?vm->serial:0;r->state=GC_STW_ACTIVE;r->last_ns=gc_fast_ns();pthread_mutex_unlock(&gc_stw_lock);
}
static void gc_stw_thread_parked(struct dvm *vm){
 struct gc_stw_thread *r=gc_stw_register(vm);if(!r)return;
 pthread_mutex_lock(&gc_stw_lock);
 size_t at=0;r->where[0]=0;
 for(int i=vm->ncallstack-1;i>=0&&at+2<sizeof r->where;i--){struct dvm_method *m=vm->callstack[i];if(!m)continue;
  int n=snprintf(r->where+at,sizeof r->where-at,"%s%s.%s",at?" <- ":"",m->cls?m->cls->name:"?",m->name?m->name:"?");
  if(n<0)break;if((size_t)n>=sizeof r->where-at){at=sizeof r->where-1;break;}at+=(size_t)n;
 }
 if(!at)snprintf(r->where,sizeof r->where,"outside_vm/published_tstate pc=%u",vm->cur_pc);
 r->state=GC_STW_PARKED;r->last_ns=gc_fast_ns();pthread_cond_broadcast(&gc_stw_changed);pthread_mutex_unlock(&gc_stw_lock);
}
static bool gc_stw_begin(struct dvm *vm,struct gc_stw_receipt *out){
 memset(out,0,sizeof *out);out->coordinator=(long)syscall(SYS_gettid);
 if(!dvm_gil_held(vm)||!pthread_equal(g_gil.owner,pthread_self())){out->failure="coordinator_not_gil_owner";return false;}
 gc_stw_thread_acquired(vm);
 pthread_mutex_lock(&gc_stw_lock);gc_stw_requested=true;gc_stw_owner=pthread_self();out->epoch=++gc_stw_epoch;
 uint64_t deadline=gc_fast_ns()+100000000u;
 for(;;){
  out->total=out->arrived=0;
  for(struct gc_stw_thread *r=gc_stw_threads;r;r=r->next){if(r->vm!=vm||r->serial!=vm->serial)continue;
   if(out->total==GC_STW_MAX_THREADS){out->failure="thread_registry_capacity";break;}
   bool ack=r->state!=GC_STW_ACTIVE||pthread_equal(r->thread,pthread_self());
   if(ack){r->ack=out->epoch;out->arrived++;}
   struct gc_stw_row *row=&out->rows[out->total++];*row=(struct gc_stw_row){.tid=r->tid,.state=r->state,.acknowledged=ack,.last_ns=r->last_ns};memcpy(row->where,r->where,sizeof row->where);
  }
  if(gc_stw_registry_failed){out->failure="thread_registry_incomplete";break;}
  if(out->failure)break;
  if(out->total&&out->arrived==out->total){out->ok=true;break;}
  if(gc_fast_ns()>=deadline){out->failure="safepoint_timeout";break;}
  struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=1000000;if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
  pthread_cond_timedwait(&gc_stw_changed,&gc_stw_lock,&until);
 }
 pthread_mutex_unlock(&gc_stw_lock);return out->ok;
}
static void gc_stw_end(void){pthread_mutex_lock(&gc_stw_lock);gc_stw_requested=false;pthread_cond_broadcast(&gc_stw_changed);pthread_mutex_unlock(&gc_stw_lock);}
static void gc_stw_write(const struct gc_stw_receipt *r,uint64_t cycle){
 static pid_t pid;bool first=pid!=getpid();pid=getpid();FILE *f=fopen(GC_FAST_DIR "gc-safepoints.csv",first?"w":"a");if(!f)return;
 if(first)fputs("pid,mono_ms,epoch,coordinator_tid,total,arrived,ok,reason,thread_tid,state,acknowledged,last_ns,last_guest_stack\n",f);
 for(unsigned i=0;i<r->total;i++){const struct gc_stw_row *q=&r->rows[i];fprintf(f,"%d,%llu,%llu,%ld,%u,%u,%d,%s,%ld,%u,%d,%llu,%s\n",getpid(),(unsigned long long)cycle,(unsigned long long)r->epoch,r->coordinator,r->total,r->arrived,r->ok,r->failure?r->failure:"none",q->tid,q->state,q->acknowledged,(unsigned long long)q->last_ns,q->where);
  if(!r->ok&&!q->acknowledged){char path[96],text[1024];snprintf(path,sizeof path,"/proc/self/task/%ld/stack",q->tid);FILE *stack=fopen(path,"r");size_t n=stack?fread(text,1,sizeof text-1,stack):0;if(stack)fclose(stack);text[n]=0;fprintf(f,"# timeout tid=%ld host_stack=%s\n",q->tid,n?text:"unavailable; see last_guest_stack and kernel permissions");}
 }
 if(!r->total)fprintf(f,"%d,%llu,%llu,%ld,0,0,0,%s,0,0,0,0,none\n",getpid(),(unsigned long long)cycle,(unsigned long long)r->epoch,r->coordinator,r->failure?r->failure:"empty_registry");
 fclose(f);
}

#ifdef GC_STW_RESTORE_FPRINTF
#pragma pop_macro("fprintf")
#undef GC_STW_RESTORE_FPRINTF
#endif
