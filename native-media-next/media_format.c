#include "media_format.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <float.h>
#include <math.h>

#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>

#define MEDIA_FORMAT_CAPACITY 64u

typedef struct {
    struct OH_AVFormat *format;
    uintptr_t token;
    uint64_t generation;
    bool has_android_color,has_android_stride;
    int32_t android_color,android_stride;
    bool pcm_encoding_set, pcm_encoding_invalid;
    int32_t pcm_encoding;
} media_format_entry;

static pthread_mutex_t g_media_format_mutex = PTHREAD_MUTEX_INITIALIZER;
static media_format_entry g_media_formats[MEDIA_FORMAT_CAPACITY];
static uint64_t g_next_generation = 1;
#define MEDIA_FORMAT_TOKEN_TAG UINT64_C(0xF100000000000000)
#define MEDIA_FORMAT_GENERATION_MAX UINT64_C(0x0000FFFFFFFFFFFF)

static bool android_video_key(const char *key)
{
    return key&&(!strcmp(key,"color-format")||!strcmp(key,"stride"));
}

static const char *map_key(const char *key)
{
    if (key == NULL) return NULL;
    if (strcmp(key, "channel-count") == 0) return OH_MD_KEY_AUD_CHANNEL_COUNT;
    if (strcmp(key, "sample-rate") == 0) return OH_MD_KEY_AUD_SAMPLE_RATE;
    if (strcmp(key, "mime") == 0) return OH_MD_KEY_CODEC_MIME;
    if (strcmp(key, "durationUs") == 0) return OH_MD_KEY_DURATION;
    if (strcmp(key, "width") == 0) return OH_MD_KEY_WIDTH;
    if (strcmp(key, "height") == 0) return OH_MD_KEY_HEIGHT;
    if (strcmp(key, "frame-rate") == 0) return OH_MD_KEY_FRAME_RATE;
    /* Android color-format uses a different enum; OH stride is explicitly
     * bytes. Preserve explicitly stored Android keys but do not expose OH
     * pixel/stride values as if a video conversion had been implemented. */
    if (strcmp(key, "language") == 0) return OH_MD_KEY_LANGUAGE;
    return key;
}

static media_format_entry *find_locked(void *handle)
{
    uintptr_t token = (uintptr_t)handle;
    if (token == 0) return NULL;
    for (size_t i = 0; i < MEDIA_FORMAT_CAPACITY; ++i) {
        if (g_media_formats[i].format != NULL && g_media_formats[i].token == token)
            return &g_media_formats[i];
    }
    return NULL;
}

void *media_format_wrap(struct OH_AVFormat *owned_format)
{
    if (owned_format == NULL) return NULL;
    if (sizeof(uintptr_t) != sizeof(uint64_t)) {
        OH_AVFormat_Destroy(owned_format);
        return NULL;
    }
    pthread_mutex_lock(&g_media_format_mutex);
    size_t free_slot = MEDIA_FORMAT_CAPACITY;
    for (size_t i = 0; i < MEDIA_FORMAT_CAPACITY; ++i) {
        if (g_media_formats[i].format == NULL) { free_slot = i; break; }
    }
    if (free_slot == MEDIA_FORMAT_CAPACITY) {
        pthread_mutex_unlock(&g_media_format_mutex);
        OH_AVFormat_Destroy(owned_format);
        return NULL;
    }
    if (g_next_generation > MEDIA_FORMAT_GENERATION_MAX) {
        pthread_mutex_unlock(&g_media_format_mutex);
        OH_AVFormat_Destroy(owned_format);
        return NULL;
    }
    uint64_t generation = g_next_generation++;
    /* Tagged integer token only: never dereference it or expose OH_AVFormat. */
    uintptr_t token = (uintptr_t)(MEDIA_FORMAT_TOKEN_TAG |
        (generation << 8) | (uint64_t)(free_slot + 1));
    g_media_formats[free_slot].format = owned_format;
    g_media_formats[free_slot].generation = generation;
    g_media_formats[free_slot].token = token;
    g_media_formats[free_slot].has_android_color=false;
    g_media_formats[free_slot].has_android_stride=false;
    g_media_formats[free_slot].pcm_encoding_set=false;
    g_media_formats[free_slot].pcm_encoding_invalid=false;
    pthread_mutex_unlock(&g_media_format_mutex);
    return (void *)token;
}

android_media_status_t android_media_format_delete(void *handle)
{
    pthread_mutex_lock(&g_media_format_mutex);
    media_format_entry *entry = find_locked(handle);
    if (entry == NULL) {
        pthread_mutex_unlock(&g_media_format_mutex);
        return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
    }
    struct OH_AVFormat *format = entry->format;
    entry->format = NULL;
    entry->token = 0;
    entry->generation = 0;
    OH_AVFormat_Destroy(format);
    pthread_mutex_unlock(&g_media_format_mutex);
    return ANDROID_MEDIA_OK;
}

bool android_media_format_get_float(void *handle, const char *key, float *out)
{
    if (key == NULL || out == NULL) return false;
    if(android_video_key(key))return false;
    pthread_mutex_lock(&g_media_format_mutex);
    media_format_entry *entry = find_locked(handle);
    const char *oh_key = map_key(key);
    bool ok = entry != NULL && OH_AVFormat_GetFloatValue(entry->format, oh_key, out);
    if (!ok && entry != NULL && strcmp(oh_key, OH_MD_KEY_FRAME_RATE) == 0) {
        double frame_rate;
        if (OH_AVFormat_GetDoubleValue(entry->format, oh_key, &frame_rate) &&
            isfinite(frame_rate) && frame_rate <= FLT_MAX && frame_rate >= -FLT_MAX) {
            *out = (float)frame_rate;
            ok = true;
        }
    }
    pthread_mutex_unlock(&g_media_format_mutex);
    return ok;
}

bool android_media_format_get_int32(void *handle, const char *key, int32_t *out)
{
    if (key == NULL || out == NULL) return false;
    pthread_mutex_lock(&g_media_format_mutex);
    media_format_entry *entry = find_locked(handle);
    if(android_video_key(key)) {
        bool color=!strcmp(key,"color-format");
        bool ok=entry&&(color?entry->has_android_color:entry->has_android_stride);
        if(ok)*out=color?entry->android_color:entry->android_stride;
        pthread_mutex_unlock(&g_media_format_mutex);return ok;
    }
    if (!strcmp(key, "pcm-encoding")) {
        int32_t sample;
        if (entry && entry->pcm_encoding_invalid) { pthread_mutex_unlock(&g_media_format_mutex); return false; }
        bool ok = entry && OH_AVFormat_GetIntValue(entry->format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, &sample);
        if (ok) {
            switch (sample) {
                case SAMPLE_S16LE: *out=2; break;
                case SAMPLE_U8: *out=3; break;
                case SAMPLE_F32LE: *out=4; break;
                case SAMPLE_S24LE: *out=21; break;
                case SAMPLE_S32LE: *out=22; break;
                default: ok=false; break;
            }
        }
        pthread_mutex_unlock(&g_media_format_mutex); return ok;
    }
    bool ok = entry != NULL && OH_AVFormat_GetIntValue(entry->format, map_key(key), out);
    pthread_mutex_unlock(&g_media_format_mutex);
    return ok;
}

bool android_media_format_get_int64(void *handle, const char *key, int64_t *out)
{
    if (key == NULL || out == NULL) return false;
    if(android_video_key(key))return false;
    pthread_mutex_lock(&g_media_format_mutex);
    media_format_entry *entry = find_locked(handle);
    bool ok = entry != NULL && OH_AVFormat_GetLongValue(entry->format, map_key(key), out);
    pthread_mutex_unlock(&g_media_format_mutex);
    return ok;
}

bool android_media_format_get_string(void *handle, const char *key, const char **out)
{
    if (key == NULL || out == NULL) return false;
    if(android_video_key(key))return false;
    pthread_mutex_lock(&g_media_format_mutex);
    media_format_entry *entry = find_locked(handle);
    bool ok = entry != NULL && OH_AVFormat_GetStringValue(entry->format, map_key(key), out);
    pthread_mutex_unlock(&g_media_format_mutex);
    return ok;
}

void android_media_format_set_int32(void *handle, const char *key, int32_t value)
{
    if (key == NULL) return;
    pthread_mutex_lock(&g_media_format_mutex);
    media_format_entry *entry = find_locked(handle);
    if (entry == NULL) {
        fprintf(stderr, "android_media_format_set_int32: invalid format handle\n");
    } else if(android_video_key(key)) {
        /* OH happens to use these same key strings. Keep caller-supplied
         * Android annotations separate; no pixel enum/stride conversion yet. */
        if(!strcmp(key,"color-format")){entry->has_android_color=true;entry->android_color=value;}
        else {entry->has_android_stride=true;entry->android_stride=value;}
    } else if (!strcmp(key, "pcm-encoding")) {
        int32_t sample;
        switch(value) {
            case 2: sample=SAMPLE_S16LE; break;
            case 3: sample=SAMPLE_U8; break;
            case 4: sample=SAMPLE_F32LE; break;
            case 21: sample=SAMPLE_S24LE; break;
            case 22: sample=SAMPLE_S32LE; break;
            default:
                entry->pcm_encoding_invalid=true;
                fprintf(stderr, "android_media_format_set_int32: unsupported PCM encoding %d\n", value);
                pthread_mutex_unlock(&g_media_format_mutex); return;
        }
        if (OH_AVFormat_SetIntValue(entry->format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, sample)) {
            entry->pcm_encoding_set=true; entry->pcm_encoding=value; entry->pcm_encoding_invalid=false;
        }
    } else if (!OH_AVFormat_SetIntValue(entry->format, map_key(key), value)) {
        fprintf(stderr, "android_media_format_set_int32: OH_AVFormat rejected key '%s'\n", key);
    }
    pthread_mutex_unlock(&g_media_format_mutex);
}

struct OH_AVFormat *media_format_clone_for_audio(void *handle)
{
    pthread_mutex_lock(&g_media_format_mutex);
    media_format_entry *entry=find_locked(handle);
    if (!entry || entry->pcm_encoding_invalid) { pthread_mutex_unlock(&g_media_format_mutex); return NULL; }
    const char *mime=NULL;
    if (!OH_AVFormat_GetStringValue(entry->format, OH_MD_KEY_CODEC_MIME, &mime) ||
        mime == NULL || strncmp(mime, "audio/", 6) != 0) {
        pthread_mutex_unlock(&g_media_format_mutex); return NULL;
    }
    struct OH_AVFormat *copy=OH_AVFormat_Create();
    if (!copy) { pthread_mutex_unlock(&g_media_format_mutex); return NULL; }
    /* Copy preserves backend audio metadata, including AAC codec_config buffers.
     * Android color/stride annotations live only in this registry sidecar. */
    bool ok=OH_AVFormat_Copy(copy, entry->format);
    pthread_mutex_unlock(&g_media_format_mutex);
    if (!ok) { OH_AVFormat_Destroy(copy); return NULL; }
    return copy;
}




