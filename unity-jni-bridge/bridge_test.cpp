#include "bridge.h"
#include "dvm_provider.h"
#include <jni.h>
#include <cstddef>
#include <cstring>

static_assert(offsetof(JNIInvokeInterface_, AttachCurrentThread)==4*sizeof(void*), "VM ABI");
static_assert(offsetof(JNINativeInterface_, FindClass)==6*sizeof(void*), "FindClass ABI");
static_assert(offsetof(JNINativeInterface_, RegisterNatives)==215*sizeof(void*), "RegisterNatives ABI");
static_assert(offsetof(JNINativeInterface_, GetJavaVM)==219*sizeof(void*), "GetJavaVM ABI");
static_assert(offsetof(JNINativeInterface_, ExceptionCheck)==228*sizeof(void*), "ExceptionCheck ABI");

static int identity;
static void *find(void *,const char *s) { return !strcmp(s,"T") ? &identity : nullptr; }
static int check(void *,void *c,const char *n,const char *s) { return c==&identity && !strcmp(n,"m") && !strcmp(s,"()V"); }
static void method() {}

/* Returns zero on success. Does not load or call any game library. */
extern "C" int jb_self_test(struct dvm *dvm) {
 jb_provider p{nullptr,find,check}; jb_vm *a=jb_create(&p), *b=jb_create(&p); int result=0; bool attached=false;
 JavaVM *vm=(JavaVM*)jb_java_vm(a), *other=(JavaVM*)jb_java_vm(b); JNIEnv *e=nullptr,*e2=nullptr; jclass c=nullptr;
 JNINativeMethod good{(char*)"m",(char*)"()V",(void*)method};
 JNINativeMethod batch[]{good,{(char*)"bad",(char*)"()V",(void*)method}};
 do {
 if(!a||!b) {result=1;break;}
 if(vm->AttachCurrentThread((void**)&e,nullptr)!=JNI_OK || !e) {result=2;break;} attached=true;
 if(other->AttachCurrentThread((void**)&e2,nullptr)==JNI_OK) {result=3;break;}
 if(vm->GetEnv((void**)&e2,JNI_VERSION_1_6)!=JNI_OK || e!=e2) {result=4;break;}
 c=e->FindClass("T"); if(!c || c!=e->FindClass("T")) {result=5;break;}
 if(e->RegisterNatives(c,batch,2)==JNI_OK || !e->ExceptionCheck() || jb_lookup_native(a,&identity,"m","()V")) {result=6;break;}
 e->ExceptionClear(); if(e->RegisterNatives(c,&good,1)!=JNI_OK || jb_lookup_native(a,&identity,"m","()V")!=(void*)method) {result=7;break;}
 if(e->FindClass("missing") || !e->ExceptionCheck()) {result=8;break;}
 e->ExceptionClear(); if(jb_destroy(a)==0) {a=nullptr;attached=false;result=9;break;}
 if(dvm) { jb_provider real=jb_dvm_provider(dvm); void *klass=real.find_class(real.context,"com/unity3d/player/NativeLoader"); if(!klass) {result=11;break;}
   if(!real.is_native_method(real.context,klass,"load","(Ljava/lang/String;)Z") || !real.is_native_method(real.context,klass,"unload","()Z")) {result=12;break;}
   if(real.is_native_method(real.context,klass,"load","()Z") || real.is_native_method(real.context,klass,"absent","()Z")) {result=13;break;}
 }
 } while(false);
 if(attached && vm->DetachCurrentThread()!=JNI_OK && !result) result=10;
 if(a && jb_destroy(a)!=0 && !result) result=10;
 if(b && jb_destroy(b)!=0 && !result) result=10;
 return result;
}
