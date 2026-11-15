#include "gc_runtime.h"
/*
 * android.content.IntentFilter for the DVM.
 *
 * The guest's ConnectivityMonitor does exactly:
 *   new IntentFilter()
 *   setPriority(990)
 *   addAction("android.net.conn.CONNECTIVITY_CHANGE")
 *   Context.registerReceiver(receiver, filter)   // two-argument overload
 * and the packaged DEX also uses the ()/String constructors, addAction,
 * addDataScheme, getAction(int), countActions and hasAction elsewhere.
 *
 * The object state below is real: every list owns its strings and the payload
 * release callback frees them when the VM recycles the object.  Nothing here
 * reports success for an operation it did not perform.  match(...) and
 * <init>(IntentFilter) have no call site in the packaged DEX and are left to
 * the strict runtime's MISSING_API path rather than guessed.
 *
 * battery_runtime also owns a private IntentFilter payload ('BFIL').  This
 * module must run before battery_runtime_call in the dispatch chain, yields
 * the BATTERY_CHANGED action back to it, and refuses to touch payloads it
 * does not own, so the battery receiver bridge keeps its existing behavior.
 */
#include "intent_filter_runtime.h"
#include "android_basics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int sdk_trace_fprintf(FILE*,const char*,...);

#define INTENT_FILTER_MAGIC 0x49465452u /* 'IFTR', distinct from battery 'BFIL' */
#define BATTERY_ACTION "android.intent.action.BATTERY_CHANGED"

typedef struct string_list {
 char **items;
 size_t count;
 size_t capacity;
} string_list;

typedef struct authority_entry {
 char *host;
 char *port; /* NULL means the authority has no port constraint */
} authority_entry;

typedef struct authority_list {
 authority_entry *items;
 size_t count;
 size_t capacity;
} authority_list;

typedef struct intent_filter_payload {
 unsigned magic;
 string_list actions;
 string_list categories;
 string_list schemes;
 string_list types;
 authority_list authorities;
 int priority;
} intent_filter_payload;

static void string_list_free(string_list *l){
 if(!l)return;
 for(size_t i=0;i<l->count;i++)free(l->items[i]);
 free(l->items);
 l->items=NULL;l->count=0;l->capacity=0;
}
static void authority_list_free(authority_list *l){
 if(!l)return;
 for(size_t i=0;i<l->count;i++){free(l->items[i].host);free(l->items[i].port);}
 free(l->items);
 l->items=NULL;l->count=0;l->capacity=0;
}
static void intent_filter_payload_free(void *ptr){
 intent_filter_payload *p=ptr;
 if(!p)return;
 string_list_free(&p->actions);
 string_list_free(&p->categories);
 string_list_free(&p->schemes);
 string_list_free(&p->types);
 authority_list_free(&p->authorities);
 free(p);
}

static void throw_oom(struct dvm *v,const char *what){
 dvm__throw(v,"java/lang/OutOfMemoryError","%s",what);
}

/* Receivers whose registerReceiver this module forwarded to android_basics.
 * unregisterReceiver is only forwarded for those receivers: battery_runtime
 * intercepts every unregisterReceiver call and owns a different receiver, so
 * forwarding unconditionally would cancel the wrong subscription, while
 * returning false unconditionally would let battery_runtime reject the
 * ConnectivityMonitor receiver with IllegalArgumentException. */
typedef struct forwarded_receiver {
 dvm_ref ref;
 unsigned count;
} forwarded_receiver;
typedef struct forwarded_receiver_list {
 forwarded_receiver *items;
 size_t count;
 size_t capacity;
} forwarded_receiver_list;
static forwarded_receiver_list forwarded_receivers;
static struct dvm *forwarded_vm;
static uint64_t forwarded_serial;

void intent_filter_runtime_vm_end(struct dvm *v){
 if(!v||forwarded_vm!=v||forwarded_serial!=v->serial)return;
 bool released=forwarded_receivers.items!=NULL;
 free(forwarded_receivers.items);
 memset(&forwarded_receivers,0,sizeof forwarded_receivers);
 forwarded_vm=NULL;forwarded_serial=0;
 sdk_trace_fprintf(stderr,"FORWARDED_VM_END serial=%llu released=%d\n",(unsigned long long)v->serial,released);
}

static void forwarded_sync(struct dvm *v){
 if(forwarded_vm==v&&forwarded_serial==v->serial)return;
 /* An earlier VM's arena has already reclaimed its table. Never free or
  * inspect that pointer from a later generation, even if malloc reused v.
  * Normal destruction calls vm_end and reaches here with an empty table. */
 if(forwarded_receivers.items)
  sdk_trace_fprintf(stderr,"FORWARDED_STALE_DETACH old_serial=%llu new_serial=%llu\n",(unsigned long long)forwarded_serial,(unsigned long long)v->serial);
 memset(&forwarded_receivers,0,sizeof forwarded_receivers);
 forwarded_vm=v;forwarded_serial=v->serial;
}
static unsigned forwarded_count(struct dvm *v,dvm_ref ref){
 forwarded_sync(v);
 for(size_t i=0;i<forwarded_receivers.count;i++)
  if(forwarded_receivers.items[i].ref==ref)return forwarded_receivers.items[i].count;
 return 0;
}
/* Reserve before the real registration so a successful registration is always
 * recorded without a later allocation failure. */
static bool forwarded_reserve(struct dvm *v){
 forwarded_sync(v);
 if(forwarded_receivers.count<forwarded_receivers.capacity)return true;
 size_t cap=forwarded_receivers.capacity?forwarded_receivers.capacity*2:4;
 if(cap<forwarded_receivers.capacity||cap>SIZE_MAX/sizeof *forwarded_receivers.items){
  throw_oom(v,"IntentFilter receiver table");return false;
 }
 forwarded_receiver *items=realloc(forwarded_receivers.items,cap*sizeof *items);
 if(!items){throw_oom(v,"IntentFilter receiver table");return false;}
 forwarded_receivers.items=items;forwarded_receivers.capacity=cap;
 return true;
}
static void forwarded_add(struct dvm *v,dvm_ref ref){
 forwarded_sync(v);
 /* Diagnostic witness: the receiver table must not alias live class text. */
 if(forwarded_receivers.items&&dvm__name_range_hit(v,forwarded_receivers.items,sizeof(forwarded_receiver)))
  sdk_trace_fprintf(stderr,"FORWARDED_TABLE_ALIAS vm=%p serial=%llu table=%p ref=%u count=%zu capacity=%zu write_count=1\n",(void*)v,(unsigned long long)v->serial,(void*)forwarded_receivers.items,ref,forwarded_receivers.count,forwarded_receivers.capacity);
 for(size_t i=0;i<forwarded_receivers.count;i++)
  if(forwarded_receivers.items[i].ref==ref){forwarded_receivers.items[i].count++;return;}
 if(forwarded_receivers.count>=forwarded_receivers.capacity)return; /* reserved */
 forwarded_receivers.items[forwarded_receivers.count].ref=ref;
 forwarded_receivers.items[forwarded_receivers.count].count=1;
 forwarded_receivers.count++;
}
static void forwarded_remove(struct dvm *v,dvm_ref ref){
 forwarded_sync(v);
 for(size_t i=0;i<forwarded_receivers.count;i++){
  if(forwarded_receivers.items[i].ref!=ref)continue;
  if(--forwarded_receivers.items[i].count==0)
   forwarded_receivers.items[i]=forwarded_receivers.items[--forwarded_receivers.count];
  return;
 }
}

/* Returns NULL only when the object belongs to another IntentFilter owner
 * (currently battery_runtime).  An uninitialised object throws, matching the
 * intent_runtime module's contract. */
static intent_filter_payload *filter_get(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","IntentFilter receiver");return NULL;}
 if(!o->data){dvm__throw(v,"java/lang/IllegalStateException","IntentFilter constructor not called");return NULL;}
 if(o->data_destroy!=intent_filter_payload_free)return NULL;
 intent_filter_payload *p=o->data;
 return p&&p->magic==INTENT_FILTER_MAGIC?p:NULL;
}
static bool filter_is_ours(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o||!o->data||o->data_destroy!=intent_filter_payload_free)return false;
 intent_filter_payload *p=o->data;
 return p->magic==INTENT_FILTER_MAGIC;
}
static intent_filter_payload *filter_alloc(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","IntentFilter receiver");return NULL;}
 if(o->data){dvm__throw(v,"java/lang/IllegalStateException","IntentFilter already initialised");return NULL;}
 intent_filter_payload *p=calloc(1,sizeof *p);
 if(!p){throw_oom(v,"IntentFilter payload");return NULL;}
 p->magic=INTENT_FILTER_MAGIC;
 p->priority=0;
 o->data=p;
 o->data_destroy=intent_filter_payload_free;
 return p;
}

static const char *string_arg(struct dvm *v,dvm_ref ref,const char *what){
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","%s",what);return NULL;}
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","%s",what);return NULL;}
 if(o->kind!=DVM_OBJ_STRING||!o->utf8){
  dvm__throw(v,"java/lang/IllegalArgumentException","%s is not a String",what);
  return NULL;
 }
 return o->utf8;
}

static char *copy_arg(struct dvm *v,const char *s,const char *what){
 char *copy=strdup(s);
 if(!copy)throw_oom(v,what);
 return copy;
}

static bool string_list_contains(const string_list *l,const char *s){
 for(size_t i=0;i<l->count;i++)if(!strcmp(l->items[i],s))return true;
 return false;
}
static bool string_list_add(struct dvm *v,string_list *l,const char *s,bool unique){
 if(unique&&string_list_contains(l,s))return true;
 if(l->count==l->capacity){
  size_t cap=l->capacity?l->capacity*2:4;
  if(cap<l->capacity||cap>SIZE_MAX/sizeof *l->items){throw_oom(v,"IntentFilter list");return false;}
  char **items=realloc(l->items,cap*sizeof *items);
  if(!items){throw_oom(v,"IntentFilter list");return false;}
  l->items=items;l->capacity=cap;
 }
 char *copy=copy_arg(v,s,"IntentFilter string");
 if(!copy)return false;
 l->items[l->count++]=copy;
 return true;
}
/* AOSP addDataType for a wildcard type (the type prefix followed by slash and
 * star) drops every existing type starting with that prefix, then stores the
 * wildcard.  Keep that observable list behavior. */
static void string_list_remove_prefix(string_list *l,const char *prefix,size_t prefix_len){
 size_t w=0;
 for(size_t i=0;i<l->count;i++){
  if(strncmp(l->items[i],prefix,prefix_len)==0){free(l->items[i]);continue;}
  l->items[w++]=l->items[i];
 }
 l->count=w;
}
static bool authority_list_add(struct dvm *v,authority_list *l,const char *host,const char *port){
 /* AOSP addDataAuthority appends unconditionally (no de-duplication). */
 if(l->count==l->capacity){
  size_t cap=l->capacity?l->capacity*2:4;
  if(cap<l->capacity||cap>SIZE_MAX/sizeof *l->items){throw_oom(v,"IntentFilter authorities");return false;}
  authority_entry *items=realloc(l->items,cap*sizeof *items);
  if(!items){throw_oom(v,"IntentFilter authorities");return false;}
  l->items=items;l->capacity=cap;
 }
 char *h=copy_arg(v,host,"IntentFilter authority host");
 if(!h)return false;
 char *p=NULL;
 if(port){p=copy_arg(v,port,"IntentFilter authority port");if(!p){free(h);return false;}}
 l->items[l->count].host=h;
 l->items[l->count].port=p;
 l->count++;
 return true;
}

/* AOSP addDataType accepts anything with a slash after the first character
 * and at least one character after it; it does not normalize or reject blanks
 * at add time.  Match that exactly so we do not reject a type Android accepts. */
static bool valid_mime_type(const char *type){
 const char *slash=strchr(type,'/');
 if(!slash||slash==type)return false;
 size_t len=strlen(type);
 size_t pos=(size_t)(slash-type);
 return len>=pos+2;
}
static void ensure_mime_exception_hierarchy(struct dvm *v){
 struct dvm_class *c=dvm_find_class_or_external(v,"android/content/IntentFilter$MalformedMimeTypeException");
 struct dvm_class *base=dvm_find_class_or_external(v,"java/lang/Exception");
 if(!c||!base||c==base)return;
 if(c->external&&(!c->super||(c->super->name&&!strcmp(c->super->name,"java/lang/Object"))))
  c->super=base;
}
static void throw_malformed_mime(struct dvm *v,const char *type){
 ensure_mime_exception_hierarchy(v);
 dvm__throw(v,"android/content/IntentFilter$MalformedMimeTypeException","%s",type);
}

static dvm_ref string_result(struct dvm *v,const char *s){
 dvm_ref r=dvm_new_string(v,s);
 if(!r)throw_oom(v,"IntentFilter result string");
 return r;
}

static bool is_context_class(const char *c){
 return !strcmp(c,"android/content/Context")||
        !strcmp(c,"android/content/ContextWrapper")||
        !strcmp(c,"android/app/Application");
}
static bool is_register_two(const char *m,const char *sig){
 return !strcmp(m,"registerReceiver")&&
        !strcmp(sig,"(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;");
}
static bool is_register_four(const char *m,const char *sig){
 return !strcmp(m,"registerReceiver")&&
        !strcmp(sig,"(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;Ljava/lang/String;Landroid/os/Handler;)Landroid/content/Intent;");
}

bool intent_filter_runtime_call(struct dvm *vm,const char *c,const char *m,
 const char *sig,dvm_ref self,const union dvm_value *a,int n,
 union dvm_value *out){
 if(!vm||!c||!m||!sig||!out)return false;

 /* Registering our real filter goes through android_basics' real receiver
  * table.  A NULL receiver is forwarded too, so android_basics raises the
  * Android NullPointerException instead of battery_runtime complaining about
  * a non-battery filter.  Filters owned by battery_runtime fall through. */
 if(is_context_class(c)&&((is_register_two(m,sig)&&n==2)||(is_register_four(m,sig)&&n==4))){
  if(a&&a[1].l&&filter_is_ours(vm,a[1].l)){
   if(!forwarded_reserve(vm))return true; /* OutOfMemoryError raised */
   bool handled=android_basics_call(vm,c,m,sig,self,a,n,out);
   /* Only a successful registration enters the receiver table, so only then
    * is the receiver recorded for later unregisterReceiver routing. */
   if(handled&&!dvm_exception(vm)&&a[0].l)forwarded_add(vm,a[0].l);
   return handled;
  }
  return false;
 }
 /* android_basics' receiver table holds the receiver registered above.
  * battery_runtime owns every unregisterReceiver it sees, so this module must
  * claim exactly the receivers it forwarded and leave all others (including
  * battery's) to the rest of the chain. */
 if(is_context_class(c)&&!strcmp(m,"unregisterReceiver")&&
  !strcmp(sig,"(Landroid/content/BroadcastReceiver;)V")&&n==1){
  if(a&&a[0].l&&forwarded_count(vm,a[0].l)){
   bool handled=android_basics_call(vm,c,m,sig,self,a,n,out);
   if(handled&&!dvm_exception(vm))forwarded_remove(vm,a[0].l);
   return handled;
  }
  return false;
 }

 if(strcmp(c,"android/content/IntentFilter"))return false;

 if(!strcmp(m,"<init>")){
  if(!strcmp(sig,"()V")&&n==0){
   /* filter_alloc raises NPE/ISE/OOM itself; the call is handled either way. */
   filter_alloc(vm,self);
   return true;
  }
  if(!strcmp(sig,"(Ljava/lang/String;)V")&&n==1){
   if(!a)return false;
   if(!a[0].l){dvm__throw(vm,"java/lang/NullPointerException","IntentFilter action");return true;}
   const char *action=string_arg(vm,a[0].l,"IntentFilter action");
   if(!action)return true; /* NPE or non-String IllegalArgumentException */
   /* battery_runtime owns the battery filter; it must see the constructor so
    * it can create its private payload. */
   if(!strcmp(action,BATTERY_ACTION))return false;
   intent_filter_payload *p=filter_alloc(vm,self);
   if(!p)return true;
   /* string_list_add has already raised OutOfMemoryError on failure. */
   string_list_add(vm,&p->actions,action,true);
   return true;
  }
  /* <init>(Landroid/content/IntentFilter;)V has no DEX call site.  Falling
   * through keeps it on the MISSING_API path instead of faking a copy. */
  return false;
 }

 intent_filter_payload *p=filter_get(vm,self);
 if(!p)return dvm_exception(vm)!=0; /* exception already raised, or not ours */

 if(!strcmp(m,"setPriority")&&!strcmp(sig,"(I)V")&&n==1){
  if(!a)return false;
  p->priority=a[0].i;
  return true;
 }
 if(!strcmp(m,"getPriority")&&!strcmp(sig,"()I")&&n==0){
  out->i=p->priority;
  return true;
 }
 if(!strcmp(m,"addAction")&&!strcmp(sig,"(Ljava/lang/String;)V")&&n==1){
  if(!a)return false;
  const char *action=string_arg(vm,a[0].l,"IntentFilter action");
  if(!action)return true;
  string_list_add(vm,&p->actions,action,true);
  return true;
 }
 if(!strcmp(m,"addCategory")&&!strcmp(sig,"(Ljava/lang/String;)V")&&n==1){
  if(!a)return false;
  const char *category=string_arg(vm,a[0].l,"IntentFilter category");
  if(!category)return true;
  string_list_add(vm,&p->categories,category,true);
  return true;
 }
 if(!strcmp(m,"addDataScheme")&&!strcmp(sig,"(Ljava/lang/String;)V")&&n==1){
  if(!a)return false;
  const char *scheme=string_arg(vm,a[0].l,"IntentFilter data scheme");
  if(!scheme)return true;
  string_list_add(vm,&p->schemes,scheme,true);
  return true;
 }
 if(!strcmp(m,"addDataAuthority")&&!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)V")&&n==2){
  if(!a)return false;
  if(!a[0].l){dvm__throw(vm,"java/lang/NullPointerException","IntentFilter authority host");return true;}
  const char *host=string_arg(vm,a[0].l,"IntentFilter authority host");
  if(!host)return true;
  const char *port=NULL;
  if(a[1].l){
   port=string_arg(vm,a[1].l,"IntentFilter authority port");
   if(!port)return true;
  }
  authority_list_add(vm,&p->authorities,host,port);
  return true;
 }
 if(!strcmp(m,"addDataType")&&!strcmp(sig,"(Ljava/lang/String;)V")&&n==1){
  if(!a)return false;
  if(!a[0].l){dvm__throw(vm,"java/lang/NullPointerException","IntentFilter data type");return true;}
  const char *type=string_arg(vm,a[0].l,"IntentFilter data type");
  if(!type)return true;
  if(!valid_mime_type(type)){throw_malformed_mime(vm,type);return true;}
  const char *slash=strchr(type,'/');
  size_t slashpos=(size_t)(slash-type);
  if(strlen(type)==slashpos+2&&type[slashpos+1]=='*')
   string_list_remove_prefix(&p->types,type,slashpos);
  if(string_list_contains(&p->types,type))return true;
  string_list_add(vm,&p->types,type,false);
  return true;
 }
 if(!strcmp(m,"countActions")&&!strcmp(sig,"()I")&&n==0){
  out->i=(int32_t)p->actions.count;
  return true;
 }
 if(!strcmp(m,"getAction")&&!strcmp(sig,"(I)Ljava/lang/String;")&&n==1){
  if(!a)return false;
  int32_t index=a[0].i;
  if(index<0||(size_t)index>=p->actions.count){
   dvm__throw(vm,"java/lang/IndexOutOfBoundsException","IntentFilter action index %d",(int)index);
   return true;
  }
  out->l=string_result(vm,p->actions.items[index]);
  return true;
 }
 if(!strcmp(m,"getAction")&&!strcmp(sig,"()Ljava/lang/String;")&&n==0){
  /* AOSP has no zero-argument getAction(); the coordinator requested this
   * compatibility accessor.  Return the first action, or null when empty. */
  if(p->actions.count==0){out->l=0;return true;}
  out->l=string_result(vm,p->actions.items[0]);
  return true;
 }
 if(!strcmp(m,"hasAction")&&!strcmp(sig,"(Ljava/lang/String;)Z")&&n==1){
  if(!a)return false;
  if(!a[0].l){out->i=0;return true;} /* AOSP: hasAction(null) is false */
  const char *action=string_arg(vm,a[0].l,"IntentFilter action");
  if(!action)return true;
  out->i=string_list_contains(&p->actions,action)?1:0;
  return true;
 }
 if(!strcmp(m,"hasCategory")&&!strcmp(sig,"(Ljava/lang/String;)Z")&&n==1){
  if(!a)return false;
  if(!a[0].l){out->i=0;return true;}
  const char *category=string_arg(vm,a[0].l,"IntentFilter category");
  if(!category)return true;
  out->i=string_list_contains(&p->categories,category)?1:0;
  return true;
 }
 /* match(...) is deliberately absent: the packaged DEX has no call site and
 * Android's matching rules involve authorities, schemes, types, wildcards and
 * Intent categories.  Reporting a guessed match result would be fabrication;
 * returning false lets strict_runtime report MISSING_API. */
 return false;
}

/* --- selftest ------------------------------------------------------------ */

static bool self_string_is(struct dvm *v,dvm_ref ref,const char *expect){
 if(!ref)return false;
 const char *s=dvm_string_utf8(v,ref);
 return s&&!strcmp(s,expect);
}
static bool self_exception_is(struct dvm *v,const char *name){
 dvm_ref e=dvm_exception(v);
 struct dvm_object *o=dvm__obj(v,e);
 return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,name);
}

bool intent_filter_runtime_selftest(struct dvm *v){
 bool ok=true;
 unsigned checks=0;
 struct dvm_class *filter_class=dvm_find_class_or_external(v,"android/content/IntentFilter");
 struct dvm_class *context_class=dvm_find_class_or_external(v,"android/content/Context");
 struct dvm_class *receiver_class=dvm_find_class_or_external(v,"android/content/BroadcastReceiver");
 union dvm_value arg={0},out={0},ignored={0};
 dvm_ref filter=0,with_action=0,context=0,receiver=0,uninit=0;
 intent_filter_payload *p=NULL;
#define IF_CHECK(expr,stage) do{ if(!(expr)){ \
 sdk_trace_fprintf(stderr,"INTENT_FILTER_RUNTIME_SELFTEST FAIL stage=%s checks=%u\n",(stage),checks); \
 ok=false;goto done; } checks++; }while(0)

 IF_CHECK(filter_class&&context_class&&receiver_class,"classes");
 /* 负例：未调用 <init> 的 IntentFilter 不能静默接受 addAction；本模块必须
  * 抛 IllegalStateException（不是崩溃，也不是吞掉错误）。正例见下方
  * init_empty + add_action。 */
 uninit=dvm_new_object(v,filter_class);
 IF_CHECK(uninit!=0,"uninit_new");
 arg.l=dvm_new_string(v,"android.net.conn.CONNECTIVITY_CHANGE");
 IF_CHECK(arg.l!=0,"uninit_action_string");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addAction","(Ljava/lang/String;)V",uninit,&arg,1,&ignored),"uninit_add_action_call");
 IF_CHECK(self_exception_is(v,"java/lang/IllegalStateException"),"uninit_add_action_exception");
 dvm_clear_exception(v);
 filter=dvm_new_object(v,filter_class);
 IF_CHECK(filter!=0,"new_filter");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","<init>","()V",filter,NULL,0,&ignored),"init_empty");
 IF_CHECK(!dvm_exception(v),"init_empty_clean");
 p=filter_get(v,filter);
 IF_CHECK(p!=NULL,"init_payload");
 IF_CHECK(p->priority==0&&p->actions.count==0&&p->categories.count==0&&
  p->schemes.count==0&&p->authorities.count==0&&p->types.count==0,"init_state");

 arg.i=990;
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","setPriority","(I)V",filter,&arg,1,&ignored),"set_priority");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","getPriority","()I",filter,NULL,0,&out)&&out.i==990,"get_priority");

 arg.l=dvm_new_string(v,"android.net.conn.CONNECTIVITY_CHANGE");
 IF_CHECK(arg.l!=0,"action_string");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addAction","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_action");
 IF_CHECK(!dvm_exception(v),"add_action_clean");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addAction","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_action_duplicate");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","countActions","()I",filter,NULL,0,&out)&&out.i==1,"count_one");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","getAction","(I)Ljava/lang/String;",filter,&(union dvm_value){.i=0},1,&out)&&
  self_string_is(v,out.l,"android.net.conn.CONNECTIVITY_CHANGE"),"get_action_index");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","getAction","()Ljava/lang/String;",filter,NULL,0,&out)&&
  self_string_is(v,out.l,"android.net.conn.CONNECTIVITY_CHANGE"),"get_action_first");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","hasAction","(Ljava/lang/String;)Z",filter,&arg,1,&out)&&out.i==1,"has_action");
 arg.l=dvm_new_string(v,"android.intent.action.OTHER");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","hasAction","(Ljava/lang/String;)Z",filter,&arg,1,&out)&&out.i==0,"has_action_absent");
 arg.l=0;
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","hasAction","(Ljava/lang/String;)Z",filter,&arg,1,&out)&&out.i==0&&!dvm_exception(v),"has_action_null");

 /* Out-of-range getAction(int) must throw, not return null. */
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","getAction","(I)Ljava/lang/String;",filter,&(union dvm_value){.i=1},1,&out),"get_action_oob_call");
 IF_CHECK(self_exception_is(v,"java/lang/IndexOutOfBoundsException"),"get_action_oob_exception");
 dvm_clear_exception(v);

 /* addAction(null) must raise NPE. */
 arg.l=0;
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addAction","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_action_null_call");
 IF_CHECK(self_exception_is(v,"java/lang/NullPointerException"),"add_action_null_exception");
 dvm_clear_exception(v);

 arg.l=dvm_new_string(v,"android.intent.category.DEFAULT");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addCategory","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_category");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addCategory","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_category_duplicate");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","hasCategory","(Ljava/lang/String;)Z",filter,&arg,1,&out)&&out.i==1,"has_category");
 arg.l=dvm_new_string(v,"android.intent.category.OTHER");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","hasCategory","(Ljava/lang/String;)Z",filter,&arg,1,&out)&&out.i==0,"has_category_absent");

 arg.l=dvm_new_string(v,"https");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataScheme","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_scheme");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataScheme","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_scheme_duplicate");
 p=filter_get(v,filter);
 IF_CHECK(p&&p->schemes.count==1,"scheme_state");
 arg.l=0;
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataScheme","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_scheme_null_call");
 IF_CHECK(self_exception_is(v,"java/lang/NullPointerException"),"add_scheme_null_exception");
 dvm_clear_exception(v);

 union dvm_value authority[2];
 authority[0].l=dvm_new_string(v,"example.com");
 authority[1].l=0;
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataAuthority","(Ljava/lang/String;Ljava/lang/String;)V",filter,authority,2,&ignored),"add_authority");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataAuthority","(Ljava/lang/String;Ljava/lang/String;)V",filter,authority,2,&ignored),"add_authority_repeat");
 p=filter_get(v,filter);
 IF_CHECK(p&&p->authorities.count==2&&p->authorities.items[0].port==NULL,"authority_state");
 authority[0].l=0;
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataAuthority","(Ljava/lang/String;Ljava/lang/String;)V",filter,authority,2,&ignored),"add_authority_null_call");
 IF_CHECK(self_exception_is(v,"java/lang/NullPointerException"),"add_authority_null_exception");
 dvm_clear_exception(v);

 arg.l=dvm_new_string(v,"text/plain");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataType","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_type");
 arg.l=dvm_new_string(v,"text/html");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataType","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_type_second");
 arg.l=dvm_new_string(v,"text/*");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataType","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_type_wildcard");
 p=filter_get(v,filter);
 IF_CHECK(p&&p->types.count==1&&!strcmp(p->types.items[0],"text/*"),"type_wildcard_state");
 arg.l=dvm_new_string(v,"not-a-mime");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataType","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_type_bad_call");
 IF_CHECK(self_exception_is(v,"android/content/IntentFilter$MalformedMimeTypeException"),"add_type_bad_exception");
 dvm_clear_exception(v);
 arg.l=0;
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","addDataType","(Ljava/lang/String;)V",filter,&arg,1,&ignored),"add_type_null_call");
 IF_CHECK(self_exception_is(v,"java/lang/NullPointerException"),"add_type_null_exception");
 dvm_clear_exception(v);

 /* The String constructor stores the action in the real payload. */
 with_action=dvm_new_object(v,filter_class);
 arg.l=dvm_new_string(v,"com.example.SELFTEST_ACTION");
 IF_CHECK(with_action!=0&&arg.l!=0,"ctor_string_setup");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","<init>","(Ljava/lang/String;)V",with_action,&arg,1,&ignored),"ctor_string");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","countActions","()I",with_action,NULL,0,&out)&&out.i==1,"ctor_string_count");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","getAction","(I)Ljava/lang/String;",with_action,&(union dvm_value){.i=0},1,&out)&&
  self_string_is(v,out.l,"com.example.SELFTEST_ACTION"),"ctor_string_action");

 /* The two-argument registerReceiver path must use the real receiver table
  * and keep the same filter object; unregister must remove that receiver. */
 context=dvm_new_object(v,context_class);
 receiver=dvm_new_object(v,receiver_class);
 IF_CHECK(context!=0&&receiver!=0,"receiver_setup");
 intent_filter_payload *before=filter_get(v,filter);
 union dvm_value reg[2]={{.l=receiver},{.l=filter}};
 /* A NULL receiver must reach android_basics and raise the Android NPE. */
 union dvm_value bad_reg[2]={{.l=0},{.l=filter}};
 IF_CHECK(intent_filter_runtime_call(v,"android/content/Context","registerReceiver",
  "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",
  context,bad_reg,2,&out),"register_null_receiver_call");
 IF_CHECK(self_exception_is(v,"java/lang/NullPointerException"),"register_null_receiver_exception");
 dvm_clear_exception(v);
 IF_CHECK(intent_filter_runtime_call(v,"android/content/Context","registerReceiver",
  "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",
  context,reg,2,&out),"register_call");
 IF_CHECK(!dvm_exception(v),"register_clean");
 IF_CHECK(out.l==0,"register_return");
 IF_CHECK(filter_is_ours(v,filter)&&filter_get(v,filter)==before,"register_filter_identity");
 IF_CHECK(intent_filter_runtime_call(v,"android/content/IntentFilter","countActions","()I",filter,NULL,0,&out)&&out.i==1,"register_filter_state");
 /* The receiver was recorded when registration was forwarded, so the first
  * unregisterReceiver must be claimed and reach android_basics' table. */
 IF_CHECK(intent_filter_runtime_call(v,"android/content/Context","unregisterReceiver",
  "(Landroid/content/BroadcastReceiver;)V",context,&(union dvm_value){.l=receiver},1,&ignored),"unregister_call");
 IF_CHECK(!dvm_exception(v),"unregister_clean");
 /* It is no longer ours: the module refuses without an exception, exactly as
  * it must for battery_runtime's receivers, and the chain's android_basics
  * handler reports the duplicate unregister. */
 IF_CHECK(!intent_filter_runtime_call(v,"android/content/Context","unregisterReceiver",
  "(Landroid/content/BroadcastReceiver;)V",context,&(union dvm_value){.l=receiver},1,&ignored)&&
  !dvm_exception(v),"unregister_second_not_owned");
 IF_CHECK(android_basics_call(v,"android/content/Context","unregisterReceiver",
  "(Landroid/content/BroadcastReceiver;)V",context,&(union dvm_value){.l=receiver},1,&ignored),"unregister_again_call");
 IF_CHECK(self_exception_is(v,"java/lang/IllegalArgumentException"),"unregister_again_exception");
 dvm_clear_exception(v);

 /* Exercise the same release/rebind boundary used by dvm_destroy. Receiver
  * unregistration leaves capacity allocated: that exact empty-table case
  * previously survived arena teardown and wrote into reused class buffers. */
 IF_CHECK(forwarded_receivers.items&&forwarded_receivers.count==0,"lifecycle_empty_allocated_table");
 intent_filter_runtime_vm_end(v);
 IF_CHECK(!forwarded_vm&&!forwarded_serial&&!forwarded_receivers.items&&
  !forwarded_receivers.count&&!forwarded_receivers.capacity,"lifecycle_release");
 intent_filter_runtime_vm_end(v);
 IF_CHECK(!forwarded_receivers.items,"lifecycle_release_idempotent");
 IF_CHECK(forwarded_reserve(v),"lifecycle_rebind_reserve");
 forwarded_add(v,receiver);
 IF_CHECK(forwarded_count(v,receiver)==1&&forwarded_serial==v->serial,"lifecycle_rebind_count");
 forwarded_remove(v,receiver);
 IF_CHECK(forwarded_count(v,receiver)==0,"lifecycle_rebind_remove");

done:
 if(ok){
  p=filter_get(v,filter);
  sdk_trace_fprintf(stderr,"INTENT_FILTER_RUNTIME_SELFTEST PASS checks=%u actions=%zu categories=%zu schemes=%zu authorities=%zu types=%zu receiver_vm_lifecycle=1\n",
   checks,p?p->actions.count:0,p?p->categories.count:0,p?p->schemes.count:0,
   p?p->authorities.count:0,p?p->types.count:0);
 }else if(dvm_exception(v)){
  dvm_clear_exception(v);
 }
#undef IF_CHECK
 return ok;
}

int filter_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 (void)v;(void)visit;(void)u;return o->data_destroy==intent_filter_payload_free?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
