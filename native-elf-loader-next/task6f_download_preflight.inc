/* Opt-in TASK6f observer on the existing UnityMain/nativeRender return.
 * One directory/file round trip and one HEAD, no handlers/downloads patched. */
#include <stdarg.h>
#include <sys/statvfs.h>
static struct {
 struct {
  void *(*domain)(void);const void **(*assemblies)(void *,size_t *);
  const void *(*image)(const void *);void *(*klass)(const void *,const char *,const char *);
  void *(*method)(void *,const char *,int);void *(*invoke)(const void *,void *,void **,void **);
  void *(*object_class)(void *);void *(*parent)(void *);void *(*field)(void *,const char *);
  size_t (*offset)(void *);uint32_t (*field_flags)(void *);void (*get)(void *,void *,void *);void (*static_get)(void *,void *);
  void *(*unbox)(void *);void *(*new_string)(const char *);int32_t (*string_length)(void *);const uint16_t *(*string_chars)(void *);
  const char *(*class_name)(void *);uint32_t (*pin)(void *,bool);void *(*target)(uint32_t);void (*unpin)(uint32_t);
 } api;
 int setup,ready,path_done,network_done,stopped;uint32_t request;uint64_t started,last_poll;unsigned ticks;char click_label[64];float screen_x,screen_y;
} task6f;
static uint64_t task6f_ms(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;}
static void task6f_log(const char *fmt,...){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task6f-preflight.txt","a");if(f){dprintf(fileno(f),"TASK6F pid=%d mono_ms=%llu ",getpid(),(unsigned long long)task6f_ms());va_list a;va_start(a,fmt);vdprintf(fileno(f),fmt,a);va_end(a);dprintf(fileno(f),"\n");fclose(f);}errno=saved;}
static void *task6f_class(const char *ns,const char *name){size_t count=0;void *d=task6f.api.domain();const void **as=d?task6f.api.assemblies(d,&count):NULL;if(!as||count>1024)return NULL;for(size_t i=0;i<count;i++){const void *im=task6f.api.image(as[i]);void *c=im?task6f.api.klass(im,ns,name):NULL;if(c)return c;}return NULL;}
static void *task6f_call(void *c,void *obj,const char *name,int n,void **a,int *ok){
 *ok=0;void *m=NULL;for(int i=0;c&&i<32&&!m;i++,c=task6f.api.parent(c))m=task6f.api.method(c,name,n);
 if(!m){task6f_log("call=%s argc=%d missing_method=1",name,n);return NULL;}
 void *error=NULL,*result=task6f.api.invoke(m,obj,a,&error);*ok=error==NULL;
 if(error){void *ec=task6f.api.object_class(error);task6f_log("call=%s argc=%d exception_type=%s",name,n,ec?task6f.api.class_name(ec):"unknown");}return result;
}
static int task6f_field(void *obj,const char *name,void *out){if(!obj)return 0;void *c=task6f.api.object_class(obj);for(int i=0;c&&i<32;i++,c=task6f.api.parent(c)){void *f=task6f.api.field(c,name);if(f){task6f.api.get(obj,f,out);return 1;}}return 0;}
static int task6f_static(void *c,const char *name,void *out){for(int i=0;c&&i<32;i++,c=task6f.api.parent(c)){void *f=task6f.api.field(c,name);if(f){task6f.api.static_get(f,out);return 1;}}return 0;}
static void task6f_string(void *str,char *dst,size_t cap){if(!cap)return;dst[0]=0;if(!str)return;int32_t n=task6f.api.string_length(str);const uint16_t *s=task6f.api.string_chars(str);size_t at=0;if(n<0||n>1048576||!s)return;for(int32_t i=0;i<n;i++){uint32_t cp=s[i];if(cp>=0xd800&&cp<=0xdbff&&i+1<n&&s[i+1]>=0xdc00&&s[i+1]<=0xdfff){cp=0x10000+((cp-0xd800)<<10)+s[++i]-0xdc00;}else if(cp>=0xd800&&cp<=0xdfff)cp=0xfffd;unsigned width=cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;if(at+width>=cap)break;if(width==1)dst[at++]=(char)(cp>=32?cp:' ');else if(width==2){dst[at++]=0xc0|(cp>>6);dst[at++]=0x80|(cp&63);}else if(width==3){dst[at++]=0xe0|(cp>>12);dst[at++]=0x80|((cp>>6)&63);dst[at++]=0x80|(cp&63);}else{dst[at++]=0xf0|(cp>>18);dst[at++]=0x80|((cp>>12)&63);dst[at++]=0x80|((cp>>6)&63);dst[at++]=0x80|(cp&63);}}dst[at]=0;}
static void task6f_path(void){
 int ok=0;void *app=task6f_class("UnityEngine","Application");void *str=task6f_call(app,NULL,"get_persistentDataPath",0,NULL,&ok);char path[4096];task6f_string(str,path,sizeof path);
 task6f_log("persistentDataPath getter_ok=%d path=%s",ok,path);task6f.path_done=1;if(!ok||!path[0])return;
 void *dir=task6f_class("System.IO","Directory"),*file=task6f_class("System.IO","File");void *args[2]={str};
 task6f_call(dir,NULL,"CreateDirectory",1,args,&ok);task6f_log("path_create_base ok=%d",ok);if(!ok)return;
 struct statvfs fs;int rc=statvfs(path,&fs),err=errno;task6f_log("storage path=%s statvfs_rc=%d errno=%d available_bytes=%llu total_bytes=%llu",path,rc,rc?err:0,rc?0:(unsigned long long)fs.f_bavail*fs.f_frsize,rc?0:(unsigned long long)fs.f_blocks*fs.f_frsize);
 char testdir[4300],testpath[4400];snprintf(testdir,sizeof testdir,"%s/.task6f-preflight-%d",path,getpid());snprintf(testpath,sizeof testpath,"%s/probe.txt",testdir);
 void *ds=task6f.api.new_string(testdir),*ps=task6f.api.new_string(testpath);args[0]=ds;task6f_call(dir,NULL,"CreateDirectory",1,args,&ok);task6f_log("path_create_test ok=%d directory=%s",ok,testdir);if(!ok)return;
 const char expected[]="TASK6f UTF-8 probe: \xE4\xB8\xAD\xE6\x96\x87\n";args[0]=ps;args[1]=task6f.api.new_string(expected);task6f_call(file,NULL,"WriteAllText",2,args,&ok);int wrote=ok;
 void *readback=task6f_call(file,NULL,"ReadAllText",1,args,&ok);int matched=ok&&readback&&task6f.api.string_length(readback)==task6f.api.string_length(args[1])&&!memcmp(task6f.api.string_chars(readback),task6f.api.string_chars(args[1]),(size_t)task6f.api.string_length(readback)*2);
 task6f_call(file,NULL,"Delete",1,args,&ok);int deleted=ok;void *exists=task6f_call(file,NULL,"Exists",1,args,&ok);int gone=ok&&exists&&!*(uint8_t*)task6f.api.unbox(exists);
 args[0]=ds;task6f_call(dir,NULL,"Delete",1,args,&ok);task6f_log("path_roundtrip write=%d read_equal=%d delete=%d file_absent=%d directory_delete=%d test_bytes=%zu",wrote,matched,deleted,gone,ok,sizeof expected-1);
}
static void task6f_network(void){
 void *c=task6f_class("UnityEngine.Networking","UnityWebRequest");int ok=0;
 if(!task6f.request){
  const char *url="https://line3-prod-patch-bd.bilibiligame.net/assetbundle/";void *a[1]={task6f.api.new_string(url)};
  void *r=task6f_call(c,NULL,"Head",1,a,&ok);if(!ok||!r){task6f.network_done=1;task6f_log("network stage=factory method=HEAD ok=0");return;}
  task6f.request=task6f.api.pin(r,false);if(!task6f.request){task6f.network_done=1;task6f_log("network stage=pin ok=0");return;}
  int timeout=15;void *numbers[1]={&timeout};task6f_call(c,r,"set_timeout",1,numbers,&ok);if(!ok){task6f.api.unpin(task6f.request);task6f.request=0;task6f.network_done=1;task6f_log("network stage=timeout_setup ok=0 request_not_sent=1");return;}
  task6f.started=task6f_ms();task6f_call(c,r,"SendWebRequest",0,NULL,&ok);task6f_log("network stage=send stack=UnityWebRequest method=HEAD url=%s body_upload=0 redirect_limit=not_exposed_by_original_API timeout_s=15 timeout_set=1 ok=%d",url,ok);
  if(!ok){task6f.api.unpin(task6f.request);task6f.request=0;task6f.network_done=1;}return;
 }
 uint64_t now=task6f_ms();if(now-task6f.last_poll<100)return;task6f.last_poll=now;
 void *r=task6f.api.target(task6f.request),*done=task6f_call(c,r,"get_isDone",0,NULL,&ok);int finished=ok&&done&&*(uint8_t*)task6f.api.unbox(done);if(!finished&&now-task6f.started<20000)return;
 if(!finished){task6f_call(c,r,"Abort",0,NULL,&ok);task6f_log("network stage=timeout elapsed_ms=%llu",(unsigned long long)(now-task6f.started));}
 void *status=task6f_call(c,r,"get_responseCode",0,NULL,&ok);int64_t http=ok&&status?*(int64_t*)task6f.api.unbox(status):0;
 char error[512],type[256];task6f_string(task6f_call(c,r,"get_error",0,NULL,&ok),error,sizeof error);void *a[1]={task6f.api.new_string("Content-Type")};task6f_string(task6f_call(c,r,"GetResponseHeader",1,a,&ok),type,sizeof type);
 void *received=task6f_call(c,r,"get_downloadedBytes",0,NULL,&ok);uint64_t bytes=ok&&received?*(uint64_t*)task6f.api.unbox(received):UINT64_MAX;
 task6f_log("network stage=complete isDone=%d HTTP=%lld Content-Type=%s elapsed_ms=%llu downloaded_bytes=%llu error=%s TLS=%s certificate_handler=unmodified_default body_request=HEAD",finished,(long long)http,type,(unsigned long long)(now-task6f.started),(unsigned long long)bytes,error,http>0?"HTTPS_response_observed":"not_proven");
 task6f_call(c,r,"Dispose",0,NULL,&ok);task6f.api.unpin(task6f.request);task6f.request=0;task6f.network_done=1;
}
#include "task6f_ugui_observer.inc"
#include "multitouch_unity.inc"
#ifdef LUNARIA_TASK6K_OBSERVER
#include "task6k_title_observer.inc"
#endif
#ifdef LUNARIA_TASK8A
#include "task8a_title_trace.inc"
#endif
static void task6f_preflight_tick(void){
 if(task6f.stopped)return;
 if(egl_window_runtime_successful_submissions()<300)return;
 if(!task6f.setup){task6f.setup=1;char error[256]={0};elf_loader_t *l=elf_loader_acquire_initialized_cached("/data/storage/el1/bundle/libs/arm64/libil2cpp.so",error,sizeof error);if(!l){task6f_log("setup ok=0 cached_only=1 error=%s",error);return;}
#define T6_API(member,symbol) task6f.api.member=(void*)elf_loader_own_executable_symbol(l,symbol)
  T6_API(domain,"il2cpp_domain_get");T6_API(assemblies,"il2cpp_domain_get_assemblies");T6_API(image,"il2cpp_assembly_get_image");T6_API(klass,"il2cpp_class_from_name");T6_API(method,"il2cpp_class_get_method_from_name");T6_API(invoke,"il2cpp_runtime_invoke");T6_API(object_class,"il2cpp_object_get_class");T6_API(parent,"il2cpp_class_get_parent");T6_API(field,"il2cpp_class_get_field_from_name");T6_API(offset,"il2cpp_field_get_offset");T6_API(get,"il2cpp_field_get_value");T6_API(static_get,"il2cpp_field_static_get_value");T6_API(unbox,"il2cpp_object_unbox");T6_API(new_string,"il2cpp_string_new");T6_API(string_length,"il2cpp_string_length");T6_API(string_chars,"il2cpp_string_chars");T6_API(class_name,"il2cpp_class_get_name");T6_API(pin,"il2cpp_gchandle_new");T6_API(target,"il2cpp_gchandle_get_target");T6_API(unpin,"il2cpp_gchandle_free");
  T6_API(field_flags,"il2cpp_field_get_flags");
#undef T6_API
  task6f.ready=task6f.api.domain&&task6f.api.assemblies&&task6f.api.image&&task6f.api.klass&&task6f.api.method&&task6f.api.invoke&&task6f.api.object_class&&task6f.api.parent&&task6f.api.field&&task6f.api.offset&&task6f.api.get&&task6f.api.static_get&&task6f.api.unbox&&task6f.api.new_string&&task6f.api.string_length&&task6f.api.string_chars&&task6f.api.class_name&&task6f.api.pin&&task6f.api.target&&task6f.api.unpin;elf_loader_close(l);task6f_log("setup ok=%d cached_only=1 thread=UnityMain nativeRender_return=1",task6f.ready);
 }
 if(!task6f.ready)return;
 multitouch_unity_tick();
#ifdef LUNARIA_TASK8A
 task6k_setup();
 /* Preserve SDK title-progress notifications. Repeated delegate/map scans
  * and scene inventories are diagnostics, not gameplay or login logic. */
 if(perf_jitter_quiet()){if(task6k.title)path_home_title_tick();return;}
 perf_result_event(13,1,0);task8a_title_trace_setup();perf_result_event(14,1,0);
#endif
#ifdef LUNARIA_TASK6K_OBSERVER
 task6k_observer_tick();return;
#endif
 if((++task6f.ticks%6)==0){FILE *marker=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task6f-click-label.txt","r");if(marker){char label[64]={0};float x=0,y=0;if(fscanf(marker,"%63s %f %f",label,&x,&y)==3&&strcmp(label,task6f.click_label)){snprintf(task6f.click_label,sizeof task6f.click_label,"%s",label);task6f.screen_x=x;task6f.screen_y=y;task6f_log("click_label=%s screen_x=%.1f screen_y=%.1f",label,x,y);if(!strcmp(label,"STOP"))task6f.stopped=1;}fclose(marker);}}
 if(task6f.stopped)return;if(!task6f.path_done)task6f_path();if(!task6f.network_done)task6f_network();task6f_ugui_tick();
}
