#include <napi/native_api.h>
#include "page_geometry.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <native_window/external_window.h>
#include <native_buffer/buffer_common.h>
#include <sstream>
#include <vector>
#include <thread>
#include <chrono>
#include <hilog/log.h>
#include <ctime>
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <dlfcn.h>
#include <atomic>
#include <cmath>
#include <deque>
#include <cstdio>
#include <mutex>
#include <map>
#include <memory>
#include <string>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <cstdint>
#include "../../../../../native-elf-loader-next/multitouch_packet.h"

static std::mutex lock;
static std::string graphics = "surface pending", touches = "no touch events";
static std::deque<std::string> touchHistory;
struct PointerStats { uint64_t down=0, up=0, cancel=0, anomalous=0; bool active=false; };
static std::map<int32_t, PointerStats> pointers;
static uint64_t touchEvents=0, touchErrors=0, cancelEvents=0;
/* 触摸转发到 guest：libsdktrace 导出的线程安全队列（VM worker 在 frame_wait
 * tick 中消费并调用 UnityPlayer.nativeInjectEvent）。 */
using QueueTouchFn = int(*)(int,int,float,float,int64_t);
static QueueTouchFn queueTouch = nullptr;
using QueueMultiTouchFn = int(*)(const mt_packet*);
static QueueMultiTouchFn queueMultiTouch = nullptr;
using ObserveTouchFn=void(*)(uint64_t,int,int,uint64_t,uint64_t);
static ObserveTouchFn observeTouch=nullptr;
static std::thread renderThread;
static std::atomic<bool> stopRender{false};
static std::atomic<bool> unityWindowOwned{false};
// XComponent owns the callback pointer. Keep a separate real native-object
// reference for the bridge; serialize OHOS's non-thread-safe reference API.
static std::mutex nativeWindowLock;
static OHNativeWindow* currentNativeWindow=nullptr;
static uint64_t nativeWindowGeneration=0;
static int32_t nativeWindowBaseWidth=0,nativeWindowBaseHeight=0,nativeWindowBaseFormat=-1;
static int ReferenceHostWindow(void* window) {
    std::lock_guard<std::mutex> guard(nativeWindowLock);
    return window?OH_NativeWindow_NativeObjectReference(window):-EINVAL;
}
static int UnreferenceHostWindow(void* window) {
    std::lock_guard<std::mutex> guard(nativeWindowLock);
    return window?OH_NativeWindow_NativeObjectUnreference(window):-EINVAL;
}
static void DetachHostWindow() {
    std::lock_guard<std::mutex> guard(nativeWindowLock);
    auto* old=currentNativeWindow;currentNativeWindow=nullptr;
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","HOST_SURFACE detach window=%{public}p generation=%{public}llu wallMs=%{public}lld",old,(unsigned long long)nativeWindowGeneration,(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    if(old)OH_NativeWindow_NativeObjectUnreference(old);
}
static bool AttachHostWindow(void* window,uint64_t componentWidth,uint64_t componentHeight) {
    std::lock_guard<std::mutex> guard(nativeWindowLock);
    if(!window||currentNativeWindow||nativeWindowGeneration==UINT64_MAX)return false;
    if(OH_NativeWindow_NativeObjectReference(window)!=0)return false;
    currentNativeWindow=static_cast<OHNativeWindow*>(window);++nativeWindowGeneration;
    int32_t w=0,h=0,format=-1;
    if(OH_NativeWindow_NativeWindowHandleOpt(currentNativeWindow,GET_BUFFER_GEOMETRY,&h,&w)!=0||w<=0||h<=0) {
        w=componentWidth<=INT32_MAX?(int32_t)componentWidth:0;
        h=componentHeight<=INT32_MAX?(int32_t)componentHeight:0;
    }
    if(OH_NativeWindow_NativeWindowHandleOpt(currentNativeWindow,GET_FORMAT,&format)!=0)format=-1;
    nativeWindowBaseWidth=w;nativeWindowBaseHeight=h;nativeWindowBaseFormat=format;
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","HOST_SURFACE attach window=%{public}p generation=%{public}llu size=%{public}dx%{public}d wallMs=%{public}lld",window,(unsigned long long)nativeWindowGeneration,w,h,(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    return true;
}
static int AcquireCurrentHostWindow(void*,void** window,uint64_t* generation) {
    if(!window||!generation)return -EINVAL;
    *window=nullptr;*generation=0;
    std::lock_guard<std::mutex> guard(nativeWindowLock);
    if(!currentNativeWindow)return -ENODEV;
    if(OH_NativeWindow_NativeObjectReference(currentNativeWindow)!=0)return -EIO;
    *window=currentNativeWindow;*generation=nativeWindowGeneration;return 0;
}
static int QueryHostWindow(void*,void* window,uint64_t generation,int32_t* width,int32_t* height) {
    if(!width||!height)return -EINVAL;
    *width=0;*height=0;
    std::lock_guard<std::mutex> guard(nativeWindowLock);
    if(!window||window!=currentNativeWindow||generation!=nativeWindowGeneration)return -ENODEV;
    int32_t w=0,h=0;
    int rc=OH_NativeWindow_NativeWindowHandleOpt(currentNativeWindow,GET_BUFFER_GEOMETRY,&h,&w);
    if(rc!=0||w<=0||h<=0)return -EIO;
    *width=w;*height=h;return 0;
}
static int ReleaseHostWindow(void*,void* window) { return UnreferenceHostWindow(window); }
static int ConfigureHostWindow(void*,void* window,uint64_t generation,int32_t width,int32_t height,int32_t androidFormat) {
    if(width<0||height<0||androidFormat<0||((width==0)!=(height==0)))return -EINVAL;
    std::lock_guard<std::mutex> guard(nativeWindowLock);
    if(!window||window!=currentNativeWindow||generation!=nativeWindowGeneration)return -ENODEV;
    int32_t format;
    switch(androidFormat) {
        case 0:format=nativeWindowBaseFormat;break;
        case 1:format=NATIVEBUFFER_PIXEL_FMT_RGBA_8888;break;
        case 2:format=NATIVEBUFFER_PIXEL_FMT_RGBX_8888;break;
        case 4:format=NATIVEBUFFER_PIXEL_FMT_RGB_565;break;
        default:return -ENOTSUP;
    }
    bool reset=width==0;
    int32_t w=reset?nativeWindowBaseWidth:width,h=reset?nativeWindowBaseHeight:height;
    if(w<=0||h<=0||format<0)return -EIO;
    using SetScalingFn=int32_t(*)(OHNativeWindow*,OHScalingModeV2);
    static auto setScaling=reinterpret_cast<SetScalingFn>(dlsym(RTLD_DEFAULT,"OH_NativeWindow_NativeWindowSetScalingModeV2"));
    if(!setScaling)return -ENOTSUP;
    // Like AOSP this sequence can partially change state on a later failure.
    // Queries always read the host; no cached success or assumed rollback.
    int rc=OH_NativeWindow_NativeWindowHandleOpt(currentNativeWindow,SET_FORMAT,format);
    if(rc==0)rc=OH_NativeWindow_NativeWindowHandleOpt(currentNativeWindow,SET_BUFFER_GEOMETRY,w,h);
    if(rc==0)rc=setScaling(currentNativeWindow,reset?OH_SCALING_MODE_FREEZE_V2:OH_SCALING_MODE_SCALE_TO_WINDOW_V2);
    return rc==0?0:-EIO;
}
static OH_AudioRenderer* renderer = nullptr;
static std::atomic<int> samples{0};
static std::mutex subscriberLock;
static napi_threadsafe_function subscriber=nullptr;
static napi_env subscriberEnv=nullptr;
static std::atomic<uint64_t> enqueueFailures{0}, touchSequence{0};
static std::mutex guestMountLock;
static std::condition_variable guestMountChanged;
static int64_t guestMountSequence=0;
static bool guestMountDone=false,guestMountAccepted=false;
static int MountGuestLayout(void*,int orientation) {
    if(orientation!=0 && orientation!=1) return -1;
    napi_threadsafe_function fn=nullptr;
    {std::lock_guard<std::mutex> guard(subscriberLock);if(subscriber && napi_acquire_threadsafe_function(subscriber)==napi_ok)fn=subscriber;}
    if(!fn)return -1;
    std::unique_lock<std::mutex> guard(guestMountLock);
    int64_t id=++guestMountSequence;guestMountDone=false;guestMountAccepted=false;
    auto* message=new std::string("GUEST_MOUNT:"+std::to_string(id)+":"+std::to_string(orientation));
    napi_status result=napi_call_threadsafe_function(fn,message,napi_tsfn_nonblocking);
    napi_release_threadsafe_function(fn,napi_tsfn_release);
    if(result!=napi_ok){delete message;return -1;}
    if(!guestMountChanged.wait_for(guard,std::chrono::seconds(2),[id]{return guestMountSequence==id&&guestMountDone;}))return -1;
    return guestMountAccepted?0:-1;
}
static napi_value AckGuestView(napi_env env,napi_callback_info info) {
    napi_value args[2],result;size_t argc=2;int64_t id=0;bool accepted=false;
    if(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr)!=napi_ok || argc!=2 ||
       napi_get_value_int64(env,args[0],&id)!=napi_ok || napi_get_value_bool(env,args[1],&accepted)!=napi_ok){
        napi_throw_type_error(env,nullptr,"Expected mount sequence and accepted boolean");return nullptr;
    }
    {std::lock_guard<std::mutex> guard(guestMountLock);if(id==guestMountSequence){guestMountDone=true;guestMountAccepted=accepted;}}
    guestMountChanged.notify_all();napi_get_undefined(env,&result);return result;
}
#include "../../../../../battery-runtime-next/battery_host.h"
#include "../../../../../sdk-init-next/content_runtime.h"
#include "../../../../../sdk-init-next/intent_launch_host.h"
#include "../../../../../native-window-next/window_runtime.h"

using LaunchTraceFn=int(*)(FILE*,const char*,...);
struct LaunchInspectionHost { LaunchTraceFn trace; };
static int InspectGuestLaunch(void *user,const intent_launch_request_v2 *request) {
    auto *host=static_cast<LaunchInspectionHost*>(user);
    if(!request||request->abi_version!=1||request->struct_size!=sizeof(*request))return -1;
    if(host&&host->trace)host->trace(stderr,
        "HOST_INTENT_REQUEST operation=%u caller=%u intent=%u package=%s class=%s flags=%u request_code=%d enter_animation=%d exit_animation=%d disposition=unsupported activity_created=0 engine_started=0\n",
        request->operation,request->caller_ref,request->intent_ref,
        request->package_name?request->package_name:"(none)",
        request->class_name?request->class_name:"(none)",request->flags,
        request->request_code,request->enter_animation,request->exit_animation);
    /* This bridge makes the real target visible; the Android Activity/Unity
     * backend is not installed yet. Never acknowledge a launch as accepted. */
    return 1;
}
#include "../../../../../sdk-init-next/network_runtime.h"
// R2 原生库“只加载、不调用”诊断探针。当前 CMakeLists.txt 只编译 probe.cpp，
// 且不在本任务允许修改的范围内，因此以单一翻译单元方式包含实现文件。
// native_probe.c 必须兼容 C++17（显式指针转换、无 VLA、无 C 指定初始化器）。
// 若后续把 native_probe.c 加入 CMakeLists 源列表，必须删除下面这行文本包含。
#include "../../../../../native-loader-next/native_probe.h"
#include "../../../../../native-loader-next/native_probe.c"
static std::mutex batteryLock;
static std::condition_variable batteryChanged;
static battery_host_emit batteryEmit=nullptr; static void* batteryUser=nullptr;
static battery_host_event batterySnapshot{}; static bool batteryHave=false,batteryAck=false,batteryAccepted=false;
static int64_t batterySequence=0;
static bool BatteryNotify(const std::string& message) {
 napi_threadsafe_function fn=nullptr; {std::lock_guard<std::mutex> g(subscriberLock); if(subscriber && napi_acquire_threadsafe_function(subscriber)==napi_ok) fn=subscriber;}
 if(!fn)return false; auto* p=new std::string(message); auto rc=napi_call_threadsafe_function(fn,p,napi_tsfn_nonblocking); if(rc!=napi_ok)delete p; napi_release_threadsafe_function(fn,napi_tsfn_release); return rc==napi_ok;
}
static int BatterySubscribe(void*,battery_host_emit emit,void* user) {
 std::unique_lock<std::mutex> g(batteryLock); if(batteryEmit)return -1;
 batteryEmit=emit; batteryUser=user; batteryHave=false; batteryAck=false; batteryAccepted=false; auto seq=++batterySequence;
 if(!BatteryNotify("BATTERY_START:"+std::to_string(seq)) || !batteryChanged.wait_for(g,std::chrono::seconds(5),[seq]{return batterySequence!=seq || batteryAck;} ) || batterySequence!=seq || !batteryAccepted) { batteryEmit=nullptr; batteryUser=nullptr; ++batterySequence; BatteryNotify("BATTERY_STOP:"+std::to_string(seq)); return -1; } return 0;
}
static void BatteryUnsubscribe(void*,void* user) { std::lock_guard<std::mutex> g(batteryLock); if(batteryUser!=user)return; batteryEmit=nullptr; batteryUser=nullptr; batteryHave=false; auto seq=batterySequence++; batteryChanged.notify_all(); BatteryNotify("BATTERY_STOP:"+std::to_string(seq)); }
static int BatterySnapshot(void*,battery_host_event* out) {
 { std::lock_guard<std::mutex> g(batteryLock); if(batteryHave){*out=batterySnapshot;return 0;} }
 // Sticky queries do not subscribe. Read the public native battery API when
 // there is no active ArkTS subscription; never invent an unknown extra.
 static void* lib=dlopen("libohbattery_info.so",RTLD_NOW|RTLD_LOCAL);
 static auto capacity=lib?reinterpret_cast<int32_t(*)()>(dlsym(lib,"OH_BatteryInfo_GetCapacity")):nullptr;
 static auto plugged=lib?reinterpret_cast<int32_t(*)()>(dlsym(lib,"OH_BatteryInfo_GetPluggedType")):nullptr;
 if(!capacity)return -1;int level=capacity();if(level<0||level>100)return -1;
 battery_host_event value{};value.present_mask=BATTERY_HAS_LEVEL|BATTERY_HAS_SCALE;value.level=level;value.scale=100;
 int supply=plugged?plugged():-1;
 if(supply>=0&&supply<=3){value.present_mask|=BATTERY_HAS_PLUGGED;value.plugged=supply==3?4:supply;}
 *out=value;
 OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbe","BATTERY_STICKY_NATIVE level=%{public}d scale=100 plugged=%{public}d mask=%{public}llu subscription_created=0",level,value.plugged,(unsigned long long)value.present_mask);
 return 0;
}
static napi_value BatteryUpdate(napi_env env,napi_callback_info info) {
 napi_value a[11],v; size_t n=11; int64_t seq=0; bool accepted=false, delivered=false;
 if(napi_get_cb_info(env,info,&n,a,nullptr,nullptr)!=napi_ok || n!=11 || napi_get_value_int64(env,a[0],&seq)!=napi_ok || napi_get_value_bool(env,a[1],&accepted)!=napi_ok || napi_get_value_bool(env,a[2],&delivered)!=napi_ok) { napi_throw_type_error(env,nullptr,"Invalid battery update"); return nullptr; }
 battery_host_event e{}; int32_t* fields[]={&e.level,&e.status,&e.health,&e.plugged,&e.temperature,&e.voltage,&e.present};
 for(int i=0;i<7;i++) if(napi_get_value_int32(env,a[3+i],fields[i])!=napi_ok) { napi_throw_type_error(env,nullptr,"Invalid battery field"); return nullptr; }
 size_t len=0; if(napi_get_value_string_utf8(env,a[10],e.technology,sizeof(e.technology),&len)!=napi_ok) { napi_throw_type_error(env,nullptr,"Invalid battery technology"); return nullptr; }
 e.scale=100; e.present_mask=BATTERY_HAS_LEVEL | BATTERY_HAS_SCALE | BATTERY_HAS_STATUS | BATTERY_HAS_HEALTH | BATTERY_HAS_PLUGGED | BATTERY_HAS_TEMPERATURE | BATTERY_HAS_VOLTAGE | BATTERY_HAS_PRESENT | BATTERY_HAS_TECHNOLOGY;
 {std::lock_guard<std::mutex> g(batteryLock); if(seq==batterySequence && batteryEmit) { if(accepted) { e.generation=batterySnapshot.generation+1; batterySnapshot=e; batteryHave=true; if(delivered) batteryEmit(batteryUser,&e); } else if(delivered) { batteryEmit=nullptr; batteryUser=nullptr; batteryHave=false; } if(!delivered) {batteryAccepted=accepted;batteryAck=true;batteryChanged.notify_all();} }}
 napi_get_undefined(env,&v);return v;
}

/* ---- HarmonyOS host bridge ------------------------------------------------
 * All @ohos APIs used by this bridge are ArkTS-only.  The VM runs on a NAPI
 * async-work worker thread, so a host callback running there cannot call them
 * directly.  The worker therefore posts a synchronous RPC over a dedicated
 * threadsafe function and waits (5 s deadline) for the ArkTS main thread to
 * answer through probe.hostReply / probe.hostReplyNetwork.  The ArkTS side
 * never touches the VM.  Only the worker thread reads or writes the VM.
 *
 * The display snapshot is different: ArkTS measures it before the run and
 * caches it here, and the worker installs it immediately before sdk_trace_run
 * and restores the diagnostic fallback afterwards. */
static napi_value Str(napi_env env,const std::string& s);
struct PlatformDisplaySnapshot {
    uint32_t struct_size;
    int32_t width_pixels,height_pixels,density_dpi,orientation;
    int32_t screen_width_dp,screen_height_dp,smallest_screen_width_dp;
    float density,scaled_density,xdpi,ydpi;
    const char* source;
};
using SetSettingsHostFn=void(*)(const content_settings_host*,void*);
using SetNetworkHostFn=void(*)(const network_host_bridge*);
using SetDisplaySnapshotFn=bool(*)(const PlatformDisplaySnapshot*);
enum HostOperation { HOST_OP_SETTINGS_GET=1, HOST_OP_SETTINGS_PUT=2, HOST_OP_NETWORK=3, HOST_OP_WINDOW_FEATURE=4, HOST_OP_SYSTEM_UI=5, HOST_OP_ROTATION=6, HOST_OP_UNITY_MOUNT=7, HOST_OP_UNITY_FOCUS=8, HOST_OP_UNITY_STATE=9, HOST_OP_WINDOW_FLAGS=10, HOST_OP_AUDIO_VOLUME=11, HOST_OP_DRAWN=12, HOST_OP_WEB_PLATFORM=13 };
struct HostCall {
    uint64_t id=0;
    int operation=0;
    bool done=false;
    int status=-1;
    std::string value;
    network_host_snapshot snapshot{};
};
static std::mutex hostRpcLock;
static std::condition_variable hostRpcChanged;
static std::shared_ptr<HostCall> hostRpcPending;
static uint64_t hostRpcSequence=0;
static std::mutex hostHandlerLock;
static napi_threadsafe_function hostHandler=nullptr;
static napi_env hostHandlerEnv=nullptr;
static std::atomic<bool> hostHandlerReady{false};
static std::atomic<uint64_t> hostSettingsCalls{0},hostSettingsFailures{0};
static std::atomic<uint64_t> hostNetworkCalls{0},hostNetworkFailures{0};
static std::atomic<uint64_t> hostRpcTimeouts{0};
static std::mutex hostInfoLock;
static PlatformDisplaySnapshot hostDisplaySnapshot{};
static std::string hostDisplaySource;
static bool hostDisplayPresent=false;
static uint64_t hostDisplayGeneration=0;
static std::string hostDisplayLastApply="not-run";
static std::string hostBridgeNote;
static std::atomic<bool> hostSettingsInstalled{false},hostNetworkInstalled{false};
static std::atomic<int> hostSettingsSymbol{-1},hostNetworkSymbol{-1},hostDisplaySymbol{-1},hostIntentSymbol{-1};

static bool ReadString(napi_env env,napi_value value,std::string& out) {
    size_t length=0;
    if(napi_get_value_string_utf8(env,value,nullptr,0,&length)!=napi_ok)return false;
    std::vector<char> buffer(length+1);
    size_t copied=0;
    if(napi_get_value_string_utf8(env,value,buffer.data(),buffer.size(),&copied)!=napi_ok)return false;
    out.assign(buffer.data(),copied);
    return true;
}
// Device i18n snapshot only: worker callbacks never reenter ArkTS or guess a zone.
static std::mutex timeZoneSnapshotLock;
static std::string timeZoneSnapshotId;
static int timeZoneRawOffsetMs=0;
static bool timeZoneSnapshotPresent=false;
static bool QueryTimeZoneSnapshot(char* id,size_t capacity,int* rawOffsetMs) {
    if(!id||!capacity||!rawOffsetMs)return false;
    std::lock_guard<std::mutex> guard(timeZoneSnapshotLock);
    if(!timeZoneSnapshotPresent||capacity<=timeZoneSnapshotId.size())return false;
    std::memcpy(id,timeZoneSnapshotId.c_str(),timeZoneSnapshotId.size()+1);
    *rawOffsetMs=timeZoneRawOffsetMs;
    return true;
}
static napi_value SetTimeZoneSnapshot(napi_env env,napi_callback_info info) {
    size_t count=2;napi_value args[2];std::string id;double offset=0;
    if(napi_get_cb_info(env,info,&count,args,nullptr,nullptr)!=napi_ok||count!=2||
       !ReadString(env,args[0],id)||id.empty()||id.size()>255||id.find('\0')!=std::string::npos||
       napi_get_value_double(env,args[1],&offset)!=napi_ok||!std::isfinite(offset)||
       offset<INT_MIN||offset>INT_MAX||std::trunc(offset)!=offset) {
        napi_throw_type_error(env,nullptr,"Expected a device time zone ID and integer raw offset milliseconds");return nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(timeZoneSnapshotLock);
        timeZoneSnapshotId=id;timeZoneRawOffsetMs=static_cast<int>(offset);timeZoneSnapshotPresent=true;
    }
    napi_value result;napi_get_undefined(env,&result);return result;
}
static bool Unreserved(unsigned char c) {
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~';
}
static std::string PercentEncode(const char* text) {
    static const char hex[]="0123456789ABCDEF";
    std::string out;
    if(!text)return out;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(text);*p;++p) {
        unsigned char c=*p;
        if(Unreserved(c))out.push_back(static_cast<char>(c));
        else {out.push_back('%');out.push_back(hex[c>>4]);out.push_back(hex[c&15]);}
    }
    return out;
}
static int HexValue(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static std::string PercentDecode(const std::string& text) {
    std::string out;out.reserve(text.size());
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='%'&&i+2<text.size()) {
            int hi=HexValue(text[i+1]),lo=HexValue(text[i+2]);
            if(hi>=0&&lo>=0) {out.push_back(static_cast<char>((hi<<4)|lo));i+=2;continue;}
        }
        out.push_back(text[i]);
    }
    return out;
}
static void ParseForm(const std::string& payload,std::map<std::string,std::string>& fields) {
    size_t start=0;
    while(start<=payload.size()) {
        size_t end=payload.find('&',start);
        if(end==std::string::npos)end=payload.size();
        if(end>start) {
            size_t eq=payload.find('=',start);
            if(eq!=std::string::npos&&eq<end) {
                std::string key=payload.substr(start,eq-start);
                if(!key.empty())fields[key]=PercentDecode(payload.substr(eq+1,end-eq-1));
            }
        }
        if(end==payload.size())break;
        start=end+1;
    }
}
static int64_t FormInt64(const std::map<std::string,std::string>& fields,const char* key,int64_t fallback) {
    auto it=fields.find(key);
    if(it==fields.end()||it->second.empty())return fallback;
    errno=0;char* end=nullptr;
    long long parsed=strtoll(it->second.c_str(),&end,10);
    if(errno!=0||end==it->second.c_str())return fallback;
    return static_cast<int64_t>(parsed);
}
static int32_t FormI32(const std::map<std::string,std::string>& fields,const char* key,int32_t fallback=0) {
    int64_t value=FormInt64(fields,key,fallback);
    if(value<INT32_MIN||value>INT32_MAX)return fallback;
    return static_cast<int32_t>(value);
}
static uint32_t FormU32(const std::map<std::string,std::string>& fields,const char* key,uint32_t fallback=0) {
    int64_t value=FormInt64(fields,key,static_cast<int64_t>(fallback));
    if(value<0||static_cast<uint64_t>(value)>UINT32_MAX)return fallback;
    return static_cast<uint32_t>(value);
}
static uint64_t FormU64(const std::map<std::string,std::string>& fields,const char* key,uint64_t fallback=0) {
    auto it=fields.find(key);
    if(it==fields.end()||it->second.empty())return fallback;
    errno=0;char* end=nullptr;
    unsigned long long parsed=strtoull(it->second.c_str(),&end,10);
    if(errno!=0||end==it->second.c_str())return fallback;
    return static_cast<uint64_t>(parsed);
}
static void CopyFormString(char* destination,size_t capacity,const std::map<std::string,std::string>& fields,const char* key) {
    if(capacity==0)return;
    auto it=fields.find(key);
    if(it==fields.end()) {destination[0]='\0';return;}
    size_t length=it->second.size();
    if(length>=capacity)length=capacity-1;
    std::memcpy(destination,it->second.data(),length);
    destination[length]='\0';
}
static std::shared_ptr<HostCall> BeginHostCall(int operation) {
    std::lock_guard<std::mutex> guard(hostRpcLock);
    if(!hostHandlerReady.load()||hostRpcPending)return nullptr;
    auto call=std::make_shared<HostCall>();
    call->id=++hostRpcSequence;
    call->operation=operation;
    hostRpcPending=call;
    return call;
}
static void AbandonHostCall(const std::shared_ptr<HostCall>& call) {
    std::lock_guard<std::mutex> guard(hostRpcLock);
    if(hostRpcPending==call)hostRpcPending.reset();
}
static bool SubmitHostCall(const std::shared_ptr<HostCall>& call,const std::string& message) {
    napi_threadsafe_function fn=nullptr;
    {
        std::lock_guard<std::mutex> guard(hostHandlerLock);
        if(hostHandler&&napi_acquire_threadsafe_function(hostHandler)==napi_ok)fn=hostHandler;
    }
    if(!fn) {AbandonHostCall(call);return false;}
    auto* payload=new std::string(message);
    napi_status status=napi_call_threadsafe_function(fn,payload,napi_tsfn_nonblocking);
    napi_release_threadsafe_function(fn,napi_tsfn_release);
    if(status!=napi_ok) {delete payload;AbandonHostCall(call);return false;}
    return true;
}
static bool WaitHostCall(const std::shared_ptr<HostCall>& call,int timeoutMs) {
    std::unique_lock<std::mutex> guard(hostRpcLock);
    bool completed=hostRpcChanged.wait_for(guard,std::chrono::milliseconds(timeoutMs),[&call]{return call->done;});
    if(!completed) {
        if(hostRpcPending==call)hostRpcPending.reset();
        return false;
    }
    return true;
}
static bool TakeHostCall(uint64_t id,int operation,std::shared_ptr<HostCall>& out) {
    std::lock_guard<std::mutex> guard(hostRpcLock);
    if(!hostRpcPending||hostRpcPending->id!=id||hostRpcPending->operation!=operation||hostRpcPending->done)return false;
    out=hostRpcPending;
    return true;
}
static void FinishHostCall(const std::shared_ptr<HostCall>& call,int status,const std::string& value,const network_host_snapshot* snapshot) {
    {
        std::lock_guard<std::mutex> guard(hostRpcLock);
        if(hostRpcPending!=call||call->done)return;
        call->status=status;
        call->value=value;
        if(snapshot)call->snapshot=*snapshot;
        call->done=true;
        hostRpcPending.reset();
    }
    hostRpcChanged.notify_all();
}
static void AbortPendingHostCall() {
    std::shared_ptr<HostCall> pending;
    {
        std::lock_guard<std::mutex> guard(hostRpcLock);
        pending=hostRpcPending;
        hostRpcPending.reset();
        if(pending) {pending->status=-1;pending->done=true;}
    }
    if(pending)hostRpcChanged.notify_all();
}
static int RequestHostRotation(void*,int *rotation) {
    if(!rotation)return -1;
    auto call=BeginHostCall(HOST_OP_ROTATION);
    if(!call||!SubmitHostCall(call,"display_rotation|"+std::to_string(call->id))||!WaitHostCall(call,5000)||call->status!=1)return -1;
    if(call->value.size()!=1||call->value[0]<'0'||call->value[0]>'3')return -1;
    *rotation=call->value[0]-'0';return 0;
}
// VM worker waits; only ArkTS's main thread invokes ArkWeb. The response is
// bounded but never truncated. No URL, UA, or Cookie value enters diagnostics.
static int RequestWebPlatform(int operation,uint32_t web,const char* arg0,const char* arg1,char* out,size_t capacity) {
    if(!out||!capacity||operation<1||operation>13)return -1;
    out[0]='\0';
    if((arg0&&std::strlen(arg0)>65535)||(arg1&&std::strlen(arg1)>65535))return -1;
    auto call=BeginHostCall(HOST_OP_WEB_PLATFORM);
    if(!call)return -1;
    std::string message="web_platform|"+std::to_string(call->id)+"|"+std::to_string(operation)+"|"+
        std::to_string(web)+"|"+PercentEncode(arg0)+"|"+PercentEncode(arg1);
    if(!SubmitHostCall(call,message))return -1;
    if(!WaitHostCall(call,5000)) {
        hostRpcTimeouts.fetch_add(1);
        OH_LOG_Print(LOG_APP,LOG_ERROR,0xFF00,"NativeProbe","WEB_COOKIE_RPC op=%{public}d error=timeout",operation);
        return -1;
    }
    if(call->status!=1)return call->status==-2?-2:-1;
    if(call->value.size()>=capacity)return -1;
    std::memcpy(out,call->value.c_str(),call->value.size()+1);
    return 1;
}
static int RequestUnityViewFocus(void*,uint32_t) {
    if(!unityWindowOwned.load())return -1;
    auto call=BeginHostCall(HOST_OP_UNITY_FOCUS);
    if(!call||!SubmitHostCall(call,"unity_focus|"+std::to_string(call->id))||!WaitHostCall(call,5000))return -1;
    return call->status==1?1:(call->status==0?0:-1);
}
static int QueryUnityActivityState(void*,int* showing,int* focused) {
    if(!showing||!focused)return -1;
    *showing=*focused=0;
    auto call=BeginHostCall(HOST_OP_UNITY_STATE);
    if(!call||!SubmitHostCall(call,"unity_state|"+std::to_string(call->id))||!WaitHostCall(call,5000)||call->status!=1)return -1;
    if(call->value!="0"&&call->value!="1"&&call->value!="2"&&call->value!="3")return -1;
    int flags=call->value[0]-'0';*showing=flags&1;*focused=(flags>>1)&1;return 0;
}
static int RequestHostSystemUi(void*,int flags) {
    auto call=BeginHostCall(HOST_OP_SYSTEM_UI);
    if(!call)return -1;
    if(!SubmitHostCall(call,"system_ui|"+std::to_string(call->id)+"|"+std::to_string(flags)))return -1;
    if(!WaitHostCall(call,5000))return -1;
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbe","system_ui flags=%{public}d status=%{public}d layout_verified_and_bar_request_ack_only=1",flags,call->status);
    return call->status==0?0:-1;
}
static int RequestHostWindowFlags(void*,int flags,int mask) {
    auto call=BeginHostCall(HOST_OP_WINDOW_FLAGS);if(!call)return -1;
    if(!SubmitHostCall(call,"window_flags|"+std::to_string(call->id)+"|"+std::to_string(flags)+"|"+std::to_string(mask))||!WaitHostCall(call,5000))return -1;
    return call->status==0?0:-1;
}
static int QueryHostAudioVolume(void*,int stream_type,int* volume) {
    if(!volume||stream_type<0)return -1;
    auto call=BeginHostCall(HOST_OP_AUDIO_VOLUME);
    if(!call)return -1;
    if(!SubmitHostCall(call,"audio_volume|"+std::to_string(call->id)+"|"+std::to_string(stream_type)))return -1;
    if(!WaitHostCall(call,5000)){hostRpcTimeouts.fetch_add(1);return -1;}
    if(call->status!=1||call->value.empty())return -1;
    int value=0;
    for(char c:call->value){if(c<'0'||c>'9')return -1;value=value*10+(c-'0');if(value>32767)return -1;}
    *volume=value;return 0;
}
static int ReportHostDrawn(void*) {
    auto call=BeginHostCall(HOST_OP_DRAWN);
    if(!call||!SubmitHostCall(call,"drawn_complete|"+std::to_string(call->id)))return -1;
    if(!WaitHostCall(call,5000)){hostRpcTimeouts.fetch_add(1);return -1;}
    return call->status;
}
static int RequestHostWindowFeature(void*,int feature) {
    if(feature!=1)return -1;
    auto call=BeginHostCall(HOST_OP_WINDOW_FEATURE);
    if(!call)return -1;
    if(!SubmitHostCall(call,"window_feature|"+std::to_string(call->id)+"|"+std::to_string(feature)))return -1;
    if(!WaitHostCall(call,5000)) {hostRpcTimeouts.fetch_add(1);return -1;}
    // The UI handler acknowledges only after applying and reading back the
    // actual OH window decoration state. Unsupported is never success.
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbe","window_feature=%{public}d host_status=%{public}d",feature,call->status);
    return call->status==0 ? 0 : (call->status==1 ? 1 : -1);
}
static int SettingsGet(void*,int domain,const char* name,char* value,size_t capacity) {
    if(!name||!value||capacity==0||domain<CONTENT_SETTINGS_GLOBAL||domain>CONTENT_SETTINGS_SECURE)return -1;
    hostSettingsCalls.fetch_add(1);
    auto call=BeginHostCall(HOST_OP_SETTINGS_GET);
    if(!call) {hostSettingsFailures.fetch_add(1);return -1;}
    std::string message="settings_get|"+std::to_string(call->id)+"|"+std::to_string(domain)+"|"+PercentEncode(name);
    if(!SubmitHostCall(call,message)) {hostSettingsFailures.fetch_add(1);return -1;}
    if(!WaitHostCall(call,5000)) {hostRpcTimeouts.fetch_add(1);hostSettingsFailures.fetch_add(1);return -1;}
    if(call->status==1) {
        if(call->value.size()>=capacity) {hostSettingsFailures.fetch_add(1);return -1;}
        std::memcpy(value,call->value.c_str(),call->value.size()+1);
        return 1;
    }
    if(call->status==0)return 0;
    if(call->status==-2)return -2;
    hostSettingsFailures.fetch_add(1);
    return -1;
}
static int SettingsPut(void*,int domain,const char* name,const char* value) {
    if(!name||domain<CONTENT_SETTINGS_GLOBAL||domain>CONTENT_SETTINGS_SECURE)return -1;
    hostSettingsCalls.fetch_add(1);
    auto call=BeginHostCall(HOST_OP_SETTINGS_PUT);
    if(!call) {hostSettingsFailures.fetch_add(1);return -1;}
    std::string message;
    if(value)message="settings_put|"+std::to_string(call->id)+"|"+std::to_string(domain)+"|"+PercentEncode(name)+"|"+PercentEncode(value);
    else message="settings_delete|"+std::to_string(call->id)+"|"+std::to_string(domain)+"|"+PercentEncode(name);
    if(!SubmitHostCall(call,message)) {hostSettingsFailures.fetch_add(1);return -1;}
    if(!WaitHostCall(call,5000)) {hostRpcTimeouts.fetch_add(1);hostSettingsFailures.fetch_add(1);return -1;}
    if(call->status==0)return 0;
    hostSettingsFailures.fetch_add(1);
    return call->status==-2 ? -2 : -1;
}
static int NetworkSnapshot(void*,network_host_snapshot* out) {
    if(!out)return -1;
    hostNetworkCalls.fetch_add(1);
    auto call=BeginHostCall(HOST_OP_NETWORK);
    if(!call) {hostNetworkFailures.fetch_add(1);return -1;}
    if(!SubmitHostCall(call,"network|"+std::to_string(call->id))) {hostNetworkFailures.fetch_add(1);return -1;}
    if(!WaitHostCall(call,5000)) {hostRpcTimeouts.fetch_add(1);hostNetworkFailures.fetch_add(1);return -1;}
    if(call->status!=0) {hostNetworkFailures.fetch_add(1);return call->status;}
    if(call->snapshot.version!=NETWORK_HOST_SNAPSHOT_VERSION||call->snapshot.size!=sizeof(network_host_snapshot)) {
        hostNetworkFailures.fetch_add(1);
        return -1;
    }
    *out=call->snapshot;
    return 0;
}
static bool ApplyCachedDisplay(PlatformDisplaySnapshot* out,std::string& source) {
    std::lock_guard<std::mutex> guard(hostInfoLock);
    if(!hostDisplayPresent)return false;
    *out=hostDisplaySnapshot;
    source=hostDisplaySource;
    out->source=source.c_str();
    return true;
}
static void HostCallJs(napi_env env,napi_value callback,void*,void* data) {
    std::unique_ptr<std::string> message(static_cast<std::string*>(data));
    if(!env||!callback||!message)return;
    napi_value arg,global;
    if(napi_create_string_utf8(env,message->c_str(),NAPI_AUTO_LENGTH,&arg)!=napi_ok)return;
    if(napi_get_undefined(env,&global)!=napi_ok)return;
    napi_status status=napi_call_function(env,global,callback,1,&arg,nullptr);
    if(status!=napi_ok) {
        napi_value ignored;
        napi_get_and_clear_last_exception(env,&ignored);
    }
}
static void HostEnvCleanup(void* data) {
    napi_threadsafe_function old=nullptr;
    {
        std::lock_guard<std::mutex> guard(hostHandlerLock);
        if(hostHandlerEnv==static_cast<napi_env>(data)) {
            old=hostHandler;hostHandler=nullptr;hostHandlerEnv=nullptr;
        }
    }
    hostHandlerReady.store(false);
    if(old)napi_release_threadsafe_function(old,napi_tsfn_abort);
    AbortPendingHostCall();
}
static void UnregisterHostInternal() {
    napi_threadsafe_function old=nullptr;napi_env oldEnv=nullptr;
    {
        std::lock_guard<std::mutex> guard(hostHandlerLock);
        old=hostHandler;oldEnv=hostHandlerEnv;hostHandler=nullptr;hostHandlerEnv=nullptr;
    }
    hostHandlerReady.store(false);
    if(oldEnv)napi_remove_env_cleanup_hook(oldEnv,HostEnvCleanup,oldEnv);
    if(old)napi_release_threadsafe_function(old,napi_tsfn_abort);
    AbortPendingHostCall();
}
static napi_value RegisterHostHandler(napi_env env,napi_callback_info info) {
    size_t argc=1;napi_value args[1];napi_valuetype type=napi_undefined;
    if(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr)!=napi_ok||argc!=1||
       napi_typeof(env,args[0],&type)!=napi_ok||type!=napi_function) {
        napi_throw_type_error(env,nullptr,"Expected host RPC callback");return nullptr;
    }
    napi_value name;
    if(napi_create_string_utf8(env,"probe host rpc",NAPI_AUTO_LENGTH,&name)!=napi_ok) {
        napi_throw_error(env,nullptr,"Cannot name host RPC callback");return nullptr;
    }
    napi_threadsafe_function fn=nullptr;
    if(napi_create_threadsafe_function(env,args[0],nullptr,name,0,1,nullptr,nullptr,nullptr,HostCallJs,&fn)!=napi_ok) {
        napi_throw_error(env,nullptr,"Cannot create host RPC callback");return nullptr;
    }
    UnregisterHostInternal();
    if(napi_add_env_cleanup_hook(env,HostEnvCleanup,env)!=napi_ok) {
        napi_release_threadsafe_function(fn,napi_tsfn_abort);
        napi_throw_error(env,nullptr,"Cannot register host cleanup hook");return nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(hostHandlerLock);
        hostHandler=fn;hostHandlerEnv=env;
    }
    hostHandlerReady.store(true);
    napi_value result;napi_get_undefined(env,&result);return result;
}
static napi_value UnregisterHostHandler(napi_env env,napi_callback_info) {
    UnregisterHostInternal();
    napi_value result;napi_get_undefined(env,&result);return result;
}
static napi_value HostReply(napi_env env,napi_callback_info info) {
    size_t argc=3;napi_value args[3];int64_t id=0;int32_t status=0;std::string value;
    if(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr)!=napi_ok||argc!=3||
       napi_get_value_int64(env,args[0],&id)!=napi_ok||
       napi_get_value_int32(env,args[1],&status)!=napi_ok||
       !ReadString(env,args[2],value)) {
        napi_throw_type_error(env,nullptr,"Expected host reply id, status and value");return nullptr;
    }
    std::shared_ptr<HostCall> call;
    if(TakeHostCall(static_cast<uint64_t>(id),HOST_OP_SETTINGS_GET,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_SETTINGS_PUT,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_WINDOW_FEATURE,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_SYSTEM_UI,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_ROTATION,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_UNITY_MOUNT,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_UNITY_FOCUS,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_UNITY_STATE,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_WINDOW_FLAGS,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_AUDIO_VOLUME,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_DRAWN,call)||
       TakeHostCall(static_cast<uint64_t>(id),HOST_OP_WEB_PLATFORM,call)) {
        const size_t limit=call->operation==HOST_OP_WEB_PLATFORM?65536:CONTENT_SETTINGS_HOST_VALUE_MAX;
        if(status==1&&value.size()>=limit)status=-1;
        FinishHostCall(call,status,status==1?value:std::string(),nullptr);
    }
    napi_value result;napi_get_undefined(env,&result);return result;
}
static napi_value HostReplyNetwork(napi_env env,napi_callback_info info) {
    size_t argc=3;napi_value args[3];int64_t id=0;int32_t status=0;std::string payload;
    if(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr)!=napi_ok||argc!=3||
       napi_get_value_int64(env,args[0],&id)!=napi_ok||
       napi_get_value_int32(env,args[1],&status)!=napi_ok||
       !ReadString(env,args[2],payload)) {
        napi_throw_type_error(env,nullptr,"Expected network reply id, status and payload");return nullptr;
    }
    std::shared_ptr<HostCall> call;
    if(TakeHostCall(static_cast<uint64_t>(id),HOST_OP_NETWORK,call)) {
        network_host_snapshot snapshot{};
        if(status==0) {
            std::map<std::string,std::string> fields;
            ParseForm(payload,fields);
            snapshot.version=NETWORK_HOST_SNAPSHOT_VERSION;
            snapshot.size=sizeof(snapshot);
            snapshot.connectivity_valid=FormI32(fields,"cv");
            snapshot.has_default_network=FormI32(fields,"hd");
            snapshot.connected=FormI32(fields,"co");
            snapshot.available=FormI32(fields,"av");
            snapshot.metered=FormI32(fields,"me");
            snapshot.type=FormI32(fields,"ty");
            snapshot.type_valid=FormI32(fields,"tv");
            snapshot.transports=FormU32(fields,"tr");
            snapshot.capabilities=FormU32(fields,"ca");
            snapshot.network_handle=FormU64(fields,"nh");
            snapshot.telephony_valid=FormI32(fields,"telv");
            snapshot.network_type=FormI32(fields,"nt");
            snapshot.network_type_valid=FormI32(fields,"ntv");
            snapshot.sim_state=FormI32(fields,"ss");
            snapshot.sim_state_valid=FormI32(fields,"ssv");
            snapshot.phone_type=FormI32(fields,"pt");
            snapshot.phone_type_valid=FormI32(fields,"ptv");
            snapshot.operator_numeric_valid=FormI32(fields,"onv");
            snapshot.operator_name_valid=FormI32(fields,"onav");
            CopyFormString(snapshot.operator_numeric,sizeof(snapshot.operator_numeric),fields,"on");
            CopyFormString(snapshot.operator_name,sizeof(snapshot.operator_name),fields,"ona");
        }
        FinishHostCall(call,status,std::string(),status==0?&snapshot:nullptr);
    }
    napi_value result;napi_get_undefined(env,&result);return result;
}
static napi_value SetDisplaySnapshot(napi_env env,napi_callback_info info) {
    size_t argc=12;napi_value args[12];int32_t ints[7];double floats[4];std::string source;
    if(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr)!=napi_ok||argc!=12) {
        napi_throw_type_error(env,nullptr,"Expected display snapshot fields");return nullptr;
    }
    for(int i=0;i<7;i++)if(napi_get_value_int32(env,args[i],&ints[i])!=napi_ok) {
        napi_throw_type_error(env,nullptr,"Invalid display integer field");return nullptr;
    }
    for(int i=0;i<4;i++)if(napi_get_value_double(env,args[7+i],&floats[i])!=napi_ok) {
        napi_throw_type_error(env,nullptr,"Invalid display float field");return nullptr;
    }
    if(!ReadString(env,args[11],source)) {
        napi_throw_type_error(env,nullptr,"Invalid display source");return nullptr;
    }
    PlatformDisplaySnapshot snapshot{};
    snapshot.struct_size=sizeof(snapshot);
    snapshot.width_pixels=ints[0];snapshot.height_pixels=ints[1];
    snapshot.density_dpi=ints[2];snapshot.orientation=ints[3];
    snapshot.screen_width_dp=ints[4];snapshot.screen_height_dp=ints[5];
    snapshot.smallest_screen_width_dp=ints[6];
    snapshot.density=static_cast<float>(floats[0]);
    snapshot.scaled_density=static_cast<float>(floats[1]);
    snapshot.xdpi=static_cast<float>(floats[2]);
    snapshot.ydpi=static_cast<float>(floats[3]);
    snapshot.source=nullptr;
    {
        std::lock_guard<std::mutex> guard(hostInfoLock);
        hostDisplaySnapshot=snapshot;
        hostDisplaySource=source;
        hostDisplayPresent=true;
        ++hostDisplayGeneration;
    }
    napi_value result;napi_get_undefined(env,&result);return result;
}
static napi_value ClearDisplaySnapshot(napi_env env,napi_callback_info) {
    {
        std::lock_guard<std::mutex> guard(hostInfoLock);
        hostDisplayPresent=false;hostDisplaySource.clear();
        hostDisplaySnapshot=PlatformDisplaySnapshot{};
    }
    napi_value result;napi_get_undefined(env,&result);return result;
}
static const char* SymbolState(int value) {
    if(value<0)return "not-probed";
    return value?"present":"missing";
}
static napi_value HostStatus(napi_env env,napi_callback_info) {
    std::ostringstream out;
    out<<"HOST_BRIDGE_STATUS v1\n";
    out<<"arkts_handler="<<(hostHandlerReady.load()?"registered":"unregistered")<<"\n";
    out<<"settings_runtime_host="<<(hostSettingsInstalled.load()?"installed-during-run":"not-installed")<<" symbol="<<SymbolState(hostSettingsSymbol.load())<<"\n";
    out<<"network_runtime_host="<<(hostNetworkInstalled.load()?"installed-during-run":"not-installed")<<" symbol="<<SymbolState(hostNetworkSymbol.load())<<"\n";
    out<<"display_runtime_symbol="<<SymbolState(hostDisplaySymbol.load())<<"\n";
    out<<"intent_launch=v2-inspection-only symbol="<<SymbolState(hostIntentSymbol.load())<<"\n";
    out<<"intent_launch_blocker=typed requests can expose the original target; no Activity/Unity host backend is installed, so requests remain unsupported.\n";
    {
        std::lock_guard<std::mutex> guard(hostInfoLock);
        out<<"display_snapshot=";
        if(hostDisplayPresent) {
            const auto& s=hostDisplaySnapshot;
            out<<"present gen="<<hostDisplayGeneration<<" w="<<s.width_pixels<<" h="<<s.height_pixels
               <<" dpi="<<s.density_dpi<<" density="<<s.density<<" scaled="<<s.scaled_density
               <<" xdpi="<<s.xdpi<<" ydpi="<<s.ydpi<<" orientation="<<s.orientation
               <<" screen_dp="<<s.screen_width_dp<<"x"<<s.screen_height_dp
               <<" smallest_dp="<<s.smallest_screen_width_dp<<" source="<<hostDisplaySource;
        } else out<<"none (runtime diagnostic fallback, display_host=0)";
        out<<"\n";
        out<<"display_last_trace="<<hostDisplayLastApply<<"\n";
        if(!hostBridgeNote.empty())out<<"note="<<hostBridgeNote<<"\n";
    }
    size_t pending=0;
    {
        std::lock_guard<std::mutex> guard(hostRpcLock);
        pending=hostRpcPending?1:0;
    }
    out<<"rpc_pending="<<pending
       <<" settings_calls="<<hostSettingsCalls.load()<<" settings_failures="<<hostSettingsFailures.load()
       <<" network_calls="<<hostNetworkCalls.load()<<" network_failures="<<hostNetworkFailures.load()
       <<" timeouts="<<hostRpcTimeouts.load()<<"\n";
    out<<"threading=@ohos APIs run on the ArkTS main thread; host callbacks run on the VM worker and use a 5 s synchronous TSFN RPC; ArkTS never touches the VM; env cleanup aborts pending calls and restores fallbacks.\n";
    out<<"abi=settings get 1/0/-2/negative, put 0/nonzero; network snapshot v"<<NETWORK_HOST_SNAPSHOT_VERSION<<" size="<<sizeof(network_host_snapshot)<<"; display struct_size="<<sizeof(PlatformDisplaySnapshot)<<"\n";
    out<<"settings_domains=supported Global(0)->deviceShared System(1)->userProperty; unsupported Secure(2)->no public USER_SECURE domain, host replies -1 (fail-closed)\n";
    out<<"settings_key_mapping=Global.airplane_mode_on->general.AIRPLANE_MODE_STATUS; System.screen_brightness->display.SCREEN_BRIGHTNESS_STATUS; System.font_scale->display.FONT_SCALE; System.screen_off_timeout->display.SCREEN_OFF_TIMEOUT; unlisted key -> -1 key mapping missing (never 0 absent)\n";
    out<<"capability_evidence=settings: four confirmed mapped keys only; network: @ohos.net.connection/@ohos.telephony snapshot; display: @ohos.display.getDefaultDisplaySync plus windowRect snapshot; separate sources, not a claim that all three bridges are verified end-to-end.\n";
    out<<"failure_policy=fail-closed: permission -> -2; unsupported domain/unmapped key/unregistered/backend/timeout -> negative; 0 only for a confirmed mapped key proven absent; no fabricated online, airplane-mode or display values.\n";
    return Str(env,out.str());
}

static std::string Snapshot() {
    std::lock_guard<std::mutex> guard(lock);
    std::string s=graphics+"\nevents="+std::to_string(touchEvents)+" errors="+std::to_string(touchErrors)+" cancels="+std::to_string(cancelEvents)+" queueFailures="+std::to_string(enqueueFailures.load())+"\n";
    for(const auto& entry:pointers) { const auto& p=entry.second; s+="id="+std::to_string(entry.first)+" down="+std::to_string(p.down)+" up="+std::to_string(p.up)+" cancel="+std::to_string(p.cancel)+" active="+std::to_string(p.active)+" anomalies="+std::to_string(p.anomalous)+"\n"; }
    return s+touches;
}
static void CallJs(napi_env env,napi_value callback,void*,void* data) {
    auto* message=static_cast<std::string*>(data);
    if(env && callback && message) { napi_value arg,global; if(napi_create_string_utf8(env,message->c_str(),NAPI_AUTO_LENGTH,&arg)==napi_ok && napi_get_undefined(env,&global)==napi_ok) napi_call_function(env,global,callback,1,&arg,nullptr); }
    delete message;
}
static void Notify() {
    napi_threadsafe_function fn=nullptr;
    { std::lock_guard<std::mutex> guard(subscriberLock); if(subscriber && napi_acquire_threadsafe_function(subscriber)==napi_ok) fn=subscriber; }
    if(!fn) return;
    auto* message=new std::string(Snapshot());
    napi_status status=napi_call_threadsafe_function(fn,message,napi_tsfn_nonblocking);
    if(status!=napi_ok) { delete message; ++enqueueFailures; }
    napi_release_threadsafe_function(fn,napi_tsfn_release);
}
// Login messages use the existing asynchronous subscriber, never platform RPC waits.
static std::atomic<void*> loginRuntime{nullptr};
static void LoginPanelEvent(const char* text) {
    napi_threadsafe_function fn=nullptr;
    { std::lock_guard<std::mutex> guard(subscriberLock); if(subscriber && napi_acquire_threadsafe_function(subscriber)==napi_ok) fn=subscriber; }
    if(!fn)return;
    auto* message=new std::string(text?text:"");
    if(napi_call_threadsafe_function(fn,message,napi_tsfn_nonblocking)!=napi_ok)delete message;
    napi_release_threadsafe_function(fn,napi_tsfn_release);
}
static napi_value LoginSubmit(napi_env env,napi_callback_info info) {
    size_t count=3;napi_value args[3];std::string id,user,password;
    if(napi_get_cb_info(env,info,&count,args,nullptr,nullptr)!=napi_ok||count!=3||!ReadString(env,args[0],id)||!ReadString(env,args[1],user)||!ReadString(env,args[2],password)) {
        napi_throw_type_error(env,nullptr,"Expected requestId, account, password");return nullptr;
    }
    void* runtime=loginRuntime.load();
    auto submit=runtime?reinterpret_cast<int(*)(const char*,const char*,const char*)>(dlsym(runtime,"login_panel_submit")):nullptr;
    int rc=submit?submit(id.c_str(),user.c_str(),password.c_str()):-1;
    volatile char* secret=password.empty()?nullptr:&password[0];for(size_t i=0;i<password.size();i++)secret[i]=0;
    napi_value result;napi_create_int32(env,rc,&result);return result;
}
static napi_value LoginSavedLogin(napi_env env,napi_callback_info info) {
    size_t count=1;napi_value args[1];std::string id;
    if(napi_get_cb_info(env,info,&count,args,nullptr,nullptr)!=napi_ok||count!=1||!ReadString(env,args[0],id)) {
        napi_throw_type_error(env,nullptr,"Expected requestId");return nullptr;
    }
    void* runtime=loginRuntime.load();
    auto submit=runtime?reinterpret_cast<int(*)(const char*)>(dlsym(runtime,"login_panel_saved_login")):nullptr;
    napi_value result;napi_create_int32(env,submit?submit(id.c_str()):-1,&result);return result;
}
static napi_value LoginCaptchaResult(napi_env env,napi_callback_info info) {
    size_t count=3;napi_value args[3];std::string id,payload,method;
    if(napi_get_cb_info(env,info,&count,args,nullptr,nullptr)!=napi_ok||count!=3||!ReadString(env,args[0],id)||!ReadString(env,args[1],payload)||!ReadString(env,args[2],method)||(method!="finishWithResult"&&method!="goBackWithResult")){napi_throw_type_error(env,nullptr,"Expected requestId, captcha payload, finishWithResult or goBackWithResult");return nullptr;}
    void* runtime=loginRuntime.load();auto send=runtime?reinterpret_cast<int(*)(const char*,const char*,const char*)>(dlsym(runtime,"login_panel_captcha_result")):nullptr;
    int rc=send?send(id.c_str(),payload.c_str(),method.c_str()):-1;volatile char* p=payload.empty()?nullptr:&payload[0];for(size_t i=0;i<payload.size();i++)p[i]=0;
    napi_value result;napi_create_int32(env,rc,&result);return result;
}
static napi_value LoginGetState(napi_env env,napi_callback_info) {
    char state[8192]="{\"ready\":false,\"required\":false,\"busy\":false,\"sessionId\":\"0\",\"activeRequestId\":\"\"}";
    void* runtime=loginRuntime.load();
    auto get=runtime?reinterpret_cast<int(*)(char*,size_t)>(dlsym(runtime,"login_panel_get_state")):nullptr;
    if(get)get(state,sizeof state);return Str(env,state);
}
static napi_value WebviewCommand(napi_env env,napi_callback_info info) {
    size_t count=3;napi_value args[3];std::string id,action,payload;
    if(napi_get_cb_info(env,info,&count,args,nullptr,nullptr)!=napi_ok||count!=3||!ReadString(env,args[0],id)||!ReadString(env,args[1],action)||!ReadString(env,args[2],payload)) {napi_throw_type_error(env,nullptr,"Expected WebView id, action and payload");return nullptr;}
    void* runtime=loginRuntime.load();auto send=runtime?reinterpret_cast<int(*)(const char*,const char*,const char*)>(dlsym(runtime,"webview_panel_command")):nullptr;
    int rc=send?send(id.c_str(),action.c_str(),payload.c_str()):-1;
    volatile char* p=payload.empty()?nullptr:&payload[0];for(size_t i=0;i<payload.size();i++)p[i]=0;
    napi_value result;napi_create_int32(env,rc,&result);return result;
}
static void StopRender();
static void EnvCleanup(void* data);
static void UnsubscribeInternal() {
    napi_threadsafe_function old=nullptr; napi_env oldEnv=nullptr;
    { std::lock_guard<std::mutex> guard(subscriberLock); old=subscriber; oldEnv=subscriberEnv; subscriber=nullptr; subscriberEnv=nullptr; }
    if(oldEnv) napi_remove_env_cleanup_hook(oldEnv,EnvCleanup,oldEnv);
    if(old) napi_release_threadsafe_function(old,napi_tsfn_abort);
}
static void EnvCleanup(void* data) {
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","HOST_LIFECYCLE NAPI.EnvCleanup");
    DetachHostWindow();
    napi_threadsafe_function old=nullptr;
    { std::lock_guard<std::mutex> guard(subscriberLock); if(subscriberEnv==static_cast<napi_env>(data)) { old=subscriber; subscriber=nullptr; subscriberEnv=nullptr; } }
    if(old) napi_release_threadsafe_function(old,napi_tsfn_abort);
    StopRender();
}
static napi_value Subscribe(napi_env env,napi_callback_info info) {
    size_t argc=1; napi_value args[1],name; napi_valuetype type;
    if(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr)!=napi_ok || argc!=1 || napi_typeof(env,args[0],&type)!=napi_ok || type!=napi_function) { napi_throw_type_error(env,nullptr,"Expected callback"); return nullptr; }
    napi_create_string_utf8(env,"probe events",NAPI_AUTO_LENGTH,&name); napi_threadsafe_function fn=nullptr;
    napi_status status=napi_create_threadsafe_function(env,args[0],nullptr,name,256,1,nullptr,nullptr,nullptr,CallJs,&fn);
    if(status!=napi_ok) { napi_throw_error(env,nullptr,"Cannot create event callback"); return nullptr; }
    UnsubscribeInternal();
    if(napi_add_env_cleanup_hook(env,EnvCleanup,env)!=napi_ok) { napi_release_threadsafe_function(fn,napi_tsfn_abort); napi_throw_error(env,nullptr,"Cannot register cleanup hook"); return nullptr; }
    { std::lock_guard<std::mutex> guard(subscriberLock); subscriber=fn; subscriberEnv=env; }
    Notify(); napi_value result; napi_get_undefined(env,&result); return result;
}
static napi_value Unsubscribe(napi_env env,napi_callback_info) { UnsubscribeInternal(); napi_value result; napi_get_undefined(env,&result); return result; }

static void StopRender() {
    stopRender.store(true); if(renderThread.joinable()) renderThread.join();
}
static int MountUnitySurface(void* runtime,void** ownedToken,int* androidFormat) {
    if(!runtime||!ownedToken||!androidFormat)return -EINVAL;
    *ownedToken=nullptr;*androidFormat=0;
    auto call=BeginHostCall(HOST_OP_UNITY_MOUNT);
    if(!call||!SubmitHostCall(call,"unity_mount|"+std::to_string(call->id))||
       !WaitHostCall(call,5000)||call->status!=1)return -ENODEV;
    // ArkUI confirmed that the existing XComponent has nonzero bounds in a
    // showing window. Keep its node alive: no replacement or generation change.
    unityWindowOwned.store(true);
    StopRender(); // join, unbind and destroy the diagnostic EGL context first
    using Acquire=void*(*)();using Release=void(*)(void*);
    auto acquire=reinterpret_cast<Acquire>(dlsym(runtime,"window_runtime_acquire_current"));
    auto release=reinterpret_cast<Release>(dlsym(runtime,"window_runtime_release"));
    if(!acquire||!release)return -ENOSYS;
    void* token=acquire();if(!token)return -ENODEV;
    int32_t actual=-1;
    {std::lock_guard<std::mutex> guard(nativeWindowLock);
     if(currentNativeWindow)OH_NativeWindow_NativeWindowHandleOpt(currentNativeWindow,GET_FORMAT,&actual);}
    switch(actual){
        case NATIVEBUFFER_PIXEL_FMT_RGBA_8888:*androidFormat=1;break;
        case NATIVEBUFFER_PIXEL_FMT_RGBX_8888:*androidFormat=2;break;
        case NATIVEBUFFER_PIXEL_FMT_RGB_565:*androidFormat=4;break;
        default:release(token);return -ENOTSUP;
    }
    *ownedToken=token;
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbe","UNITY_HOST_MOUNT real_xcomponent=1 diagnostic_render_stopped=1 android_format=%{public}d",*androidFormat);
    return 0;
}
static void Destroy(OH_NativeXComponent*, void*) {
    DetachHostWindow();
    StopRender(); { std::lock_guard<std::mutex> guard(lock); graphics="surface destroyed; render thread joined"; } Notify();
}
static void Render(void* window, uint64_t width, uint64_t height, int sizeResult) {
    EGLDisplay display=EGL_NO_DISPLAY; EGLSurface surface=EGL_NO_SURFACE; EGLContext context=EGL_NO_CONTEXT;
    struct Cleanup {
        EGLDisplay& display; EGLSurface& surface; EGLContext& context; void* window;
        ~Cleanup() { if(display!=EGL_NO_DISPLAY) { eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); if(surface!=EGL_NO_SURFACE) eglDestroySurface(display,surface); if(context!=EGL_NO_CONTEXT) eglDestroyContext(display,context); eglTerminate(display); } UnreferenceHostWindow(window); }
    } cleanup{display,surface,context,window};
    std::ostringstream info;
    int32_t fmt=-1, bw=-1, bh=-1;
    auto* nativeWindow=static_cast<OHNativeWindow*>(window);
    int formatResult=window ? OH_NativeWindow_NativeWindowHandleOpt(nativeWindow,GET_FORMAT,&fmt) : -1;
    int geometryResult=window ? OH_NativeWindow_NativeWindowHandleOpt(nativeWindow,GET_BUFFER_GEOMETRY,&bh,&bw) : -1;
    info << "window=" << (window ? "yes" : "null") << " size=" << width << "x" << height << " rc=" << sizeResult
         << " buffer=" << bw << "x" << bh << " rc=" << geometryResult << " format=" << fmt << " rc=" << formatResult << "\n";
    auto fail=[&](const char* stage, EGLint error) { info << "FAIL " << stage << " EGL=0x" << std::hex << error; { std::lock_guard<std::mutex> guard(lock); graphics=info.str(); } Notify(); };
    if(!window) { fail("null window",EGL_SUCCESS); return; }
    display=eglGetDisplay(EGL_DEFAULT_DISPLAY); EGLint error=eglGetError();
    if(display==EGL_NO_DISPLAY) { fail("eglGetDisplay",error); return; }
    EGLint major=0,minor=0;
    EGLBoolean success=eglInitialize(display,&major,&minor); error=eglGetError();
    if(!success) { fail("eglInitialize",error); return; }
    info << "EGL " << major << "." << minor << " ";
    success=eglBindAPI(EGL_OPENGL_ES_API); error=eglGetError();
    if(!success) { fail("eglBindAPI",error); return; }
    info << "API=0x" << std::hex << eglQueryAPI() << std::dec << " ";
    EGLConfig config=nullptr; EGLint n=0;
    const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    success=eglChooseConfig(display,attrs,nullptr,0,&n); error=eglGetError();
    info << "choose=" << success << " total=" << n << " err=0x" << std::hex << error << std::dec << " ";
    if(!success || n<=0) { fail("no window ES2 config",error); return; }
    if(formatResult!=0) { fail("native format unavailable",EGL_SUCCESS); return; }
    std::vector<EGLConfig> configs(static_cast<size_t>(n)); EGLint received=0;
    success=eglChooseConfig(display,attrs,configs.data(),n,&received); error=eglGetError();
    if(!success) { fail("enumerate EGL configs",error); return; }
    EGLint visual=-1,id=-1,r=-1,g=-1,b=-1,a=-1, matches=0, bestAlpha=-1;
    for(EGLint i=0;i<received;++i) {
        EGLint candidateVisual=-1,candidateAlpha=-1;
        if(!eglGetConfigAttrib(display,configs[i],EGL_NATIVE_VISUAL_ID,&candidateVisual) ||
           !eglGetConfigAttrib(display,configs[i],EGL_ALPHA_SIZE,&candidateAlpha)) { error=eglGetError(); fail("query config visual/alpha",error); return; }
        if(candidateVisual!=fmt) continue;
        ++matches;
        // Prefer an 8-bit alpha channel among matching native visuals.
        if(!config || (candidateAlpha>=8 && bestAlpha<8)) { config=configs[i]; bestAlpha=candidateAlpha; }
    }
    info << "received=" << received << " visualMatches=" << matches << " ";
    if(!config) { fail("no config matching native format",EGL_SUCCESS); return; }
    if(!eglGetConfigAttrib(display,config,EGL_NATIVE_VISUAL_ID,&visual) || !eglGetConfigAttrib(display,config,EGL_CONFIG_ID,&id) ||
       !eglGetConfigAttrib(display,config,EGL_RED_SIZE,&r) || !eglGetConfigAttrib(display,config,EGL_GREEN_SIZE,&g) ||
       !eglGetConfigAttrib(display,config,EGL_BLUE_SIZE,&b) || !eglGetConfigAttrib(display,config,EGL_ALPHA_SIZE,&a)) {
        error=eglGetError(); fail("query selected config",error); return;
    }
    info << "config=" << id << " visual=" << visual << " rgba=" << r << "/" << g << "/" << b << "/" << a << "\n";
    surface=eglCreateWindowSurface(display,config,reinterpret_cast<EGLNativeWindowType>(window),nullptr); error=eglGetError();
    if(surface==EGL_NO_SURFACE) { fail("eglCreateWindowSurface",error); return; }
    const EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx); error=eglGetError();
    if(context==EGL_NO_CONTEXT) { fail("eglCreateContext",error); return; }
    success=eglMakeCurrent(display,surface,surface,context); error=eglGetError();
    if(!success) { fail("eglMakeCurrent",error); return; }
    EGLint sw=0,sh=0;
    success=eglQuerySurface(display,surface,EGL_WIDTH,&sw); error=eglGetError();
    if(!success) { fail("query surface width",error); return; }
    success=eglQuerySurface(display,surface,EGL_HEIGHT,&sh); error=eglGetError();
    if(!success || sw<=0 || sh<=0) { info << "surface=" << sw << "x" << sh << " "; fail("query surface height/size",error); return; }
    using GetStringFn = const GLubyte* (*)(GLenum);
    using ViewportFn = void (*)(GLint,GLint,GLsizei,GLsizei);
    using ClearColorFn = void (*)(GLfloat,GLfloat,GLfloat,GLfloat);
    using ClearFn = void (*)(GLbitfield);
    using ReadPixelsFn = void (*)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
    using GetErrorFn = GLenum (*)();
    auto getString=reinterpret_cast<GetStringFn>(eglGetProcAddress("glGetString"));
    auto viewport=reinterpret_cast<ViewportFn>(eglGetProcAddress("glViewport"));
    auto clearColor=reinterpret_cast<ClearColorFn>(eglGetProcAddress("glClearColor"));
    auto clear=reinterpret_cast<ClearFn>(eglGetProcAddress("glClear"));
    auto readPixels=reinterpret_cast<ReadPixelsFn>(eglGetProcAddress("glReadPixels"));
    auto getError=reinterpret_cast<GetErrorFn>(eglGetProcAddress("glGetError"));
    const GLubyte* directVendor=glGetString(GL_VENDOR);
    const GLubyte* vendor=getString ? getString(GL_VENDOR) : nullptr;
    const GLubyte* rendererName=getString ? getString(GL_RENDERER) : nullptr;
    const GLubyte* version=getString ? getString(GL_VERSION) : nullptr;
    auto str=[](const GLubyte* s) { return s ? reinterpret_cast<const char*>(s) : "null"; };
    info << "surface=" << sw << "x" << sh << " current=" << (eglGetCurrentContext()==context)
         << " draw=" << (eglGetCurrentSurface(EGL_DRAW)==surface) << " directVendor=" << str(directVendor)
         << " procVendor=" << str(vendor) << " renderer=" << str(rendererName) << " version=" << str(version) << "\n";
    if((!getString || !viewport || !clearColor || !clear || !readPixels || !getError || !vendor || !version) && directVendor) {
        getString=glGetString; viewport=glViewport; clearColor=glClearColor; clear=glClear; readPixels=glReadPixels; getError=glGetError;
        vendor=directVendor; version=getString(GL_VERSION); info << "using direct GL calls\n";
    }
    if(!getString || !viewport || !clearColor || !clear || !readPixels || !getError || !vendor || !version) { fail("GL dispatch unavailable",eglGetError()); return; }
    uint64_t frames=0; unsigned char pixel[4]={0,0,0,0};
    while(!stopRender.load()) {
        viewport(0,0,sw,sh); clearColor(0.08f,0.35f,0.5f,1); clear(GL_COLOR_BUFFER_BIT);
        if(frames==0) readPixels(sw/2,sh/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        GLenum glError=getError();
        if(glError!=GL_NO_ERROR) { info << "frames=" << frames << " GL=0x" << std::hex << glError << " "; fail("GL clear/read",EGL_SUCCESS); return; }
        success=eglSwapBuffers(display,surface); error=eglGetError();
        if(!success) { info << "frames=" << frames << " "; fail("eglSwapBuffers",error); return; }
        ++frames;
        if(frames==1 || frames%30==0) {
            std::ostringstream state; state << info.str() << "frames=" << frames << " pixel=" << (int)pixel[0] << "/" << (int)pixel[1] << "/" << (int)pixel[2] << "/" << (int)pixel[3] << " GL=0 swap OK";
            { std::lock_guard<std::mutex> guard(lock); graphics=state.str(); } Notify();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}
static void Created(OH_NativeXComponent* component, void* window) {
    DetachHostWindow();
    StopRender();
    uint64_t width=0,height=0;
    int sizeResult=window ? OH_NativeXComponent_GetXComponentSize(component,window,&width,&height) : -1;
    bool attached=AttachHostWindow(window,width,height);
    if(unityWindowOwned.load()) {
        {std::lock_guard<std::mutex> guard(lock);graphics=attached?"Unity host surface live; diagnostic renderer disabled":"Unity host surface unavailable";}Notify();return;
    }
    int refResult=attached ? ReferenceHostWindow(window) : -1;
    if(refResult!=0) { DetachHostWindow(); { std::lock_guard<std::mutex> guard(lock); graphics="native window reference failed: "+std::to_string(refResult); } Notify(); return; }
    stopRender.store(false);
    try { renderThread=std::thread(Render,window,width,height,sizeResult); }
    catch (...) { UnreferenceHostWindow(window); DetachHostWindow(); { std::lock_guard<std::mutex> guard(lock); graphics="render thread creation failed"; } Notify(); }
}
static void Changed(OH_NativeXComponent* c, void* w) {
    uint64_t width=0,height=0;
    if(w&&OH_NativeXComponent_GetXComponentSize(c,w,&width,&height)==0&&
       width>0&&height>0&&width<=INT32_MAX&&height<=INT32_MAX){
        std::lock_guard<std::mutex> guard(nativeWindowLock);
        if(w==currentNativeWindow){
            nativeWindowBaseWidth=(int32_t)width;nativeWindowBaseHeight=(int32_t)height;
            OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbe","HOST_SURFACE_CHANGED same_window=1 generation=%{public}llu size=%{public}llux%{public}llu unity_owned=%{public}d",(unsigned long long)nativeWindowGeneration,(unsigned long long)width,(unsigned long long)height,unityWindowOwned.load()?1:0);
            return;
        }
    }
    Created(c,w);
}
#include "touch_record.inc"
static void Touch(OH_NativeXComponent* c, void* w) {
    // Game-owned XComponent forwards real input only. Per-MOVE diagnostic
    // strings/hilog/ArkTS state notifications compete with the same UI loop.
    const bool diagnosticTouch=!unityWindowOwned.load(std::memory_order_relaxed);
    timespec now{}; clock_gettime(CLOCK_MONOTONIC,&now); uint64_t seq=++touchSequence;
    OH_NativeXComponent_TouchEvent e{}; int rc=OH_NativeXComponent_GetTouchEvent(c,w,&e);
    if(diagnosticTouch)OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "NativeProbeTouch", "seq=%{public}llu event_ns=%{public}lld recv_mono_ns=%{public}lld id=%{public}d type=%{public}d count=%{public}u rc=%{public}d", (unsigned long long)seq, (long long)e.timeStamp, (long long)now.tv_sec*1000000000LL+now.tv_nsec, rc?-1:e.id, rc?-1:(int)e.type, rc?0:e.numPoints, rc);
    if(rc) { if(observeTouch)observeTouch(seq,-1,rc,(uint64_t)now.tv_sec*1000000000ull+now.tv_nsec,(uint64_t)now.tv_sec*1000000000ull+now.tv_nsec); { std::lock_guard<std::mutex> guard(lock); ++touchErrors; } if(diagnosticTouch)Notify(); return; }
    /* Full host snapshot and changed pointer identity. The optional runtime
     * maps OH DOWN/UP to Android POINTER_DOWN/UP for additional fingers.
     * Coordinates are local pixels; only an explicitly disabled switch uses
     * the legacy first-pointer path. */
    RecordTouch(e,seq,(uint64_t)now.tv_sec*1000000000ull+now.tv_nsec);
    int multiResult=-38;
    int deliveryResult=-38;
    if(queueMultiTouch) {
        mt_packet p{};p.size=sizeof p;p.version=1;p.sequence=seq;
        p.callback_ns=(uint64_t)now.tv_sec*1000000000ull+now.tv_nsec;
        p.event_ns=e.timeStamp;p.action=(int)e.type;p.changed_id=e.id;
        p.count=e.numPoints<=MT_MAX?e.numPoints:MT_MAX;
        bool found=false;
        for(unsigned i=0;i<p.count;i++) {
            const auto& t=e.touchPoints[i];auto& d=p.points[i];
            d.host_id=t.id;d.id=-1;d.pressed=t.isPressed;d.x=t.x;d.y=t.y;
            d.raw_x=t.screenX;d.raw_y=t.screenY;d.pressure=t.force;d.size=(float)t.size;
            if(t.id==e.id)found=true;
        }
        // Some host UP snapshots omit the released point; e.id/x/y retain it.
        if(!found&&p.count<MT_MAX&&(e.type==OH_NATIVEXCOMPONENT_DOWN||e.type==OH_NATIVEXCOMPONENT_UP)) {
            auto& d=p.points[p.count++];d.host_id=e.id;d.id=-1;d.x=e.x;d.y=e.y;
            d.raw_x=e.screenX;d.raw_y=e.screenY;d.pressure=e.force;d.size=(float)e.size;
            d.pressed=e.type==OH_NATIVEXCOMPONENT_DOWN;
        }
        multiResult=e.numPoints>MT_MAX?-22:queueMultiTouch(&p);
        deliveryResult=multiResult;
    }
    if(multiResult==-38&&queueTouch && e.numPoints>0) {
        int action=(int)e.type;
        if(action>=0 && action<=3) {
            int qrc=queueTouch(action,(int)e.touchPoints[0].id,
                       e.touchPoints[0].x,e.touchPoints[0].y,
                       (int64_t)e.timeStamp);
            deliveryResult=qrc;
            if(diagnosticTouch)OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCHQ host=1 action=%{public}d id=%{public}d x=%{public}d y=%{public}d rc=%{public}d",action,(int)e.touchPoints[0].id,(int)e.touchPoints[0].x,(int)e.touchPoints[0].y,qrc);
        }
    } else if(!queueTouch && e.numPoints>0) {
        if(diagnosticTouch)OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCHQ host=1 dropped=symbol_missing type=%{public}d points=%{public}u",(int)e.type,e.numPoints);
    }
    if(observeTouch){timespec end{};clock_gettime(CLOCK_MONOTONIC,&end);observeTouch(seq,(int)e.type,deliveryResult,(uint64_t)now.tv_sec*1000000000ull+now.tv_nsec,(uint64_t)end.tv_sec*1000000000ull+end.tv_nsec);}
    char b[1024];
    if(diagnosticTouch){int n=snprintf(b,sizeof(b),"type=%d points=%u time=%lld",e.type,e.numPoints,(long long)e.timeStamp);
    for(unsigned i=0;i<e.numPoints && i<OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER && n<(int)sizeof(b)-80;i++) n+=snprintf(b+n,sizeof(b)-n," | id=%d x=%.1f y=%.1f",e.touchPoints[i].id,e.touchPoints[i].x,e.touchPoints[i].y);}
    { std::lock_guard<std::mutex> guard(lock); ++touchEvents;
    if(e.type==OH_NATIVEXCOMPONENT_CANCEL) { ++cancelEvents; for(auto& entry:pointers) if(entry.second.active) { ++entry.second.cancel; entry.second.active=false; } }
    else if(e.type==OH_NATIVEXCOMPONENT_DOWN || e.type==OH_NATIVEXCOMPONENT_UP) {
        auto& p=pointers[e.id]; if(e.type==OH_NATIVEXCOMPONENT_DOWN) { ++p.down; if(p.active) ++p.anomalous; p.active=true; }
        else { ++p.up; if(!p.active) ++p.anomalous; p.active=false; }
    }
    if(diagnosticTouch){touchHistory.emplace_back(b); if (touchHistory.size() > 5) touchHistory.pop_front(); touches.clear(); for (const auto& event : touchHistory) touches += event + "\n";}
    } if(diagnosticTouch)Notify();
}
static OH_NativeXComponent_Callback callbacks={Created,Changed,Destroy,Touch};
static int32_t Write(OH_AudioRenderer*,void*,void* data,int32_t bytes) {
    auto* p=static_cast<int16_t*>(data); int n=bytes/2; int frames=n/2;
    // A renderer invokes its write callback serially. Counter saturates after the finite pulse.
    int first=samples.load(std::memory_order_relaxed);
    int last=first + (frames < 12000-first ? frames : 12000-first);
    samples.store(last,std::memory_order_relaxed);
    for(int i=0;i<n;i+=2) { int frame=first+i/2; int16_t v=frame<12000 ? static_cast<int16_t>(2500*sin(2*3.141592653589793*440*frame/48000.0)) : 0; p[i]=v; if(i+1<n) p[i+1]=v; } return 0;
}
static napi_value Str(napi_env env,const std::string& s) { napi_value out; napi_create_string_utf8(env,s.c_str(),NAPI_AUTO_LENGTH,&out); return out; }
/* 诊断页直接注入一次 TAP（DOWN+UP）到 guest 触摸队列，绕过 XComponent
 * 触摸回调，用于验证 sdk_input_queue_touch → nativeInjectEvent 链路。 */
static napi_value InjectTap(napi_env env,napi_callback_info info) {
    size_t argc=2; napi_value argv[2]={nullptr,nullptr}; double x=1280.0,y=1370.0;
    napi_get_cb_info(env,info,&argc,argv,nullptr,nullptr);
    if(argc>0) napi_get_value_double(env,argv[0],&x);
    if(argc>1) napi_get_value_double(env,argv[1],&y);
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts);
    int64_t ns=(int64_t)ts.tv_sec*1000000000LL+ts.tv_nsec;
    int down=queueTouch?queueTouch(0,0,(float)x,(float)y,ns):-1;
    int up=queueTouch?queueTouch(1,0,(float)x,(float)y,ns):-1;
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCHQ host_inject=1 x=%{public}d y=%{public}d down=%{public}d up=%{public}d sym=%{public}d",(int)x,(int)y,down,up,queueTouch?1:0);
    char b[160]; snprintf(b,sizeof(b),"injectTap x=%.0f y=%.0f down=%d up=%d sym=%d",x,y,down,up,queueTouch?1:0);
    { std::lock_guard<std::mutex> guard(lock); touchHistory.emplace_back(b); if(touchHistory.size()>5) touchHistory.pop_front(); touches.clear(); for(const auto& event:touchHistory) touches+=event+"\n"; }
    Notify();
    return Str(env,b);
}
static napi_value Test(napi_env env,napi_callback_info) {
    dlerror(); void* h=dlopen("libprobe_payload.so",RTLD_NOW); const char* err=dlerror();
    if(!h) return Str(env,std::string("dlopen failed: ")+(err?err:"unknown"));
    dlerror(); auto f=reinterpret_cast<int(*)()>(dlsym(h,"probe_payload_value")); err=dlerror();
    std::string result=err ? std::string("dlsym failed: ")+err : (f && f()==42 ? "dlopen + symbol OK" : "payload returned unexpected value"); dlclose(h); return Str(env,result);
}
static napi_value AndroidElf(napi_env env,napi_callback_info) {
    dlerror(); void* h=dlopen("libprobe_android_guest.so",RTLD_NOW|RTLD_LOCAL); const char* err=dlerror();
    if(!h) return Str(env,std::string("Android-target ELF dlopen failed: ")+(err?err:"unknown"));
    using Ctor=int(*)(); using Integer=unsigned long long(*)(unsigned long long,unsigned long long); using Float=double(*)(double,double);
    dlerror(); auto ctor=reinterpret_cast<Ctor>(dlsym(h,"probe_android_ctor_state")); const char* e1=dlerror(); std::string error=e1?e1:"";
    dlerror(); auto integer=reinterpret_cast<Integer>(dlsym(h,"probe_android_integer")); const char* e2=dlerror(); if(e2) error+=std::string(" ")+e2;
    dlerror(); auto floating=reinterpret_cast<Float>(dlsym(h,"probe_android_float")); const char* e3=dlerror(); if(e3) error+=std::string(" ")+e3;
    if(!error.empty() || !ctor || !integer || !floating) { dlclose(h); return Str(env,"Android ELF dlsym failed: "+error); }
    int c=ctor(); unsigned long long i=integer(0x1122334455667788ULL,0x0102030405060708ULL); double f=floating(2.5,0.25);
    bool pass=c==0x1357 && i==0x1020304050607091ULL && f==5.25; dlclose(h);
    std::ostringstream out; out << "Android-target ELF " << (pass?"PASS":"FAIL") << " ctor=0x" << std::hex << c << " int=0x" << i << std::dec << " float=" << f << "; no libc/Bionic test"; return Str(env,out.str());
}
static napi_value AndroidLibc(napi_env env,napi_callback_info) {
    dlerror();
    void* handle=dlopen("libprobe_android_libc.so",RTLD_NOW|RTLD_LOCAL);
    const char* error=dlerror();
    if(!handle) return Str(env,std::string("C runtime guest dlopen failed: ")+(error?error:"unknown"));
    using Run=int(*)(unsigned long long,unsigned long long*);
    dlerror();
    auto run=reinterpret_cast<Run>(dlsym(handle,"probe_libc_run"));
    error=dlerror();
    if(error || !run) {
        std::string message=std::string("C runtime guest dlsym failed: ")+(error?error:"null function");
        dlclose(handle);
        return Str(env,message);
    }
    unsigned long long observed=0;
    int result=run(0x1122334455667788ULL,&observed);
    bool pass=result==0 && observed==0xb478961e475221f0ULL;
    std::ostringstream message;
    message << "C runtime guest " << (pass?"PASS":"FAIL") << " step=" << result
            << " result=0x" << std::hex << observed
            << "; malloc/free/memcpy/strlen/snprintf only; not full Bionic";
    dlclose(handle);
    return Str(env,message.str());
}
#include "apk_payload_extract.inc"
struct SdkTraceJob {
    napi_async_work work=nullptr;
    napi_deferred deferred=nullptr;
    std::string directory;
    std::string output;
    bool unity=false;
    bool originalMain=false;
    bool audio=false;
    bool jni=false;
    bool login=false;
    bool appOwner=false;
};
static std::atomic<bool> sdkTraceBusy{false};
static std::atomic<bool> appVmBusy{false},appWindowReady{false},appForeground{false},appDestroyRequested{false};
static std::atomic<int> loadingStage{0}; // 1: dlopen entered, 2: runtime loaded / engine entered.
static napi_value LoadingStatus(napi_env env,napi_callback_info) {
    unsigned long long submissions=0;
    void* runtime=loginRuntime.load();
    if(runtime){auto count=reinterpret_cast<unsigned long long(*)()>(dlsym(runtime,"egl_window_runtime_successful_submissions"));if(count)submissions=count();}
    return Str(env,"stage="+std::to_string(loadingStage.load())+" submissions="+std::to_string(submissions)+" mono_ms="+std::to_string(PayloadMs()));
}
// Read-only attachment: RTLD_NOLOAD must not instantiate a diagnostic stream
// or substitute its callbacks for evidence of the game's BGM output.
static napi_value AudioLatencyStatus(napi_env env,napi_callback_info) {
    std::string result="pid="+std::to_string(getpid())+" mono_ms="+std::to_string(PayloadMs());
    result+=" oh_burst_symbol="+std::to_string(dlsym(RTLD_DEFAULT,"OH_AudioRenderer_GetFrameSizeInCallback")!=nullptr);
    result+=" oh_buffer_get_symbol="+std::to_string(dlsym(RTLD_DEFAULT,"OH_AudioRenderer_GetBufferSizeInFrames")!=nullptr);
    result+=" oh_buffer_set_symbol="+std::to_string(dlsym(RTLD_DEFAULT,"OH_AudioRenderer_SetBufferSizeInFrames")!=nullptr);
    void*h=dlopen("libgame_aaudio_bridge.so",RTLD_NOW|RTLD_NOLOAD);
    if(!h)result+=" bridge_loaded=0 reason=no_game_bridge_handle";
    else {result+=" bridge_loaded=1 ";auto summary=reinterpret_cast<const char*(*)()>(dlsym(h,"AAudioBridge_diagnosticsSummary"));result+=summary?summary():"reason=summary_symbol_absent";dlclose(h);}
    return Str(env,result);
}

static void AppVmSignal(const char* symbol,int value=0) {
    void* runtime=loginRuntime.load();
    if(!runtime)return;
    if(std::string(symbol)=="sdk_app_vm_set_foreground") {
        auto fn=reinterpret_cast<void(*)(int)>(dlsym(runtime,symbol));if(fn)fn(value);
    }else {auto fn=reinterpret_cast<void(*)()>(dlsym(runtime,symbol));if(fn)fn();}
}
static napi_value AppVmWindowReady(napi_env env,napi_callback_info) {
    appWindowReady.store(true);AppVmSignal("sdk_app_vm_window_ready");return Str(env,"window-ready");
}
static napi_value AppVmHeartbeat(napi_env env,napi_callback_info) {
    AppVmSignal("sdk_task8a_main_heartbeat");
    using BootStatus=int(*)(char*,size_t);
    static BootStatus bootStatus=nullptr;
    void *runtime=loginRuntime.load();
    if(runtime&&!bootStatus)bootStatus=reinterpret_cast<BootStatus>(dlsym(runtime,"dvm_gc_foundation_boot_status"));
    char status[256]={0};if(bootStatus)bootStatus(status,sizeof status);
    return Str(env,status);
}
static napi_value AppVmForeground(napi_env env,napi_callback_info info) {
    size_t argc=1;napi_value args[1];bool value=false;
    napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);
    if(argc==1)napi_get_value_bool(env,args[0],&value);
    appForeground.store(value);AppVmSignal("sdk_app_vm_set_foreground",value?1:0);
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","APP_VM foreground=%{public}d",value?1:0);
    return Str(env,value?"foreground":"background");
}
static napi_value AppVmDestroy(napi_env env,napi_callback_info) {
    appDestroyRequested.store(true);AppVmSignal("sdk_app_vm_destroy_request");
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","APP_VM onDestroy requested");
    return Str(env,"destroy-requested");
}

struct HostProbeJob {
    napi_async_work work=nullptr;
    napi_deferred deferred=nullptr;
    std::string mode;
    std::string output;
};
static std::string ProbeSettings() {
    std::ostringstream out;
    out<<"[settings] (worker thread -> ArkTS settings.getValueSync/setValueSync)\n";
    char value[CONTENT_SETTINGS_HOST_VALUE_MAX];
    std::string airplaneValue;
    int airplane=SettingsGet(nullptr,CONTENT_SETTINGS_GLOBAL,"airplane_mode_on",value,sizeof(value));
    out<<"Global.airplane_mode_on status="<<airplane;
    if(airplane==1){airplaneValue.assign(value);out<<" value="<<airplaneValue;}
    out<<"\n";
    int brightness=SettingsGet(nullptr,CONTENT_SETTINGS_SYSTEM,"screen_brightness",value,sizeof(value));
    out<<"System.screen_brightness status="<<brightness;
    if(brightness==1)out<<" value="<<value;
    out<<"\n";
    int missing=SettingsGet(nullptr,CONTENT_SETTINGS_GLOBAL,"__native_probe_missing_key__",value,sizeof(value));
    out<<"Global.__native_probe_missing_key__ status="<<missing<<" (0=absent)\n";
    int secure=SettingsGet(nullptr,CONTENT_SETTINGS_SECURE,"android_id",value,sizeof(value));
    out<<"Secure.android_id status="<<secure<<" (public @ohos.settings exposes no USER_SECURE domain constant; host reports absent)\n";
    if(airplane==1) {
        int writeBack=SettingsPut(nullptr,CONTENT_SETTINGS_GLOBAL,"airplane_mode_on",airplaneValue.c_str());
        out<<"write_back_same_value status="<<writeBack<<" (same value; no intended state change; 0=ok, -2=permission, other=backend)\n";
    } else {
        out<<"write_back=skipped; no readable value and the host refuses blind system writes\n";
    }
    return out.str();
}
static std::string ProbeNetwork() {
    std::ostringstream out;
    out<<"[network] (worker thread -> ArkTS @ohos.net.connection/@ohos.telephony)\n";
    network_host_snapshot s{};
    int status=NetworkSnapshot(nullptr,&s);
    out<<"snapshot status="<<status<<"\n";
    if(status==0) {
        out<<"connectivity_valid="<<s.connectivity_valid<<" has_default="<<s.has_default_network
           <<" connected="<<s.connected<<" available="<<s.available<<" metered="<<s.metered
           <<" type="<<s.type<<" type_valid="<<s.type_valid
           <<" transports=0x"<<std::hex<<s.transports<<std::dec
           <<" capabilities=0x"<<std::hex<<s.capabilities<<std::dec
           <<" handle="<<s.network_handle<<"\n";
        out<<"telephony_valid="<<s.telephony_valid<<" network_type="<<s.network_type
           <<" network_type_valid="<<s.network_type_valid<<" sim_state="<<s.sim_state
           <<" sim_state_valid="<<s.sim_state_valid<<" phone_type="<<s.phone_type
           <<" phone_type_valid="<<s.phone_type_valid<<"\n";
        out<<"operator_numeric_valid="<<s.operator_numeric_valid<<" operator_numeric="<<s.operator_numeric
           <<" operator_name_valid="<<s.operator_name_valid<<" operator_name="<<s.operator_name<<"\n";
    }
    return out.str();
}
static std::string ProbeDisplay() {
    std::ostringstream out;
    out<<"[display] (cached ArkTS measurement, installed by the worker before sdk_trace_run)\n";
    PlatformDisplaySnapshot s{};std::string source;
    bool present=ApplyCachedDisplay(&s,source);
    out<<"cached_snapshot="<<(present?"present (display_host candidate; applied before sdk_trace_run)":"none (display_host=0 fallback)")<<"\n";
    if(present) {
        out<<"width_pixels="<<s.width_pixels<<" height_pixels="<<s.height_pixels
           <<" density_dpi="<<s.density_dpi<<" orientation="<<s.orientation
           <<" screen_dp="<<s.screen_width_dp<<"x"<<s.screen_height_dp
           <<" smallest_dp="<<s.smallest_screen_width_dp
           <<" density="<<s.density<<" scaled="<<s.scaled_density
           <<" xdpi="<<s.xdpi<<" ydpi="<<s.ydpi<<" source="<<source<<"\n";
    }
    void* runtime=dlopen("libsdktrace.so",RTLD_NOW|RTLD_LOCAL);
    if(runtime) {
        using SourceFn=const char*(*)();
        auto sourceFn=reinterpret_cast<SourceFn>(dlsym(runtime,"sdk_display_source"));
        auto setFn=reinterpret_cast<SetDisplaySnapshotFn>(dlsym(runtime,"sdk_set_display_snapshot"));
        if(sourceFn)out<<"runtime_source_now="<<sourceFn()<<"\n";
        if(!setFn)out<<"runtime_set_display_snapshot=missing\n";
        dlclose(runtime);
    } else {
        const char* error=dlerror();
        out<<"runtime_load_failed="<<(error?error:"unknown")<<"\n";
    }
    return out.str();
}
static std::string ProbeIntent() {
    std::ostringstream out;
    out<<"[intent]\nintent_launch=not-installed\n";
    out<<"reason=the launch host receives only an opaque dvm_ref; intent_runtime exposes no getComponent/getPackage/getClassName accessor, so an equivalent HarmonyOS Want cannot be constructed. No fake success is reported.\n";
    return out.str();
}
static void ExecuteHostProbe(napi_env,void* data) {
    auto* job=static_cast<HostProbeJob*>(data);
    try {
        std::ostringstream out;
        out<<"HOST_PROBE mode="<<job->mode<<" thread=VM-worker\n";
        if(job->mode=="settings"||job->mode=="all")out<<ProbeSettings();
        if(job->mode=="network"||job->mode=="all")out<<ProbeNetwork();
        if(job->mode=="display"||job->mode=="all")out<<ProbeDisplay();
        if(job->mode=="intent"||job->mode=="all")out<<ProbeIntent();
        if(job->mode!="settings"&&job->mode!="network"&&job->mode!="display"&&job->mode!="intent"&&job->mode!="all") {
            out<<"unknown mode; use settings|network|display|intent|all\n";
        }
        job->output=out.str();
    } catch(const std::exception& error) {
        job->output=std::string("HOST_PROBE exception: ")+error.what();
    } catch(...) {
        job->output="HOST_PROBE exception";
    }
}
static void CompleteHostProbe(napi_env env,napi_status status,void* data) {
    auto* job=static_cast<HostProbeJob*>(data);
    napi_value result=Str(env,status==napi_ok?job->output:"Host probe cancelled");
    if(status==napi_ok)napi_resolve_deferred(env,job->deferred,result);
    else napi_reject_deferred(env,job->deferred,result);
    napi_delete_async_work(env,job->work);
    sdkTraceBusy.store(false);
    delete job;
}
static napi_value HostProbe(napi_env env,napi_callback_info info) {
    size_t argc=1;napi_value args[1];std::string mode;
    if(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr)!=napi_ok||argc!=1||!ReadString(env,args[0],mode)) {
        napi_throw_type_error(env,nullptr,"Expected host probe mode");return nullptr;
    }
    if(sdkTraceBusy.exchange(true)) {napi_throw_error(env,nullptr,"A diagnostic is already running");return nullptr;}
    auto* job=new HostProbeJob();job->mode=mode;
    napi_value promise,name;
    napi_status status=napi_create_promise(env,&job->deferred,&promise);
    if(status==napi_ok)status=napi_create_string_utf8(env,"Host bridge probe",NAPI_AUTO_LENGTH,&name);
    if(status==napi_ok)status=napi_create_async_work(env,nullptr,name,ExecuteHostProbe,CompleteHostProbe,job,&job->work);
    if(status==napi_ok)status=napi_queue_async_work(env,job->work);
    if(status!=napi_ok) {
        if(job->work)napi_delete_async_work(env,job->work);
        delete job;sdkTraceBusy.store(false);
        napi_throw_error(env,nullptr,"Cannot queue host probe");return nullptr;
    }
    return promise;
}
static std::atomic<int> bridgeCallbacks{0};
static int32_t SilentBridgeCallback(void*,void*,void*,int32_t) {bridgeCallbacks.fetch_add(1);return 1;}
static std::string CheckAudioBridge() {
    std::string report="BUILD AUDIO_BRIDGE_V1\n";
    static void* handle=dlopen("libgame_aaudio_bridge.so",RTLD_NOW|RTLD_LOCAL);
    if(!handle) { const char* e=dlerror();return report+"RESULT load failed: "+(e?e:"unknown"); }
    using Create=int32_t(*)(void**); using Set=void(*)(void*,int32_t);
    using Callback=void(*)(void*,int32_t(*)(void*,void*,void*,int32_t),void*);
    using Open=int32_t(*)(void*,void**); using Get=int32_t(*)(void*);
    auto create=reinterpret_cast<Create>(dlsym(handle,"AAudio_createStreamBuilder"));
    auto destroy=reinterpret_cast<Get>(dlsym(handle,"AAudioStreamBuilder_delete"));
    auto setMode=reinterpret_cast<Set>(dlsym(handle,"AAudioStreamBuilder_setPerformanceMode"));
    auto setUsage=reinterpret_cast<Set>(dlsym(handle,"AAudioStreamBuilder_setUsage"));
    auto setCallback=reinterpret_cast<Callback>(dlsym(handle,"AAudioStreamBuilder_setDataCallback"));
    auto open=reinterpret_cast<Open>(dlsym(handle,"AAudioStreamBuilder_openStream"));
    auto close=reinterpret_cast<Get>(dlsym(handle,"AAudioStream_close"));
    auto rate=reinterpret_cast<Get>(dlsym(handle,"AAudioStream_getSampleRate"));
    auto channels=reinterpret_cast<Get>(dlsym(handle,"AAudioStream_getChannelCount"));
    auto mode=reinterpret_cast<Get>(dlsym(handle,"AAudioStream_getPerformanceMode"));
    auto burst=reinterpret_cast<Get>(dlsym(handle,"AAudioStream_getFramesPerBurst"));
    auto buffer=reinterpret_cast<Get>(dlsym(handle,"AAudioStream_getBufferSizeInFrames"));
    auto setBuffer=reinterpret_cast<int32_t(*)(void*,int32_t)>(dlsym(handle,"AAudioStream_setBufferSizeInFrames"));
    if(!create||!destroy||!setMode||!setUsage||!setCallback||!open||!close||!rate||!channels||!mode) return report+"RESULT required export missing";
    void* builder=nullptr; int result=create(&builder);
    if(result||!builder) return report+"RESULT builder failed code="+std::to_string(result);
    bridgeCallbacks.store(0); setMode(builder,12);setUsage(builder,14);setCallback(builder,SilentBridgeCallback,nullptr);
    void* stream=nullptr; result=open(builder,&stream);destroy(builder);
    report+="OPEN code="+std::to_string(result)+"\n";
    if(result||!stream) return report+"RESULT game FAST renderer creation failed; no start requested";
    int actualRate=rate(stream),actualChannels=channels(stream),actualMode=mode(stream);
    int actualBurst=burst?burst(stream):-890,actualBuffer=buffer?buffer(stream):-890;
    int setResult=(setBuffer&&actualBurst>0&&actualBurst<=INT32_MAX/2)?setBuffer(stream,2*actualBurst):-890;
    int closed=close(stream);
    report+="FORMAT rate="+std::to_string(actualRate)+" channels="+std::to_string(actualChannels)+" performance="+std::to_string(actualMode)+"\n";
    report+="PREPARED_PROBE_ONLY frames_per_burst="+std::to_string(actualBurst)+" buffer_frames="+std::to_string(actualBuffer)+" set_2_bursts="+std::to_string(setResult)+" closed_burst="+std::to_string(burst?burst(stream):-890)+"\n";
    report+="RESULT close="+std::to_string(closed)+" callbacks="+std::to_string(bridgeCallbacks.load())+"; renderer never started; game audio and latency unverified";
    return report;
}
static void ExecuteSdkTrace(napi_env,void* data) {
    auto* job=static_cast<SdkTraceJob*>(data);
    try {
        if(!job->appOwner&&!job->unity&&!job->audio&&!job->originalMain) {
            void* runtime=loginRuntime.load();
            auto snapshot=runtime?reinterpret_cast<const char*(*)(const char*)>(dlsym(runtime,job->login?"sdk_login_trace_run":"sdk_trace_run")):nullptr;
            job->output=snapshot?snapshot(job->directory.c_str()):"SDK_LOGIN_TRACE\nAPP_VM_DIAGNOSTIC attached=0 creates_vm=0 unbinds_vm=0\nRESULT login diagnostic stopped; app owner not ready\n";
            OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","APP_VM diagnostic snapshot creates_vm=0 unbinds_vm=0");
            return;
        }
        if(job->jni) {
            static void* bridge=dlopen("libjnibatch.so",RTLD_NOW|RTLD_LOCAL);
            if(!bridge) {const char* e=dlerror();job->output=std::string("BUILD JNI_BATCH_V1\nRESULT load failed: ")+(e?e:"unknown");return;}
            using Run=const char*(*)(const char*);
        auto run=reinterpret_cast<Run>(dlsym(bridge,"jni_batch_run"));
            if(!run) {job->output="BUILD JNI_BATCH_V1\nRESULT entry missing";return;}
            const char* report=run(job->directory.c_str());
            job->output=report?report:"BUILD JNI_BATCH_V1\nRESULT no report";return;
        }
        if(job->audio) {job->output=CheckAudioBridge();return;}
        if(job->originalMain) {
            static void* loader=dlopen("libunity_spike_loader.so",RTLD_NOW|RTLD_LOCAL);
            if(!loader) { const char* e=dlerror(); job->output=std::string("BUILD ORIGINAL_MAIN_V1\nRESULT loader unavailable: ")+(e?e:"unknown"); return; }
            using Open=int(*)(char*,unsigned long);
            auto open=reinterpret_cast<Open>(dlsym(loader,"spike_open_original_main"));
            if(!open) { job->output="BUILD ORIGINAL_MAIN_V1\nRESULT loader entry missing"; return; }
            char output[2048]={0}; int result=open(output,sizeof(output));
            job->output=std::string("BUILD ORIGINAL_MAIN_V1\nRESULT code=")+std::to_string(result)+" "+output;
            return;
        }
        if(job->unity) {
            static void* batch=nullptr;
            if(!batch) batch=dlopen("libunitybatch.so",RTLD_NOW|RTLD_LOCAL);
            if(!batch) { const char* e=dlerror(); job->output=std::string("Unity batch unavailable: ")+(e?e:"unknown"); return; }
            using Batch=const char*(*)();
            auto run=reinterpret_cast<Batch>(dlsym(batch,"unity_batch_run"));
            if(!run) { job->output="Unity batch entry missing"; return; }
            const char* report=run();
            job->output=report?report:"Unity batch returned no report";
            return;
        }
        // Keep this diagnostic runtime resident for its static report storage.
        static void* runtime=nullptr;
        if(!runtime) {
            if(job->appOwner)loadingStage.store(1);
            dlerror(); runtime=dlopen("libsdktrace.so",RTLD_NOW|RTLD_LOCAL);
            const char* error=dlerror();
            if(!runtime) { job->output=std::string("SDK trace runtime unavailable: ")+(error?error:"unknown"); return; }
        }
        auto configurePages=reinterpret_cast<void(*)(const char*)>(dlsym(runtime,"page_geometry_configure"));
        if(!configurePages){job->output="E-PAGE-GEOMETRY-ABI missing";return;}
        configurePages(job->directory.c_str());
        auto setApkAssets=reinterpret_cast<void(*)(int(*)(const char*))>(dlsym(runtime,"sdk_apk_set_asset_open"));
        if(!setApkAssets){job->output="APK runtime asset ABI unavailable";return;}
        setApkAssets(PayloadOpenAsset);
        dlerror();
        using Run=const char*(*)(const char*);
        auto setLoginEvent=reinterpret_cast<void(*)(void(*)(const char*))>(dlsym(runtime,"login_panel_set_event_host"));
        if(setLoginEvent){setLoginEvent(LoginPanelEvent);loginRuntime.store(runtime);}
        using SetWebHost=void(*)(int(*)(int,uint32_t,const char*,const char*,char*,size_t));
        auto setWebHost=reinterpret_cast<SetWebHost>(dlsym(runtime,"android_web_set_host"));
        if(!setWebHost){job->output="SDK_LOGIN_TRACE\nRESULT Cookie/UA host ABI unavailable";return;}
        setWebHost(RequestWebPlatform);
        if(job->appOwner) {
            AppVmSignal("sdk_app_vm_set_foreground",appForeground.load()?1:0);
            if(appWindowReady.load())AppVmSignal("sdk_app_vm_window_ready");
            if(appDestroyRequested.load())AppVmSignal("sdk_app_vm_destroy_request");
        }
        using SetWindowHost=int(*)(const window_runtime_host*);
        auto setWindowHost=reinterpret_cast<SetWindowHost>(dlsym(runtime,"window_runtime_set_host"));
        static const window_runtime_host windowHost={WINDOW_RUNTIME_HOST_ABI_VERSION,sizeof(window_runtime_host),nullptr,
            AcquireCurrentHostWindow,QueryHostWindow,ReleaseHostWindow,ConfigureHostWindow};
        if(setWindowHost&&setWindowHost(&windowHost)!=0) {
            job->output="SDK_LOGIN_TRACE\nRESULT real native window host installation failed";return;
        }
        if(job->login) {
            using SetHost=void(*)(int(*)(void*,int),void*);
            auto setHost=reinterpret_cast<SetHost>(dlsym(runtime,"sdk_set_activity_host"));
            if(!setHost){job->output="SDK_LOGIN_TRACE\nRESULT Activity host entry missing";return;}
            setHost(MountGuestLayout,nullptr);
            using SetUnityMount=void(*)(int(*)(void*,void**,int*),void*);
            auto setUnityMount=reinterpret_cast<SetUnityMount>(dlsym(runtime,"sdk_set_unity_view_mount_host"));
            if(setUnityMount)setUnityMount(MountUnitySurface,runtime);
            using SetViewFocus=void(*)(int(*)(void*,uint32_t),void*);
            auto setViewFocus=reinterpret_cast<SetViewFocus>(dlsym(runtime,"sdk_set_view_focus_host"));
            if(setViewFocus)setViewFocus(RequestUnityViewFocus,nullptr);
            using SetActivityState=void(*)(int(*)(void*,int*,int*),void*);
            auto setActivityState=reinterpret_cast<SetActivityState>(dlsym(runtime,"sdk_set_unity_activity_state_host"));
            if(setActivityState)setActivityState(QueryUnityActivityState,nullptr);
            using SetWindowFeatureHost=void(*)(int(*)(void*,int),void*);
            auto setWindowFeatureHost=reinterpret_cast<SetWindowFeatureHost>(dlsym(runtime,"sdk_set_activity_window_feature_host"));
            if(setWindowFeatureHost)setWindowFeatureHost(RequestHostWindowFeature,nullptr);
            using SetDrawnHost=void(*)(int(*)(void*),void*);
            auto setDrawnHost=reinterpret_cast<SetDrawnHost>(dlsym(runtime,"sdk_set_activity_drawn_host"));
            if(setDrawnHost)setDrawnHost(ReportHostDrawn,nullptr);
            auto setSystemUiHost=reinterpret_cast<SetWindowFeatureHost>(dlsym(runtime,"sdk_set_view_system_ui_host"));
            if(setSystemUiHost)setSystemUiHost(RequestHostSystemUi,nullptr);
            using SetWindowFlags=void(*)(int(*)(void*,int,int),void*);
            auto setWindowFlags=reinterpret_cast<SetWindowFlags>(dlsym(runtime,"sdk_set_window_flags_host"));
            if(setWindowFlags)setWindowFlags(RequestHostWindowFlags,nullptr);
            using SetRotationHost=void(*)(int(*)(void*,int*),void*);
            auto setRotationHost=reinterpret_cast<SetRotationHost>(dlsym(runtime,"sdk_set_display_rotation_host"));
            if(setRotationHost)setRotationHost(RequestHostRotation,nullptr);
            auto queueTouchSymbol=reinterpret_cast<QueueTouchFn>(dlsym(runtime,"sdk_input_queue_touch"));
            if(queueTouchSymbol)queueTouch=queueTouchSymbol;
            queueMultiTouch=reinterpret_cast<QueueMultiTouchFn>(dlsym(runtime,"sdk_input_queue_multitouch"));
            observeTouch=reinterpret_cast<ObserveTouchFn>(dlsym(runtime,"sdk_input_observe_host"));
            OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCH_SYM ok=%{public}d",queueTouchSymbol?1:0);
            using SetAudioVolumeHost=void(*)(int(*)(void*,int,int*),void*);
            auto setAudioVolumeHost=reinterpret_cast<SetAudioVolumeHost>(dlsym(runtime,"sdk_set_audio_volume_host"));
            if(setAudioVolumeHost)setAudioVolumeHost(QueryHostAudioVolume,nullptr);
        }
        using SetBattery=void(*)(const battery_host_callbacks*,void*);
        auto setBattery=reinterpret_cast<SetBattery>(dlsym(runtime,"battery_runtime_set_host"));
        static const battery_host_callbacks batteryCallbacks={BatterySubscribe,BatteryUnsubscribe,BatterySnapshot};
        if(setBattery) setBattery(&batteryCallbacks,nullptr);
        auto setSettings=reinterpret_cast<SetSettingsHostFn>(dlsym(runtime,"sdk_set_settings_host"));
        auto setNetwork=reinterpret_cast<SetNetworkHostFn>(dlsym(runtime,"sdk_set_network_host"));
        auto setDisplay=reinterpret_cast<SetDisplaySnapshotFn>(dlsym(runtime,"sdk_set_display_snapshot"));
        using SetTimeZoneHostFn=void(*)(bool(*)(char*,size_t,int*));
        auto setTimeZone=reinterpret_cast<SetTimeZoneHostFn>(dlsym(runtime,"sdk_set_timezone_host"));
        // The getter/snapshot belong to the process, not this diagnostic job.
        // A temporary HostInstallGuard must not unbind a still-live App VM.
        if(setTimeZone)setTimeZone(QueryTimeZoneSnapshot);
        char zoneEvidence[256];int zoneOffsetEvidence=0;
        bool zonePresent=QueryTimeZoneSnapshot(zoneEvidence,sizeof(zoneEvidence),&zoneOffsetEvidence);
        OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","HOST_TIMEZONE host_bound=%{public}d snapshot=%{public}d",setTimeZone?1:0,zonePresent?1:0);
        using SetIntentLaunchFn=void(*)(intent_launch_host_v2_fn,void*);
        auto setIntentLaunch=reinterpret_cast<SetIntentLaunchFn>(dlsym(runtime,"intent_runtime_set_launch_host_v2"));
        LaunchInspectionHost launchInspector{reinterpret_cast<LaunchTraceFn>(dlsym(runtime,"sdk_trace_fprintf"))};
        hostSettingsSymbol.store(setSettings?1:0);
        hostNetworkSymbol.store(setNetwork?1:0);
        hostDisplaySymbol.store(setDisplay?1:0);
        hostIntentSymbol.store(setIntentLaunch?1:0);
        static const content_settings_host settingsHost={SettingsGet,SettingsPut};
        static const network_host_bridge networkBridge={nullptr,NetworkSnapshot};
        struct HostInstallGuard {
            SetSettingsHostFn settings;SetNetworkHostFn network;SetDisplaySnapshotFn display;SetIntentLaunchFn launch;
            ~HostInstallGuard(){
                if(settings)settings(nullptr,nullptr);
                if(network)network(nullptr);
                if(display)display(nullptr);
                if(launch)launch(nullptr,nullptr);
                hostSettingsInstalled.store(false);
                hostNetworkInstalled.store(false);
                std::lock_guard<std::mutex> guard(hostInfoLock);
                if(hostDisplayLastApply.rfind("applied",0)==0||hostDisplayLastApply.rfind("rejected",0)==0)
                    hostDisplayLastApply+="; restored fallback after run";
            }
        } hostGuard{setSettings,setNetwork,setDisplay,setIntentLaunch};
        if(setIntentLaunch&&launchInspector.trace)setIntentLaunch(InspectGuestLaunch,&launchInspector);
        if(setSettings){setSettings(&settingsHost,nullptr);hostSettingsInstalled.store(true);}
        if(setNetwork){setNetwork(&networkBridge);hostNetworkInstalled.store(true);}
        if(setDisplay) {
            PlatformDisplaySnapshot snapshot{};std::string source;
            bool have=ApplyCachedDisplay(&snapshot,source);
            bool applied=setDisplay(have?&snapshot:nullptr);
            std::lock_guard<std::mutex> guard(hostInfoLock);
            hostDisplayLastApply=have?(applied?"applied":"rejected-by-runtime"):"no-cached-snapshot; fallback";
        } else {
            std::lock_guard<std::mutex> guard(hostInfoLock);
            hostDisplayLastApply="symbol-missing; fallback";
        }
        {
            std::lock_guard<std::mutex> guard(hostInfoLock);
            if(!hostHandlerReady.load())hostBridgeNote="ArkTS host handler was not registered before the run; settings/network host RPC fails closed";
            else hostBridgeNote.clear();
        }
        // Optional host setters may be absent in an earlier SDK candidate.
        // Their dlerror must not be attributed to this required entry point.
        dlerror();
        auto run=reinterpret_cast<Run>(dlsym(runtime,job->appOwner?"sdk_app_vm_run":job->login?"sdk_login_trace_run":"sdk_trace_run"));
        const char* error=dlerror();
        if(error || !run) { job->output=std::string("SDK trace entry unavailable: ")+(error?error:"null function"); return; }
        if(job->appOwner)loadingStage.store(2);
        const char* report=run(job->directory.c_str());
        job->output=report?report:"SDK trace returned no report; initialization NOT verified";
    } catch(const std::exception& error) {
        job->output=std::string("SDK trace exception: ")+error.what();
    } catch(...) {
        job->output="SDK trace exception; initialization NOT verified";
    }
}
static void CompleteSdkTrace(napi_env env,napi_status status,void* data) {
    auto* job=static_cast<SdkTraceJob*>(data);
    if (job->appOwner && job->output.find("BLOCKED") != std::string::npos) {
        job->output = "E-VM-BOOT-FAILED step=unity_bootstrap\n" + job->output;
        status = napi_generic_failure;
    }
    napi_value result=Str(env,status==napi_ok||job->appOwner?job->output:"SDK trace work cancelled; initialization NOT verified");
    if(status==napi_ok) napi_resolve_deferred(env,job->deferred,result);
    else napi_reject_deferred(env,job->deferred,result);
    napi_delete_async_work(env,job->work);
    (job->appOwner?appVmBusy:sdkTraceBusy).store(false);
    delete job;
}
static napi_value SdkTrace(napi_env env,napi_callback_info info) {
    size_t argc=1,length=0; napi_value args[1]; void* mode=nullptr;
    if(napi_get_cb_info(env,info,&argc,args,nullptr,&mode)!=napi_ok || argc!=1 ||
       napi_get_value_string_utf8(env,args[0],nullptr,0,&length)!=napi_ok || length==0 || length>4096) {
        napi_throw_type_error(env,nullptr,"Expected DEX directory path"); return nullptr;
    }
    bool owner=mode && *static_cast<int*>(mode)==3;
    auto& busy=owner?appVmBusy:sdkTraceBusy;
    if(busy.exchange(true)) { napi_throw_error(env,nullptr,"SDK work already running"); return nullptr; }
    auto* job=new SdkTraceJob();
    job->jni=mode && *static_cast<int*>(mode)==1;
    job->login=mode && *static_cast<int*>(mode)==2;
    job->appOwner=owner;if(owner)job->login=true;
    std::vector<char> path(length+1);
    if(napi_get_value_string_utf8(env,args[0],path.data(),path.size(),&length)!=napi_ok) {
        delete job; busy.store(false); napi_throw_error(env,nullptr,"Cannot read DEX path"); return nullptr;
    }
    job->directory.assign(path.data(),length);
    napi_value promise,name;
    napi_status status=napi_create_promise(env,&job->deferred,&promise);
    if(status==napi_ok) status=napi_create_string_utf8(env,"Original SDK DEX trace",NAPI_AUTO_LENGTH,&name);
    if(status==napi_ok) status=napi_create_async_work(env,nullptr,name,ExecuteSdkTrace,CompleteSdkTrace,job,&job->work);
    if(status==napi_ok) status=napi_queue_async_work(env,job->work);
    if(status!=napi_ok) {
        if(job->work) napi_delete_async_work(env,job->work);
        delete job; busy.store(false); napi_throw_error(env,nullptr,"Cannot queue SDK trace"); return nullptr;
    }
    return promise;
}
static napi_value UnityBatch(napi_env env,napi_callback_info info) {
    if(sdkTraceBusy.exchange(true)) { napi_throw_error(env,nullptr,"A diagnostic is already running"); return nullptr; }
    auto* job=new SdkTraceJob(); job->unity=true;
    size_t argc=0; void* mode=nullptr;
    napi_get_cb_info(env,info,&argc,nullptr,nullptr,&mode);
    job->originalMain=mode && *static_cast<int*>(mode)==1;
    job->audio=mode && *static_cast<int*>(mode)==2;
    napi_value promise,name;
    napi_status status=napi_create_promise(env,&job->deferred,&promise);
    if(status==napi_ok) status=napi_create_string_utf8(env,"Unity dependency batch",NAPI_AUTO_LENGTH,&name);
    if(status==napi_ok) status=napi_create_async_work(env,nullptr,name,ExecuteSdkTrace,CompleteSdkTrace,job,&job->work);
    if(status==napi_ok) status=napi_queue_async_work(env,job->work);
    if(status!=napi_ok) {
        if(job->work) napi_delete_async_work(env,job->work);
        delete job; sdkTraceBusy.store(false); napi_throw_error(env,nullptr,"Cannot queue Unity diagnostic"); return nullptr;
    }
    return promise;
}
static napi_value NativeDependencies(napi_env env,napi_callback_info) {
    const char* names[]={"libc.so","libdl.so","libm.so","liblog.so","libOpenSLES.so","libandroid.so","libmediandk.so"};
    std::string report="HAP library namespace (not ABI verification)\n";
    for(const char* name:names) {
        dlerror(); void* handle=dlopen(name,RTLD_NOW|RTLD_LOCAL); const char* error=dlerror();
        report+=std::string(name)+(handle?": available":std::string(": unavailable: ")+(error?error:"unknown"))+"\n";
        if(handle) dlclose(handle);
    }
    return Str(env,report);
}
static napi_value Status(napi_env env,napi_callback_info) { return Str(env,Snapshot()); }
static napi_value Stop(napi_env env,napi_callback_info) {
    if(!renderer) return Str(env,"Audio already stopped");
    auto stop=OH_AudioRenderer_Stop(renderer); auto release=OH_AudioRenderer_Release(renderer); renderer=nullptr;
    return Str(env,"Audio stop="+std::to_string(stop)+" release="+std::to_string(release));
}
static napi_value Play(napi_env env,napi_callback_info) {
    if(renderer) return Str(env,"Stop audio before replay"); OH_AudioStreamBuilder* b=nullptr;
    OH_AudioStream_Result r=OH_AudioStreamBuilder_Create(&b,AUDIOSTREAM_TYPE_RENDERER); if(r!=AUDIOSTREAM_SUCCESS) return Str(env,"audio builder failed: "+std::to_string(r));
    OH_AudioRenderer_Callbacks cb{}; cb.OH_AudioRenderer_OnWriteData=Write; samples=0;
    const char* stage="rate";
    if((r=OH_AudioStreamBuilder_SetSamplingRate(b,48000))==AUDIOSTREAM_SUCCESS &&
       (stage="channels", (r=OH_AudioStreamBuilder_SetChannelCount(b,2))==AUDIOSTREAM_SUCCESS) &&
       (stage="sample format", (r=OH_AudioStreamBuilder_SetSampleFormat(b,AUDIOSTREAM_SAMPLE_S16LE))==AUDIOSTREAM_SUCCESS) &&
       (stage="encoding", (r=OH_AudioStreamBuilder_SetEncodingType(b,AUDIOSTREAM_ENCODING_TYPE_RAW))==AUDIOSTREAM_SUCCESS) &&
       (stage="usage", (r=OH_AudioStreamBuilder_SetRendererInfo(b,AUDIOSTREAM_USAGE_GAME))==AUDIOSTREAM_SUCCESS) &&
       (stage="latency", (r=OH_AudioStreamBuilder_SetLatencyMode(b,AUDIOSTREAM_LATENCY_MODE_FAST))==AUDIOSTREAM_SUCCESS) &&
       (stage="callback", (r=OH_AudioStreamBuilder_SetRendererCallback(b,cb,nullptr))==AUDIOSTREAM_SUCCESS)) {
        stage="generate"; r=OH_AudioStreamBuilder_GenerateRenderer(b,&renderer);
    }
    auto destroyed=OH_AudioStreamBuilder_Destroy(b);
    if(r==AUDIOSTREAM_SUCCESS && destroyed!=AUDIOSTREAM_SUCCESS) { stage="destroy builder"; r=destroyed; }
    if(r==AUDIOSTREAM_SUCCESS) { stage="start"; r=OH_AudioRenderer_Start(renderer); }
    if(r!=AUDIOSTREAM_SUCCESS) { if(renderer) OH_AudioRenderer_Release(renderer); renderer=nullptr; return Str(env,std::string("audio ")+stage+" failed: "+std::to_string(r)); }
    OH_AudioStream_LatencyMode mode; r=OH_AudioRenderer_GetLatencyMode(renderer,&mode); return Str(env,"Pulse started; latency query="+std::to_string(r)+" mode="+std::to_string(r==AUDIOSTREAM_SUCCESS?int(mode):-1)+" (1=fast)");
}
// R2 诊断入口：只执行 dlopen/dlsym 与 ELF 静态摘要，绝不调用 Java_* 导出。
// 可选参数为额外搜索目录（字符串），优先顺序由 native_probe.c 固定：
// /data/storage/el1/bundle/libs/arm64 -> 该参数 -> NATIVE_PROBE_LIB_DIR。
static napi_value NativeProbeRun(napi_env env,napi_callback_info info) {
    size_t argc=1; napi_value argv[1]={nullptr}; char dir[512]={0}; size_t len=0;
    bool hasDir=false;
    if(napi_get_cb_info(env,info,&argc,argv,nullptr,nullptr)==napi_ok && argc>=1 && argv[0]) {
        napi_valuetype type=napi_undefined;
        if(napi_typeof(env,argv[0],&type)==napi_ok && type==napi_string &&
           napi_get_value_string_utf8(env,argv[0],dir,sizeof(dir),&len)==napi_ok &&
           len>0 && len<sizeof(dir)-1) {
            hasDir=true;
        }
    }
    if(hasDir) sdk_native_probe_run_with_dir(dir);
    else sdk_native_probe_run();
    return Str(env,sdk_native_probe_json());
}
// 自定义 ELF64 加载器探针：真实 mmap/重定位原 Android 库，只查找 Java_ 导出。
// Host device identity for android.os.Build.  Values come from ArkTS
// @ohos.deviceInfo; missing/empty values stay unset (Build reads fail closed).
static napi_value SetDeviceSnapshot(napi_env env,napi_callback_info info) {
    size_t argc=8; napi_value argv[8]={nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
    if(napi_get_cb_info(env,info,&argc,argv,nullptr,nullptr)!=napi_ok||argc<6) return nullptr;
    /* The first six values are the original deviceInfo snapshot.  The optional
     * seventh value is deviceInfo.abiList, a host fact used only for the real
     * Build.SUPPORTED_ABIS array; omitting it leaves the list unset rather
     * than inventing an ABI. */
    size_t count=argc<7?6:argc<8?7:8;
    char values[8][256]={{0}};
    for(size_t i=0;i<count;i++){
        size_t len=0;
        napi_valuetype type=napi_undefined;
        if(napi_typeof(env,argv[i],&type)!=napi_ok||type!=napi_string) return nullptr;
        if(napi_get_value_string_utf8(env,argv[i],values[i],sizeof(values[i]),&len)!=napi_ok) return nullptr;
    }
    void* h=dlopen("libsdktrace.so",RTLD_NOW|RTLD_LOCAL);
    if(h){
        auto set=reinterpret_cast<void(*)(const char*,const char*,const char*,const char*,const char*,const char*)>(dlsym(h,"device_runtime_set_snapshot"));
        if(set) set(values[0],values[1],values[2],values[3],values[4],values[5]);
        if(count>=7){
            auto set_abis=reinterpret_cast<void(*)(const char*)>(dlsym(h,"device_runtime_set_abis"));
            if(set_abis) set_abis(values[6]);
        }
        if(count>=8){
            auto set_hardware=reinterpret_cast<void(*)(const char*)>(dlsym(h,"device_runtime_set_hardware"));
            if(set_hardware) set_hardware(values[7]);
            OH_LOG_Print(LOG_APP,LOG_INFO,0xD003D00,"NativeProbe",
                "BUILD_HARDWARE source=deviceInfo.hardwareModel bytes=%{public}zu setter=%{public}d",
                strlen(values[7]),set_hardware!=nullptr);
        }
    }
    return nullptr;
}
static napi_value ElfLoaderProbe(napi_env env,napi_callback_info info) {
    (void)info;
    void* h=dlopen("libsdktrace.so",RTLD_NOW|RTLD_LOCAL);
    if(!h){const char* e=dlerror();return Str(env,std::string("elf loader probe: libsdktrace.so unavailable: ")+(e?e:"unknown"));}
    auto run=reinterpret_cast<int(*)(const char*)>(dlsym(h,"elf_loader_probe_run"));
    auto json=reinterpret_cast<const char*(*)()>(dlsym(h,"elf_loader_probe_json"));
    if(!run||!json){const char* e=dlerror();return Str(env,std::string("elf loader probe: symbols unavailable: ")+(e?e:"unknown"));}
    int rc=run("/data/storage/el1/bundle/libs/arm64");
    std::string report=json()?json():"";
    report += "\nELF_LOADER_PROBE_RC="+std::to_string(rc);
    return Str(env,report);
}
static napi_value PageGeometry(napi_env env,napi_callback_info info) {
    size_t argc=1;napi_value arg;char directory[2048];size_t length=0;
    if(napi_get_cb_info(env,info,&argc,&arg,nullptr,nullptr)!=napi_ok||argc!=1||
       napi_get_value_string_utf8(env,arg,directory,sizeof directory,&length)!=napi_ok||!length||length>=sizeof directory-1){
        napi_throw_error(env,nullptr,"E-PAGE-GEOMETRY-PATH");return nullptr;
    }
    page_geometry_configure(directory);
    {std::lock_guard<std::mutex> g(payloadStateLock);payloadRoot=directory;}
    PayloadLog(page_geometry_status());
    return Str(env,page_geometry_status());
}
static napi_value PageMapProbe(napi_env env,napi_callback_info info) {
    size_t argc=1,length=0;napi_value arg;char directory[2048];
    if(napi_get_cb_info(env,info,&argc,&arg,nullptr,nullptr)!=napi_ok||argc!=1||
       napi_get_value_string_utf8(env,arg,directory,sizeof directory,&length)!=napi_ok||!length||length>=sizeof directory-1){
        napi_throw_error(env,nullptr,"E-PAGE-PROBE-PATH");return nullptr;
    }
    static void *runtime=dlopen("libsdktrace.so",RTLD_NOW|RTLD_LOCAL);
    if(!runtime){napi_throw_error(env,nullptr,"E-PAGE-PROBE-RUNTIME");return nullptr;}
    auto configure=reinterpret_cast<void(*)(const char*)>(dlsym(runtime,"page_geometry_configure"));
    auto run=reinterpret_cast<int(*)(const char*)>(dlsym(runtime,"elf_loader_probe_run"));
    auto json=reinterpret_cast<const char*(*)()>(dlsym(runtime,"elf_loader_probe_json"));
    auto host=reinterpret_cast<size_t(*)()>(dlsym(runtime,"host_page_size"));
    auto guest=reinterpret_cast<size_t(*)()>(dlsym(runtime,"guest_page_align"));
    if(!configure||!run||!json||!host||!guest){napi_throw_error(env,nullptr,"E-PAGE-PROBE-ABI");return nullptr;}
    configure(directory);
    int rc=run("/data/storage/el1/bundle/libs/arm64");
    std::string report="{\"pid\":"+std::to_string(getpid())+",\"host_page_size\":"+std::to_string(host())+
      ",\"guest_page_align\":"+std::to_string(guest())+",\"constructors_run\":false,\"jni_called\":false,\"rc\":"+std::to_string(rc)+",\"probe\":"+(json()?json():"null")+"}\n";
    FILE *f=fopen((std::string(directory)+"/page-map-probe.json").c_str(),"w");
    if(!f){napi_throw_error(env,nullptr,"E-PAGE-PROBE-WRITE");return nullptr;}
    fchmod(fileno(f),0666);size_t written=fwrite(report.data(),1,report.size(),f);int closed=fclose(f);
    if(written!=report.size()||closed){napi_throw_error(env,nullptr,"E-PAGE-PROBE-WRITE");return nullptr;}
    PayloadLog("page_map_probe_complete rc="+std::to_string(rc)+" constructors=0 jni=0 gameplay=0");
    return Str(env,report);
}
static napi_value Init(napi_env env,napi_value exports) {
    RecStart(env);
    napi_property_descriptor payload[]={
        {"pageMapProbe",nullptr,PageMapProbe,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"pageGeometry",nullptr,PageGeometry,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"prepareNativeAssets",nullptr,PrepareNativeAssets,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"apkVersionInfo",nullptr,ApkVersionInfo,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"nativeAssetsStatus",nullptr,NativeAssetsStatus,nullptr,nullptr,nullptr,napi_default,nullptr}};
    napi_define_properties(env,exports,sizeof(payload)/sizeof(payload[0]),payload);
    napi_property_descriptor batteryUpdate={"batteryUpdate",nullptr,BatteryUpdate,nullptr,nullptr,nullptr,napi_default,nullptr};
    napi_define_properties(env,exports,1,&batteryUpdate);
    napi_property_descriptor ack={"ackGuestView",nullptr,AckGuestView,nullptr,nullptr,nullptr,napi_default,nullptr};
    napi_define_properties(env,exports,1,&ack);
    static int loginMode=2;
    napi_property_descriptor login={"loginTrace",nullptr,SdkTrace,nullptr,nullptr,nullptr,napi_default,&loginMode};
    static int appOwnerMode=3;
    napi_property_descriptor appOwner={"appVmCreate",nullptr,SdkTrace,nullptr,nullptr,nullptr,napi_default,&appOwnerMode};
    napi_define_properties(env,exports,1,&appOwner);
    napi_define_properties(env,exports,1,&login);
    static int jniMode=1;
    napi_property_descriptor jni={"jniBatch",nullptr,SdkTrace,nullptr,nullptr,nullptr,napi_default,&jniMode};
    napi_define_properties(env,exports,1,&jni);
    static int audioMode=2;
    napi_property_descriptor audio={"audioBridge",nullptr,UnityBatch,nullptr,nullptr,nullptr,napi_default,&audioMode};
    napi_define_properties(env,exports,1,&audio);
    static int originalMode=1;
    napi_property_descriptor original={"originalMain",nullptr,UnityBatch,nullptr,nullptr,nullptr,napi_default,&originalMode};
    napi_define_properties(env,exports,1,&original);
    napi_property_descriptor batch={"unityBatch",nullptr,UnityBatch,nullptr,nullptr,nullptr,napi_default,nullptr};
    napi_define_properties(env,exports,1,&batch);
    napi_value x; if(napi_get_named_property(env,exports,OH_NATIVE_XCOMPONENT_OBJ,&x)==napi_ok) { OH_NativeXComponent* c=nullptr; if(napi_unwrap(env,x,reinterpret_cast<void**>(&c))==napi_ok && c) OH_NativeXComponent_RegisterCallback(c,&callbacks); }
    napi_property_descriptor p[]={
        {"loginSubmit",nullptr,LoginSubmit,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"loginSavedLogin",nullptr,LoginSavedLogin,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"loginGetState",nullptr,LoginGetState,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"loginCaptchaResult",nullptr,LoginCaptchaResult,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"webviewCommand",nullptr,WebviewCommand,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"test",nullptr,Test,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"status",nullptr,Status,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"play",nullptr,Play,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"stop",nullptr,Stop,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"subscribe",nullptr,Subscribe,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"unsubscribe",nullptr,Unsubscribe,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"androidElf",nullptr,AndroidElf,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"androidLibc",nullptr,AndroidLibc,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"sdkTrace",nullptr,SdkTrace,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"appVmWindowReady",nullptr,AppVmWindowReady,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"appVmHeartbeat",nullptr,AppVmHeartbeat,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"loadingStatus",nullptr,LoadingStatus,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"audioLatencyStatus",nullptr,AudioLatencyStatus,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"appVmForeground",nullptr,AppVmForeground,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"appVmDestroy",nullptr,AppVmDestroy,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"nativeDependencies",nullptr,NativeDependencies,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"registerHostHandler",nullptr,RegisterHostHandler,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"unregisterHostHandler",nullptr,UnregisterHostHandler,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"hostReply",nullptr,HostReply,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"hostReplyNetwork",nullptr,HostReplyNetwork,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"setDisplaySnapshot",nullptr,SetDisplaySnapshot,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"clearDisplaySnapshot",nullptr,ClearDisplaySnapshot,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"hostProbe",nullptr,HostProbe,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"hostStatus",nullptr,HostStatus,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"nativeProbeRun",nullptr,NativeProbeRun,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"elfLoaderProbe",nullptr,ElfLoaderProbe,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"setDeviceSnapshot",nullptr,SetDeviceSnapshot,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"setTimeZoneSnapshot",nullptr,SetTimeZoneSnapshot,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"injectTap",nullptr,InjectTap,nullptr,nullptr,nullptr,napi_default,nullptr}
    }; napi_define_properties(env,exports,sizeof(p)/sizeof(p[0]),p); return exports;
}
static napi_module mod={1,0,nullptr,Init,"probe",nullptr,{0}};
extern "C" __attribute__((constructor)) void Register() { napi_module_register(&mod); }

