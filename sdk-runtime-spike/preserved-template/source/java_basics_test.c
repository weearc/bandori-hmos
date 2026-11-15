#include "cert_runtime.h"
#include "exception_hierarchy.h"
#include "ssl_runtime.h"
#include "set_runtime.h"
#include "socket_runtime.h"
#include "object_runtime.h"
#include "list_views.h"
#include "platform_runtime.h"
#include "array_runtime.h"
#include "reflection_runtime.h"
#include "prefs_runtime.h"
#include "string_runtime.h"
#include "json_runtime.h"
#include "file_runtime.h"
extern bool asset_reader_selftest(struct dvm*);
extern bool package_metadata_selftest(struct dvm*);
#include "throwable_reference.h"
extern bool foundation_next_selftest(struct dvm*);
#include "thread_runtime.h"
#include "thread_runtime.h"
/* Tests run inside the native HAP before original SDK calls, on the exact
 * implementation used by the interpreter's external API hooks. */
#include "java_basics.h"
#include "portable.h"
#include <pthread.h>
#include "android_basics.h"
#include "guest_profile.h"
#include "collections.h"
#include "runtime_extra.h"
#include "executor_runtime.h"
#include "numeric_runtime.h"

static const char *cow="java/util/concurrent/CopyOnWriteArrayList";
static const char *ab="java/util/concurrent/atomic/AtomicBoolean";
static const char *tl="java/lang/ThreadLocal";
static union dvm_value call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref r,union dvm_value a,union dvm_value b){
 union dvm_value args[]={a,b},out={0};
 if(!java_basics_call(v,c,m,s,r,args,2,&out))dvm__throw(v,"java/lang/AssertionError","test dispatch not handled: %s.%s",c,m);
 return out;
}
struct thread_test {struct dvm *v;dvm_ref local,thread_value;bool okay;};
static void *other_thread(void *arg){
 struct thread_test *t=arg;dvm_gil_acquire(t->v);union dvm_value z={0},a={.l=t->thread_value};
 bool fresh=!call(t->v,tl,"get","()Ljava/lang/Object;",t->local,z,z).l;
 call(t->v,tl,"set","(Ljava/lang/Object;)V",t->local,a,z);
 t->okay=fresh&&call(t->v,tl,"get","()Ljava/lang/Object;",t->local,z,z).l==t->thread_value;
 dvm_gil_release(t->v);return NULL;
}
bool java_basics_selftest(void){
 unsigned checks=0;bool okay=true;struct dvm *v=dvm_create(NULL);if(!v)return false;
 union dvm_value z={0},one={.i=1},out={0};
#define CHECK(expr) do {++checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 const char *classes[]={"java/lang/Void","java/lang/Boolean","java/lang/Byte","java/lang/Character","java/lang/Short","java/lang/Integer","java/lang/Long","java/lang/Float","java/lang/Double"};
 const char *descs[]={"V","Z","B","C","S","I","J","F","D"};dvm_ref seen[9]={0};
 for(int i=0;i<9;++i){CHECK(java_basics_static(v,classes[i],"TYPE","Ljava/lang/Class;",&out));seen[i]=out.l;struct dvm_object *o=dvm__obj(v,out.l);CHECK(o&&o->klass&&o->klass->is_primitive&&!strcmp(o->klass->desc,descs[i]));CHECK(java_basics_static(v,classes[i],"TYPE","Ljava/lang/Class;",&out)&&out.l==seen[i]);for(int j=0;j<i;++j)CHECK(seen[j]!=seen[i]);}
 CHECK(!java_basics_static(v,"java/lang/Integer","UNKNOWN","Ljava/lang/Class;",&out));
 dvm_ref atom=dvm_new_object(v,dvm_find_class_or_external(v,ab));
 call(v,ab,"<init>","(Z)V",atom,one,z);CHECK(call(v,ab,"get","()Z",atom,z,z).i==1);
 CHECK(call(v,ab,"compareAndSet","(ZZ)Z",atom,z,one).i==0);CHECK(call(v,ab,"get","()Z",atom,z,z).i==1);
 CHECK(call(v,ab,"compareAndSet","(ZZ)Z",atom,one,z).i==1);CHECK(call(v,ab,"getAndSet","(Z)Z",atom,one,z).i==0);
 call(v,ab,"lazySet","(Z)V",atom,z,z);CHECK(call(v,ab,"get","()Z",atom,z,z).i==0);
 dvm_ref list=dvm_new_object(v,dvm_find_class_or_external(v,cow));call(v,cow,"<init>","()V",list,z,z);
 CHECK(call(v,cow,"isEmpty","()Z",list,z,z).i==1);
 union dvm_value a={.l=dvm_new_string(v,"first")},same={.l=dvm_new_string(v,"first")},b={.l=dvm_new_string(v,"second")};
 CHECK(call(v,cow,"add","(Ljava/lang/Object;)Z",list,a,z).i==1);CHECK(call(v,cow,"isEmpty","()Z",list,z,z).i==0);
 CHECK(call(v,cow,"contains","(Ljava/lang/Object;)Z",list,same,z).i==1);
 CHECK(call(v,cow,"addIfAbsent","(Ljava/lang/Object;)Z",list,same,z).i==0);
 dvm_ref it=call(v,cow,"iterator","()Ljava/util/Iterator;",list,z,z).l;
 call(v,cow,"add","(Ljava/lang/Object;)Z",list,b,z);CHECK(call(v,cow,"size","()I",list,z,z).i==2);
 CHECK(call(v,"java/util/Iterator","next","()Ljava/lang/Object;",it,z,z).l==a.l);
 CHECK(call(v,"java/util/Iterator","hasNext","()Z",it,z,z).i==0);
 CHECK(call(v,cow,"set","(ILjava/lang/Object;)Ljava/lang/Object;",list,z,b).l==a.l);
 CHECK(call(v,cow,"get","(I)Ljava/lang/Object;",list,z,z).l==b.l);
 CHECK(call(v,cow,"remove","(Ljava/lang/Object;)Z",list,b,z).i==1);CHECK(call(v,cow,"size","()I",list,z,z).i==1);
 call(v,cow,"clear","()V",list,z,z);CHECK(call(v,cow,"isEmpty","()Z",list,z,z).i==1);
 CHECK(call(v,cow,"addIfAbsent","(Ljava/lang/Object;)Z",list,z,z).i==1);CHECK(call(v,cow,"addIfAbsent","(Ljava/lang/Object;)Z",list,z,z).i==0);
 dvm_ref arr=call(v,cow,"toArray","()[Ljava/lang/Object;",list,z,z).l;CHECK(dvm_array_length(v,arr)==1&&((dvm_ref *)dvm_array_data(v,arr))[0]==0);
 call(v,cow,"get","(I)Ljava/lang/Object;",list,one,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 call(v,"java/util/Iterator","next","()Ljava/lang/Object;",it,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 call(v,"java/util/Iterator","remove","()V",it,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 CHECK(!java_basics_call(v,cow,"unimplemented","()V",list,NULL,0,&out));
 dvm_ref local=dvm_new_object(v,dvm_find_class_or_external(v,tl));call(v,tl,"<init>","()V",local,z,z);
 CHECK(call(v,tl,"get","()Ljava/lang/Object;",local,z,z).l==0);call(v,tl,"set","(Ljava/lang/Object;)V",local,a,z);
 struct thread_test test={.v=v,.local=local,.thread_value=b.l};pthread_t thread;int created=pthread_create(&thread,NULL,other_thread,&test);
 CHECK(created==0);if(!created){thread_runtime_join_host(v,thread);CHECK(test.okay);}
 CHECK(call(v,tl,"get","()Ljava/lang/Object;",local,z,z).l==a.l);call(v,tl,"remove","()V",local,z,z);CHECK(call(v,tl,"get","()Ljava/lang/Object;",local,z,z).l==0);
 CHECK(!dvm_exception(v));if(!android_basics_selftest(v,&checks))okay=false;if(!guest_profile_selftest(v,&checks))okay=false;if(!collections_selftest(v,&checks))okay=false;if(!runtime_extra_selftest(v,&checks))okay=false;if(!numeric_runtime_selftest(v))okay=false;if(!vm_arena_java_selftest(v))okay=false;if(!thread_runtime_selftest(v))okay=false;if(!executor_runtime_selftest(v,&checks))okay=false;if(!foundation_next_selftest(v))okay=false;if(!throwable_reference_selftest(v))okay=false;if(!package_metadata_selftest(v))okay=false;if(!asset_reader_selftest(v))okay=false;if(!string_runtime_selftest(v))okay=false;if(!json_runtime_selftest(v))okay=false;if(!file_runtime_selftest(v))okay=false;if(!prefs_runtime_selftest(v))okay=false;if(!reflection_runtime_selftest(v))okay=false;if(!array_runtime_selftest(v))okay=false;if(!list_views_selftest(v))okay=false;if(!platform_runtime_selftest(v))okay=false;if(!object_runtime_selftest(v))okay=false;if(!socket_runtime_selftest(v))okay=false;if(!exception_hierarchy_selftest(v))okay=false;if(!cert_runtime_selftest(v))okay=false;if(!ssl_runtime_selftest(v))okay=false;if(!set_runtime_selftest(v))okay=false;dvm_destroy(v);
 sdk_trace_fprintf(stderr,"SELFTEST %s checks=%u marker=integrated-login-tls-next primitive_identity=9 cow_snapshot=1 atomic_cas=1 threadlocal_threads=2\n",okay?"PASS":"FAIL",checks);
 return okay;
}
#include "runtime_extra_test.inc"
