#include "dvm/dvm_internal.h"
#include "leak_scope.h"
#include "perf_attr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

_Thread_local struct leak_scope *leak_scope_current;
static _Atomic uint64_t opened,retired,reaped,rejected[LEAK_REASON_COUNT];
void leak_scope_cancel(struct dvm *v,unsigned reason){
 struct leak_scope *s=leak_scope_current;
 if(s&&s->vm==v&&!s->reason)s->reason=reason;
}
bool leak_scope_box(struct dvm *v,uint32_t ref){
 struct leak_scope *s=leak_scope_current;return s&&s->vm==v&&ref&&ref==s->box;
}
bool leak_scope_mentions(struct dvm *v,uint32_t self,const uint32_t *slots,int n){
 struct leak_scope *s=leak_scope_current;if(!s||s->vm!=v)return false;
 if(self&&(self==s->array||self==s->box))return true;
 /* Scalars with the same bits may cause conservative rejection. Never
  * mistake absence from a partial reference map for proof of no escape. */
 for(int i=0;slots&&i<n;i++)if(slots[i]==s->array||slots[i]==s->box)return true;
 return false;
}
void leak_scope_method(struct dvm *v,const struct dvm_method *m,uint32_t self,const uint32_t *slots,int n){
 if(!leak_scope_mentions(v,self,slots,n))return;
 const char *c=m&&m->cls?m->cls->name:NULL;
 bool known=c&&m->name&&m->sig&&(
  (!strcmp(c,"bitter/jnibridge/a")&&!strcmp(m->name,"invoke")&&!strcmp(m->sig,"(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;"))||
  (!strcmp(c,"bitter/jnibridge/JNIBridge")&&!strcmp(m->name,"invoke")&&!strcmp(m->sig,"(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;"))||
  (!strcmp(c,"java/lang/Long")&&!strcmp(m->name,"longValue")&&!strcmp(m->sig,"()J")));
 if(!known)leak_scope_cancel(v,LEAK_UNKNOWN_CALL);
}
bool leak_scope_external_allowed(struct dvm *v,const char *c,const char *m,const char *sig,uint32_t self,const uint32_t *slots,int n){
 extern bool leak_scope_external_is_trusted(struct dvm *) __attribute__((weak));
 if(!leak_scope_external_is_trusted||!leak_scope_external_is_trusted(v)||
    leak_scope_mentions(v,self,slots,n))return false;
 /* These implementations only read metadata, or enqueue the existing
  * callback (which must not be one of the private temporaries). Unknown
  * C extensions are rejected even without an explicit candidate argument. */
 if(!strcmp(c,"java/lang/reflect/Method"))return
  (!strcmp(m,"getDeclaringClass")&&!strcmp(sig,"()Ljava/lang/Class;"))||
  (!strcmp(m,"getName")&&!strcmp(sig,"()Ljava/lang/String;"))||
  (!strcmp(m,"getModifiers")&&!strcmp(sig,"()I"));
 if(!strcmp(c,"android/view/Choreographer"))return
  (!strcmp(m,"postFrameCallback")&&!strcmp(sig,"(Landroid/view/Choreographer$FrameCallback;)V"))||
  (!strcmp(m,"getFrameTimeNanos")&&!strcmp(sig,"()J"))||
  (!strcmp(m,"getInstance")&&!strcmp(sig,"()Landroid/view/Choreographer;"));
 if(!strcmp(c,"android/os/Looper"))return
  (!strcmp(m,"myLooper")||!strcmp(m,"getMainLooper"))&&!strcmp(sig,"()Landroid/os/Looper;");
 return false;
}
bool leak_scope_begin(struct leak_scope *s,struct dvm *v,uint32_t array,uint32_t box,int64_t value){
 *s=(struct leak_scope){0};
 if(leak_scope_current){leak_scope_cancel(leak_scope_current->vm,LEAK_NESTED);return false;}
 /* No original native-body certificate exists yet. An absent provider is
  * UNKNOWN, even when the operator explicitly sets the control to 1. */
 extern bool leak_scope_native_body_certified(struct dvm *) __attribute__((weak));
 if(!leak_scope_native_body_certified||!leak_scope_native_body_certified(v)){
  atomic_fetch_add(&rejected[LEAK_UNKNOWN_CALL],1);return false;
 }
 if(!perf_leak_proxy_reclaim()||!dvm_gil_held(v)||!dvm_leak_collection_off(v)||
    (value>=-128&&value<=127)||!leak_scope_owned_long(v,box,value))return false;
 struct dvm_object *a=dvm__obj(v,array),*b=dvm__obj(v,box);
 if(!a||!b||a->kind!=DVM_OBJ_ARRAY||a->elem_kind!='L'||a->length!=1||
    !a->data||((uint32_t*)a->data)[0]!=box||a->data_destroy||a->data_owner||
    a->pins!=1||b->pins||a->gc_strong_handles||b->gc_strong_handles)return false;
 *s=(struct leak_scope){.vm=v,.array=array,.box=box,.array_id=a->identity_generation,.box_id=b->identity_generation};
 leak_scope_current=s;atomic_fetch_add(&opened,1);return true;
}
void leak_scope_end(struct leak_scope *s,bool ok,uint32_t result){
 if(!s->vm)return;
 if(leak_scope_current!=s){leak_scope_cancel(s->vm,LEAK_NESTED);return;}
 if(!dvm_gil_held(s->vm))leak_scope_cancel(s->vm,LEAK_UNKNOWN_CALL);
 if(!ok)leak_scope_cancel(s->vm,LEAK_EXCEPTION);
 if(result==s->array||result==s->box)leak_scope_cancel(s->vm,LEAK_RETURN);
 leak_scope_current=NULL;
 if(!s->reason&&dvm_leak_collection_off(s->vm)){
  if(dvm_leak_retire_pair(s->vm,s->array,s->box,s->array_id,s->box_id))atomic_fetch_add(&retired,1);
  else s->reason=LEAK_RETAINED_ROOT;
 }else if(!s->reason)s->reason=LEAK_UNKNOWN_CALL;
 if(s->reason&&s->reason<LEAK_REASON_COUNT)atomic_fetch_add(&rejected[s->reason],1);
}
void leak_scope_opcode(struct dvm *v,unsigned op){
 /* Positive allow-list: register moves/constants, reads, comparisons,
  * control flow, calls, arithmetic. Allocations, fill-array-data, all stores,
  * throws and unrecognized/quick opcodes retain the pair. Monitors have a
  * separate alias check in the VM monitor implementation. */
 bool safe=op<=0x21 || (op>=0x28&&op<=0x3d) ||
  (op>=0x44&&op<=0x4a)||(op>=0x52&&op<=0x58)||
  (op>=0x6e&&op<=0x72)||
  (op>=0x74&&op<=0x78)||(op>=0x7b&&op<=0xe2);
 if(!safe)leak_scope_cancel(v,LEAK_STORE);
}
void leak_scope_monitor(struct dvm *v,uint32_t ref){
 if(leak_scope_mentions(v,ref,NULL,0))leak_scope_cancel(v,LEAK_MONITOR);
}
void leak_scope_jni(struct dvm *v,const char *name){
 if(!leak_scope_current)return;
 /* Local tokens never escape their native frame. Global/weak references,
  * field/array writes, new object construction, raw buffers and monitors
  * are deliberately absent. Call* re-enters the guarded DVM dispatcher. */
 static const char *const reads[]={
  "bridge_GetVersion","bridge_FindClass","bridge_GetSuperclass",
  "bridge_IsAssignableFrom","bridge_GetObjectClass","bridge_IsInstanceOf",
  "bridge_IsSameObject","bridge_GetMethodID","bridge_GetStaticMethodID",
  "bridge_GetFieldID","bridge_GetStaticFieldID","bridge_GetArrayLength",
  "bridge_GetObjectArrayElement","bridge_PushLocalFrame","bridge_PopLocalFrame",
  "bridge_NewLocalRef","bridge_DeleteLocalRef","bridge_EnsureLocalCapacity",
  "bridge_ExceptionCheck","bridge_GetObjectRefType","bridge_GetJavaVM"};
 for(unsigned i=0;i<sizeof reads/sizeof reads[0];i++)if(!strcmp(name,reads[i]))return;
 /* Match only currently generated Call{Static,Nonvirtual}{Type}Method[A,V]
  * entry points; a future similarly named API is not implicitly trusted. */
 if(!strncmp(name,"bridge_Call",11)){
  const char *p=name+11;
  if(!strncmp(p,"Static",6))p+=6;else if(!strncmp(p,"Nonvirtual",10))p+=10;
  static const char *const kinds[]={"Object","Boolean","Byte","Char","Short","Int","Long","Float","Double","Void"};
  for(unsigned i=0;i<sizeof kinds/sizeof kinds[0];i++){
   size_t len=strlen(kinds[i]);
   if(!strncmp(p,kinds[i],len)&&!strncmp(p+len,"Method",6)){
    const char *end=p+len+6;if(!*end||((end[0]=='A'||end[0]=='V')&&!end[1]))return;
   }
  }
 }
 leak_scope_cancel(v,LEAK_UNKNOWN_JNI);
}
void leak_scope_reaped(void){atomic_fetch_add(&reaped,1);}
void leak_scope_status(void){
 /* Only the independent arena observer calls this. No new filesystem
  * operation is added to owner polling/render/callback paths. */
 static uint64_t last;uint64_t now=perf_now();if(now-last<1000000000ull)return;last=now;
 FILE *control=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/leak-proxy-reclaim","r");int enabled=0;
 if(control){if(fscanf(control,"%d",&enabled)!=1)enabled=0;fclose(control);}
 perf_leak_proxy_reclaim_configure(enabled);
 if(!perf_leak_proxy_reclaim()&&!atomic_load(&opened))return;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/leak-proxy-reclaim.txt","w");if(!f)return;
 fprintf(f,"pid=%d enabled=%d opened=%llu retired=%llu reaped_pairs=%llu moving=0 collection_started=0\n",getpid(),perf_leak_proxy_reclaim(),(unsigned long long)atomic_load(&opened),(unsigned long long)atomic_load(&retired),(unsigned long long)atomic_load(&reaped));
 for(unsigned i=1;i<LEAK_REASON_COUNT;i++)fprintf(f,"rejected_reason_%u=%llu\n",i,(unsigned long long)atomic_load(&rejected[i]));
 for(unsigned i=0;i<LEAK_CERT_REJECT_COUNT;i++)fprintf(f,"certificate_rejected_%u=%llu\n",i,(unsigned long long)leak_observer_rejections(i));
 fclose(f);
}

/* Enrol only these statically linked implementations, never the pointer supplied
 * by a call site. Symbols below are >16 bytes in both checked target objects.
 * Prefix equality also compares bytes: the hash alone is not an attestation.
 * Code rewriting/concurrent unloading is unsupported and cannot unlock reclaim.
 */
extern void leak_cert_task6(void) __attribute__((weak,visibility("hidden")));
extern void leak_cert_panel_pre(void) __attribute__((weak,visibility("hidden")));
extern void leak_cert_panel_post(void) __attribute__((weak,visibility("hidden")));
#define LEAK_PREFIX 16
static struct {uintptr_t fn;unsigned char bytes[LEAK_PREFIX];uint64_t hash;} observer_identity[LEAK_OBSERVER_ROLES];
static pthread_once_t observer_once=PTHREAD_ONCE_INIT;
static _Atomic uint64_t certificate_rejects[LEAK_CERT_REJECT_COUNT];
uint64_t leak_observer_rejections(unsigned reason){return reason<LEAK_CERT_REJECT_COUNT?atomic_load(&certificate_rejects[reason]):0;}
static uint64_t observer_hash(const unsigned char *p){
 uint64_t h=UINT64_C(14695981039346656037);
 for(unsigned i=0;i<LEAK_PREFIX;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h;
}
static void observer_enrol(void){
 const uintptr_t f[]={ (uintptr_t)leak_cert_task6,(uintptr_t)leak_cert_task6,
  (uintptr_t)leak_cert_panel_pre,(uintptr_t)leak_cert_panel_post };
 for(unsigned i=0;i<LEAK_OBSERVER_ROLES;i++)if(f[i]){
  observer_identity[i].fn=f[i];memcpy(observer_identity[i].bytes,(const void*)f[i],LEAK_PREFIX);
  observer_identity[i].hash=observer_hash(observer_identity[i].bytes);
 }
}
void leak_observer_bootstrap(void){pthread_once(&observer_once,observer_enrol);}
static bool observer_proxy(struct dvm *v,uint32_t r){
 struct dvm_object *o=dvm__obj(v,r);return o&&o->cls&&o->cls->generated_proxy;
}
static bool observer_class(struct dvm *v,uint32_t r,const char *name){
 struct dvm_object *o=dvm__obj(v,r);return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,name);
}
bool leak_observer_check(struct dvm *v,unsigned role,uintptr_t fn,const struct dvm_method *m,
 uint32_t self,const uint32_t *args,int n,const void *dispatcher,struct leak_observer_certificate *cert){
 struct leak_scope *s=leak_scope_current;
 if(cert)memset(cert,0,sizeof *cert);
 if(!s||s->vm!=v)return true;
 /* Never lazily enrol at the observer boundary: a missing bootstrap rejects. */
 unsigned failure=LEAK_CERT_REJECT_COUNT;
 struct dvm_object *a=dvm__obj(v,s->array),*b=dvm__obj(v,s->box);
 bool good=a&&b&&a->identity_generation==s->array_id&&b->identity_generation==s->box_id;
 if(!good)failure=LEAK_CERT_GENERATION;
 if(good&&(role>=LEAK_OBSERVER_ROLES||!fn||observer_identity[role].fn!=fn)){good=false;failure=LEAK_CERT_IDENTITY;}
 if(good){unsigned char bytes[LEAK_PREFIX];memcpy(bytes,(const void*)fn,sizeof bytes);
  good=observer_hash(bytes)==observer_identity[role].hash&&!memcmp(bytes,observer_identity[role].bytes,sizeof bytes);
  if(!good)failure=LEAK_CERT_PREFIX;
 }
 extern bool leak_scope_external_is_trusted(struct dvm *) __attribute__((weak));
 if(good&&!dvm_gil_held(v)){good=false;failure=LEAK_CERT_GIL;}
 if(good&&(dispatcher!=(const void*)v->hooks.call_external||!leak_scope_external_is_trusted||!leak_scope_external_is_trusted(v))){good=false;failure=LEAK_CERT_DISPATCH;}
 if(good&&(!m||!m->cls||!m->cls->name||!m->name||!m->sig)){good=false;failure=LEAK_CERT_METHOD;}
 unsigned selector=0;const char *receiver=NULL,*shape=NULL;
 if(good){
  const char *c=m->cls->name;
  if(!strcmp(c,"bitter/jnibridge/a")&&!strcmp(m->name,"invoke")&&
     !strcmp(m->sig,"(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;")&&
     observer_class(v,self,c)&&n==3&&args&&observer_proxy(v,args[0])&&args[0]!=s->array&&args[0]!=s->box&&
     observer_class(v,args[1],"java/lang/reflect/Method")&&args[2]==s->array){
   selector=1;receiver="exact:bitter/jnibridge/a";shape="L(proxy),L(Method),L(candidate-array)";
  }else if(!strcmp(c,"bitter/jnibridge/JNIBridge")&&!strcmp(m->name,"invoke")&&
     !strcmp(m->sig,"(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;")&&
     !self&&n==5&&args&&(args[0]||args[1])&&observer_class(v,args[2],"java/lang/Class")&&
     observer_class(v,args[3],"java/lang/reflect/Method")&&args[4]==s->array){
   selector=2;receiver="static:null";shape="J.low(primitive),J.high(primitive),L(Class),L(Method),L(candidate-array)";
  }else if(!strcmp(c,"java/lang/Long")&&!strcmp(m->name,"longValue")&&!strcmp(m->sig,"()J")&&
     self==s->box&&observer_class(v,self,c)&&n==0){
   selector=3;receiver="exact:candidate-Long";shape="none";
  }else if(!strcmp(c,"java/lang/reflect/Method")&&!strcmp(m->name,"getDeclaringClass")&&
     !strcmp(m->sig,"()Ljava/lang/Class;")&&observer_class(v,self,c)&&
     self!=s->array&&self!=s->box&&n==0){
   selector=4;receiver="exact:noncandidate-Method";shape="none";
  }
 }
 if(!good||!selector){
  if(failure==LEAK_CERT_REJECT_COUNT)failure=LEAK_CERT_SHAPE;
  atomic_fetch_add(&certificate_rejects[failure],1);leak_scope_cancel(v,LEAK_OBSERVER);return false;
 }
 if(cert)*cert=(struct leak_observer_certificate){fn,observer_identity[role].hash,LEAK_PREFIX,role,
  1+role*4+selector-1,m->cls->name,m->name,m->sig,receiver,shape};
 return true;
}
