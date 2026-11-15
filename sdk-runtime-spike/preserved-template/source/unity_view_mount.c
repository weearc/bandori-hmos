#include "unity_view_mount.h"
#include "view_state_runtime.h"
#include "surface_view_runtime.h"
#include "window_runtime.h"
#include "dvm/dvm_internal.h"
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);
/* Engine-stage diagnostic channel: sdk_trace_fprintf stops reaching the
 * progress file once the SDK runner returns, so append directly instead. */
static void engine_probe_log(const char *fmt,...){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 va_list a;va_start(a,fmt);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fclose(f);
}
#define TREE_CAP 256
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unity_view_mount_host host;
static void *host_user;
/* Process-pinned: an attached host or partial callback may outlive the call. */
static struct dvm *owner;
static uint64_t serial;
static int phase;
void sdk_set_unity_view_mount_host(unity_view_mount_host cb,void *user){pthread_mutex_lock(&lock);host=cb;host_user=user;pthread_mutex_unlock(&lock);}
static bool has_class(struct dvm *v,dvm_ref ref,const char *name){struct dvm_object *o=dvm__obj(v,ref);for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)if(c->name&&!strcmp(c->name,name))return true;return false;}
/* SurfaceView's surfaceCreated reaches UnityPlayer.updateDisplayInternal, which
 * silently skips its Handler post while U0.run has not yet published U0.a.
 * Wait for that publication (releasing the GIL so the engine thread can run)
 * before mounting, so the real surface callbacks reach the engine Looper. */
static bool engine_main_handler_ready(struct dvm *v,dvm_ref root){
 struct dvm_class *up=dvm__class_by_desc(v,"Lcom/unity3d/player/UnityPlayer;");
 if(!up||!root)return false;
 union dvm_value mt={0},handler={0};
 if(!dvm__get_field_from(v,root,up,"m_MainThread","Lcom/unity3d/player/U0;",&mt)||!mt.l)return false;
 struct dvm_class *u0=dvm_object_class(v,mt.l);
 if(!u0)return false;
 if(!dvm__get_field_from(v,mt.l,u0,"a","Landroid/os/Handler;",&handler))return false;
 return handler.l!=0;
}
static bool reject(const char *reason){engine_probe_log("UNITY_VIEW_MOUNT_REJECT reason=%s",reason);return false;}
bool unity_view_mount(struct dvm *v,dvm_ref root){
 if(!v||!root)return reject("arguments");
 if(!dvm_gil_held(v))return reject("gil_not_held");
 if(!has_class(v,root,"com/unity3d/player/UnityPlayer"))return reject("root_not_unity_player");
 dvm_ref tree[TREE_CAP],again[TREE_CAP],view=0;int count=view_state_runtime_snapshot_tree(v,root,tree,TREE_CAP);
 if(count<=0)return reject("empty_tree");for(int i=0;i<count;i++)if(has_class(v,tree[i],"android/view/SurfaceView")){if(view)return reject("multiple_surface_views");view=tree[i];}if(!view)return reject("no_surface_view");
 pthread_mutex_lock(&lock);if(phase||!host){pthread_mutex_unlock(&lock);return reject(phase?"phase_busy":"host_not_registered");}
 owner=v;serial=v->serial;phase=1;unity_view_mount_host cb=host;void *user=host_user;pthread_mutex_unlock(&lock);
 for(int i=0;i<count;i++)dvm_pin(v,tree[i]);
 int waited=0;for(int i=0;i<200&&!engine_main_handler_ready(v,root);i++){unsigned d=dvm_gil_unlock_all(v);struct timespec wait={.tv_sec=0,.tv_nsec=5000000};nanosleep(&wait,NULL);dvm_gil_relock(v,d);waited=i+1;}
 engine_probe_log("UNITY_VIEW_MOUNT_WAIT handler_ready=%d polls=%d",engine_main_handler_ready(v,root)?1:0,waited);
 void *token=NULL;int format=0;unsigned depth=dvm_gil_unlock_all(v);int result=cb(user,&token,&format);dvm_gil_relock(v,depth);
 int now=view_state_runtime_snapshot_tree(v,root,again,TREE_CAP);bool same=now==count;
 if(same)for(int i=0;i<count;i++)if(tree[i]!=again[i]){same=false;break;}
 bool ok=result==0&&token&&same&&v==owner&&v->serial==serial&&window_runtime_acquire_alive(token,NULL);
 if(ok){window_runtime_release(token);ok=view_state_runtime_set_attached(v,root,true);if(ok){pthread_mutex_lock(&lock);phase=2;pthread_mutex_unlock(&lock);ok=surface_view_runtime_attached(v,view,token,format);}}
 engine_probe_log("UNITY_VIEW_MOUNT tree=%d host_result=%d token=%d same=%d final=%d",count,result,token!=NULL,same,ok?1:0);
 if(result==0&&token)window_runtime_release(token);
 for(int i=0;i<count;i++)dvm_unpin(v,tree[i]);
 pthread_mutex_lock(&lock);if(phase==1){phase=0;owner=NULL;serial=0;}else phase=ok?3:4;pthread_mutex_unlock(&lock);
 return ok;
}

