#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Generated table includes names imported by all 16 original game libraries.
   This probe opens platform libraries only; it never opens game SOs. */
#include "symbols.inc"
static char report[1000000];
__attribute__((visibility("default")))
const char *unity_batch_run(void) {
 size_t used=(size_t)snprintf(report,sizeof(report),"BUILD UNITY_BATCH_V2\nBEGIN_STAGE platform_dlsym_only\n");
 const char *libs[]={"libc.so","libdl.so","libm.so","liblog.so","libandroid.so","libEGL.so","libGLESv2.so","libOpenSLES.so","libmediandk.so","libstdc++.so","libz.so"};
 void *handles[sizeof(libs)/sizeof(libs[0])]={0};
 for(size_t i=0;i<sizeof(libs)/sizeof(libs[0]);i++){
  dlerror(); handles[i]=dlopen(libs[i],RTLD_NOW|RTLD_LOCAL);
  const char* error=dlerror();
  used+=(size_t)snprintf(report+used,sizeof(report)-used,"PLATFORM %s available=%d error=%s\n",libs[i],handles[i]!=NULL,error?error:"");
 }
 int total=0,found=0,weak=0;
 for(size_t l=0;l<game_lib_count;l++){int n=0,ok=0,w=0; for(size_t s=0;s<symbol_count;s++) if(!strcmp(symbols[s].lib,game_libs[l])){n++;total++; if(symbols[s].weak){weak++;w++;continue;} int visible=0; for(size_t h=0;h<sizeof(handles)/sizeof(handles[0]);h++)if(handles[h]){dlerror();if(dlsym(handles[h],symbols[s].name)){ok++;found++;visible=1;break;}} if(!visible && used+2048<sizeof(report)) used+=(size_t)snprintf(report+used,sizeof(report)-used,"MISSING_SYMBOL %s %s\n",symbols[s].lib,symbols[s].name); } used+=(size_t)snprintf(report+used,sizeof(report)-used,"LIBRARY %s imports=%d weak=%d strong_dlsym_visible=%d not_visible=%d\n",game_libs[l],n,w,ok,n-w-ok); }
 for(size_t i=0;i<sizeof(handles)/sizeof(handles[0]);i++)if(handles[i])dlclose(handles[i]);
 used+=(size_t)snprintf(report+used,sizeof(report)-used,"RESULT imports=%d strong_visible=%d weak=%d strong_not_visible=%d\nEND_STAGE\nBOUNDARY names visible in selected platform handles only; game libraries not loaded; versions, ABI, JNI and Unity startup unverified\n",total,found,weak,total-found-weak);
 return report;
}
