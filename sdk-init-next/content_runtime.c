#include "content_runtime.h"
#include "gc_runtime.h"
#include "uri_runtime.h"
#include "dvm/dvm_internal.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

/* Context.getContentResolver() hands out one unpinned object per VM.  The
 * settings tables are C strings, not VM references, so they need no pinning
 * either.  A host VM switch drops everything: the old handles belong to a VM
 * that no longer exists.
 *
 * When the NAPI shell installs the @ohos.settings bridge, reads consult the
 * real host.  A key the host reports absent (or the absence of any host)
 * yields the caller's default, exactly like an unstored Android setting.  A
 * host failure is fail-closed: it surfaces as SecurityException (permission)
 * or IllegalStateException (backend) and is never converted into "key absent"
 * or a fabricated value.  The in-memory table is a fixture store that only the
 * selftest/explicit fixture mode may use; production execution never sees it.
 * Global retains that original host/fixture policy. Secure/System production
 * calls use the explicit local compatibility policy in settings_compat.inc:
 * bounded VM-local writes, defaults for unmapped keys and no invented IDs.
 * For the original Global path, a host write is authoritative: a rejected write returns false instead of
 * being mirrored into the table, so the guest is never told a system setting
 * changed when it did not.  Without a host, put* returns false and traces the
 * unsupported write instead of pretending it succeeded. */
#define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))
enum { SETTINGS_GLOBAL,SETTINGS_SYSTEM,SETTINGS_SECURE,SETTINGS_TABLE_COUNT };
typedef struct settings_entry {
 char *key;
 char *value;
 struct settings_entry *next;
} settings_entry;
static struct dvm *active;
static dvm_ref content_resolver;
static dvm_ref location_manager;
static unsigned compat_noted[3];
/* ContentObserver registrations are real in-process records.  There is no
 * host content-change source yet, so no callback is ever fabricated; the
 * observer's Handler is retained for a future real notification backend. */
#define CONTENT_OBSERVER_CAP 32
typedef struct {
 dvm_ref observer;
 dvm_ref uri;
 bool descendants;
} content_observation;
static content_observation observations[CONTENT_OBSERVER_CAP];
static int observation_count;
static bool content_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 if(!v||!visit||context!=&active||active!=v||!dvm_gil_held(v))return false;
 /* The resolver singleton and registered observations are host-owned;
  * an observer's Handler remains an owner edge, not an independent root. */
 if(content_resolver)visit(&content_resolver,user);
 if(location_manager)visit(&location_manager,user);
 if(observation_count<0||observation_count>CONTENT_OBSERVER_CAP)return false;
 for(int i=0;i<observation_count;i++){
  if(observations[i].observer)visit(&observations[i].observer,user);
  if(observations[i].uri)visit(&observations[i].uri,user);
 }
 return true;
}
static bool content_gc_ensure(struct dvm *v){
 if(dvm_gc_root_provider_add(v,"content-runtime/host",DVM_GC_ROOT_STRONG,content_gc_roots,&active))return true;
 if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Content host GC roots");return false;
}
void content_runtime_gc_vm_end(struct dvm *v){
 dvm_gc_root_provider_remove(v,"content-runtime/host",&active);
 if(active==v){content_resolver=location_manager=0;memset(observations,0,sizeof observations);observation_count=0;active=NULL;}
}
typedef struct {dvm_ref handler;} content_observer_state;
static void observer_state_drop(void *pointer){free(pointer);}
static settings_entry *settings[SETTINGS_TABLE_COUNT];
static bool airplane_probe_printed;
static const content_settings_host *host_api;
static void *host_user;
static bool host_active_reported,host_fallback_reported;
/* Off by default.  Only content_runtime_selftest() or an explicit fixture
 * harness turns this on; production execution never reads or writes the
 * in-memory table. */
static bool fixture_mode;
static const char *entry_value(int table,const char *key);
static const char *domain_name(int table){
 if(table==SETTINGS_GLOBAL)return "global";
 if(table==SETTINGS_SYSTEM)return "system";
 if(table==SETTINGS_SECURE)return "secure";
 return "unknown";
}
static void settings_values_clear(void){
 for(int i=0;i<SETTINGS_TABLE_COUNT;++i){
  settings_entry *e=settings[i];
  while(e){settings_entry *next=e->next;free(e->key);free(e->value);free(e);e=next;}
  settings[i]=NULL;
 }
}
static void settings_clear(void){
 settings_values_clear();
 content_resolver=0;airplane_probe_printed=false;
 location_manager=0;memset(compat_noted,0,sizeof compat_noted);
 memset(observations,0,sizeof observations);observation_count=0;
 host_active_reported=false;host_fallback_reported=false;
}
void content_runtime_set_host(const content_settings_host *api,void *user){
 host_api=api;host_user=user;
 host_active_reported=false;host_fallback_reported=false;
}
/* Explicit fixture mode.  Enabling starts from a clean table so one test
 * cannot inherit another test's values; disabling only gates the table off so
 * the selftest can prove production reads ignore it. */
void content_runtime_set_fixture_mode(bool enabled){
 if(enabled)settings_values_clear();
 if(enabled!=fixture_mode)
  sdk_trace_fprintf(stderr,"SETTINGS_FIXTURE_MODE enabled=%d\n",enabled?1:0);
 fixture_mode=enabled;
}
__attribute__((visibility("default"))) void sdk_set_settings_host(const content_settings_host *api,void *user){
 content_runtime_set_host(api,user);
}
void content_runtime_vm_start(struct dvm *v){settings_clear();fixture_mode=false;active=v;}
static void activate(struct dvm *v){if(v!=active){settings_clear();active=v;}}
static void host_note_active(void){
 if(host_api&&!host_active_reported){
  host_active_reported=true;
  sdk_trace_fprintf(stderr,"SETTINGS_HOST backend=ohos.settings available=1\n");
 }
}
enum { SETTING_READ_ABSENT=0, SETTING_READ_HOST=1, SETTING_READ_MEMORY=2 };
/* Returns a SETTING_READ_* value, or the negative host status when the host
 * reported a failure.  The caller owns host_buffer.  Only an explicit fixture
 * session may consult the in-memory table, and only when the host has no
 * value.  A host failure never falls back: it must not be confused with an
 * absent key. */
static int setting_read(int table,const char *key,char *host_buffer,size_t capacity,const char **value){
 const char *memory=fixture_mode?entry_value(table,key):NULL;
 *value=NULL;
 if(host_api&&host_api->get){
  host_note_active();
  int status=host_api->get(host_user,table,key,host_buffer,capacity);
  if(status==1){
   if(capacity)host_buffer[capacity-1]=0;
   *value=host_buffer;return SETTING_READ_HOST;
  }
  if(status!=0)return status;
 }
 if(memory){
  if(host_api&&host_api->get&&!host_fallback_reported){
   host_fallback_reported=true;
   sdk_trace_fprintf(stderr,"SETTINGS_HOST_FALLBACK domain=%s key=%s source=fixture_memory\n",
    domain_name(table),key);
  }
  *value=memory;return SETTING_READ_MEMORY;
 }
 return SETTING_READ_ABSENT;
}
/* A host failure is not an absent key.  Keep reads fail-closed with the
 * closest Android semantics and record the decision in the trace. */
static bool setting_host_failure(struct dvm *v,int status,int table,const char *key,const char *op){
 sdk_trace_fprintf(stderr,"SETTINGS_HOST_ERROR op=%s domain=%s key=%s status=%d kind=%s fail_closed=1\n",
  op,domain_name(table),key,status,status==-2?"permission":"backend");
 if(status==-2)dvm__throw(v,"java/lang/SecurityException",
  "Settings %s permission denied: %s/%s",op,domain_name(table),key);
 else dvm__throw(v,"java/lang/IllegalStateException",
  "Settings %s host unavailable: %s/%s",op,domain_name(table),key);
 return true;
}
/* The first airplane-mode answer states where it came from.  The absent case
 * is the caller's default, which is not a system state and is never printed as
 * one; a host failure is reported as fail-closed. */
static void airplane_note(int table,const char *key,int source,const char *value){
 if(table!=SETTINGS_GLOBAL||strcmp(key,"airplane_mode_on")||airplane_probe_printed)return;
 airplane_probe_printed=true;
 if(source==SETTING_READ_HOST)
  sdk_trace_fprintf(stderr,"SETTINGS_PROBE airplane_mode_on=%.64s source=ohos.settings\n",value?value:"");
 else if(source==SETTING_READ_MEMORY)
  sdk_trace_fprintf(stderr,"SETTINGS_PROBE airplane_mode_on=%.64s source=fixture_memory\n",value?value:"");
 else if(source<0)
  sdk_trace_fprintf(stderr,"SETTINGS_PROBE airplane_mode_on source=host_error fail_closed=1\n");
 else
  sdk_trace_fprintf(stderr,"SETTINGS_PROBE airplane_mode_on source=caller_default\n");
}
static char *text_copy(struct dvm *v,const char *s){
 size_t n=strlen(s);char *copy=malloc(n+1);
 if(!copy){dvm__throw(v,"java/lang/OutOfMemoryError","Settings storage");return NULL;}
 memcpy(copy,s,n+1);return copy;
}
static settings_entry *entry_find(int table,const char *key){
 for(settings_entry *e=settings[table];e;e=e->next)if(!strcmp(e->key,key))return e;
 return NULL;
}
static const char *entry_value(int table,const char *key){
 settings_entry *e=entry_find(table,key);return e?e->value:NULL;
}
/* A null value is Android's delete spelling: putString(cr,name,null) removes
 * the key instead of storing the string "null". */
static bool entry_set(struct dvm *v,int table,const char *key,const char *value){
 settings_entry *e=entry_find(table,key);
 if(!value){
  if(!e)return true;
  settings_entry **link=&settings[table];
  while(*link!=e)link=&(*link)->next;
  *link=e->next;free(e->key);free(e->value);free(e);return true;
 }
 if(e){
  char *copy=text_copy(v,value);
  if(!copy)return false;
  free(e->value);e->value=copy;return true;
 }
 char *key_copy=text_copy(v,key);
 if(!key_copy)return false;
 char *value_copy=text_copy(v,value);
 if(!value_copy){free(key_copy);return false;}
 e=calloc(1,sizeof *e);
 if(!e){free(key_copy);free(value_copy);dvm__throw(v,"java/lang/OutOfMemoryError","Settings entry");return false;}
 e->key=key_copy;e->value=value_copy;e->next=settings[table];settings[table]=e;return true;
}
static const char *key_arg(struct dvm *v,dvm_ref ref){
 if(!ref){dvm__throw(v,"java/lang/IllegalArgumentException","Settings key is null");return NULL;}
 const char *text=dvm_string_utf8(v,ref);
 if(!text)dvm__throw(v,"java/lang/IllegalArgumentException","Settings key is not a String");
 return text;
}
/* AOSP dereferences the ContentResolver (cr.getUserId()/cr.call), so null is
 * not a supported input there.  The diagnostic runtime states that boundary
 * explicitly instead of passing the failure through as a NullPointerException. */
static bool resolver_arg(struct dvm *v,dvm_ref ref){
 if(!ref){dvm__throw(v,"java/lang/IllegalArgumentException","ContentResolver is null");return false;}
 return true;
}
static bool integer_text(const char *text,int32_t *out){
 if(!text||!*text)return false;
 const char *digits=text;
 if(*digits=='+'||*digits=='-')++digits;
 if(!*digits)return false;
 for(const char *p=digits;*p;++p)if(*p<'0'||*p>'9')return false;
 errno=0;
 char *end=NULL;
 long long value=strtoll(text,&end,10);
 if(errno==ERANGE||end==text||*end||value<INT32_MIN||value>INT32_MAX)return false;
 *out=(int32_t)value;return true;
}
static bool long_text(const char *text,int64_t *out){
 if(!text||!*text)return false;
 const char *digits=text;
 if(*digits=='+'||*digits=='-')++digits;
 if(!*digits)return false;
 for(const char *p=digits;*p;++p)if(*p<'0'||*p>'9')return false;
 errno=0;
 char *end=NULL;
 long long value=strtoll(text,&end,10);
 if(errno==ERANGE||end==text||*end)return false;
 *out=(int64_t)value;return true;
}
static bool float_text(const char *text,float *out){
 if(!text||!*text)return false;
 char *end=NULL;
 float value=strtof(text,&end);
 if(end==text)return false;
 while(*end==' '||*end=='\t'||*end=='\n'||*end=='\r'||*end=='\f'||*end=='\v')++end;
 if(*end)return false;
 *out=value;return true;
}
static bool context_class(const char *c){
 return !strcmp(c,"android/content/Context")||!strcmp(c,"android/content/ContextWrapper")||
  !strcmp(c,"android/app/Application")||!strcmp(c,"android/app/Activity")||
  !strcmp(c,"android/app/Service");
}
static int settings_namespace(const char *c){
 if(!strcmp(c,"android/provider/Settings$Global"))return SETTINGS_GLOBAL;
 if(!strcmp(c,"android/provider/Settings$System"))return SETTINGS_SYSTEM;
 if(!strcmp(c,"android/provider/Settings$Secure"))return SETTINGS_SECURE;
 return -1;
}
/* A host-backed write is authoritative.  When the bridge rejects it the guest
 * gets false.  Without a host only an explicit fixture session may write the
 * table; production gets false plus a trace, never a false "system setting
 * changed" answer. */
static bool put_setting(struct dvm *v,int table,const char *key,const char *text,union dvm_value *out){
 if(host_api&&host_api->put){
  host_note_active();
  int status=host_api->put(host_user,table,key,text);
  if(status){
   sdk_trace_fprintf(stderr,"SETTINGS_HOST_WRITE_FAILED domain=%s key=%s status=%d result=false\n",
    domain_name(table),key,status);
   out->i=0;
  } else out->i=1;
  return true;
 }
 if(fixture_mode){
  if(entry_set(v,table,key,text))out->i=1;
  return true;
 }
 sdk_trace_fprintf(stderr,"SETTINGS_HOST_UNAVAILABLE op=put domain=%s key=%s result=false\n",
  domain_name(table),key);
 out->i=0;
 return true;
}
static bool settings_call(struct dvm *v,const char *c,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;
 int table=settings_namespace(c);
 if(table<0)return false;
 /* Settings.System.getUriFor(String) returns content://settings/<domain>/<key>.
  * OrientationLockListener registers a ContentObserver on this Uri. */
 if(IS("getUriFor","(Ljava/lang/String;)Landroid/net/Uri;")&&n==1){
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Settings key");return true;}
  const char *key=key_arg(v,a[0].l);
  if(!key)return true;
  char text[256];
  int length=snprintf(text,sizeof text,"content://settings/%s/%s",domain_name(table),key);
  if(length<0||(size_t)length>=sizeof text){dvm__throw(v,"java/lang/IllegalArgumentException","Settings key too long");return true;}
  dvm_ref uri_text=dvm_new_string(v,text);
  if(!uri_text){dvm__throw(v,"java/lang/OutOfMemoryError","Settings Uri");return true;}
  union dvm_value arg={.l=uri_text},parsed={0};
  if(!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&parsed)){
   if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Uri runtime unavailable");
   return true;
  }
  out->l=parsed.l;return true;
 }
 if(IS("getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I")&&n==3){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  char host_value[CONTENT_SETTINGS_HOST_VALUE_MAX];
  const char *text=NULL;
  int source=setting_read(table,key,host_value,sizeof host_value,&text);
  airplane_note(table,key,source,text);
  if(source<0)return setting_host_failure(v,source,table,key,"get");
  int32_t value=0;
  out->i=integer_text(text,&value)?value:a[2].i;return true;
 }
 if(IS("getLong","(Landroid/content/ContentResolver;Ljava/lang/String;J)J")&&n==3){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  char host_value[CONTENT_SETTINGS_HOST_VALUE_MAX];
  const char *text=NULL;
  int source=setting_read(table,key,host_value,sizeof host_value,&text);
  if(source<0)return setting_host_failure(v,source,table,key,"get");
  int64_t value=0;
  out->j=long_text(text,&value)?value:a[2].j;return true;
 }
 if(IS("getFloat","(Landroid/content/ContentResolver;Ljava/lang/String;F)F")&&n==3){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  char host_value[CONTENT_SETTINGS_HOST_VALUE_MAX];
  const char *text=NULL;
  int source=setting_read(table,key,host_value,sizeof host_value,&text);
  if(source<0)return setting_host_failure(v,source,table,key,"get");
  float value=0;
  out->f=float_text(text,&value)?value:a[2].f;return true;
 }
 if(IS("getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;")&&n==2){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  char host_value[CONTENT_SETTINGS_HOST_VALUE_MAX];
  const char *text=NULL;
  int source=setting_read(table,key,host_value,sizeof host_value,&text);
  if(source<0)return setting_host_failure(v,source,table,key,"get");
  if(source!=SETTING_READ_ABSENT){
   out->l=dvm_new_string(v,text);
   if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Settings string");
  }
  return true;
 }
 if(IS("getString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")&&n==3){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  char host_value[CONTENT_SETTINGS_HOST_VALUE_MAX];
  const char *text=NULL;
  int source=setting_read(table,key,host_value,sizeof host_value,&text);
  if(source<0)return setting_host_failure(v,source,table,key,"get");
  if(source==SETTING_READ_ABSENT){out->l=a[2].l;return true;}
  out->l=dvm_new_string(v,text);
  if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Settings string");
  return true;
 }
 if(IS("putInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)Z")&&n==3){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  char text[32];snprintf(text,sizeof text,"%d",a[2].i);
  return put_setting(v,table,key,text,out);
 }
 if(IS("putLong","(Landroid/content/ContentResolver;Ljava/lang/String;J)Z")&&n==3){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  char text[32];snprintf(text,sizeof text,"%lld",(long long)a[2].j);
  return put_setting(v,table,key,text,out);
 }
 if(IS("putFloat","(Landroid/content/ContentResolver;Ljava/lang/String;F)Z")&&n==3){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  char text[64];snprintf(text,sizeof text,"%.9g",(double)a[2].f);
  return put_setting(v,table,key,text,out);
 }
 if(IS("putString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z")&&n==3){
  if(!resolver_arg(v,a[0].l))return true;
  const char *key=key_arg(v,a[1].l);
  if(!key)return true;
  const char *value=NULL;
  if(a[2].l){
   value=dvm_string_utf8(v,a[2].l);
   if(!value){dvm__throw(v,"java/lang/IllegalArgumentException","Settings value is not a String");return true;}
  }
  return put_setting(v,table,key,value,out);
 }
 return false;
}
#include "settings_compat.inc"
static bool content_observer_call(struct dvm *v,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!self)return false;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o)return false;
 if(!strcmp(m,"<init>")&&(!strcmp(sig,"()V")||!strcmp(sig,"(Landroid/os/Handler;)V"))){
  if(o->data||o->data_destroy){dvm__throw(v,"java/lang/IllegalStateException","ContentObserver already initialized");return true;}
  if(!strcmp(sig,"(Landroid/os/Handler;)V")&&(!a||n!=1))return false;
  content_observer_state *state=calloc(1,sizeof *state);
  if(!state){dvm__throw(v,"java/lang/OutOfMemoryError","ContentObserver");return true;}
  if(!strcmp(sig,"(Landroid/os/Handler;)V"))state->handler=a[0].l;
  o->data=state;o->data_destroy=observer_state_drop;return true;
 }
 /* AOSP: deliverSelfNotifications() defaults to false, and onChange is an
  * empty method that subclasses override.  Both are real base semantics. */
 if(!strcmp(m,"deliverSelfNotifications")&&!strcmp(sig,"()Z")){out->i=0;return true;}
 if(!strcmp(m,"onChange")&&(!strcmp(sig,"(Z)V")||!strcmp(sig,"(ZLandroid/net/Uri;)V")))return true;
 return false;
}
static bool content_resolver_call(struct dvm *v,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 if(!self)return false;
 if(!strcmp(m,"registerContentObserver")&&!strcmp(sig,"(Landroid/net/Uri;ZLandroid/database/ContentObserver;)V")){
  if(!a||n!=3)return false;
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","uri");return true;}
  if(!a[2].l){dvm__throw(v,"java/lang/NullPointerException","observer");return true;}
  if(observation_count>=CONTENT_OBSERVER_CAP){dvm__throw(v,"java/lang/IllegalStateException","ContentObserver capacity exhausted");return true;}
  content_observation *record=&observations[observation_count++];
  record->observer=a[2].l;record->uri=a[0].l;record->descendants=a[1].i!=0;
  sdk_trace_fprintf(stderr,"CONTENT_OBSERVER register uri=%u descendants=%d count=%d callback_source=none\n",
   (unsigned)a[0].l,record->descendants?1:0,observation_count);
  return true;
 }
 if(!strcmp(m,"unregisterContentObserver")&&!strcmp(sig,"(Landroid/database/ContentObserver;)V")){
  if(!a||n!=1)return false;
  int removed=0,kept=0;
  for(int i=0;i<observation_count;++i){
   if(observations[i].observer==a[0].l){++removed;continue;}
   observations[kept++]=observations[i];
  }
  if(kept<observation_count)memset(observations+kept,0,(size_t)(observation_count-kept)*sizeof observations[0]);
  observation_count=kept;
  sdk_trace_fprintf(stderr,"CONTENT_OBSERVER unregister removed=%d count=%d\n",removed,observation_count);
  return true;
 }
 return false;
}
bool content_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!v||!c||!m||!sig||!out)return false;
 memset(out,0,sizeof *out);
 activate(v);
 if(!content_gc_ensure(v))return false;
 if(context_class(c)&&IS("getSystemService","(Ljava/lang/String;)Ljava/lang/Object;")&&n==1&&a&&a[0].l){
  const char *service=dvm_string_utf8(v,a[0].l);
  if(service&&!strcmp(service,"location")){
   if(!self){dvm__throw(v,"java/lang/NullPointerException","Context receiver");return true;}
   if(!location_manager)location_manager=dvm_new_object(v,dvm_find_class_or_external(v,"android/location/LocationManager"));
   out->l=location_manager;compat_note(0,1,"service");return true;
  }
 }
 if(!strcmp(c,"android/location/LocationManager")&&n==1&&content_runtime_method_supported(c,m,sig)){
  compat_note(0,2,"get_disabled_no_location");return true;
 }
 if(!fixture_mode&&compat_settings_call(v,c,m,sig,a,n,out))return true;
 if(context_class(c)&&IS("getContentResolver","()Landroid/content/ContentResolver;")&&n==0){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Context receiver");return true;}
  if(!content_resolver){
   struct dvm_class *cls=dvm_find_class_or_external(v,"android/content/ContentResolver");
   content_resolver=cls?dvm_new_object(v,cls):0;
   if(!content_resolver){dvm__throw(v,"java/lang/OutOfMemoryError","ContentResolver");return true;}
  }
  out->l=content_resolver;return true;
 }
 if(!strcmp(c,"android/database/ContentObserver"))return content_observer_call(v,m,sig,self,a,n,out);
 if(!strcmp(c,"android/content/ContentResolver"))return content_resolver_call(v,m,sig,self,a,n,out);
 return settings_call(v,c,m,sig,self,a,n,out);
}
static bool text_is(struct dvm *v,dvm_ref ref,const char *want){
 const char *text=ref?dvm_string_utf8(v,ref):NULL;
 return text&&!strcmp(text,want);
}
static bool exception_is(struct dvm *v,const char *name){
 dvm_ref ref=dvm_exception(v);struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
 for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)
  if(k->name&&!strcmp(k->name,name))return true;
 return false;
}
static bool call_ok(struct dvm *v,const char *c,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 return content_runtime_call(v,c,m,sig,self,a,n,out)&&!dvm_exception(v);
}
static bool clear_key(struct dvm *v,const char *c,dvm_ref resolver,const char *key){
 union dvm_value args[3]={{.l=resolver},{.l=dvm_new_string(v,key)},{.l=0}},out={0};
 if(!args[1].l)return false;
 return content_runtime_call(v,c,"putString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z",
  0,args,3,&out)&&!dvm_exception(v)&&out.i;
}
/* Host fixture used only by the selftest.  The public selftest parks the real
 * bridge and enables fixture mode first, so neither the fake host nor the
 * in-memory table can touch production state. */
typedef struct content_host_fixture {
 int get_calls,put_calls,fail_get,fail_put;bool present;char value[64];
} content_host_fixture;
static int fixture_get(void *user,int domain,const char *name,char *value,size_t capacity){
 (void)domain;(void)name;
 content_host_fixture *f=user;++f->get_calls;
 if(f->fail_get)return f->fail_get;
 if(!f->present)return 0;
 if(capacity)snprintf(value,capacity,"%s",f->value);
 return 1;
}
static int fixture_put(void *user,int domain,const char *name,const char *value){
 (void)domain;(void)name;
 content_host_fixture *f=user;++f->put_calls;
 if(f->fail_put)return 1;
 snprintf(f->value,sizeof f->value,"%s",value?value:"");
 f->present=true;return 0;
}
static bool expect_exception(struct dvm *v,const char *c,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int n,union dvm_value *out,const char *name){
 if(!content_runtime_call(v,c,m,sig,self,a,n,out))return false;
 bool hit=exception_is(v,name);
 dvm_clear_exception(v);
 return hit&&!dvm_exception(v);
}
/* Fixture phase (fixture_mode on, no real bridge): deterministic in-memory
 * store, namespace isolation, and a fixture host. */
static bool selftest_fixture(struct dvm *v){
 if(!v)return false;
 union dvm_value out={0};
 struct dvm_class *context_cls=dvm_find_class_or_external(v,"android/content/Context");
 dvm_ref context=context_cls?dvm_new_object(v,context_cls):0;
 dvm_ref context_two=context_cls?dvm_new_object(v,context_cls):0;
 if(!context||!context_two)return false;
 if(!call_ok(v,"android/content/Context","getContentResolver","()Landroid/content/ContentResolver;",
  context,NULL,0,&out)||!out.l)return false;
 dvm_ref resolver=out.l;
 if(!call_ok(v,"android/content/Context","getContentResolver","()Landroid/content/ContentResolver;",
  context_two,NULL,0,&out)||out.l!=resolver)return false;
 static const char *const classes[3]={
  "android/provider/Settings$Global","android/provider/Settings$System","android/provider/Settings$Secure"};
 static const char *const tags[3]={"global","system","secure"};
 for(int i=0;i<3;++i){
  char key[96];
  snprintf(key,sizeof key,"__content_runtime_selftest_%s__",tags[i]);
  if(!clear_key(v,classes[i],resolver,key))return false;
  dvm_ref key_ref=dvm_new_string(v,key);
  if(!key_ref)return false;
  union dvm_value a[3]={{.l=resolver},{.l=key_ref},{.i=42}};
  if(!call_ok(v,classes[i],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,a,3,&out)||out.i!=42)return false;
  a[2]=(union dvm_value){.f=1.5f};
  if(!call_ok(v,classes[i],"getFloat","(Landroid/content/ContentResolver;Ljava/lang/String;F)F",0,a,3,&out)||out.f!=1.5f)return false;
  a[2]=(union dvm_value){.j=99};
  if(!call_ok(v,classes[i],"getLong","(Landroid/content/ContentResolver;Ljava/lang/String;J)J",0,a,3,&out)||out.j!=99)return false;
  if(!call_ok(v,classes[i],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,a,2,&out)||out.l)return false;
  union dvm_value fallback={.l=dvm_new_string(v,"fallback")};
  if(!fallback.l)return false;
  union dvm_value with_default[3]={{.l=resolver},{.l=key_ref},fallback};
  if(!call_ok(v,classes[i],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
   0,with_default,3,&out)||!text_is(v,out.l,"fallback"))return false;
  a[2]=(union dvm_value){.i=7};
  if(!call_ok(v,classes[i],"putInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)Z",0,a,3,&out)||!out.i)return false;
  a[2]=(union dvm_value){.i=42};
  if(!call_ok(v,classes[i],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,a,3,&out)||out.i!=7)return false;
  if(!call_ok(v,classes[i],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,a,2,&out)||
   !text_is(v,out.l,"7"))return false;
  a[2]=(union dvm_value){.f=2.5f};
  if(!call_ok(v,classes[i],"putFloat","(Landroid/content/ContentResolver;Ljava/lang/String;F)Z",0,a,3,&out)||!out.i)return false;
  a[2]=(union dvm_value){.f=0};
  if(!call_ok(v,classes[i],"getFloat","(Landroid/content/ContentResolver;Ljava/lang/String;F)F",0,a,3,&out)||out.f!=2.5f)return false;
  a[2]=(union dvm_value){.j=1234567890123LL};
  if(!call_ok(v,classes[i],"putLong","(Landroid/content/ContentResolver;Ljava/lang/String;J)Z",0,a,3,&out)||!out.i)return false;
  a[2]=(union dvm_value){.j=0};
  if(!call_ok(v,classes[i],"getLong","(Landroid/content/ContentResolver;Ljava/lang/String;J)J",0,a,3,&out)||out.j!=1234567890123LL)return false;
  union dvm_value text_args[3]={{.l=resolver},{.l=key_ref},{.l=dvm_new_string(v,"hello-settings")}};
  if(!text_args[2].l)return false;
  if(!call_ok(v,classes[i],"putString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z",
   0,text_args,3,&out)||!out.i)return false;
  if(!call_ok(v,classes[i],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,text_args,2,&out)||
   !text_is(v,out.l,"hello-settings"))return false;
  a[2]=(union dvm_value){.i=42};
  if(!call_ok(v,classes[i],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,a,3,&out)||out.i!=42)return false;
  if(!clear_key(v,classes[i],resolver,key))return false;
 }
 /* A key written in one Settings namespace is invisible to the other two. */
 dvm_ref iso_ref=dvm_new_string(v,"__content_runtime_isolation__");
 if(!iso_ref)return false;
 for(int i=0;i<3;++i)if(!clear_key(v,classes[i],resolver,"__content_runtime_isolation__"))return false;
 union dvm_value iso[3]={{.l=resolver},{.l=iso_ref},{.i=7}};
 if(!call_ok(v,classes[0],"putInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)Z",0,iso,3,&out)||!out.i)return false;
 iso[2]=(union dvm_value){.i=8};
 if(!call_ok(v,classes[1],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,iso,3,&out)||out.i!=8)return false;
 iso[2]=(union dvm_value){.i=9};
 if(!call_ok(v,classes[2],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,iso,3,&out)||out.i!=9)return false;
 iso[2]=(union dvm_value){.i=0};
 if(!call_ok(v,classes[0],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,iso,3,&out)||out.i!=7)return false;
 if(!clear_key(v,classes[0],resolver,"__content_runtime_isolation__"))return false;
 /* airplane_mode_on is an ordinary key now.  With no value it returns the
    caller's default verbatim (no fixed 0 override); a fixture value wins
    while it exists. */
 const char *airplane="airplane_mode_on";
 if(!clear_key(v,classes[0],resolver,airplane))return false;
 dvm_ref airplane_ref=dvm_new_string(v,airplane);
 if(!airplane_ref)return false;
 union dvm_value air[3]={{.l=resolver},{.l=airplane_ref},{.i=1}};
 if(!call_ok(v,classes[0],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,air,3,&out)||out.i!=1)return false;
 if(!airplane_probe_printed)return false;
 air[2]=(union dvm_value){.i=0};
 if(!call_ok(v,classes[0],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,air,3,&out)||out.i!=0)return false;
 air[2]=(union dvm_value){.i=1};
 if(!call_ok(v,classes[0],"putInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)Z",0,air,3,&out)||!out.i)return false;
 air[2]=(union dvm_value){.i=0};
 if(!call_ok(v,classes[0],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,air,3,&out)||out.i!=1)return false;
 if(!clear_key(v,classes[0],resolver,airplane))return false;
 air[2]=(union dvm_value){.i=1};
 if(!call_ok(v,classes[0],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,air,3,&out)||out.i!=1)return false;
 /* Host bridge: the host value wins over the fixture store; an absent host
    falls back to the fixture value; a failed host is fail-closed and must not
    leak the fixture value or a caller default.  A rejected host write returns
    false without touching the fixture store.  The real bridge is parked by
    the wrapper. */
 const char *host_key="__content_runtime_host_bridge__";
 dvm_ref host_key_ref=dvm_new_string(v,host_key);
 if(!host_key_ref)return false;
 union dvm_value host_args[3]={{.l=resolver},{.l=host_key_ref},{.l=dvm_new_string(v,"memory-value")}};
 if(!host_args[2].l)return false;
 if(!call_ok(v,classes[0],"putString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z",
  0,host_args,3,&out)||!out.i)return false;
 content_host_fixture fixture={0};
 fixture.present=true;
 snprintf(fixture.value,sizeof fixture.value,"%s","host-value");
 content_settings_host fake={fixture_get,fixture_put};
 content_runtime_set_host(&fake,&fixture);
 if(!call_ok(v,classes[0],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,host_args,2,&out)||
  !text_is(v,out.l,"host-value")||fixture.get_calls!=1)return false;
 host_args[2].l=dvm_new_string(v,"written-to-host");
 if(!host_args[2].l)return false;
 if(!call_ok(v,classes[0],"putString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z",
  0,host_args,3,&out)||!out.i||fixture.put_calls!=1||strcmp(fixture.value,"written-to-host"))return false;
 fixture.present=false;
 if(!call_ok(v,classes[0],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,host_args,2,&out)||
  !text_is(v,out.l,"memory-value"))return false;
 /* Backend failure fails closed: the fixture value exists but must not be
    returned, and no fixed fallback is invented. */
 fixture.fail_get=-1;
 if(!expect_exception(v,classes[0],"getString",
   "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,host_args,2,&out,
   "java/lang/IllegalStateException"))return false;
 /* Permission failure is reported as SecurityException, not as an absent key
    and not as the caller default. */
 fixture.fail_get=-2;
 union dvm_value fail_default[3]={{.l=resolver},{.l=host_key_ref},{.i=42}};
 if(!expect_exception(v,classes[0],"getInt",
   "(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,fail_default,3,&out,
   "java/lang/SecurityException"))return false;
 /* A rejected host write reports false and leaves the fixture store alone. */
 fixture.fail_get=0;fixture.fail_put=1;
 host_args[2].l=dvm_new_string(v,"rejected");
 if(!host_args[2].l)return false;
 if(!call_ok(v,classes[0],"putString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z",
  0,host_args,3,&out)||out.i)return false;
 fixture.fail_put=0;fixture.present=false;
 if(!call_ok(v,classes[0],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,host_args,2,&out)||
  !text_is(v,out.l,"memory-value"))return false;
 content_runtime_set_host(NULL,NULL);
 if(!call_ok(v,classes[0],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,host_args,2,&out)||
  !text_is(v,out.l,"memory-value"))return false;
 /* With the bridge removed the fixture store is authoritative in fixture
    mode, and a null putString deletes rather than storing "null". */
 host_args[2].l=0;
 if(!call_ok(v,classes[0],"putString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z",
  0,host_args,3,&out)||!out.i)return false;
 if(!call_ok(v,classes[0],"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,host_args,2,&out)||out.l)return false;
 if(!clear_key(v,classes[0],resolver,host_key))return false;
 /* Leave one fixture-only value in the table for the production phase, which
    proves the table is gated off rather than merely emptied. */
 dvm_ref held_ref=dvm_new_string(v,"__content_runtime_fixture_isolation__");
 if(!held_ref)return false;
 union dvm_value held[3]={{.l=resolver},{.l=held_ref},{.i=7}};
 if(!call_ok(v,classes[0],"putInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)Z",0,held,3,&out)||!out.i)return false;
 union dvm_value null_key[3]={{.l=resolver},{.l=0},{.i=0}};
 if(!content_runtime_call(v,classes[0],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,null_key,3,&out))return false;
 bool key_rejected=exception_is(v,"java/lang/IllegalArgumentException");
 dvm_clear_exception(v);
 if(!key_rejected)return false;
 union dvm_value null_resolver[3]={{.l=0},{.l=dvm_new_string(v,"key")},{.i=0}};
 if(!null_resolver[1].l)return false;
 if(!content_runtime_call(v,classes[0],"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,null_resolver,3,&out))return false;
 bool resolver_rejected=exception_is(v,"java/lang/IllegalArgumentException");
 dvm_clear_exception(v);
 if(!resolver_rejected||dvm_exception(v))return false;
 return true;
}
/* Production phase (fixture_mode off, no host installed).  The fixture value
 * from the previous phase is still physically present but must be invisible:
 * reads return the caller default and writes report failure instead of
 * claiming the system setting changed. */
static bool selftest_production(struct dvm *v){
 if(!v)return false;
 const char *held_key="__content_runtime_fixture_isolation__";
 settings_entry *held=entry_find(SETTINGS_GLOBAL,held_key);
 if(!held||!held->value||strcmp(held->value,"7"))return false;
 union dvm_value out={0};
 struct dvm_class *context_cls=dvm_find_class_or_external(v,"android/content/Context");
 dvm_ref context=context_cls?dvm_new_object(v,context_cls):0;
 if(!context)return false;
 if(!call_ok(v,"android/content/Context","getContentResolver","()Landroid/content/ContentResolver;",
  context,NULL,0,&out)||!out.l)return false;
 dvm_ref resolver=out.l;
 dvm_ref held_ref=dvm_new_string(v,held_key);
 if(!held_ref)return false;
 union dvm_value args[3]={{.l=resolver},{.l=held_ref},{.i=42}};
 if(!call_ok(v,"android/provider/Settings$Global","getInt",
  "(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,args,3,&out)||out.i!=42)return false;
 args[2]=(union dvm_value){.i=9};
 if(!call_ok(v,"android/provider/Settings$Global","putInt",
  "(Landroid/content/ContentResolver;Ljava/lang/String;I)Z",0,args,3,&out)||out.i)return false;
 union dvm_value put_text[3]={{.l=resolver},{.l=held_ref},{.l=dvm_new_string(v,"no-host-write")}};
 if(!put_text[2].l)return false;
 if(!call_ok(v,"android/provider/Settings$Global","putString",
  "(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z",0,put_text,3,&out)||out.i)return false;
 if(!call_ok(v,"android/provider/Settings$Global","getString",
  "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",0,args,2,&out)||out.l)return false;
 union dvm_value str_default[3]={{.l=resolver},{.l=held_ref},{.l=dvm_new_string(v,"caller-default")}};
 if(!str_default[2].l)return false;
 if(!call_ok(v,"android/provider/Settings$Global","getString",
  "(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
  0,str_default,3,&out)||!text_is(v,out.l,"caller-default"))return false;
 /* An absent airplane_mode_on returns the caller default verbatim; there is no
    fixed 0 override in business execution any more. */
 airplane_probe_printed=false;
 dvm_ref airplane_ref=dvm_new_string(v,"airplane_mode_on");
 if(!airplane_ref)return false;
 union dvm_value air[3]={{.l=resolver},{.l=airplane_ref},{.i=1}};
 if(!call_ok(v,"android/provider/Settings$Global","getInt",
  "(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,air,3,&out)||out.i!=1)return false;
 if(!airplane_probe_printed)return false;
 air[2]=(union dvm_value){.i=0};
 if(!call_ok(v,"android/provider/Settings$Global","getInt",
  "(Landroid/content/ContentResolver;Ljava/lang/String;I)I",0,air,3,&out)||out.i!=0)return false;
 /* The gated fixture value is still there and the failed put did not change
    it.  Production ignored both the value and the failed write. */
 held=entry_find(SETTINGS_GLOBAL,held_key);
 if(!held||!held->value||strcmp(held->value,"7"))return false;
 return true;
}
static bool selftest_observers(struct dvm *v){
 bool ok=true;
 union dvm_value out={0},args[3];
 struct dvm_class *observer_class=dvm_find_class_or_external(v,"android/database/ContentObserver");
 struct dvm_class *uri_class=dvm_find_class_or_external(v,"android/net/Uri");
 struct dvm_class *resolver_class=dvm_find_class_or_external(v,"android/content/ContentResolver");
 dvm_ref observer=observer_class?dvm_new_object(v,observer_class):0;
 dvm_ref observer_null_handler=observer_class?dvm_new_object(v,observer_class):0;
 dvm_ref uri=uri_class?dvm_new_object(v,uri_class):0;
 dvm_ref resolver=resolver_class?dvm_new_object(v,resolver_class):0;
 if(!observer||!observer_null_handler||!uri||!resolver)return false;
 if(!content_observer_call(v,"<init>","()V",observer,NULL,0,&out)||dvm_exception(v))ok=false;
 memset(&out,0,sizeof out);
 if(!content_observer_call(v,"deliverSelfNotifications","()Z",observer,NULL,0,&out)||out.i!=0||dvm_exception(v))ok=false;
 /* ContentObserver(null) is legal: the observer inherits the caller's thread. */
 memset(&out,0,sizeof out);args[0].l=0;
 if(!content_observer_call(v,"<init>","(Landroid/os/Handler;)V",observer_null_handler,args,1,&out)||dvm_exception(v))ok=false;
 /* Settings.System.getUriFor builds a real content://settings/system/<key> Uri. */
 {
  union dvm_value key={0},uri_out={0};
  key.l=dvm_new_string(v,"accelerometer_rotation");
  if(!key.l)ok=false;
  else if(!content_runtime_call(v,"android/provider/Settings$System","getUriFor",
   "(Ljava/lang/String;)Landroid/net/Uri;",0,&key,1,&uri_out)||dvm_exception(v)||
   !uri_out.l||!uri_runtime_is_uri(v,uri_out.l))ok=false;
  if(dvm_exception(v))dvm_clear_exception(v);
 }
 args[0].l=uri;args[1].i=1;args[2].l=observer;
 memset(&out,0,sizeof out);
 if(!content_resolver_call(v,"registerContentObserver","(Landroid/net/Uri;ZLandroid/database/ContentObserver;)V",resolver,args,3,&out)||dvm_exception(v))ok=false;
 if(observation_count!=1)ok=false;
 args[0].l=observer;args[1].i=0;args[2].l=0;
 if(!content_resolver_call(v,"unregisterContentObserver","(Landroid/database/ContentObserver;)V",resolver,args,1,&out)||dvm_exception(v))ok=false;
 if(observation_count!=0)ok=false;
 /* Unregistering an observer that is not registered is a no-op. */
 if(!content_resolver_call(v,"unregisterContentObserver","(Landroid/database/ContentObserver;)V",resolver,args,1,&out)||dvm_exception(v))ok=false;
 if(dvm_exception(v))dvm_clear_exception(v);
 if(!ok)sdk_trace_fprintf(stderr,"CONTENT_OBSERVER_SELFTEST FAIL\n");
 return ok;
}
bool content_runtime_selftest(struct dvm *v){
 /* The selftest exercises the explicit fixture store and a fixture host; the
  * real @ohos.settings bridge must not see test reads or writes.  The fixture
  * table is left in place for selftest_production() to prove it is gated off,
  * then the caller's host/fixture state is restored. */
 const content_settings_host *saved=host_api;
 void *saved_user=host_user;
 bool saved_fixture=fixture_mode;
 content_runtime_set_host(NULL,NULL);
 content_runtime_set_fixture_mode(true);
 bool ok=selftest_fixture(v);
 content_runtime_set_fixture_mode(false);
 if(ok)ok=selftest_production(v);
 content_runtime_set_host(saved,saved_user);
 content_runtime_set_fixture_mode(saved_fixture);
 /* The isolation proof above needs the fixture entry to survive the switch
  * off; drop it now so a normal selftest leaves no fixture data behind. */
 if(!saved_fixture)settings_values_clear();
 if(ok)ok=selftest_observers(v);
 if(!ok)return false;
 sdk_trace_fprintf(stderr,"CONTENT_RUNTIME_SELFTEST PASS resolver_singleton=1 global=1 system=1 secure=1 "
  "get_default=1 put_get_roundtrip=1 namespace_isolation=1 "
  "airplane_mode_caller_default=1 airplane_mode_fixture_value=1 "
  "host_bridge=1 host_priority=1 host_absent_fixture_fallback=1 "
  "host_backend_fail_closed=1 host_permission_fail_closed=1 host_write_failure=1 "
  "fixture_isolation=1 no_host_put_false=1 "
  "null_key_rejected=1 null_resolver_rejected=1 "
  "observer_register=1 callback_source=none\n");
 return true;
}

int content_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 (void)v;if(o->data_destroy!=observer_state_drop)return DVM_GC_PAYLOAD_UNKNOWN;
 content_observer_state *p=o->data;if(!p)return DVM_GC_PAYLOAD_INVALID;
 if(visit)visit(&p->handler,u);return DVM_GC_PAYLOAD_REFS;
}
