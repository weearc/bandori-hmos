#include "sdk_init.h"
#include "collections.h"
#include "android_basics.h"
#include "java_basics.h"
#include "io_runtime.h"
#include "lrucache_runtime.h"
#include "latch_runtime.h"
#include "executor_extras.h"
#include "thread_extras.h"
#include "bundle_runtime.h"
#include "intent_runtime.h"
#include "uri_runtime.h"
#include "content_runtime.h"
#include "network_runtime.h"
#include "sqlite_runtime.h"
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
/* The original SDK uses ConcurrentHashMap for provider/plugin registries. The
 * VM runs guest bytecode under one GIL, so the HashMap implementation provides
 * the same serialized map semantics here. It is not a general concurrent map:
 * host callbacks still copy POD events and never touch guest objects. */
extern void java_basics_reset(struct dvm *v);
extern bool java_basics_wrapper_selftest(struct dvm *v);
extern dvm_ref java_basics_boolean_box(struct dvm *v,int value);
extern bool thread_runtime_stack_selftest(struct dvm *v);
extern void throwable_reference_vm_start(struct dvm *v);
extern void exception_hierarchy_vm_start(struct dvm *v);
void sdk_init_vm_start(struct dvm *v){
 java_basics_reset(v);
 /* The synthesized Throwable/Exception/RuntimeException chain must exist at
  * VM start, not only after TLS initialization: HttpException (and OkHttp's
  * exceptions) construct before any SSL runtime lazily runs its own
  * exception-hierarchy pass.  72bs reached Exception.<init>(Throwable) with
  * java/lang/Exception still parented to Object because of that ordering. */
 throwable_reference_vm_start(v);
 /* Same ordering problem one level down: the java.io/java.net chain
  * (IOException -> SocketException -> ConnectException, and the SSL/security
  * subclasses) is only installed by the ssl/cert runtimes, so an early
  * ConnectException(String) threw NoSuchMethodError before TLS init (72bv). */
 exception_hierarchy_vm_start(v);
 content_runtime_vm_start(v);
 network_runtime_vm_start(v);
 struct dvm_class *chm=dvm_find_class_or_external(v,"java/util/concurrent/ConcurrentHashMap");
 struct dvm_class *hm=dvm_find_class_or_external(v,"java/util/HashMap");
 if(chm&&hm&&chm->external)chm->super=hm;
 /* Router's Warehouse uses UniqueKeyTreeMap for its route indexes. The guest
  * path only needs containsKey/put/get/size; the serialized HashMap backing
  * provides those. Natural ordering is not exposed on this path. */
 struct dvm_class *tree=dvm_find_class_or_external(v,"java/util/TreeMap");
 if(tree&&hm&&tree->external)tree->super=hm;
}
static const char *sdk_init_exception_name(struct dvm *v){
 dvm_ref ref=dvm_exception(v);
 if(!ref)return "none";
 struct dvm_class *cls=dvm_object_class(v,ref);
 if(cls&&cls->name)return cls->name;
 return "unknown";
}

static bool sdk_init_stage_fail(struct dvm *v,const char *stage,
 const char *expected,const char *actual){
 sdk_trace_fprintf(stderr,
  "SDK_INIT_SELFTEST STAGE %s FAIL expected=%s actual=%s exception=%s\n",
  stage,expected,actual,sdk_init_exception_name(v));
 return false;
}

static bool sdk_init_stage_fail_int(struct dvm *v,const char *stage,
 long long expected,long long actual){
 char expected_text[32],actual_text[32];
 snprintf(expected_text,sizeof expected_text,"%lld",expected);
 snprintf(actual_text,sizeof actual_text,"%lld",actual);
 return sdk_init_stage_fail(v,stage,expected_text,actual_text);
}

#define SDK_INIT_CHECK(stage_,expr_) do { \
 if(!(expr_))return sdk_init_stage_fail(v,(stage_),#expr_,"false"); \
} while(0)

#define SDK_INIT_INT(stage_,actual_,expected_) do { \
 long long sdk_init_actual_value_=(long long)(actual_); \
 long long sdk_init_expected_value_=(long long)(expected_); \
 if(sdk_init_actual_value_!=sdk_init_expected_value_) \
  return sdk_init_stage_fail_int(v,(stage_), \
   sdk_init_expected_value_,sdk_init_actual_value_); \
} while(0)

#define SDK_INIT_NONNULL(stage_,ref_) do { \
 if(!(ref_))return sdk_init_stage_fail(v,(stage_),"non_null","null"); \
} while(0)

#define SDK_INIT_SAME(stage_,actual_,expected_) do { \
 dvm_ref sdk_init_actual_ref_=(actual_); \
 dvm_ref sdk_init_expected_ref_=(expected_); \
 if(!sdk_init_actual_ref_||sdk_init_actual_ref_!=sdk_init_expected_ref_) \
  return sdk_init_stage_fail(v,(stage_),"same_reference", \
   sdk_init_actual_ref_?"different_reference":"null"); \
} while(0)

#define SDK_INIT_DISTINCT(stage_,actual_,other_) do { \
 dvm_ref sdk_init_actual_ref_=(actual_); \
 dvm_ref sdk_init_other_ref_=(other_); \
 if(!sdk_init_actual_ref_||sdk_init_actual_ref_==sdk_init_other_ref_) \
  return sdk_init_stage_fail(v,(stage_),"distinct_reference", \
   sdk_init_actual_ref_?"same_reference":"null"); \
} while(0)

typedef struct sdk_init_probe {
 const char *name;
 bool (*run)(struct dvm *);
} sdk_init_probe;

/* Run sub-selftests in order without C-level || short-circuiting. The first
 * failure is reported with the failing stage plus which probes ran and which
 * were skipped; the aggregate still returns false at that point. */
static bool sdk_init_run_group(struct dvm *v,const char *group,
 const sdk_init_probe *probes,size_t count){
 for(size_t i=0;i<count;++i){
  sdk_trace_fprintf(stderr,
   "SDK_INIT_SELFTEST STAGE %s BEGIN group=%s\n",probes[i].name,group);
  if(probes[i].run(v))continue;
  char executed[320],not_executed[320];
  size_t executed_len=0,not_executed_len=0;
  executed[0]=0;
  not_executed[0]=0;
  for(size_t j=0;j<count;++j){
   char *buffer=j<=i?executed:not_executed;
   size_t *used=j<=i?&executed_len:&not_executed_len;
   int written=snprintf(buffer+*used,sizeof executed-*used,"%s%s",
    *used?",":"",probes[j].name);
   if(written>0&&(size_t)written<sizeof executed-*used)
    *used+=(size_t)written;
  }
  sdk_trace_fprintf(stderr,
   "SDK_INIT_SELFTEST STAGE %s FAIL group=%s expected=true actual=false "
   "exception=%s executed=%s not_executed=%s\n",
   probes[i].name,group,sdk_init_exception_name(v),executed,not_executed);
  return false;
 }
 return true;
}

bool sdk_init_selftest(struct dvm *v){
 sdk_trace_fprintf(stderr,"SDK_INIT_SELFTEST BEGIN\n");
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE begin\n");
 union dvm_value out={0},ignored={0};
 dvm_ref map=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/ConcurrentHashMap"));
 SDK_INIT_NONNULL("chm_object",map);
 SDK_INIT_CHECK("chm_ctor",
  collections_call(v,"java/util/concurrent/ConcurrentHashMap","<init>","()V",map,NULL,0,&ignored));
 SDK_INIT_CHECK("chm_ctor_exception",dvm_exception(v)==0);
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE chm_ctor\n");
 union dvm_value key={.l=dvm_new_string(v,"sdk-init")},value={.l=dvm_new_string(v,"registered")};
 union dvm_value pair[2]={key,value};
 SDK_INIT_CHECK("chm_put",
  collections_call(v,"java/util/concurrent/ConcurrentHashMap","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,pair,2,&out));
 SDK_INIT_CHECK("chm_put_exception",dvm_exception(v)==0);
 SDK_INIT_CHECK("chm_get",
  collections_call(v,"java/util/concurrent/ConcurrentHashMap","get","(Ljava/lang/Object;)Ljava/lang/Object;",map,&pair[0],1,&out));
 SDK_INIT_SAME("chm_get_value",out.l,value.l);
 SDK_INIT_CHECK("chm_contains_key",
  collections_call(v,"java/util/concurrent/ConcurrentHashMap","containsKey","(Ljava/lang/Object;)Z",map,&pair[0],1,&out));
 SDK_INIT_INT("chm_contains_key_value",out.i,1);
 SDK_INIT_CHECK("chm_size",
  collections_call(v,"java/util/concurrent/ConcurrentHashMap","size","()I",map,NULL,0,&out));
 SDK_INIT_INT("chm_size_value",out.i,1);
 SDK_INIT_CHECK("chm_assignable",
  dvm__class_assignable(v,dvm_object_class(v,map),dvm_find_class_or_external(v,"java/util/Map")));
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE chm_puts\n");
 dvm_ref tree=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/TreeMap"));
 SDK_INIT_NONNULL("tree_object",tree);
 SDK_INIT_CHECK("tree_ctor",
  collections_call(v,"java/util/TreeMap","<init>","()V",tree,NULL,0,&ignored));
 SDK_INIT_CHECK("tree_ctor_exception",dvm_exception(v)==0);
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE tree_ctor\n");
 union dvm_value tkey={.l=dvm_new_string(v,"tree-key")},tvalue={.l=dvm_new_string(v,"tree-value")},tpair[2]={tkey,tvalue};
 SDK_INIT_CHECK("tree_put",
  collections_call(v,"java/util/TreeMap","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",tree,tpair,2,&out));
 SDK_INIT_CHECK("tree_put_exception",dvm_exception(v)==0);
 SDK_INIT_CHECK("tree_contains_key",
  collections_call(v,"java/util/TreeMap","containsKey","(Ljava/lang/Object;)Z",tree,&tpair[0],1,&out));
 SDK_INIT_INT("tree_contains_key_value",out.i,1);
 SDK_INIT_CHECK("tree_get",
  collections_call(v,"java/util/TreeMap","get","(Ljava/lang/Object;)Ljava/lang/Object;",tree,&tpair[0],1,&out));
 SDK_INIT_SAME("tree_get_value",out.l,tvalue.l);
 SDK_INIT_CHECK("tree_size",
  collections_call(v,"java/util/TreeMap","size","()I",tree,NULL,0,&out));
 SDK_INIT_INT("tree_size_value",out.i,1);
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE tree_puts\n");
 static const sdk_init_probe io_probes[]={
  {"io",io_runtime_selftest},
  {"lrucache",lrucache_runtime_selftest},
  {"latch",latch_runtime_selftest},
  {"executor_extras",executor_extras_selftest},
  {"thread_extras",thread_extras_selftest}
 };
 if(!sdk_init_run_group(v,"io",
  io_probes,sizeof io_probes/sizeof io_probes[0]))return false;
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE io_selftests\n");
 dvm_ref text=dvm_new_string(v,"abc");
 SDK_INIT_NONNULL("charsequence_text",text);
 SDK_INIT_CHECK("charsequence_length_call",
  android_basics_call(v,"java/lang/CharSequence","length","()I",text,NULL,0,&out));
 SDK_INIT_INT("charsequence_length_value",out.i,3);
 SDK_INIT_CHECK("charsequence_isempty_call",
  android_basics_call(v,"java/lang/CharSequence","isEmpty","()Z",text,NULL,0,&out));
 SDK_INIT_INT("charsequence_isempty_value",out.i,0);
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE charsequence\n");
 union dvm_value btrue={0},btrue2={0},bfalse={0},bflag={0};
 SDK_INIT_CHECK("boolean_true_static",
  java_basics_static(v,"java/lang/Boolean","TRUE","Ljava/lang/Boolean;",&btrue));
 SDK_INIT_NONNULL("boolean_true_object",btrue.l);
 SDK_INIT_CHECK("boolean_true_static_repeat",
  java_basics_static(v,"java/lang/Boolean","TRUE","Ljava/lang/Boolean;",&btrue2));
 SDK_INIT_SAME("boolean_true_canonical_identity",btrue2.l,btrue.l);
 SDK_INIT_CHECK("boolean_false_static",
  java_basics_static(v,"java/lang/Boolean","FALSE","Ljava/lang/Boolean;",&bfalse));
 SDK_INIT_NONNULL("boolean_false_object",bfalse.l);
 SDK_INIT_DISTINCT("boolean_false_distinct",bfalse.l,btrue.l);
 SDK_INIT_CHECK("boolean_true_value_call",
  java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",btrue.l,NULL,0,&bflag));
 SDK_INIT_INT("boolean_true_value",bflag.i,1);
 SDK_INIT_CHECK("boolean_false_value_call",
  java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",bfalse.l,NULL,0,&bflag));
 SDK_INIT_INT("boolean_false_value",bflag.i,0);
 SDK_INIT_SAME("boolean_box_true",java_basics_boolean_box(v,1),btrue.l);
 SDK_INIT_SAME("boolean_box_false",java_basics_boolean_box(v,0),bfalse.l);
 union dvm_value barg={0},bvalue={0};
 barg.i=1;
 SDK_INIT_CHECK("boolean_valueof_true_call",
  java_basics_call(v,"java/lang/Boolean","valueOf","(Z)Ljava/lang/Boolean;",0,&barg,1,&bvalue));
 SDK_INIT_SAME("boolean_valueof_true_identity",bvalue.l,btrue.l);
 barg.i=0;
 SDK_INIT_CHECK("boolean_valueof_false_call",
  java_basics_call(v,"java/lang/Boolean","valueOf","(Z)Ljava/lang/Boolean;",0,&barg,1,&bvalue));
 SDK_INIT_SAME("boolean_valueof_false_identity",bvalue.l,bfalse.l);
 struct dvm_class *bool_cls=dvm_find_class_or_external(v,"java/lang/Boolean");
 dvm_ref constructed=dvm_new_object(v,bool_cls);
 barg.i=1;
 SDK_INIT_NONNULL("boolean_constructor_object",constructed);
 SDK_INIT_DISTINCT("boolean_constructor_distinct_true",constructed,btrue.l);
 SDK_INIT_DISTINCT("boolean_constructor_distinct_false",constructed,bfalse.l);
 SDK_INIT_CHECK("boolean_constructor_init_true",
  java_basics_call(v,"java/lang/Boolean","<init>","(Z)V",constructed,&barg,1,&bvalue));
 SDK_INIT_CHECK("boolean_constructor_value_call",
  java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",constructed,NULL,0,&bvalue));
 SDK_INIT_INT("boolean_constructor_value_true",bvalue.i,1);
 barg.l=btrue.l;
 SDK_INIT_CHECK("boolean_constructor_equals_true_call",
  java_basics_call(v,"java/lang/Boolean","equals","(Ljava/lang/Object;)Z",constructed,&barg,1,&bvalue));
 SDK_INIT_INT("boolean_constructor_equals_true_value",bvalue.i,1);
 dvm_ref constructed_false=dvm_new_object(v,bool_cls);
 barg.i=0;
 SDK_INIT_NONNULL("boolean_constructor_false_object",constructed_false);
 SDK_INIT_DISTINCT("boolean_constructor_false_distinct_true",constructed_false,btrue.l);
 SDK_INIT_DISTINCT("boolean_constructor_false_distinct_false",constructed_false,bfalse.l);
 SDK_INIT_CHECK("boolean_constructor_init_false",
  java_basics_call(v,"java/lang/Boolean","<init>","(Z)V",constructed_false,&barg,1,&bvalue));
 SDK_INIT_CHECK("boolean_constructor_false_value_call",
  java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",constructed_false,NULL,0,&bvalue));
 SDK_INIT_INT("boolean_constructor_value_false",bvalue.i,0);
 barg.l=bfalse.l;
 SDK_INIT_CHECK("boolean_constructor_equals_false_call",
  java_basics_call(v,"java/lang/Boolean","equals","(Ljava/lang/Object;)Z",constructed_false,&barg,1,&bvalue));
 SDK_INIT_INT("boolean_constructor_equals_false_value",bvalue.i,1);
 dvm_ref atomic=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/atomic/AtomicBoolean"));
 barg.i=1;
 SDK_INIT_NONNULL("atomic_boolean_object",atomic);
 SDK_INIT_CHECK("atomic_boolean_init",
  java_basics_call(v,"java/util/concurrent/atomic/AtomicBoolean","<init>","(Z)V",atomic,&barg,1,&bvalue));
 barg.l=atomic;
 SDK_INIT_CHECK("boolean_equals_atomic_true_call",
  java_basics_call(v,"java/lang/Boolean","equals","(Ljava/lang/Object;)Z",btrue.l,&barg,1,&bvalue));
 SDK_INIT_INT("boolean_equals_atomic_true_value",bvalue.i,0);
 SDK_INIT_CHECK("boolean_equals_atomic_constructed_call",
  java_basics_call(v,"java/lang/Boolean","equals","(Ljava/lang/Object;)Z",constructed,&barg,1,&bvalue));
 SDK_INIT_INT("boolean_equals_atomic_constructed_value",bvalue.i,0);
 SDK_INIT_CHECK("atomic_boolean_not_java_basics_equal",
  java_basics_equal(v,btrue.l,atomic)==0);
 SDK_INIT_CHECK("atomic_boolean_get_call",
  java_basics_call(v,"java/util/concurrent/atomic/AtomicBoolean","get","()Z",atomic,NULL,0,&bvalue));
 SDK_INIT_INT("atomic_boolean_get_value",bvalue.i,1);
 /* java_basics_reset is the cache reset a new VM performs. This verifies
  * invalidation inside one VM; it is not two simultaneously live VMs. */
 dvm_ref old_true=btrue.l,old_false=bfalse.l;
 java_basics_reset(v);
 union dvm_value ntrue={0},nfalse={0};
 SDK_INIT_CHECK("boolean_reset_true_static",
  java_basics_static(v,"java/lang/Boolean","TRUE","Ljava/lang/Boolean;",&ntrue));
 SDK_INIT_NONNULL("boolean_reset_true_object",ntrue.l);
 SDK_INIT_DISTINCT("boolean_reset_true_reacquire",ntrue.l,old_true);
 SDK_INIT_CHECK("boolean_reset_false_static",
  java_basics_static(v,"java/lang/Boolean","FALSE","Ljava/lang/Boolean;",&nfalse));
 SDK_INIT_NONNULL("boolean_reset_false_object",nfalse.l);
 SDK_INIT_DISTINCT("boolean_reset_false_reacquire",nfalse.l,old_false);
 SDK_INIT_DISTINCT("boolean_reset_false_distinct_true",nfalse.l,ntrue.l);
 SDK_INIT_SAME("boolean_reset_box_true",java_basics_boolean_box(v,1),ntrue.l);
 SDK_INIT_SAME("boolean_reset_box_false",java_basics_boolean_box(v,0),nfalse.l);
 barg.i=1;
 SDK_INIT_CHECK("boolean_reset_valueof_true_call",
  java_basics_call(v,"java/lang/Boolean","valueOf","(Z)Ljava/lang/Boolean;",0,&barg,1,&bvalue));
 SDK_INIT_SAME("boolean_reset_valueof_true_identity",bvalue.l,ntrue.l);
 SDK_INIT_CHECK("boolean_reset_true_value_call",
  java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",ntrue.l,NULL,0,&bvalue));
 SDK_INIT_INT("boolean_reset_true_value",bvalue.i,1);
 SDK_INIT_CHECK("boolean_reset_false_value_call",
  java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",nfalse.l,NULL,0,&bvalue));
 SDK_INIT_INT("boolean_reset_false_value",bvalue.i,0);
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE boolean\n");
 static const sdk_init_probe wrapper_probes[]={
  {"wrapper",java_basics_wrapper_selftest}
 };
 if(!sdk_init_run_group(v,"java_basics",
  wrapper_probes,sizeof wrapper_probes/sizeof wrapper_probes[0]))return false;
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE wrapper\n");
 static const sdk_init_probe module_probes[]={
  {"bundle",bundle_runtime_selftest},
  {"intent",intent_runtime_selftest},
  {"uri",uri_runtime_selftest},
  {"content",content_runtime_selftest},
  {"network",network_runtime_selftest},
  {"sqlite",sqlite_runtime_selftest}
 };
 if(!sdk_init_run_group(v,"modules",
  module_probes,sizeof module_probes/sizeof module_probes[0]))return false;
 sdk_trace_fprintf(stderr,"SDK_INIT_STAGE thread_stack\n");
 if(!thread_runtime_stack_selftest(v))return false;
 sdk_trace_fprintf(stderr,"SDK_INIT_SELFTEST PASS concurrent_hash_map_serialized=1 map_identity=1 treemap_alias=1 charsequence_length=1 boolean_statics=1 boolean_factory_identity=1 boolean_constructor_value=1 atomic_boolean_distinct=1 boolean_reset_reacquire=1 bundle=1 intent=1 uri=1 content=1 network=1 sqlite=1\n");
 return true;
}
