#include "native_asset_bridge.h"
#include "jni_bridge.h"
#include "dvm/dvm_internal.h"
#include "asset_reader.h"
#define GAME_ASSET_PRIVATE_BACKEND 1
#include "native_asset_backend_names.h"
#include "assets.h"
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

extern bool asset_reader_is_manager(struct dvm *, dvm_ref);
extern int sdk_trace_fprintf(FILE *,const char *,...);
static void asset_probe_log(const char *fmt,...){
    FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
    va_list a;va_start(a,fmt);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fclose(f);
}
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static struct dvm *owner;
static AAssetManager *manager;
typedef struct asset_handle { AAsset *asset; struct asset_handle *next; } asset_handle;
static asset_handle *handles;
static _Atomic unsigned asset_log_count;
bool native_asset_bridge_configure(struct dvm *vm,const char *root) {
    if (!vm || !root) return false;
    AAssetManager *m=game_assets_create(root); if(!m){sdk_trace_fprintf(stderr,"NATIVE_ASSET root_unavailable path=%s\n",root);return false;}
    AAsset *metadata=AAssetManager_open(m,"bin/Data/Managed/Metadata/global-metadata.dat",2);
    off_t size=metadata?AAsset_getLength64(metadata):-1;
    if(metadata)AAsset_close(metadata);
    sdk_trace_fprintf(stderr,"NATIVE_ASSET metadata_file_bytes=%lld expected=29633644 root=%s\n",(long long)size,root);
    if(size!=29633644){game_assets_destroy(m);return false;}
    pthread_mutex_lock(&guard);
    if(owner) { pthread_mutex_unlock(&guard); game_assets_destroy(m); return false; }
    owner=vm; manager=m; pthread_mutex_unlock(&guard);
    sdk_trace_fprintf(stderr,"NATIVE_ASSET configured process-lifetime root=%s\n",root); return true;
}
static void *from_java(JNIEnv *env,jobject obj) {
    struct jni_bridge *b=jni_bridge_for_env(env); if(!b)return NULL;
    struct dvm *vm=jni_bridge_vm(b); bool own=!dvm_gil_held(vm); if(own)dvm_gil_acquire(vm);
    dvm_ref ref=0; bool valid=(jni_bridge_resolve_object(b,obj,JNI_REF_LOCAL,&ref)||jni_bridge_resolve_object(b,obj,JNI_REF_GLOBAL,&ref)) && ref && asset_reader_is_manager(vm,ref);
    pthread_mutex_lock(&guard); void *result=(valid && vm==owner)?manager:NULL; pthread_mutex_unlock(&guard);
    sdk_trace_fprintf(stderr,"NATIVE_ASSET fromJava receiver=%u manager_valid=%d configured_owner=%d result=%p\n",(unsigned)ref,valid,vm==owner,result);
    asset_probe_log("AAssetManager_fromJava receiver=%u manager_valid=%d owner_match=%d result=%p",(unsigned)ref,valid?1:0,vm==owner?1:0,(void*)result);
    if(own)dvm_gil_release(vm); return result;
}
static asset_handle *lookup(void *p) { for(asset_handle *h=handles;h;h=h->next) if(h==p)return h; return NULL; }
static void *open_asset(void *m,const char *name,int mode) {
    pthread_mutex_lock(&guard); AAsset *a=m==manager && manager?AAssetManager_open(manager,name,mode):NULL;
    asset_handle *h=a?malloc(sizeof(*h)):NULL; if(a&&!h)AAsset_close(a);
    if(h){h->asset=a;h->next=handles;handles=h;} pthread_mutex_unlock(&guard);
    if(atomic_fetch_add(&asset_log_count,1)<128){sdk_trace_fprintf(stderr,"NATIVE_ASSET open name=%s mode=%d result=%p\n",name?name:"(null)",mode,(void*)h);asset_probe_log("AAsset_open name=%s mode=%d result=%p",name?name:"(null)",mode,(void*)h);}
    return h;
}
static void close_asset(void *p) { pthread_mutex_lock(&guard);asset_handle *h=lookup(p);if(h&&h->asset){AAsset_close(h->asset);h->asset=NULL;}pthread_mutex_unlock(&guard); }
#define WRAP(ret,name,args,call,fail) static ret name args { pthread_mutex_lock(&guard);asset_handle *h=lookup(p);ret r=(h&&h->asset)?(call):(fail);pthread_mutex_unlock(&guard);return r; }
static _Atomic unsigned asset_read_log_count;
static int read_asset(void *p,void *buf,size_t n){pthread_mutex_lock(&guard);asset_handle *h=lookup(p);int r=(h&&h->asset)?AAsset_read(h->asset,buf,n):-1;pthread_mutex_unlock(&guard);
 if(atomic_fetch_add(&asset_read_log_count,1)<256)asset_probe_log("AAsset_read handle=%p bytes=%zu result=%d",p,n,r);return r;}
WRAP(off_t,seek_asset,(void *p,off_t off,int whence),AAsset_seek64(h->asset,off,whence),-1)
WRAP(off_t,length_asset,(void *p),AAsset_getLength64(h->asset),-1)
WRAP(off_t,remaining_asset,(void *p),AAsset_getRemainingLength64(h->asset),-1)
WRAP(int,fd_asset,(void *p,off_t *start,off_t *length),AAsset_openFileDescriptor64(h->asset,start,length),-1)
WRAP(const void *,buffer_asset,(void *p),AAsset_getBuffer(h->asset),NULL)
WRAP(int,allocated_asset,(void *p),AAsset_isAllocated(h->asset),0)
static void *open_dir(void *m,const char *name){(void)m;(void)name;return NULL;}
static const char *next_dir(void *d){(void)d;return NULL;}
static void void_dir(void *d){(void)d;}
void *native_asset_bridge_symbol(const char *name) {
    if(!name)return NULL;
#define MAP(n,f) if(!strcmp(name,n))return (void *)(f)
    MAP("AAssetManager_fromJava",from_java); MAP("AAssetManager_open",open_asset);
    MAP("AAsset_close",close_asset); MAP("AAsset_read",read_asset);
    MAP("AAsset_seek",seek_asset); MAP("AAsset_seek64",seek_asset);
    MAP("AAsset_getLength",length_asset); MAP("AAsset_getLength64",length_asset);
    MAP("AAsset_getRemainingLength",remaining_asset); MAP("AAsset_getRemainingLength64",remaining_asset);
    MAP("AAsset_openFileDescriptor",fd_asset); MAP("AAsset_openFileDescriptor64",fd_asset);
    MAP("AAsset_getBuffer",buffer_asset); MAP("AAsset_isAllocated",allocated_asset);
    MAP("AAssetManager_openDir",open_dir); MAP("AAssetDir_getNextFileName",next_dir);
    MAP("AAssetDir_rewind",void_dir); MAP("AAssetDir_close",void_dir);
#undef MAP
    return NULL;
}

