#include "network_runtime.h"
#include "gc_runtime.h"
#include "dvm/dvm_internal.h"
#include "gc_operation.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

extern int sdk_trace_fprintf(FILE*,const char*,...);

/* Manager and enum objects are ordinary VM objects.  They are held only by
 * the per-VM C cache and are deliberately not pinned, following the existing
 * runtime modules.  A host VM switch drops every cached reference. */
#define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))

enum {
 NETWORK_STATE_CONNECTING,
 NETWORK_STATE_CONNECTED,
 NETWORK_STATE_SUSPENDED,
 NETWORK_STATE_DISCONNECTING,
 NETWORK_STATE_DISCONNECTED,
 NETWORK_STATE_UNKNOWN,
 NETWORK_STATE_COUNT
};
enum {
 NETWORK_DETAILED_IDLE,
 NETWORK_DETAILED_SCANNING,
 NETWORK_DETAILED_CONNECTING,
 NETWORK_DETAILED_AUTHENTICATING,
 NETWORK_DETAILED_OBTAINING_IPADDR,
 NETWORK_DETAILED_CONNECTED,
 NETWORK_DETAILED_SUSPENDED,
 NETWORK_DETAILED_DISCONNECTING,
 NETWORK_DETAILED_DISCONNECTED,
 NETWORK_DETAILED_FAILED,
 NETWORK_DETAILED_BLOCKED,
 NETWORK_DETAILED_VERIFYING_POOR_LINK,
 NETWORK_DETAILED_CAPTIVE_PORTAL_CHECK,
 NETWORK_DETAILED_COUNT
};

static const char *const network_state_names[NETWORK_STATE_COUNT] = {
 "CONNECTING","CONNECTED","SUSPENDED","DISCONNECTING","DISCONNECTED","UNKNOWN"
};
static const char *const network_detailed_names[NETWORK_DETAILED_COUNT] = {
 "IDLE","SCANNING","CONNECTING","AUTHENTICATING","OBTAINING_IPADDR",
 "CONNECTED","SUSPENDED","DISCONNECTING","DISCONNECTED","FAILED","BLOCKED",
 "VERIFYING_POOR_LINK","CAPTIVE_PORTAL_CHECK"
};

#define NETWORK_INFO_MAGIC 0x4e57494eu /* NWIN */
#define NETWORK_CAPS_MAGIC 0x4e574350u /* NWCP */
#define NETWORK_ENUM_MAGIC 0x4e57454du /* NWEN */
#define NETWORK_OBJ_MAGIC  0x4e57454fu /* NWEO */

typedef struct network_info_payload {
 uint32_t magic;
 int32_t type;
 bool connected;
 bool available;
 int32_t state;
 int32_t detailed;
 /* TelephonyManager.NETWORK_TYPE_* observed for this NetworkInfo, or
  * NETWORK_TYPE_UNKNOWN (0) when the host has no telephony information.
  * getSubtypeName() returns TelephonyManager.getNetworkTypeName(subtype);
  * it never changes connected/available state. */
 int32_t subtype;
} network_info_payload;

typedef struct network_caps_payload {
 uint32_t magic;
 uint32_t transports;
 uint32_t capabilities;
} network_caps_payload;

typedef struct network_obj_payload {
 uint32_t magic;
 uint64_t handle;
} network_obj_payload;

typedef struct network_enum_payload {
 uint32_t magic;
 const char *name;
 int32_t ordinal;
} network_enum_payload;

typedef struct network_int_constant {
 const char *name;
 int32_t value;
} network_int_constant;

static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static network_host_bridge host_bridge;
static struct dvm *active;
static dvm_ref connectivity_manager;
static dvm_ref telephony_manager;
static dvm_ref activity_manager;
static dvm_ref clipboard_manager;
static dvm_ref audio_manager;
static dvm_ref media_router_manager;
static dvm_ref input_manager;
static dvm_ref notification_manager, vibrator_manager;
static bool notification_services_install(struct dvm *v);
static dvm_ref network_states[NETWORK_STATE_COUNT];
static dvm_ref network_detailed_states[NETWORK_DETAILED_COUNT];
/* GIL-owned strong global references; bounded and removed on unregister/VM end. */
#define NETWORK_CALLBACKS 16
static struct network_registration {
 dvm_ref callback, network, caps;
 uint64_t generation, handle;
 uint32_t transports, capabilities;
 bool online, initialized;
} network_callbacks[NETWORK_CALLBACKS];
static uint64_t network_generation;
static _Thread_local unsigned network_query_scope;
static _Thread_local network_host_snapshot network_query_snapshot;
static _Thread_local bool network_query_valid;
static _Thread_local const char *network_snapshot_failure;
static _Thread_local const char *network_query_reason;
static network_host_snapshot network_last_snapshot;
static bool network_last_valid;
static bool network_delivering;
static bool network_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 if(!v||!visit||context!=&active||active!=v||!dvm_gil_held(v))return false;
 /* Host service singletons and stable enum constants are process roots.
  * NetworkInfo/Capabilities result objects are ordinary guest owner graphs. */
 dvm_ref *roots[]={&connectivity_manager,&telephony_manager,&activity_manager,
  &clipboard_manager,&audio_manager,&media_router_manager,&input_manager,&notification_manager,&vibrator_manager};
 for(unsigned i=0;i<sizeof roots/sizeof roots[0];i++)if(*roots[i])visit(roots[i],user);
 for(unsigned i=0;i<NETWORK_STATE_COUNT;i++)if(network_states[i])visit(&network_states[i],user);
 for(unsigned i=0;i<NETWORK_DETAILED_COUNT;i++)if(network_detailed_states[i])visit(&network_detailed_states[i],user);
 for(unsigned i=0;i<NETWORK_CALLBACKS;i++){
  struct network_registration *r=&network_callbacks[i];
  if(r->callback)visit(&r->callback,user);
  if(r->network)visit(&r->network,user);
  if(r->caps)visit(&r->caps,user);
 }
 return true;
}
static bool network_gc_ensure(struct dvm *v){
 if(dvm_gc_root_provider_add(v,"network-runtime/host",DVM_GC_ROOT_STRONG,network_gc_roots,&active))return true;
 if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Network host GC roots");return false;
}
void network_runtime_gc_vm_end(struct dvm *v){
 dvm_gc_root_provider_remove(v,"network-runtime/host",&active);
 if(active==v){
  memset(network_callbacks,0,sizeof network_callbacks);
  network_last_valid=false;memset(&network_last_snapshot,0,sizeof network_last_snapshot);
  connectivity_manager=telephony_manager=activity_manager=clipboard_manager=audio_manager=media_router_manager=input_manager=notification_manager=vibrator_manager=0;
  memset(network_states,0,sizeof network_states);memset(network_detailed_states,0,sizeof network_detailed_states);active=NULL;
 }
}

static void network_info_destroy(void *ptr){free(ptr);}
static void network_caps_destroy(void *ptr){free(ptr);}
static void network_obj_destroy(void *ptr){free(ptr);}
static void network_enum_destroy(void *ptr){free(ptr);}

static bool oom(struct dvm *v,const char *what){
 if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","%s",what);
 return false;
}

static void activate(struct dvm *v){
 if(!v||v==active)return;
 active=v;
 memset(network_callbacks,0,sizeof network_callbacks);
 network_last_valid=false;memset(&network_last_snapshot,0,sizeof network_last_snapshot);
 network_delivering=false;
 connectivity_manager=0;
 telephony_manager=0;
  clipboard_manager=0;
  audio_manager=0;
  input_manager=0;
 notification_manager=vibrator_manager=0;
 memset(network_states,0,sizeof network_states);
 memset(network_detailed_states,0,sizeof network_detailed_states);
 /* Give the synthesized enum classes an Enum superclass, as Java's enum
  * identity/name machinery expects.  Never touch a guest-defined class. */
 struct dvm_class *enum_cls=dvm_find_class_or_external(v,"java/lang/Enum");
 struct dvm_class *state_cls=dvm_find_class_or_external(v,"android/net/NetworkInfo$State");
 struct dvm_class *detailed_cls=dvm_find_class_or_external(v,"android/net/NetworkInfo$DetailedState");
 if(enum_cls&&state_cls&&state_cls->external)state_cls->super=enum_cls;
 if(enum_cls&&detailed_cls&&detailed_cls->external)detailed_cls->super=enum_cls;
}

void network_runtime_vm_start(struct dvm *v){
 active=NULL;
 activate(v);
 notification_services_install(v);
}

static void host_bridge_copy(network_host_bridge *out){
 pthread_mutex_lock(&host_lock);
 *out=host_bridge;
 pthread_mutex_unlock(&host_lock);
}

void network_runtime_set_host(const network_host_bridge *bridge){
 pthread_mutex_lock(&host_lock);
 if(bridge&&bridge->snapshot)host_bridge=*bridge;
 else memset(&host_bridge,0,sizeof host_bridge);
 pthread_mutex_unlock(&host_lock);
}

void sdk_set_network_host(const network_host_bridge *bridge){
 network_runtime_set_host(bridge);
}

/* AudioManager volume observation.  The host bridge queries the real OHOS
 * audio volume for the requested stream; without a bridge, or when the host
 * rejects the stream, getStreamVolume fails closed instead of reporting a
 * fabricated volume. */
typedef int (*audio_volume_host_fn)(void *user,int stream_type,int *volume);
static audio_volume_host_fn audio_volume_host;
static void *audio_volume_user;
__attribute__((visibility("default"))) void sdk_set_audio_volume_host(audio_volume_host_fn callback,void *user){
 pthread_mutex_lock(&host_lock);audio_volume_host=callback;audio_volume_user=user;pthread_mutex_unlock(&host_lock);
}

void network_runtime_clear_host(void){
 network_runtime_set_host(NULL);
}

static bool snapshot_flag(int32_t value){
 return value==0||value==1;
}

static bool snapshot_terminated(const char *value,size_t capacity){
 return memchr(value,0,capacity)!=NULL;
}

static const char *snapshot_invalid_field(const network_host_snapshot *snap){
 if(!snapshot_flag(snap->connectivity_valid))return "connectivity_valid";
 if(!snapshot_flag(snap->has_default_network))return "has_default_network";
 if(!snapshot_flag(snap->connected))return "connected";
 if(!snapshot_flag(snap->available))return "available";
 if(!snapshot_flag(snap->metered))return "metered";
 if(!snapshot_flag(snap->type_valid))return "type_valid";
 if(!snapshot_flag(snap->telephony_valid))return "telephony_valid";
 if(!snapshot_flag(snap->network_type_valid))return "network_type_valid";
 if(!snapshot_flag(snap->sim_state_valid))return "sim_state_valid";
 if(!snapshot_flag(snap->phone_type_valid))return "phone_type_valid";
 if(!snapshot_flag(snap->operator_numeric_valid))
  return "operator_numeric_valid";
 if(!snapshot_flag(snap->operator_name_valid))return "operator_name_valid";
 if(!snapshot_terminated(snap->operator_numeric,
  sizeof snap->operator_numeric))return "operator_numeric";
 if(!snapshot_terminated(snap->operator_name,sizeof snap->operator_name))
  return "operator_name";
 return NULL;
}

static void snapshot_fail(const char *reason,int rc,
 const network_host_snapshot *snap,const char *field){
 sdk_trace_fprintf(stderr,
  "NETWORK_HOST_SNAPSHOT_FAIL reason=%s rc=%d version=%u size=%u "
  "expected_version=%u expected_size=%u field=%s\n",
  reason,rc,snap->version,snap->size,NETWORK_HOST_SNAPSHOT_VERSION,
  (unsigned)sizeof *snap,field?field:"none");
}

/* Host ABI: the callback returns 0 on success and a negative value on failure
 * (see network_runtime.h).  A nonzero result is rejected even if the callback
 * filled in otherwise valid fields; the buffer is never used in that case.
 * Only a zero result with matching version/size and legal fields succeeds. */
static bool host_snapshot(network_host_snapshot *out){
 network_snapshot_failure=NULL;
 network_host_bridge bridge;
 host_bridge_copy(&bridge);
 if(!bridge.snapshot){
  network_snapshot_failure="no_bridge";
  network_host_snapshot empty;
  memset(&empty,0,sizeof empty);
  snapshot_fail("no_bridge",0,&empty,NULL);
  return false;
 }
 memset(out,0,sizeof *out);
 out->version=NETWORK_HOST_SNAPSHOT_VERSION;
 out->size=(uint32_t)sizeof *out;
 int rc=bridge.snapshot(bridge.ctx,out);
 if(rc!=0){
  network_snapshot_failure="callback_failed";
  snapshot_fail("callback_failed",rc,out,NULL);
  memset(out,0,sizeof *out);
  return false;
 }
 if(out->version!=NETWORK_HOST_SNAPSHOT_VERSION){
  network_snapshot_failure="version_mismatch";
  snapshot_fail("version_mismatch",rc,out,NULL);
  memset(out,0,sizeof *out);
  return false;
 }
 if(out->size!=(uint32_t)sizeof *out){
  network_snapshot_failure="size_mismatch";
  snapshot_fail("size_mismatch",rc,out,NULL);
  memset(out,0,sizeof *out);
  return false;
 }
 const char *invalid=snapshot_invalid_field(out);
 if(invalid){
  network_snapshot_failure="invalid_field";
  snapshot_fail("invalid_field",rc,out,invalid);
  memset(out,0,sizeof *out);
  return false;
 }
 return true;
}

static const char *network_snapshot_reason(const network_host_snapshot *s,bool valid){
 return !valid?(network_snapshot_failure?network_snapshot_failure:"connectivity_unobservable"):
  !s->has_default_network?"no_default_network":"ok";
}

static bool unavailable(struct dvm *v,const char *api){
 sdk_trace_fprintf(stderr,"NETWORK_HOST_UNAVAILABLE api=%s\n",api);
 if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException",
  "%s: HarmonyOS network host bridge unavailable",api);
 return true;
}

static bool connectivity_snapshot_ok(struct dvm *v,network_host_snapshot *snap,
 const char *api){
 /* A Unity reachability query fails closed without leaving a pending JNI
  * exception. Reuse one coherent host observation throughout its constructor. */
 if(network_query_scope){
  *snap=network_query_snapshot;
  return network_query_valid;
 }
 /* unavailable() raises the guest-visible exception and returns true (the
  * call was handled).  The caller must stop here, so this helper returns
  * false; returning unavailable()'s true would let the caller continue with
  * a cleared snapshot and write a bogus result into out. */
 if(!host_snapshot(snap)||!snap->connectivity_valid){
  (void)unavailable(v,api);
  return false;
 }
 return true;
}

static bool telephony_snapshot_ok(struct dvm *v,network_host_snapshot *snap,
 const char *api){
 if(!host_snapshot(snap)||!snap->telephony_valid){
  (void)unavailable(v,api);
  return false;
 }
 return true;
}

static bool transport_bit(uint32_t mask,int32_t bit){
 return bit>=0&&bit<32&&(mask&(1u<<(uint32_t)bit))!=0;
}

static int32_t snapshot_type(const network_host_snapshot *snap){
 if(snap->type_valid)return snap->type;
 if(transport_bit(snap->transports,1))return 1; /* TYPE_WIFI */
 if(transport_bit(snap->transports,0))return 0; /* TYPE_MOBILE */
 if(transport_bit(snap->transports,3))return 9; /* TYPE_ETHERNET */
 return -1; /* TYPE_NONE */
}

static bool string_ref(struct dvm *v,const char *field,size_t capacity,
 const char *what,union dvm_value *out){
 const char *end=memchr(field,0,capacity);
 if(!end){
  dvm__throw(v,"java/lang/IllegalStateException",
   "%s: host string is not NUL-terminated",what);
  return false;
 }
 out->l=dvm_new_string_n(v,field,(size_t)(end-field));
 if(!out->l){
  oom(v,what);
  return false;
 }
 return true;
}

static dvm_ref new_plain_object(struct dvm *v,const char *desc,
 const char *what){
 struct dvm_class *cls=dvm_find_class_or_external(v,desc);
 if(!cls){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/NoClassDefFoundError","%s",desc);
  return 0;
 }
 dvm_ref ref=dvm_new_object(v,cls);
 if(!ref&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","%s",what);
 return ref;
}

static dvm_ref manager_object(struct dvm *v,const char *desc,const char *what,
 dvm_ref *slot){
 if(*slot)return *slot;
 dvm_ref ref=new_plain_object(v,desc,what);
 if(!ref)return 0;
 *slot=ref;
 return ref;
}

static dvm_ref attach_payload(struct dvm *v,const char *desc,void *payload,
 void (*destroy)(void*),const char *what){
 struct dvm_class *cls=dvm_find_class_or_external(v,desc);
 if(!cls){
  destroy(payload);
  if(!dvm_exception(v))dvm__throw(v,"java/lang/NoClassDefFoundError","%s",desc);
  return 0;
 }
 dvm_ref ref=dvm_new_object(v,cls);
 struct dvm_object *obj=ref?dvm__obj(v,ref):NULL;
 if(!obj){
  destroy(payload);
  if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","%s",what);
  return 0;
 }
 obj->data=payload;
 obj->data_destroy=destroy;
 return ref;
}

static dvm_ref network_info_new(struct dvm *v,const network_host_snapshot *snap){
 network_info_payload *payload=calloc(1,sizeof *payload);
 if(!payload){
  oom(v,"NetworkInfo payload");
  return 0;
 }
 payload->magic=NETWORK_INFO_MAGIC;
 payload->type=snapshot_type(snap);
 payload->connected=snap->connected!=0;
 /* Android never reports connected-but-unavailable; if the host only fills
  * connected, isAvailable() must still agree with it. */
 payload->available=(snap->available!=0)||payload->connected;
 payload->state=payload->connected?NETWORK_STATE_CONNECTED:NETWORK_STATE_DISCONNECTED;
 payload->detailed=payload->connected?NETWORK_DETAILED_CONNECTED:NETWORK_DETAILED_DISCONNECTED;
 /* Copy the host's real telephony network type only when the host says the
  * telephony family is observable and that field is valid.  Otherwise the
  * subtype is Android's NETWORK_TYPE_UNKNOWN (0); no guessed LTE/GSM value. */
 payload->subtype=(snap->telephony_valid&&snap->network_type_valid)
  ?snap->network_type:0;
 dvm_ref ref=attach_payload(v,"android/net/NetworkInfo",payload,
  network_info_destroy,"NetworkInfo object");
 return ref;
}

static dvm_ref network_obj_new(struct dvm *v,uint64_t handle){
 network_obj_payload *payload=calloc(1,sizeof *payload);
 if(!payload){
  oom(v,"Network payload");
  return 0;
 }
 payload->magic=NETWORK_OBJ_MAGIC;
 payload->handle=handle;
 return attach_payload(v,"android/net/Network",payload,network_obj_destroy,
  "Network object");
}

static dvm_ref network_caps_new(struct dvm *v,const network_host_snapshot *snap){
 network_caps_payload *payload=calloc(1,sizeof *payload);
 if(!payload){
  oom(v,"NetworkCapabilities payload");
  return 0;
 }
 payload->magic=NETWORK_CAPS_MAGIC;
 payload->transports=snap->transports;
 payload->capabilities=snap->capabilities;
 return attach_payload(v,"android/net/NetworkCapabilities",payload,
  network_caps_destroy,"NetworkCapabilities object");
}

static bool network_info_copy(struct dvm *v,dvm_ref self,network_info_payload *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,self))return false;
 struct dvm_object *obj=dvm__obj(v,self);
 if(!obj||obj->data_destroy!=network_info_destroy)return false;
 const network_info_payload *payload=obj->data;
 if(!payload||payload->magic!=NETWORK_INFO_MAGIC)return false;
 *out=*payload;return true;
}

static bool network_obj_copy(struct dvm *v,dvm_ref self,network_obj_payload *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,self))return false;
 struct dvm_object *obj=dvm__obj(v,self);
 if(!obj||obj->data_destroy!=network_obj_destroy)return false;
 const network_obj_payload *payload=obj->data;
 if(!payload||payload->magic!=NETWORK_OBJ_MAGIC)return false;
 *out=*payload;return true;
}

static bool network_caps_copy(struct dvm *v,dvm_ref self,network_caps_payload *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,self))return false;
 struct dvm_object *obj=dvm__obj(v,self);
 if(!obj||obj->data_destroy!=network_caps_destroy)return false;
 const network_caps_payload *payload=obj->data;
 if(!payload||payload->magic!=NETWORK_CAPS_MAGIC)return false;
 *out=*payload;return true;
}

static bool network_enum_copy(struct dvm *v,dvm_ref self,network_enum_payload *out){
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,self))return false;
 struct dvm_object *obj=dvm__obj(v,self);
 if(!obj||obj->data_destroy!=network_enum_destroy)return false;
 const network_enum_payload *payload=obj->data;
 if(!payload||payload->magic!=NETWORK_ENUM_MAGIC)return false;
 *out=*payload;return true;
}

static dvm_ref enum_value(struct dvm *v,const char *desc,const char *name,
 int32_t ordinal,dvm_ref *cache){
 if(*cache)return *cache;
 network_enum_payload *payload=calloc(1,sizeof *payload);
 if(!payload){
  oom(v,"NetworkInfo enum payload");
  return 0;
 }
 payload->magic=NETWORK_ENUM_MAGIC;
 payload->name=name;
 payload->ordinal=ordinal;
 dvm_ref ref=attach_payload(v,desc,payload,network_enum_destroy,
  "NetworkInfo enum object");
 if(!ref)return 0;
 *cache=ref;
 return ref;
}

static dvm_ref state_value(struct dvm *v,int32_t state){
 if(state<0||state>=NETWORK_STATE_COUNT)state=NETWORK_STATE_UNKNOWN;
 return enum_value(v,"android/net/NetworkInfo$State",
  network_state_names[state],state,&network_states[state]);
}

static dvm_ref detailed_state_value(struct dvm *v,int32_t state){
 if(state<0||state>=NETWORK_DETAILED_COUNT)state=NETWORK_DETAILED_IDLE;
 return enum_value(v,"android/net/NetworkInfo$DetailedState",
  network_detailed_names[state],state,&network_detailed_states[state]);
}

static dvm_ref type_name_string(struct dvm *v,int32_t type){
 const char *name;
 switch(type){
  case -1: name="NONE"; break;
  case 0: name="MOBILE"; break;
  case 1: name="WIFI"; break;
  case 2: name="MOBILE_MMS"; break;
  case 3: name="MOBILE_SUPL"; break;
  case 4: name="MOBILE_DUN"; break;
  case 5: name="MOBILE_HIPRI"; break;
  case 6: name="WIMAX"; break;
  case 7: name="BLUETOOTH"; break;
  case 8: name="DUMMY"; break;
  case 9: name="ETHERNET"; break;
  case 10: name="MOBILE_FOTA"; break;
  case 11: name="MOBILE_IMS"; break;
  case 12: name="MOBILE_CBS"; break;
  case 13: name="WIFI_P2P"; break;
  case 14: name="MOBILE_IA"; break;
  case 15: name="MOBILE_EMERGENCY"; break;
  case 16: name="PROXY"; break;
  case 17: name="VPN"; break;
  default: name=NULL; break;
 }
 char buffer[16];
 if(!name){
  snprintf(buffer,sizeof buffer,"%d",(int)type);
  name=buffer;
 }
 dvm_ref ref=dvm_new_string(v,name);
 if(!ref)oom(v,"NetworkInfo.getTypeName");
 return ref;
}

/* Exact android.telephony.TelephonyManager.getNetworkTypeName(int) strings.
 * NetworkInfo.getSubtypeName() exposes the name for the stored subtype; any
 * value outside NETWORK_TYPE_* (including NETWORK_TYPE_UNKNOWN and a missing
 * telephony observation) is Android's "UNKNOWN", never a fabricated bearer. */
static const char *telephony_network_type_name(int32_t type){
 switch(type){
  case 1: return "GPRS";
  case 2: return "EDGE";
  case 3: return "UMTS";
  case 4: return "CDMA";
  case 5: return "CDMA - EvDo rev. 0";
  case 6: return "CDMA - EvDo rev. A";
  case 7: return "CDMA - 1xRTT";
  case 8: return "HSDPA";
  case 9: return "HSUPA";
  case 10: return "HSPA";
  case 11: return "iDEN";
  case 12: return "CDMA - EvDo rev. B";
  case 13: return "LTE";
  case 14: return "CDMA - eHRPD";
  case 15: return "HSPA+";
  case 16: return "GSM";
  case 17: return "TD-SCDMA";
  case 18: return "IWLAN";
  case 19: return "LTE_CA";
  case 20: return "NR";
  default: return "UNKNOWN";
 }
}

static bool telephony_string(struct dvm *v,const char *api,int32_t valid,
 const char *field,size_t capacity,union dvm_value *out){
 if(!valid){
  out->l=dvm_new_string(v,"");
  if(!out->l)oom(v,api);
  return true;
 }
 string_ref(v,field,capacity,api,out);
 return true;
}

static bool enum_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,
 int n,union dvm_value *out){
 network_enum_payload value;
 if(!network_enum_copy(v,self,&value))return false;
 const network_enum_payload *payload=&value;
 if((IS("name","()Ljava/lang/String;")||IS("toString","()Ljava/lang/String;"))
  &&n==0){
  out->l=dvm_new_string(v,payload->name);
  if(!out->l)oom(v,"NetworkInfo enum name");
  return true;
 }
 if(IS("ordinal","()I")&&n==0){
  out->i=payload->ordinal;
  return true;
 }
 return false;
}

#include "notification_services.inc"

static bool context_call(struct dvm *v,const char *c,const char *m,
 const char *sig,dvm_ref self,const union dvm_value *a,int n,
 union dvm_value *out){
 if(strcmp(c,"android/content/Context"))return false;
 if(!IS("getSystemService","(Ljava/lang/String;)Ljava/lang/Object;")||n!=1)
  return false;
 if(!self){
  dvm__throw(v,"java/lang/NullPointerException","Context receiver");
  return true;
 }
 if(!a[0].l){
  dvm__throw(v,"java/lang/NullPointerException","Context.getSystemService name");
  return true;
 }
 const char *name=dvm_string_utf8(v,a[0].l);
 if(!name){
  dvm__throw(v,"java/lang/ClassCastException",
   "Context.getSystemService name is not a String");
  return true;
 }
 /* Reject embedded NULs; they cannot name one of the services below. */
 if(strlen(name)!=(size_t)dvm_string_utf8_length(v,a[0].l)){out->l=0;return true;}
 /* Bugly (com/tencent/bugly/proguard/ay.<init>) asks for Context.ACTIVITY_SERVICE.
  * The manager object is real; its methods still fail closed until a host
  * backend exists, so this does not fabricate any activity state. */
 if(!strcmp(name,"notification")){out->l=manager_object(v,"android/app/NotificationManager","NotificationManager",&notification_manager);return true;}
 if(!strcmp(name,"vibrator")){out->l=manager_object(v,"android/os/Vibrator","Vibrator",&vibrator_manager);return true;}
 if(!strcmp(name,"activity")){
  out->l=manager_object(v,"android/app/ActivityManager",
   "ActivityManager object",&activity_manager);
  return out->l!=0||dvm_exception(v);
 }
 if(!strcmp(name,"connectivity")){
  out->l=manager_object(v,"android/net/ConnectivityManager",
   "ConnectivityManager object",&connectivity_manager);
  return out->l!=0||dvm_exception(v);
 }
 if(!strcmp(name,"phone")){
  out->l=manager_object(v,"android/telephony/TelephonyManager",
   "TelephonyManager object",&telephony_manager);
  return out->l!=0||dvm_exception(v);
 }
 /* UnityPlayer stores (ClipboardManager) getSystemService("clipboard").
  * The manager object is real; getPrimaryClip/setPrimaryClip still need a
  * host pasteboard backend and stay missing until implemented. */
 if(!strcmp(name,"clipboard")){
  out->l=manager_object(v,"android/content/ClipboardManager",
   "ClipboardManager object",&clipboard_manager);
  return out->l!=0||dvm_exception(v);
 }
 /* HFPStatus.<init> stores (AudioManager) getSystemService("audio") before
  * initHFPStatusJni().  The manager object is real; its methods (startBluetoothSco,
  * stopBluetoothSco, setMode) still fail closed until a host audio backend exists. */
 if(!strcmp(name,"audio")){
  out->l=manager_object(v,"android/media/AudioManager",
   "AudioManager object",&audio_manager);
  return out->l!=0||dvm_exception(v);
 }
 /* UnityPlayer.-$$Nest$mnativeRender 解析 Context.MEDIA_ROUTER_SERVICE
  * （"media_router"）；管理器对象真实存在，路由方法在后端可用前 fail closed。 */
  if(!strcmp(name,"media_router")){
   out->l=manager_object(v,"android/media/MediaRouter",
    "MediaRouter object",&media_router_manager);
   return out->l!=0||dvm_exception(v);
  }
  /* UnityPlayer.-$$Nest$mnativeRender 解析 Context.INPUT_SERVICE（"input"）
   * （72pr 实测 SYSTEM_SERVICE_UNHANDLED name=input 留 pending=688 的
   * NoSuchMethodError）；管理器对象真实存在，输入方法在后端可用前
   * fail closed。 */
  if(!strcmp(name,"input")){
   out->l=manager_object(v,"android/hardware/input/InputManager",
    "InputManager object",&input_manager);
   return out->l!=0||dvm_exception(v);
  }
 /* activity_runtime owns the WindowManager identity and its display backend.
  * Continue dispatch so getSystemService and getWindowManager share it. */
 if(!strcmp(name,"window")||!strcmp(name,"layout_inflater")){
  return false;
 }
 if(!strcmp(name,"location")||!strcmp(name,"display")){
  /* This Context has no host adapter for these services. Context returns null when
   * the requested service is unavailable. */
  sdk_trace_fprintf(stderr,"SYSTEM_SERVICE_UNAVAILABLE name=%s\n",name);
  out->l=0;
  return true;
 }
 /* Unsupported services are absent, not missing Java methods. */
 sdk_trace_fprintf(stderr,"SYSTEM_SERVICE_UNAVAILABLE name=%s\n",name);
 out->l=0;
 return true;
}

static bool audio_call(struct dvm *v,const char *c,const char *m,const char *sig,
 dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(strcmp(c,"android/media/AudioManager"))return false;
 if(!strcmp(m,"getStreamVolume")&&!strcmp(sig,"(I)I")&&n==1){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","AudioManager receiver");return true;}
  pthread_mutex_lock(&host_lock);audio_volume_host_fn callback=audio_volume_host;void *user=audio_volume_user;pthread_mutex_unlock(&host_lock);
  if(!callback){
   sdk_trace_fprintf(stderr,"AUDIO_VOLUME host=absent stream=%d result=unsupported\n",a[0].i);
   dvm__throw(v,"java/lang/UnsupportedOperationException","AudioManager host unavailable");return true;
  }
  int volume=-1;
  unsigned depth=dvm_gil_unlock_all(v);int status=callback(user,a[0].i,&volume);dvm_gil_relock(v,depth);
  if(status!=0||volume<0){
   sdk_trace_fprintf(stderr,"AUDIO_VOLUME host=reject stream=%d status=%d volume=%d result=unsupported\n",a[0].i,status,volume);
   dvm__throw(v,"java/lang/UnsupportedOperationException","AudioManager host rejected the volume query");return true;
  }
  sdk_trace_fprintf(stderr,"AUDIO_VOLUME stream=%d volume=%d source=host\n",a[0].i,volume);
  out->i=volume;return true;
 }
 return false;
}

extern void perf_diag_connectivity(int,int,const char *) __attribute__((weak));
extern void perf_diag_connectivity_detail(const char *,const char *) __attribute__((weak));
static _Thread_local unsigned network_detail_count;
static void network_detail(const char *stage,const char *detail){
 /* A scope has at most 24 metadata rows, including the failure stack. The
  * existing worker queue owns all formatting-to-file/rotation/permissions. */
 if(!perf_diag_connectivity_detail||network_detail_count>=24)return;
 network_detail_count++;
 perf_diag_connectivity_detail(stage,detail);
}
static void network_symbol(char *out,size_t size,const char *text){
 size_t n=0;if(!text)text="none";
 for(;text[n]&&n+1<size;n++){
  unsigned char c=(unsigned char)text[n];
  out[n]=(isalnum(c)||strchr("_$/;.<>()[]-",c))?(char)c:'_';
 }
 out[n]=0;
}
static bool network_lookup_text(const char *text){
 if(!text)return false;
 if(strncmp(text,"android/",8)&&strncmp(text,"java/",5)&&strncmp(text,"com/unity3d/player/",19))return false;
 for(size_t i=0;text[i];i++){
  unsigned char c=(unsigned char)text[i];
  if(i>=255||(!isalnum(c)&&!strchr("_$/;.<>()[]-",c)))return false;
 }
 return true;
}
void network_runtime_query_failure(struct dvm *v,const char *stage){
 if(!v||!perf_diag_connectivity_detail)return;
 int saved_errno=errno;
 dvm_ref ex=dvm_exception(v);
 struct dvm_object *o=ex?dvm__obj(v,ex):NULL;
 char cls[160],msg[256]="redacted",detail[768];
 network_symbol(cls,sizeof cls,o&&o->cls?o->cls->name:NULL);
 /* dvm__throw keeps an always-readable native message in utf8. Restrict
  * message capture to code-symbol lookup errors with platform/Unity names;
  * never call Throwable.toString/getMessage or stringify arbitrary objects. */
 const char *text=o?o->utf8:NULL;
 bool lookup=o&&o->cls&&(!strcmp(o->cls->name,"java/lang/NoSuchMethodError")||
  !strcmp(o->cls->name,"java/lang/NoClassDefFoundError")||
  !strcmp(o->cls->name,"java/lang/NoSuchFieldError"));
 if(lookup&&network_lookup_text(text))network_symbol(msg,sizeof msg,text);
 snprintf(detail,sizeof detail,"exception_class=%s message=%s has_exception=%d trace_count=%d pc_bytes=%u fallback_internal=%d",
  cls,msg,ex!=0,v->nexc_trace,v->exc_pc*2,!ex);
 network_detail(stage,detail);
 for(int i=0;i<v->nexc_trace&&i<8;i++){
  const struct dvm_method *m=v->exc_trace[i];
  if(!m)continue;
  char owner[160],method[96],sig[192];
  network_symbol(owner,sizeof owner,m->cls?m->cls->name:NULL);
  network_symbol(method,sizeof method,m->name);network_symbol(sig,sizeof sig,m->sig);
  snprintf(detail,sizeof detail,"depth=%d owner=%s method=%s sig=%s pc_bytes=%u",i,owner,method,sig,i?0:v->exc_pc*2);
  network_detail("guest_stack",detail);
 }
 errno=saved_errno;
}
static int network_reachability(const network_host_snapshot *s,bool valid){
 return valid&&s->has_default_network?((s->transports&1u)?1:2):0;
}
static void network_status(const network_host_snapshot *s,bool valid,const char *reason){
 int registered=0;
 for(unsigned i=0;i<NETWORK_CALLBACKS;i++)registered+=network_callbacks[i].callback!=0;
 if(perf_diag_connectivity)perf_diag_connectivity(network_reachability(s,valid),registered,reason);
}
static bool network_event(struct dvm *v,struct network_registration *r,
 const char *name,const char *sig,const union dvm_value *args,int n){
 struct dvm_class *cls=dvm_object_class(v,r->callback);
 struct dvm_method *method=cls?dvm_find_method(v,cls,name,sig):NULL;
 /* Inherited Android defaults are no-ops. Guest overrides run unchanged. */
 if(!method)return true;
 union dvm_value out={0};
 uint64_t generation=r->generation;
 bool ok=dvm_call(v,method,r->callback,args,n,&out)&&!dvm_exception(v);
 if(!ok){network_detail("callback_failed",name);network_runtime_query_failure(v,"callback_exception");}
 else network_detail("callback_completed",name);
 if(!ok&&r->generation==generation)r->initialized=false; /* retry, never latch a failed delivery */
 return ok;
}
static void network_deliver(struct dvm *v,const network_host_snapshot *s,bool valid){
 if(network_delivering||dvm_exception(v))return;
 network_delivering=true;
 bool online=valid&&s->has_default_network;
 for(unsigned i=0;i<NETWORK_CALLBACKS&&!dvm_exception(v);i++){
  struct network_registration *r=&network_callbacks[i];
  if(!r->callback)continue;
  uint64_t generation=r->generation;
  bool was_online=r->online,initial=!r->initialized;
  bool replaced=was_online&&online&&r->handle!=s->network_handle;
  bool changed=initial||was_online!=online||replaced||
   (online&&(r->transports!=s->transports||r->capabilities!=s->capabilities));
  if(!changed)continue;
  r->initialized=true;r->online=online;
  union dvm_value a[2]={{.l=r->network},{0}};
  if(was_online&&(!online||replaced)){
   if(!network_event(v,r,"onLost","(Landroid/net/Network;)V",a,1))break;
   if(r->generation!=generation||!r->callback)continue;
  }
  if(!online){
   r->network=r->caps=0;
   if(initial&&!network_event(v,r,"onUnavailable","()V",NULL,0))break;
   continue;
  }
  r->handle=s->network_handle;r->transports=s->transports;r->capabilities=s->capabilities;
  if(!was_online||replaced){r->network=network_obj_new(v,s->network_handle);if(!r->network){r->initialized=false;break;}}
  r->caps=network_caps_new(v,s);if(!r->caps){r->initialized=false;break;}
  a[0].l=r->network;a[1].l=r->caps;
  if(!was_online||replaced){
   if(!network_event(v,r,"onAvailable","(Landroid/net/Network;)V",a,1))break;
   if(r->generation!=generation||!r->callback)continue;
  }
  if(!network_event(v,r,"onCapabilitiesChanged","(Landroid/net/Network;Landroid/net/NetworkCapabilities;)V",a,2))break;
 }
 network_delivering=false;
}
/* Called under JNI's GIL, before the original Unity getter. There is currently
 * no push channel from ArkTS: refresh on registration and each JNI query only.
 * The existing host RPC has its existing timeout; never poll per frame. */
bool network_runtime_query_begin(struct dvm *v,const char *cls,const char *name,const char *sig){
 if(strcmp(cls,"com/unity3d/player/UnityPlayer")||strcmp(name,"getNetworkConnectivity")||strcmp(sig,"()I"))return false;
 if(network_query_scope){network_query_scope++;return true;}
 network_detail_count=0;
 if(dvm_exception(v)){network_runtime_query_failure(v,"query_begin_pending");return false;}
 activate(v);
 if(!network_gc_ensure(v)){network_runtime_query_failure(v,"query_begin_roots_failed");return false;}
 memset(&network_query_snapshot,0,sizeof network_query_snapshot);
 network_query_valid=host_snapshot(&network_query_snapshot)&&network_query_snapshot.connectivity_valid;
 network_query_reason=network_snapshot_reason(&network_query_snapshot,network_query_valid);
 network_last_snapshot=network_query_snapshot;network_last_valid=network_query_valid;
 network_query_scope=1;
 char detail[160];snprintf(detail,sizeof detail,"valid=%d default_network=%d transports=%u reason=%s",network_query_valid,
  network_query_snapshot.has_default_network,network_query_snapshot.transports,network_query_reason?network_query_reason:"none");
 network_detail("query_begin",detail);
 network_deliver(v,&network_query_snapshot,network_query_valid);
 return true;
}
void network_runtime_query_end(int result,bool completed){
 if(network_query_scope&&!--network_query_scope){
  int registered=0;for(unsigned i=0;i<NETWORK_CALLBACKS;i++)registered+=network_callbacks[i].callback!=0;
  if(perf_diag_connectivity)perf_diag_connectivity(completed?result:0,registered,
   !completed?"guest_query_failed":network_query_reason);
 }
}

static bool connectivity_call(struct dvm *v,const char *c,const char *m,
 const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 if(!strcmp(c,"android/net/ConnectivityManager$NetworkCallback")){
  if((IS("<init>","()V")&&n==0)||(IS("onUnavailable","()V")&&n==0)||
   ((IS("onAvailable","(Landroid/net/Network;)V")||IS("onLost","(Landroid/net/Network;)V"))&&n==1)||
   (IS("onCapabilitiesChanged","(Landroid/net/Network;Landroid/net/NetworkCapabilities;)V")&&n==2))return true;
  return false;
 }
 if(strcmp(c,"android/net/ConnectivityManager"))return false;
 network_host_snapshot snap;
 memset(out,0,sizeof *out);

 if((IS("registerDefaultNetworkCallback","(Landroid/net/ConnectivityManager$NetworkCallback;)V")||
     IS("unregisterNetworkCallback","(Landroid/net/ConnectivityManager$NetworkCallback;)V"))&&n==1){
  bool registration=m[0]=='r';int slot=-1,empty=-1;
  if(!network_query_scope)network_detail_count=0;
  network_detail(registration?"register_enter":"unregister_enter","api=ConnectivityManager");
  if(!a[0].l){network_status(&network_last_snapshot,network_last_valid,"null_callback");dvm__throw(v,"java/lang/NullPointerException","NetworkCallback");return true;}
  for(unsigned i=0;i<NETWORK_CALLBACKS;i++){
   if(network_callbacks[i].callback==a[0].l)slot=(int)i;
   if(!network_callbacks[i].callback&&empty<0)empty=(int)i;
  }
  if(!registration){
   if(slot>=0){memset(&network_callbacks[slot],0,sizeof network_callbacks[slot]);network_callbacks[slot].generation=++network_generation;}
   network_status(&network_last_snapshot,network_last_valid,"unregistered");
   return true;
  }
  if(slot>=0){network_status(&network_last_snapshot,network_last_valid,"already_registered");return true;}
  if(empty<0){network_status(&network_last_snapshot,network_last_valid,"callback_capacity");dvm__throw(v,"java/lang/IllegalStateException","NetworkCallback capacity=16");return true;}
  struct network_registration *r=&network_callbacks[empty];
  memset(r,0,sizeof *r);r->callback=a[0].l;r->generation=++network_generation;
  network_detail("register_slot","allocated=1");
  uint64_t generation=r->generation;
  memset(&snap,0,sizeof snap);
  bool valid;
  if(network_query_scope){snap=network_query_snapshot;valid=network_query_valid;}
  else valid=host_snapshot(&snap)&&snap.connectivity_valid;
  const char *reason=network_query_scope?network_query_reason:network_snapshot_reason(&snap,valid);
  network_last_snapshot=snap;network_last_valid=valid;
  network_deliver(v,&snap,valid);
  if(dvm_exception(v)&&r->generation==generation){
   memset(r,0,sizeof *r);r->generation=++network_generation;reason="callback_exception";
   network_detail("register_rollback","reason=callback_exception");
  }
  network_status(&snap,valid,reason);
  network_detail("register_end",reason?reason:"none");
  return true;
 }

 if(IS("getActiveNetworkInfo","()Landroid/net/NetworkInfo;")&&n==0){
  if(network_query_scope)network_detail("manager_getActiveNetworkInfo","entered=1");
  if(!connectivity_snapshot_ok(v,&snap,
   "ConnectivityManager.getActiveNetworkInfo"))return true;
  out->l=0;
  if(snap.has_default_network)out->l=network_info_new(v,&snap);
  return true;
 }
 if(IS("getNetworkInfo","(I)Landroid/net/NetworkInfo;")&&n==1){
  if(!connectivity_snapshot_ok(v,&snap,"ConnectivityManager.getNetworkInfo"))
   return true;
  out->l=0;
  if(!snap.has_default_network)return true;
  if(a[0].i!=snapshot_type(&snap))return true;
  out->l=network_info_new(v,&snap);
  return true;
 }
 if(IS("isActiveNetworkMetered","()Z")&&n==0){
  if(!connectivity_snapshot_ok(v,&snap,
   "ConnectivityManager.isActiveNetworkMetered"))return true;
  out->i=(!snap.has_default_network||snap.metered)?1:0;
  return true;
 }
 if(IS("getActiveNetwork","()Landroid/net/Network;")&&n==0){
  if(network_query_scope)network_detail("manager_getActiveNetwork","entered=1");
  if(!connectivity_snapshot_ok(v,&snap,
   "ConnectivityManager.getActiveNetwork"))return true;
  out->l=0;
  if(snap.has_default_network)out->l=network_obj_new(v,snap.network_handle);
  return true;
 }
 if(IS("getNetworkCapabilities",
  "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;")&&n==1){
  if(network_query_scope)network_detail("manager_getNetworkCapabilities","entered=1");
  if(!connectivity_snapshot_ok(v,&snap,
   "ConnectivityManager.getNetworkCapabilities"))return true;
  out->l=0;
  if(!snap.has_default_network)return true;
  network_obj_payload network;
  /* The host snapshot callback may release/reacquire the GIL. Resolve AFTER it. */
  if(!network_obj_copy(v,a[0].l,&network)||network.handle!=snap.network_handle)return true;
  out->l=network_caps_new(v,&snap);
  return true;
 }
 return false;
}

static bool network_info_call(struct dvm *v,const char *c,const char *m,
 const char *sig,dvm_ref self,int n,union dvm_value *out){
 if(strcmp(c,"android/net/NetworkInfo"))return false;
 network_info_payload value;
 if(!network_info_copy(v,self,&value))return false;
 const network_info_payload *payload=&value;

 if(IS("isConnected","()Z")&&n==0){
  out->i=payload->connected?1:0;
  return true;
 }
 if(IS("isConnectedOrConnecting","()Z")&&n==0){
  out->i=(payload->connected||payload->state==NETWORK_STATE_CONNECTING)?1:0;
  return true;
 }
 if(IS("isAvailable","()Z")&&n==0){
  out->i=payload->available?1:0;
  return true;
 }
 if(IS("getType","()I")&&n==0){
  out->i=payload->type;
  return true;
 }
 if(IS("getTypeName","()Ljava/lang/String;")&&n==0){
  out->l=type_name_string(v,payload->type);
  return true;
 }
 if(IS("getSubtype","()I")&&n==0){
  /* NetworkInfo.getSubtype() 返回 TelephonyManager 观测到的 networkType。
   * network_info_new 只在 telephony_valid && network_type_valid 时复制该
   * 字段，否则保持 0 (NETWORK_TYPE_UNKNOWN)；这里不得根据连接状态或
   * getSubtypeName() 反推 LTE。getSubtype 是纯读取，不改变 connected。 */
  out->i=payload->subtype;
  return true;
 }
 if(IS("getSubtypeName","()Ljava/lang/String;")&&n==0){
  out->l=dvm_new_string(v,telephony_network_type_name(payload->subtype));
  if(!out->l)oom(v,"NetworkInfo.getSubtypeName");
  return true;
 }
 if(IS("getState","()Landroid/net/NetworkInfo$State;")&&n==0){
  out->l=state_value(v,payload->state);
  return out->l!=0||dvm_exception(v);
 }
 if(IS("getDetailedState","()Landroid/net/NetworkInfo$DetailedState;")&&n==0){
  out->l=detailed_state_value(v,payload->detailed);
  return out->l!=0||dvm_exception(v);
 }
 return false;
}

static bool network_call(struct dvm *v,const char *c,const char *m,
 const char *sig,dvm_ref self,int n,union dvm_value *out){
 if(strcmp(c,"android/net/Network"))return false;
 network_obj_payload value;
 if(!network_obj_copy(v,self,&value))return false;
 const network_obj_payload *payload=&value;
 if(IS("getNetworkHandle","()J")&&n==0){
  out->j=(int64_t)payload->handle;
  return true;
 }
 return false;
}

static bool network_caps_call(struct dvm *v,const char *c,const char *m,
 const char *sig,dvm_ref self,int n,const union dvm_value *a,
 union dvm_value *out){
 if(strcmp(c,"android/net/NetworkCapabilities"))return false;
 network_caps_payload value;
 if(!network_caps_copy(v,self,&value))return false;
 const network_caps_payload *payload=&value;
 if(IS("hasTransport","(I)Z")&&n==1){
  out->i=transport_bit(payload->transports,a[0].i)?1:0;
  return true;
 }
 if(IS("hasCapability","(I)Z")&&n==1){
  out->i=transport_bit(payload->capabilities,a[0].i)?1:0;
  return true;
 }
 return false;
}

static bool telephony_call(struct dvm *v,const char *c,const char *m,
 const char *sig,int n,union dvm_value *out){
 if(strcmp(c,"android/telephony/TelephonyManager"))return false;
 network_host_snapshot snap;

 if(IS("getNetworkType","()I")&&n==0){
  if(!telephony_snapshot_ok(v,&snap,"TelephonyManager.getNetworkType"))
   return true;
  out->i=snap.network_type_valid?snap.network_type:0;
  return true;
 }
 if(IS("getNetworkOperator","()Ljava/lang/String;")&&n==0){
  if(!telephony_snapshot_ok(v,&snap,"TelephonyManager.getNetworkOperator"))
   return true;
  return telephony_string(v,"TelephonyManager.getNetworkOperator",
   snap.operator_numeric_valid,snap.operator_numeric,
   sizeof snap.operator_numeric,out);
 }
 if(IS("getNetworkOperatorName","()Ljava/lang/String;")&&n==0){
  if(!telephony_snapshot_ok(v,&snap,
   "TelephonyManager.getNetworkOperatorName"))return true;
  return telephony_string(v,"TelephonyManager.getNetworkOperatorName",
   snap.operator_name_valid,snap.operator_name,sizeof snap.operator_name,out);
 }
 if(IS("getSimState","()I")&&n==0){
  if(!telephony_snapshot_ok(v,&snap,"TelephonyManager.getSimState"))return true;
  out->i=snap.sim_state_valid?snap.sim_state:0;
  return true;
 }
 if(IS("getPhoneType","()I")&&n==0){
  if(!telephony_snapshot_ok(v,&snap,"TelephonyManager.getPhoneType"))
   return true;
  out->i=snap.phone_type_valid?snap.phone_type:0;
  return true;
 }
 return false;
}

bool network_runtime_call(struct dvm *v,const char *c,const char *m,
 const char *sig,dvm_ref self,const union dvm_value *a,int n,
 union dvm_value *out){
 if(!v||!c||!m||!sig||!out)return false;
 if(n>0&&!a)return false;
 activate(v);
 if(!network_gc_ensure(v))return false;
 if(enum_call(v,m,sig,self,n,out))return true;
 if(notification_services_call(v,c,m,sig,self,a,n,out))return true;
 if(context_call(v,c,m,sig,self,a,n,out))return true;
 if(audio_call(v,c,m,sig,self,a,n,out))return true;
 if(connectivity_call(v,c,m,sig,a,n,out))return true;
 if(network_info_call(v,c,m,sig,self,n,out))return true;
 if(network_call(v,c,m,sig,self,n,out))return true;
 if(network_caps_call(v,c,m,sig,self,n,a,out))return true;
 if(telephony_call(v,c,m,sig,n,out))return true;
 return false;
}

static const network_int_constant connectivity_constants[] = {
 {"TYPE_NONE",-1},
 {"TYPE_MOBILE",0},
 {"TYPE_WIFI",1},
 {"TYPE_MOBILE_MMS",2},
 {"TYPE_MOBILE_SUPL",3},
 {"TYPE_MOBILE_DUN",4},
 {"TYPE_MOBILE_HIPRI",5},
 {"TYPE_WIMAX",6},
 {"TYPE_BLUETOOTH",7},
 {"TYPE_DUMMY",8},
 {"TYPE_ETHERNET",9},
 {"TYPE_MOBILE_FOTA",10},
 {"TYPE_MOBILE_IMS",11},
 {"TYPE_MOBILE_CBS",12},
 {"TYPE_WIFI_P2P",13},
 {"TYPE_MOBILE_IA",14},
 {"TYPE_MOBILE_EMERGENCY",15},
 {"TYPE_PROXY",16},
 {"TYPE_VPN",17}
};

static const network_int_constant network_caps_constants[] = {
 {"TRANSPORT_CELLULAR",0},
 {"TRANSPORT_WIFI",1},
 {"TRANSPORT_BLUETOOTH",2},
 {"TRANSPORT_ETHERNET",3},
 {"TRANSPORT_VPN",4},
 {"TRANSPORT_WIFI_AWARE",5},
 {"TRANSPORT_LOWPAN",6}
};

static const network_int_constant telephony_constants[] = {
 {"NETWORK_TYPE_UNKNOWN",0},
 {"NETWORK_TYPE_GPRS",1},
 {"NETWORK_TYPE_EDGE",2},
 {"NETWORK_TYPE_UMTS",3},
 {"NETWORK_TYPE_CDMA",4},
 {"NETWORK_TYPE_EVDO_0",5},
 {"NETWORK_TYPE_EVDO_A",6},
 {"NETWORK_TYPE_1xRTT",7},
 {"NETWORK_TYPE_HSDPA",8},
 {"NETWORK_TYPE_HSUPA",9},
 {"NETWORK_TYPE_HSPA",10},
 {"NETWORK_TYPE_IDEN",11},
 {"NETWORK_TYPE_EVDO_B",12},
 {"NETWORK_TYPE_LTE",13},
 {"NETWORK_TYPE_EHRPD",14},
 {"NETWORK_TYPE_HSPAP",15},
 {"NETWORK_TYPE_GSM",16},
 {"NETWORK_TYPE_TD_SCDMA",17},
 {"NETWORK_TYPE_IWLAN",18},
 {"NETWORK_TYPE_LTE_CA",19},
 {"NETWORK_TYPE_NR",20},
 {"SIM_STATE_UNKNOWN",0},
 {"SIM_STATE_ABSENT",1},
 {"SIM_STATE_PIN_REQUIRED",2},
 {"SIM_STATE_PUK_REQUIRED",3},
 {"SIM_STATE_NETWORK_LOCKED",4},
 {"SIM_STATE_READY",5},
 {"SIM_STATE_NOT_READY",6},
 {"SIM_STATE_PERM_DISABLED",7},
 {"SIM_STATE_CARD_IO_ERROR",8},
 {"SIM_STATE_CARD_RESTRICTED",9},
 {"SIM_STATE_LOADED",10},
 {"SIM_STATE_PRESENT",11},
 {"PHONE_TYPE_NONE",0},
 {"PHONE_TYPE_GSM",1},
 {"PHONE_TYPE_CDMA",2},
 {"PHONE_TYPE_SIP",3}
};

static bool int_constant(const char *field,const char *type,
 const network_int_constant *table,size_t count,union dvm_value *out){
 if(strcmp(type,"I"))return false;
 for(size_t i=0;i<count;++i){
  if(!strcmp(field,table[i].name)){
   out->i=table[i].value;
   return true;
  }
 }
 return false;
}

bool network_runtime_static(struct dvm *v,const char *c,const char *field,
 const char *type,union dvm_value *out){
 if(!v||!c||!field||!type||!out)return false;
 activate(v);
 if(!network_gc_ensure(v))return false;

 if(!strcmp(c,"android/net/NetworkInfo$State")&&
  !strcmp(type,"Landroid/net/NetworkInfo$State;")){
  for(int i=0;i<NETWORK_STATE_COUNT;++i){
   if(!strcmp(field,network_state_names[i])){
    out->l=state_value(v,i);
    return out->l!=0||dvm_exception(v);
   }
  }
  return false;
 }
 if(!strcmp(c,"android/net/NetworkInfo$DetailedState")&&
  !strcmp(type,"Landroid/net/NetworkInfo$DetailedState;")){
  for(int i=0;i<NETWORK_DETAILED_COUNT;++i){
   if(!strcmp(field,network_detailed_names[i])){
    out->l=detailed_state_value(v,i);
    return out->l!=0||dvm_exception(v);
   }
  }
  return false;
 }
 if(!strcmp(c,"android/net/ConnectivityManager"))
  return int_constant(field,type,connectivity_constants,
   sizeof connectivity_constants/sizeof connectivity_constants[0],out);
 if(!strcmp(c,"android/net/NetworkCapabilities"))
  return int_constant(field,type,network_caps_constants,
   sizeof network_caps_constants/sizeof network_caps_constants[0],out);
 if(!strcmp(c,"android/telephony/TelephonyManager"))
  return int_constant(field,type,telephony_constants,
   sizeof telephony_constants/sizeof telephony_constants[0],out);
 return false;
}

/* ---- selftest ---------------------------------------------------------- */

typedef struct selftest_fixture {
 int32_t connectivity_valid;
 int32_t has_default_network;
 int32_t connected;
 int32_t available;
 int32_t metered;
 int32_t type;
 int32_t type_valid;
 uint32_t transports;
 uint32_t capabilities;
 uint64_t network_handle;
 int32_t telephony_valid;
 int32_t network_type;
 int32_t network_type_valid;
 int32_t sim_state;
 int32_t sim_state_valid;
 int32_t phone_type;
 int32_t phone_type_valid;
 int32_t result;                /* callback ABI: 0 success, negative failure */
 uint32_t version;              /* nonzero: override the pre-filled version */
 uint32_t size;                 /* nonzero: override the pre-filled size */
 int32_t invalid_connected;     /* nonzero: write an out-of-range boolean */
 int32_t unterminated_operator; /* nonzero: leave operator_name unterminated */
 int32_t operator_numeric_valid;
 int32_t operator_name_valid;
 char operator_numeric[NETWORK_HOST_OPERATOR_MAX];
 char operator_name[NETWORK_HOST_NAME_MAX];
} selftest_fixture;

static int selftest_snapshot(void *ctx,network_host_snapshot *snap){
 const selftest_fixture *fixture=ctx;
 if(fixture->version)snap->version=fixture->version;
 if(fixture->size)snap->size=fixture->size;
 snap->connectivity_valid=fixture->connectivity_valid;
 snap->has_default_network=fixture->has_default_network;
 snap->connected=fixture->invalid_connected?2:fixture->connected;
 snap->available=fixture->available;
 snap->metered=fixture->metered;
 snap->type=fixture->type;
 snap->type_valid=fixture->type_valid;
 snap->transports=fixture->transports;
 snap->capabilities=fixture->capabilities;
 snap->network_handle=fixture->network_handle;
 snap->telephony_valid=fixture->telephony_valid;
 snap->network_type=fixture->network_type;
 snap->network_type_valid=fixture->network_type_valid;
 snap->sim_state=fixture->sim_state;
 snap->sim_state_valid=fixture->sim_state_valid;
 snap->phone_type=fixture->phone_type;
 snap->phone_type_valid=fixture->phone_type_valid;
 snap->operator_numeric_valid=fixture->operator_numeric_valid;
 snap->operator_name_valid=fixture->operator_name_valid;
 memcpy(snap->operator_numeric,fixture->operator_numeric,
  sizeof snap->operator_numeric);
 memcpy(snap->operator_name,fixture->operator_name,
  sizeof snap->operator_name);
 if(fixture->unterminated_operator)
  memset(snap->operator_name,'x',sizeof snap->operator_name);
 return fixture->result;
}

static bool exception_is(struct dvm *v,const char *name){
 dvm_ref ref=dvm_exception(v);
 if(!ref)return false;
 struct dvm_object *obj=dvm__obj(v,ref);
 return obj&&obj->cls&&obj->cls->name&&!strcmp(obj->cls->name,name);
}

static bool string_is(struct dvm *v,dvm_ref ref,const char *text){
 if(!ref)return false;
 const char *value=dvm_string_utf8(v,ref);
 size_t length=(size_t)dvm_string_utf8_length(v,ref);
 if(!value)return false;
 size_t want=strlen(text);
 return length==want&&!memcmp(value,text,want);
}

static const char *selftest_exception_name(struct dvm *v){
 dvm_ref ref=dvm_exception(v);
 if(!ref)return "none";
 struct dvm_object *obj=dvm__obj(v,ref);
 if(obj&&obj->cls&&obj->cls->name)return obj->cls->name;
 return "unknown";
}

static void selftest_fail(struct dvm *v,const char *stage,const char *expected,
 const char *actual){
 sdk_trace_fprintf(stderr,
  "NETWORK_RUNTIME_SELFTEST FAIL stage=%s expected=%s actual=%s exception=%s\n",
  stage,expected,actual,selftest_exception_name(v));
}

static void selftest_fail_int(struct dvm *v,const char *stage,int64_t expected,
 int64_t actual){
 char expected_text[32],actual_text[32];
 snprintf(expected_text,sizeof expected_text,"%lld",(long long)expected);
 snprintf(actual_text,sizeof actual_text,"%lld",(long long)actual);
 selftest_fail(v,stage,expected_text,actual_text);
}

#define SELFTEST_FAIL(stage_,expected_,actual_) do { \
 selftest_fail(v,(stage_),(expected_),(actual_)); \
 ok=false; \
 goto done; \
} while(0)

static int audio_volume_fixture(void *user,int stream_type,int *volume){
 int *calls=user;if(calls)++*calls;
 if(stream_type!=3)return -1;
 *volume=7;return 0;
}

#define SELFTEST_CHECK(stage_,expr_) do { \
 if(!(expr_))SELFTEST_FAIL((stage_),#expr_,"false"); \
} while(0)

#define SELFTEST_INT(stage_,actual_,expected_) do { \
 int64_t selftest_actual_value_=(int64_t)(actual_); \
 int64_t selftest_expected_value_=(int64_t)(expected_); \
 if(selftest_actual_value_!=selftest_expected_value_){ \
  selftest_fail_int(v,(stage_),selftest_expected_value_,selftest_actual_value_); \
  ok=false; \
  goto done; \
 } \
} while(0)

#define SELFTEST_TEXT(stage_,ref_,expected_) do { \
 dvm_ref selftest_ref_=(ref_); \
 if(!string_is(v,selftest_ref_,(expected_))){ \
  char selftest_actual_text_[64]; \
  const char *selftest_text_=selftest_ref_?dvm_string_utf8(v,selftest_ref_):NULL; \
  if(selftest_text_){ \
   size_t selftest_len_=strlen(selftest_text_); \
   if(selftest_len_>sizeof selftest_actual_text_-1) \
    selftest_len_=sizeof selftest_actual_text_-1; \
   memcpy(selftest_actual_text_,selftest_text_,selftest_len_); \
   selftest_actual_text_[selftest_len_]=0; \
  }else{ \
   snprintf(selftest_actual_text_,sizeof selftest_actual_text_,"%s", \
    selftest_ref_?"unreadable_string":"null"); \
  } \
  SELFTEST_FAIL((stage_),(expected_),selftest_actual_text_); \
 } \
} while(0)

#define SELFTEST_TEXT_EMPTY(stage_,ref_) do { \
 dvm_ref selftest_ref_=(ref_); \
 if(!string_is(v,selftest_ref_,"")){ \
  sdk_trace_fprintf(stderr, \
   "NETWORK_RUNTIME_SELFTEST FAIL stage=%s expected=empty_string " \
   "actual=%s exception=%s\n", \
   (stage_),selftest_ref_?"non_empty_or_unreadable":"null", \
   selftest_exception_name(v)); \
  ok=false; \
  goto done; \
 } \
} while(0)

#define SELFTEST_NONNULL(stage_,ref_) do { \
 if(!(ref_))SELFTEST_FAIL((stage_),"non_null","null"); \
} while(0)

#define SELFTEST_SAME(stage_,actual_,expected_) do { \
 dvm_ref selftest_actual_ref_=(actual_); \
 dvm_ref selftest_expected_ref_=(expected_); \
 if(!selftest_actual_ref_||selftest_actual_ref_!=selftest_expected_ref_) \
  SELFTEST_FAIL((stage_),"same_reference", \
   selftest_actual_ref_?"different_reference":"null"); \
} while(0)

#define SELFTEST_DISTINCT(stage_,actual_,other_) do { \
 dvm_ref selftest_actual_ref_=(actual_); \
 dvm_ref selftest_other_ref_=(other_); \
 if(!selftest_actual_ref_||selftest_actual_ref_==selftest_other_ref_) \
  SELFTEST_FAIL((stage_),"distinct_reference", \
   selftest_actual_ref_?"same_reference":"null"); \
} while(0)

bool network_runtime_selftest(struct dvm *v){
 if(!v){
  sdk_trace_fprintf(stderr,
   "NETWORK_RUNTIME_SELFTEST FAIL stage=begin expected=vm actual=null "
   "exception=none\n");
  return false;
 }
 activate(v);
 sdk_trace_fprintf(stderr,
  "NETWORK_RUNTIME_SELFTEST BEGIN stage=begin expected=selftest_start "
  "actual=started exception=none host_bridge=fixture\n");

 network_host_bridge saved;
 host_bridge_copy(&saved);
 bool ok=true;
 union dvm_value out={0},arg={0};
 selftest_fixture fixture;
 network_host_bridge bridge;
 dvm_ref context=0,cm=0,info=0,info2=0,network=0,caps=0,tm=0,state=0,detailed=0;

 /* No bridge: manager objects still exist, observations fail closed. */
 network_runtime_set_host(NULL);
 context=new_plain_object(v,"android/content/Context","selftest Context");
 SELFTEST_NONNULL("no_bridge_context",context);
 arg.l=dvm_new_string(v,"connectivity");
 SELFTEST_NONNULL("no_bridge_service_name",arg.l);
 SELFTEST_CHECK("no_bridge_get_system_service",
  network_runtime_call(v,"android/content/Context",
   "getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",
   context,&arg,1,&out));
 cm=out.l;
 SELFTEST_NONNULL("no_bridge_connectivity_manager",cm);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("no_bridge_manager_stable_call",
  network_runtime_call(v,"android/content/Context",
   "getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",
   context,&arg,1,&out));
 SELFTEST_SAME("no_bridge_manager_stable",out.l,cm);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("no_bridge_fail_closed_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetworkInfo","()Landroid/net/NetworkInfo;",cm,NULL,0,&out));
 SELFTEST_CHECK("no_bridge_fail_closed_exception",dvm_exception(v)!=0);
 SELFTEST_CHECK("no_bridge_fail_closed_class",
  exception_is(v,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);

 /* AudioManager.getStreamVolume: no host fails closed; a real host value is
  * passed through unchanged and an unsupported stream is rejected. */
 {
  union dvm_value audio_arg={0},stream={.i=3};
  dvm_ref audio_manager=0;
  pthread_mutex_lock(&host_lock);
  audio_volume_host_fn saved_audio_host=audio_volume_host;
  void *saved_audio_user=audio_volume_user;
  pthread_mutex_unlock(&host_lock);
  sdk_set_audio_volume_host(NULL,NULL);
  audio_arg.l=dvm_new_string(v,"audio");
  SELFTEST_NONNULL("audio_service_name_selftest",audio_arg.l);
  memset(&out,0,sizeof out);
  SELFTEST_CHECK("audio_service_call_selftest",
   network_runtime_call(v,"android/content/Context",
    "getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",
    context,&audio_arg,1,&out));
  audio_manager=out.l;
  SELFTEST_NONNULL("audio_manager_selftest",audio_manager);
  memset(&out,0,sizeof out);
  SELFTEST_CHECK("audio_no_host_call",
   network_runtime_call(v,"android/media/AudioManager","getStreamVolume","(I)I",
    audio_manager,&stream,1,&out));
  SELFTEST_CHECK("audio_no_host_fail_closed",dvm_exception(v)!=0&&
   exception_is(v,"java/lang/UnsupportedOperationException"));
  dvm_clear_exception(v);
  int audio_calls=0;
  sdk_set_audio_volume_host(audio_volume_fixture,&audio_calls);
  memset(&out,0,sizeof out);
  SELFTEST_CHECK("audio_fixture_call",
   network_runtime_call(v,"android/media/AudioManager","getStreamVolume","(I)I",
    audio_manager,&stream,1,&out));
  SELFTEST_INT("audio_fixture_volume",out.i,7);
  SELFTEST_INT("audio_fixture_calls",audio_calls,1);
  stream.i=1;
  memset(&out,0,sizeof out);
  SELFTEST_CHECK("audio_fixture_reject_call",
   network_runtime_call(v,"android/media/AudioManager","getStreamVolume","(I)I",
    audio_manager,&stream,1,&out));
  SELFTEST_CHECK("audio_fixture_reject_fail_closed",dvm_exception(v)!=0&&
   exception_is(v,"java/lang/UnsupportedOperationException"));
  dvm_clear_exception(v);
  sdk_set_audio_volume_host(saved_audio_host,saved_audio_user);
 }

 /* Connected=0 fixture: the runtime may say "disconnected", never "online". */
 memset(&fixture,0,sizeof fixture);
 fixture.connectivity_valid=1;
 fixture.has_default_network=1;
 fixture.connected=0;
 fixture.available=1;
 fixture.metered=1;
 fixture.type_valid=1;
 fixture.type=1; /* TYPE_WIFI */
 fixture.transports=1u<<1; /* TRANSPORT_WIFI */
 fixture.capabilities=1u<<12; /* NET_CAPABILITY_INTERNET */
 fixture.network_handle=0x1122334455667788ULL;
 fixture.telephony_valid=1;
 fixture.network_type_valid=1;
 fixture.network_type=0; /* NETWORK_TYPE_UNKNOWN */
 fixture.sim_state_valid=1;
 fixture.sim_state=0; /* SIM_STATE_UNKNOWN */
 fixture.phone_type_valid=0; /* no OHOS equivalent; must answer NONE */
 fixture.phone_type=1; /* ignored because phone_type_valid == 0 */
 memset(&bridge,0,sizeof bridge);
 bridge.ctx=&fixture;
 bridge.snapshot=selftest_snapshot;
 network_runtime_set_host(&bridge);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("fixture_active_network_info_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetworkInfo","()Landroid/net/NetworkInfo;",cm,NULL,0,&out));
 info=out.l;
 SELFTEST_NONNULL("fixture_active_network_info",info);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_is_connected_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "isConnected","()Z",info,NULL,0,&out));
 SELFTEST_INT("info_is_connected_value",out.i,0);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_is_connected_or_connecting_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "isConnectedOrConnecting","()Z",info,NULL,0,&out));
 SELFTEST_INT("info_is_connected_or_connecting_value",out.i,0);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_is_available_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "isAvailable","()Z",info,NULL,0,&out));
 SELFTEST_INT("info_is_available_value",out.i,1);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_get_type_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getType","()I",info,NULL,0,&out));
 SELFTEST_INT("info_get_type_value",out.i,1);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_get_type_name_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getTypeName","()Ljava/lang/String;",info,NULL,0,&out));
 SELFTEST_TEXT("info_get_type_name_value",out.l,"WIFI");
 /* 已知 telephony 观测为 NETWORK_TYPE_UNKNOWN(0)：getSubtypeName 必须按
  * TelephonyManager.getNetworkTypeName 返回 "UNKNOWN"，不能编造 LTE。 */
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_get_subtype_name_unknown_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getSubtypeName","()Ljava/lang/String;",info,NULL,0,&out));
 SELFTEST_TEXT("info_get_subtype_name_unknown_value",out.l,"UNKNOWN");
 /* getSubtype()I 必须返回 payload 的真实 subtype；这个 fixture 只是
  * NETWORK_TYPE_UNKNOWN，不能因为 subtypeName 是字符串就伪造数值。 */
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_get_subtype_unknown_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getSubtype","()I",info,NULL,0,&out));
 SELFTEST_INT("info_get_subtype_unknown_value",out.i,0);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_subtype_keeps_disconnected_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "isConnected","()Z",info,NULL,0,&out));
 SELFTEST_INT("info_subtype_keeps_disconnected_value",out.i,0);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_get_state_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getState","()Landroid/net/NetworkInfo$State;",info,NULL,0,&out));
 state=out.l;
 SELFTEST_NONNULL("info_get_state_value",state);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_get_state_static_call",
  network_runtime_static(v,"android/net/NetworkInfo$State",
   "DISCONNECTED","Landroid/net/NetworkInfo$State;",&out));
 SELFTEST_SAME("info_get_state_static_identity",out.l,state);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("info_get_detailed_state_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getDetailedState","()Landroid/net/NetworkInfo$DetailedState;",
   info,NULL,0,&out));
 detailed=out.l;
 SELFTEST_NONNULL("info_get_detailed_state_value",detailed);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("detailed_state_static_call",
  network_runtime_static(v,
   "android/net/NetworkInfo$DetailedState","DISCONNECTED",
   "Landroid/net/NetworkInfo$DetailedState;",&out));
 SELFTEST_SAME("detailed_state_static_identity",out.l,detailed);

 arg.i=1;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("network_info_by_type_wifi_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getNetworkInfo","(I)Landroid/net/NetworkInfo;",cm,&arg,1,&out));
 info2=out.l;
 SELFTEST_NONNULL("network_info_by_type_wifi_value",info2);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("network_info_by_type_wifi_type_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getType","()I",info2,NULL,0,&out));
 SELFTEST_INT("network_info_by_type_wifi_type_value",out.i,1);

 arg.i=0;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("network_info_by_type_mobile_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getNetworkInfo","(I)Landroid/net/NetworkInfo;",cm,&arg,1,&out));
 SELFTEST_CHECK("network_info_by_type_mobile_null",out.l==0);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("is_active_network_metered_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "isActiveNetworkMetered","()Z",cm,NULL,0,&out));
 SELFTEST_INT("is_active_network_metered_value",out.i,1);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("active_network_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetwork","()Landroid/net/Network;",cm,NULL,0,&out));
 network=out.l;
 SELFTEST_NONNULL("active_network_value",network);

 arg.l=network;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("network_capabilities_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getNetworkCapabilities",
   "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;",
   cm,&arg,1,&out));
 caps=out.l;
 SELFTEST_NONNULL("network_capabilities_value",caps);
 arg.i=1;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("caps_has_transport_wifi_call",
  network_runtime_call(v,"android/net/NetworkCapabilities",
   "hasTransport","(I)Z",caps,&arg,1,&out));
 SELFTEST_INT("caps_has_transport_wifi_value",out.i,1);
 arg.i=0;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("caps_has_transport_cellular_call",
  network_runtime_call(v,"android/net/NetworkCapabilities",
   "hasTransport","(I)Z",caps,&arg,1,&out));
 SELFTEST_INT("caps_has_transport_cellular_value",out.i,0);
 arg.i=12;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("caps_has_capability_internet_call",
  network_runtime_call(v,"android/net/NetworkCapabilities",
   "hasCapability","(I)Z",caps,&arg,1,&out));
 SELFTEST_INT("caps_has_capability_internet_value",out.i,1);

 arg.l=dvm_new_string(v,"phone");
 SELFTEST_NONNULL("telephony_service_name",arg.l);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("telephony_service_call",
  network_runtime_call(v,"android/content/Context",
   "getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",
   context,&arg,1,&out));
 tm=out.l;
 SELFTEST_NONNULL("telephony_service_value",tm);
 arg.l=dvm_new_string(v,"clipboard");
 SELFTEST_NONNULL("clipboard_service_name",arg.l);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("clipboard_service_call",
  network_runtime_call(v,"android/content/Context",
   "getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",
   context,&arg,1,&out));
 SELFTEST_NONNULL("clipboard_service_value",out.l);
 arg.l=dvm_new_string(v,"audio");
 SELFTEST_NONNULL("audio_service_name",arg.l);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("audio_service_call",
  network_runtime_call(v,"android/content/Context",
   "getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",
   context,&arg,1,&out));
 SELFTEST_NONNULL("audio_service_value",out.l);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("telephony_network_operator_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkOperator","()Ljava/lang/String;",tm,NULL,0,&out));
 SELFTEST_TEXT_EMPTY("telephony_network_operator_value",out.l);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("telephony_network_operator_name_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkOperatorName","()Ljava/lang/String;",tm,NULL,0,&out));
 SELFTEST_TEXT_EMPTY("telephony_network_operator_name_value",out.l);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("telephony_network_type_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkType","()I",tm,NULL,0,&out));
 SELFTEST_INT("telephony_network_type_value",out.i,0);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("telephony_sim_state_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getSimState","()I",tm,NULL,0,&out));
 SELFTEST_INT("telephony_sim_state_value",out.i,0);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("telephony_phone_type_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getPhoneType","()I",tm,NULL,0,&out));
 SELFTEST_INT("telephony_phone_type_value",out.i,0);

 /* Success contract: rc==0 with a valid snapshot must be accepted and its
  * fields used verbatim.  This fixture is explicitly connected so the
  * assertions prove the runtime copies host data instead of hard-coding an
  * online or offline answer. */
 memset(&fixture,0,sizeof fixture);
 fixture.connectivity_valid=1;
 fixture.has_default_network=1;
 fixture.connected=1;
 fixture.available=1;
 fixture.metered=0;
 fixture.type_valid=1;
 fixture.type=0; /* TYPE_MOBILE */
 fixture.transports=1u<<0; /* TRANSPORT_CELLULAR */
 fixture.capabilities=1u<<12; /* NET_CAPABILITY_INTERNET */
 fixture.network_handle=0x8877665544332211ULL;
 fixture.telephony_valid=1;
 fixture.network_type_valid=1;
 fixture.network_type=13; /* NETWORK_TYPE_LTE */
 fixture.sim_state_valid=1;
 fixture.sim_state=5; /* SIM_STATE_READY */
 fixture.phone_type_valid=1;
 fixture.phone_type=2; /* PHONE_TYPE_CDMA */
 fixture.operator_numeric_valid=1;
 snprintf(fixture.operator_numeric,sizeof fixture.operator_numeric,"%s",
  "46001");
 fixture.operator_name_valid=1;
 snprintf(fixture.operator_name,sizeof fixture.operator_name,"%s",
  "Selftest Carrier");
 fixture.result=0;
 network_runtime_set_host(&bridge);

 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_info_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetworkInfo","()Landroid/net/NetworkInfo;",cm,NULL,0,&out));
 info=out.l;
 SELFTEST_NONNULL("snapshot_success_info",info);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_connected_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "isConnected","()Z",info,NULL,0,&out));
 SELFTEST_INT("snapshot_success_connected_value",out.i,1);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_available_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "isAvailable","()Z",info,NULL,0,&out));
 SELFTEST_INT("snapshot_success_available_value",out.i,1);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_type_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getType","()I",info,NULL,0,&out));
 SELFTEST_INT("snapshot_success_type_value",out.i,0);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_metered_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "isActiveNetworkMetered","()Z",cm,NULL,0,&out));
 SELFTEST_INT("snapshot_success_metered_value",out.i,0);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_network_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetwork","()Landroid/net/Network;",cm,NULL,0,&out));
 network=out.l;
 SELFTEST_NONNULL("snapshot_success_network",network);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_network_handle_call",
  network_runtime_call(v,"android/net/Network",
   "getNetworkHandle","()J",network,NULL,0,&out));
 SELFTEST_INT("snapshot_success_network_handle_value",out.j,
  0x8877665544332211ULL);
 arg.l=network;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_caps_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getNetworkCapabilities",
   "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;",
   cm,&arg,1,&out));
 caps=out.l;
 SELFTEST_NONNULL("snapshot_success_caps",caps);
 arg.i=0;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_transport_call",
  network_runtime_call(v,"android/net/NetworkCapabilities",
   "hasTransport","(I)Z",caps,&arg,1,&out));
 SELFTEST_INT("snapshot_success_transport_value",out.i,1);
 arg.i=12;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_capability_call",
  network_runtime_call(v,"android/net/NetworkCapabilities",
   "hasCapability","(I)Z",caps,&arg,1,&out));
 SELFTEST_INT("snapshot_success_capability_value",out.i,1);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_network_type_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkType","()I",tm,NULL,0,&out));
 SELFTEST_INT("snapshot_success_network_type_value",out.i,13);
 /* 同一快照的 NetworkInfo 必须携带真实 subtype=13 并映射为 AOSP 的 "LTE"；
  * 读 subtype 不改变 connected（仍为 1）。 */
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_subtype_name_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getSubtypeName","()Ljava/lang/String;",info,NULL,0,&out));
 SELFTEST_TEXT("snapshot_success_subtype_name_value",out.l,"LTE");
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_subtype_value_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getSubtype","()I",info,NULL,0,&out));
 SELFTEST_INT("snapshot_success_subtype_value",out.i,13);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_subtype_keeps_connected_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "isConnected","()Z",info,NULL,0,&out));
 SELFTEST_INT("snapshot_success_subtype_keeps_connected_value",out.i,1);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_sim_state_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getSimState","()I",tm,NULL,0,&out));
 SELFTEST_INT("snapshot_success_sim_state_value",out.i,5);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_phone_type_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getPhoneType","()I",tm,NULL,0,&out));
 SELFTEST_INT("snapshot_success_phone_type_value",out.i,2);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_operator_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkOperator","()Ljava/lang/String;",tm,NULL,0,&out));
 SELFTEST_TEXT("snapshot_success_operator_value",out.l,"46001");
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_success_operator_name_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkOperatorName","()Ljava/lang/String;",tm,NULL,0,&out));
 SELFTEST_TEXT("snapshot_success_operator_name_value",out.l,
  "Selftest Carrier");

 /* rc != 0 must fail closed even though the fixture left every field valid. */
 fixture.result=-1;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_callback_failure_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetworkInfo","()Landroid/net/NetworkInfo;",cm,NULL,0,&out));
 SELFTEST_CHECK("snapshot_callback_failure_output",out.l==0);
 SELFTEST_CHECK("snapshot_callback_failure_exception",dvm_exception(v)!=0);
 SELFTEST_CHECK("snapshot_callback_failure_class",
  exception_is(v,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_callback_failure_telephony_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkType","()I",tm,NULL,0,&out));
 SELFTEST_CHECK("snapshot_callback_failure_telephony_exception",
  dvm_exception(v)!=0);
 SELFTEST_CHECK("snapshot_callback_failure_telephony_class",
  exception_is(v,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);

 /* rc == 0 with a bad version must be rejected, not treated as rc failure. */
 fixture.result=0;
 fixture.version=NETWORK_HOST_SNAPSHOT_VERSION+1u;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_bad_version_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetworkInfo","()Landroid/net/NetworkInfo;",cm,NULL,0,&out));
 SELFTEST_CHECK("snapshot_bad_version_output",out.l==0);
 SELFTEST_CHECK("snapshot_bad_version_exception",dvm_exception(v)!=0);
 SELFTEST_CHECK("snapshot_bad_version_class",
  exception_is(v,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);

 fixture.version=0;
 fixture.size=(uint32_t)sizeof(network_host_snapshot)-1u;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_bad_size_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetworkInfo","()Landroid/net/NetworkInfo;",cm,NULL,0,&out));
 SELFTEST_CHECK("snapshot_bad_size_output",out.l==0);
 SELFTEST_CHECK("snapshot_bad_size_exception",dvm_exception(v)!=0);
 SELFTEST_CHECK("snapshot_bad_size_class",
  exception_is(v,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);

 fixture.size=0;
 fixture.invalid_connected=1;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_invalid_field_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetworkInfo","()Landroid/net/NetworkInfo;",cm,NULL,0,&out));
 SELFTEST_CHECK("snapshot_invalid_field_output",out.l==0);
 SELFTEST_CHECK("snapshot_invalid_field_exception",dvm_exception(v)!=0);
 SELFTEST_CHECK("snapshot_invalid_field_class",
  exception_is(v,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);

 fixture.invalid_connected=0;
 fixture.unterminated_operator=1;
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_unterminated_operator_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkOperatorName","()Ljava/lang/String;",tm,NULL,0,&out));
 SELFTEST_CHECK("snapshot_unterminated_operator_output",out.l==0);
 SELFTEST_CHECK("snapshot_unterminated_operator_exception",
  dvm_exception(v)!=0);
 SELFTEST_CHECK("snapshot_unterminated_operator_class",
  exception_is(v,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);

 /* telephony_valid=0：connectivity 快照仍真实存在，所以 NetworkInfo 可以
  * 创建；但没有任何 telephony 观测时 subtype 必须为 UNKNOWN，而 connected
  * 仍按 connectivity 快照为 1，不能因为缺少 telephony 就伪造断网。 */
 fixture.unterminated_operator=0;
 fixture.telephony_valid=0;
 fixture.network_type_valid=0;
 fixture.network_type=13; /* invalid flag: must be ignored as a subtype */
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_no_telephony_info_call",
  network_runtime_call(v,"android/net/ConnectivityManager",
   "getActiveNetworkInfo","()Landroid/net/NetworkInfo;",cm,NULL,0,&out));
 info=out.l;
 SELFTEST_NONNULL("snapshot_no_telephony_info",info);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_no_telephony_subtype_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getSubtypeName","()Ljava/lang/String;",info,NULL,0,&out));
 SELFTEST_TEXT("snapshot_no_telephony_subtype_value",out.l,"UNKNOWN");
 /* network_type_valid=0 时即使 raw snapshot 里塞了 13，也不能泄漏成
  * getSubtype()==13；必须是 NETWORK_TYPE_UNKNOWN(0)。 */
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_no_telephony_get_subtype_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "getSubtype","()I",info,NULL,0,&out));
 SELFTEST_INT("snapshot_no_telephony_get_subtype_value",out.i,0);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("snapshot_no_telephony_connected_call",
  network_runtime_call(v,"android/net/NetworkInfo",
   "isConnected","()Z",info,NULL,0,&out));
 SELFTEST_INT("snapshot_no_telephony_connected_value",out.i,1);

 /* With the bridge removed, telephony observations fail closed too. */
 network_runtime_set_host(NULL);
 memset(&out,0,sizeof out);
 SELFTEST_CHECK("telephony_bridge_removed_call",
  network_runtime_call(v,"android/telephony/TelephonyManager",
   "getNetworkType","()I",tm,NULL,0,&out));
 SELFTEST_CHECK("telephony_bridge_removed_exception",dvm_exception(v)!=0);
 SELFTEST_CHECK("telephony_bridge_removed_class",
  exception_is(v,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);

done:
 network_runtime_set_host(saved.snapshot?&saved:NULL);
 if(!ok){
  if(dvm_exception(v))dvm_clear_exception(v);
  return false;
 }
 if(dvm_exception(v)){
  sdk_trace_fprintf(stderr,
   "NETWORK_RUNTIME_SELFTEST FAIL stage=finalize "
   "expected=no_pending_exception actual=pending_exception exception=%s\n",
   selftest_exception_name(v));
  dvm_clear_exception(v);
  return false;
 }
 sdk_trace_fprintf(stderr,
  "NETWORK_RUNTIME_SELFTEST PASS host_bridge=fixture connected=0 "
  "fail_closed=1 manager_stable=1 state_ref_identity=1 "
  "network_info_disconnected=1 telephony_unknown=1 no_fake_online=1 "
  "clipboard_manager=1 audio_manager=1\n");
 return true;
}

/* Native-only state: no guest identity in these tagged payloads. */
int network_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;(void)visit;(void)user;return o->data_destroy==network_info_destroy||o->data_destroy==network_caps_destroy||o->data_destroy==network_obj_destroy||o->data_destroy==network_enum_destroy?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
