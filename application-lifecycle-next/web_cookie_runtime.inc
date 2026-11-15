/* Included after shadow_settings and the VM-owner helpers. All cookie/UA
 * state belongs to the real host backend; only guest identity/callback roots
 * live here. No cookie values or UA strings are logged. */
static android_web_host_fn android_web_host;
static struct {
 struct dvm *vm;dvm_ref manager,sync,inflight;
 struct {dvm_ref callback;int value;} *pending;
 size_t count,capacity;int polling;
} android_web_state;
static bool android_web_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user){
 if(android_web_state.vm!=v)return true;
 /* Backend singletons and queued callbacks are host-owned global roots;
  * per-WebView settings remain owner edges in the WebView payload. */
 if(android_web_state.manager)visit(&android_web_state.manager,user);
 if(android_web_state.sync)visit(&android_web_state.sync,user);
 if(android_web_state.inflight)visit(&android_web_state.inflight,user);
 if(android_web_state.count>android_web_state.capacity || (android_web_state.count&&!android_web_state.pending))return false;
 for(size_t i=0;i<android_web_state.count;i++)if(android_web_state.pending[i].callback)visit(&android_web_state.pending[i].callback,user);
 return true;
}
static void android_web_log(int op,int status,size_t count,size_t length,int check){
 sdk_trace_fprintf(stderr,"ANDROID_WEB pid=%ld thread=%ld op=%d status=%d count=%zu length=%zu check=%d\n",(long)getpid(),(long)gettid(),op,status,count,length,check);
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/web-cookie-events.txt","a");
 if(f){fprintf(f,"pid=%ld thread=%ld op=%d status=%d count=%zu length=%zu check=%d\n",(long)getpid(),(long)gettid(),op,status,count,length,check);fclose(f);}
}
void android_web_set_host(android_web_host_fn fn){android_web_host=fn;}
static bool android_web_host_call(struct dvm *v,int op,dvm_ref web,const char *a,const char *b,char *out,size_t cap){
 if(out&&cap)out[0]=0;
 int rc=android_web_host?android_web_host(op,web,a,b,out,cap):-2;
 if(rc==1&&out&&cap&&!memchr(out,0,cap))rc=-1;
 if(out&&cap)out[cap-1]=0;
 android_web_log(op,rc,0,out?strlen(out):0u,0);
 if(rc==1)return true;
 char message[128];snprintf(message,sizeof message,"ArkWeb backend operation %d failed rc=%d",op,rc);
 dvm__throw(v,rc==-2?"java/lang/UnsupportedOperationException":"java/lang/IllegalStateException",message);return false;
}
bool android_web_release(struct dvm *v,dvm_ref web){char result[16];return android_web_host_call(v,ANDROID_WEB_RELEASE_UA,web,NULL,NULL,result,sizeof result);}
static bool android_web_bool_result(struct dvm *v,const char *s,int *value){
 if(!strcmp(s,"1")){*value=1;return true;}if(!strcmp(s,"0")){*value=0;return true;}
 dvm__throw(v,"java/lang/IllegalStateException","ArkWeb backend invalid Boolean result");return false;
}
static bool android_web_bind(struct dvm *v){
 if(!android_basics_gc_ensure(v))return false;
 if(android_web_state.vm==v)return true;
 if(android_web_state.vm){dvm__throw(v,"java/lang/IllegalStateException","ArkWeb runtime belongs to another VM; unbind required");return false;}
 android_web_state.vm=v;return true;
}
void android_web_unbind(struct dvm *v){
 if(android_web_state.vm!=v)return;
 for(size_t i=0;i<android_web_state.count;i++)dvm_unpin(v,android_web_state.pending[i].callback);
 if(android_web_state.manager)dvm_unpin(v,android_web_state.manager);
 if(android_web_state.sync)dvm_unpin(v,android_web_state.sync);
 free(android_web_state.pending);memset(&android_web_state,0,sizeof android_web_state);
}
static bool android_web_callback_reserve(struct dvm *v,dvm_ref callback){
 if(!callback)return true;
 if(!owner(v)){dvm__throw(v,"java/lang/UnsupportedOperationException","Cookie ValueCallback requires supported VM main Looper");return false;}
 if(android_web_state.count<android_web_state.capacity)return true;
 size_t cap=android_web_state.capacity?android_web_state.capacity*2:16;
 if(cap>SIZE_MAX/sizeof *android_web_state.pending){dvm__throw(v,"java/lang/OutOfMemoryError","Cookie ValueCallback queue size");return false;}
 void *p=realloc(android_web_state.pending,cap*sizeof *android_web_state.pending);
 if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","Cookie ValueCallback queue");return false;}
 android_web_state.pending=p;android_web_state.capacity=cap;return true;
}
static void android_web_callback_enqueue(struct dvm *v,dvm_ref callback,int value){
 if(!callback)return;
 dvm_pin(v,callback);size_t i=android_web_state.count++;
 android_web_state.pending[i].callback=callback;android_web_state.pending[i].value=value;
}
void android_web_poll(struct dvm *v){
 if(android_web_state.vm!=v||android_web_state.polling||!owner(v)||dvm_exception(v))return;
 android_web_state.polling=1;size_t budget=android_web_state.count;
 while(budget--&&android_web_state.count&&!dvm_exception(v)){
  dvm_ref cb=android_web_state.pending[0].callback;android_web_state.inflight=cb;int value=android_web_state.pending[0].value;
  memmove(android_web_state.pending,android_web_state.pending+1,(--android_web_state.count)*sizeof *android_web_state.pending);
  struct dvm_method *box=dvm_find_method(v,dvm_find_class_or_external(v,"java/lang/Boolean"),"valueOf","(Z)Ljava/lang/Boolean;");
  struct dvm_method *method=dvm_find_method(v,dvm_object_class(v,cb),"onReceiveValue","(Ljava/lang/Object;)V");
  union dvm_value a={.i=value},boxed={0},ignored={0};
  if(!method&&!dvm_exception(v))dvm__throw(v,"java/lang/AbstractMethodError","ValueCallback.onReceiveValue(Object)");
  if(!box&&!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","Boolean.valueOf(boolean)");
  if(box&&method&&!dvm_exception(v)&&dvm_call(v,box,0,&a,1,&boxed))dvm_call(v,method,android_web_state.inflight,&boxed,1,&ignored);
  dvm_unpin(v,android_web_state.inflight);android_web_state.inflight=0;
 }
 android_web_state.polling=0;
}
static bool android_web_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 bool cookie=!strcmp(c,"android/webkit/CookieManager"),sync=!strcmp(c,"android/webkit/CookieSyncManager"),settings=!strcmp(c,"android/webkit/WebSettings");
 if(!cookie&&!sync&&!settings)return false;
#define WEB_IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
 if(settings){
  if(WEB_IS("getDefaultUserAgent","(Landroid/content/Context;)Ljava/lang/String;")){
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","WebSettings Context");return true;}
   char *s=calloc(1,65536);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","UA host output");return true;}
   if(android_web_host_call(v,ANDROID_WEB_DEFAULT_UA,0,NULL,NULL,s,65536))out->l=dvm_new_string(v,s);free(s);return true;
  }
  if(!WEB_IS("setUserAgentString","(Ljava/lang/String;)V")&&!WEB_IS("getUserAgentString","()Ljava/lang/String;"))return false;
  struct dvm_object *o=dvm__obj(v,self);struct shadow_settings *s=o&&o->data_destroy==shadow_settings_free?o->data:NULL;
  if(!s||!s->owner){dvm__throw(v,"java/lang/IllegalStateException","WebSettings has no owning WebView");return true;}
  struct dvm_object *wo=dvm__obj(v,s->owner);struct task5_component *component=wo&&wo->data_destroy==task5_component_free?wo->data:NULL;
  if(!component||component->destroyed){dvm__throw(v,"java/lang/IllegalStateException","WebSettings owning WebView destroyed");return true;}
  if(n){char result[16];const char *ua=a[0].l?dvm_string_utf8(v,a[0].l):NULL;if(a[0].l&&!ua)return true;android_web_host_call(v,ANDROID_WEB_SET_UA,s->owner,ua,NULL,result,sizeof result);return true;}
  char *ua=calloc(1,65536);if(!ua){dvm__throw(v,"java/lang/OutOfMemoryError","UA host output");return true;}
  if(android_web_host_call(v,ANDROID_WEB_GET_UA,s->owner,NULL,NULL,ua,65536))out->l=dvm_new_string(v,ua);free(ua);return true;
 }
 if(!android_web_bind(v))return true;
 if(WEB_IS("getInstance",cookie?"()Landroid/webkit/CookieManager;":"()Landroid/webkit/CookieSyncManager;")||(sync&&WEB_IS("createInstance","(Landroid/content/Context;)Landroid/webkit/CookieSyncManager;"))){
  if(sync&&n&&!a[0].l){dvm__throw(v,"java/lang/NullPointerException","CookieSyncManager Context");return true;}
  dvm_ref *slot=cookie?&android_web_state.manager:&android_web_state.sync;
  if(sync&&!n&&!*slot){dvm__throw(v,"java/lang/IllegalStateException","CookieSyncManager.createInstance must be called first");return true;}
  if(!*slot){*slot=dvm_new_object(v,dvm_find_class_or_external(v,c));if(*slot)dvm_pin(v,*slot);}out->l=*slot;return true;
 }
 if(!self){dvm__throw(v,"java/lang/NullPointerException","Cookie runtime receiver");return true;}
 if(sync){
  if(WEB_IS("sync","()V")){char result[16];android_web_host_call(v,ANDROID_WEB_FLUSH,0,NULL,NULL,result,sizeof result);return true;}
  if(WEB_IS("startSync","()V")||WEB_IS("stopSync","()V")||WEB_IS("resetSync","()V")){dvm__throw(v,"java/lang/UnsupportedOperationException","CookieSyncManager periodic timer not provided; use sync/flush");return true;}
  return false;
 }
 if(WEB_IS("setCookie","(Ljava/lang/String;Ljava/lang/String;)V")||WEB_IS("setCookie","(Ljava/lang/String;Ljava/lang/String;Landroid/webkit/ValueCallback;)V")){
  if(!a[0].l||!a[1].l){dvm__throw(v,"java/lang/NullPointerException","Cookie URL/value");return true;}
  dvm_ref cb=n==3?a[2].l:0;if(!android_web_callback_reserve(v,cb))return true;
  const char *url=dvm_string_utf8(v,a[0].l),*value=dvm_string_utf8(v,a[1].l);if(!url||!value)return true;
  char result[16];int changed=0;
  if(android_web_host_call(v,ANDROID_WEB_SET_COOKIE,0,url,value,result,sizeof result)&&android_web_bool_result(v,result,&changed))android_web_callback_enqueue(v,cb,changed);return true;
 }
 if(WEB_IS("getCookie","(Ljava/lang/String;)Ljava/lang/String;")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Cookie URL");return true;}
  const char *url=dvm_string_utf8(v,a[0].l);if(!url)return true;
  char *result=calloc(1,65536);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","Cookie host output");return true;}
  if(android_web_host_call(v,ANDROID_WEB_GET_COOKIE,0,url,NULL,result,65536))out->l=*result?dvm_new_string(v,result):0;free(result);return true;
 }
 int op=0;dvm_ref cb=0;bool result_bool=false;
 if(WEB_IS("removeAllCookies","(Landroid/webkit/ValueCallback;)V")||WEB_IS("removeAllCookie","()V")){op=ANDROID_WEB_REMOVE_ALL;cb=n?a[0].l:0;}
 else if(WEB_IS("removeSessionCookies","(Landroid/webkit/ValueCallback;)V")||WEB_IS("removeSessionCookie","()V")){op=ANDROID_WEB_REMOVE_SESSION;cb=n?a[0].l:0;}
 else if(WEB_IS("flush","()V"))op=ANDROID_WEB_FLUSH;
 else if(WEB_IS("setAcceptCookie","(Z)V"))op=ANDROID_WEB_SET_ACCEPT;
 else if(WEB_IS("acceptCookie","()Z")){op=ANDROID_WEB_GET_ACCEPT;result_bool=true;}
 else if(WEB_IS("hasCookies","()Z")){op=ANDROID_WEB_HAS_COOKIES;result_bool=true;}
 else return false;
 if(!android_web_callback_reserve(v,cb))return true;
 char result[16];if(android_web_host_call(v,op,0,op==ANDROID_WEB_SET_ACCEPT?(a[0].i?"1":"0"):NULL,NULL,result,sizeof result)){
  if(result_bool||op==ANDROID_WEB_REMOVE_ALL||op==ANDROID_WEB_REMOVE_SESSION){int value;if(android_web_bool_result(v,result,&value)){if(result_bool)out->i=value;android_web_callback_enqueue(v,cb,value);}}
 }
 return true;
#undef WEB_IS
}
/* Explicit, user-authorized diagnostic only: never invoked at VM start. The
 * caller authorizes global jar clearing; natural cookies are measured only. */
static struct {struct dvm *vm;unsigned calls;int last;} android_web_test_callback;
static bool android_web_test_receive(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;memset(out,0,sizeof *out);
 if(android_web_test_callback.vm!=v||n!=1||!a[0].l){dvm__throw(v,"java/lang/IllegalStateException","Cookie diagnostic callback state");return false;}
 struct dvm_method *unbox=dvm_find_method(v,dvm_object_class(v,a[0].l),"booleanValue","()Z");union dvm_value value={0};
 if(!unbox||!dvm_call(v,unbox,a[0].l,NULL,0,&value))return false;
 android_web_test_callback.calls++;android_web_test_callback.last=value.i!=0;return true;
}
static bool android_web_test_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_class *cl=self?dvm_object_class(v,self):dvm_find_class_or_external(v,c);
 struct dvm_method *method=cl?dvm_find_method(v,cl,m,sig):NULL;
 if(!method){if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","Cookie diagnostic method resolution");return false;}
 memset(out,0,sizeof *out);return dvm_call(v,method,self,a,n,out)&&!dvm_exception(v);
}
bool android_web_selftest(struct dvm *v,dvm_ref web,const char *url,unsigned *checks){
 unsigned local=0;if(!checks)checks=&local;
 if(!v||!web||!url||!owner(v)||dvm_exception(v))return false;
 bool acquired=!dvm_gil_held(v);if(acquired)dvm_gil_acquire(v);
 bool ok=true,changed_accept=false,have_ua=false;int initial_accept=1,jar_nonempty=1;
 dvm_ref oldua=0,settings=0,manager=0,callback=0;
 union dvm_value q={0},args[3]={{0},{0},{0}};
#define WEB_TEST(expr) do{++*checks;int pass=(expr)!=0;android_web_log(0,pass?1:-1,0,0,(int)*checks);if(!pass){ok=false;goto web_test_cleanup;}}while(0)
 WEB_TEST(android_web_test_call(v,"android/webkit/CookieManager","getInstance","()Landroid/webkit/CookieManager;",0,NULL,0,&q)&&q.l);manager=q.l;
 WEB_TEST(android_web_test_call(v,NULL,"acceptCookie","()Z",manager,NULL,0,&q));initial_accept=q.i!=0;
 WEB_TEST(android_web_test_call(v,NULL,"hasCookies","()Z",manager,NULL,0,&q));jar_nonempty=q.i!=0;
 android_web_log(ANDROID_WEB_HAS_COOKIES,1,jar_nonempty?1:0,0,0);
 args[0].l=dvm_new_string(v,url);WEB_TEST(android_web_test_call(v,NULL,"getCookie","(Ljava/lang/String;)Ljava/lang/String;",manager,args,1,&q));
 {const char *natural=q.l?dvm_string_utf8(v,q.l):NULL;size_t count=natural&&*natural?1:0;if(natural)for(const char *p=natural;*p;p++)if(*p==';')count++;android_web_log(ANDROID_WEB_GET_COOKIE,1,count,natural?strlen(natural):0,0);}
 args[0].i=!initial_accept;WEB_TEST(android_web_test_call(v,NULL,"setAcceptCookie","(Z)V",manager,args,1,&q));changed_accept=true;
 WEB_TEST(android_web_test_call(v,NULL,"acceptCookie","()Z",manager,NULL,0,&q)&&q.i==!initial_accept);
 args[0].i=1;WEB_TEST(android_web_test_call(v,NULL,"setAcceptCookie","(Z)V",manager,args,1,&q));
 WEB_TEST(android_web_test_call(v,NULL,"getSettings","()Landroid/webkit/WebSettings;",web,NULL,0,&q)&&q.l);settings=q.l;
 WEB_TEST(android_web_test_call(v,NULL,"getUserAgentString","()Ljava/lang/String;",settings,NULL,0,&q)&&q.l);oldua=q.l;dvm_pin(v,oldua);have_ua=true;
 args[0].l=android_basics_application(v);WEB_TEST(android_web_test_call(v,"android/webkit/WebSettings","getDefaultUserAgent","(Landroid/content/Context;)Ljava/lang/String;",0,args,1,&q)&&q.l);
 dvm_ref defaultua=q.l;dvm_pin(v,defaultua);
 args[0].l=dvm_new_string(v,"HostCookieDiagnostic/1.0");bool setua=android_web_test_call(v,NULL,"setUserAgentString","(Ljava/lang/String;)V",settings,args,1,&q);
 if(!setua){dvm_unpin(v,defaultua);WEB_TEST(false);}
 bool readua=android_web_test_call(v,NULL,"getUserAgentString","()Ljava/lang/String;",settings,NULL,0,&q)&&q.l&&!strcmp(dvm_string_utf8(v,q.l),"HostCookieDiagnostic/1.0");
 if(!readua){dvm_unpin(v,defaultua);WEB_TEST(false);}WEB_TEST(readua);
 args[0].l=0;bool reset=android_web_test_call(v,NULL,"setUserAgentString","(Ljava/lang/String;)V",settings,args,1,&q)&&android_web_test_call(v,NULL,"getUserAgentString","()Ljava/lang/String;",settings,NULL,0,&q)&&q.l&&!strcmp(dvm_string_utf8(v,q.l),dvm_string_utf8(v,defaultua));
 dvm_unpin(v,defaultua);WEB_TEST(reset);
 /* The parent exposes this explicit operation only for the user-authorized
  * cookie-clear acceptance. Initial natural state was recorded above. */
 args[0].l=0;WEB_TEST(android_web_test_call(v,NULL,"removeAllCookies","(Landroid/webkit/ValueCallback;)V",manager,args,1,&q));
 WEB_TEST(android_web_test_call(v,NULL,"hasCookies","()Z",manager,NULL,0,&q)&&!q.i);
 args[0].l=dvm_new_string(v,url);WEB_TEST(android_web_test_call(v,NULL,"getCookie","(Ljava/lang/String;)Ljava/lang/String;",manager,args,1,&q)&&!q.l);
 struct dvm_class *probe=dvm__register_builtin(v,"Lsdktrace/tests/WebCookieValueCallback;");
 WEB_TEST(probe!=NULL);
 if(!probe->methods){probe->methods=calloc(1,sizeof *probe->methods);if(probe->methods){probe->nmethods=1;probe->methods[0].cls=probe;probe->methods[0].name="onReceiveValue";probe->methods[0].sig=strdup("(Ljava/lang/Object;)V");probe->methods[0].arg_count=probe->methods[0].arg_slots=1;probe->methods[0].ret_kind='V';probe->methods[0].builtin=android_web_test_receive;}}
 WEB_TEST(probe->methods&&probe->methods[0].sig);
 callback=dvm_new_object(v,probe);WEB_TEST(callback!=0);dvm_pin(v,callback);
 android_web_test_callback.vm=v;android_web_test_callback.calls=0;
 args[0].l=dvm_new_string(v,url);args[1].l=dvm_new_string(v,"__hmos_diagnostic_session=1; Path=/; Secure; SameSite=Lax");args[2].l=callback;
 WEB_TEST(android_web_test_call(v,NULL,"setCookie","(Ljava/lang/String;Ljava/lang/String;Landroid/webkit/ValueCallback;)V",manager,args,3,&q));
 WEB_TEST(android_web_test_callback.calls==0);android_web_poll(v);WEB_TEST(!dvm_exception(v)&&android_web_test_callback.calls==1&&android_web_test_callback.last);
 args[0].l=dvm_new_string(v,url);args[1].l=dvm_new_string(v,"__hmos_diagnostic_persistent=1; Path=/; Secure; Max-Age=600; SameSite=Lax");
 WEB_TEST(android_web_test_call(v,NULL,"setCookie","(Ljava/lang/String;Ljava/lang/String;)V",manager,args,2,&q));
 args[0].l=dvm_new_string(v,url);WEB_TEST(android_web_test_call(v,NULL,"getCookie","(Ljava/lang/String;)Ljava/lang/String;",manager,args,1,&q)&&q.l);
 const char *both=dvm_string_utf8(v,q.l);WEB_TEST(both&&strstr(both,"__hmos_diagnostic_session=1")&&strstr(both,"__hmos_diagnostic_persistent=1"));
 {char page[16];int shared=0;WEB_TEST(android_web_host_call(v,ANDROID_WEB_PAGE_PROBE,web,url,NULL,page,sizeof page)&&android_web_bool_result(v,page,&shared)&&shared);}
 args[0].l=callback;WEB_TEST(android_web_test_call(v,NULL,"removeSessionCookies","(Landroid/webkit/ValueCallback;)V",manager,args,1,&q));
 android_web_poll(v);WEB_TEST(!dvm_exception(v)&&android_web_test_callback.calls==2&&android_web_test_callback.last);
 args[0].l=dvm_new_string(v,url);WEB_TEST(android_web_test_call(v,NULL,"getCookie","(Ljava/lang/String;)Ljava/lang/String;",manager,args,1,&q)&&q.l);
 const char *persistent=dvm_string_utf8(v,q.l);WEB_TEST(persistent&&!strstr(persistent,"__hmos_diagnostic_session=1")&&strstr(persistent,"__hmos_diagnostic_persistent=1"));
 WEB_TEST(android_web_test_call(v,NULL,"flush","()V",manager,NULL,0,&q));
 args[0].l=callback;WEB_TEST(android_web_test_call(v,NULL,"removeAllCookies","(Landroid/webkit/ValueCallback;)V",manager,args,1,&q));
 android_web_poll(v);WEB_TEST(!dvm_exception(v)&&android_web_test_callback.calls==3&&android_web_test_callback.last);
 WEB_TEST(android_web_test_call(v,NULL,"hasCookies","()Z",manager,NULL,0,&q)&&!q.i);
 args[0].l=dvm_new_string(v,url);WEB_TEST(android_web_test_call(v,NULL,"getCookie","(Ljava/lang/String;)Ljava/lang/String;",manager,args,1,&q)&&!q.l);
 args[0].l=0;WEB_TEST(android_web_test_call(v,NULL,"removeAllCookies","(Landroid/webkit/ValueCallback;)V",manager,args,1,&q));
web_test_cleanup:
 /* Preserve original exception while restoring the two explicitly modified
  * host settings. Never use a failed assertion as a reason to clear cookies. */
 {dvm_ref exception=dvm_exception(v);bool parked=v->parked;if(exception)dvm_pin(v,exception);dvm_clear_exception(v);
  if(have_ua){args[0].l=oldua;if(!android_web_test_call(v,NULL,"setUserAgentString","(Ljava/lang/String;)V",settings,args,1,&q))ok=false;dvm_unpin(v,oldua);}
  if(changed_accept){dvm_clear_exception(v);args[0].i=initial_accept;if(!android_web_test_call(v,NULL,"setAcceptCookie","(Z)V",manager,args,1,&q))ok=false;}
  if(exception){dvm_clear_exception(v);v->exception=exception;v->parked=parked;dvm_unpin(v,exception);}
 }
 if(callback&&android_web_state.vm==v){size_t keep=0;for(size_t i=0;i<android_web_state.count;i++){if(android_web_state.pending[i].callback==callback)dvm_unpin(v,callback);else android_web_state.pending[keep++]=android_web_state.pending[i];}android_web_state.count=keep;}
 if(callback)dvm_unpin(v,callback);android_web_test_callback.vm=NULL;
 android_web_log(0,ok?1:-1,*checks,0,0);if(acquired)dvm_gil_release(v);return ok;
#undef WEB_TEST
}
