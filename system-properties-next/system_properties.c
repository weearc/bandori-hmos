#include "system_properties.h"
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
bool system_properties_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;
 /* The diagnostic guest profile installs no SecurityManager, matching the
  * default Android process. Thread factories then take the current thread's
  * ThreadGroup branch instead of SecurityManager.getThreadGroup(). */
 if(!strcmp(c,"java/lang/System")&&!strcmp(m,"getSecurityManager")&&!strcmp(sig,"()Ljava/lang/SecurityManager;")&&n==0){out->l=0;return true;}
 if(strcmp(c,"java/lang/System")||strcmp(m,"getProperty"))return false;
 bool defaults=!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
 if((!defaults&&strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;"))||n!=(defaults?2:1))return false;
 const char *key=dvm_string_utf8(v,a[0].l);
 if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","property key");return true;}
 if(!key)return false;size_t length=dvm_string_utf8_length(v,a[0].l);
 if(!length){dvm__throw(v,"java/lang/IllegalArgumentException","empty property key");return true;}
 /* java.vm.name still identifies the actual DEX interpreter. java.vm.version is
  * the multi-DEX capability version AndroidX MultiDex parses; this VM loads all
  * three original DEX files itself, so its legacy secondary-dex installer must
  * stay disabled rather than attempt a fake classloader injection. */
 /* Android exposes java.version="0" rather than a desktop JDK release.
  * Libraries must receive a non-null version string on this Android guest. */
 static const char *names[]={"java.vm.name","java.vm.vendor","java.vm.version","file.separator","path.separator","line.separator","java.version"};
 static const char *values[]={"Lunaria","Lunaria","2.1.0","/",":","\n","0"};
 for(unsigned i=0;i<sizeof names/sizeof names[0];i++)if(length==strlen(names[i])&&!memcmp(key,names[i],length)){out->l=dvm_new_string(v,values[i]);return true;}
 out->l=defaults?a[1].l:0;return true;
}
bool system_properties_selftest(struct dvm *v){
 union dvm_value a[2]={{.l=dvm_new_string(v,"java.vm.name")},{.l=dvm_new_string(v,"fallback")}},out={0};
 if(!system_properties_call(v,"java/lang/System","getProperty","(Ljava/lang/String;)Ljava/lang/String;",0,a,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"Lunaria"))return false;
 a[0].l=dvm_new_string(v,"sdktrace.undefined.property");
 union dvm_value version_key={.l=dvm_new_string(v,"java.version")};
 if(!system_properties_call(v,"java/lang/System","getProperty","(Ljava/lang/String;)Ljava/lang/String;",0,&version_key,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"0"))return false;
 if(!system_properties_call(v,"java/lang/System","getProperty","(Ljava/lang/String;)Ljava/lang/String;",0,a,1,&out)||out.l)return false;
 if(!system_properties_call(v,"java/lang/System","getProperty","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,a,2,&out)||out.l!=a[1].l)return false;
 a[0].l=0;if(!system_properties_call(v,"java/lang/System","getProperty","(Ljava/lang/String;)Ljava/lang/String;",0,a,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 a[0].l=dvm_new_string(v,"");if(!system_properties_call(v,"java/lang/System","getProperty","(Ljava/lang/String;)Ljava/lang/String;",0,a,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"SYSTEM_PROPERTIES_SELFTEST PASS actual_vm_identity=1 absent_null=1 default_identity=1 null_empty_rejected=1\n");return true;
}
