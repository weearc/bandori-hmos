/* Fixture only: tests the C/VM host contract, not Harmony batteryInfo. */
#include "battery_runtime.h"
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
#define fprintf sdk_trace_fprintf
#include <stdio.h>
#include <stdlib.h>

static int received,sticky_seen,change_seen;
static bool receive_fixture(struct dvm *v,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out){(void)self;(void)out;
 if(nargs!=2)return false;union dvm_value key[2]={{.l=dvm_new_string(v,"level")},{.i=-1}},value={0};
 if(!battery_runtime_call(v,"android/content/Intent","getIntExtra","(Ljava/lang/String;I)I",args[1].l,key,2,&value))return false;
 ++received;if(value.i==42)++sticky_seen;if(value.i==43)++change_seen;return true;}

struct fixture { int subscribe_result, snapshot_result, subscriptions, cancellations; battery_host_emit emit; void *user; };
static int subscribe_fixture(void *ctx,battery_host_emit emit,void *user){struct fixture *f=ctx;++f->subscriptions;f->emit=emit;f->user=user;return f->subscribe_result;}
static void unsubscribe_fixture(void *ctx,void *user){struct fixture *f=ctx;(void)user;++f->cancellations;f->emit=NULL;f->user=NULL;}
static int snapshot_fixture(void *ctx,battery_host_event *out){struct fixture *f=ctx;if(f->snapshot_result)return f->snapshot_result;*out=(battery_host_event){.present_mask=BATTERY_HAS_LEVEL|BATTERY_HAS_SCALE,.generation=1,.level=42,.scale=100};return 0;}
bool battery_runtime_selftest(void){
 battery_host_callbacks saved={0};void *saved_ctx=NULL;battery_runtime_get_host(&saved,&saved_ctx);
 struct dvm *v=dvm_create(NULL);if(!v)return false;
 struct fixture f={0};const battery_host_callbacks cb={subscribe_fixture,unsubscribe_fixture,snapshot_fixture};battery_runtime_set_host(&cb,&f);
 struct dvm_class *fc=dvm_find_class_or_external(v,"android/content/IntentFilter"),*base=dvm_find_class_or_external(v,"android/content/BroadcastReceiver");
 struct dvm_class *rc=dvm__register_builtin(v,"Lbattery/FixtureReceiver;");rc->super=base;rc->methods=calloc(1,sizeof *rc->methods);rc->nmethods=1;
 rc->methods[0]=(struct dvm_method){.cls=rc,.name="onReceive",.sig=strdup("(Landroid/content/Context;Landroid/content/Intent;)V"),.builtin=receive_fixture,.arg_count=2,.arg_slots=2,.ret_kind='V'};
 dvm_ref filter=dvm_new_object(v,fc),receiver=dvm_new_object(v,rc),context=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/Context"));
 union dvm_value out={0},fa={.l=dvm_new_string(v,"android.intent.action.BATTERY_CHANGED")};
 bool ok=filter&&receiver&&context&&battery_runtime_call(v,"android/content/IntentFilter","<init>","(Ljava/lang/String;)V",filter,&fa,1,&out);
 ok=ok&&battery_runtime_call(v,"android/content/BroadcastReceiver","<init>","()V",receiver,NULL,0,&out);
 union dvm_value reg[2]={{.l=receiver},{.l=filter}};
 ok=ok&&battery_runtime_context_broadcast(v,"registerReceiver","(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",context,reg,2,&out);
 dvm_ref intent=out.l;ok=ok&&intent&&!dvm_exception(v)&&f.subscriptions==1;
 received=sticky_seen=change_seen=0;battery_runtime_pump(v);ok=ok&&received==1&&sticky_seen==1&&!dvm_exception(v);
 battery_host_event event={.present_mask=BATTERY_HAS_LEVEL|BATTERY_HAS_SCALE,.generation=2,.level=43,.scale=100};f.emit(f.user,&event);battery_runtime_pump(v);ok=ok&&received==2&&change_seen==1&&!dvm_exception(v);
 event.generation=1;f.emit(f.user,&event);battery_runtime_pump(v);ok=ok&&received==2;
 for(uint64_t generation=3;generation<=22;++generation){event.generation=generation;event.level=50;f.emit(f.user,&event);}
 battery_runtime_pump(v);ok=ok&&received==18&&!dvm_exception(v);
 union dvm_value extra[2]={{.l=dvm_new_string(v,"level")},{.i=-99}},value={0};
 ok=ok&&battery_runtime_call(v,"android/content/Intent","getIntExtra","(Ljava/lang/String;I)I",intent,extra,2,&value)&&value.i==42;
 extra[0].l=dvm_new_string(v,"voltage");ok=ok&&battery_runtime_call(v,"android/content/Intent","getIntExtra","(Ljava/lang/String;I)I",intent,extra,2,&value)&&value.i==-99;
 union dvm_value unreg={.l=receiver};ok=ok&&battery_runtime_context_broadcast(v,"unregisterReceiver","(Landroid/content/BroadcastReceiver;)V",context,&unreg,1,&out)&&f.cancellations==1;
 f.subscribe_result=7;ok=ok&&battery_runtime_context_broadcast(v,"registerReceiver","(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",context,reg,2,&out)&&dvm_exception(v)!=0;
 dvm_clear_exception(v);f.subscribe_result=0;f.snapshot_result=9;
 ok=ok&&battery_runtime_context_broadcast(v,"registerReceiver","(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",context,reg,2,&out)&&dvm_exception(v)!=0;
 dvm_clear_exception(v);battery_runtime_finish(v);dvm_destroy(v);battery_runtime_set_host(&saved,saved_ctx);
 if(ok)fprintf(stderr,"BATTERY_RUNTIME_SELFTEST PASS fixture_only\n");return ok;
}
