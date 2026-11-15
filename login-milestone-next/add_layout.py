from pathlib import Path
p=Path('login-milestone-next/activity_runtime.h');s=p.read_text().replace('void activity_runtime_vm_start','void activity_runtime_set_host(int (*mount)(void *, int), void *user);\nvoid sdk_set_activity_host(int (*mount)(void *, int), void *user);\nvoid activity_runtime_vm_start');p.write_text(s)
p=Path('login-milestone-next/activity_runtime.c');s=p.read_text().replace('#include <string.h>','#include <string.h>\n#include <pthread.h>')
s=s.replace('int lifecycle; } state;','int lifecycle; dvm_ref layout; int orientation; } state;\nstatic pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;\nstatic int (*host_mount)(void *, int);\nstatic void *host_user;\nvoid activity_runtime_set_host(int (*mount)(void *, int),void *user){pthread_mutex_lock(&host_lock);host_mount=mount;host_user=user;pthread_mutex_unlock(&host_lock);}\n__attribute__((visibility("default"))) void sdk_set_activity_host(int (*mount)(void *, int),void *user){activity_runtime_set_host(mount,user);}')
s=s.replace('if(a&&w)a->super=w;','''if(a&&w)a->super=w;
 struct dvm_class *view=dvm__class_by_desc(v,"Landroid/view/View;");
 struct dvm_class *group=dvm__class_by_desc(v,"Landroid/view/ViewGroup;");
 struct dvm_class *layout=dvm__class_by_desc(v,"Landroid/widget/LinearLayout;");
 if(group&&view)group->super=view;if(layout&&group)layout->super=group;''')
needle=' if(!self||self!=state.activity||(state.lifecycle!=1&&state.lifecycle!=2))return false;'
block=''' if(!strcmp(c,"android/widget/LinearLayout")&&!strcmp(m,"<init>")&&!strcmp(sig,"(Landroid/content/Context;)V")){
  if(!self||!dvm__obj(v,self)||state.layout||state.lifecycle!=2||!a||a[0].l!=state.activity){dvm__throw(v,"java/lang/IllegalStateException","LinearLayout requires the created diagnostic Activity context");return true;}
  state.layout=self;state.orientation=0;return true;
 }
 if(!strcmp(c,"android/widget/LinearLayout")&&!strcmp(m,"setOrientation")&&!strcmp(sig,"(I)V")){
  if(!self||self!=state.layout||!a||(a[0].i!=0&&a[0].i!=1)){dvm__throw(v,"java/lang/IllegalArgumentException","unsupported LinearLayout or orientation");return true;}
  state.orientation=a[0].i;return true;
 }
 if(!self||self!=state.activity||(state.lifecycle!=1&&state.lifecycle!=2))return false;'''
assert needle in s;s=s.replace(needle,block)
needle=' if(!strcmp(m,"getApplicationContext")'
block=''' if(!strcmp(m,"setContentView")&&!strcmp(sig,"(Landroid/view/View;)V")){
  if(state.lifecycle!=2||!a||!state.layout||a[0].l!=state.layout){dvm__throw(v,"java/lang/UnsupportedOperationException","only the diagnostic LinearLayout can be mounted");return true;}
  pthread_mutex_lock(&host_lock);int (*mount)(void *,int)=host_mount;void *user=host_user;pthread_mutex_unlock(&host_lock);
  if(!mount||mount(user,state.orientation)!=0){sdk_trace_missing_api("android/app/Activity","setContentView","(Landroid/view/View;)V");dvm__throw(v,"java/lang/UnsupportedOperationException","host LinearLayout mount unavailable or rejected");return true;}
  return true;
 }
'''
assert needle in s;s=s.replace(needle,block+needle,1);p.write_text(s)
