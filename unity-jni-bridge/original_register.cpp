#include "bridge.h"
#include "dvm_provider.h"
extern "C" {
#include "dvm/dvm.h"
}
#include <jni.h>
#include <dlfcn.h>
#include <cstdio>

extern "C" int jb_self_test(struct dvm *);
static thread_local char report[512];

/* Explicit future diagnostic; caller serializes shared DVM arena access. */
extern "C" const char *jni_original_register(const char *dexdir) {
 const char *status="FAIL invalid dex directory"; int dex_count=-1, test=-1, version=0;
 bool attached=false; void *library=nullptr; jb_vm *bridge=nullptr; JavaVM *jvm=nullptr; JNIEnv *env=nullptr;
 struct dvm *dvm=nullptr;
 do {
  if(!dexdir || !*dexdir) break;
  dvm_hooks hooks{}; dvm=dvm_create(&hooks);
  if(!dvm) {status="FAIL dvm_create";break;}
  dex_count=dvm_add_apk_dir(dvm,dexdir);
  if(dex_count!=3) {status="FAIL dex count";break;}
  test=jb_self_test(dvm);
  if(test!=0) {status="FAIL prerequisite self test";break;}
  jb_provider provider=jb_dvm_provider(dvm);
  void *klass=provider.find_class(provider.context,"com/unity3d/player/NativeLoader");
  if(!klass) {status="FAIL class";break;}
  bridge=jb_create(&provider); if(!bridge) {status="FAIL bridge create";break;}
  jvm=(JavaVM *)jb_java_vm(bridge);
  if(jvm->AttachCurrentThread((void**)&env,nullptr)!=JNI_OK || !env) {status="FAIL attach";break;} attached=true;
  library=dlopen("libspike_original_main.so",RTLD_NOW | RTLD_LOCAL);
  if(!library) {status="FAIL dlopen";break;}
  typedef jint (*onload_fn)(JavaVM *,void *);
  auto entry=(onload_fn)dlsym(library,"JNI_OnLoad");
  if(!entry) {status="FAIL dlsym";break;}
  version=entry(jvm,nullptr);
  if(version!=JNI_VERSION_1_6) {status="FAIL JNI version";break;}
  if(jb_pending_exception(bridge)) {status="FAIL pending exception";break;}
  if(!jb_lookup_native(bridge,klass,"load","(Ljava/lang/String;)Z") ||
     !jb_lookup_native(bridge,klass,"unload","()Z")) {status="FAIL registration";break;}
  status="PASS";
 } while(false);
 if(attached && jvm->DetachCurrentThread()!=JNI_OK) status="FAIL detach";
 if(bridge && jb_destroy(bridge)!=0) status="FAIL bridge destroy";
 if(library && dlclose(library)!=0) status="FAIL dlclose";
 if(dvm) dvm_destroy(dvm);
 std::snprintf(report,sizeof(report),"BUILD JNI_ORIGINAL_REGISTER_V1\nDEX_COUNT %d\nSELF_TEST_CODE %d\nJNI_VERSION 0x%x\nRESULT %s\n",dex_count,test,(unsigned)version,status);
 return report;
}
