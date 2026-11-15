#ifndef NATIVE_MEDIA_NEXT_MEDIA_CODEC_H
#define NATIVE_MEDIA_NEXT_MEDIA_CODEC_H
#include "media_format.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
} android_media_codec_buffer_info;
void *android_media_codec_create_decoder_by_type(const char *mime);
android_media_status_t android_media_codec_configure(void *, const void *,
                                                     void *, void *, uint32_t);
android_media_status_t android_media_codec_start(void *);
android_media_status_t android_media_codec_stop(void *);
android_media_status_t android_media_codec_flush(void *);
android_media_status_t android_media_codec_delete(void *);
ssize_t android_media_codec_dequeue_input_buffer(void *, int64_t);
uint8_t *android_media_codec_get_input_buffer(void *, size_t, size_t *);
android_media_status_t android_media_codec_queue_input_buffer(void *, size_t,
                                                              off_t, size_t,
                                                              uint64_t,
                                                              uint32_t);
ssize_t android_media_codec_dequeue_output_buffer(
    void *, android_media_codec_buffer_info *, int64_t);
uint8_t *android_media_codec_get_output_buffer(void *, size_t, size_t *);
android_media_status_t android_media_codec_release_output_buffer(void *, size_t,
                                                                 bool);
void *android_media_codec_get_output_format(void *);
android_media_status_t android_media_codec_set_output_surface(void *, void *);
#endif
