#include "view_state_runtime.h"
#include "layout_params_runtime.h"
#include "dvm/dvm_internal.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);

#define VIEW_CAP 256
#define CHILD_CAP 32
typedef struct { dvm_ref view,context,description,layout_params; uint64_t generation; int id,focusable,focusable_touch; int decor,requested,applied,applied_valid,busy,layout_requested,background_set; uint32_t background_color; dvm_ref parent,children[CHILD_CAP]; unsigned child_count; int attached; } view_entry;
static struct {struct dvm *vm;struct dvm_class *view,*group,*frame,*context;view_entry entries[VIEW_CAP];} state;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static view_focus_host focus_host;
static void *focus_user;
void sdk_set_view_focus_host(view_focus_host callback,void *user){pthread_mutex_lock(&mu);focus_host=callback;focus_user=user;pthread_mutex_unlock(&mu);}
static view_system_ui_host system_ui_host;
static void *system_ui_user;
static uint64_t next_generation=1;
#define SUPPORTED_SYSTEM_UI_FLAGS (1u|2u|4u|256u|512u|1024u)
__attribute__((visibility("default"))) void sdk_set_view_system_ui_host(view_system_ui_host callback,void *user){pthread_mutex_lock(&mu);system_ui_host=callback;system_ui_user=user;pthread_mutex_unlock(&mu);}

static int parent_is_compatible(struct dvm_class *child,struct dvm_class *parent)
{
 return !child->super || child->super==parent || (child->super->name && !strcmp(child->super->name,"java/lang/Object"));
}
/* ---------------- Android MotionEvent（触摸注入的 guest 载体） ----------------
 * Unity 的输入入口是 UnityPlayer.nativeInjectEvent(InputEvent,int)；native
 * 侧通过 JNI 调用 MotionEvent/InputEvent 的 getter 读取动作、坐标、时间。
 * 这里实现最小真实快照对象（每对象一份 motion_event），并给宿主触摸转发
 * 暴露 sdk_motion_event_create()。obtain() 与常量按 Android 契约提供。 */
typedef struct { int action,source,device_id,flags; int64_t down_time,event_time; uint32_t count;
 struct { int id; float x,y,raw_x,raw_y,pressure,size; } pts[16]; } motion_event;
static void me_destroy(void*p){free(p);}
static motion_event* me_data(struct dvm_object*o){return o&&o->data_destroy==me_destroy?o->data:NULL;}
__attribute__((visibility("default"))) dvm_ref sdk_motion_event_create(struct dvm*v,int action,int pointer_id,
 float x,float y,int source,int64_t down_time_ms,int64_t event_time_ms){
 if(!v)return 0;
 struct dvm_class *cls=dvm_find_class_or_external(v,"android/view/MotionEvent");
 if(!cls){dvm__throw(v,"java/lang/NoClassDefFoundError","android/view/MotionEvent");return 0;}
 dvm_ref r=dvm_new_object(v,cls);struct dvm_object*o=dvm__obj(v,r);
 if(!o){dvm__throw(v,"java/lang/OutOfMemoryError","MotionEvent");return 0;}
 motion_event*e=calloc(1,sizeof *e);
 if(!e){dvm__throw(v,"java/lang/OutOfMemoryError","MotionEvent");return 0;}
 e->action=action;e->source=source;e->device_id=-1;e->down_time=down_time_ms;e->event_time=event_time_ms;e->count=1;
 e->pts[0].id=pointer_id;e->pts[0].x=x;e->pts[0].y=y;e->pts[0].raw_x=x;e->pts[0].raw_y=y;
 e->pts[0].pressure=1.0f;e->pts[0].size=1.0f;
 o->data=e;o->data_destroy=me_destroy;
 return r;
}
bool sdk_motion_event_set_device_id(struct dvm*v,dvm_ref event,int id){
 motion_event *e=me_data(dvm__obj(v,event));if(!e)return false;e->device_id=id;return true;
}
static bool me_call(struct dvm*v,const char*m,const char*sig,dvm_ref self,const union dvm_value*a,int n,union dvm_value*out){
 if(!strcmp(m,"obtain")&&!strcmp(sig,"(JJIFFI)Landroid/view/MotionEvent;")&&n==6){
  out->l=sdk_motion_event_create(v,a[2].i,0,a[3].f,a[4].f,0,a[0].j,a[1].j);
  return out->l!=0||dvm_exception(v);
 }
 struct dvm_object*o=dvm__obj(v,self);motion_event*e=me_data(o);
 if(!e)return false;
 {
  static int logged;
  if(logged<64){logged++;FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
   if(f){fprintf(f,"MOTIONEVENT_CALL name=%s sig=%s action=%d count=%u x=%.1f y=%.1f\n",m,sig,e->action,(unsigned)e->count,e->pts[0].x,e->pts[0].y);fclose(f);}}
 }
 if(!strcmp(m,"getAction")&&!strcmp(sig,"()I")&&n==0){out->i=e->action;return true;}
 if(!strcmp(m,"getActionMasked")&&!strcmp(sig,"()I")&&n==0){out->i=e->action&0xff;return true;}
 if(!strcmp(m,"getActionIndex")&&!strcmp(sig,"()I")&&n==0){out->i=(e->action&0xff00)>>8;return true;}
 if(!strcmp(m,"getPointerCount")&&!strcmp(sig,"()I")&&n==0){out->i=(int)e->count;return true;}
 if(!strcmp(m,"getPointerId")&&!strcmp(sig,"(I)I")&&n==1){
  int i=a[0].i;if(i<0||(uint32_t)i>=e->count){dvm__throw(v,"java/lang/IllegalArgumentException","pointer index");return true;}
  out->i=e->pts[i].id;return true;
 }
 if((!strcmp(m,"getX")||!strcmp(m,"getY"))&&(!strcmp(sig,"()F")||!strcmp(sig,"(I)F"))){
  int i=0;if(!strcmp(sig,"(I)F")){if(n!=1)return false;i=a[0].i;}
  if(i<0||(uint32_t)i>=e->count){dvm__throw(v,"java/lang/IllegalArgumentException","pointer index");return true;}
  out->f=!strcmp(m,"getX")?e->pts[i].x:e->pts[i].y;return true;
 }
 if((!strcmp(m,"getRawX")||!strcmp(m,"getRawY"))&&!strcmp(sig,"()F")&&n==0){
  out->f=!strcmp(m,"getRawX")?e->pts[0].raw_x:e->pts[0].raw_y;return true;
 }
 if((!strcmp(m,"getPressure")||!strcmp(m,"getSize"))&&(!strcmp(sig,"()F")||!strcmp(sig,"(I)F"))){
  int i=0;if(!strcmp(sig,"(I)F")){if(n!=1)return false;i=a[0].i;}
  if(i<0||(uint32_t)i>=e->count){dvm__throw(v,"java/lang/IllegalArgumentException","pointer index");return true;}
  out->f=!strcmp(m,"getPressure")?e->pts[i].pressure:e->pts[i].size;return true;
 }
 if(!strcmp(m,"getEventTime")&&!strcmp(sig,"()J")&&n==0){out->j=e->event_time;return true;}
 if(!strcmp(m,"getDownTime")&&!strcmp(sig,"()J")&&n==0){out->j=e->down_time;return true;}
 if(!strcmp(m,"getSource")&&!strcmp(sig,"()I")&&n==0){out->i=e->source;return true;}
 if(!strcmp(m,"getDeviceId")&&!strcmp(sig,"()I")&&n==0){out->i=e->device_id;return true;}
 if(!strcmp(m,"getMetaState")&&!strcmp(sig,"()I")&&n==0){out->i=0;return true;}
 if(!strcmp(m,"getFlags")&&!strcmp(sig,"()I")&&n==0){out->i=e->flags;return true;}
 if(!strcmp(m,"recycle")&&!strcmp(sig,"()V")&&n==0){return true;}
 return false;
}
bool view_state_runtime_vm_start(struct dvm *v)
{
 if(!v)return false;
 pthread_mutex_lock(&mu);
 if(state.vm){bool same=state.vm==v;pthread_mutex_unlock(&mu);return same;}
 struct dvm_class *view=dvm__class_by_desc(v,"Landroid/view/View;");
 struct dvm_class *group=dvm__class_by_desc(v,"Landroid/view/ViewGroup;");
 struct dvm_class *frame=dvm__class_by_desc(v,"Landroid/widget/FrameLayout;");
 struct dvm_class *context=dvm__class_by_desc(v,"Landroid/content/Context;");
 struct dvm_class *object=dvm__class_by_desc(v,"Ljava/lang/Object;");
 struct dvm_class *input_event=dvm__class_by_desc(v,"Landroid/view/InputEvent;");
 struct dvm_class *motion_event_cls=dvm__class_by_desc(v,"Landroid/view/MotionEvent;");
 if(!view||!group||!frame||!context||!parent_is_compatible(group,view)||!parent_is_compatible(frame,group)){
  pthread_mutex_unlock(&mu);return false;
 }
 group->super=view;frame->super=group;state.vm=v;state.view=view;state.group=group;state.frame=frame;state.context=context;
 if(input_event&&motion_event_cls){
  if(object&&!input_event->super)input_event->super=object;
  motion_event_cls->super=input_event;
  if(!motion_event_cls->sfields){
   static const struct { const char*name; int value; } me_consts[]={
    {"ACTION_DOWN",0},{"ACTION_UP",1},{"ACTION_MOVE",2},{"ACTION_CANCEL",3},
    {"ACTION_OUTSIDE",4},{"ACTION_POINTER_DOWN",5},{"ACTION_POINTER_UP",6},
    {"ACTION_MASK",255},{"ACTION_POINTER_INDEX_MASK",65280},
    {"ACTION_POINTER_INDEX_SHIFT",8},{"ACTION_HOVER_MOVE",7},{"ACTION_SCROLL",8}};
   size_t nc=sizeof me_consts/sizeof me_consts[0];
   motion_event_cls->sfields=calloc(nc,sizeof *motion_event_cls->sfields);
   motion_event_cls->sslots=calloc(nc,sizeof *motion_event_cls->sslots);
   if(motion_event_cls->sfields&&motion_event_cls->sslots){
    for(size_t i=0;i<nc;i++){
     struct dvm_field*f=&motion_event_cls->sfields[i];
     f->cls=motion_event_cls;f->name=me_consts[i].name;f->type="I";f->kind='I';
     f->width=1;f->access=0x19;f->slot=(uint16_t)i;
     motion_event_cls->sslots[i].i=me_consts[i].value;
    }
    motion_event_cls->nsfields=motion_event_cls->nsslots=(int)nc;
   }
  }
 }
 pthread_mutex_unlock(&mu);return true;
}
void view_state_runtime_vm_shutdown(struct dvm *v)
{
 pthread_mutex_lock(&mu);if(state.vm==v){for(int i=0;i<VIEW_CAP;i++)if(state.entries[i].description)dvm_unpin(v,state.entries[i].description);memset(&state,0,sizeof state);}pthread_mutex_unlock(&mu);
}
static view_entry *find(dvm_ref view){for(int i=0;i<VIEW_CAP;i++)if(state.entries[i].view==view)return &state.entries[i];return NULL;}
static bool is_view(struct dvm *v,dvm_ref ref,struct dvm_class *type)
{
 struct dvm_object *o=dvm__obj(v,ref);return o&&o->cls&&dvm__class_assignable(v,o->cls,type);
}
static bool owner_on_chain(struct dvm *v,dvm_ref ref,const char *owner)
{
 struct dvm_object *o=dvm__obj(v,ref);
 for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super){
  if(c->name && !strcmp(c->name,owner))return true;
  if(c==state.view)break;
 }
 return false;
}
dvm_ref view_state_runtime_create_decor(struct dvm *v,dvm_ref context)
{
 if(!v||!context){if(v)dvm__throw(v,"java/lang/NullPointerException","DecorView requires Context");return 0;}
 pthread_mutex_lock(&mu);
 if(state.vm!=v || !is_view(v,context,state.context)){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","invalid DecorView context or VM");return 0;}
 view_entry *available=NULL;
 for(int i=0;i<VIEW_CAP;i++){
  view_entry *e=&state.entries[i];
  if(e->view&&e->decor&&e->context==context){dvm_ref result=e->view;pthread_mutex_unlock(&mu);return result;}
  if(!e->view&&!available)available=e;
 }
 if(!available||!next_generation){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/OutOfMemoryError","DecorView capacity exhausted");return 0;}
 dvm_ref ref=dvm_new_object(v,state.view);
 if(ref){available->view=ref;available->context=context;available->id=-1;available->decor=1;available->generation=next_generation++;}
 pthread_mutex_unlock(&mu);return ref;
}

static void dirty_ancestors(view_entry *e)
{
 for(unsigned i=0;e&&i<VIEW_CAP;i++){e->layout_requested=1;e=e->parent?find(e->parent):NULL;}
}
/* Caller holds mu; collect and validate before publishing any result/state. */
static int collect_tree(dvm_ref root,dvm_ref *list)
{
 view_entry *first=find(root);if(!first)return -1;
 dvm_ref stack[VIEW_CAP];unsigned top=0,count=0;stack[top++]=root;
 while(top){
  dvm_ref ref=stack[--top];view_entry *e=find(ref);if(!e||count==VIEW_CAP||e->child_count>CHILD_CAP)return -1;
  for(unsigned i=0;i<count;i++)if(list[i]==ref)return -1;
  list[count++]=ref;
  for(unsigned i=e->child_count;i>0;i--){view_entry *child=find(e->children[i-1]);if(!child||child->parent!=ref||top==VIEW_CAP)return -1;stack[top++]=child->view;}
 }
 return (int)count;
}
int view_state_runtime_snapshot_tree(struct dvm *v,dvm_ref root,dvm_ref *out,size_t cap)
{
 if(!v||!root||!out||!dvm_gil_held(v))return -1;
 dvm_ref refs[VIEW_CAP];pthread_mutex_lock(&mu);int n=state.vm==v?collect_tree(root,refs):-1;
 if(n>=0&&(size_t)n<=cap)memcpy(out,refs,(size_t)n*sizeof *out);else n=-1;
 pthread_mutex_unlock(&mu);return n;
}
bool view_state_runtime_set_attached(struct dvm *v,dvm_ref root,bool attached)
{
 if(!v||!root||!dvm_gil_held(v))return false;
 dvm_ref refs[VIEW_CAP];pthread_mutex_lock(&mu);view_entry *e=state.vm==v?find(root):NULL;
 int n=e&&!e->parent?collect_tree(root,refs):-1;
 if(n>=0)for(int i=0;i<n;i++)find(refs[i])->attached=attached?1:0;
 pthread_mutex_unlock(&mu);return n>=0;
}
bool view_state_runtime_call(struct dvm*v,const char*owner,const char*m,const char*sig,dvm_ref self,const union dvm_value*a,int n,union dvm_value*out)
{
 if(!v||!owner||!m||!sig)return false;
 if(!strcmp(owner,"android/view/MotionEvent")||!strcmp(owner,"android/view/InputEvent")){
  if(me_call(v,m,sig,self,a,n,out))return true;
 }
 int view_ctor=!strcmp(owner,"android/view/View")&&!strcmp(m,"<init>")&&!strcmp(sig,"(Landroid/content/Context;)V");
 int frame_ctor=!strcmp(owner,"android/widget/FrameLayout")&&!strcmp(m,"<init>")&&!strcmp(sig,"(Landroid/content/Context;)V");
 int ctor=view_ctor||frame_ctor;
 int getcontext=!strcmp(m,"getContext")&&!strcmp(sig,"()Landroid/content/Context;");
 int getresources=!strcmp(m,"getResources")&&!strcmp(sig,"()Landroid/content/res/Resources;");
 int forcelayout=!strcmp(m,"forceLayout")&&!strcmp(sig,"()V");
 int layoutrequested=!strcmp(m,"isLayoutRequested")&&!strcmp(sig,"()Z");
 int setbackground=!strcmp(m,"setBackgroundColor")&&!strcmp(sig,"(I)V");
 int setid=!strcmp(m,"setId")&&!strcmp(sig,"(I)V"),getid=!strcmp(m,"getId")&&!strcmp(sig,"()I");
 int setfocus=!strcmp(m,"setFocusable")&&!strcmp(sig,"(Z)V"),getfocus=!strcmp(m,"isFocusable")&&!strcmp(sig,"()Z");
 int settouch=!strcmp(m,"setFocusableInTouchMode")&&!strcmp(sig,"(Z)V"),gettouch=!strcmp(m,"isFocusableInTouchMode")&&!strcmp(sig,"()Z");
 int setdesc=!strcmp(m,"setContentDescription")&&!strcmp(sig,"(Ljava/lang/CharSequence;)V"),getdesc=!strcmp(m,"getContentDescription")&&!strcmp(sig,"()Ljava/lang/CharSequence;");
 int property=setid||getid||setfocus||getfocus||settouch||gettouch||setdesc||getdesc;
 int setsystem=!strcmp(m,"setSystemUiVisibility")&&!strcmp(sig,"(I)V");
 int getsystem=!strcmp(m,"getSystemUiVisibility")&&!strcmp(sig,"()I");
 int addparams=!strcmp(m,"addView")&&!strcmp(sig,"(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
 int setparams=!strcmp(m,"setLayoutParams")&&!strcmp(sig,"(Landroid/view/ViewGroup$LayoutParams;)V"),getparams=!strcmp(m,"getLayoutParams")&&!strcmp(sig,"()Landroid/view/ViewGroup$LayoutParams;");
 int addchild=addparams||(!strcmp(m,"addView")&&!strcmp(sig,"(Landroid/view/View;)V"));
 int removechild=!strcmp(m,"removeView")&&!strcmp(sig,"(Landroid/view/View;)V");
 int frontchild=!strcmp(m,"bringChildToFront")&&!strcmp(sig,"(Landroid/view/View;)V");
 int childcount=!strcmp(m,"getChildCount")&&!strcmp(sig,"()I");
 int childat=!strcmp(m,"getChildAt")&&!strcmp(sig,"(I)Landroid/view/View;");
 int getparent=!strcmp(m,"getParent")&&!strcmp(sig,"()Landroid/view/ViewParent;");
 int tree=addchild||removechild||frontchild||childcount||childat||getparent;
 int ui=(!strcmp(m,"addView") && (!strcmp(sig,"(Landroid/view/View;)V") || !strcmp(sig,"(Landroid/view/View;I)V") || !strcmp(sig,"(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V"))) ||
  (!strcmp(m,"removeView") && !strcmp(sig,"(Landroid/view/View;)V")) ||
  (!strcmp(m,"requestFocus") && !strcmp(sig,"()Z")) ||
  (!strcmp(m,"setLayoutParams") && !strcmp(sig,"(Landroid/view/ViewGroup$LayoutParams;)V")) ||
  (!strcmp(m,"bringChildToFront") && !strcmp(sig,"(Landroid/view/View;)V"));
 if(!ctor&&!getcontext&&!getresources&&!ui&&!setsystem&&!getsystem&&!forcelayout&&!layoutrequested&&!setbackground&&!tree&&!property&&!setparams&&!getparams)return false;
 pthread_mutex_lock(&mu);
 if(state.vm!=v){pthread_mutex_unlock(&mu);if(ctor){dvm__throw(v,"java/lang/IllegalStateException","view state VM unavailable");return true;}return false;}
 view_entry *e=self?find(self):NULL;
 if(ctor){
  if(!self||!is_view(v,self,frame_ctor?state.frame:state.view)){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","view constructor receiver has wrong class");return true;}
  if(n!=1||!a||!a[0].l){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/NullPointerException","view constructor requires Context");return true;}
  if(!is_view(v,a[0].l,state.context)){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","view constructor context is not Context");return true;}
  if(e){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","view already constructed");return true;}
  for(int i=0;i<VIEW_CAP;i++)if(!state.entries[i].view){e=&state.entries[i];break;}
  if(!e){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/OutOfMemoryError","view state capacity exhausted");return true;}
  if(!next_generation){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","view generation exhausted");return true;}
  e->view=self;e->context=a[0].l;e->id=-1;e->generation=next_generation++;pthread_mutex_unlock(&mu);return true;
 }
 if(!e || !owner_on_chain(v,self,owner)){pthread_mutex_unlock(&mu);return false;}
 if(setparams||getparams){
  if(getparams){if(n||!out){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","getLayoutParams arguments");return true;}out->l=e->layout_params;pthread_mutex_unlock(&mu);return true;}
  if(n!=1||!a||!layout_params_runtime_valid(v,a[0].l)){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","invalid layout params");return true;}
  if(e->attached){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/UnsupportedOperationException","attached layout requires host");return true;}
  dvm_pin(v,a[0].l);if(e->layout_params)dvm_unpin(v,e->layout_params);e->layout_params=a[0].l;dirty_ancestors(e);pthread_mutex_unlock(&mu);return true;
 }
 if(property){
  bool setter=setid||setfocus||settouch||setdesc;
  if((setter&&(n!=1||!a))||(!setter&&(n!=0||!out))){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","View property arguments");return true;}
  if(setdesc){
   dvm_ref desc=a[0].l;
   if(desc){struct dvm_object *o=dvm__obj(v,desc);struct dvm_class *cs=dvm__class_by_desc(v,"Ljava/lang/CharSequence;");
    if(!o||!o->cls||(!dvm__class_assignable(v,o->cls,cs)&&strcmp(o->cls->name,"java/lang/String")&&strcmp(o->cls->name,"java/lang/StringBuilder"))){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","content description must implement CharSequence");return true;}
    dvm_pin(v,desc);
   }
   if(e->description)dvm_unpin(v,e->description);e->description=desc;
  }else if(setid)e->id=a[0].i;
  else if(setfocus)e->focusable=a[0].i!=0;
  else if(settouch){e->focusable_touch=a[0].i!=0;if(e->focusable_touch)e->focusable=1;}
  else if(getid)out->i=e->id;
  else if(getfocus)out->i=e->focusable;
  else if(gettouch)out->i=e->focusable_touch;
  else out->l=e->description;
  pthread_mutex_unlock(&mu);return true;
 }
 if(tree){
  if(getparent){if(n!=0||!out){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","getParent arguments");return true;}out->l=e->parent;pthread_mutex_unlock(&mu);return true;}
  if(!is_view(v,self,state.group)){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","receiver is not ViewGroup");return true;}
  if(childcount){if(n!=0||!out){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","getChildCount arguments");return true;}out->i=(int)e->child_count;pthread_mutex_unlock(&mu);return true;}
  if(childat){if(n!=1||!a||!out){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","getChildAt arguments");return true;}int index=a[0].i;out->l=index<0||(unsigned)index>=e->child_count?0:e->children[index];pthread_mutex_unlock(&mu);return true;}
  if(n!=(addparams?2:1)||!a||!a[0].l||(addparams&&!layout_params_runtime_valid(v,a[1].l))){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","child or layout params invalid");return true;}
  view_entry *child=find(a[0].l);
  if(!child||!is_view(v,a[0].l,state.view)){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","child View is not constructed");return true;}
  if(e->attached||e->decor||child->attached||child->decor){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/UnsupportedOperationException","attached View tree mutation requires host");return true;}
  if(addchild){
   if(child->parent){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","child already has a parent");return true;}
   dvm_ref walk=self;unsigned steps=0;while(walk && steps++<VIEW_CAP){if(walk==child->view){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","View tree cycle");return true;}view_entry *ancestor=find(walk);walk=ancestor?ancestor->parent:0;}
   if(walk){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","invalid View tree ancestry");return true;}
   if(e->child_count==CHILD_CAP){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","ViewGroup child capacity exhausted");return true;}
   if(addparams){dvm_pin(v,a[1].l);if(child->layout_params)dvm_unpin(v,child->layout_params);child->layout_params=a[1].l;}e->children[e->child_count++]=child->view;child->parent=self;dirty_ancestors(e);pthread_mutex_unlock(&mu);return true;
  }
  unsigned index=0;while(index<e->child_count&&e->children[index]!=child->view)index++;
  if(index<e->child_count){
   for(unsigned j=index+1;j<e->child_count;j++)e->children[j-1]=e->children[j];
   if(frontchild)e->children[e->child_count-1]=child->view;
   else {e->children[--e->child_count]=0;child->parent=0;}
   dirty_ancestors(e);
  }
  pthread_mutex_unlock(&mu);return true;
 }
 if(!strcmp(m,"requestFocus")&&!strcmp(sig,"()Z")){
  if(n!=0||!out){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","requestFocus arguments");return true;}
  bool unity=owner_on_chain(v,self,"com/unity3d/player/UnityPlayer"),surface=owner_on_chain(v,self,"android/view/SurfaceView");
  if(surface){view_entry *ancestor=e;unsigned steps=0;while(ancestor&&steps++<VIEW_CAP&&!owner_on_chain(v,ancestor->view,"com/unity3d/player/UnityPlayer"))ancestor=ancestor->parent?find(ancestor->parent):NULL;surface=ancestor!=NULL;}
  if(!unity&&!surface){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/UnsupportedOperationException","focus outside Unity view tree");return true;}
  if(!e->attached){out->i=0;pthread_mutex_unlock(&mu);return true;}
  view_focus_host callback=focus_host;void *user=focus_user;uint64_t generation=e->generation,vmserial=v->serial;
  if(!callback){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/UnsupportedOperationException","focus host unavailable");return true;}
  if(e->busy){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","view request in progress");return true;}
  e->busy=1;dvm_pin(v,self);pthread_mutex_unlock(&mu);
  unsigned depth=dvm_gil_held(v)?dvm_gil_unlock_all(v):0;int status=callback(user,self);if(depth)dvm_gil_relock(v,depth);
  pthread_mutex_lock(&mu);e=state.vm==v&&v->serial==vmserial?find(self):NULL;bool valid=e&&e->generation==generation&&e->attached&&e->busy;if(e&&e->generation==generation)e->busy=0;pthread_mutex_unlock(&mu);dvm_unpin(v,self);
  if(!valid||status<0){dvm__throw(v,"java/lang/UnsupportedOperationException","focus host request failed");return true;}out->i=status==1;return true;
 }
 if(forcelayout){if(n!=0){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","forceLayout arguments");return true;}e->layout_requested=1;pthread_mutex_unlock(&mu);return true;}
 if(layoutrequested){if(n!=0||!out){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","isLayoutRequested arguments");return true;}out->i=e->layout_requested;pthread_mutex_unlock(&mu);return true;}
 if(setbackground){if(n!=1||!a){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","setBackgroundColor arguments");return true;}e->background_color=(uint32_t)a[0].i;e->background_set=1;pthread_mutex_unlock(&mu);sdk_trace_fprintf(stderr,"VIEW_BACKGROUND_COLOR ref=%u argb=0x%08x render_pending=1 host_rendered=0\n",self,(unsigned)a[0].i);return true;}
 if(setsystem||getsystem){
  if(!e->decor){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/UnsupportedOperationException","system UI only supported for DecorView");return true;}
  if(getsystem){if(!out){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","missing system UI result");return true;}out->i=e->applied;pthread_mutex_unlock(&mu);return true;}
  if(!a||n!=1){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","missing system UI flags");return true;}
  int flags=a[0].i;e->requested=flags;
  /* LOW_PROFILE has no separate OH dim-icons bridge. It is redundant only
   * while BOTH bars are requested hidden; reject the standalone hint. */
  if(((unsigned)flags & ~SUPPORTED_SYSTEM_UI_FLAGS)!=0 || ((flags&1)&&(flags&6)!=6)){pthread_mutex_unlock(&mu);sdk_trace_fprintf(stderr,"DECOR_SYSTEM_UI rawflags=0x%x result=unsupported\n",(unsigned)flags);dvm__throw(v,"java/lang/UnsupportedOperationException","unsupported system UI flags 0x%x",(unsigned)flags);return true;}
  if(e->busy){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","system UI request already in progress");return true;}
  view_system_ui_host callback=system_ui_host;void *user=system_ui_user;uint64_t generation=e->generation;
  if(!callback){pthread_mutex_unlock(&mu);sdk_trace_fprintf(stderr,"DECOR_SYSTEM_UI rawflags=0x%x result=no_host\n",(unsigned)flags);dvm__throw(v,"java/lang/UnsupportedOperationException","system UI host unavailable");return true;}
  e->busy=1;pthread_mutex_unlock(&mu);
  unsigned depth=dvm_gil_held(v)?dvm_gil_unlock_all(v):0;
  int status=callback(user,flags);
  if(depth)dvm_gil_relock(v,depth);
  pthread_mutex_lock(&mu);e=state.vm==v?find(self):NULL;
  bool valid=e&&e->decor&&e->generation==generation&&e->busy;
  if(valid){e->busy=0;if(!status){e->applied=flags;e->applied_valid=1;}}
  pthread_mutex_unlock(&mu);
  sdk_trace_fprintf(stderr,"DECOR_SYSTEM_UI rawflags=0x%x host_result=%d identity_valid=%d os_request_accepted=%d visibility_verified=0\n",(unsigned)flags,status,valid,valid&&!status);
  if(!valid || status)dvm__throw(v,"java/lang/UnsupportedOperationException","system UI host request failed (%d)",status);
  return true;
 }
 dvm_ref context=e->context;pthread_mutex_unlock(&mu);
 if(ui){dvm__throw(v,"java/lang/UnsupportedOperationException","%s requires host view backend",m);return true;}
 if(!out){dvm__throw(v,"java/lang/IllegalArgumentException","view result storage missing");return true;}
 if(getcontext){out->l=context;return true;}
 if(!v->hooks.call_external || !v->hooks.call_external(v->hooks.user,v,"android/content/Context","getResources","()Landroid/content/res/Resources;",context,NULL,0,out)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","Context resources backend unavailable");
 }
 return true;
}
static void remove_entry(struct dvm *v,dvm_ref ref){pthread_mutex_lock(&mu);if(state.vm==v){view_entry*e=find(ref);if(e){if(e->description)dvm_unpin(v,e->description);if(e->layout_params)dvm_unpin(v,e->layout_params);memset(e,0,sizeof(*e));}}pthread_mutex_unlock(&mu);}
static const char *class_name(const struct dvm_class *c){return c&&c->name?c->name:"<null>";}
static void selftest_check(struct dvm *v,bool pass,const char *stage,const char *expr,int line,bool *ok)
{
 dvm_ref exc=v?dvm_exception(v):0;char detail[512]="-";
 if(exc)dvm_describe_exception(v,exc,detail,sizeof detail);
 sdk_trace_fprintf(stderr,"VIEW_STATE_CHECK stage=%s result=%s line=%d expression=%s exception=%u exception_text=%s\n",stage,pass?"PASS":"FAIL",line,expr,exc,detail);
 if(!pass)*ok=false;
 if(exc)dvm_clear_exception(v);
}
#define VCHECK(stage,expr) do {bool check_value=!!(expr);selftest_check(v,check_value,stage,#expr,__LINE__,&ok);}while(0)
bool view_state_runtime_selftest(struct dvm*v,dvm_ref context)
{
 bool ok=true;
 if(!v||!context||dvm_exception(v)){
  selftest_check(v,false,"precondition","vm && context && !exception",__LINE__,&ok);return false;
 }
 pthread_mutex_lock(&mu);struct dvm_class *frame=state.vm==v?state.frame:NULL;
 struct dvm_class *group=state.vm==v?state.group:NULL,*view=state.vm==v?state.view:NULL;
 struct dvm_class *context_base=state.vm==v?state.context:NULL;pthread_mutex_unlock(&mu);
 struct dvm_object *ctx=dvm__obj(v,context);
 sdk_trace_fprintf(stderr,"VIEW_STATE_TYPES frame=%s frame_super=%s group=%s group_super=%s view=%s context_class=%s context_super=%s context_base=%s context_ref=%u\n",
  class_name(frame),class_name(frame?frame->super:NULL),class_name(group),class_name(group?group->super:NULL),class_name(view),
  class_name(ctx?ctx->cls:NULL),class_name(ctx&&ctx->cls?ctx->cls->super:NULL),class_name(context_base),context);
 VCHECK("class_chain",frame&&group&&view&&frame->super==group&&group->super==view);
 if(dvm_gil_held(v)){
  struct dvm_class *unity=dvm__class_by_desc(v,"Lcom/unity3d/player/UnityPlayer;");
  if(unity&&dvm__class_assignable(v,unity,view)){
   dvm_ref focus_view=dvm_new_object(v,unity);union dvm_value fa={0},fo={0};fa.l=context;
   bool fh=view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",focus_view,&fa,1,&fo);
   VCHECK("focus_construct",fh&&!dvm_exception(v));fo.i=-1;
   fh=view_state_runtime_call(v,"android/view/View","requestFocus","()Z",focus_view,NULL,0,&fo);
   VCHECK("focus_unattached_false",fh&&!dvm_exception(v)&&fo.i==0);
   pthread_mutex_lock(&mu);view_focus_host saved=focus_host;void *saved_user=focus_user;focus_host=NULL;pthread_mutex_unlock(&mu);
   VCHECK("focus_attach_test",view_state_runtime_set_attached(v,focus_view,true));
   fh=view_state_runtime_call(v,"android/view/View","requestFocus","()Z",focus_view,NULL,0,&fo);
   VCHECK("focus_no_host_rejected",fh&&dvm_exception(v));
   (void)view_state_runtime_set_attached(v,focus_view,false);remove_entry(v,focus_view);
   pthread_mutex_lock(&mu);focus_host=saved;focus_user=saved_user;pthread_mutex_unlock(&mu);
  }
 }
 if(!frame)return false;
 dvm_ref ref=dvm_new_object(v,frame);VCHECK("allocate",ref!=0&&!dvm_exception(v));if(!ref)return false;
 union dvm_value arg={.l=context},out={0};
 bool handled=view_state_runtime_call(v,"android/widget/FrameLayout","<init>","(Landroid/content/Context;)V",ref,&arg,1,&out);
 VCHECK("construct",handled&&!dvm_exception(v));
 handled=view_state_runtime_call(v,"android/view/View","getContext","()Landroid/content/Context;",ref,NULL,0,&out);
 sdk_trace_fprintf(stderr,"VIEW_STATE_VALUES stage=context handled=%d actual=%u expected=%u\n",handled,out.l,context);
 VCHECK("context_identity",handled&&out.l==context&&!dvm_exception(v));
 union dvm_value actual={0},delegated={0};
 bool backend=v->hooks.call_external && v->hooks.call_external(v->hooks.user,v,"android/content/Context","getResources","()Landroid/content/res/Resources;",context,NULL,0,&actual);
 VCHECK("context_resources",backend&&actual.l&&!dvm_exception(v));
 bool via_view=view_state_runtime_call(v,"android/view/View","getResources","()Landroid/content/res/Resources;",ref,NULL,0,&delegated);
 sdk_trace_fprintf(stderr,"VIEW_STATE_VALUES stage=resources backend=%d via_view=%d actual=%u delegated=%u\n",backend,via_view,actual.l,delegated.l);
 VCHECK("resources_identity",via_view&&actual.l&&actual.l==delegated.l&&!dvm_exception(v));
 handled=view_state_runtime_call(v,"java/lang/Object","getContext","()Landroid/content/Context;",ref,NULL,0,&out);
 VCHECK("unrelated_owner",!handled&&!dvm_exception(v));
 handled=view_state_runtime_call(v,"android/view/ViewGroup","addView","(I)V",ref,&arg,1,&out);
 VCHECK("unknown_signature",!handled&&!dvm_exception(v));
 handled=view_state_runtime_call(v,"android/widget/FrameLayout","<init>","(Landroid/content/Context;)V",ref,&arg,1,&out);
 VCHECK("duplicate_rejected",handled&&dvm_exception(v));

 dvm_ref invalid=dvm_new_object(v,frame);VCHECK("allocate_invalid",invalid!=0&&!dvm_exception(v));if(!invalid)return false;
 arg.l=ref;handled=view_state_runtime_call(v,"android/widget/FrameLayout","<init>","(Landroid/content/Context;)V",invalid,&arg,1,&out);
 VCHECK("foreign_context_rejected",handled&&dvm_exception(v));
 arg.l=0;handled=view_state_runtime_call(v,"android/widget/FrameLayout","<init>","(Landroid/content/Context;)V",invalid,&arg,1,&out);
 VCHECK("null_context_rejected",handled&&dvm_exception(v));remove_entry(v,invalid);
 /* Real View super-construction for a guest subclass, without touching its
  * object->data or fields. Both contexts retain independent identity. */
 struct dvm_class *sub=dvm__class_by_desc(v,"Lsdktrace/tests/PlainViewSubclass;");
 if(!sub){VCHECK("plain_view_subclass",false);return false;}
 sub->super=view;dvm_ref first=dvm_new_object(v,sub),second=dvm_new_object(v,view);
 dvm_ref context2=dvm_new_object(v,context_base);VCHECK("plain_view_allocate",first&&second&&context2&&!dvm_exception(v));
 if(!first||!second||!context2)return false;
 arg.l=context;handled=view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",first,&arg,1,&out);
 VCHECK("plain_view_construct",handled&&!dvm_exception(v));
 arg.l=context2;handled=view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",second,&arg,1,&out);
 VCHECK("plain_view_second",handled&&!dvm_exception(v));
 out.l=0;handled=view_state_runtime_call(v,"android/view/View","getContext","()Landroid/content/Context;",first,NULL,0,&out);
 VCHECK("plain_view_context_first",handled&&!dvm_exception(v)&&out.l==context);
 out.l=0;handled=view_state_runtime_call(v,"android/view/View","getContext","()Landroid/content/Context;",second,NULL,0,&out);
 VCHECK("plain_view_context_second",handled&&!dvm_exception(v)&&out.l==context2&&context!=context2);
 union dvm_value plain_resources={0};handled=view_state_runtime_call(v,"android/view/View","getResources","()Landroid/content/res/Resources;",first,NULL,0,&plain_resources);
 VCHECK("plain_view_resources",handled&&!dvm_exception(v)&&actual.l&&plain_resources.l==actual.l);
 arg.l=context;handled=view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",first,&arg,1,&out);
 VCHECK("plain_view_duplicate",handled&&dvm_exception(v));
 arg.l=first;handled=view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",invalid,&arg,1,&out);
 VCHECK("plain_view_wrong_context",handled&&dvm_exception(v));
 arg.l=first;handled=view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",ref,&arg,1,&out);
 VCHECK("tree_add_first",handled&&!dvm_exception(v));
 arg.l=second;handled=view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",ref,&arg,1,&out);
 VCHECK("tree_add_second",handled&&!dvm_exception(v));
 if(dvm_gil_held(v)){
  dvm_ref snapshot[3]={0};int count=view_state_runtime_snapshot_tree(v,ref,snapshot,3);
  VCHECK("tree_snapshot",count==3&&snapshot[0]==ref&&snapshot[1]==first&&snapshot[2]==second);
  snapshot[0]=0;VCHECK("tree_snapshot_capacity",view_state_runtime_snapshot_tree(v,ref,snapshot,2)<0&&snapshot[0]==0);
  VCHECK("tree_attach",view_state_runtime_set_attached(v,ref,true));
  arg.l=first;handled=view_state_runtime_call(v,"android/view/ViewGroup","removeView","(Landroid/view/View;)V",ref,&arg,1,&out);
  VCHECK("tree_attached_mutation_rejected",handled&&dvm_exception(v));
  VCHECK("tree_detach",view_state_runtime_set_attached(v,ref,false));
 }
 handled=view_state_runtime_call(v,"android/view/ViewGroup","getChildCount","()I",ref,NULL,0,&out);VCHECK("tree_count",handled&&!dvm_exception(v)&&out.i==2);
 arg.i=0;handled=view_state_runtime_call(v,"android/view/ViewGroup","getChildAt","(I)Landroid/view/View;",ref,&arg,1,&out);VCHECK("tree_order",handled&&!dvm_exception(v)&&out.l==first);
 handled=view_state_runtime_call(v,"android/view/View","getParent","()Landroid/view/ViewParent;",first,NULL,0,&out);VCHECK("tree_parent",handled&&!dvm_exception(v)&&out.l==ref);
 arg.l=first;handled=view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",ref,&arg,1,&out);VCHECK("tree_duplicate",handled&&dvm_exception(v));
 arg.l=ref;handled=view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",ref,&arg,1,&out);VCHECK("tree_cycle",handled&&dvm_exception(v));
 arg.l=context;handled=view_state_runtime_call(v,"android/widget/FrameLayout","<init>","(Landroid/content/Context;)V",invalid,&arg,1,&out);VCHECK("tree_subgroup_construct",handled&&!dvm_exception(v));
 arg.l=invalid;handled=view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",ref,&arg,1,&out);VCHECK("tree_subgroup_add",handled&&!dvm_exception(v));
 arg.l=ref;handled=view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",invalid,&arg,1,&out);VCHECK("tree_ancestor_cycle",handled&&dvm_exception(v));
 arg.l=invalid;handled=view_state_runtime_call(v,"android/view/ViewGroup","removeView","(Landroid/view/View;)V",ref,&arg,1,&out);VCHECK("tree_subgroup_remove",handled&&!dvm_exception(v));
 arg.l=first;handled=view_state_runtime_call(v,"android/view/ViewGroup","bringChildToFront","(Landroid/view/View;)V",ref,&arg,1,&out);VCHECK("tree_front",handled&&!dvm_exception(v));
 arg.i=1;handled=view_state_runtime_call(v,"android/view/ViewGroup","getChildAt","(I)Landroid/view/View;",ref,&arg,1,&out);VCHECK("tree_front_order",handled&&!dvm_exception(v)&&out.l==first);
 arg.l=first;handled=view_state_runtime_call(v,"android/view/ViewGroup","removeView","(Landroid/view/View;)V",ref,&arg,1,&out);VCHECK("tree_remove",handled&&!dvm_exception(v));
 handled=view_state_runtime_call(v,"android/view/View","getParent","()Landroid/view/ViewParent;",first,NULL,0,&out);VCHECK("tree_parent_cleared",handled&&!dvm_exception(v)&&!out.l);
 arg.l=second;handled=view_state_runtime_call(v,"android/view/ViewGroup","removeView","(Landroid/view/View;)V",ref,&arg,1,&out);VCHECK("tree_remove_second",handled&&!dvm_exception(v));
 out.i=-1;handled=view_state_runtime_call(v,"android/view/View","isLayoutRequested","()Z",first,NULL,0,&out);
 VCHECK("layout_initial",handled&&!dvm_exception(v)&&out.i==0);
 handled=view_state_runtime_call(v,"android/view/View","forceLayout","()V",first,NULL,0,&out);
 VCHECK("layout_mark",handled&&!dvm_exception(v));
 out.i=-1;handled=view_state_runtime_call(v,"android/view/View","isLayoutRequested","()Z",first,NULL,0,&out);
 VCHECK("layout_requested",handled&&!dvm_exception(v)&&out.i==1);
 out.i=-1;handled=view_state_runtime_call(v,"android/view/View","isLayoutRequested","()Z",second,NULL,0,&out);
 VCHECK("layout_isolation",handled&&!dvm_exception(v)&&out.i==0);
 arg.i=(int32_t)0xff231f20u;handled=view_state_runtime_call(v,"android/view/View","setBackgroundColor","(I)V",first,&arg,1,&out);
 pthread_mutex_lock(&mu);view_entry *first_entry=find(first),*second_entry=find(second);
 bool background_ok=first_entry&&first_entry->background_set&&first_entry->background_color==UINT32_C(0xff231f20)&&second_entry&&!second_entry->background_set;
 pthread_mutex_unlock(&mu);VCHECK("background_color_isolation",handled&&!dvm_exception(v)&&background_ok);
 out.i=0;handled=view_state_runtime_call(v,"android/view/View","getId","()I",first,NULL,0,&out);VCHECK("view_id_default",handled&&out.i==-1&&!dvm_exception(v));
 arg.i=42;handled=view_state_runtime_call(v,"android/view/View","setId","(I)V",first,&arg,1,&out);VCHECK("view_id_set",handled&&!dvm_exception(v));
 out.i=0;handled=view_state_runtime_call(v,"android/view/View","getId","()I",first,NULL,0,&out);VCHECK("view_id_get",handled&&out.i==42&&!dvm_exception(v));
 out.i=0;handled=view_state_runtime_call(v,"android/view/View","getId","()I",second,NULL,0,&out);VCHECK("view_id_isolated",handled&&out.i==-1&&!dvm_exception(v));
 arg.i=1;handled=view_state_runtime_call(v,"android/view/View","setFocusableInTouchMode","(Z)V",first,&arg,1,&out);VCHECK("view_focus_set",handled&&!dvm_exception(v));
 out.i=0;handled=view_state_runtime_call(v,"android/view/View","isFocusable","()Z",first,NULL,0,&out);VCHECK("view_focus_get",handled&&out.i==1&&!dvm_exception(v));
 out.i=0;handled=view_state_runtime_call(v,"android/view/View","isFocusableInTouchMode","()Z",first,NULL,0,&out);VCHECK("view_touch_get",handled&&out.i==1&&!dvm_exception(v));
 arg.l=dvm_new_string(v,"content description");dvm_ref description=arg.l;
 handled=view_state_runtime_call(v,"android/view/View","setContentDescription","(Ljava/lang/CharSequence;)V",first,&arg,1,&out);VCHECK("view_description_set",handled&&!dvm_exception(v));
 out.l=0;handled=view_state_runtime_call(v,"android/view/View","getContentDescription","()Ljava/lang/CharSequence;",first,NULL,0,&out);VCHECK("view_description_get",handled&&out.l==description&&!dvm_exception(v));
 arg.l=0;handled=view_state_runtime_call(v,"android/view/View","setContentDescription","(Ljava/lang/CharSequence;)V",first,&arg,1,&out);VCHECK("view_description_clear",handled&&!dvm_exception(v));
 remove_entry(v,first);remove_entry(v,second);remove_entry(v,invalid);remove_entry(v,ref);
 return ok;
}

struct mock_system_ui {struct dvm *vm;int calls,result,last,held;};
static int system_ui_mock(void *user,int flags){struct mock_system_ui *m=user;m->calls++;m->last=flags;m->held=dvm_gil_held(m->vm);return m->result;}
bool view_state_system_ui_selftest(struct dvm*v,dvm_ref context)
{
 if(!v||!context||dvm_exception(v))return false;
 pthread_mutex_lock(&mu);bool free_decor=state.vm==v;for(int i=0;i<VIEW_CAP;i++)if(state.entries[i].view&&state.entries[i].decor&&state.entries[i].context==context)free_decor=false;
 view_system_ui_host old=system_ui_host;void *old_user=system_ui_user;pthread_mutex_unlock(&mu);
 if(!free_decor)return false;
 bool ok=true;struct mock_system_ui mock={.vm=v};dvm_ref decor=view_state_runtime_create_decor(v,context);
 VCHECK("decor_mock_create",decor&&!dvm_exception(v));if(!decor)return false;
 dvm_ref again=view_state_runtime_create_decor(v,context);VCHECK("decor_mock_stable",again==decor&&!dvm_exception(v));
 union dvm_value arg={.i=0x706},out={0};sdk_set_view_system_ui_host(system_ui_mock,&mock);
 bool handled=view_state_runtime_call(v,"android/view/View","setSystemUiVisibility","(I)V",decor,&arg,1,&out);
 VCHECK("decor_mock_set",handled&&!dvm_exception(v)&&mock.calls==1&&mock.last==0x706&&!mock.held);
 handled=view_state_runtime_call(v,"android/view/View","getSystemUiVisibility","()I",decor,NULL,0,&out);
 VCHECK("decor_mock_get",handled&&!dvm_exception(v)&&out.i==0x706);
 mock.result=-1;arg.i=4;handled=view_state_runtime_call(v,"android/view/View","setSystemUiVisibility","(I)V",decor,&arg,1,&out);
 VCHECK("decor_mock_rejected",handled&&dvm_exception(v)&&mock.calls==2);
 out.i=-1;handled=view_state_runtime_call(v,"android/view/View","getSystemUiVisibility","()I",decor,NULL,0,&out);
 VCHECK("decor_mock_preserve",handled&&!dvm_exception(v)&&out.i==0x706);
 arg.i=0x1000;handled=view_state_runtime_call(v,"android/view/View","setSystemUiVisibility","(I)V",decor,&arg,1,&out);
 VCHECK("decor_mock_unsupported_flags",handled&&dvm_exception(v)&&mock.calls==2);
 sdk_set_view_system_ui_host(NULL,NULL);arg.i=0;handled=view_state_runtime_call(v,"android/view/View","setSystemUiVisibility","(I)V",decor,&arg,1,&out);
 VCHECK("decor_mock_missing_host",handled&&dvm_exception(v)&&mock.calls==2);
 sdk_set_view_system_ui_host(old,old_user);remove_entry(v,decor);
 sdk_trace_fprintf(stderr,"DECOR_SYSTEM_UI_SELFTEST %s mock=1 host_real=0\n",ok?"PASS":"FAIL");
 return ok;
}
#undef VCHECK



