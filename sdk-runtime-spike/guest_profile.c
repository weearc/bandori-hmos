/* Explicit guest-platform declaration. Nothing is inferred from HarmonyOS's
 * host API level. Metadata is bound to original APK-generated public facts. */
#include "guest_profile.h"
#include "guest_identity.h"
#include "portable.h"
#include <stdio.h>
#include <stdlib.h>

#define STR_(x) #x
#define STR(x) STR_(x)
struct entry {const char *name,*expected;};
static const struct entry fields[]={
 {"schema","1"},{"profile",NULL},{"android_api",NULL},
 {"package",GUEST_EXPECTED_PACKAGE},{"process",GUEST_EXPECTED_PROCESS},
 {"apk_sha256",GUEST_EXPECTED_APK_SHA256},{"manifest_sha256",GUEST_EXPECTED_MANIFEST_SHA256},
 {"cert_md5",GUEST_EXPECTED_CERT_MD5},{"cert_sha256",GUEST_EXPECTED_CERT_SHA256},
 {"min_sdk",STR(GUEST_EXPECTED_MIN_SDK)},{"target_sdk",STR(GUEST_EXPECTED_TARGET_SDK)},
 {"compile_sdk",STR(GUEST_EXPECTED_COMPILE_SDK)},{"version_code",STR(GUEST_EXPECTED_VERSION_CODE)},
 {"version_name",GUEST_EXPECTED_VERSION_NAME}};
enum{N_FIELDS=sizeof fields/sizeof fields[0]};
struct profile{bool loaded;int api;char id[64];};
static struct profile active;
static bool parse(const char *source,struct profile *out,char *why,size_t why_size){
 char data[4096];size_t size=strlen(source);if(size>=sizeof data){snprintf(why,why_size,"profile exceeds 4095 bytes");return false;}
 memcpy(data,source,size+1);bool seen[N_FIELDS]={0};char values[N_FIELDS][256];
 for(char *line=data;line;){char *next=strchr(line,'\n');if(next)*next++=0;
  size_t n=strlen(line);if(n&&line[n-1]=='\r')line[--n]=0;
  if(n&&line[0]!='#'){
   char *eq=strchr(line,'=');if(!eq||eq==line||!eq[1]){snprintf(why,why_size,"malformed or empty profile entry");return false;}*eq++=0;
   size_t i=0;while(i<N_FIELDS&&strcmp(fields[i].name,line))++i;
   if(i==N_FIELDS||seen[i]){snprintf(why,why_size,"unknown or duplicate profile key: %.64s",line);return false;}
   if(strlen(eq)>=sizeof values[i]){snprintf(why,why_size,"profile value too long");return false;}
   strcpy(values[i],eq);seen[i]=true;
  }line=next;
 }
 for(size_t i=0;i<N_FIELDS;++i){
  if(!seen[i]){snprintf(why,why_size,"missing profile key: %s",fields[i].name);return false;}
  if(fields[i].expected&&strcmp(values[i],fields[i].expected)){snprintf(why,why_size,"profile not bound to original APK: %s",fields[i].name);return false;}
 }
 const struct{const char *id,*api;} table[]={
  {"android-api24-experimental","24"},{"android-api28-experimental","28"},{"android-api33-experimental","33"}};
 bool selected=false;for(size_t i=0;i<sizeof table/sizeof table[0];++i)if(!strcmp(values[1],table[i].id)&&!strcmp(values[2],table[i].api)){out->api=atoi(table[i].api);strcpy(out->id,table[i].id);selected=true;break;}
 if(!selected){snprintf(why,why_size,"unknown or inconsistent experimental API profile");return false;}
 out->loaded=true;return true;
}
bool guest_profile_load(const char *directory){
 memset(&active,0,sizeof active);char path[1024],data[4096],why[256];
 int n=directory?snprintf(path,sizeof path,"%s/guest-profile.cfg",directory):-1;
 if(n<0||(size_t)n>=sizeof path){sdk_trace_fprintf(stderr,"PROFILE_BLOCKED invalid directory\n");return false;}
 FILE *f=fopen(path,"rb");if(!f){sdk_trace_fprintf(stderr,"PROFILE_BLOCKED required file missing: %s\n",path);return false;}
 size_t bytes=fread(data,1,sizeof data-1,f);bool valid=!ferror(f)&&feof(f);fclose(f);
 if(!valid||memchr(data,0,bytes)){sdk_trace_fprintf(stderr,"PROFILE_BLOCKED unreadable, oversized or binary configuration\n");return false;}data[bytes]=0;
 if(!parse(data,&active,why,sizeof why)){sdk_trace_fprintf(stderr,"PROFILE_BLOCKED %s\n",why);return false;}
 sdk_trace_fprintf(stderr,"GUEST_PROFILE marker=guest-profile-v4 selected=%s android_sdk_int=%d host_harmony_api_not_used=1 complete_android_support=0\n",active.id,active.api);
 sdk_trace_fprintf(stderr,"PROFILE_REASON API28 default is within APK min24/target33 and first public Application.getProcessName baseline; explicit API24/API33 profiles explore original version branches, never imply complete platform support\n");
 sdk_trace_fprintf(stderr,"GUEST_IDENTITY package=%s process=%s apk_min=%d apk_target=%d apk_compile=%d version=%s(%d)\n",GUEST_EXPECTED_PACKAGE,GUEST_EXPECTED_PROCESS,GUEST_EXPECTED_MIN_SDK,GUEST_EXPECTED_TARGET_SDK,GUEST_EXPECTED_COMPILE_SDK,GUEST_EXPECTED_VERSION_NAME,GUEST_EXPECTED_VERSION_CODE);
 sdk_trace_fprintf(stderr,"GUEST_PROVENANCE apk_sha256=%s manifest_sha256=%s cert_md5=%s cert_sha256=%s\n",GUEST_EXPECTED_APK_SHA256,GUEST_EXPECTED_MANIFEST_SHA256,GUEST_EXPECTED_CERT_MD5,GUEST_EXPECTED_CERT_SHA256);
 sdk_trace_fprintf(stderr,"PROFILE_LIMITS public_certificate_inventory_only=1 whole_apk_signature_verified=0 authentication_implemented=0 unknown_android_services=strict_failure no_login_or_risk_control_result_synthesized=1\n");
 return true;
}
bool guest_profile_static(struct dvm *v,const char *c,const char *f,const char *t,union dvm_value *out){
 (void)v;if(!active.loaded)return false;
 if(!strcmp(c,"android/os/Build$VERSION")&&!strcmp(f,"SDK_INT")&&!strcmp(t,"I")){out->i=active.api;return true;}return false;
}
bool guest_profile_call(struct dvm *v,const char *c,const char *m,const char *s,union dvm_value *out){
 if(!active.loaded)return false;
 if(active.api>=28&&!strcmp(c,"android/app/Application")&&!strcmp(m,"getProcessName")&&!strcmp(s,"()Ljava/lang/String;")){out->l=dvm_new_string(v,GUEST_EXPECTED_PROCESS);return true;}
 return false;
}
bool guest_profile_selftest(struct dvm *v,unsigned *checks){
 bool okay=true;char why[256],changed[4096];struct profile p={0};union dvm_value result={0};
#define CHECK(expr) do{++*checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL profile line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 CHECK(parse(GUEST_PROFILE_CANONICAL_CFG,&p,why,sizeof why)&&p.api==28);
 strcpy(changed,GUEST_PROFILE_CANONICAL_CFG);char *id24=strstr(changed,"api28-experimental"),*sdk24=strstr(changed,"android_api=28");if(id24)id24[4]='4';if(sdk24)sdk24[13]='4';CHECK(parse(changed,&p,why,sizeof why)&&p.api==24);
 strcpy(changed,GUEST_PROFILE_CANONICAL_CFG);char *id33=strstr(changed,"api28-experimental"),*sdk33=strstr(changed,"android_api=28");if(id33){id33[3]='3';id33[4]='3';}if(sdk33){sdk33[12]='3';sdk33[13]='3';}CHECK(parse(changed,&p,why,sizeof why)&&p.api==33);
 CHECK(!parse("schema=1\n",&p,why,sizeof why));
 snprintf(changed,sizeof changed,"%sunknown=1\n",GUEST_PROFILE_CANONICAL_CFG);CHECK(!parse(changed,&p,why,sizeof why));
 snprintf(changed,sizeof changed,"%sschema=1\n",GUEST_PROFILE_CANONICAL_CFG);CHECK(!parse(changed,&p,why,sizeof why));
 strcpy(changed,GUEST_PROFILE_CANONICAL_CFG);char *api=strstr(changed,"android_api=28");if(api)api[12]='9';CHECK(!parse(changed,&p,why,sizeof why));
 strcpy(changed,GUEST_PROFILE_CANONICAL_CFG);char *package=strstr(changed,"package=");if(package)package[8]='X';CHECK(!parse(changed,&p,why,sizeof why));
 CHECK(active.loaded&&guest_profile_static(v,"android/os/Build$VERSION","SDK_INT","I",&result)&&result.i==active.api);
 CHECK(!guest_profile_static(v,"android/os/Build$VERSION","UNKNOWN","I",&result));
 if(active.api>=28){CHECK(guest_profile_call(v,"android/app/Application","getProcessName","()Ljava/lang/String;",&result));CHECK(!strcmp(dvm_string_utf8(v,result.l),GUEST_EXPECTED_PROCESS));}
 else CHECK(!guest_profile_call(v,"android/app/Application","getProcessName","()Ljava/lang/String;",&result));
 sdk_trace_fprintf(stderr,"PROFILE_SELFTEST %s marker=guest-profile-v4 explicit_config=1 metadata_bound=1 unknown_profile_rejected=1\n",okay?"PASS":"FAIL");return okay;
}
