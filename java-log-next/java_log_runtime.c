#include "java_log_runtime.h"
#include <hilog/log.h>
#include <string.h>

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
bool java_log_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out) {
 (void)self;
 if(strcmp(c,"android/util/Log"))return false;
 int priority=0;int tag=0,message=1;
 if(!strcmp(m,"isLoggable")&&!strcmp(sig,"(Ljava/lang/String;I)Z")&&nargs==2) {
  LogLevel level;
  out->i=level_for(a[1].i,&level)&&OH_LOG_IsLoggable(0xB011,"AndroidGuest",level);return true;
 }
 if(!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)I")&&nargs==2) {
  if(!strcmp(m,"v"))priority=2;else if(!strcmp(m,"d"))priority=3;
  else if(!strcmp(m,"i"))priority=4;else if(!strcmp(m,"w"))priority=5;
  else if(!strcmp(m,"e"))priority=6;else return false;
 }else if(!strcmp(m,"println")&&!strcmp(sig,"(ILjava/lang/String;Ljava/lang/String;)I")&&nargs==3) {
  priority=a[0].i;tag=1;message=2;
 }else return false;
 LogLevel level;
 if(!level_for(priority,&level)){dvm__throw(v,"java/lang/IllegalArgumentException","unsupported log priority");return true;}
 const char *text=a[message].l?dvm_string_utf8(v,a[message].l):NULL;
 const char *label=a[tag].l?dvm_string_utf8(v,a[tag].l):"";
 if(!text){dvm__throw(v,"java/lang/NullPointerException","log message");return true;}
 if(!label){dvm__throw(v,"java/lang/IllegalArgumentException","log tag type");return true;}
 int result=OH_LOG_Print(LOG_APP,level,0xB011,"AndroidGuest","%{private}s: %{private}s",label,text);
 out->i=result>=0?1:result;return true;
}
