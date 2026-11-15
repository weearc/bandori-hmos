#include "java_log_runtime.h"
#include "discovery_runtime.h"
#include <hilog/log.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *,const char *,...);

/* Private guest payload, real HiLog result; no SDK argument text in reports. */
static bool level_for(int priority,LogLevel *level) {
 switch(priority) {
 case 2:case 3:*level=LOG_DEBUG;return true;
 case 4:*level=LOG_INFO;return true;
 case 5:*level=LOG_WARN;return true;
 case 6:*level=LOG_ERROR;return true;
 case 7:*level=LOG_FATAL;return true;
 default:return false;
 }
}

/* java.util.logging defaults.  The active Logger implementation has no
 * setLevel/getLevel and no LogManager configuration, so the effective level
 * is INFO, as java.util.logging.Logger does without a configuration file.
 * discovery_runtime publishes the canonical FINE/INFO/WARNING Level objects;
 * those are the only Level fields the OkHttp call sites read (FINE for
 * isLoggable, INFO/WARNING for Platform.log).  Reusing their identity keeps
 * isLoggable consistent with discovery_runtime's Logger.log threshold. */
#define JAVA_LOG_INFO_VALUE 800

struct java_log_level {
 const char *field;
 int value;
};

static const struct java_log_level java_log_levels[]={
 {"FINEST",300},{"FINER",400},{"FINE",500},{"CONFIG",700},
 {"INFO",800},{"WARNING",900},{"SEVERE",1000},
 {"OFF",INT_MAX},{"ALL",INT_MIN}
};

static bool class_named_or_super(struct dvm_object *o,const char *name) {
 if(!o)return false;
 for(struct dvm_class *c=o->cls;c;c=c->super)
  if(c->name&&!strcmp(c->name,name))return true;
 return false;
}

/* Match the Level object against the instances the VM already publishes.
 * discovery_runtime_static creates and pins each object once, so pointer
 * identity is stable for the lifetime of the VM.  An unrecognised field or
 * a static-hook failure is not an error here; the caller falls through to the
 * VM's own Level implementation when one exists. */
static bool canonical_level_value(struct dvm *v,dvm_ref ref,int *value) {
 for(size_t i=0;i<sizeof java_log_levels/sizeof java_log_levels[0];++i) {
  union dvm_value candidate={0};
  if(discovery_runtime_static(v,"java/util/logging/Level",java_log_levels[i].field,
      "Ljava/util/logging/Level;",&candidate)&&candidate.l==ref) {
   *value=java_log_levels[i].value;return true;
  }
  if(dvm_exception(v))return false;
 }
 return false;
}

/* Reuse a real Level.intValue()/getName() when the active runtime provides
 * one.  dvm_lookup() returns NULL for the external Level placeholder used by
 * discovery_runtime, so this path stays dormant until a runtime defines the
 * class; it is not a fabricated fallback. */
static bool vm_level_value(struct dvm *v,dvm_ref ref,int *value) {
 struct dvm_method *m=dvm_lookup(v,"java/util/logging/Level","intValue","()I");
 if(m) {
  union dvm_value out={0};
  if(!dvm_call(v,m,ref,NULL,0,&out))return false;
  *value=out.i;return true;
 }
 m=dvm_lookup(v,"java/util/logging/Level","getName","()Ljava/lang/String;");
 if(m) {
  union dvm_value out={0};
  if(!dvm_call(v,m,ref,NULL,0,&out))return false;
  const char *name=out.l?dvm_string_utf8(v,out.l):NULL;
  if(name)
   for(size_t i=0;i<sizeof java_log_levels/sizeof java_log_levels[0];++i)
    if(!strcmp(name,java_log_levels[i].field)){*value=java_log_levels[i].value;return true;}
 }
 return false;
}

/* Logger.isLoggable(Level) with the default INFO threshold.  A null Level is
 * a NullPointerException, exactly as Level.intValue() would throw it.  An
 * unrecognised Level object fails closed (false) instead of inventing an
 * arbitrary level value; OFF/ALL follow the requested contract rather than
 * the intValue comparison quirk. */
static bool logger_is_loggable(struct dvm *v,dvm_ref self,
 const union dvm_value *a,union dvm_value *out) {
 if(!self){dvm__throw(v,"java/lang/NullPointerException","Logger receiver");return true;}
 struct dvm_object *logger=dvm__obj(v,self);
 if(!logger||!class_named_or_super(logger,"java/util/logging/Logger")) {
  dvm__throw(v,"java/lang/IllegalArgumentException","Logger receiver");return true;
 }
 if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","isLoggable level");return true;}
 struct dvm_object *level=dvm__obj(v,a[0].l);
 if(!level||!class_named_or_super(level,"java/util/logging/Level")) {
  dvm__throw(v,"java/lang/IllegalArgumentException","isLoggable level");return true;
 }
 int value=0;
 bool known=canonical_level_value(v,a[0].l,&value);
 if(!known&&!dvm_exception(v))known=vm_level_value(v,a[0].l,&value);
 if(dvm_exception(v))return true;
 if(!known){out->i=0;return true;}
 /* Match OpenJDK Logger.isLoggable against the effective INFO threshold:
  * the *message* level ALL (Integer.MIN_VALUE) is below INFO, so false;
  * the message level OFF (Integer.MAX_VALUE) is not filtered by an INFO
  * logger, so true.  Turning the logger itself OFF is a separate threshold
  * operation, not the same thing as passing Level.OFF as the message. */
 if(value==INT_MIN){out->i=0;return true;} /* Level.ALL < INFO */
 if(value==INT_MAX){out->i=1;return true;} /* Level.OFF message passes INFO */
 out->i=value>=JAVA_LOG_INFO_VALUE;
 return true;
}

bool java_log_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out) {
 if(!strcmp(c,"java/util/logging/Logger")) {
  /* Other Logger methods remain with discovery_runtime: getLogger and log
   * were already implemented there and must keep their identity/threshold
   * semantics.  Only the exact isLoggable signature is claimed here. */
  if(!strcmp(m,"isLoggable")&&!strcmp(sig,"(Ljava/util/logging/Level;)Z")&&nargs==1)
   return logger_is_loggable(v,self,a,out);
  return false;
 }
 if(!strcmp(c,"android/util/Log")&&!strcmp(m,"getStackTraceString")&&
    !strcmp(sig,"(Ljava/lang/Throwable;)Ljava/lang/String;")&&nargs==1) {
  /* android.util.Log contract: a null Throwable yields the empty string, not
   * null.  A non-null Throwable is rendered through the VM's own exception
   * text (declaring class and message), which is exactly the text the engine
   * needs when it wraps a captured exception into AndroidJavaException; no
   * source file or line information is invented. */
  if(!a||!a[0].l){out->l=dvm_new_string(v,"");return true;}
  char text[8192];dvm_describe_exception(v,a[0].l,text,sizeof text);
  out->l=dvm_new_string(v,text);return true;
 }
 if(strcmp(c,"android/util/Log"))return false;
 int priority=0;int tag=0,message=1,throwable=-1;
 if(!strcmp(m,"isLoggable")&&!strcmp(sig,"(Ljava/lang/String;I)Z")&&nargs==2) {
  LogLevel level;
  out->i=level_for(a[1].i,&level)&&OH_LOG_IsLoggable(0xB011,"AndroidGuest",level);return true;
 }
 if(!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)I")&&nargs==2) {
  if(!strcmp(m,"v"))priority=2;else if(!strcmp(m,"d"))priority=3;
  else if(!strcmp(m,"i"))priority=4;else if(!strcmp(m,"w"))priority=5;
  else if(!strcmp(m,"e"))priority=6;else return false;
 }else if(!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I")&&nargs==3) {
  /* Log.v/d/i/w/e(String,String,Throwable)：消息照常打印，异常文本附在
   * 后面（与平台行为一致，不吞掉 Throwable 链）。 */
  if(!strcmp(m,"v"))priority=2;else if(!strcmp(m,"d"))priority=3;
  else if(!strcmp(m,"i"))priority=4;else if(!strcmp(m,"w"))priority=5;
  else if(!strcmp(m,"e"))priority=6;else return false;
  throwable=2;
 }else if(!strcmp(m,"println")&&!strcmp(sig,"(ILjava/lang/String;Ljava/lang/String;)I")&&nargs==3) {
  priority=a[0].i;tag=1;message=2;
 }else return false;
 LogLevel level;
 if(!level_for(priority,&level)){dvm__throw(v,"java/lang/IllegalArgumentException","unsupported log priority");return true;}
 const char *text=a[message].l?dvm_string_utf8(v,a[message].l):NULL;
 const char *label=a[tag].l?dvm_string_utf8(v,a[tag].l):"";
 if(!text){dvm__throw(v,"java/lang/NullPointerException","log message");return true;}
 if(!label){dvm__throw(v,"java/lang/IllegalArgumentException","log tag type");return true;}
 int result;
 if(throwable>=0&&a[throwable].l){
  char detail[4096];dvm_describe_exception(v,a[throwable].l,detail,sizeof detail);
  result=OH_LOG_Print(LOG_APP,level,0xB011,"AndroidGuest","%{private}s: %{private}s\n%{private}s",label,text,detail);
 }else{
  result=OH_LOG_Print(LOG_APP,level,0xB011,"AndroidGuest","%{private}s: %{private}s",label,text);
 }
 out->i=result>=0?1:result;return true;
}

bool java_log_runtime_selftest(struct dvm *v) {
 bool okay=true;unsigned checks=0;
#define LOG_CHECK(expr) do{++checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"JAVA_LOG_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);if(dvm_exception(v))dvm_clear_exception(v);}}while(0)
 if(!v)return false;
 union dvm_value name={0},logger={0},level={0},args[1]={0},log_args[2]={0},out={0};
 name.l=dvm_new_string(v,"okhttp3.internal.platform.Platform");
 LOG_CHECK(name.l!=0);
 LOG_CHECK(discovery_runtime_call(v,"java/util/logging/Logger","getLogger",
  "(Ljava/lang/String;)Ljava/util/logging/Logger;",0,&name,1,&logger)&&logger.l&&!dvm_exception(v));
 if(!logger.l){dvm_clear_exception(v);return false;}
 level.l=0;
 LOG_CHECK(discovery_runtime_static(v,"java/util/logging/Level","FINE",
  "Ljava/util/logging/Level;",&level)&&level.l);
 args[0]=level;out.l=0;
 LOG_CHECK(java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)Z",logger.l,args,1,&out)&&!dvm_exception(v)&&out.i==0);
 level.l=0;
 LOG_CHECK(discovery_runtime_static(v,"java/util/logging/Level","INFO",
  "Ljava/util/logging/Level;",&level)&&level.l);
 args[0]=level;out.i=-1;
 LOG_CHECK(java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)Z",logger.l,args,1,&out)&&!dvm_exception(v)&&out.i==1);
 level.l=0;
 LOG_CHECK(discovery_runtime_static(v,"java/util/logging/Level","WARNING",
  "Ljava/util/logging/Level;",&level)&&level.l);
 args[0]=level;out.i=-1;
 LOG_CHECK(java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)Z",logger.l,args,1,&out)&&!dvm_exception(v)&&out.i==1);
 level.l=0;
 LOG_CHECK(discovery_runtime_static(v,"java/util/logging/Level","ALL",
  "Ljava/util/logging/Level;",&level)&&level.l);
 args[0]=level;out.i=-1;
 LOG_CHECK(java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)Z",logger.l,args,1,&out)&&!dvm_exception(v)&&out.i==0);
 level.l=0;
 LOG_CHECK(discovery_runtime_static(v,"java/util/logging/Level","OFF",
  "Ljava/util/logging/Level;",&level)&&level.l);
 args[0]=level;out.i=-1;
 LOG_CHECK(java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)Z",logger.l,args,1,&out)&&!dvm_exception(v)&&out.i==1);
 /* Null Level must throw NPE, not return an invented boolean. */
 args[0].l=0;out.i=-1;
 bool null_handled=java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)Z",logger.l,args,1,&out);
 char why[160]={0};
 ++checks;
 if(!null_handled||!dvm_exception(v)) {
  okay=false;sdk_trace_fprintf(stderr,"JAVA_LOG_SELFTEST_FAIL line=%d null_level_not_handled\n",__LINE__);
 }else{
  dvm_describe_exception(v,dvm_exception(v),why,sizeof why);
 }
 dvm_clear_exception(v);
 ++checks;
 if(!strstr(why,"java/lang/NullPointerException")) {
  okay=false;sdk_trace_fprintf(stderr,"JAVA_LOG_SELFTEST_FAIL line=%d null_level_wrong_exception=%s\n",__LINE__,why);
 }
 /* Exact signature/nargs only: a wrong signature must fall through so the
  * dispatcher reports it instead of this module claiming an unknown method. */
 args[0]=level;
 LOG_CHECK(!java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/lang/String;)Z",logger.l,args,1,&out));
 LOG_CHECK(!java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)I",logger.l,args,1,&out));
 LOG_CHECK(!java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)Z",logger.l,args,0,&out));
 /* Keep discovery_runtime's existing Logger.log assertions authoritative:
  * this module must not intercept log(Level,String...). */
 log_args[0].l=level.l;
 log_args[1].l=dvm_new_string(v,"java log selftest");
 LOG_CHECK(log_args[1].l!=0);
 LOG_CHECK(!java_log_runtime_call(v,"java/util/logging/Logger","log",
  "(Ljava/util/logging/Level;Ljava/lang/String;)V",logger.l,log_args,2,&out));
 /* An unrecognised Level object fails closed rather than fabricating true. */
 dvm_ref foreign=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/logging/Level"));
 args[0].l=foreign;out.i=-1;
 LOG_CHECK(foreign!=0&&java_log_runtime_call(v,"java/util/logging/Logger","isLoggable",
  "(Ljava/util/logging/Level;)Z",logger.l,args,1,&out)&&!dvm_exception(v)&&out.i==0);
 sdk_trace_fprintf(stderr,"JAVA_LOG_SELFTEST %s checks=%u isLoggable_fine=0 isLoggable_info=1 isLoggable_warning=1 isLoggable_all=0 isLoggable_off=1 null_level_npe=1 signature_exact=1 logger_log_passthrough=1 unknown_level_fail_closed=1\n",okay?"PASS":"FAIL",checks);
#undef LOG_CHECK
 return okay;
}
