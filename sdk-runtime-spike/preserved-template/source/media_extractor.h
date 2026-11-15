#ifndef NATIVE_MEDIA_NEXT_MEDIA_EXTRACTOR_H
#define NATIVE_MEDIA_NEXT_MEDIA_EXTRACTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t android_media_extractor_status_t;
typedef struct AMediaExtractor AMediaExtractor;
typedef struct AMediaDataSource AMediaDataSource;
typedef enum {
    AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC = 0,
    AMEDIAEXTRACTOR_SEEK_NEXT_SYNC = 1,
    AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC = 2
} SeekMode;

enum {
    ANDROID_MEDIA_EXTRACTOR_OK = 0,
    ANDROID_MEDIA_EXTRACTOR_ERROR_UNKNOWN = -10000,
    ANDROID_MEDIA_EXTRACTOR_ERROR_UNSUPPORTED = -10002,
    ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_OBJECT = -10003,
    ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_PARAMETER = -10004,
    ANDROID_MEDIA_EXTRACTOR_ERROR_INVALID_OPERATION = -10005
};

AMediaExtractor *android_media_extractor_new(void);
android_media_extractor_status_t android_media_extractor_delete(AMediaExtractor *ex);
android_media_extractor_status_t android_media_extractor_set_data_source_fd(AMediaExtractor *ex, int fd, int64_t offset, int64_t length);
android_media_extractor_status_t android_media_extractor_set_data_source(AMediaExtractor *ex, const char *location);
android_media_extractor_status_t android_media_extractor_set_data_source_custom(AMediaExtractor *ex, AMediaDataSource *src);
size_t android_media_extractor_get_track_count(AMediaExtractor *ex);
void *android_media_extractor_get_track_format(AMediaExtractor *ex, size_t idx);
android_media_extractor_status_t android_media_extractor_select_track(AMediaExtractor *ex, size_t idx);
ssize_t android_media_extractor_read_sample_data(AMediaExtractor *ex, uint8_t *buffer, size_t capacity);
int android_media_extractor_get_sample_track_index(AMediaExtractor *ex);
int64_t android_media_extractor_get_sample_time(AMediaExtractor *ex);
bool android_media_extractor_advance(AMediaExtractor *ex);
android_media_extractor_status_t android_media_extractor_seek_to(AMediaExtractor *ex, int64_t seekPosUs, SeekMode mode);
const char *android_media_extractor_last_error(void *ex);

#ifdef __cplusplus
}
#endif
#endif
