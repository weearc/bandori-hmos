/* GIL-only metadata. Every non-atomic field below is protected by g_gil.m.
 * Switches are polled by the native observer, never from an acquire hot path. */
static atomic_int gil_broadcast, gil_priority, gil_short, gil_frame_touch;
static atomic_int gil_deep_enabled;
static atomic_int gil_deep_service_scan;
static atomic_int emu_detect_fast, emu_detect_profile;
int dvm_emu_detect_fast_enabled(void){return atomic_load(&emu_detect_fast);}
int dvm_gil_deep_service_scan_enabled(void){return atomic_load(&gil_deep_service_scan);}
static uint64_t gil_retries, gil_wake_returns, gil_queue_hist[129];
static atomic_uint_fast64_t gil_sites[5][10],gil_site_sum[5],gil_site_max[5];
static _Thread_local int gil_site;
static _Thread_local uint64_t gil_site_since;
static void gil_site_pause(void){
 if(gil_site_since&&atomic_load(&gil_obs_enabled)){
  uint64_t ns=gc_fast_ns()-gil_site_since;atomic_fetch_add(&gil_sites[gil_site][gil_bucket(ns)],1);atomic_fetch_add(&gil_site_sum[gil_site],ns);
  uint64_t old=atomic_load(&gil_site_max[gil_site]);while(old<ns&&!atomic_compare_exchange_weak(&gil_site_max[gil_site],&old,ns)){} }
 gil_site_since=0;
}
static void gil_site_resume(void){gil_site_since=atomic_load(&gil_obs_enabled)?gc_fast_ns():0;}
static struct gil_site_scope gil_site_enter(const char *cls){
 struct gil_site_scope s={gil_site};gil_site_pause();
 gil_site=cls&&strstr(cls,"reflect")?4:cls&&(!strncmp(cls,"java/io/",8)||!strncmp(cls,"java/net/",9)||strstr(cls,"okio/"))?3:cls&&!strcmp(cls,"GC")?2:cls&&!strcmp(cls,"JNI")?1:0;
 if(t_gil_depth)gil_site_resume();return s;
}
static void gil_site_leave(struct gil_site_scope *s){gil_site_pause();gil_site=s->previous;if(t_gil_depth)gil_site_resume();}
static atomic_long gil_render_tid;
void dvm_gil_render_thread(void){atomic_store(&gil_render_tid,gil_tid());}
static bool gil_is_main(void){return gil_tid()==atomic_load(&gil_render_tid);}
static bool gil_short_enabled(void){return atomic_load_explicit(&gil_short,memory_order_relaxed)!=0;}
int dvm_gil_frame_touch_enabled(void){return atomic_load(&gil_frame_touch);}
void dvm_gil_jni_thread(void){if(!t_gil_depth)gil_site=1;}
bool dvm_gil_try_acquire(struct dvm *vm);
static void gil_perf_controls(void){
 const char *names[]={"gil-broadcast","gil-priority","gil-short-slices","gil-frame-touch","gil-deep-trace","gil-deep-service-scan","emu-detect-fast","emu-detect-profile"};
 atomic_int *values[]={&gil_broadcast,&gil_priority,&gil_short,&gil_frame_touch,&gil_deep_enabled,&gil_deep_service_scan,&emu_detect_fast,&emu_detect_profile};
 for(unsigned i=0;i<8;i++){char p[256];snprintf(p,sizeof p,GIL_DIR "%s",names[i]);
  FILE *f=fopen(p,"r");int v=0;if(f){if(fscanf(f,"%d",&v)!=1)v=0;fclose(f);}
  else {f=fopen(p,"wx");if(f){fputs("0\n",f);fclose(f);chmod(p,0666);}}
  atomic_store(values[i],v==1);}
}
/* FIFO is mandatory once its head has waited 2ms. Priority is only allowed
 * ahead of younger background waiters. This bounds overtaking, not OS run time. */
static struct dvm_gil_waiter **gil_select_link(struct dvm_gil_waiter **head,uint64_t now,int priority){
 struct dvm_gil_waiter **link=head;
 if(priority&&*link&&now-(*link)->enqueued_ns<2000000ull){
  for(struct dvm_gil_waiter **p=link;*p;p=&(*p)->next)if((*p)->main_thread){link=p;break;}}
 return link;
}
static struct dvm_gil_waiter *gil_choose_waiter(uint64_t now){
 struct dvm_gil_waiter **link=gil_select_link(&g_gil_head,now,atomic_load(&gil_priority));
 struct dvm_gil_waiter *w=*link;
 if(w){*link=w->next;if(g_gil_tail==w){g_gil_tail=g_gil_head;while(g_gil_tail&&g_gil_tail->next)g_gil_tail=g_gil_tail->next;}}
 return w;
}
struct gil_wake_test {struct dvm *vm;atomic_int nonblocking,acquired;};
static void *gil_wake_test_thread(void *data){struct gil_wake_test *s=data;
 bool got=dvm_gil_try_acquire(s->vm);atomic_store(&s->nonblocking,!got);if(got)dvm_gil_release(s->vm);
 dvm_gil_acquire(s->vm);atomic_store(&s->acquired,1);dvm_gil_release(s->vm);return NULL;}
bool dvm_gil_scheduler_selftest(struct dvm *vm){
 FILE *f=fopen(GIL_DIR "gil-scheduler-selftest.txt","w");if(!f)return false;
 int pass=0;struct dvm_gil_waiter b={.enqueued_ns=9000000},m={.main_thread=true,.enqueued_ns=9000000};b.next=&m;
 struct dvm_gil_waiter *head=&b;
 bool fifo=gil_select_link(&head,10000000,0)==&head;
 bool priority=*gil_select_link(&head,10000000,1)==&m;
 bool aged=gil_select_link(&head,12000000,1)==&head;
 fprintf(f,"pid=%d FIFO=%s priority=%s aged_2ms_prevents_overtaking=%s\n",getpid(),fifo?"PASS":"FAIL",priority?"PASS":"FAIL",aged?"PASS":"FAIL");pass+=fifo+priority+aged;
 struct gil_wake_test s={.vm=vm};pthread_t t;int saved=atomic_exchange(&gil_broadcast,1);
 int rc=pthread_create(&t,NULL,gil_wake_test_thread,&s);bool spurious=false;
 if(!rc){
  for(int i=0;i<100;i++){if(atomic_load(&g_gil.waiters))break;struct timespec d={0,1000000};nanosleep(&d,NULL);}
  pthread_mutex_lock(&g_gil.m);uint64_t before=gil_retries;pthread_cond_broadcast(&g_gil.cv);pthread_mutex_unlock(&g_gil.m);
  for(int i=0;i<100;i++){pthread_mutex_lock(&g_gil.m);spurious=gil_retries>before;pthread_mutex_unlock(&g_gil.m);if(spurious)break;struct timespec d={0,1000000};nanosleep(&d,NULL);}
  bool excluded=!atomic_load(&s.acquired);unsigned depth=dvm_gil_unlock_all(vm);pthread_join(t,NULL);dvm_gil_relock(vm,depth);
  bool wake=atomic_load(&s.acquired);fprintf(f,"try_contended_returns=%d spurious_rechecked=%d exclusion=%d release_woke_waiter=%d restored=%d\n",atomic_load(&s.nonblocking),spurious,excluded,wake,dvm_gil_held(vm));pass+=atomic_load(&s.nonblocking)+spurious+excluded+wake;
 }
 atomic_store(&gil_broadcast,saved);fprintf(f,"RESULT checks=7 pass=%d result=%s moving_enabled=0\n",pass,pass==7?"PASS":"FAIL");fclose(f);return pass==7;
}
static void gil_perf_write(FILE *f){
 pthread_mutex_lock(&g_gil.m);
 uint64_t retries=gil_retries,wakes=gil_wake_returns,queue[129];memcpy(queue,gil_queue_hist,sizeof queue);
 pthread_mutex_unlock(&g_gil.m);
 fprintf(f,"PERF pid=%d broadcast=%d priority=%d short_slices=%d frame_touch=%d wake_returns=%llu predicate_retries=%llu lost_wakeups=not_measured priority_overtake_limit_ms=2\n",
  getpid(),atomic_load(&gil_broadcast),atomic_load(&gil_priority),atomic_load(&gil_short),atomic_load(&gil_frame_touch),
  (unsigned long long)wakes,(unsigned long long)retries);
 fprintf(f,"render_tid=%ld\n",atomic_load(&gil_render_tid));
 fputs("queue_length_at_enqueue_hist=",f);for(unsigned i=0;i<129;i++)fprintf(f,"%s%llu",i?",":"",(unsigned long long)queue[i]);fputc('\n',f);
 const char *names[]={"VM","JNI","GC","IO","reflection"};
 for(int c=0;c<5;c++){fprintf(f,"site=%s exclusive_segment_ms=%.6f max_ms=%.6f histogram=",names[c],atomic_load(&gil_site_sum[c])/1e6,atomic_load(&gil_site_max[c])/1e6);for(int i=0;i<10;i++)fprintf(f,"%s%llu",i?",":"",(unsigned long long)atomic_load(&gil_sites[c][i]));fputc('\n',f);}
}
