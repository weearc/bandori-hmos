#include "enum_runtime.h"
#include "java_basics.h"
#include <limits.h>
#include <stdio.h>
int sdk_trace_fprintf(FILE*,const char*,...);
bool foundation_next_selftest(struct dvm *v) {
 bool ok=true;unsigned checks=0;union dvm_value out={0};
#define CHECK(x) do{++checks;if(!(x)||dvm_exception(v)){ok=false;sdk_trace_fprintf(stderr,"FOUNDATION_SELFTEST_FAIL line=%d\n",__LINE__);dvm_clear_exception(v);}}while(0)
 struct dvm_class *base=dvm_find_class_or_external(v,"java/lang/Enum");
 struct dvm_class *type=dvm_find_class_or_external(v,"sdktrace/tests/FoundationEnum");
 if(!base||!type)return false;
 type->super=base;
 dvm_ref first=dvm_new_object(v,type),second=dvm_new_object(v,type);
 union dvm_value args[2]={{.l=dvm_new_string(v,"FIRST")},{.i=0}};
 CHECK(first&&second&&args[0].l);
 CHECK(enum_runtime_call(v,"java/lang/Enum","<init>","(Ljava/lang/String;I)V",first,args,2,&out));
 CHECK(enum_runtime_call(v,"java/lang/Enum","name","()Ljava/lang/String;",first,NULL,0,&out)&&out.l==args[0].l);
 CHECK(enum_runtime_call(v,"java/lang/Enum","ordinal","()I",first,NULL,0,&out)&&out.i==0);
 args[0].l=dvm_new_string(v,"SECOND");args[1].i=7;
 CHECK(enum_runtime_call(v,"java/lang/Enum","<init>","(Ljava/lang/String;I)V",second,args,2,&out));
 args[0].l=second;
 CHECK(enum_runtime_call(v,"java/lang/Enum","compareTo","(Ljava/lang/Enum;)I",first,args,1,&out)&&out.i==-7);
 CHECK(enum_runtime_call(v,"java/lang/Enum","equals","(Ljava/lang/Object;)Z",first,args,1,&out)&&!out.i);
 CHECK(enum_runtime_call(v,"java/lang/Enum","getDeclaringClass","()Ljava/lang/Class;",first,NULL,0,&out)&&out.l==dvm_class_object(v,type));
 args[0].l=0;bool rejected=enum_runtime_call(v,"java/lang/Enum","compareTo","(Ljava/lang/Enum;)I",first,args,1,&out)&&dvm_exception(v);
 ++checks;if(!rejected)ok=false;
 dvm_clear_exception(v);
 struct dvm_class *integer=dvm_find_class_or_external(v,"java/util/concurrent/atomic/AtomicInteger");
 dvm_ref atomic=integer?dvm_new_object(v,integer):0;args[0].i=INT_MAX;
 CHECK(atomic&&java_basics_call(v,"java/util/concurrent/atomic/AtomicInteger","<init>","(I)V",atomic,args,1,&out));
 CHECK(java_basics_call(v,"java/util/concurrent/atomic/AtomicInteger","getAndIncrement","()I",atomic,NULL,0,&out)&&out.i==INT_MAX);
 CHECK(java_basics_call(v,"java/util/concurrent/atomic/AtomicInteger","get","()I",atomic,NULL,0,&out)&&out.i==INT_MIN);
 args[0].i=INT_MIN;args[1].i=23;
 CHECK(java_basics_call(v,"java/util/concurrent/atomic/AtomicInteger","compareAndSet","(II)Z",atomic,args,2,&out)&&out.i);
 CHECK(java_basics_call(v,"java/util/concurrent/atomic/AtomicInteger","getAndDecrement","()I",atomic,NULL,0,&out)&&out.i==23);
 CHECK(java_basics_call(v,"java/util/concurrent/atomic/AtomicInteger","get","()I",atomic,NULL,0,&out)&&out.i==22);
 sdk_trace_fprintf(stderr,"FOUNDATION_SELFTEST %s checks=%u enum_state=1 atomic_integer_overflow=1\n",ok?"PASS":"FAIL",checks);
#undef CHECK
 return ok;
}
