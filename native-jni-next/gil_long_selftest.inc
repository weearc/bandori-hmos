/* Explicit debug-only ABI-dispatch/GIL-boundary test; no guest references,
 * no pressure allocations, no registered guest method is claimed here. */
struct gil_long_case {struct dvm *vm;_Atomic int started,returned,acquired;uint64_t wait_ns;int before_return;};
static uint64_t gil_test_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uintptr_t gil_test_native(JNIEnv *env,...){
 (void)env;va_list ap;va_start(ap,env);struct gil_long_case *s=(void*)va_arg(ap,uintptr_t);va_end(ap);
 atomic_store(&s->started,1);struct timespec delay={0,200000000};while(nanosleep(&delay,&delay)&&errno==EINTR){}atomic_store(&s->returned,1);return 0;
}
static void *gil_test_contender(void *data){struct gil_long_case *s=data;while(!atomic_load(&s->started)){struct timespec d={0,100000};nanosleep(&d,NULL);}uint64_t start=gil_test_ns();dvm_gil_acquire(s->vm);s->wait_ns=gil_test_ns()-start;s->before_return=!atomic_load(&s->returned);atomic_store(&s->acquired,1);dvm_gil_release(s->vm);return NULL;}
void jni_gil_long_selftest(struct dvm *vm){
 struct jni_bridge *b=jni_bridge_for_vm(vm);if(!b||!dvm_gil_held(vm))return;struct jni_thread_env *t=thread_ensure(b);if(!t)return;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/gil-long-jni-test.txt","w");if(!f)return;
 for(int released=0;released<2;released++){
  struct gil_long_case s={.vm=vm};pthread_t contender;int rc=pthread_create(&contender,NULL,gil_test_contender,&s);if(rc){fprintf(f,"FAIL pthread_create=%d\n",rc);break;}
  uint64_t start=gil_test_ns();unsigned depth=released?jni_bridge_gil_drop(b):0;
  (void)native_call_words((jni_native_va_fn)gil_test_native,(JNIEnv*)(void*)t,(uintptr_t)&s,NULL,0);
  if(released)jni_bridge_gil_restore(b,depth);
  unsigned join_depth=jni_bridge_gil_drop(b);pthread_join(contender,NULL);jni_bridge_gil_restore(b,join_depth);
  int pass=atomic_load(&s.acquired)&&(!released||(s.before_return&&s.wait_ns<50000000));
  fprintf(f,"case=%s native_sleep_ms=200 wall_ms=%.3f contender_wait_ms=%.3f acquired_before_native_return=%d restored=%d result=%s boundary=actual_native_call_words_and_bridge_drop_restore\n",released?"released":"held_control",(double)(gil_test_ns()-start)/1e6,(double)s.wait_ns/1e6,s.before_return,dvm_gil_held(vm),pass?"PASS":"FAIL");
 }fclose(f);
}
