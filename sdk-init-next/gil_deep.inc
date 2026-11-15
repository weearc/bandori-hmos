/* Owner-side snapshots only: the observer never walks another thread's VM.
 * Bounded native records contain metadata, never guest values or credentials. */
struct deep_record {uint64_t ns,cpu_ns,stamp,samples,repeated;long tid;int site,n;char host[96],why[32],frames[8][512];};
static pthread_mutex_t deep_lock=PTHREAD_MUTEX_INITIALIZER;
static struct deep_record deep_segments[10],deep_holds[10];
static uint64_t deep_count,deep_hold_count;
static _Thread_local struct gil_deep_scope *deep_top;
static _Thread_local uint64_t deep_since;
static _Thread_local uint64_t deep_cpu_since;
static uint64_t deep_cpu_now(void){struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static void deep_start(void){deep_since=gc_fast_ns();deep_cpu_since=deep_cpu_now();}
static _Thread_local struct deep_record deep_best;
static _Thread_local const char *deep_host;
static _Thread_local const char *deep_hosts[32];
static _Thread_local unsigned deep_host_depth;
static void play_publish_method(struct dvm_method *m){
 if(!perf_play_enabled())return;
 atomic_fetch_add(&play_method_seq,1);atomic_store(&play_method_tid,gil_tid());
 atomic_store(&play_method_class,m&&m->cls?m->cls->name:"unknown");
 atomic_store(&play_method_name,m?m->name:"unknown");atomic_store(&play_method_sig,m?m->sig:"");
 atomic_fetch_add(&play_method_seq,1);
}
static void deep_snapshot(struct deep_record *r,uint64_t ns,const char *why){
 memset(r,0,sizeof *r);r->ns=ns;r->stamp=gc_fast_ns();r->tid=gil_tid();r->site=gil_site;
 snprintf(r->host,sizeof r->host,"%s",deep_host?deep_host:"none");snprintf(r->why,sizeof r->why,"%s",why);
 for(struct gil_deep_scope *s=deep_top;s&&r->n<8;s=s->previous){
  struct dvm_method *m=s->method;if(!m)continue;
  long long off=m->has_code&&m->dex&&m->dex->p?(long long)((uintptr_t)m->code.insns-(uintptr_t)m->dex->p+2ull*s->pc):-1;
  snprintf(r->frames[r->n++],512,"%s.%s%s pc_code_units=%u dex_byte_offset=%lld has_code=%d builtin=%d",m->cls?m->cls->name:"?",m->name?m->name:"?",m->sig?m->sig:"?",s->pc,off,m->has_code,m->builtin!=NULL);
  if(s==deep_top){r->samples=s->samples;r->repeated=s->repeated;}
 }
 if(!r->n)snprintf(r->frames[r->n++],512,"<host-no-active-guest-frame> dex_byte_offset=not_applicable");
}
static void deep_insert(struct deep_record *a,const struct deep_record *r){
 unsigned i=0;while(i<10&&a[i].ns>=r->ns)i++;if(i==10)return;
 memmove(a+i+1,a+i,(9-i)*sizeof *a);a[i]=*r;
}
static void deep_finish(const char *why){
 if(!deep_since)return;uint64_t ns=gc_fast_ns()-deep_since;deep_since=0;
 if(ns<1000000ull)return;
 struct deep_record r;deep_snapshot(&r,ns,why);r.cpu_ns=deep_cpu_now()-deep_cpu_since;if(ns>deep_best.ns)deep_best=r;
 pthread_mutex_lock(&deep_lock);deep_count++;deep_insert(deep_segments,&r);pthread_mutex_unlock(&deep_lock);
}
static void gil_deep_enter(struct gil_deep_scope *s,struct dvm *vm,struct dvm_method *m){
 if(!atomic_load_explicit(&gil_deep_enabled,memory_order_relaxed))return;
 if(deep_top&&deep_top->method==vm->cur_method)deep_top->pc=vm->cur_pc;
 deep_finish("method_enter");s->vm=vm;s->method=m;s->previous=deep_top;s->tracked=true;deep_top=s;deep_start();
 play_publish_method(m);
}
static void gil_deep_leave(struct gil_deep_scope *s){
 if(!s->tracked)return;deep_finish("method_return");deep_top=s->previous;
 play_publish_method(deep_top?deep_top->method:NULL);
 if(t_gil_depth&&atomic_load(&gil_deep_enabled))deep_start();else deep_since=0;
}
static void gil_deep_tick(uint32_t pc){
 if(!deep_top)return;deep_top->samples++;if(pc==deep_top->last_pc)deep_top->repeated++;
 deep_top->last_pc=pc;deep_top->pc=pc;
 /* Same-PC sampling is observational, not proof of a loop. Final intervals
  * are inserted once on return/release, so top ten contains distinct spans. */
}
static void gil_deep_pause(void){deep_finish("gil_release");}
static void gil_deep_resume(void){deep_best=(struct deep_record){0};if(atomic_load(&gil_deep_enabled))deep_start();else deep_since=0;}
static void gil_deep_hold_end(struct dvm *vm,uint64_t ns){
 (void)vm;if(!atomic_load(&gil_deep_enabled))return;deep_finish("hold_end");
 struct deep_record r=deep_best;if(!r.ns)deep_snapshot(&r,ns,"hold_no_leaf");
 r.ns=ns;snprintf(r.why,sizeof r.why,"complete_hold_peak_leaf");
 pthread_mutex_lock(&deep_lock);deep_hold_count++;deep_insert(deep_holds,&r);pthread_mutex_unlock(&deep_lock);
}
/* Native diagnostics execute on the owner; nested JNI may temporarily drop
 * GIL. The pause/resume hooks exclude that off-lock interval automatically. */
void dvm_gil_deep_host(struct dvm *vm,const char *name,int entering){
 (void)vm;if(!atomic_load(&gil_deep_enabled))return;deep_finish("host_boundary");
 if(entering){if(deep_host_depth<32){deep_hosts[deep_host_depth++]=deep_host;deep_host=name;}}
 else if(deep_host_depth)deep_host=deep_hosts[--deep_host_depth];
 if(t_gil_depth)deep_start();else deep_since=0;
}
static void gil_deep_write(void){
 if(!atomic_load(&gil_deep_enabled))return;
 struct deep_record segments[10],holds[10];uint64_t n,h;
 pthread_mutex_lock(&deep_lock);memcpy(segments,deep_segments,sizeof segments);memcpy(holds,deep_holds,sizeof holds);n=deep_count;h=deep_hold_count;pthread_mutex_unlock(&deep_lock);
 FILE *f=fopen(GIL_DIR "gil-deep-top.txt","w");if(!f)return;
 fprintf(f,"pid=%d segments_over_1ms=%llu complete_holds=%llu source=owner_only threshold_ms=50 moving_enabled=0 wait_reason=not_measured same_pc_samples_are_not_loop_proof\n",getpid(),(unsigned long long)n,(unsigned long long)h);
 for(int kind=0;kind<2;kind++)for(int i=0;i<10;i++){struct deep_record *r=kind?&holds[i]:&segments[i];if(!r->ns)continue;
  fprintf(f,"kind=%s rank=%d tid=%ld duration_ms=%.6f peak_leaf_cpu_ms=%.6f site=%d host=%s boundary=%s mono_ns=%llu pc_samples=%llu same_pc_samples=%llu\n",kind?"complete_hold":"leaf_segment",i+1,r->tid,r->ns/1e6,r->cpu_ns/1e6,r->site,r->host,r->why,(unsigned long long)r->stamp,(unsigned long long)r->samples,(unsigned long long)r->repeated);
  for(int j=0;j<r->n;j++)fprintf(f," frame=%d %s\n",j,r->frames[j]);
 }fclose(f);
}

bool dvm_gil_deep_selftest(struct dvm *vm){
 FILE *f=fopen(GIL_DIR "gil-deep-selftest.txt","w");if(!f)return false;
 struct dvm_class *base=dvm_find_class_or_external(vm,"sdktrace/GilDeepHeaderFixture");
 struct dvm_class *sub=dvm_find_class_or_external(vm,"sdktrace/GilDeepHeaderChild");
 bool ok=base&&sub&&!dvm_exception(vm);int pass=0;
 if(ok){sub->super=base;bool empty=heap_header_count_class(vm,base->name)==0;pass+=empty;
  dvm_ref a=dvm_new_object(vm,base);if(a)dvm_pin(vm,a);dvm_ref b=dvm_new_object(vm,sub);if(b)dvm_pin(vm,b);
  bool two=a&&b&&heap_header_count_class(vm,base->name)==2;pass+=two;
  bool child=heap_header_count_class(vm,sub->name)==1;pass+=child;
  struct dvm_gc_native_scope q={0};bool entered=dvm_gc_native_enter(vm,&q,a,"()V",NULL,0);
  if(entered){struct dvm_object *p=dvm__obj(vm,a);(void)p;uint32_t before=q.count;
   uint32_t count=heap_header_count_class(vm,base->name);bool stable=count==2&&q.count==before;pass+=stable;
   fprintf(f,"pid=%d empty=%d base_count=%u child_count=%u borrow_count_before=%u borrow_count_after=%u no_payload_borrow_added=%d\n",getpid(),empty,count,heap_header_count_class(vm,sub->name),before,q.count,stable);
   dvm_gc_native_leave(&q);
  }
  if(b)dvm_unpin(vm,b);if(a)dvm_unpin(vm,a);
 }
 ok=ok&&pass==4&&!dvm_exception(vm);fprintf(f,"pid=%d RESULT checks=4 pass=%d result=%s moving_enabled=0 GIL_released=0\n",getpid(),pass,ok?"PASS":"FAIL");fclose(f);return ok;
}

/* Narrow inclusive wall-clock timing, independent of heavy deep stack tracing.
 * Samples are real completed invoke() calls; selftest helper timings live in a
 * separate file. No guest values, pointers or credentials cross this mutex. */
static const struct {const char *owner,*name;} emu_methods[]={
 {"android/app/ActivityManager","getRunningServices"},
 {"android/app/ActivityManager","getRunningAppProcesses"},
 {"android/app/ActivityManager","getRunningTasks"},
 {"android/app/ActivityManager","getMemoryInfo"},
 {"android/app/ActivityManager","getProcessMemoryInfo"},
 {"android/content/pm/PackageManager","getInstalledPackages"},
 {"android/content/pm/PackageManager","getInstalledApplications"},
 {"android/content/pm/PackageManager","getPackageInfo"},
 {"android/content/pm/PackageManager","queryIntentActivities"},
 {"jp/co/cyberagent/ssg/ssgutil/EmulatorDetector","isEmulator"}
};
struct emu_sample_row {uint64_t count,max,sum,ns[256];int fast[256];};
static struct emu_sample_row emu_rows[10];
static pthread_mutex_t emu_profile_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint64_t emu_profile_generation;
static struct emu_profile_scope emu_profile_enter(struct dvm_method *m){
 struct emu_profile_scope s={.index=-1};
 if(!atomic_load(&emu_detect_profile)||!m||!m->cls)return s;
 for(int i=0;i<10;i++)if(!strcmp(m->cls->name,emu_methods[i].owner)&&!strcmp(m->name,emu_methods[i].name)){s.index=i;s.fast=atomic_load(&emu_detect_fast);s.start=gc_fast_ns();break;}
 return s;
}
static void emu_profile_leave(struct emu_profile_scope *s){
 if(s->index<0)return;uint64_t ns=gc_fast_ns()-s->start;
 pthread_mutex_lock(&emu_profile_mutex);struct emu_sample_row *r=&emu_rows[s->index];
 if(r->count<256){r->ns[r->count]=ns;r->fast[r->count]=s->fast;}r->count++;r->sum+=ns;if(ns>r->max)r->max=ns;emu_profile_generation++;
 pthread_mutex_unlock(&emu_profile_mutex);
}
static void emu_profile_write(void){
 static uint64_t published;if(!atomic_load(&emu_detect_profile))return;
 struct emu_sample_row rows[10];pthread_mutex_lock(&emu_profile_mutex);
 uint64_t generation=emu_profile_generation;if(generation==published){pthread_mutex_unlock(&emu_profile_mutex);return;}
 memcpy(rows,emu_rows,sizeof rows);pthread_mutex_unlock(&emu_profile_mutex);
 FILE *f=fopen(GIL_DIR "emu-detect-timing.csv","w");if(!f)return;
 fprintf(f,"pid,owner,method,index,elapsed_ms,total_calls,max_ms,fast\n");
 for(int i=0;i<10;i++)for(unsigned j=0;j<rows[i].count&&j<256;j++)fprintf(f,"%d,%s,%s,%u,%.6f,%llu,%.6f,%d\n",getpid(),emu_methods[i].owner,emu_methods[i].name,j,rows[i].ns[j]/1e6,(unsigned long long)rows[i].count,rows[i].max/1e6,rows[i].fast[j]);
 fclose(f);published=generation;
}
