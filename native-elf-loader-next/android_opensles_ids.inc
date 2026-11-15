/*
 * Copyright (c) 2007-2009 The Khronos Group Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and /or associated documentation files (the "Materials "), to
 * deal in the Materials without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Materials, and to permit persons to whom the Materials are
 * furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Materials.
 *
 * THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE MATERIALS OR THE USE OR OTHER DEALINGS IN THE
 * MATERIALS.
 *
 * OpenSLES_IID.c - OpenSL ES version 1.0
 *
 */


/* Exact Android 14 extension GUIDs. Declaring an ID does not implement its interface.
 * Source: https://android.googlesource.com/platform/frameworks/wilhelm/+/refs/tags/android-14.0.0_r1/src/OpenSLES_IID.cpp
 * Unknown/unsupported interfaces must still fail in the actual host backend. */
#include <SLES/OpenSLES.h>
static const struct SLInterfaceID_ android_guid_ANDROIDCONFIGURATION = { 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDCONFIGURATION = &android_guid_ANDROIDCONFIGURATION;
static const struct SLInterfaceID_ android_guid_ANDROIDSIMPLEBUFFERQUEUE = { 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &android_guid_ANDROIDSIMPLEBUFFERQUEUE;
static const struct SLInterfaceID_ android_guid_ANDROIDACOUSTICECHOCANCELLATION = { 0x7b491460, 0x8d4d, 0x11e0, 0xbd61, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDACOUSTICECHOCANCELLATION = &android_guid_ANDROIDACOUSTICECHOCANCELLATION;
static const struct SLInterfaceID_ android_guid_ANDROIDAUTOMATICGAINCONTROL = { 0x0a8abfe0, 0x654c, 0x11e0, 0xba26, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDAUTOMATICGAINCONTROL = &android_guid_ANDROIDAUTOMATICGAINCONTROL;
static const struct SLInterfaceID_ android_guid_ANDROIDNOISESUPPRESSION = { 0x58b4b260, 0x8e06, 0x11e0, 0xaa8e, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDNOISESUPPRESSION = &android_guid_ANDROIDNOISESUPPRESSION;

/* Android 使用固定32位整数；本机OH头文件使用unsigned long。
 * 两侧接口必须独立，音频数据只传原指针，不增加PCM缓冲或复制。 */
#include <SLES/OpenSLES_OpenHarmony.h>
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
typedef uint32_t as_u;
typedef int32_t as_i;
typedef void (*as_callback)(void *, void *);
typedef void (*as_play_callback)(void *, void *, as_u);
typedef void (*as_object_callback)(void *,void *,as_u,as_u,as_u,void *);
typedef struct as_object as_object;
typedef struct { const void *vtable; as_object *owner; } as_iface;
typedef struct { as_u count,index; } as_queue_state;
typedef struct { as_u locatorType,numBuffers; } as_guest_queue;
typedef struct { as_u locatorType; void *mix; } as_guest_mix;
typedef struct { as_u formatType,channels,rate,bits,container,mask,endian; } as_guest_pcm;
typedef struct { as_u feature,data; } as_guest_option;
typedef struct {
    uint64_t enqueues,bytes,enqueue_failures,callback_count,callback_length_sum;
    uint64_t callback_length_last,callback_length_min,callback_length_max;
    uint64_t last_callback_ns,last_enqueue_ns;
    uint64_t interval_count,interval_sum_ns,interval_min_ns,interval_max_ns;
    uint64_t delay_count,delay_sum_ns,delay_min_ns,delay_max_ns;
    uint64_t state_calls,state_count,state_index;
    as_u enqueue_result,state_result;
} as_metrics;
_Static_assert(sizeof(as_guest_pcm)==28,"Android PCM ABI");
_Static_assert(sizeof(as_guest_queue)==8,"Android queue locator ABI");
_Static_assert(sizeof(as_queue_state)==8,"Android queue state ABI");
_Static_assert(offsetof(as_guest_mix,mix)==8,"Android output mix pointer ABI");
struct as_object {
    as_iface object,engine,queue,play,volume,configuration;
    SLObjectItf real;
    SLEngineItf he;
    SLOHBufferQueueItf hq;
    SLPlayItf hp;
    SLVolumeItf hv;
    atomic_int dead,callbacks,initialized;
    atomic_int destroy_pending;
    atomic_uint_fast64_t destroy_after;
    atomic_uint_fast64_t destroy_order;
    pthread_mutex_t lock;
    as_callback queue_cb; void *queue_context;
    as_play_callback play_cb; void *play_context;
    as_object_callback object_cb; void *object_context;
    uint64_t queue_cb_version,play_cb_version,object_cb_version;
    SLDataLocator_BufferQueue locator;
    SLDataLocator_OutputMix mix;
    SLDataFormat_PCM pcm;
    SLDataSource source;
    SLDataSink sink;
    as_metrics metrics;
    as_u frame_bytes;
    int has_configuration;
};
/* 销毁后保留少量接口身份记录，不复用，避免晚到的回调访问已释放元数据。 */
#define AS_OBJECT_LIMIT 128
static as_object as_objects[AS_OBJECT_LIMIT];
static atomic_uint as_objects_used,as_log_lines;
static _Thread_local int as_configuration_diagnostic;
static pthread_once_t as_host_once=PTHREAD_ONCE_INIT;
static void *as_host_library;
static SLresult (*as_host_create)(SLObjectItf*,SLuint32,const SLEngineOption*,SLuint32,const SLInterfaceID*,const SLboolean*);
static SLInterfaceID as_id_engine,as_id_play,as_id_volume,as_id_queue,as_id_standard_queue;
static uint64_t as_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec; }
static void as_log(const char *fmt,...) {
    if(atomic_fetch_add(&as_log_lines,1)>=240)return;
    int saved=errno;
    FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
    if(f){va_list ap;va_start(ap,fmt);fprintf(f,"ENGINE_LOG_OPENSL_ANDROID ");vfprintf(f,fmt,ap);fputc('\n',f);va_end(ap);fclose(f);}errno=saved;
}
static void as_load_host(void) {
    as_host_library=dlopen("libOpenSLES.so",RTLD_NOW|RTLD_LOCAL);
    if(!as_host_library){as_log("host_load failed=%s",dlerror());return;}
    as_host_create=(void*)dlsym(as_host_library,"slCreateEngine");
    const SLInterfaceID *p;
#define AS_ID(dst,name) do {p=(const SLInterfaceID*)dlsym(as_host_library,name);dst=p?*p:NULL;}while(0)
    AS_ID(as_id_engine,"SL_IID_ENGINE");AS_ID(as_id_play,"SL_IID_PLAY");AS_ID(as_id_volume,"SL_IID_VOLUME");AS_ID(as_id_queue,"SL_IID_OH_BUFFERQUEUE");
    AS_ID(as_id_standard_queue,"SL_IID_BUFFERQUEUE");
#undef AS_ID
    as_log("host_loaded uint32_bytes=%zu queue_state_bytes=%zu create=%p",sizeof(SLuint32),sizeof(SLOHBufferQueueState),(void*)as_host_create);
}
static as_object *as_owner(void *self){return self?((as_iface*)self)->owner:NULL;}
static int as_live(as_object *o){return o&&!atomic_load(&o->dead);}
static as_u as_bad(void){return SL_RESULT_PARAMETER_INVALID;}
static as_u as_unsupported(void){return SL_RESULT_FEATURE_UNSUPPORTED;}
static const void *as_object_vtable[],*as_engine_vtable[],*as_queue_vtable[],*as_play_vtable[],*as_volume_vtable[],*as_configuration_vtable[];
static as_object *as_wrap(SLObjectItf real) {
    unsigned n=atomic_fetch_add(&as_objects_used,1);
    if(n>=AS_OBJECT_LIMIT){if(real)(*real)->Destroy(real);return NULL;}
    as_object *o=&as_objects[n]; o->real=real;pthread_mutex_init(&o->lock,NULL);
    o->object=(as_iface){as_object_vtable,o};o->engine=(as_iface){as_engine_vtable,o};o->queue=(as_iface){as_queue_vtable,o};o->play=(as_iface){as_play_vtable,o};o->volume=(as_iface){as_volume_vtable,o};o->configuration=(as_iface){as_configuration_vtable,o};atomic_store(&o->initialized,1);return o;
}
static void as_queue_trampoline(SLOHBufferQueueItf caller,void *context,SLuint32 requested) {
    (void)caller;as_object *o=context;if(!o)return;
    atomic_fetch_add(&o->callbacks,1);
    pthread_mutex_lock(&o->lock);
    as_metrics *m=&o->metrics;uint64_t now=as_now();
    if(m->last_callback_ns){uint64_t v=now-m->last_callback_ns;m->interval_sum_ns+=v;if(!m->interval_count||v<m->interval_min_ns)m->interval_min_ns=v;if(v>m->interval_max_ns)m->interval_max_ns=v;m->interval_count++;}
    if(m->last_enqueue_ns){uint64_t v=now-m->last_enqueue_ns;m->delay_sum_ns+=v;if(!m->delay_count||v<m->delay_min_ns)m->delay_min_ns=v;if(v>m->delay_max_ns)m->delay_max_ns=v;m->delay_count++;}
    if(!m->callback_count||(uint64_t)requested<m->callback_length_min)m->callback_length_min=requested;
    if((uint64_t)requested>m->callback_length_max)m->callback_length_max=requested;
    m->callback_count++;m->callback_length_sum+=(uint64_t)requested;m->callback_length_last=requested;m->last_callback_ns=now;
    as_callback cb=o->queue_cb;void *ctx=o->queue_context;
    pthread_mutex_unlock(&o->lock);
    if(cb&&!atomic_load(&o->dead))cb(&o->queue,ctx);
    atomic_fetch_sub(&o->callbacks,1);
}
static void as_play_trampoline(SLPlayItf caller,void *context,SLuint32 event){(void)caller;as_object *o=context;atomic_fetch_add(&o->callbacks,1);pthread_mutex_lock(&o->lock);as_play_callback cb=o->play_cb;void *c=o->play_context;pthread_mutex_unlock(&o->lock);if(cb&&!atomic_load(&o->dead))cb(&o->play,c,(as_u)event);atomic_fetch_sub(&o->callbacks,1);}
static void as_object_trampoline(SLObjectItf caller,const void *context,SLuint32 event,SLresult result,SLuint32 param,void *itf){(void)caller;(void)itf;as_object *o=(as_object*)context;atomic_fetch_add(&o->callbacks,1);pthread_mutex_lock(&o->lock);as_object_callback cb=o->object_cb;void *c=o->object_context;pthread_mutex_unlock(&o->lock);if(cb&&!atomic_load(&o->dead))cb(&o->object,c,(as_u)event,(as_u)result,(as_u)param,NULL);atomic_fetch_sub(&o->callbacks,1);}
static void as_configuration_post_realize(as_object *o);
static as_u as_realize(void *s,as_u async){as_object *o=as_owner(s);if(!as_live(o))return as_bad();as_u r=(*o->real)->Realize(o->real,async);as_log("realize object=%p async=%u result=%u",(void*)o,async,r);if(!r&&o->has_configuration)as_configuration_post_realize(o);return r;}
static as_u as_resume(void *s,as_u async){as_object *o=as_owner(s);return as_live(o)?(*o->real)->Resume(o->real,async):as_bad();}
static as_u as_object_state(void *s,as_u *out){as_object *o=as_owner(s);if(!as_live(o)||!out)return as_bad();SLuint32 value=0;SLresult r=(*o->real)->GetState(o->real,&value);if(!r)*out=(as_u)value;return r;}
static as_u as_interface(void *s,const void *id,void **out){
    as_object *o=as_owner(s);if(!as_live(o)||!id||!out)return as_bad();*out=NULL;SLresult r;
    if(id==as_id_engine){r=(*o->real)->GetInterface(o->real,as_id_engine,&o->he);if(!r)*out=&o->engine;}
    /* Standard Android BUFFERQUEUE has the same four slots and 32-bit state
     * as AndroidSimpleBufferQueue; never expose OH's five-slot table. */
    else if(id==shim_SL_IID_ANDROIDSIMPLEBUFFERQUEUE||id==as_id_standard_queue){r=(*o->real)->GetInterface(o->real,as_id_queue,&o->hq);if(!r)*out=&o->queue;}
    else if(id==as_id_play){r=(*o->real)->GetInterface(o->real,as_id_play,&o->hp);if(!r)*out=&o->play;}
    else if(id==as_id_volume){r=(*o->real)->GetInterface(o->real,as_id_volume,&o->hv);if(!r)*out=&o->volume;}
    else if(id==shim_SL_IID_ANDROIDCONFIGURATION&&o->has_configuration){r=0;*out=&o->configuration;}
    else r=SL_RESULT_FEATURE_UNSUPPORTED;
    as_log("get_interface object=%p iid=%p result=%lu proxy=%p configuration=%d origin=%s",(void*)o,id,(unsigned long)r,*out,id==shim_SL_IID_ANDROIDCONFIGURATION,as_configuration_diagnostic?"host_diagnostic":"guest");return r;
}
static as_u as_object_register(void *s,as_object_callback cb,void *context){as_object *o=as_owner(s);if(!as_live(o))return as_bad();pthread_mutex_lock(&o->lock);as_object_callback old=o->object_cb;void *old_context=o->object_context;uint64_t version=++o->object_cb_version;o->object_cb=cb;o->object_context=context;pthread_mutex_unlock(&o->lock);SLresult r=(*o->real)->RegisterCallback(o->real,cb?as_object_trampoline:NULL,cb?o:NULL);if(r){pthread_mutex_lock(&o->lock);if(o->object_cb_version==version){o->object_cb=old;o->object_context=old_context;++o->object_cb_version;}pthread_mutex_unlock(&o->lock);}return r;}
static void as_abort(void *s){as_object *o=as_owner(s);if(as_live(o))(*o->real)->AbortAsyncOperation(o->real);}
static atomic_int as_defer_destroy,as_defer_ready;
static atomic_uint_fast64_t as_destroy_order;
/* Experimental host-only mitigation, OFF by default. Object metadata remains
 * retained as before. Serial worker isolates a blocking host Destroy from the
 * caller; it does not prove or repair a CRI-side join/condition deadlock. */
static void *as_destroy_worker(void *unused){(void)unused;for(;;){
 unsigned count=atomic_load(&as_objects_used);if(count>AS_OBJECT_LIMIT)count=AS_OBJECT_LIMIT;
 as_object*o=NULL;uint64_t first=UINT64_MAX;
 for(unsigned i=0;i<count;i++)if(atomic_load(&as_objects[i].destroy_pending)==1&&atomic_load(&as_objects[i].destroy_order)<first){o=&as_objects[i];first=atomic_load(&o->destroy_order);}
 if(o&&!atomic_load(&o->callbacks)&&as_now()>=atomic_load(&o->destroy_after)){
  int pending=1;if(atomic_compare_exchange_strong(&o->destroy_pending,&pending,2)){
   FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/perf-audio-destroy.txt","a");if(f){fprintf(f,"pid=%d mono_ns=%llu order=%llu object=%p phase=enter callbacks=%d\n",getpid(),(unsigned long long)as_now(),(unsigned long long)first,(void*)o,atomic_load(&o->callbacks));fclose(f);}
   (*o->real)->Destroy(o->real);atomic_store(&o->destroy_pending,3);
   f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/perf-audio-destroy.txt","a");if(f){fprintf(f,"pid=%d mono_ns=%llu order=%llu object=%p phase=returned\n",getpid(),(unsigned long long)as_now(),(unsigned long long)first,(void*)o);fclose(f);}
  }
 }struct timespec wait={0,20000000};nanosleep(&wait,NULL);
 }return NULL;}
void perf_audio_controls(void){
 FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/perf-audio-defer-destroy","r");int enabled=0;if(f){fscanf(f,"%d",&enabled);fclose(f);}
 if(enabled==1&&!atomic_load(&as_defer_ready)){pthread_t t;if(!pthread_create(&t,NULL,as_destroy_worker,NULL)){pthread_detach(t);atomic_store(&as_defer_ready,1);}}
 atomic_store(&as_defer_destroy,enabled==1&&atomic_load(&as_defer_ready));
}
static void as_destroy(void *s){as_object *o=as_owner(s);if(!o||atomic_exchange(&o->dead,1))return;
 if(atomic_load(&as_defer_destroy)){atomic_store(&o->destroy_order,atomic_fetch_add(&as_destroy_order,1)+1);atomic_store(&o->destroy_after,as_now()+200000000);atomic_store(&o->destroy_pending,1);return;}
 as_log("destroy object=%p callback_active=%d metadata_retained=1 capacity=128",(void*)o,atomic_load(&o->callbacks));(*o->real)->Destroy(o->real);}
static as_u as_priority(void *s,as_i priority,as_u preemptable){as_object *o=as_owner(s);return as_live(o)?(*o->real)->SetPriority(o->real,priority,preemptable):as_bad();}
static as_u as_get_priority(void *s,as_i *priority,as_u *preemptable){as_object *o=as_owner(s);if(!as_live(o)||!priority||!preemptable)return as_bad();SLint32 p=0;SLboolean b=0;SLresult r=(*o->real)->GetPriority(o->real,&p,&b);if(!r){*priority=p;*preemptable=b;}return r;}
static as_u as_loss(void *s,int16_t count,const void **ids,as_u enabled){(void)s;(void)count;(void)ids;(void)enabled;return as_unsupported();}
static const void *as_object_vtable[]={as_realize,as_resume,as_object_state,as_interface,as_object_register,as_abort,as_destroy,as_priority,as_get_priority,as_loss};

/* OH OpenSL's player is fixed MUSIC/MEDIA, not an arbitrary stream selector.
 * Official adapter uses CONTENT_TYPE_MUSIC=2, STREAM_USAGE_MEDIA=1,
 * RENDERER_NEW=0; device CreateRenderer options match (see TASK7B2 source
 * evidence). Android MEDIA=3 is therefore a truthful idempotent Set/Get.
 * Other values and all other keys remain unsupported, never merely saved. */
static as_u as_configuration_get(void *s,const char *key,as_u *size,void *value){
    as_object *o=as_owner(s);if(!as_live(o)||!o->has_configuration||!key||!size)return as_bad();
    if(strcmp(key,"androidPlaybackStreamType"))return as_unsupported();
    as_u capacity=*size;*size=sizeof(as_i);if(!value)return 0;
    if(capacity<sizeof(as_i))return SL_RESULT_BUFFER_INSUFFICIENT;
    as_i media=3;memcpy(value,&media,sizeof media);return 0;
}
static as_u as_configuration_set(void *s,const char *key,const void *value,as_u size){
    as_object *o=as_owner(s);if(!as_live(o)||!o->has_configuration||!key||!value)return as_bad();
    as_i requested=0;int scalar=size==sizeof requested;if(scalar)memcpy(&requested,value,sizeof requested);
    as_u r;
    if(strcmp(key,"androidPlaybackStreamType"))r=as_unsupported();
    else if(!scalar)r=as_bad();
    else {
        /* Host Realize may complete even when async was requested. Read the
         * actual object state rather than infer it from the request flag. */
        SLuint32 state=0;r=(*o->real)->GetState(o->real,&state);
        if(!r&&state!=SL_OBJECT_STATE_UNREALIZED)r=SL_RESULT_PRECONDITIONS_VIOLATED;
        if(!r&&requested!=3)r=as_unsupported();
    }
    as_i readback=-1;as_u readsize=sizeof readback;
    as_u gr=as_configuration_get(s,key,&readsize,&readback);
    as_log("configuration_set object=%p key=%s size=%u scalar=%d requested=%d result=%u get_result=%u get_size=%u readback=%d backend=fixed_MUSIC_MEDIA actual_fast_mode=unknown origin=%s",(void*)o,key,size,scalar,requested,r,gr,readsize,readback,as_configuration_diagnostic?"host_diagnostic":"guest");
    return r;
}
static as_u as_configuration_acquire(void*s,as_u type,void**out){(void)s;(void)type;if(out)*out=NULL;return as_unsupported();}
static as_u as_configuration_release(void*s,as_u type){(void)s;(void)type;return as_unsupported();}
static const void *as_configuration_vtable[]={as_configuration_set,as_configuration_get,as_configuration_acquire,as_configuration_release};
/* Diagnostic checks exercise the real player proxy and real host GetState.
 * They only confirm the already-active MEDIA default or attempt rejected
 * operations; they neither create a renderer nor modify PCM/play state. */
static void as_configuration_probe(as_object *o){
#ifdef LUNARIA_TASK6K_OBSERVER
    if(!o->has_configuration)return;
    int previous=as_configuration_diagnostic;as_configuration_diagnostic=1;
    void *s=NULL;as_u ir=as_interface(&o->object,shim_SL_IID_ANDROIDCONFIGURATION,&s);
    if(ir||s!=&o->configuration){as_log("configuration_probe checks=1 failures=1 ok=0 get_interface_result=%u",ir);as_configuration_diagnostic=previous;return;}
    const void *const *vtable=((as_iface*)s)->vtable;
    as_u (*set)(void*,const char*,const void*,as_u)=(void*)vtable[0];
    as_u (*get)(void*,const char*,as_u*,void*)=(void*)vtable[1];
    const char *key="androidPlaybackStreamType";
    as_i media=3,other=0,value=-1;as_u size=0,r;unsigned checks=1,failures=0;
#define AS_CHECK(expr) do{checks++;if(!(expr))failures++;}while(0)
    r=get(s,key,&size,NULL);AS_CHECK(r==0&&size==4);
    r=set(s,key,&media,4);AS_CHECK(r==0);
    size=4;r=get(s,key,&size,&value);AS_CHECK(r==0&&size==4&&value==media);
    as_u canary=0x12345678;size=2;r=get(s,key,&size,&canary);AS_CHECK(r==SL_RESULT_BUFFER_INSUFFICIENT&&size==4&&canary==0x12345678);
    AS_CHECK(get(s,key,NULL,&value)==SL_RESULT_PARAMETER_INVALID);
    size=4;AS_CHECK(get(s,NULL,&size,&value)==SL_RESULT_PARAMETER_INVALID);
    AS_CHECK(set(s,key,NULL,4)==SL_RESULT_PARAMETER_INVALID);
    AS_CHECK(set(s,key,&media,3)==SL_RESULT_PARAMETER_INVALID);
    AS_CHECK(set(s,key,&media,8)==SL_RESULT_PARAMETER_INVALID);
    AS_CHECK(set(s,key,&other,4)==SL_RESULT_FEATURE_UNSUPPORTED);
    AS_CHECK(set(s,"androidPerformanceMode",&media,4)==SL_RESULT_FEATURE_UNSUPPORTED);
    size=4;AS_CHECK(get(s,"androidPerformanceMode",&size,&value)==SL_RESULT_FEATURE_UNSUPPORTED);
    AS_CHECK(as_configuration_acquire(s,1,NULL)==SL_RESULT_FEATURE_UNSUPPORTED);
    size=4;r=get(s,key,&size,&value);AS_CHECK(r==0&&value==media);
    as_log("configuration_probe object=%p checks=%u failures=%u ok=%d get_via_proxy_vtable=1 real_host_player=1 backend_unchanged=1",(void*)o,checks,failures,failures==0);
    as_configuration_diagnostic=previous;
#undef AS_CHECK
#else
    (void)o;
#endif
}
static void as_configuration_post_realize(as_object *o){
#ifdef LUNARIA_TASK6K_OBSERVER
    SLuint32 state=0;as_u sr=(*o->real)->GetState(o->real,&state);
    if(sr||state!=SL_OBJECT_STATE_REALIZED){as_log("configuration_post_realize host_state_result=%u host_state=%lu pending_or_failed=1",sr,(unsigned long)state);return;}
    int previous=as_configuration_diagnostic;as_configuration_diagnostic=1;
    as_i media=3;as_u r=as_configuration_set(&o->configuration,"androidPlaybackStreamType",&media,4);
    as_configuration_diagnostic=previous;
    as_log("configuration_post_realize host_state=%lu set_result=%u expected=%lu ok=%d",(unsigned long)state,r,(unsigned long)SL_RESULT_PRECONDITIONS_VIOLATED,r==SL_RESULT_PRECONDITIONS_VIOLATED);
#else
    (void)o;
#endif
}
static as_u as_map_ids(as_u count,const void *const *ids,const as_u *required,SLInterfaceID *host,SLboolean *needed,SLuint32 *n,int local_configuration){
    if(count>16||(count&&(!ids||!required)))return as_bad();*n=0;
    for(as_u i=0;i<count;i++){SLInterfaceID h=NULL;
        /* The player proxy itself implements this subset; it is not an OH
         * interface and must not be forwarded to OH's interface list. */
        if(local_configuration&&ids[i]==shim_SL_IID_ANDROIDCONFIGURATION)continue;
        if(ids[i]==shim_SL_IID_ANDROIDSIMPLEBUFFERQUEUE||ids[i]==as_id_standard_queue)h=as_id_queue;
        else if(ids[i]==as_id_play)h=as_id_play;else if(ids[i]==as_id_volume)h=as_id_volume;
        else if(ids[i]==as_id_engine)h=as_id_engine;
        if(!h){as_log("interface_unsupported iid=%p required=%u configuration=%d",ids[i],required[i],ids[i]==shim_SL_IID_ANDROIDCONFIGURATION);if(required[i])return as_unsupported();continue;}
        host[*n]=h;needed[*n]=required[i]!=0;(*n)++;
    }return 0;
}
static as_u as_create_player(void *s,void **out,const SLDataSource *guest_source,const SLDataSink *guest_sink,as_u count,const void *const *ids,const as_u *required){
    as_object *engine=as_owner(s);if(!as_live(engine)||!engine->he||!out||!guest_source||!guest_sink||!guest_source->pLocator||!guest_source->pFormat||!guest_sink->pLocator)return as_bad();*out=NULL;
    const as_guest_queue *q=guest_source->pLocator;const as_guest_pcm *p=guest_source->pFormat;const as_guest_mix *m=guest_sink->pLocator;
    if((q->locatorType!=0x800007bdu&&q->locatorType!=6)||p->formatType!=2||m->locatorType!=4)return as_unsupported();
    as_object *mix=as_owner(m->mix);if(!as_live(mix))return as_bad();
    SLInterfaceID hi[16];SLboolean hr[16];SLuint32 hn;as_u r=as_map_ids(count,ids,required,hi,hr,&hn,1);if(r)return r;
    as_object *o=as_wrap(NULL);if(!o)return SL_RESULT_RESOURCE_ERROR;
    o->locator=(SLDataLocator_BufferQueue){SL_DATALOCATOR_BUFFERQUEUE,q->numBuffers};o->pcm=(SLDataFormat_PCM){p->formatType,p->channels,p->rate,p->bits,p->container,p->mask,p->endian};
    o->mix=(SLDataLocator_OutputMix){SL_DATALOCATOR_OUTPUTMIX,mix->real};o->source=(SLDataSource){&o->locator,&o->pcm};o->sink=(SLDataSink){&o->mix,NULL};
    r=(*engine->he)->CreateAudioPlayer(engine->he,&o->real,&o->source,&o->sink,hn,hi,hr);as_log("create_player result=%u channels=%u rate_millihz=%u bits=%u container=%u mask=%u endian=%u buffers=%u zero_extra_pcm_buffers=1",r,p->channels,p->rate,p->bits,p->container,p->mask,p->endian,q->numBuffers);
    if(r){atomic_store(&o->dead,1);return r;}for(as_u i=0;i<count;i++)if(ids[i]==shim_SL_IID_ANDROIDCONFIGURATION)o->has_configuration=1;pthread_mutex_lock(&o->lock);o->frame_bytes=p->channels*(p->container/8);pthread_mutex_unlock(&o->lock);as_configuration_probe(o);*out=&o->object;return 0;
}
static as_u as_create_mix(void *s,void **out,as_u count,const void *const *ids,const as_u *required){as_object *o=as_owner(s);if(!as_live(o)||!o->he||!out)return as_bad();*out=NULL;SLInterfaceID hi[16];SLboolean hr[16];SLuint32 hn;as_u r=as_map_ids(count,ids,required,hi,hr,&hn,0);if(r)return r;SLObjectItf real=NULL;r=(*o->he)->CreateOutputMix(o->he,&real,hn,hi,hr);if(!r){as_object *mix=as_wrap(real);if(!mix)return SL_RESULT_RESOURCE_ERROR;*out=&mix->object;}as_log("create_output_mix result=%u",r);return r;}
/* 未实现的Engine槽保留各自Android原型，避免错误调用约定。 */
static as_u as_device(void*s,void**o,as_u d,as_u n,const void*const*i,const as_u*r){(void)s;(void)d;(void)n;(void)i;(void)r;if(o)*o=NULL;return as_unsupported();}
static as_u as_recorder(void*s,void**o,void*a,void*b,as_u n,const void*const*i,const as_u*r){(void)s;(void)a;(void)b;(void)n;(void)i;(void)r;if(o)*o=NULL;return as_unsupported();}
static as_u as_midi(void*s,void**o,void*a,void*b,void*c,void*d,void*e,as_u n,const void*const*i,const as_u*r){(void)s;(void)a;(void)b;(void)c;(void)d;(void)e;(void)n;(void)i;(void)r;if(o)*o=NULL;return as_unsupported();}
static as_u as_listener(void*s,void**o,as_u n,const void*const*i,const as_u*r){(void)s;(void)n;(void)i;(void)r;if(o)*o=NULL;return as_unsupported();}
static as_u as_metadata(void*s,void**o,void*a,as_u n,const void*const*i,const as_u*r){(void)a;return as_listener(s,o,n,i,r);}
static as_u as_extension(void*s,void**o,void*a,as_u id,as_u n,const void*const*i,const as_u*r){(void)a;(void)id;return as_listener(s,o,n,i,r);}
static as_u as_query_num(void*s,as_u id,as_u*out){(void)s;(void)id;(void)out;return as_unsupported();}
static as_u as_query_itf(void*s,as_u id,as_u index,const void**out){(void)s;(void)id;(void)index;(void)out;return as_unsupported();}
static as_u as_num_extensions(void*s,as_u*out){(void)s;(void)out;return as_unsupported();}
static as_u as_query_extension(void*s,as_u index,char*name,int16_t*len){(void)s;(void)index;(void)name;(void)len;return as_unsupported();}
static as_u as_is_extension(void*s,const char*name,as_u*out){(void)s;(void)name;(void)out;return as_unsupported();}
static const void *as_engine_vtable[]={as_device,as_device,as_create_player,as_recorder,as_midi,as_listener,as_listener,as_create_mix,as_metadata,as_extension,as_query_num,as_query_itf,as_num_extensions,as_query_extension,as_is_extension};
static as_u as_enqueue(void *s,const void *data,as_u size){as_object *o=as_owner(s);if(!as_live(o)||!o->hq||!data||!size)return as_bad();SLresult r=(*o->hq)->Enqueue(o->hq,data,size);pthread_mutex_lock(&o->lock);o->metrics.enqueue_result=r;if(!r){o->metrics.enqueues++;o->metrics.bytes+=size;o->metrics.last_enqueue_ns=as_now();}else o->metrics.enqueue_failures++;pthread_mutex_unlock(&o->lock);return r;}
static as_u as_clear(void*s){as_object *o=as_owner(s);return as_live(o)&&o->hq?(*o->hq)->Clear(o->hq):as_bad();}
static as_u as_queue_state_get(void*s,as_queue_state*out){as_object *o=as_owner(s);if(!as_live(o)||!o->hq||!out)return as_bad();SLOHBufferQueueState h={0};SLresult r=(*o->hq)->GetState(o->hq,&h);if(!r){out->count=h.count;out->index=h.index;}pthread_mutex_lock(&o->lock);o->metrics.state_calls++;o->metrics.state_result=r;if(!r){o->metrics.state_count=h.count;o->metrics.state_index=h.index;}pthread_mutex_unlock(&o->lock);return r;}
static as_u as_queue_register(void*s,as_callback cb,void*context){as_object *o=as_owner(s);if(!as_live(o)||!o->hq)return as_bad();pthread_mutex_lock(&o->lock);as_callback old=o->queue_cb;void *old_context=o->queue_context;uint64_t version=++o->queue_cb_version;o->queue_cb=cb;o->queue_context=context;pthread_mutex_unlock(&o->lock);SLresult r=(*o->hq)->RegisterCallback(o->hq,cb?as_queue_trampoline:NULL,cb?o:NULL);if(r){pthread_mutex_lock(&o->lock);if(o->queue_cb_version==version){o->queue_cb=old;o->queue_context=old_context;++o->queue_cb_version;}pthread_mutex_unlock(&o->lock);}as_log("queue_register object=%p callback=%p result=%lu",(void*)o,(void*)cb,(unsigned long)r);return r;}
static const void *as_queue_vtable[]={as_enqueue,as_clear,as_queue_state_get,as_queue_register};
#define AS_PLAY_SET(name,method) static as_u name(void*s,as_u v){as_object*o=as_owner(s);return as_live(o)&&o->hp?(*o->hp)->method(o->hp,v):as_bad();}
#define AS_PLAY_GET(name,method) static as_u name(void*s,as_u*out){as_object*o=as_owner(s);if(!as_live(o)||!o->hp||!out)return as_bad();SLuint32 v=0;SLresult r=(*o->hp)->method(o->hp,&v);if(!r)*out=v;return r;}
static as_u as_play_state_set(void*s,as_u v){as_object*o=as_owner(s);as_u r=as_live(o)&&o->hp?(*o->hp)->SetPlayState(o->hp,v):as_bad();as_log("set_play_state object=%p state=%u result=%u",(void*)o,v,r);return r;}
AS_PLAY_GET(as_play_state_get,GetPlayState) AS_PLAY_GET(as_duration,GetDuration) AS_PLAY_GET(as_position,GetPosition)
AS_PLAY_SET(as_mask_set,SetCallbackEventsMask) AS_PLAY_GET(as_mask_get,GetCallbackEventsMask) AS_PLAY_SET(as_marker_set,SetMarkerPosition) AS_PLAY_GET(as_marker_get,GetMarkerPosition) AS_PLAY_SET(as_period_set,SetPositionUpdatePeriod) AS_PLAY_GET(as_period_get,GetPositionUpdatePeriod)
static as_u as_marker_clear(void*s){as_object*o=as_owner(s);return as_live(o)&&o->hp?(*o->hp)->ClearMarkerPosition(o->hp):as_bad();}
static as_u as_play_register(void*s,as_play_callback cb,void*c){as_object*o=as_owner(s);if(!as_live(o)||!o->hp)return as_bad();pthread_mutex_lock(&o->lock);as_play_callback old=o->play_cb;void *old_context=o->play_context;uint64_t version=++o->play_cb_version;o->play_cb=cb;o->play_context=c;pthread_mutex_unlock(&o->lock);SLresult r=(*o->hp)->RegisterCallback(o->hp,cb?as_play_trampoline:NULL,cb?o:NULL);if(r){pthread_mutex_lock(&o->lock);if(o->play_cb_version==version){o->play_cb=old;o->play_context=old_context;++o->play_cb_version;}pthread_mutex_unlock(&o->lock);}return r;}
static const void *as_play_vtable[]={as_play_state_set,as_play_state_get,as_duration,as_position,as_play_register,as_mask_set,as_mask_get,as_marker_set,as_marker_clear,as_marker_get,as_period_set,as_period_get};
#undef AS_PLAY_SET
#undef AS_PLAY_GET
static as_u as_volume_set(void*s,int16_t v){as_object*o=as_owner(s);return as_live(o)&&o->hv?(*o->hv)->SetVolumeLevel(o->hv,v):as_bad();}
static as_u as_volume_get(void*s,int16_t*v){as_object*o=as_owner(s);return as_live(o)&&o->hv&&v?(*o->hv)->GetVolumeLevel(o->hv,v):as_bad();}
static as_u as_volume_max(void*s,int16_t*v){as_object*o=as_owner(s);return as_live(o)&&o->hv&&v?(*o->hv)->GetMaxVolumeLevel(o->hv,v):as_bad();}
static as_u as_mute_set(void*s,as_u v){as_object*o=as_owner(s);return as_live(o)&&o->hv?(*o->hv)->SetMute(o->hv,v):as_bad();}
static as_u as_mute_get(void*s,as_u*v){as_object*o=as_owner(s);if(!as_live(o)||!o->hv||!v)return as_bad();SLboolean h=0;SLresult r=(*o->hv)->GetMute(o->hv,&h);if(!r)*v=h;return r;}
static as_u as_stereo_enable(void*s,as_u v){as_object*o=as_owner(s);return as_live(o)&&o->hv?(*o->hv)->EnableStereoPosition(o->hv,v):as_bad();}
static as_u as_stereo_enabled(void*s,as_u*v){as_object*o=as_owner(s);if(!as_live(o)||!o->hv||!v)return as_bad();SLboolean h=0;SLresult r=(*o->hv)->IsEnabledStereoPosition(o->hv,&h);if(!r)*v=h;return r;}
static as_u as_stereo_set(void*s,int16_t v){as_object*o=as_owner(s);return as_live(o)&&o->hv?(*o->hv)->SetStereoPosition(o->hv,v):as_bad();}
static as_u as_stereo_get(void*s,int16_t*v){as_object*o=as_owner(s);return as_live(o)&&o->hv&&v?(*o->hv)->GetStereoPosition(o->hv,v):as_bad();}
static const void *as_volume_vtable[]={as_volume_set,as_volume_get,as_volume_max,as_mute_set,as_mute_get,as_stereo_enable,as_stereo_enabled,as_stereo_set,as_stereo_get};
uint32_t android_sl_create_engine(void **out,uint32_t option_count,const void *options,uint32_t interface_count,const void *const *ids,const uint32_t *required){
    if(!out||option_count>16||(option_count&&!options))return as_bad();*out=NULL;pthread_once(&as_host_once,as_load_host);if(!as_host_create||!as_id_engine||!as_id_queue)return as_unsupported();
    SLEngineOption ho[16];const as_guest_option *go=options;for(as_u i=0;i<option_count;i++){ho[i].feature=go[i].feature;ho[i].data=go[i].data;}
    SLInterfaceID hi[16];SLboolean hr[16];SLuint32 hn;as_u r=as_map_ids(interface_count,ids,required,hi,hr,&hn,0);if(r)return r;SLObjectItf real=NULL;r=as_host_create(&real,option_count,ho,hn,hi,hr);if(!r){as_object *o=as_wrap(real);if(!o)return SL_RESULT_RESOURCE_ERROR;*out=&o->object;}as_log("create_engine result=%u object=%p",r,*out);return r;
}
void *android_opensles_adapted_symbol(const char *name){
    if(!name)return NULL;
    if(!strcmp(name,"slCreateEngine"))return (void*)android_sl_create_engine;
    if(!strcmp(name,"SL_IID_ANDROIDCONFIGURATION"))return (void*)&shim_SL_IID_ANDROIDCONFIGURATION;
    if(!strcmp(name,"SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))return (void*)&shim_SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    if(!strcmp(name,"SL_IID_ANDROIDACOUSTICECHOCANCELLATION"))return (void*)&shim_SL_IID_ANDROIDACOUSTICECHOCANCELLATION;
    if(!strcmp(name,"SL_IID_ANDROIDAUTOMATICGAINCONTROL"))return (void*)&shim_SL_IID_ANDROIDAUTOMATICGAINCONTROL;
    if(!strcmp(name,"SL_IID_ANDROIDNOISESUPPRESSION"))return (void*)&shim_SL_IID_ANDROIDNOISESUPPRESSION;
    return NULL;
}
/* 由UnityMain的低频观测点调用；只复制计数，不在音频线程写文件。
 * 不在此处调用宿主GetState：保存的值仅来自guest真实GetState调用。 */
void android_opensles_dump_metrics(void (*logger)(const char*,...)){
    if(!logger)return;
    unsigned used=atomic_load(&as_objects_used),count=used<AS_OBJECT_LIMIT?used:AS_OBJECT_LIMIT;
    logger("ENGINE_LOG_OPENSL_ANDROID metrics objects_reserved=%u capacity=%u lifetime_slot_reuse=0 rendered_frames=unknown",used,AS_OBJECT_LIMIT);
    for(unsigned i=0;i<count;i++){
        as_object *o=&as_objects[i];if(!atomic_load(&o->initialized))continue;
        as_metrics m;as_u frame_bytes;int dead=atomic_load(&o->dead),active=atomic_load(&o->callbacks);
        pthread_mutex_lock(&o->lock);m=o->metrics;frame_bytes=o->frame_bytes;pthread_mutex_unlock(&o->lock);
        if(!frame_bytes&&!m.enqueues&&!m.callback_count&&!m.enqueue_failures)continue;
        logger("ENGINE_LOG_OPENSL_ANDROID metrics object=%u dead=%d callback_active=%d frame_bytes=%u enqueues=%llu enqueued_bytes=%llu enqueued_frames=%llu enqueue_failures=%llu enqueue_last_result=%u callbacks=%llu callback_length_arg_sum=%llu callback_length_last=%llu callback_length_min=%llu callback_length_max=%llu callback_length_units=backend_length rendered_frames=unknown",
            i,dead,active,frame_bytes,(unsigned long long)m.enqueues,(unsigned long long)m.bytes,(unsigned long long)(frame_bytes?m.bytes/frame_bytes:0),(unsigned long long)m.enqueue_failures,m.enqueue_result,(unsigned long long)m.callback_count,(unsigned long long)m.callback_length_sum,(unsigned long long)m.callback_length_last,(unsigned long long)m.callback_length_min,(unsigned long long)m.callback_length_max);
        logger("ENGINE_LOG_OPENSL_ANDROID intervals object=%u count=%llu sum_ns=%llu min_ns=%llu max_ns=%llu mean_ns=%llu last_enqueue_to_callback_count=%llu sum_ns=%llu min_ns=%llu max_ns=%llu mean_ns=%llu queue_delay_is_estimate=1",
            i,(unsigned long long)m.interval_count,(unsigned long long)m.interval_sum_ns,(unsigned long long)m.interval_min_ns,(unsigned long long)m.interval_max_ns,(unsigned long long)(m.interval_count?m.interval_sum_ns/m.interval_count:0),(unsigned long long)m.delay_count,(unsigned long long)m.delay_sum_ns,(unsigned long long)m.delay_min_ns,(unsigned long long)m.delay_max_ns,(unsigned long long)(m.delay_count?m.delay_sum_ns/m.delay_count:0));
        logger("ENGINE_LOG_OPENSL_ANDROID getstate object=%u observed_calls=%llu last_result=%u last_count=%llu last_index=%llu observed_only=1 rendered_frames=unknown",
            i,(unsigned long long)m.state_calls,m.state_result,(unsigned long long)m.state_count,(unsigned long long)m.state_index);
    }
}
