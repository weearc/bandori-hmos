"""Build real-thread candidate from v8 snapshot without overwriting v7/v8."""
from pathlib import Path
import shutil,subprocess,re,hashlib,json
ROOT=Path(__file__).resolve().parent
BASE=ROOT/'build/next/source'
OUT=ROOT/'build/thread-next';SRC=OUT/'source'
pins={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [ROOT/'build/libsdktrace.so',ROOT/'build/next/libsdktrace.so']}
shutil.copytree(BASE,SRC,dirs_exist_ok=True)
for n in ['thread_runtime.c','thread_runtime.h','thread_runtime_test.c']:shutil.copy2(ROOT/n,SRC/n)
def edit(name,old,new,count=1):
 p=SRC/name;s=p.read_text(encoding='utf8');assert s.count(old)==count,(name,old,s.count(old));p.write_text(s.replace(old,new),encoding='utf8')
for name in ['strict_runtime.c','java_basics_test.c','runtime_extra_test.inc','next_arena.c']:
 p=SRC/name;s=p.read_text(encoding='utf8').replace('managed-arena-v8','real-thread-v9');p.write_text(s,encoding='utf8')
for name in ['strict_runtime.c','android_basics.c','java_basics_test.c','android_basics_test.c']:
 p=SRC/name;p.write_text('#include "thread_runtime.h"\n'+p.read_text(encoding='utf8'),encoding='utf8')
# GIL protects every main-thread VM entry; worker acquires the same primitive.
edit(Path('generated/dvm/dvm.c'),'   g_gil_vm = vm;','   g_gil_vm = vm;')
p=SRC/'generated/dvm/dvm.c';s=p.read_text(encoding='utf8');m=re.search(r'struct dvm \*dvm_create\([^;]*?\)\s*\{.*?^}',s,re.M|re.S);body=m.group();assert body.count('   return vm;')==1;body=body.replace('   return vm;','   dvm_gil_acquire(vm);\n   dvm__tstate_reset(vm);\n   vm->cur_thread=thread_runtime_current(vm);\n   return vm;');s=s[:m.start()]+body+s[m.end():];s='#include "thread_runtime.h"\n'+s;s=s.replace('   dvm_request_stop(vm);\n   dvm_threads_finish(vm);','   thread_runtime_shutdown(vm,100);\n   dvm_request_stop(vm);\n   dvm_threads_finish(vm);');s=s.replace('   free(vm);\n   vm_arena_end();','   dvm_gil_release(vm);\n   free(vm);\n   vm_arena_end();');p.write_text(s,encoding='utf8')
edit(Path('generated/dvm/dvm.c'),'      ++vm->steps;','      if (++vm->steps>100000) { dvm__throw(vm,"java/lang/Error","aggregate diagnostic bytecode budget"); dvm_request_stop(vm); return false; }')
edit(Path('strict_runtime.c'),'static _Thread_local char trace','static char trace');edit(Path('strict_runtime.c'),'static _Thread_local size_t trace_used','static size_t trace_used');edit(Path('strict_runtime.c'),'static _Thread_local unsigned missing','static unsigned missing');edit(Path('strict_runtime.c'),'static _Thread_local bool details_truncated','static bool details_truncated')
# Capture all thread output into one locked run buffer; avoid recursive logging
# when emitting the truncation message while holding this mutex.
edit(Path('strict_runtime.c'),'static pthread_mutex_t runner_lock=PTHREAD_MUTEX_INITIALIZER;','static pthread_mutex_t runner_lock=PTHREAD_MUTEX_INITIALIZER;\nstatic pthread_mutex_t trace_lock=PTHREAD_MUTEX_INITIALIZER;')
edit(Path('strict_runtime.c'),'static char trace[131072];','static char trace[131072];\nstatic _Thread_local char returned_trace[131072];')
edit(Path('strict_runtime.c'),'pthread_mutex_unlock(&runner_lock);return trace;','memcpy(returned_trace,trace,trace_used+1);pthread_mutex_unlock(&runner_lock);return returned_trace;',count=3)
edit(Path('strict_runtime.c'),'THREAD_MODEL VM main Looper belongs to interpreter creation thread (NAPI worker); no ArkTS/UI-thread binding, post or loop scheduler','THREAD_MODEL main Looper identity is NAPI worker, no ArkTS/UI queue; one real HandlerThread supports asynchronous worker Looper dispatch and joined shutdown')
edit(Path('android_basics_test.c'),'scheduler_unimplemented=1','main_scheduler_unimplemented=1')
edit(Path('runtime_extra_test.inc'),'thread_new_only=1','thread_initial_state_NEW=1')
edit(Path('next_arena.c'),'this candidate (Thread.start remains unsupported). Reclaim unreachable data','this candidate after thread_runtime_shutdown has joined them. Reclaim unreachable data')
edit(Path('strict_runtime.c'),' (void)f;\n bool verbose=',' (void)f;\n pthread_mutex_lock(&trace_lock);\n bool verbose=')
edit(Path('strict_runtime.c'),'!strncmp(fmt," on @",5);','!strncmp(fmt," on @",5)||!strcmp(fmt,"%c");')
edit(Path('strict_runtime.c'),'if(!details_truncated){details_truncated=true;sdk_trace_fprintf(f,"TRACE_TRUNCATED verbose details capped; stage summaries and missing APIs retained\\n");}\n  return 0;', 'if(!details_truncated){details_truncated=true;const char *notice="\\nTRACE_TRUNCATED verbose details capped; stage summaries and missing APIs retained\\n";size_t n=strlen(notice);if(trace_used+n<sizeof trace){memcpy(trace+trace_used,notice,n+1);trace_used+=n;}}\n  pthread_mutex_unlock(&trace_lock);return 0;')
edit(Path('strict_runtime.c'),' return n;\n}',' pthread_mutex_unlock(&trace_lock);return n;\n}')
edit(Path('strict_runtime.c'),'if(!details_truncated)sdk_trace_fprintf(f,"%c",ch);','sdk_trace_fprintf(f,"%c",ch);')
p=SRC/'strict_runtime.c';s=p.read_text(encoding='utf8').replace('++missing;','trace_missing_increment();');index=s.index('static bool external(');s=s[:index]+'static void trace_missing_increment(void){pthread_mutex_lock(&trace_lock);++missing;pthread_mutex_unlock(&trace_lock);}\nvoid sdk_trace_async_failure(struct dvm *v,const char *where){trace_missing_increment();char why[1024];dvm_describe_exception(v,dvm_exception(v),why,sizeof why);sdk_trace_fprintf(stderr,"ASYNC_FAILURE where=%s exception=%s\\n",where,why);}\n'+s[index:];s=s.replace('if(runtime_extra_call(vm,','if(thread_runtime_call(vm,c,m,s,self,a,n,out)||runtime_extra_call(vm,');s=s.replace('runtime_extra_vm_start(v);}','runtime_extra_vm_start(v);thread_runtime_init(v);}');s=s.replace('dvm_ref dvm__current_thread(struct dvm *v) {(void)v;return 0;}','dvm_ref dvm__current_thread(struct dvm *v) {return thread_runtime_current(v);}');s=s.replace('bool dvm__logical_main_thread(struct dvm *v) {(void)v;return true;}','bool dvm__logical_main_thread(struct dvm *v) {return thread_runtime_is_main(v);}');s=s.replace(' sdk_trace_fprintf(stderr,"END_STAGE %s',' thread_runtime_shutdown(v,100);\n sdk_trace_fprintf(stderr,"END_STAGE %s');p.write_text(s,encoding='utf8')
edit(Path('android_basics.c'),' (void)nargs;memset(out,0,sizeof *out);',' (void)nargs;memset(out,0,sizeof *out);\n if(thread_runtime_call(v,c,m,sig,self,a,nargs,out))return true;')
android=SRC/'android_basics.c';s=android.read_text(encoding='utf8');begin=s.index(' bool thread_receiver=false;');end=s.index(' if(!strcmp(c,"java/lang/String")&&(IS("substring"',begin);s=s[:begin]+s[end:];android.write_text(s,encoding='utf8')
edit(Path('android_basics.c'),'if(l!=main_looper(v)){','if(l!=main_looper(v)&&!thread_runtime_looper(v,l)){')
# Existing C-only tests now obey VM owner synchronization across host threads.
edit(Path('java_basics_test.c'),' struct thread_test *t=arg;',' struct thread_test *t=arg;dvm_gil_acquire(t->v);')
edit(Path('java_basics_test.c'),' return NULL;\n}',' dvm_gil_release(t->v);return NULL;\n}')
edit(Path('java_basics_test.c'),'pthread_join(thread,NULL);','thread_runtime_join_host(v,thread);')
edit(Path('android_basics_test.c'),'struct thread_case *t=arg;','struct thread_case *t=arg;dvm_gil_acquire(t->v);')
edit(Path('android_basics_test.c'),'return NULL;}','dvm_gil_release(t->v);return NULL;}')
edit(Path('android_basics_test.c'),'pthread_join(thread,NULL);','thread_runtime_join_host(v,thread);')
edit(Path('runtime_extra_test.inc'),'EXTRA_CHECK(!android_basics_call(v,thread,"start","()V",ht,NULL,0,&out));','EXTRA_CHECK(!thread_runtime_looper(v,0));')
edit(Path('java_basics_test.c'),'if(!vm_arena_java_selftest(v))okay=false;dvm_destroy(v);','if(!vm_arena_java_selftest(v))okay=false;if(!thread_runtime_selftest(v))okay=false;dvm_destroy(v);')
compiler=Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/llvm/bin/clang.exe');sdk=compiler.parents[2];common=[str(compiler),'--target=aarch64-linux-ohos','--sysroot='+str(sdk/'sysroot'),'-std=gnu11','-fPIC','-O1','-I'+str(SRC),'-I'+str(SRC/'generated')]
names=['generated/dvm/dvm.c','generated/dvm/dex.c','strict_runtime.c','java_basics.c','java_basics_test.c','android_basics.c','android_basics_test.c','guest_profile.c','collections.c','collections_test.c','next_arena.c','thread_runtime.c','thread_runtime_test.c'];objects=[]
for name in names:
 obj=OUT/(Path(name).stem+'.o');args=common.copy()
 if name!='next_arena.c':args+=['-include',str(SRC/'next_interpose.h')]
 subprocess.run(args+['-c',str(SRC/name),'-o',str(obj)],check=True);objects.append(str(obj))
coverage=[];nm=compiler.parent/'llvm-nm.exe';allocation_symbols={'malloc','calloc','realloc','free','strdup','strndup','mmap','aligned_alloc','posix_memalign'}
for name,obj in zip(names,objects):
 if name=='next_arena.c':continue
 undefined=subprocess.check_output([str(nm),'--undefined-only',obj],text=True);bypass=allocation_symbols.intersection(line.split()[-1] for line in undefined.splitlines() if line.split());assert not bypass,(name,bypass);coverage.append(name)
subprocess.run(common+['-shared',*objects,'-Wl,--no-undefined','-lm','-o',str(OUT/'libsdktrace.so')],check=True)
for path,digest in pins.items():assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==digest
manifest={'candidate':'real-thread-v9','sha256':hashlib.sha256((OUT/'libsdktrace.so').read_bytes()).hexdigest(),'bytes':(OUT/'libsdktrace.so').stat().st_size,'preserved_artifacts':pins,'execution_status':'compiled only; not installed or device-tested','allocation_symbol_audit_passed':coverage,'scope':'one real HandlerThread, GIL/TLS, published worker Looper, asynchronous Runnable/Message queue, quit+join before arena teardown','bounds':{'aggregate_bytecode_per_VM':100000,'queue_tasks':1024,'publication_timeout_ms':2000,'shutdown_drain_ms':100}}
(OUT/'candidate.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest,indent=2))
