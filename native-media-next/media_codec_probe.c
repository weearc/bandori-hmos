#define _GNU_SOURCE 1
#include "media_codec_probe.h"
#include "media_codec.h"
#include "media_format.h"

#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define CODEC_TRY_AGAIN (-1)
#define CODEC_FORMAT_CHANGED (-2)
#define PCM_EXPECTED_BYTES 3200
#define INPUT_BYTES 1600
#define DEADLINE_NS (5LL * 1000LL * 1000LL * 1000LL)

static int64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int verify_pcm(const uint8_t *pcm, size_t bytes)
{
    if (bytes != PCM_EXPECTED_BYTES || (bytes & 1)) return 0;
    for (size_t i = 0; i < INPUT_BYTES; ++i) {
        int16_t value;
        memcpy(&value, pcm + i * 2, sizeof(value));
        int16_t magnitude = i < INPUT_BYTES / 2 ? 32124 : 15996;
        int16_t expected = (i & 1) ? magnitude : -magnitude;
        if (value != expected) return 0;
    }
    return 1;
}

int media_codec_probe_run(const char *directory)
{
    const char *android_mime = "audio/g711-mlaw";
    const char *native_mime = OH_AVCODEC_MIMETYPE_AUDIO_G711MU;
    uint8_t compressed[INPUT_BYTES];
    uint8_t pcm[PCM_EXPECTED_BYTES];
    size_t pcm_bytes = 0;
    void *codec = NULL, *oh_format = NULL, *android_format = NULL, *out_format = NULL;
    int rc = 0, started = 0, input_submitted = 0, eos_submitted = 0;
    size_t input_bytes = 0;
    int format_changes = 0, saw_output_eos = 0, output_count = 0;
    int64_t first_pts = -1, last_pts = -1;
    char error[192] = "none";
    int32_t rate = 0, channels = 0, sample_format = -1;
    int16_t bits = 0;
    int64_t deadline = mono_ns();
    if (deadline < 0) deadline = 0; else deadline += DEADLINE_NS;
    if (!directory || !native_mime) { rc = 1; snprintf(error, sizeof(error), "invalid directory or OH G711MU MIME unavailable"); goto finish; }

    for (size_t i = 0; i < INPUT_BYTES; ++i) compressed[i] = (i < INPUT_BYTES/2 ? 0x00 : 0x10) | ((i & 1) ? 0x80 : 0x00);
    oh_format = OH_AVFormat_CreateAudioFormat(native_mime, 8000, 1);
    if (!oh_format) { rc = 2; snprintf(error, sizeof(error), "OH_AVFormat_CreateAudioFormat failed"); goto finish; }
    if (!OH_AVFormat_SetIntValue((OH_AVFormat *)oh_format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE)) {
        rc = 3; snprintf(error, sizeof(error), "set OH SAMPLE_S16LE failed"); goto finish;
    }
    android_format = media_format_wrap((OH_AVFormat *)oh_format);
    oh_format = NULL; /* ownership transferred, including wrap failure */
    if (!android_format) { rc = 4; snprintf(error, sizeof(error), "media_format_wrap failed"); goto finish; }

    codec = android_media_codec_create_decoder_by_type(android_mime);
    if (!codec) { rc = 5; snprintf(error, sizeof(error), "create_decoder_by_type(audio/g711-mlaw) failed"); goto finish; }
    android_media_status_t status = android_media_codec_configure(codec, android_format, NULL, NULL, 0);
    if (status != ANDROID_MEDIA_OK) { rc = 6; snprintf(error, sizeof(error), "configure returned %d", status); goto finish; }
    status = android_media_codec_start(codec);
    if (status != ANDROID_MEDIA_OK) { rc = 7; snprintf(error, sizeof(error), "start returned %d", status); goto finish; }
    started = 1;

    while (mono_ns() < deadline && (!eos_submitted || !saw_output_eos)) {
        /* Feed input as soon as a ticket is available; zero-timeout polling lets
         * output drainage continue even when the decoder applies backpressure. */
        if (!input_submitted) {
            ssize_t in_index = android_media_codec_dequeue_input_buffer(codec, 0);
            if (in_index >= 0) {
                size_t capacity = 0;
                uint8_t *dst = android_media_codec_get_input_buffer(codec, (size_t)in_index, &capacity);
                const size_t packet_size=INPUT_BYTES/2;
                if (!dst || capacity < packet_size) { rc = 8; snprintf(error, sizeof(error), "input buffer unavailable/too small: %zu", capacity); goto finish; }
                memcpy(dst, compressed+input_bytes, packet_size);
                status = android_media_codec_queue_input_buffer(codec, (size_t)in_index, 0, packet_size, input_bytes*125, 0);
                if (status != ANDROID_MEDIA_OK) { rc = 9; snprintf(error, sizeof(error), "queue data returned %d", status); goto finish; }
                input_bytes+=packet_size;
                input_submitted = input_bytes==INPUT_BYTES;
                continue;
            }
            if (in_index != CODEC_TRY_AGAIN) { rc = 10; snprintf(error, sizeof(error), "dequeue input returned %zd", in_index); goto finish; }
        }
        if (input_submitted && !eos_submitted) {
            ssize_t in_index = android_media_codec_dequeue_input_buffer(codec, 0);
            if (in_index >= 0) {
                status = android_media_codec_queue_input_buffer(codec, (size_t)in_index, 0, 0, 200000, 4);
                if (status != ANDROID_MEDIA_OK) { rc = 11; snprintf(error, sizeof(error), "queue EOS returned %d", status); goto finish; }
                eos_submitted = 1;
                continue;
            }
            if (in_index != CODEC_TRY_AGAIN) { rc = 12; snprintf(error, sizeof(error), "dequeue EOS input returned %zd", in_index); goto finish; }
        }

        android_media_codec_buffer_info info = {0};
        ssize_t out_index = android_media_codec_dequeue_output_buffer(codec, &info, 10000);
        if (out_index == CODEC_TRY_AGAIN) continue;
        if (out_index == CODEC_FORMAT_CHANGED) {
            ++format_changes;
            if (out_format) { android_media_format_delete(out_format); out_format = NULL; }
            out_format = android_media_codec_get_output_format(codec);
            if (!out_format) { rc = 13; snprintf(error, sizeof(error), "format-changed without output format"); goto finish; }
            if (!android_media_format_get_int32(out_format, "sample-rate", &rate) ||
                !android_media_format_get_int32(out_format, "channel-count", &channels)) {
                rc = 14; snprintf(error, sizeof(error), "output format missing rate/channels"); goto finish;
            }
            (void)android_media_format_get_int32(out_format, "pcm-encoding", &sample_format);
            bits = sample_format == 2 ? 16 : 0; /* Android PCM_16BIT value */
            continue;
        }
        if (out_index < 0) { rc = 15; snprintf(error, sizeof(error), "dequeue output returned %zd", out_index); goto finish; }
        size_t capacity = 0;
        uint8_t *src = android_media_codec_get_output_buffer(codec, (size_t)out_index, &capacity);
        int valid = (src || info.size == 0) && info.offset >= 0 && info.size >= 0 && (uint64_t)info.offset <= capacity &&
                    (uint64_t)info.size <= capacity - (size_t)info.offset && pcm_bytes <= sizeof(pcm) &&
                    (size_t)info.size <= sizeof(pcm) - pcm_bytes;
        if (!valid) {
            android_media_codec_release_output_buffer(codec, (size_t)out_index, false);
            rc = 16; snprintf(error, sizeof(error), "invalid output bounds offset=%d size=%d capacity=%zu", info.offset, info.size, capacity); goto finish;
        }
        if (first_pts < 0) first_pts = info.presentationTimeUs;
        last_pts = info.presentationTimeUs;
        if (info.size) memcpy(pcm + pcm_bytes, src + info.offset, (size_t)info.size);
        pcm_bytes += (size_t)info.size;
        ++output_count;
        if (info.flags & 4u) saw_output_eos = 1;
        status = android_media_codec_release_output_buffer(codec, (size_t)out_index, false);
        if (status != ANDROID_MEDIA_OK) { rc = 17; snprintf(error, sizeof(error), "release output returned %d", status); goto finish; }
    }
    if (!eos_submitted || !saw_output_eos) { rc = 18; snprintf(error, sizeof(error), "5s deadline: EOS input/output missing"); goto finish; }
    /* A fixed-format decoder need not signal an actual format change. Query
     * its real output description instead of inventing a change event. */
    if (!out_format) {
        out_format=android_media_codec_get_output_format(codec);
        if (!out_format || !android_media_format_get_int32(out_format,"sample-rate",&rate) ||
            !android_media_format_get_int32(out_format,"channel-count",&channels)) {
            rc=24;snprintf(error,sizeof(error),"output description missing rate/channels");goto finish;
        }
        (void)android_media_format_get_int32(out_format,"pcm-encoding",&sample_format);
        bits=sample_format==2?16:0;
    }
    if (rate != 8000 || channels != 1 || bits != 16) { rc = 19; snprintf(error, sizeof(error), "output format mismatch rate=%d channels=%d sample_format=%d", rate, channels, sample_format); goto finish; }
    if (!verify_pcm(pcm, pcm_bytes)) { rc = 20; snprintf(error, sizeof(error), "PCM mismatch bytes=%zu expected=3200 ordered packets 32124/15996", pcm_bytes); goto finish; }
finish:
    if (started && codec) {
        android_media_status_t stop_status = android_media_codec_stop(codec);
        if (stop_status != ANDROID_MEDIA_OK && rc == 0) { rc = 21; snprintf(error, sizeof(error), "stop returned %d", stop_status); }
    }
    if (codec) {
        android_media_status_t delete_status = android_media_codec_delete(codec);
        if (delete_status != ANDROID_MEDIA_OK && rc == 0) { rc = 22; snprintf(error, sizeof(error), "delete returned %d", delete_status); }
    }
    /* Android getOutputFormat returns an independently owned snapshot. */
    if (out_format) {
        int32_t retained_rate=0;
        if (rc==0 && (!android_media_format_get_int32(out_format,"sample-rate",&retained_rate)||retained_rate!=8000)) {
            rc=23;snprintf(error,sizeof(error),"output format invalid after codec destruction");
        }
        android_media_format_delete(out_format);
    }
    if (android_format) android_media_format_delete(android_format);
    if (oh_format) OH_AVFormat_Destroy((OH_AVFormat *)oh_format);
    sdk_trace_fprintf(stderr, "NATIVE_MEDIA_CODEC_PROBE %s check=%d backend=OH_AudioCodec mime_android=%s mime_oh=%s input_bytes=%d input_submitted=%d eos_submitted=%d output_bytes=%zu outputs=%d output_eos=%d rate=%d channels=%d bits=%d first_pts_us=%lld last_pts_us=%lld format_changes=%d played=0 pcm_exact=%d error=%s\n",
        rc ? "FAIL" : "PASS", rc, android_mime, native_mime ? native_mime : "unavailable", INPUT_BYTES, input_submitted,
        eos_submitted, pcm_bytes, output_count, saw_output_eos, rate, channels, bits, (long long)first_pts,
        (long long)last_pts, format_changes, verify_pcm(pcm,pcm_bytes), error);
    return rc;
}
