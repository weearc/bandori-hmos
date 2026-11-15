#include "media_codec.h"
#include <limits.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avformat.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define SLOTS 16
#define CAP 128
#define BAD ANDROID_MEDIA_ERROR_INVALID_OPERATION
#define UNKNOWN ANDROID_MEDIA_ERROR_UNKNOWN

typedef struct {
  int state;
  uint32_t index;
  OH_AVBuffer *buffer;
  OH_AVCodecBufferAttr attr;
  size_t ticket;
  uint64_t seq;
} entry;
typedef struct {
  pthread_mutex_t op, q;
  pthread_cond_t cv;
  OH_AVCodec *native;
  uintptr_t token;
  int used, closing, refs;
  int running, configured, error, eos, cancelled;
  entry in[CAP], out[CAP];
  uint64_t sequence;
} codec;
static codec slots[SLOTS];
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t drained = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static uintptr_t generation = 1;
static size_t ticket = 1;
static size_t next_ticket(void) {
  size_t current = __atomic_load_n(&ticket, __ATOMIC_RELAXED);
  while (current && current <= (size_t)SSIZE_MAX) {
    size_t next = current == (size_t)SSIZE_MAX ? 0 : current + 1;
    if (__atomic_compare_exchange_n(&ticket, &current, next, 0,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      return current;
  }
  return 0;
}
static void init(void) {
  for (int i = 0; i < SLOTS; i++) {
    pthread_mutex_init(&slots[i].op, 0);
    pthread_mutex_init(&slots[i].q, 0);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&slots[i].cv, &a);
    pthread_condattr_destroy(&a);
  }
}
static codec *pin(void *h, OH_AVCodec *native) {
  pthread_once(&once, init);
  pthread_mutex_lock(&registry);
  codec *s = 0;
  for (int i = 0; i < SLOTS; i++)
    if (slots[i].used && !slots[i].closing && slots[i].token == (uintptr_t)h &&
        (!native || slots[i].native == native)) {
      s = &slots[i];
      s->refs++;
      break;
    }
  pthread_mutex_unlock(&registry);
  return s;
}
static void unpin(codec *s) {
  pthread_mutex_lock(&registry);
  if (--s->refs == 0)
    pthread_cond_broadcast(&drained);
  pthread_mutex_unlock(&registry);
}
static void fail(codec *s, int err) {
  pthread_mutex_lock(&s->q);
  s->error = err ? err : 1;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->q);
}
static void on_error(OH_AVCodec *n, int32_t err, void *u) {
  codec *s = pin(u, n);
  if (s) {
    fail(s, err);
    unpin(s);
  }
}
static void on_format(OH_AVCodec *n, OH_AVFormat *f, void *u) {
  (void)f;
  codec *s = pin(u, n);
  if (!s)
    return;
  pthread_mutex_lock(&s->q);
  if (s->running) {
    int i;
    for (i = 0; i < CAP; i++)
      if (!s->out[i].state)
        break;
    if (i == CAP || s->sequence == UINT64_MAX)
      s->error = 1;
    else
      s->out[i] = (entry){.state = 3, .seq = ++s->sequence};
    pthread_cond_broadcast(&s->cv);
  }
  pthread_mutex_unlock(&s->q);
  unpin(s);
}
static void on_buffer(OH_AVCodec *n, uint32_t idx, OH_AVBuffer *b, void *u,
                      int output) {
  codec *s = pin(u, n);
  if (!s)
    return;
  OH_AVCodecBufferAttr a = {0};
  int bad = !b || (output && OH_AVBuffer_GetBufferAttr(b, &a) != 0);
  if (output && !bad) {
    int32_t capacity = OH_AVBuffer_GetCapacity(b);
    if (capacity < 0 || a.offset < 0 || a.size < 0 ||
        (uint64_t)a.offset + (uint64_t)a.size > (uint64_t)capacity ||
        (a.flags & ~15u))
      bad = 1;
  }
  pthread_mutex_lock(&s->q);
  if (s->running) {
    entry *v = output ? s->out : s->in;
    int freepos = -1;
    for (int i = 0; i < CAP; i++) {
      if ((v[i].state == 1 || v[i].state == 2) && v[i].index == idx)
        bad = 1;
      if (!v[i].state && freepos < 0)
        freepos = i;
    }
    if (bad || freepos < 0 || s->sequence == UINT64_MAX)
      s->error = 1;
    else {
      v[freepos] = (entry){.state = 1,
                           .index = idx,
                           .buffer = b,
                           .attr = a,
                           .seq = ++s->sequence};
    }
    pthread_cond_broadcast(&s->cv);
  }
  pthread_mutex_unlock(&s->q);
  unpin(s);
}
static void on_in(OH_AVCodec *n, uint32_t i, OH_AVBuffer *b, void *u) {
  on_buffer(n, i, b, u, 0);
}
static void on_out(OH_AVCodec *n, uint32_t i, OH_AVBuffer *b, void *u) {
  on_buffer(n, i, b, u, 1);
}
void *android_media_codec_create_decoder_by_type(const char *mime) {
  if (!mime || strncmp(mime, "audio/", 6) || sizeof(uintptr_t) < 8)
    return NULL;
  const char *ohmime = strcmp(mime, "audio/g711-mlaw") == 0
                           ? OH_AVCODEC_MIMETYPE_AUDIO_G711MU
                           : mime;
  OH_AVCodec *n = OH_AudioCodec_CreateByMime(ohmime, false);
  if (!n)
    return NULL;
  pthread_once(&once, init);
  pthread_mutex_lock(&registry);
  codec *s = 0;
  for (int i = 0; i < SLOTS; i++)
    if (!slots[i].used) {
      s = &slots[i];
      break;
    }
  if (!s || generation > UINT64_C(0x0000FFFFFFFFFFFF)) {
    pthread_mutex_unlock(&registry);
    OH_AudioCodec_Destroy(n);
    return NULL;
  }
  s->used = 1;
  s->closing = 0;
  s->refs = 0;
  s->native = n;
  s->token = UINT64_C(0xC100000000000000) | (generation++ << 8) |
             (uintptr_t)(s - slots + 1);
  s->running = s->configured = s->error = s->eos = s->cancelled = 0;
  s->sequence = 0;
  memset(s->in, 0, sizeof(s->in));
  memset(s->out, 0, sizeof(s->out));
  void *h = (void *)s->token;
  pthread_mutex_unlock(&registry);
  OH_AVCodecCallback cb = {on_error, on_format, on_in, on_out};
  if (OH_AudioCodec_RegisterCallback(n, cb, h) != 0) {
    android_media_codec_delete(h);
    return NULL;
  }
  return h;
}
android_media_status_t android_media_codec_delete(void *h) {
  pthread_once(&once, init);
  pthread_mutex_lock(&registry);
  codec *s = 0;
  for (int i = 0; i < SLOTS; i++)
    if (slots[i].used && !slots[i].closing && slots[i].token == (uintptr_t)h) {
      s = &slots[i];
      break;
    }
  if (!s) {
    pthread_mutex_unlock(&registry);
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  }
  s->closing = 1;
  pthread_mutex_unlock(&registry);
  pthread_mutex_lock(&s->q);
  s->running = 0;
  s->cancelled = 1;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->q);
  pthread_mutex_lock(&registry);
  while (s->refs)
    pthread_cond_wait(&drained, &registry);
  pthread_mutex_unlock(&registry);
  int r = OH_AudioCodec_Destroy(s->native);
  pthread_mutex_lock(&registry);
  s->used = 0;
  s->native = 0;
  pthread_mutex_unlock(&registry);
  return r ? UNKNOWN : 0;
}
android_media_status_t android_media_codec_configure(void *h, const void *f,
                                                     void *surface,
                                                     void *crypto,
                                                     uint32_t flags) {
  if (surface || crypto || flags)
    return ANDROID_MEDIA_ERROR_UNSUPPORTED;
  codec *s = pin(h, 0);
  if (!s)
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  pthread_mutex_lock(&s->op);
  pthread_mutex_lock(&s->q);
  int valid = !s->cancelled && !s->running && !s->configured;
  pthread_mutex_unlock(&s->q);
  if (!valid) {
    pthread_mutex_unlock(&s->op);
    unpin(s);
    return BAD;
  }
  OH_AVFormat *copy = media_format_clone_for_audio((void *)f);
  if (copy) {
    const char *mime = NULL;
    if (OH_AVFormat_GetStringValue(copy, OH_MD_KEY_CODEC_MIME, &mime) &&
        !strcmp(mime, "audio/g711-mlaw") &&
        !OH_AVFormat_SetStringValue(copy, OH_MD_KEY_CODEC_MIME,
                                    OH_AVCODEC_MIMETYPE_AUDIO_G711MU)) {
      OH_AVFormat_Destroy(copy);
      copy = NULL;
    }
  }
  int r = copy ? OH_AudioCodec_Configure(s->native, copy) : -1;
  if (copy)
    OH_AVFormat_Destroy(copy);
  if (!r)
    r = OH_AudioCodec_Prepare(s->native);
  if (!r)
    s->configured = 1;
  pthread_mutex_unlock(&s->op);
  unpin(s);
  return r ? BAD : 0;
}
android_media_status_t android_media_codec_start(void *h) {
  codec *s = pin(h, 0);
  if (!s)
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  pthread_mutex_lock(&s->op);
  pthread_mutex_lock(&s->q);
  int valid = s->configured && !s->running && !s->cancelled;
  if (valid) {
    s->running = 1;
    s->error = 0;
    s->eos = 0;
  }
  pthread_mutex_unlock(&s->q);
  int r = valid ? OH_AudioCodec_Start(s->native) : -1;
  if (r) {
    pthread_mutex_lock(&s->q);
    s->running = 0;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->q);
  }
  pthread_mutex_unlock(&s->op);
  unpin(s);
  return r ? BAD : 0;
}
android_media_status_t android_media_codec_stop(void *h) {
  codec *s = pin(h, 0);
  if (!s)
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  pthread_mutex_lock(&s->op);
  pthread_mutex_lock(&s->q);
  int was = s->running;
  s->running = 0;
  /* A stopped native instance is terminal until restart callback isolation is
   * proven. A fresh decoder supplies a fresh callback token. */
  s->cancelled = 1;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->q);
  int r = was ? OH_AudioCodec_Stop(s->native) : -1;
  pthread_mutex_lock(&s->q);
  memset(s->in, 0, sizeof(s->in));
  memset(s->out, 0, sizeof(s->out));
  s->configured = 0;
  pthread_mutex_unlock(&s->q);
  pthread_mutex_unlock(&s->op);
  unpin(s);
  return r ? BAD : 0;
}
android_media_status_t android_media_codec_flush(void *h) {
  codec *s = pin(h, 0);
  if (!s)
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  unpin(s);
  return ANDROID_MEDIA_ERROR_UNSUPPORTED;
}
static ssize_t dequeue(void *h, android_media_codec_buffer_info *info,
                       int64_t us, int out) {
  codec *s = pin(h, 0);
  if (!s)
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  if (out && !info) {
    unpin(s);
    return ANDROID_MEDIA_ERROR_INVALID_PARAMETER;
  }
  struct timespec t = {0};
  if (us > 0) {
    clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t sec = us / 1000000;
    if (sec > INT_MAX)
      sec = INT_MAX;
    t.tv_sec += sec;
    t.tv_nsec += (us % 1000000) * 1000;
    if (t.tv_nsec >= 1000000000) {
      t.tv_sec++;
      t.tv_nsec -= 1000000000;
    }
  }
  pthread_mutex_lock(&s->q);
  ssize_t result = BAD;
  for (;;) {
    if (!s->running || s->error)
      break;
    entry *v = out ? s->out : s->in;
    int i = -1;
    for (int j = 0; j < CAP; j++)
      if ((v[j].state == 1 || (out && v[j].state == 3)) &&
          (i < 0 || v[j].seq < v[i].seq))
        i = j;
    if (i >= 0 && v[i].state == 3) {
      v[i].state = 0;
      result = -2;
      break;
    }
    if (i >= 0) {
      size_t id = next_ticket();
      if (!id) {
        s->error = 1;
        break;
      }
      v[i].state = 2;
      v[i].ticket = id;
      if (out) {
        info->offset = v[i].attr.offset;
        info->size = v[i].attr.size;
        info->presentationTimeUs = v[i].attr.pts;
        uint32_t f = v[i].attr.flags;
        info->flags = ((f & 1) ? 4 : 0) | ((f & 2) ? 1 : 0) |
                      ((f & 8) ? 2 : 0) | ((f & 4) ? 8 : 0);
        if (f & ~15u) {
          s->error = 1;
          result = BAD;
          break;
        }
      }
      result = (ssize_t)id;
      break;
    }
    if (!us) {
      result = -1;
      break;
    }
    int r = us < 0 ? pthread_cond_wait(&s->cv, &s->q)
                   : pthread_cond_timedwait(&s->cv, &s->q, &t);
    if (r) {
      result = -1;
      break;
    }
  }
  pthread_mutex_unlock(&s->q);
  unpin(s);
  return result;
}
ssize_t android_media_codec_dequeue_input_buffer(void *h, int64_t us) {
  return dequeue(h, 0, us, 0);
}
ssize_t android_media_codec_dequeue_output_buffer(
    void *h, android_media_codec_buffer_info *i, int64_t us) {
  return dequeue(h, i, us, 1);
}
static entry *find(entry *v, size_t id) {
  for (int i = 0; i < CAP; i++)
    if (v[i].state == 2 && v[i].ticket == id)
      return &v[i];
  return 0;
}
static uint8_t *get(void *h, size_t id, size_t *size, int out) {
  if (size)
    *size = 0;
  codec *s = pin(h, 0);
  if (!s || !size) {
    if (s)
      unpin(s);
    return 0;
  }
  pthread_mutex_lock(&s->op);
  pthread_mutex_lock(&s->q);
  entry *e = s->running ? find(out ? s->out : s->in, id) : 0;
  OH_AVBuffer *b = e ? e->buffer : 0;
  pthread_mutex_unlock(&s->q);
  int32_t cap = b ? OH_AVBuffer_GetCapacity(b) : -1;
  uint8_t *p = cap >= 0 ? OH_AVBuffer_GetAddr(b) : 0;
  if (p)
    *size = (size_t)cap;
  pthread_mutex_unlock(&s->op);
  unpin(s);
  return p;
}
uint8_t *android_media_codec_get_input_buffer(void *h, size_t i, size_t *z) {
  return get(h, i, z, 0);
}
uint8_t *android_media_codec_get_output_buffer(void *h, size_t i, size_t *z) {
  return get(h, i, z, 1);
}
android_media_status_t android_media_codec_queue_input_buffer(
    void *h, size_t id, off_t off, size_t size, uint64_t pts, uint32_t flags) {
  if (off < 0 || off > INT32_MAX || size > INT32_MAX || pts > INT64_MAX ||
      (flags & ~15u))
    return ANDROID_MEDIA_ERROR_INVALID_PARAMETER;
  codec *s = pin(h, 0);
  if (!s)
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  pthread_mutex_lock(&s->op);
  pthread_mutex_lock(&s->q);
  entry *e = s->running && !s->eos ? find(s->in, id) : 0;
  OH_AVBuffer *b = e ? e->buffer : 0;
  uint32_t idx = e ? e->index : 0;
  pthread_mutex_unlock(&s->q);
  int32_t cap = b ? OH_AVBuffer_GetCapacity(b) : -1;
  int r = -1;
  if (cap >= 0 && (uint64_t)off + size <= (uint64_t)cap) {
    OH_AVCodecBufferAttr a = {
        .pts = (int64_t)pts,
        .offset = (int32_t)off,
        .size = (int32_t)size,
        .flags = ((flags & 4) ? 1 : 0) | ((flags & 1) ? 2 : 0) |
                 ((flags & 2) ? 8 : 0) | ((flags & 8) ? 4 : 0)};
    r = OH_AVBuffer_SetBufferAttr(b, &a);
    if (!r) {
      pthread_mutex_lock(&s->q);
      e->state = 0;
      if (flags & 4)
        s->eos = 1;
      pthread_mutex_unlock(&s->q);
      r = OH_AudioCodec_PushInputBuffer(s->native, idx);
      if (r)
        fail(s, r);
    }
  }
  pthread_mutex_unlock(&s->op);
  unpin(s);
  return r ? BAD : 0;
}
android_media_status_t
android_media_codec_release_output_buffer(void *h, size_t id, bool render) {
  if (render)
    return ANDROID_MEDIA_ERROR_UNSUPPORTED;
  codec *s = pin(h, 0);
  if (!s)
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  pthread_mutex_lock(&s->op);
  pthread_mutex_lock(&s->q);
  entry *e = s->running ? find(s->out, id) : 0;
  uint32_t idx = e ? e->index : 0;
  if (e)
    e->state = 0;
  pthread_mutex_unlock(&s->q);
  int r = e ? OH_AudioCodec_FreeOutputBuffer(s->native, idx) : -1;
  if (e && r)
    fail(s, r);
  pthread_mutex_unlock(&s->op);
  unpin(s);
  return r ? BAD : 0;
}
void *android_media_codec_get_output_format(void *h) {
  codec *s = pin(h, 0);
  if (!s)
    return 0;
  pthread_mutex_lock(&s->op);
  OH_AVFormat *f = OH_AudioCodec_GetOutputDescription(s->native);
  OH_AVFormat *copy = f ? OH_AVFormat_Create() : 0;
  if (copy && !OH_AVFormat_Copy(copy, f)) {
    OH_AVFormat_Destroy(copy);
    copy = 0;
  }
  if (f)
    OH_AVFormat_Destroy(f);
  void *result = media_format_wrap(copy);
  pthread_mutex_unlock(&s->op);
  unpin(s);
  return result;
}
android_media_status_t android_media_codec_set_output_surface(void *h,
                                                              void *w) {
  (void)w;
  codec *s = pin(h, 0);
  if (!s)
    return ANDROID_MEDIA_ERROR_INVALID_OBJECT;
  unpin(s);
  return ANDROID_MEDIA_ERROR_UNSUPPORTED;
}
