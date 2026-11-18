#include "platform_runtime.h"
#include "gc_operation.h"
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

typedef struct { regex_t regex; int compiled; int capture_count; } pattern_data;
typedef struct { struct dvm *vm; dvm_ref pattern; dvm_ref input; size_t next;
                 regmatch_t groups[4]; int ngroups; int matched; } matcher_data;
static struct dvm *active;
static dvm_ref gmt, utc;
static const char java_ip_regex[] = "([0-9a-fA-F]*:[0-9a-fA-F:.]*)|([\\d.]+)";
static const char posix_ip_regex[] = "([0-9a-fA-F]*:[0-9a-fA-F:.]*)|([0-9.]+)";
/* AndroidX MultiDex.isVMMultidexCapable() compiles this exact expression to
 * decide whether the interpreter can load several DEX files itself. */
static const char java_vm_version_regex[] = "(\\d+)\\.(\\d+)(\\.\\d+)?";
static const char posix_vm_version_regex[] = "([0-9]+)\\.([0-9]+)(\\.[0-9]+)?";

static bool fail(struct dvm *v, const char *type) { dvm__throw(v,type,"platform runtime"); return true; }
static dvm_ref object(struct dvm *v,const char *name) { return dvm_new_object(v,dvm_find_class_or_external(v,name)); }
static void pattern_free(void *p) { pattern_data *d=p; if(d){if(d->compiled)regfree(&d->regex);free(d);} }
static void matcher_free(void *p) { matcher_data *d=p; if(d){dvm_unpin(d->vm,d->pattern);dvm_unpin(d->vm,d->input);free(d);} }
bool platform_runtime_vm_start(struct dvm *v) { active=v;gmt=utc=0;return true; }
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
static bool platform_runtime_call_step8_operation(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out) {
 if(v!=active)return false;
 if(!strcmp(c,"java/util/TimeZone")) {
  if(IS("getTimeZone","(Ljava/lang/String;)Ljava/util/TimeZone;")&&n==1){
   if(!a[0].l)return fail(v,"java/lang/NullPointerException");
   const char *id=dvm_string_utf8(v,a[0].l);size_t len=dvm_string_utf8_length(v,a[0].l);
   if(!id||strlen(id)!=len||((len!=3||strcmp(id,"GMT"))&&(len!=3||strcmp(id,"UTC"))))return fail(v,"java/lang/UnsupportedOperationException");
   dvm_ref *slot=!strcmp(id,"GMT")?&gmt:&utc;if(!*slot)*slot=object(v,c);out->l=*slot;return true;
  }
  if(self&&(self==gmt||self==utc)){
   if(IS("getID","()Ljava/lang/String;")){out->l=dvm_new_string(v,self==gmt?"GMT":"UTC");return true;}
   if(IS("getRawOffset","()I")){out->i=0;return true;}
  }
  return false;
 }
 if(!strcmp(c,"java/util/regex/Pattern")) {
  if((IS("compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;")&&n==1)||(IS("compile","(Ljava/lang/String;I)Ljava/util/regex/Pattern;")&&n==2)){
   if(!a[0].l)return fail(v,"java/lang/NullPointerException");
   if(n==2&&a[1].i)return fail(v,"java/lang/UnsupportedOperationException");
   const char *expr=dvm_string_utf8(v,a[0].l);size_t len=dvm_string_utf8_length(v,a[0].l);
   if(!expr||strlen(expr)!=len)return fail(v,"java/lang/UnsupportedOperationException");
   const char *posix=NULL;int captures=0;
   if(!strcmp(expr,java_ip_regex)){posix=posix_ip_regex;captures=1;}
   else if(!strcmp(expr,java_vm_version_regex)){posix=posix_vm_version_regex;captures=4;}
   else return fail(v,!strcmp(expr,"[")?"java/util/regex/PatternSyntaxException":"java/lang/UnsupportedOperationException");
   pattern_data *d=calloc(1,sizeof *d);if(!d)return fail(v,"java/lang/OutOfMemoryError");
   d->capture_count=captures;
   if(regcomp(&d->regex,posix,REG_EXTENDED)){free(d);return fail(v,"java/util/regex/PatternSyntaxException");}d->compiled=1;
   dvm_ref ref=object(v,c);struct dvm_object *o=dvm__obj(v,ref);if(!o){pattern_free(d);return fail(v,"java/lang/OutOfMemoryError");}o->data=d;o->data_destroy=pattern_free;out->l=ref;return true;
  }
  if(IS("matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;")&&n==1){
   struct dvm_object *p=dvm__obj(v,self);if(!p||p->data_destroy!=pattern_free)return false;
   if(!a[0].l)return fail(v,"java/lang/NullPointerException");
   if(!dvm_string_utf8(v,a[0].l))return fail(v,"java/lang/UnsupportedOperationException");
   matcher_data *d=calloc(1,sizeof *d);if(!d)return fail(v,"java/lang/OutOfMemoryError");d->vm=v;d->pattern=self;d->input=a[0].l;dvm_pin(v,d->pattern);dvm_pin(v,d->input);
   dvm_ref ref=object(v,"java/util/regex/Matcher");struct dvm_object *o=dvm__obj(v,ref);if(!o){matcher_free(d);return fail(v,"java/lang/OutOfMemoryError");}o->data=d;o->data_destroy=matcher_free;out->l=ref;return true;
  }
  return false;
 }
 if(!strcmp(c,"java/util/regex/Matcher")){
  struct dvm_object *o=dvm__obj(v,self);if(!o||o->data_destroy!=matcher_free)return false;matcher_data *d=o->data;
  struct dvm_object *p=dvm__obj(v,d->pattern);if(!p||p->data_destroy!=pattern_free)return fail(v,"java/lang/IllegalStateException");
  const char *s=dvm_string_utf8(v,d->input);size_t len=dvm_string_utf8_length(v,d->input);
  if(!s||strlen(s)!=len)return fail(v,"java/lang/UnsupportedOperationException");
  pattern_data *pd=p->data;regex_t *re=&pd->regex;regmatch_t match[4];
  if(IS("matches","()Z")){
   int rc=regexec(re,s,(size_t)pd->capture_count,match,0);
   bool ok=rc==0&&match[0].rm_so==0&&(size_t)match[0].rm_eo==len;
   d->matched=ok;d->ngroups=ok?pd->capture_count:0;
   if(ok)for(int i=0;i<pd->capture_count&&i<4;i++)d->groups[i]=match[i];
   out->i=ok;return true;
  }
  if(IS("find","()Z")){
   if(d->next>len){out->i=0;d->matched=0;d->ngroups=0;return true;}
   int rc=regexec(re,s+d->next,(size_t)pd->capture_count,match,d->next?REG_NOTBOL:0);
   out->i=rc==0;
   if(out->i){
    d->matched=1;d->ngroups=pd->capture_count;
    for(int i=0;i<pd->capture_count&&i<4;i++){
     d->groups[i]=match[i];
     if(match[i].rm_so>=0){d->groups[i].rm_so+=(regoff_t)d->next;d->groups[i].rm_eo+=(regoff_t)d->next;}
    }
    d->next+=(size_t)match[0].rm_eo>0?(size_t)match[0].rm_eo:1;
   }else{d->matched=0;d->ngroups=0;d->next=len+1;}
   return true;
  }
  if(IS("group","(I)Ljava/lang/String;")&&n==1){
   if(!d->matched)return fail(v,"java/lang/IllegalStateException");
   int index=a[0].i;
   if(index<0||index>=d->ngroups)return fail(v,"java/lang/IndexOutOfBoundsException");
   regmatch_t g=d->groups[index];
   if(g.rm_so<0||g.rm_eo<g.rm_so){out->l=0;return true;}
   out->l=dvm_new_string_n(v,s+g.rm_so,(size_t)(g.rm_eo-g.rm_so));return true;
  }
  if(IS("reset","()Ljava/util/regex/Matcher;")){d->next=0;d->matched=0;d->ngroups=0;out->l=self;return true;}
 }
 return false;
}
bool platform_runtime_selftest(struct dvm *v){
 union dvm_value out={0},arg[2]={{0},{0}};
 arg[0].l=dvm_new_string(v,"GMT");
 if(!platform_runtime_call(v,"java/util/TimeZone","getTimeZone","(Ljava/lang/String;)Ljava/util/TimeZone;",0,arg,1,&out)||!out.l)return false;
 dvm_ref zone=out.l;
 if(!platform_runtime_call(v,"java/util/TimeZone","getRawOffset","()I",zone,NULL,0,&out)||out.i)return false;
 if(!platform_runtime_call(v,"java/util/TimeZone","getID","()Ljava/lang/String;",zone,NULL,0,&out)||strcmp(dvm_string_utf8(v,out.l),"GMT"))return false;
 arg[0].l=dvm_new_string(v,"UTC");if(!platform_runtime_call(v,"java/util/TimeZone","getTimeZone","(Ljava/lang/String;)Ljava/util/TimeZone;",0,arg,1,&out)||!out.l||out.l==zone)return false;
 arg[0].l=dvm_new_string(v,"Europe/Paris");if(!platform_runtime_call(v,"java/util/TimeZone","getTimeZone","(Ljava/lang/String;)Ljava/util/TimeZone;",0,arg,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 arg[0].l=dvm_new_string(v,java_ip_regex);
 if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref pattern=out.l;
 static const struct {const char *input;int matches,find;} cases[]={ {"2001:db8::1",1,1},{"127.0.0.1",1,1},{"x127.0.0.1y",0,1},{"xyz",0,0} };
 for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
  arg[0].l=dvm_new_string(v,cases[i].input);
  if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",pattern,arg,1,&out)||!out.l)return false;
  dvm_ref matcher=out.l;
  if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",matcher,NULL,0,&out)||out.i!=cases[i].matches)return false;
  if(!platform_runtime_call(v,"java/util/regex/Matcher","find","()Z",matcher,NULL,0,&out)||out.i!=cases[i].find)return false;
 }
 arg[0].l=dvm_new_string(v,java_vm_version_regex);
 if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref version_pattern=out.l;
 arg[0].l=dvm_new_string(v,"2.1.0");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",version_pattern,arg,1,&out)||!out.l)return false;
 dvm_ref version_matcher=out.l;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",version_matcher,NULL,0,&out)||!out.i)return false;
 union dvm_value group={.i=1};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",version_matcher,&group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"2"))return false;
 group.i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",version_matcher,&group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"1"))return false;
 arg[0].l=dvm_new_string(v,"[");if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 arg[0].l=dvm_new_string(v,java_ip_regex);arg[1].i=2;
 if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;I)Ljava/util/regex/Pattern;",0,arg,2,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"PLATFORM_SELFTEST PASS GMT_UTC=1 exact_IP_regex=1 matches_find=1 VM_version_regex_captures=1 unsupported_rejected=1\n");
 return true;
}

bool platform_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active)return false;
 if(strcmp(c,"java/util/TimeZone")&&strcmp(c,"java/util/regex/Pattern")&&strcmp(c,"java/util/regex/Matcher"))
  return platform_runtime_call_step8_operation(v,c,m,sig,self,a,n,out);
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,self))return true;
 bool handled=platform_runtime_call_step8_operation(v,c,m,sig,self,a,n,out);
 if(handled&&!dvm_exception(v))gc_operation_result(&op,sig,out->l);
 return handled;
}
