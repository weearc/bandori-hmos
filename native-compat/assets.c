/* ARM64 extracted-asset backend. Does not parse APKs or manufacture JNI objects. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#ifdef GAME_ASSET_PRIVATE_BACKEND
#include "native_asset_backend_names.h"
#endif
#include "assets.h"
/* Installed once by libprobe before the VM starts. A returned fd is owned by
 * the caller. Do not fall back to stale files from a different APK when set. */
static int (*apk_asset_open)(const char*);
void sdk_apk_set_asset_open(int (*open_asset)(const char*)){apk_asset_open=open_asset;}
int sdk_apk_open_asset(const char* name){return apk_asset_open?apk_asset_open(name):-1;}
int sdk_apk_asset_source_ready(void){return apk_asset_open!=NULL;}
struct game_asset_manager { int root; };
struct game_asset { int fd; off_t length,position; void* mapping; };
_Static_assert(sizeof(off_t)==8,"Only ARM64/64-bit file offsets supported");

AAssetManager* game_assets_create(const char* root) {
    if(!root) return NULL;
    int fd=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return NULL;
    AAssetManager* manager=malloc(sizeof(*manager));
    if(!manager) { close(fd); return NULL; }
    manager->root=fd; return manager;
}
void game_assets_destroy(AAssetManager* manager) {
    if(manager) {close(manager->root); free(manager);}
}
/* Walk each component beneath the supplied root; never follow symlinks. */
static int asset_fd(AAssetManager* manager,const char* name) {
    if(!manager || !name || !*name || *name=='/' || strchr(name,'\\')) return -1;
    char* path=strdup(name); if(!path) return -1;
    int directory=fcntl(manager->root,F_DUPFD_CLOEXEC,0), result=-1;
    if(directory<0) {free(path);return -1;}
    char* component=path;
    for(;;) {
        char* next=strchr(component,'/'); if(next) *next=0;
        if(!*component || !strcmp(component,".") || !strcmp(component,"..")) break;
        int fd=openat(directory,component,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|(next?O_DIRECTORY:0));
        if(fd<0) break;
        if(!next) {result=fd;break;}
        close(directory);directory=fd;component=next+1;
    }
    close(directory);free(path);return result;
}
AAsset* AAssetManager_open(AAssetManager* manager,const char* name,int mode) {
    if(mode<0 || mode>3) return NULL;
    int fd=apk_asset_open?apk_asset_open(name):asset_fd(manager,name); if(fd<0) return NULL;
    struct stat info;
    if(fstat(fd,&info)<0 || !S_ISREG(info.st_mode) || info.st_size<0) {close(fd);return NULL;}
    AAsset* asset=calloc(1,sizeof(*asset));
    if(!asset) {close(fd);return NULL;}
    asset->fd=fd;asset->length=info.st_size;return asset;
}
int AAsset_read(AAsset* asset,void* output,size_t count) {
    if(!asset || (!output && count)) return -1;
    if(count>(size_t)INT_MAX) count=INT_MAX;
    off_t remaining=asset->length-asset->position;
    if(count>(uint64_t)remaining) count=(size_t)remaining;
    if(!count) return 0;
    ssize_t n; do {n=pread(asset->fd,output,count,asset->position);} while(n<0 && errno==EINTR);
    if(n>0) asset->position+=n;
    return (int)n;
}
off_t AAsset_seek64(AAsset* asset,off_t offset,int whence) {
    if(!asset) return -1;
    off_t base;
    switch(whence) {case SEEK_SET:base=0;break;case SEEK_CUR:base=asset->position;break;case SEEK_END:base=asset->length;break;default:return -1;}
    if(offset < -base || offset > asset->length-base) return -1;
    asset->position=base+offset;return asset->position;
}
off_t AAsset_seek(AAsset* asset,off_t offset,int whence) {return AAsset_seek64(asset,offset,whence);}
off_t AAsset_getLength64(AAsset* asset) {return asset?asset->length:-1;}
off_t AAsset_getLength(AAsset* asset) {return AAsset_getLength64(asset);}
off_t AAsset_getRemainingLength64(AAsset* asset) {return asset?asset->length-asset->position:-1;}
off_t AAsset_getRemainingLength(AAsset* asset) {return AAsset_getRemainingLength64(asset);}
int AAsset_openFileDescriptor64(AAsset* asset,off_t* start,off_t* length) {
    if(!asset || !start || !length) return -1;
    int fd=fcntl(asset->fd,F_DUPFD_CLOEXEC,0);
    if(fd>=0) {*start=0;*length=asset->length;} return fd;
}
int AAsset_openFileDescriptor(AAsset* asset,off_t* start,off_t* length) {return AAsset_openFileDescriptor64(asset,start,length);}
const void* AAsset_getBuffer(AAsset* asset) {
    if(!asset) return NULL;
    if(!asset->length) return "";
    if(!asset->mapping) {
        void* buffer=mmap(NULL,(size_t)asset->length,PROT_READ,MAP_PRIVATE,asset->fd,0);
        if(buffer==MAP_FAILED) return NULL;
        asset->mapping=buffer;
    }
    return asset->mapping;
}
int AAsset_isAllocated(AAsset* asset) {(void)asset;return 0;}
void AAsset_close(AAsset* asset) {
    if(!asset) return;
    if(asset->mapping) munmap(asset->mapping,(size_t)asset->length);
    close(asset->fd);free(asset);
}


