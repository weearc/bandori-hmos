#ifndef GAME_ASSETS_H
#define GAME_ASSETS_H
#include <stddef.h>
#include <sys/types.h>
typedef struct game_asset_manager AAssetManager;
typedef struct game_asset AAsset;
AAssetManager *game_assets_create(const char *root);
void game_assets_destroy(AAssetManager *manager);
AAsset *AAssetManager_open(AAssetManager *, const char *, int);
int AAsset_read(AAsset *, void *, size_t);
off_t AAsset_seek(AAsset *, off_t, int);
off_t AAsset_seek64(AAsset *, off_t, int);
off_t AAsset_getLength(AAsset *);
off_t AAsset_getLength64(AAsset *);
off_t AAsset_getRemainingLength(AAsset *);
off_t AAsset_getRemainingLength64(AAsset *);
int AAsset_openFileDescriptor(AAsset *, off_t *, off_t *);
int AAsset_openFileDescriptor64(AAsset *, off_t *, off_t *);
const void *AAsset_getBuffer(AAsset *);
int AAsset_isAllocated(AAsset *);
void AAsset_close(AAsset *);
#endif
