#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kept self-contained so the NAPI/ArkTS shell can include this file to mirror
 * the host ABI without pulling in the VM headers. */
struct dvm;
union dvm_value;
typedef uint32_t dvm_ref;

/* Minimal android.net.ConnectivityManager / android.net.NetworkInfo /
 * android.telephony.TelephonyManager compatibility runtime.
 *
 * The VM cannot observe the device by itself, so every observation goes
 * through a HarmonyOS host bridge:
 *   @ohos.net.connection  -> connectivity snapshot
 *   @ohos.telephony.radio / @ohos.telephony.sim -> telephony snapshot
 *
 * There is deliberately no synthesized "offline" or "online" answer.  When a
 * bridge is absent, or the host says it cannot observe the requested family
 * (connectivity_valid/telephony_valid == 0), observation methods throw
 * java.lang.UnsupportedOperationException.
 * Exception: the scoped Unity JNI reachability query treats an unavailable
 * snapshot as no network (null active Network), with a diagnostic reason and
 * no new pending exception; it does not clear unrelated guest exceptions.
 * Manager objects are still handed out by Context.getSystemService() and stay stable per VM so callers can
 * obtain them before the bridge is installed.  When a bridge is present, a
 * valid snapshot with has_default_network == 0 is a real "no default network"
 * answer (null from getActiveNetworkInfo/getActiveNetwork), not a failure.
 *
 * The callback runs synchronously on the VM worker thread.  It must not
 * re-enter the VM.  network_runtime_set_host() may be called from another
 * thread; the caller must not free the context while a snapshot call is in
 * flight.
 *
 * Suggested OHOS mapping (the shell owns the enum translation):
 *   hasDefaultNetSync()                  -> has_default_network
 *   getDefaultNetSync().netId            -> network_handle (0 when absent)
 *   getNetCapabilitiesSync(handle)       -> transports (bearerTypes -> 1 <<
 *                                           Android TRANSPORT_*), capabilities
 *                                           (Android NET_CAPABILITY_* bits),
 *                                           metered (NOT_METERED bit absent),
 *                                           type (primary bearer: cellular 0,
 *                                           Wi-Fi 1, Ethernet 9)
 *   radio.getOperatorNameSync(slot)      -> operator_name (short or long)
 *   radio NetworkState.plmnNumeric       -> operator_numeric.  getNetworkState
 *                                           is asynchronous, so the shell may
 *                                           cache it and refresh on telephony
 *                                           state changes.
 *   radio.getRadioTechSync(slot)         -> network_type, translated to
 *                                           Android TelephonyManager.
 *                                           NETWORK_TYPE_* (OHOS NetworkType
 *                                           values 0..6 are NOT Android values
 *                                           1..20).  getRadioTechSync needs
 *                                           ohos.permission.GET_NETWORK_INFO
 *                                           and API 18+; otherwise leave
 *                                           network_type_valid == 0.
 *   sim.getSimStateSync(slot)            -> sim_state, translated; OHOS
 *                                           SIM_STATE_LOCKED cannot distinguish
 *                                           Android PIN/PUK/network lock, so
 *                                           leave sim_state_valid == 0 for it.
 *   no OHOS equivalent of getPhoneType() -> leave phone_type_valid == 0; the
 *                                           runtime then answers
 *                                           PHONE_TYPE_NONE (unknown/no radio).
 *
 * hasDefaultNetSync/getDefaultNetSync/getNetCapabilitiesSync and
 * getRadioTechSync require ohos.permission.GET_NETWORK_INFO.  The shell must
 * catch permission/syscap/service failures and set the corresponding
 * connectivity_valid/telephony_valid or per-field *_valid flag to 0; it must
 * not substitute a guessed value.
 *
 * OHOS has no NetworkInfo link-state API.  A live default network with a
 * successful capability query is the honest closest signal; the host must not
 * set connected from remembered Wi-Fi settings or other desired state.
 *
 * Unknown/unavailable per-field values are reported as Android's own
 * not-known values: empty string for operator fields, NETWORK_TYPE_UNKNOWN,
 * SIM_STATE_UNKNOWN, PHONE_TYPE_NONE.  The per-field *_valid flags let the
 * shell distinguish that from a real zero-valued answer where Android has one.
 */
#define NETWORK_HOST_SNAPSHOT_VERSION 1u
#define NETWORK_HOST_OPERATOR_MAX 48
#define NETWORK_HOST_NAME_MAX 64

typedef struct network_host_snapshot {
 uint32_t version;              /* must be NETWORK_HOST_SNAPSHOT_VERSION */
 uint32_t size;                 /* must be sizeof(network_host_snapshot) */

 /* android.net.ConnectivityManager */
 int32_t connectivity_valid;    /* 1: host can observe connectivity */
 int32_t has_default_network;   /* 1: a default network currently exists */
 int32_t connected;             /* NetworkInfo.isConnected() */
 int32_t available;             /* NetworkInfo.isAvailable(); connected implies
                                 * available even if this is left 0 */
 int32_t metered;               /* 1: active network is metered */
 int32_t type;                  /* ConnectivityManager.TYPE_* */
 int32_t type_valid;            /* 0: derive type from transports */
 uint32_t transports;           /* 1u << NetworkCapabilities.TRANSPORT_* */
 uint32_t capabilities;         /* 1u << NetworkCapabilities.NET_CAPABILITY_* */
 uint64_t network_handle;       /* Network.getNetworkHandle()/netId */

 /* android.telephony.TelephonyManager */
 int32_t telephony_valid;       /* 1: host can observe telephony */
 int32_t network_type;          /* TelephonyManager.NETWORK_TYPE_* */
 int32_t network_type_valid;
 int32_t sim_state;             /* TelephonyManager.SIM_STATE_* */
 int32_t sim_state_valid;
 int32_t phone_type;            /* TelephonyManager.PHONE_TYPE_* */
 int32_t phone_type_valid;
 int32_t operator_numeric_valid;
 int32_t operator_name_valid;
 char operator_numeric[NETWORK_HOST_OPERATOR_MAX];
 char operator_name[NETWORK_HOST_NAME_MAX];
} network_host_snapshot;

/* Return 0 for success.  On failure, or when version/size do not match, the
 * runtime fails closed.  Strings must be NUL-terminated inside their arrays. */
typedef int (*network_host_snapshot_fn)(void *ctx, network_host_snapshot *out);
typedef struct network_host_bridge {
 void *ctx;
 network_host_snapshot_fn snapshot;
} network_host_bridge;

#ifdef __cplusplus
extern "C" {
#endif
/* Installed by the NAPI shell before the SDK run.  Passing NULL (or a bridge
 * without a snapshot callback) clears it.  network_runtime_clear_host() is the
 * named spelling for the latter. */
void network_runtime_set_host(const network_host_bridge *bridge);
/* Stable dlsym alias, following sdk_set_settings_host. */
__attribute__((visibility("default"))) void sdk_set_network_host(const network_host_bridge *bridge);
void network_runtime_clear_host(void);

/* Resets the per-VM manager/enum caches.  Call once per VM, alongside the
 * other *_runtime_vm_start() hooks. */
void network_runtime_vm_start(struct dvm *vm);
/* JNI query scope: refresh real host state, deliver callbacks, reuse snapshot.
 * Requires GIL. Pair every true return with query_end, including error paths. */
bool network_runtime_query_begin(struct dvm *,const char *,const char *,const char *);
void network_runtime_query_end(int result,bool completed);
/* Metadata-only failure evidence before JNI moves the original exception.
 * No guest invocation, allocation, exception clearing or producer file IO. */
void network_runtime_query_failure(struct dvm *,const char *stage);

bool network_runtime_call(struct dvm *vm, const char *cls, const char *method,
 const char *sig, dvm_ref self, const union dvm_value *args, int nargs,
 union dvm_value *out);
/* Handles NetworkInfo$State / NetworkInfo$DetailedState sget-object fields and
 * the integer TYPE_, TRANSPORT_, NETWORK_TYPE_, SIM_STATE_ and PHONE_TYPE_
 * constants.  Enum objects are cached so getState() and sget-object produce
 * the same reference, as Java's enum identity requires. */
bool network_runtime_static(struct dvm *vm, const char *cls, const char *field,
 const char *type, union dvm_value *out);
/* Temporarily replaces and then restores the installed bridge; run it from the
 * single-threaded startup selftest, not while guest code is live. */
bool network_runtime_selftest(struct dvm *vm);
#ifdef __cplusplus
}
#endif
