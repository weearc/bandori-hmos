#define _FILE_OFFSET_BITS 64
#include "media_extractor.h"
#include "media_format.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "native_avbuffer.h"
#include "native_avbuffer_info.h"
#include "native_avdemuxer.h"
#include "native_avsource.h"

#define EXTRACTOR_SLOTS 64
#define SAMPLE_BUFFER_LIMIT (4 * 1024 * 1024)
#define INITIAL_SAMPLE_CAPACITY SAMPLE_BUFFER_LIMIT
#define EXTRACTOR_TOKEN_TAG UINT64_C(0xE100000000000000)
#define EXTRACTOR_GENERATION_MAX UINT64_C(0x0000FFFFFFFFFFFF)

typedef struct {
    uint64_t generation;
    int live;
    OH_AVSource *source;
    OH_AVDemuxer *demuxer;
    int owned_fd;
    size_t track_count;
    uint32_t selected_track;
    int selected;
    OH_AVBuffer *sample;
    OH_AVCodecBufferAttr attr;
    int has_sample;
    int eos;
    int hard_error;
    char error[256];
} ExtractorSlot;

static ExtractorSlot g_slots[EXTRACTOR_SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_next_generation = 1;

static void set_error(ExtractorSlot *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->error, sizeof(s->error), fmt, ap);
    va_end(ap);
}

static void *make_token(size_t index, uint64_t generation)
{
    uintptr_t value = (uintptr_t)(EXTRACTOR_TOKEN_TAG | (generation << 8) | (uint64_t)(index + 1));
    return (void *)value;
}

static ExtractorSlot *lookup(void *token)
{
    uintptr_t value = (uintptr_t)token;
    if ((value & UINT64_C(0xFF00000000000000)) != EXTRACTOR_TOKEN_TAG) return NULL;
    size_t encoded_index = (size_t)(value & 0xff);
    uint64_t generation = ((uint64_t)value >> 8) & EXTRACTOR_GENERATION_MAX;
    if (!encoded_index || encoded_index > EXTRACTOR_SLOTS || !generation) return NULL;
    ExtractorSlot *s = &g_slots[encoded_index - 1];
    return s->live && s->generation == generation ? s : NULL;
}
static void clear_media(ExtractorSlot *s)
{
    if (s->sample) {
        OH_AVBuffer_Destroy(s->sample);
        s->sample = NULL;
    }
    if (s->demuxer) {
        OH_AVDemuxer_Destroy(s->demuxer);
        s->demuxer = NULL;
    }
    if (s->source) {
        OH_AVSource_Destroy(s->source);
        s->source = NULL;
    }
    if (s->owned_fd >= 0) {
        close(s->owned_fd);
        s->owned_fd = -1;
    }
    s->track_count = 0;
    s->selected = 0;
    s->has_sample = 0;
    s->eos = 0;
    s->hard_error = 0;
}

static android_media_extractor_status_t bad_handle(void)
{
    return ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_OBJECT;
}

static int refill(ExtractorSlot *s)
{
    if (s->hard_error || s->eos || !s->selected || !s->demuxer) return s->has_sample;
    OH_AVErrCode rc = OH_AVDemuxer_ReadSampleBuffer(s->demuxer, s->selected_track, s->sample);
    if (rc != AV_ERR_OK) {
        s->hard_error = 1;
        set_error(s, "OH_AVDemuxer_ReadSampleBuffer failed: %d", (int)rc);
        fprintf(stderr, "android_media_extractor: %s\n", s->error);
        return 0;
    }
    rc = OH_AVBuffer_GetBufferAttr(s->sample, &s->attr);
    if (rc != AV_ERR_OK) {
        s->hard_error = 1;
        set_error(s, "OH_AVBuffer_GetBufferAttr failed: %d", (int)rc);
        fprintf(stderr, "android_media_extractor: %s\n", s->error);
        return 0;
    }
    int32_t capacity = OH_AVBuffer_GetCapacity(s->sample);
    if (s->attr.offset < 0 || s->attr.size < 0 || capacity < 0 ||
        s->attr.offset > capacity || s->attr.size > capacity - s->attr.offset) {
        s->hard_error = 1;
        set_error(s, "invalid OH sample bounds (offset=%d size=%d capacity=%d)", s->attr.offset, s->attr.size, capacity);
        fprintf(stderr, "android_media_extractor: %s\n", s->error);
        return 0;
    }
    if (s->attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
        s->eos = 1;
        s->has_sample = 0;
        set_error(s, "end of stream (OH EOS flag)");
        return 0;
    }
    s->has_sample = 1;
    return 1;
}

AMediaExtractor *android_media_extractor_new(void)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < EXTRACTOR_SLOTS; ++i) {
        ExtractorSlot *s = &g_slots[i];
        if (!s->live) {
            if (g_next_generation > EXTRACTOR_GENERATION_MAX) {
                pthread_mutex_unlock(&g_lock);
                return NULL;
            }
            uint64_t generation = g_next_generation++;
            memset(s, 0, sizeof(*s));
            s->generation = generation;
            s->live = 1;
            s->owned_fd = -1;
            strcpy(s->error, "no error");
            void *token = make_token(i, generation);
            pthread_mutex_unlock(&g_lock);
            return (AMediaExtractor *)token;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

android_media_extractor_status_t android_media_extractor_delete(AMediaExtractor *ex)
{
    pthread_mutex_lock(&g_lock);
    ExtractorSlot *s = lookup(ex);
    if (!s) { pthread_mutex_unlock(&g_lock); return bad_handle(); }
    clear_media(s);
    s->live = 0;
    pthread_mutex_unlock(&g_lock);
    return ANDROID_MEDIA_EXTRACTOR_OK;
}

android_media_extractor_status_t android_media_extractor_set_data_source_fd(AMediaExtractor *ex, int fd, int64_t offset, int64_t length)
{
    pthread_mutex_lock(&g_lock);
    ExtractorSlot *s = lookup(ex);
    if (!s) { pthread_mutex_unlock(&g_lock); return bad_handle(); }
    if (s->source || fd < 0 || offset < 0 || length <= 0 || offset > INT64_MAX - length) {
        set_error(s, "invalid FD source arguments or source already set");
        int status = s->source ? ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_OPERATION : ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_PARAMETER;
        pthread_mutex_unlock(&g_lock);
        return status;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || offset > st.st_size || length > st.st_size - offset) {
        set_error(s, "FD range is not a verified regular-file segment (errno=%d)", errno);
        pthread_mutex_unlock(&g_lock);
        return ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_PARAMETER;
    }
    int owned = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned < 0) {
        set_error(s, "dup FD failed: errno=%d", errno);
        pthread_mutex_unlock(&g_lock);
        return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN;
    }
    s->source = OH_AVSource_CreateWithFD(owned, offset, length);
    if (!s->source) {
        close(owned); s->source = NULL;
        set_error(s, "OH_AVSource_CreateWithFD failed");
        pthread_mutex_unlock(&g_lock);
        return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN;
    }
    s->demuxer = OH_AVDemuxer_CreateWithSource(s->source);
    if (!s->demuxer) {
        OH_AVSource_Destroy(s->source); s->source = NULL;
        close(owned);
        set_error(s, "OH_AVDemuxer_CreateWithSource failed");
        pthread_mutex_unlock(&g_lock);
        return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN;
    }
    s->owned_fd = owned;
    OH_AVFormat *format = OH_AVSource_GetSourceFormat(s->source);
    int32_t count = -1;
    if (!format || !OH_AVFormat_GetIntValue(format, OH_MD_KEY_TRACK_COUNT, &count) || count < 0) {
        if (format) OH_AVFormat_Destroy(format);
        clear_media(s);
        set_error(s, "could not read valid OH track count");
        pthread_mutex_unlock(&g_lock);
        return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN;
    }
    OH_AVFormat_Destroy(format);
    s->track_count = (size_t)count;
    if (!s->eos) strcpy(s->error, "no error");
    pthread_mutex_unlock(&g_lock);
    return ANDROID_MEDIA_EXTRACTOR_OK;
}

android_media_extractor_status_t android_media_extractor_set_data_source(AMediaExtractor *ex, const char *location)
{
    pthread_mutex_lock(&g_lock);
    ExtractorSlot *s = lookup(ex);
    if (!s) { pthread_mutex_unlock(&g_lock); return bad_handle(); }
    set_error(s, "URL data sources are unsupported in this backend");
    pthread_mutex_unlock(&g_lock);
    (void)location;
    return ANDROID_MEDIA_EXTRACTOR_ERROR_UNSUPPORTED;
}

android_media_extractor_status_t android_media_extractor_set_data_source_custom(AMediaExtractor *ex, AMediaDataSource *src)
{
    pthread_mutex_lock(&g_lock);
    ExtractorSlot *s = lookup(ex);
    if (!s) { pthread_mutex_unlock(&g_lock); return bad_handle(); }
    set_error(s, "custom data sources are unsupported in this backend");
    pthread_mutex_unlock(&g_lock);
    (void)src;
    return ANDROID_MEDIA_EXTRACTOR_ERROR_UNSUPPORTED;
}

size_t android_media_extractor_get_track_count(AMediaExtractor *ex)
{
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    size_t n = s ? s->track_count : 0;
    pthread_mutex_unlock(&g_lock); return n;
}

void *android_media_extractor_get_track_format(AMediaExtractor *ex, size_t idx)
{
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    if (!s || !s->source || idx >= s->track_count || idx > UINT32_MAX) {
        if (s) set_error(s, "track index out of range");
        pthread_mutex_unlock(&g_lock); return NULL;
    }
    OH_AVFormat *format = OH_AVSource_GetTrackFormat(s->source, (uint32_t)idx);
    if (!format) { set_error(s, "OH_AVSource_GetTrackFormat failed for track %zu", idx); pthread_mutex_unlock(&g_lock); return NULL; }
    void *wrapped = media_format_wrap(format); /* Takes ownership, including failure. */
    if (!wrapped) set_error(s, "media_format_wrap failed for track %zu", idx);
    pthread_mutex_unlock(&g_lock); return wrapped;
}

android_media_extractor_status_t android_media_extractor_select_track(AMediaExtractor *ex, size_t idx)
{
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    if (!s) { pthread_mutex_unlock(&g_lock); return bad_handle(); }
    if (!s->demuxer || idx >= s->track_count || idx > UINT32_MAX) {
        set_error(s, "track index out of range or no source set"); pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_PARAMETER;
    }
    if (s->selected) {
        int same = s->selected_track == (uint32_t)idx;
        if (!same) set_error(s, "only one selected track is supported");
        if (same && (s->hard_error || !s->sample)) {
            pthread_mutex_unlock(&g_lock);
            return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN;
        }
        pthread_mutex_unlock(&g_lock);
        return same ? ANDROID_MEDIA_EXTRACTOR_OK : ANDROID_MEDIA_EXTRACTOR_ERROR_UNSUPPORTED;
    }
    OH_AVErrCode rc = OH_AVDemuxer_SelectTrackByID(s->demuxer, (uint32_t)idx);
    if (rc != AV_ERR_OK) { set_error(s, "OH_AVDemuxer_SelectTrackByID failed: %d", (int)rc); pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_ERROR_UNSUPPORTED; }
    s->selected = 1; s->selected_track = (uint32_t)idx;
    s->sample = OH_AVBuffer_Create(INITIAL_SAMPLE_CAPACITY);
    if (!s->sample) {
        s->hard_error = 1; set_error(s, "OH_AVBuffer_Create failed");
        pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN;
    }
    (void)refill(s);
    if (s->hard_error) { pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN; }
    if (!s->eos) strcpy(s->error, "no error");
    pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_OK;
}

ssize_t android_media_extractor_read_sample_data(AMediaExtractor *ex, uint8_t *buffer, size_t capacity)
{
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    if (!s || !s->selected || !s->sample) { pthread_mutex_unlock(&g_lock); return -1; }
    if (!s->has_sample) { pthread_mutex_unlock(&g_lock); return -1; }
    if (s->hard_error) { pthread_mutex_unlock(&g_lock); return -1; }
    if (s->attr.size > 0 && !buffer) { set_error(s, "null destination for non-empty sample"); pthread_mutex_unlock(&g_lock); return -1; }
    if ((uint64_t)s->attr.size > capacity) { set_error(s, "destination capacity too small (%zu < %d)", capacity, s->attr.size); pthread_mutex_unlock(&g_lock); return -1; }
    uint8_t *addr = OH_AVBuffer_GetAddr(s->sample);
    if (!addr && s->attr.size) { set_error(s, "OH_AVBuffer_GetAddr returned null"); pthread_mutex_unlock(&g_lock); return -1; }
    if (s->attr.size) memcpy(buffer, addr + s->attr.offset, (size_t)s->attr.size);
    ssize_t result = s->attr.size;
    pthread_mutex_unlock(&g_lock); return result;
}

int android_media_extractor_get_sample_track_index(AMediaExtractor *ex)
{
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    int result = s && s->has_sample && !s->hard_error ? (int)s->selected_track : -1;
    pthread_mutex_unlock(&g_lock); return result;
}

int64_t android_media_extractor_get_sample_time(AMediaExtractor *ex)
{
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    int64_t result = s && s->has_sample && !s->hard_error ? s->attr.pts : -1;
    pthread_mutex_unlock(&g_lock); return result;
}

bool android_media_extractor_advance(AMediaExtractor *ex)
{
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    if (!s || !s->has_sample || s->hard_error) { pthread_mutex_unlock(&g_lock); return false; }
    s->has_sample = 0;
    (void)refill(s);
    bool result = s->has_sample && !s->hard_error;
    pthread_mutex_unlock(&g_lock); return result;
}

android_media_extractor_status_t android_media_extractor_seek_to(AMediaExtractor *ex, int64_t seekPosUs, SeekMode mode)
{
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    if (!s) { pthread_mutex_unlock(&g_lock); return bad_handle(); }
    if (!s->demuxer || seekPosUs < 0) { set_error(s, "seek requires source and nonnegative time"); pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_PARAMETER; }
    OH_AVSeekMode oh_mode;
    switch (mode) {
        case AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC: oh_mode = SEEK_MODE_PREVIOUS_SYNC; break;
        case AMEDIAEXTRACTOR_SEEK_NEXT_SYNC: oh_mode = SEEK_MODE_NEXT_SYNC; break;
        case AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC: oh_mode = SEEK_MODE_CLOSEST_SYNC; break;
        default: set_error(s, "invalid seek mode %d", (int)mode); pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_PARAMETER;
    }
    OH_AVErrCode rc = OH_AVDemuxer_SeekToTime(s->demuxer, seekPosUs / 1000, oh_mode);
    if (rc != AV_ERR_OK) { set_error(s, "OH_AVDemuxer_SeekToTime failed: %d", (int)rc); pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN; }
    s->has_sample = 0; s->eos = 0; s->hard_error = 0;
    if (s->selected) (void)refill(s);
    if (s->hard_error) { pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN; }
    if (!s->eos) strcpy(s->error, "no error (seek time truncated to whole milliseconds)");
    pthread_mutex_unlock(&g_lock); return ANDROID_MEDIA_EXTRACTOR_OK;
}

const char *android_media_extractor_last_error(void *ex)
{
    static _Thread_local char copy[256];
    pthread_mutex_lock(&g_lock); ExtractorSlot *s = lookup(ex);
    if (!s) snprintf(copy, sizeof(copy), "invalid or stale extractor token");
    else snprintf(copy, sizeof(copy), "%s", s->error);
    pthread_mutex_unlock(&g_lock); return copy;
}
