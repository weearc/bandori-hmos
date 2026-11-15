/* Generic shadow WebView transport. VM objects are touched only by the GIL
 * owner; ArkTS commands are bounded, copied, and drained as independent events.
 * No URLs, headers, cookies, JS payloads or page text are written to diagnostics. */
#include "view_state_runtime.h"
#include "web_cookie_runtime.h"
#include <stdarg.h>
enum { WEB_SLOTS=32, WEB_QUEUE=32, WEB_INTERFACES=16 };
struct web_binding {dvm_ref object;char name[128];};
struct web_page {struct dvm *vm;dvm_ref web,context;unsigned serial,revision;int open;char *snapshot;size_t snapshot_bytes;char id[80];struct web_binding interfaces[WEB_INTERFACES];};
static struct web_page web_pages[WEB_SLOTS];
static struct {struct dvm *vm;dvm_ref container,listener;} web_listeners[64];
static struct {char id[80],action[180],payload[8193];} web_commands[WEB_QUEUE];
static unsigned web_serial,web_revision,web_head,web_count;
static pthread_mutex_t web_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int web_polling;
static struct dvm *web_gc_vm;
static bool webview_panel_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 if(!v||!visit||context!=&web_gc_vm||!dvm_gil_held(v))return false;
 /* Guest refs are written only by the GIL owner. web_lock protects the host
  * text-command queue/open flag, which are deliberately not visited here.
  * Do not lock web_lock then invoke GC/guest work or nest panel_lock. */
 for(unsigned i=0;i<WEB_SLOTS;i++)if(web_pages[i].vm==v&&web_pages[i].web){
  struct web_page *p=&web_pages[i];visit(&p->web,user);
  if(p->context)visit(&p->context,user);
  for(unsigned j=0;j<WEB_INTERFACES;j++)if(p->interfaces[j].object)visit(&p->interfaces[j].object,user);
 }
 for(unsigned i=0;i<64;i++)if(web_listeners[i].vm==v){
  if(web_listeners[i].container)visit(&web_listeners[i].container,user);
  if(web_listeners[i].listener)visit(&web_listeners[i].listener,user);
 }
 return true;
}
static bool webview_panel_gc_bind(struct dvm *v){
 if(!v||!dvm_gil_held(v))return false;
 if(web_gc_vm==v)return true;
 if(web_gc_vm)return false;
 if(!dvm_gc_root_provider_add(v,"webview-panel/strong",DVM_GC_ROOT_STRONG,webview_panel_gc_roots,&web_gc_vm))return false;
 web_gc_vm=v;return true;
}
static void web_log(struct dvm *v,const char *stage,const char *id,int ok){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/webview-events.txt","a");
 if(f){fprintf(f,"WEBVIEW pid=%d mono_ms=%llu thread=%u stage=%s id=%s ok=%d exception_class=%s\n",getpid(),(unsigned long long)monotonic_ms(),v?v->cur_thread:0,stage,id?id:"",ok,v&&v->exception&&dvm_object_class(v,v->exception)?dvm_object_class(v,v->exception)->name:"none");
 if(v&&v->exception)for(int i=0;i<v->nexc_trace&&i<8;i++){struct dvm_method *m=v->exc_trace[i];fprintf(f,"WEBVIEW_EXCEPTION_FRAME pid=%d depth=%d owner=%s method=%s sig=%s throw_pc=%u\n",getpid(),i,m&&m->cls?m->cls->name:"?",m?m->name:"?",m?m->sig:"?",v->exc_pc);}
 fclose(f);}
}
static void web_emit(const char *event){pthread_mutex_lock(&panel_lock);void(*fn)(const char*)=emit_host;pthread_mutex_unlock(&panel_lock);if(fn)fn(event);}
static struct web_page *web_find(struct dvm *v,dvm_ref web,int create){
 if(create&&!webview_panel_gc_bind(v)){dvm__throw(v,"java/lang/OutOfMemoryError","Web root registration failed");return NULL;}
 for(unsigned i=0;i<WEB_SLOTS;i++)if(web_pages[i].vm==v&&web_pages[i].web==web)return &web_pages[i];
 if(create)for(unsigned i=0;i<WEB_SLOTS;i++)if(!web_pages[i].web){struct web_page *p=&web_pages[i];p->vm=v;p->web=web;dvm_pin(v,web);return p;}
 return NULL;
}
static void webview_panel_interface(struct dvm *v,dvm_ref web,dvm_ref obj,const char *name){
 if(!name||strlen(name)>=sizeof web_pages[0].interfaces[0].name){dvm__throw(v,"java/lang/IllegalArgumentException","JS interface name too long");return;}
 struct web_page *p=web_find(v,web,obj!=0);if(!p)return;int slot=-1;
 for(unsigned i=0;i<WEB_INTERFACES;i++)if(p->interfaces[i].object&&!strcmp(p->interfaces[i].name,name)){slot=(int)i;break;}
 if(slot<0&&obj)for(unsigned i=0;i<WEB_INTERFACES;i++)if(!p->interfaces[i].object){slot=(int)i;break;}
 if(slot<0){if(obj)dvm__throw(v,"java/lang/IllegalStateException","Web bridge interface capacity");return;}
 if(obj)dvm_pin(v,obj);if(p->interfaces[slot].object)dvm_unpin(v,p->interfaces[slot].object);p->interfaces[slot].object=obj;snprintf(p->interfaces[slot].name,sizeof p->interfaces[slot].name,"%s",obj?name:"");
}
struct web_json {char *s;size_t used,cap;int ok;};
static void web_add(struct web_json *j,const char *fmt,...){if(!j->ok)return;va_list a;va_start(a,fmt);int n=vsnprintf(j->s+j->used,j->cap-j->used,fmt,a);va_end(a);if(n<0||(size_t)n>=j->cap-j->used){j->ok=0;return;}j->used+=(size_t)n;}
static void web_string(struct web_json *j,const char *s){web_add(j,"\"");for(;s&&*s;s++){unsigned char c=*s;if(c=='\\'||c=='\"')web_add(j,"\\%c",c);else if(c<32)web_add(j,"\\u%04x",c);else web_add(j,"%c",c);}web_add(j,"\"");}
static int web_js_supported(const char *name,const char *sig){
 if(!strcmp(name,"handler"))return !strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
 if(!strcmp(name,"onJSAndDomReady"))return !strcmp(sig,"()V");
 const char *names[]={"close","finishWithResult","goBackWithResult","call","CallbackFromWebView"};
 for(unsigned i=0;i<sizeof names/sizeof *names;i++)if(!strcmp(name,names[i]))return !strcmp(sig,"(Ljava/lang/String;)V");
 return 0;
}
static int web_headers(struct dvm *v,dvm_ref map,struct web_json *j){
 web_add(j,"[");if(!map){web_add(j,"]");return 1;}
 union dvm_value entries={0},it={0},entry={0},has={0},key={0},value={0};
 if(!invoke(v,map,"entrySet","()Ljava/util/Set;",NULL,0,&entries)||!invoke(v,entries.l,"iterator","()Ljava/util/Iterator;",NULL,0,&it))return 0;
 unsigned count=0;
 while(invoke(v,it.l,"hasNext","()Z",NULL,0,&has)&&has.i){
  if(count>=64){dvm__throw(v,"java/lang/IllegalArgumentException","Web header capacity");return 0;}
  if(!invoke(v,it.l,"next","()Ljava/lang/Object;",NULL,0,&entry)||!invoke(v,entry.l,"getKey","()Ljava/lang/Object;",NULL,0,&key)||!invoke(v,entry.l,"getValue","()Ljava/lang/Object;",NULL,0,&value))return 0;
  const char *k=dvm_string_utf8(v,key.l),*s=dvm_string_utf8(v,value.l);if(!k||!s){dvm__throw(v,"java/lang/IllegalArgumentException","Web headers must be strings");return 0;}
  web_add(j,"%s{\"headerKey\":",count++?",":"");web_string(j,k);web_add(j,",\"headerValue\":");web_string(j,s);web_add(j,"}");
 }
 web_add(j,"]");return !dvm_exception(v);
}
void webview_panel_open(struct dvm *v,dvm_ref web,dvm_ref context,const char *url,dvm_ref headers,const char *ua){
 if(!url)return;
 if(!strncmp(url,"javascript:",11)){
  struct web_page *p=web_find(v,web,0);if(!p||!p->open){web_log(v,"evaluate_no_page",NULL,0);return;}
  char *event=malloc(65536);if(!event){dvm__throw(v,"java/lang/OutOfMemoryError","Web JavaScript encoding");return;}
  struct web_json j={event,0,65536,1};web_add(&j,"{\"type\":\"webview/evaluate\",\"id\":");web_string(&j,p->id);web_add(&j,",\"revision\":%u,\"script\":",p->revision);web_string(&j,url+11);web_add(&j,"}");
  if(j.ok){web_emit(event);web_log(v,"evaluate",p->id,1);}else{web_log(v,"evaluate_capacity",p->id,0);dvm__throw(v,"java/lang/IllegalArgumentException","Web JavaScript exceeds transport capacity");}
  wipe(event,65536);free(event);return;
 }
 if(strncmp(url,"https://",8)&&strncmp(url,"http://",7)&&strcmp(url,"about:blank")){dvm__throw(v,"java/lang/UnsupportedOperationException","Web URL scheme unsupported");return;}
 struct web_page *p=web_find(v,web,1);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","Web page registry capacity");return;}
 if(context)dvm_pin(v,context);if(p->context)dvm_unpin(v,p->context);p->context=context;
 if(!p->open){p->serial=++web_serial;snprintf(p->id,sizeof p->id,"%d-%u-%u",getpid(),web,p->serial);}
 char *event=malloc(65536);if(!event){dvm__throw(v,"java/lang/OutOfMemoryError","Web event encoding");return;}
 unsigned revision=++web_revision;
 struct web_json j={event,0,65536,1};web_add(&j,"{\"type\":\"webview/open\",\"id\":");web_string(&j,p->id);web_add(&j,",\"webRef\":%u,\"revision\":%u,\"url\":",web,revision);web_string(&j,url);web_add(&j,",\"userAgent\":");web_string(&j,ua?ua:"");web_add(&j,",\"headers\":");int ok=web_headers(v,headers,&j);web_add(&j,",\"interfaces\":[");unsigned count=0;
 for(unsigned i=0;i<WEB_INTERFACES;i++)if(p->interfaces[i].object){
  web_add(&j,"%s{\"name\":",count++?",":"");web_string(&j,p->interfaces[i].name);web_add(&j,",\"methods\":[");unsigned n=0;
  struct dvm_class *c=dvm_object_class(v,p->interfaces[i].object);
  for(;c&&strcmp(c->name,"java/lang/Object");c=c->super)for(int k=0;k<c->nmethods;k++){struct dvm_method *m=&c->methods[k];if((m->access&1)&&web_js_supported(m->name,m->sig)){web_add(&j,"%s",n++?",":"");web_string(&j,m->name);}}
  web_add(&j,"]}");
 }
 web_add(&j,"]}");if(!ok||!j.ok){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalArgumentException","Web event exceeds transport capacity");free(event);return;}
 if(p->snapshot){wipe(p->snapshot,p->snapshot_bytes);free(p->snapshot);}p->snapshot=event;p->snapshot_bytes=j.used+1;p->revision=revision;
 pthread_mutex_lock(&web_lock);p->open=1;pthread_mutex_unlock(&web_lock);web_emit(p->snapshot);web_log(v,"open",p->id,1);
 FILE *meta=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/webview-events.txt","a");if(meta){fprintf(meta,"WEBVIEW_META id=%s url_bytes=%zu headers_present=%d interfaces=%u cookies_backend=ArkWeb_shared ua_backend=ArkWeb_controller\n",p->id,strlen(url),headers!=0,count);fclose(meta);}
}
void webview_panel_destroy(struct dvm *v,dvm_ref web){
 struct web_page *p=web_find(v,web,0);if(!p)return;char id[80];strcpy(id,p->id);
 pthread_mutex_lock(&web_lock);int opened=p->open;p->open=0;pthread_mutex_unlock(&web_lock);
 if(opened){char event[160];snprintf(event,sizeof event,"{\"type\":\"webview/closed\",\"id\":\"%s\"}",id);web_emit(event);web_log(v,"destroy",id,1);}
 for(unsigned i=0;i<64;i++)if(web_listeners[i].vm==v&&web_listeners[i].container){dvm_ref nodes[128];int n=view_state_runtime_snapshot_tree(v,web_listeners[i].container,nodes,128),found=0;for(int k=0;k<n;k++)if(nodes[k]==web)found=1;if(found){if(web_listeners[i].listener)dvm_unpin(v,web_listeners[i].listener);dvm_unpin(v,web_listeners[i].container);memset(&web_listeners[i],0,sizeof web_listeners[i]);}}
 for(unsigned i=0;i<WEB_INTERFACES;i++)if(p->interfaces[i].object)dvm_unpin(v,p->interfaces[i].object);
 if(p->context)dvm_unpin(v,p->context);dvm_unpin(v,p->web);
 if(p->snapshot){wipe(p->snapshot,p->snapshot_bytes);free(p->snapshot);p->snapshot=NULL;p->snapshot_bytes=0;}
 pthread_mutex_lock(&web_lock);memset(p,0,sizeof *p);pthread_mutex_unlock(&web_lock);
}
static void web_listener_observe(struct dvm *v,struct dvm_method *m,dvm_ref self,const uint32_t *args,int count,int ok){
 if(!ok||dvm_exception(v)||!self||count!=1||!args||strncmp(m->name,"set",3)||!strstr(m->name,"CloseListener"))return;
 if(!webview_panel_gc_bind(v)){dvm__throw(v,"java/lang/OutOfMemoryError","Web listener root registration failed");return;}
 struct dvm_class *c=args[0]?dvm_object_class(v,args[0]):NULL;
 if(args[0]&&(!c||!dvm_find_method(v,c,"onClose","()V")))return;
 int slot=-1;for(unsigned i=0;i<64;i++)if(web_listeners[i].vm==v&&web_listeners[i].container==self){slot=(int)i;break;}
 if(slot<0&&args[0])for(unsigned i=0;i<64;i++)if(!web_listeners[i].container){slot=(int)i;break;}
 if(slot<0){if(!args[0])return;}else{
  if(args[0])dvm_pin(v,args[0]);if(web_listeners[slot].listener)dvm_unpin(v,web_listeners[slot].listener);
  if(!web_listeners[slot].container&&args[0])dvm_pin(v,self);
  if(!args[0]&&web_listeners[slot].container)dvm_unpin(v,web_listeners[slot].container);
  web_listeners[slot].vm=args[0]?v:NULL;web_listeners[slot].container=args[0]?self:0;web_listeners[slot].listener=args[0];return;
 }
 web_log(v,"close_listener_capacity",NULL,0);
}
__attribute__((visibility("default"))) int webview_panel_command(const char *id,const char *action,const char *payload){
 if(!id||!action||!payload||strlen(id)>=80||strlen(action)>=180||strlen(payload)>8192)return -2;
 int path_event=!strcmp(action,"path:top_close")||!strcmp(action,"path:bottom_close")||!strcmp(action,"path:force_close")||!strcmp(action,"path:system_back")||!strcmp(action,"path:web_touch")||!strcmp(action,"path:bridge_ready")||!strcmp(action,"path:bridge_missing")||!strcmp(action,"path:dom_agree")||!strcmp(action,"path:dom_disagree")||!strcmp(action,"path:read_bottom")||!strcmp(action,"path:read_pending")||!strcmp(action,"path:dom_click")||!strcmp(action,"path:type_error")||!strcmp(action,"path:page_error");
 if(!path_event&&strcmp(action,"sync")&&strcmp(action,"close")&&strcmp(action,"force_close")&&strcmp(action,"page_end")&&strcmp(action,"page_error")&&strncmp(action,"js:",3))return -2;
 if(path_event)web_log(NULL,action,id,1); /* Host arrival, even while GIL blocked. */
 /* Sync only enqueues a value command; snapshot access stays on the VM owner. */
 pthread_mutex_lock(&web_lock);int exists=!strcmp(action,"sync");for(unsigned i=0;i<WEB_SLOTS;i++)if(web_pages[i].open&&!strcmp(web_pages[i].id,id)){exists=1;break;}
 int rc=-1;if(exists&&web_count<WEB_QUEUE){unsigned at=(web_head+web_count)%WEB_QUEUE;strcpy(web_commands[at].id,id);strcpy(web_commands[at].action,action);strcpy(web_commands[at].payload,payload);web_count++;rc=0;}else if(exists)rc=-3;
 pthread_mutex_unlock(&web_lock);return rc;
}
static void webview_panel_poll(struct dvm *v){
 if(web_polling)return;web_polling=1;
 for(unsigned batch=0;batch<8;batch++){
  char id[80],action[180],payload[8193];pthread_mutex_lock(&web_lock);if(!web_count){pthread_mutex_unlock(&web_lock);break;}
  strcpy(id,web_commands[web_head].id);strcpy(action,web_commands[web_head].action);strcpy(payload,web_commands[web_head].payload);wipe(&web_commands[web_head],sizeof web_commands[0]);web_head=(web_head+1)%WEB_QUEUE;web_count--;pthread_mutex_unlock(&web_lock);
  if(!strcmp(action,"sync")){
   char ack[4096];struct web_json j={ack,0,sizeof ack,1};unsigned count=0;web_add(&j,"{\"type\":\"webview/synced\",\"pages\":[");
   unsigned order[WEB_SLOTS],total=0;for(unsigned i=0;i<WEB_SLOTS;i++)if(web_pages[i].vm==v&&web_pages[i].open&&web_pages[i].snapshot){unsigned at=total++;while(at&&web_pages[order[at-1]].revision>web_pages[i].revision){order[at]=order[at-1];at--;}order[at]=i;}
   for(unsigned k=0;k<total;k++){struct web_page *page=&web_pages[order[k]];web_emit(page->snapshot);web_add(&j,"%s{\"id\":",count++?",":"");web_string(&j,page->id);web_add(&j,",\"revision\":%u}",page->revision);}
   web_add(&j,"]}");if(j.ok)web_emit(ack);web_log(v,"sync",NULL,j.ok);wipe(payload,sizeof payload);continue;
  }
  struct web_page *p=NULL;for(unsigned i=0;i<WEB_SLOTS;i++)if(web_pages[i].vm==v&&web_pages[i].open&&!strcmp(web_pages[i].id,id)){p=&web_pages[i];break;}
  if(!p){wipe(payload,sizeof payload);continue;}dvm_ref web=p->web,context=p->context;
  dvm_ref saved=v->exception;bool parked=v->parked;uint64_t steps=v->call_steps;v->exception=0;v->parked=false;v->call_steps=0;int ok=0;
  if(!strncmp(action,"path:",5)){
   web_log(v,action,id,1); /* VM-owner delivery; no business action. */
  }else if(!strncmp(action,"js:",3)){
   char *name=action+3,*method=strchr(name,':');if(method){*method++=0;for(unsigned i=0;i<WEB_INTERFACES;i++)if(p->interfaces[i].object&&!strcmp(p->interfaces[i].name,name)){
    const char *sig=!strcmp(method,"handler")?"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V":!strcmp(method,"onJSAndDomReady")?"()V":"(Ljava/lang/String;)V";
    union dvm_value a[3]={{0}},q={0};struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,p->interfaces[i].object),method,sig);
    if(m&&(m->access&1)&&web_js_supported(method,sig)){
     int n=!strcmp(method,"handler")?3:strcmp(sig,"()V")?1:0,pinned=0;bool valid=true;
     if(n==3){
      dvm_ref object=dvm_new_object(v,dvm_find_class_or_external(v,"org/json/JSONObject"));dvm_pin(v,object);
      union dvm_value input={.l=dvm_new_string(v,payload)};
      valid=object&&json_runtime_call(v,"org/json/JSONObject","<init>","(Ljava/lang/String;)V",object,&input,1,&q)&&!v->exception;
      const char *keys[]={"name","data","callback"};
      for(int j=0;valid&&j<3;j++){input.l=dvm_new_string(v,keys[j]);valid=json_runtime_call(v,"org/json/JSONObject","getString","(Ljava/lang/String;)Ljava/lang/String;",object,&input,1,&a[j])&&!v->exception;if(valid){dvm_pin(v,a[j].l);pinned++;}}
      dvm_unpin(v,object);
     }else if(n)a[0].l=dvm_new_string(v,payload);
     if(valid)ok=dvm_call(v,m,p->interfaces[i].object,a,n,&q)&&!v->exception;
     for(int j=0;j<pinned;j++)dvm_unpin(v,a[j].l);
    }break;
   }}
   web_log(v,!strcmp(method?method:"","onJSAndDomReady")?"js_dom_ready":"js_dispatch",id,ok);
  }else if(!strcmp(action,"page_end")||!strcmp(action,"page_error")){
   extern bool android_shadow_web_event(struct dvm *,dvm_ref,int,const char *);ok=android_shadow_web_event(v,web,!strcmp(action,"page_end"),payload);web_log(v,action,id,ok);
  }else {
   int force=!strcmp(action,"force_close");dvm_ref listener=0;
   if(!force){dvm_ref ancestor=web;for(unsigned depth=0;ancestor&&!listener&&depth<128;depth++){for(unsigned i=0;i<64;i++)if(web_listeners[i].vm==v&&web_listeners[i].listener&&ancestor==web_listeners[i].container){listener=web_listeners[i].listener;break;}if(!listener){union dvm_value parent={0};if(!view_state_runtime_call(v,"android/view/View","getParent","()Landroid/view/ViewParent;",ancestor,NULL,0,&parent))break;ancestor=parent.l;}}}
   union dvm_value q={0};if(listener)ok=invoke(v,listener,"onClose","()V",NULL,0,&q);
   web_log(v,listener?"original_close_listener":"close_no_listener",id,ok);
   /* Escaping a failed/blocked page is unconditional; retain any original
    * callback failure in diagnostics before invoking Activity teardown. */
   if(v->exception){web_log(v,"close_exception",id,0);dvm_clear_exception(v);}
   extern bool activity_runtime_web_finish(struct dvm *,dvm_ref,int);
   int finished=activity_runtime_web_finish(v,context,force||!listener||!ok);
   web_log(v,force?"force_close":"close",id,finished);
   extern bool android_shadow_web_destroy(struct dvm *,dvm_ref);
   if(web_find(v,web,0))android_shadow_web_destroy(v,web);
  }
  if(v->exception)web_log(v,"dispatch_exception",id,0);
  dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;wipe(payload,sizeof payload);
 }
 /* A page's real JS callback can finish its Activity without a host close. */
 for(unsigned i=0;i<WEB_SLOTS;i++)if(web_pages[i].vm==v&&web_pages[i].open){extern bool activity_runtime_web_finished(struct dvm *,dvm_ref);extern bool activity_runtime_web_finish(struct dvm *,dvm_ref,int);if(activity_runtime_web_finished(v,web_pages[i].context)){dvm_ref web=web_pages[i].web;dvm_ref saved=v->exception;uint64_t steps=v->call_steps;bool parked=v->parked;v->exception=0;v->call_steps=0;v->parked=false;activity_runtime_web_finish(v,web_pages[i].context,0);if(web_find(v,web,0)){extern bool android_shadow_web_destroy(struct dvm *,dvm_ref);android_shadow_web_destroy(v,web);}dvm_clear_exception(v);v->exception=saved;v->call_steps=steps;v->parked=parked;}}
 /* An explicit development command exercises the real VM/ArkWeb APIs on an
  * already-loaded agreement page. Never run destructive Cookie checks merely
  * because an app or WebView started. The task explicitly authorizes clearing. */
 static uint64_t cookie_control_last;
 uint64_t cookie_now=monotonic_ms();
 if(cookie_now-cookie_control_last>=250){
  cookie_control_last=cookie_now;
  const char *path="/data/storage/el2/base/haps/entry/files/sdkdex/web-cookie-control.txt";
  FILE *control=fopen(path,"r");
  if(control){char command[64]={0};int n=fscanf(control,"%63s",command);fclose(control);unlink(path);
   if(n==1&&!strcmp(command,"SELFTEST"))for(unsigned i=0;i<WEB_SLOTS;i++)if(web_pages[i].vm==v&&web_pages[i].open){
    dvm_ref saved=v->exception;uint64_t steps=v->call_steps;bool parked=v->parked;v->exception=0;v->call_steps=0;v->parked=false;
    union dvm_value url={0};unsigned checks=0;
    int ok=invoke(v,web_pages[i].web,"getUrl","()Ljava/lang/String;",NULL,0,&url)&&!v->exception;
    const char *address=ok?dvm_string_utf8(v,url.l):NULL;
    ok=address&&android_web_selftest(v,web_pages[i].web,address,&checks)&&!v->exception;
    FILE *log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/web-cookie-events.txt","a");
    if(log){fprintf(log,"WEB_COOKIE_SELFTEST_RESULT pid=%d ok=%d checks=%u exception_class=%s\n",getpid(),ok,checks,v->exception&&dvm_object_class(v,v->exception)?dvm_object_class(v,v->exception)->name:"none");fclose(log);}
    dvm_clear_exception(v);v->exception=saved;v->call_steps=steps;v->parked=parked;break;
   }
  }
 }
 web_polling=0;
}
static void webview_panel_unbind(struct dvm *v){
 for(unsigned i=0;i<WEB_SLOTS;i++)if(web_pages[i].vm==v)webview_panel_destroy(v,web_pages[i].web);
 for(unsigned i=0;i<64;i++)if(web_listeners[i].vm==v){if(web_listeners[i].container)dvm_unpin(v,web_listeners[i].container);if(web_listeners[i].listener)dvm_unpin(v,web_listeners[i].listener);memset(&web_listeners[i],0,sizeof web_listeners[i]);}
 pthread_mutex_lock(&web_lock);wipe(web_commands,sizeof web_commands);web_head=web_count=0;pthread_mutex_unlock(&web_lock);
 if(web_gc_vm==v){dvm_gc_root_provider_remove(v,"webview-panel/strong",&web_gc_vm);web_gc_vm=NULL;}
}
