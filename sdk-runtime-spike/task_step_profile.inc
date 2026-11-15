/* Argument-free exclusive task counts and inclusive single-call costs.
 * Explicit Runnable boundaries: parser budget restores never reset evidence. */
#define TASK_PROFILE_CAP 8192
struct task_profile_entry { const struct dvm_method *method; uint64_t exclusive, calls, completed, max_inclusive, max_exclusive; };
struct task_profile_frame { struct task_profile_frame *previous; struct task_profile_entry *entry; uint64_t start, exclusive; };
struct task_step_profile { struct task_step_profile *previous; struct dvm *vm; dvm_ref task; uint64_t serial,ticks,entry_limit; unsigned overflow; char owner[192]; struct task_profile_frame *frame; struct task_profile_entry entries[TASK_PROFILE_CAP]; };
static _Thread_local struct task_step_profile *task_profile;
static atomic_ullong task_profile_serial;
bool dvm_task_profile_begin(struct dvm *v,dvm_ref task,const char *owner){
 struct task_step_profile *p=calloc(1,sizeof *p);if(!p)return false;
 p->previous=task_profile;p->vm=v;p->task=task;p->entry_limit=v->step_limit;
 p->serial=atomic_fetch_add(&task_profile_serial,1)+1;
 snprintf(p->owner,sizeof p->owner,"%s",owner?owner:"?");task_profile=p;return true;
}
static struct task_profile_entry *task_profile_entry(const struct dvm_method *m){
 struct task_step_profile *p=task_profile;if(!p||!m)return NULL;
 unsigned at=(unsigned)(((uintptr_t)m>>4)*2654435761u)&(TASK_PROFILE_CAP-1);
 for(unsigned i=0;i<TASK_PROFILE_CAP;i++,at=(at+1)&(TASK_PROFILE_CAP-1)){
  struct task_profile_entry *e=&p->entries[at];if(!e->method||e->method==m){e->method=m;return e;}
 }p->overflow++;return NULL;
}
static void task_profile_enter(struct dvm *v,const struct dvm_method *m,struct task_profile_frame *f){
 memset(f,0,sizeof *f);struct task_step_profile *p=task_profile;if(!p||p->vm!=v)return;
 f->entry=task_profile_entry(m);f->start=p->ticks;f->previous=p->frame;p->frame=f;if(f->entry)f->entry->calls++;
}
static void task_profile_tick(struct dvm *v,const struct dvm_method *m){
 struct task_step_profile *p=task_profile;if(!p||p->vm!=v)return;
 p->ticks++;struct task_profile_entry *e=task_profile_entry(m);if(e)e->exclusive++;
 if(p->frame)p->frame->exclusive++;
}
static void task_profile_leave(struct dvm *v,struct task_profile_frame *f){
 struct task_step_profile *p=task_profile;if(!p||p->vm!=v||p->frame!=f)return;
 if(f->entry){uint64_t n=p->ticks-f->start;f->entry->completed++;if(n>f->entry->max_inclusive)f->entry->max_inclusive=n;if(f->exclusive>f->entry->max_exclusive)f->entry->max_exclusive=f->exclusive;}
 p->frame=f->previous;
}
static void task_profile_report(struct dvm *v,const char *reason){
 struct task_step_profile *p=task_profile;if(!p||p->vm!=v)return;
 int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task-step-profile.txt","a");if(!f)return;
 dprintf(fileno(f),"TASK_PROFILE pid=%d id=%llu thread=%u task=%u owner=%s reason=%s executed_dex=%llu charged_steps=%llu current_limit=%llu entry_limit=%llu overflow=%u limit_source=executor_worker_run:step_limit=100000\n",getpid(),(unsigned long long)p->serial,v->cur_thread,p->task,p->owner,reason,(unsigned long long)p->ticks,(unsigned long long)v->call_steps,(unsigned long long)v->step_limit,(unsigned long long)p->entry_limit,p->overflow);
 for(int mode=0;mode<3;mode++){
  unsigned picked[5];int count=0;
  for(int rank=0;rank<5;rank++){
   int best=-1;uint64_t maximum=0,best_self=0;
   for(unsigned i=0;i<TASK_PROFILE_CAP;i++){
    struct task_profile_entry *e=&p->entries[i];if(!e->method)continue;
    bool used=false;for(int j=0;j<count;j++)if(picked[j]==i)used=true;if(used)continue;
    uint64_t incl=e->max_inclusive,self=e->max_exclusive;
    for(struct task_profile_frame *a=p->frame;a;a=a->previous)if(a->entry==e){uint64_t active=p->ticks-a->start;if(active>incl)incl=active;if(a->exclusive>self)self=a->exclusive;}
    uint64_t value=mode==0?e->exclusive:mode==1?incl:self;
    if(best<0||value>maximum){best=(int)i;maximum=value;best_self=self;}
   }
   if(best<0)break;picked[count++]=(unsigned)best;struct task_profile_entry *e=&p->entries[best];
   uint64_t inclusive=e->max_inclusive;
   for(struct task_profile_frame *a=p->frame;a;a=a->previous)if(a->entry==e&&p->ticks-a->start>inclusive)inclusive=p->ticks-a->start;
   dprintf(fileno(f),"TASK_PROFILE_TOP pid=%d id=%llu group=%s rank=%d method=%s.%s%s exclusive_task_steps=%llu calls=%llu completed=%llu max_single_inclusive=%llu max_single_exclusive=%llu metric=%llu\n",getpid(),(unsigned long long)p->serial,mode==0?"task_exclusive":mode==1?"single_call_inclusive":"single_call_exclusive",rank+1,e->method->cls?e->method->cls->name:"?",e->method->name,e->method->sig,(unsigned long long)e->exclusive,(unsigned long long)e->calls,(unsigned long long)e->completed,(unsigned long long)inclusive,(unsigned long long)best_self,(unsigned long long)maximum);
  }
 }
 fclose(f);errno=saved;
}
void dvm_task_profile_end(struct dvm *v){struct task_step_profile *p=task_profile;if(!p||p->vm!=v)return;task_profile_report(v,"return");task_profile=p->previous;free(p);}
bool dvm_task_profile_selftest(struct dvm *v){
 struct dvm_class c={.name="host/selftest/Profile"};
 struct dvm_method a={.cls=&c,.name="repeated",.sig="()V"},b={.cls=&c,.name="child",.sig="()V"};
 uint64_t saved=v->call_steps;struct task_profile_frame f,g;
 if(!dvm_task_profile_begin(v,0,"profile-unit-fixture"))return false;
 task_profile_enter(v,&a,&f);for(int i=0;i<3;i++)task_profile_tick(v,&a);task_profile_leave(v,&f);
 v->call_steps=99999;task_profile_enter(v,&a,&f);for(int i=0;i<2;i++)task_profile_tick(v,&a);
 task_profile_enter(v,&b,&g);for(int i=0;i<4;i++)task_profile_tick(v,&b);task_profile_leave(v,&g);
 v->call_steps=1;for(int i=0;i<3;i++)task_profile_tick(v,&a);task_profile_leave(v,&f);
 struct task_profile_entry *ea=task_profile_entry(&a),*eb=task_profile_entry(&b);
 bool ok=task_profile&&task_profile->ticks==12&&ea->exclusive==8&&ea->calls==2&&ea->completed==2&&ea->max_inclusive==9&&ea->max_exclusive==5&&eb->exclusive==4&&eb->calls==1&&eb->max_inclusive==4;
 FILE *log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task-step-profile-selftest.txt","w");if(log){dprintf(fileno(log),"PROFILE_SELFTEST ok=%d ticks=%llu repeated_exclusive=%llu calls=%llu max_inclusive=%llu max_exclusive=%llu child_exclusive=%llu restore_99999_to_1=1\n",ok,(unsigned long long)task_profile->ticks,(unsigned long long)ea->exclusive,(unsigned long long)ea->calls,(unsigned long long)ea->max_inclusive,(unsigned long long)ea->max_exclusive,(unsigned long long)eb->exclusive);fclose(log);}
 dvm_task_profile_end(v);v->call_steps=saved;return ok;
}
