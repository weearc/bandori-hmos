#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <atomic>
#include <cstring>
#include <new>
#include <time.h>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
struct Stream;
using Data = int32_t(*)(Stream*,void*,void*,int32_t);
using ErrorCallback = void(*)(Stream*,void*,int32_t);
struct Builder { int rate=48000, channels=2, format=1, direction=0, performance=10, usage=1, content=2; Data cb=nullptr; void* user=nullptr; ErrorCallback error=nullptr; void* errorUser=nullptr; bool unsupported=false; };
struct Stream { OH_AudioRenderer* renderer=nullptr; Data cb; void* user; ErrorCallback error; void* errorUser; int rate,channels,format,performance=10; std::atomic<bool> closing{false}, stop_requested{false}, stop_processed{false}, stop_finished{false}, worker_exit{false}; std::atomic<int> callbacks{0}; std::mutex control; std::thread worker; };
static constexpr int ERR=-896, INVALID=-898, UNAVAILABLE=-889, BAD_STATE=-895;
static thread_local Stream* in_callback=nullptr;
static int32_t write_cb(OH_AudioRenderer*,void* context,void* data,int32_t bytes) {
 auto* s=static_cast<Stream*>(context); s->callbacks.fetch_add(1); int framebytes=s->channels*(s->format==1?2:4);
 if(bytes>0) std::memset(data,0,bytes);
 if(!s->closing.load() && !s->stop_requested.load() && s->cb && bytes>0 && bytes%framebytes==0) {
  in_callback=s; int result=1; try { result=s->cb(s,s->user,data,bytes/framebytes); } catch (...) { result=1; } in_callback=nullptr;
  if(result!=0) s->stop_requested.store(true);
 }
 s->callbacks.fetch_sub(1); return 0;
}
static int32_t error_cb(OH_AudioRenderer*, void* context, OH_AudioStream_Result error) {
 auto* s=static_cast<Stream*>(context); s->callbacks.fetch_add(1);
 if(!s->closing.load() && s->error) { in_callback=s; int mapped=ERR; if(error==AUDIOSTREAM_ERROR_INVALID_PARAM) mapped=INVALID; else if(error==AUDIOSTREAM_ERROR_ILLEGAL_STATE) mapped=BAD_STATE; else if(error==AUDIOSTREAM_ERROR_SERVICE_DIED) mapped=-899; try { s->error(s,s->errorUser,mapped); } catch (...) {} in_callback=nullptr; }
 s->callbacks.fetch_sub(1); return 0;
}
static void worker(Stream* s) {
 while(!s->worker_exit.load()) {
  if(s->stop_requested.load() && !s->stop_processed.exchange(true)) { std::lock_guard<std::mutex> lock(s->control); if(!s->closing.load()) OH_AudioRenderer_Stop(s->renderer); s->stop_finished=true; }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
 }
}
extern "C" {
int32_t AAudio_createStreamBuilder(Builder** out) { if(!out)return INVALID; *out=new(std::nothrow) Builder; return *out?0:ERR; }
int32_t AAudioStreamBuilder_delete(Builder* b) { if(!b)return INVALID; delete b; return 0; }
void AAudioStreamBuilder_setSampleRate(Builder* b,int32_t v){if(b)b->rate=v;}
void AAudioStreamBuilder_setChannelCount(Builder* b,int32_t v){if(b)b->channels=v;}
void AAudioStreamBuilder_setSamplesPerFrame(Builder* b,int32_t v){if(b)b->channels=v;}
void AAudioStreamBuilder_setFormat(Builder* b,int32_t v){if(b)b->format=v;}
void AAudioStreamBuilder_setDirection(Builder* b,int32_t v){if(b)b->direction=v;}
void AAudioStreamBuilder_setDataCallback(Builder* b,Data f,void* u){if(b){b->cb=f;b->user=u;}}
void AAudioStreamBuilder_setPerformanceMode(Builder* b,int32_t v){if(b){if(v!=10 && v!=12)b->unsupported=true;else b->performance=v;}}
void AAudioStreamBuilder_setSharingMode(Builder* b,int32_t v){if(b && v!=1)b->unsupported=true;}
void AAudioStreamBuilder_setDeviceId(Builder* b,int32_t v){if(b && v!=0)b->unsupported=true;}
void AAudioStreamBuilder_setBufferCapacityInFrames(Builder* b,int32_t v){if(b && v!=0)b->unsupported=true;}
void AAudioStreamBuilder_setFramesPerDataCallback(Builder* b,int32_t v){if(b && v!=0)b->unsupported=true;}
// Unsupported setters poison the builder so open cannot report success with ignored configuration.
void AAudioStreamBuilder_setErrorCallback(Builder*b,ErrorCallback f,void* u){if(b){b->error=f;b->errorUser=u;}}
void AAudioStreamBuilder_setChannelMask(Builder*b,int32_t){if(b)b->unsupported=true;}
void AAudioStreamBuilder_setContentType(Builder*b,int32_t v){if(b){if(v!=2 && v!=3)b->unsupported=true;else b->content=v;}}
void AAudioStreamBuilder_setUsage(Builder*b,int32_t v){if(b){if(v!=1 && v!=14)b->unsupported=true;else b->usage=v;}}
void AAudioStreamBuilder_setInputPreset(Builder*b,int32_t){if(b)b->unsupported=true;}
void AAudioStreamBuilder_setSessionId(Builder*b,int32_t){if(b)b->unsupported=true;}
void AAudioStreamBuilder_setAllowedCapturePolicy(Builder*b,int32_t){if(b)b->unsupported=true;}
void AAudioStreamBuilder_setPrivacySensitive(Builder*b,bool){if(b)b->unsupported=true;}
void AAudioStreamBuilder_setIsContentSpatialized(Builder*b,bool){if(b)b->unsupported=true;}
void AAudioStreamBuilder_setSpatializationBehavior(Builder*b,int32_t){if(b)b->unsupported=true;}
void AAudioStreamBuilder_setPackageName(Builder*b,const char*){if(b)b->unsupported=true;}
void AAudioStreamBuilder_setAttributionTag(Builder*b,const char*){if(b)b->unsupported=true;}
int32_t AAudioStreamBuilder_openStream(Builder* b,Stream** out) {
 if(!b||!out)return INVALID; *out=nullptr; if(b->unsupported||(b->usage==14 && b->content!=2)||b->direction!=0||!b->cb||b->rate<=0||b->channels<=0||(b->format!=1&&b->format!=2))return UNAVAILABLE;
 auto* s=new(std::nothrow) Stream; if(!s)return ERR; s->cb=b->cb;s->user=b->user;s->error=b->error;s->errorUser=b->errorUser;s->rate=b->rate;s->channels=b->channels;s->format=b->format;
 OH_AudioStreamBuilder* ob=nullptr; auto r=OH_AudioStreamBuilder_Create(&ob,AUDIOSTREAM_TYPE_RENDERER);
 if(r==AUDIOSTREAM_SUCCESS) { OH_AudioRenderer_Callbacks cb{}; cb.OH_AudioRenderer_OnWriteData=write_cb; cb.OH_AudioRenderer_OnError=error_cb;
 if((r=OH_AudioStreamBuilder_SetSamplingRate(ob,b->rate))==AUDIOSTREAM_SUCCESS && (r=OH_AudioStreamBuilder_SetChannelCount(ob,b->channels))==AUDIOSTREAM_SUCCESS && (r=OH_AudioStreamBuilder_SetSampleFormat(ob,b->format==1?AUDIOSTREAM_SAMPLE_S16LE:AUDIOSTREAM_SAMPLE_F32LE))==AUDIOSTREAM_SUCCESS && (r=OH_AudioStreamBuilder_SetEncodingType(ob,AUDIOSTREAM_ENCODING_TYPE_RAW))==AUDIOSTREAM_SUCCESS && (r=OH_AudioStreamBuilder_SetLatencyMode(ob,b->performance==12?AUDIOSTREAM_LATENCY_MODE_FAST:AUDIOSTREAM_LATENCY_MODE_NORMAL))==AUDIOSTREAM_SUCCESS && (r=OH_AudioStreamBuilder_SetRendererInfo(ob,b->usage==14?AUDIOSTREAM_USAGE_GAME:(b->content==3?AUDIOSTREAM_USAGE_MOVIE:AUDIOSTREAM_USAGE_MUSIC)))==AUDIOSTREAM_SUCCESS && (r=OH_AudioStreamBuilder_SetRendererCallback(ob,cb,s))==AUDIOSTREAM_SUCCESS) r=OH_AudioStreamBuilder_GenerateRenderer(ob,&s->renderer);
 OH_AudioStreamBuilder_Destroy(ob); }
 if(r!=AUDIOSTREAM_SUCCESS){delete s;return UNAVAILABLE;} int32_t rate=0, channels=0; OH_AudioStream_SampleFormat format; OH_AudioStream_LatencyMode latency;
 if(OH_AudioRenderer_GetLatencyMode(s->renderer,&latency)!=AUDIOSTREAM_SUCCESS || latency!=(b->performance==12?AUDIOSTREAM_LATENCY_MODE_FAST:AUDIOSTREAM_LATENCY_MODE_NORMAL) || OH_AudioRenderer_GetSamplingRate(s->renderer,&rate)!=AUDIOSTREAM_SUCCESS || OH_AudioRenderer_GetChannelCount(s->renderer,&channels)!=AUDIOSTREAM_SUCCESS || OH_AudioRenderer_GetSampleFormat(s->renderer,&format)!=AUDIOSTREAM_SUCCESS || rate!=s->rate || channels!=s->channels || format!=(s->format==1?AUDIOSTREAM_SAMPLE_S16LE:AUDIOSTREAM_SAMPLE_F32LE)) { OH_AudioRenderer_Release(s->renderer); delete s; return UNAVAILABLE; }
 s->performance=b->performance;
 try { s->worker=std::thread(worker,s); } catch (...) { s->closing=true; OH_AudioRenderer_Release(s->renderer); delete s; return ERR; } *out=s;return 0;
}
int32_t AAudioStream_requestStart(Stream*s){if(!s)return INVALID;if(in_callback==s)return BAD_STATE;std::lock_guard<std::mutex> l(s->control);if(s->closing)return BAD_STATE;if(s->stop_requested && !s->stop_finished)return BAD_STATE;s->stop_processed=false;s->stop_finished=false;s->stop_requested=false;return OH_AudioRenderer_Start(s->renderer)==AUDIOSTREAM_SUCCESS?0:ERR;}
int32_t AAudioStream_requestPause(Stream*s){if(!s)return INVALID;if(in_callback==s)return BAD_STATE;std::lock_guard<std::mutex> l(s->control);if(s->closing)return BAD_STATE;return OH_AudioRenderer_Pause(s->renderer)==AUDIOSTREAM_SUCCESS?0:ERR;}
int32_t AAudioStream_requestStop(Stream*s){if(!s)return INVALID;if(in_callback==s)return BAD_STATE;std::lock_guard<std::mutex> l(s->control);if(s->closing)return BAD_STATE;return OH_AudioRenderer_Stop(s->renderer)==AUDIOSTREAM_SUCCESS?0:ERR;}
int32_t AAudioStream_requestFlush(Stream*s){if(!s)return INVALID;if(in_callback==s)return BAD_STATE;std::lock_guard<std::mutex> l(s->control);if(s->closing)return BAD_STATE;return OH_AudioRenderer_Flush(s->renderer)==AUDIOSTREAM_SUCCESS?0:ERR;}
int32_t AAudioStream_close(Stream*s){
 if(!s)return INVALID;if(in_callback==s)return BAD_STATE;
 {std::lock_guard<std::mutex> l(s->control);if(s->closing.exchange(true))return BAD_STATE; OH_AudioRenderer_Stop(s->renderer);}
 s->worker_exit=true; if(s->worker.joinable())s->worker.join();
 while(s->callbacks.load()!=0)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 {std::lock_guard<std::mutex> l(s->control);if(OH_AudioRenderer_Release(s->renderer)!=AUDIOSTREAM_SUCCESS)return ERR;s->renderer=nullptr;}
 // Retain the tombstone: SDK headers do not guarantee callbacks are quiescent after release.
 return 0;
}
int32_t AAudioStream_getSampleRate(Stream*s){return !s?INVALID:s->closing?BAD_STATE:s->rate;}
int32_t AAudioStream_getChannelCount(Stream*s){return !s?INVALID:s->closing?BAD_STATE:s->channels;}
int32_t AAudioStream_getSamplesPerFrame(Stream*s){return AAudioStream_getChannelCount(s);}
int32_t AAudioStream_getFormat(Stream*s){return !s?INVALID:s->closing?BAD_STATE:s->format;}
int32_t AAudioStream_getDirection(Stream*s){return !s?INVALID:s->closing?BAD_STATE:0;}
int32_t AAudioStream_getState(Stream*s){if(!s)return INVALID;if(in_callback==s)return BAD_STATE;std::lock_guard<std::mutex> l(s->control);if(s->closing)return BAD_STATE;OH_AudioStream_State state; if(OH_AudioRenderer_GetCurrentState(s->renderer,&state)!=AUDIOSTREAM_SUCCESS)return ERR;switch(state){case AUDIOSTREAM_STATE_NEW:return 1;case AUDIOSTREAM_STATE_PREPARED:return 2;case AUDIOSTREAM_STATE_RUNNING:return 4;case AUDIOSTREAM_STATE_PAUSED:return 6;case AUDIOSTREAM_STATE_STOPPED:return 10;case AUDIOSTREAM_STATE_RELEASED:return 12;default:return 0;}}
int64_t AAudioStream_getFramesWritten(Stream*s){if(!s)return INVALID;if(in_callback==s)return BAD_STATE;std::lock_guard<std::mutex> l(s->control);if(s->closing)return BAD_STATE;int64_t v=0;return s&&OH_AudioRenderer_GetFramesWritten(s->renderer,&v)==AUDIOSTREAM_SUCCESS?v:ERR;}
int32_t AAudioStream_getTimestamp(Stream*s,clockid_t c,int64_t*f,int64_t*t){if(!s)return INVALID;if(in_callback==s)return BAD_STATE;std::lock_guard<std::mutex> l(s->control);if(s->closing)return BAD_STATE;if(!s||!f||!t)return INVALID;return OH_AudioRenderer_GetTimestamp(s->renderer,c,f,t)==AUDIOSTREAM_SUCCESS?0:ERR;}
int32_t AAudioStream_getXRunCount(Stream*s){if(!s)return INVALID;if(in_callback==s)return BAD_STATE;std::lock_guard<std::mutex> l(s->control);if(s->closing)return BAD_STATE;uint32_t v=0;return s&&OH_AudioRenderer_GetUnderflowCount(s->renderer,&v)==AUDIOSTREAM_SUCCESS?static_cast<int32_t>(v):ERR;}
int32_t AAudioStream_write(Stream*,const void*,int32_t,int64_t){return UNAVAILABLE;}
int32_t AAudioStream_read(Stream*,void*,int32_t,int64_t){return UNAVAILABLE;}
// No OH SDK equivalent with the same semantics; do not invent a buffer or route.
int32_t AAudioStream_getBufferCapacityInFrames(Stream*s){return s?UNAVAILABLE:INVALID;}
int32_t AAudioStream_getBufferSizeInFrames(Stream*s){return s?UNAVAILABLE:INVALID;}
int32_t AAudioStream_getFramesPerBurst(Stream*s){return s?UNAVAILABLE:INVALID;}
int32_t AAudioStream_getDeviceId(Stream*s){return s?UNAVAILABLE:INVALID;}
int32_t AAudioStream_getSharingMode(Stream*s){return s?UNAVAILABLE:INVALID;}
int32_t AAudioStream_getChannelMask(Stream*s){return s?UNAVAILABLE:INVALID;}
int32_t AAudioStream_getAllowedCapturePolicy(Stream*s){return s?UNAVAILABLE:INVALID;}
int32_t AAudioStream_getSpatializationBehavior(Stream*s){return s?UNAVAILABLE:INVALID;}
int64_t AAudioStream_getFramesRead(Stream*s){return s?UNAVAILABLE:INVALID;}
int32_t AAudioStream_getPerformanceMode(Stream*s){return !s?INVALID:s->closing?BAD_STATE:s->performance;}
int32_t AAudioStream_release(Stream*s){return s?-890:INVALID;}
int32_t AAudioStream_setBufferSizeInFrames(Stream*s,int32_t){return s?-890:INVALID;}
int32_t AAudioStream_waitForStateChange(Stream*s,int32_t input,int32_t*next,int64_t timeout){
 if(!s||timeout<0)return INVALID;if(in_callback==s)return BAD_STATE;
 auto start=std::chrono::steady_clock::now();
 for(;;){int state=AAudioStream_getState(s);if(state<0)return state;if(next)*next=state;if(state!=input)return 0;
 auto elapsed=std::chrono::steady_clock::now()-start;if(elapsed>=std::chrono::nanoseconds(timeout))return -885;
 auto remain=std::chrono::nanoseconds(timeout)-std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
 std::this_thread::sleep_for(remain<std::chrono::milliseconds(1)?remain:std::chrono::milliseconds(1));}
}
const char* AAudio_convertResultToText(int32_t x){switch(x){case 0:return "AAUDIO_OK";case -899:return "AAUDIO_ERROR_DISCONNECTED";case -898:return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";case -896:return "AAUDIO_ERROR_INTERNAL";case -895:return "AAUDIO_ERROR_INVALID_STATE";case -890:return "AAUDIO_ERROR_UNIMPLEMENTED";case -889:return "AAUDIO_ERROR_UNAVAILABLE";case -885:return "AAUDIO_ERROR_TIMEOUT";default:return "AAUDIO_ERROR_UNKNOWN";}}
const char* AAudio_convertStreamStateToText(int32_t x){switch(x){case 0:return "AAUDIO_STREAM_STATE_UNKNOWN";case 1:return "AAUDIO_STREAM_STATE_UNINITIALIZED";case 2:return "AAUDIO_STREAM_STATE_OPEN";case 3:return "AAUDIO_STREAM_STATE_STARTING";case 4:return "AAUDIO_STREAM_STATE_STARTED";case 5:return "AAUDIO_STREAM_STATE_PAUSING";case 6:return "AAUDIO_STREAM_STATE_PAUSED";case 7:return "AAUDIO_STREAM_STATE_FLUSHING";case 8:return "AAUDIO_STREAM_STATE_FLUSHED";case 9:return "AAUDIO_STREAM_STATE_STOPPING";case 10:return "AAUDIO_STREAM_STATE_STOPPED";case 11:return "AAUDIO_STREAM_STATE_CLOSING";case 12:return "AAUDIO_STREAM_STATE_CLOSED";case 13:return "AAUDIO_STREAM_STATE_DISCONNECTED";default:return "AAUDIO_STREAM_STATE_UNKNOWN";}}

}
