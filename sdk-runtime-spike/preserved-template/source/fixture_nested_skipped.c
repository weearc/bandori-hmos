/* Small dependency used by caller.c to exercise recursive loader resolution. */
extern int *__errno(void);
#include <string.h>
#define MEDIA_KEY(name, value) extern const char *name;
#include "../bionic_media_keys.inc"
#undef MEDIA_KEY

__attribute__((visibility("default"))) int leaf_media_keys_check(void) {
    int checked = 0;
#define MEDIA_KEY(name, value) do { if (!name || strcmp(name, value)) return -1; ++checked; } while (0);
#include "../bionic_media_keys.inc"
#undef MEDIA_KEY
    return checked;
}
extern const char *_ctype_;
extern int ANativeWindow_getWidth(void*);
extern int ANativeWindow_getHeight(void*);
extern void ANativeWindow_acquire(void*);
extern void ANativeWindow_release(void*);
extern int ANativeWindow_setBuffersGeometry(void*,int,int,int);

__attribute__((visibility("default"))) int leaf_window_check(void* window,int width,int height) {
    if(ANativeWindow_getWidth(window)!=width||ANativeWindow_getHeight(window)!=height)return 1;
    ANativeWindow_acquire(window);
    ANativeWindow_release(window);
    if(ANativeWindow_setBuffersGeometry(window,width,height,0)!=0)return 3;
    if(ANativeWindow_setBuffersGeometry(window,0,height,0)>=0)return 4;
    if(ANativeWindow_setBuffersGeometry(window,width,height,0x7fffffff)>=0)return 5;
    if(ANativeWindow_setBuffersGeometry(window,0,0,0)!=0)return 6;
    return ANativeWindow_getWidth(window)==width?0:2;
}

/* Independent ASCII definition checks every entry through the imported pointer.
 * Bionic's compatibility table has an EOF entry, then unsigned byte + 1. */
__attribute__((visibility("default"))) int leaf_ctype_check(void) {
    if (!_ctype_ || (unsigned char)_ctype_[0] != 0) return 1;
    for (unsigned c = 0; c < 256; ++c) {
        unsigned expected = 0;
        if (c < 32 || c == 127) expected |= 32;
        if (c == 32 || (c >= 9 && c <= 13)) expected |= 8;
        if (c == 32) expected |= 128;
        if (c >= '0' && c <= '9') expected |= 4;
        if (c >= 'A' && c <= 'Z') expected |= 1;
        if (c >= 'a' && c <= 'z') expected |= 2;
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) expected |= 64;
        if (c >= 33 && c <= 126 && !(expected & 7)) expected |= 16;
        if ((unsigned char)_ctype_[c + 1] != expected) return (int)c + 2;
    }
    return 0;
}

static int constructor_count;

static void touch_errno_preserving(void) {
    int *slot = __errno();
    if (slot == 0) {
        return;
    }
    int saved = *slot;
    *slot = 0x4e4c;
    *slot = saved;
}

__attribute__((constructor)) static void leaf_init(void) {
    ++constructor_count;
    touch_errno_preserving();
}

__attribute__((visibility("default"))) int leaf_get_constructor_count(void) {
    touch_errno_preserving();
    return constructor_count;
}

__attribute__((visibility("default"))) int leaf_fixed_value(void) {
    touch_errno_preserving();
    return 0x4e57;
}
