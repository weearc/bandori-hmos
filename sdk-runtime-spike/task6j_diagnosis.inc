#include "gc_operation.h"
/* TASK6j diagnostics only. No guest calls, allocations, buffer reads or writes.
 * All JSON string values are masked except decimal "code". Unknown text is
 * opaque. We observe the original return object, never call string()/bytes(). */
#include <ctype.h>
#include <stdarg.h>
static void diag6j_log(struct dvm *v,const char *fmt,...){
 int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task6j-diagnosis.txt","a");
 if(f){dprintf(fileno(f),"TASK6J pid=%d vm=%llu time=%lld mono_ms=%llu thread=%u ",getpid(),(unsigned long long)v->serial,(long long)time(NULL),(unsigned long long)monotonic_ms(),v->cur_thread);va_list a;va_start(a,fmt);vdprintf(fileno(f),fmt,a);va_end(a);dprintf(fileno(f),"\n");fclose(f);}errno=saved;
}
static struct dvm_object *diag6j_obj(struct dvm *v,dvm_ref r){if(r&&!gc_operation_admit(v,r))return NULL;return r?dvm__obj(v,r):NULL;}
static dvm_ref diag6j_ref(struct dvm *v,dvm_ref r,const char *name){
 struct dvm_object *o=diag6j_obj(v,r);if(!o||!o->slots)return 0;
 for(struct dvm_class *c=o->cls;c;c=c->super)for(int i=0;i<c->nifields;i++)if(!strcmp(c->ifields[i].name,name)&&c->ifields[i].slot<o->cls->islots)return o->slots[c->ifields[i].slot].l;return 0;
}
/* Diagnostic observers own no guest objects. Ref IDs can be recycled: pair
 * every cached ID with its allocation identity and discard stale candidates. */
static bool diag6j_identity(struct dvm *v,dvm_ref ref,uint64_t generation){
 if(!generation)return false;struct dvm_object *o=diag6j_obj(v,ref);return o&&o->identity_generation==generation;
}
static struct {uint64_t vm,hash,ref_generation,body_generation;dvm_ref ref,body;size_t bytes;} diag6j_strings[128];
static unsigned diag6j_string_next;
static uint64_t diag6j_hash(const char *s,size_t n){uint64_t h=1469598103934665603ULL;for(size_t i=0;i<n;i++){h^=(unsigned char)s[i];h*=1099511628211ULL;}return h;}
static void diag6j_body_string(struct dvm *v,dvm_ref body,dvm_ref ref){
 struct dvm_object *o=diag6j_obj(v,ref),*b=diag6j_obj(v,body);
 if(!o||o->kind!=DVM_OBJ_STRING||!o->utf8||!b)return;
 unsigned i=diag6j_string_next++%128;memset(&diag6j_strings[i],0,sizeof diag6j_strings[i]);
 diag6j_strings[i].vm=v->serial;diag6j_strings[i].ref=ref;diag6j_strings[i].ref_generation=o->identity_generation;
 diag6j_strings[i].body=body;diag6j_strings[i].body_generation=b->identity_generation;
 diag6j_strings[i].bytes=o->utf8_len;diag6j_strings[i].hash=diag6j_hash(o->utf8,o->utf8_len);
}
static void diag6j_compare(struct dvm *v,dvm_ref ref,const char *target){
 struct dvm_object *o=diag6j_obj(v,ref);const char *s=o&&o->kind==DVM_OBJ_STRING?o->utf8:NULL;size_t n=s?o->utf8_len:0,a=0,z=n;while(a<n&&isspace((unsigned char)s[a]))a++;while(z>a&&isspace((unsigned char)s[z-1]))z--;
 uint64_t h=s?diag6j_hash(s,n):0;int match=-1;
 for(int i=0;i<128;i++){
  if(diag6j_strings[i].vm&&diag6j_strings[i].vm!=v->serial){memset(&diag6j_strings[i],0,sizeof diag6j_strings[i]);continue;}
  /* Check identity only for content candidates: probing every old observer
   * through dvm__obj would unnecessarily borrow all128 objects in this scope. */
  if(!s||diag6j_strings[i].vm!=v->serial||diag6j_strings[i].bytes!=n||diag6j_strings[i].hash!=h)continue;
  if(!diag6j_identity(v,diag6j_strings[i].ref,diag6j_strings[i].ref_generation)||
     !diag6j_identity(v,diag6j_strings[i].body,diag6j_strings[i].body_generation)){
   memset(&diag6j_strings[i],0,sizeof diag6j_strings[i]);continue;
  }
  match=i;
 }
 diag6j_log(v,"kind=fromJson_input_shape target=%s ref=%u bytes=%zu starts_object=%d quoted_string=%d body_content_match=%d matched_body=%u matched_body_bytes=%zu note=no_match_can_be_local_or_decrypted_not_proof_of_truncation",target,ref,n,a<n&&s[a]=='{',z>a&&s[a]=='"'&&s[z-1]=='"',match>=0,match>=0?diag6j_strings[match].body:0,match>=0?diag6j_strings[match].bytes:0);
}
static void diag6j_string(struct dvm *v,dvm_ref ref,const char *context){
 struct dvm_object *o=diag6j_obj(v,ref);const char *s=o&&o->kind==DVM_OBJ_STRING?o->utf8:NULL;size_t n=s?o->utf8_len:0;
 char safe[16384];size_t at=0,i=0;char key[96]="";int fields=0;
 /* Bound diagnostic work; never change/truncate the actual returned String. */
 if(n>1048576){diag6j_log(v,"kind=string context=%s ref=%u bytes=%zu content=opaque_over_1MiB",context,ref,n);return;}
 if(!s||!n){diag6j_log(v,"kind=string context=%s ref=%u bytes=%zu null=%d already_read_or_closed=1 evidence=empty_is_not_proof",context,ref,n,!s);return;}
 size_t start=0;while(start<n&&isspace((unsigned char)s[start]))start++;
 if(start==n||(s[start]!='{'&&s[start]!='[')){diag6j_log(v,"kind=string context=%s ref=%u bytes=%zu first80=<opaque-redacted> last80=<opaque-redacted> full_redacted=<opaque-redacted>",context,ref,n);if(n<96&&!strncmp(s,"java.io.StringWriter@",21)){bool identity=true;for(size_t j=21;j<n;j++)if(!isxdigit((unsigned char)s[j]))identity=false;if(identity)diag6j_log(v,"kind=identity_string context=%s full=%s",context,s);}return;}
 while(i<n&&at+150<sizeof safe){
  unsigned char ch=s[i];
  if(ch=='"'){
   size_t a=++i;while(i<n){if(s[i]=='\\'&&i+1<n){i+=2;continue;}if(s[i]=='"')break;i++;}size_t z=i-a;if(i<n)i++;
   size_t next=i;while(next<n&&isspace((unsigned char)s[next]))next++;
   if(next<n&&s[next]==':'){
    size_t k=z<sizeof key-1?z:sizeof key-1;memcpy(key,s+a,k);key[k]=0;
    safe[at++]='"';for(size_t j=0;j<k;j++)safe[at++]=(key[j]>=32&&key[j]<127)?key[j]:'?';safe[at++]='"';
    if(fields++<64){size_t value=next+1;while(value<n&&isspace((unsigned char)s[value]))value++;const char *type=value==n?"missing":s[value]=='"'?"string":s[value]=='{'?"object":s[value]=='['?"array":s[value]=='n'?"null":s[value]=='t'||s[value]=='f'?"boolean":"number";diag6j_log(v,"kind=json_field context=%s key=%s type=%s",context,key,type);}
   }else{
    bool code=!strcmp(key,"code")&&z&&z<16;for(size_t j=0;j<z&&code;j++)if(!isdigit((unsigned char)s[a+j])&&s[a+j]!='-')code=false;
    if(code){safe[at++]='"';memcpy(safe+at,s+a,z);at+=z;safe[at++]='"';}
    else at+=(size_t)snprintf(safe+at,sizeof safe-at,"\"<redacted:%zu>\"",z);
   }
  }else if((isdigit(ch)||ch=='-')&&strcmp(key,"code")){size_t first=i;while(i<n&&(isdigit((unsigned char)s[i])||s[i]=='-'||s[i]=='+'||s[i]=='.'||s[i]=='e'||s[i]=='E'))i++;at+=(size_t)snprintf(safe+at,sizeof safe-at,"<number-redacted:%zu>",i-first);}
  else if(ch>=32&&ch<127){safe[at++]=ch;i++;}else{i++;}
 }
 safe[at]=0;char first[81],last80[81];size_t first_n=at<80?at:80;memcpy(first,safe,first_n);first[first_n]=0;size_t tail=at<80?0:at-80;snprintf(last80,sizeof last80,"%s",safe+tail);
 size_t units=0;for(size_t j=0;j<n;j++){unsigned char ch=s[j];if((ch&0xc0)!=0x80)units+=ch>=0xf0?2:1;}
 diag6j_log(v,"kind=string context=%s ref=%u bytes=%zu utf16_units=%zu diagnostic_complete=%d first80=%s last80=%s",context,ref,n,units,i==n,first,i==n?last80:"<not_logged:diagnostic_capacity>");
 if(i==n)diag6j_log(v,"kind=string_full context=%s ref=%u full_redacted=%s",context,ref,safe);
}
static bool diag6j_class(struct dvm *v,dvm_ref ref,const char *name){struct dvm_object *o=diag6j_obj(v,ref);for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)if(!strcmp(c->name,name))return true;return false;}
static void diag6j_model(struct dvm *v,dvm_ref ref,const char *context){
 struct dvm_object *o=diag6j_obj(v,ref);diag6j_log(v,"kind=model context=%s ref=%u class=%s",context,ref,o&&o->cls?o->cls->name:"null");if(!o||!o->slots)return;
 for(struct dvm_class *c=o->cls;c;c=c->super)for(int i=0;i<c->nifields&&i<32;i++){struct dvm_field *f=&c->ifields[i];if(f->slot>=o->cls->islots)continue;dvm_ref r=o->slots[f->slot].l;struct dvm_object *q=diag6j_obj(v,r);const char *t=q&&q->cls?q->cls->name:r?"invalid_ref":"null";diag6j_log(v,"kind=model_field context=%s name=%s expected=%s actual=%s bytes=%u",context,f->name,f->type,t,q&&q->kind==DVM_OBJ_STRING?q->utf8_len:0);if(!strcmp(f->name,"code")&&q&&q->utf8&&q->utf8_len<16){bool numeric=true;for(unsigned j=0;j<q->utf8_len;j++)if(!isdigit((unsigned char)q->utf8[j])&&q->utf8[j]!='-')numeric=false;if(numeric)diag6j_log(v,"kind=model_code context=%s value=%s",context,q->utf8);}}
}
static long long diag6j_number(struct dvm *v,dvm_ref ref,const char *name,long long absent){
 struct dvm_object *o=diag6j_obj(v,ref);if(!o||!o->slots)return absent;for(struct dvm_class *c=o->cls;c;c=c->super)for(int i=0;i<c->nifields;i++){struct dvm_field *f=&c->ifields[i];if(!strcmp(f->name,name)&&f->slot<o->cls->islots)return f->kind=='J'?o->slots[f->slot].j:o->slots[f->slot].i;}return absent;
}
static struct {uint64_t vm,generation,source_generation;dvm_ref ref,source;unsigned strings,bytes,sources,closes;} diag6j_bodies[128];
static unsigned diag6j_next;
static int diag6j_fixture;
static void diag6j_call(struct dvm *v,struct dvm_method *m,dvm_ref self,const uint32_t *a,int n,const union dvm_value *out,int entering,int ok){
 /* GC-STEP6: this operation owns all raw diagnostic/fixture borrows. */
 struct gc_operation gc6_operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&gc6_operation))return;

 if(!m||!m->cls)return;const char *c=m->cls->name;
 if(diag6j_fixture&&!strcmp(m->name,"toString")&&self){struct dvm_object *o=diag6j_obj(v,self);if(o&&o->cls&&!strcmp(o->cls->name,"java/io/StringWriter"))diag6j_log(v,"kind=fixture_writer stage=%s resolved_owner=%s receiver=%s pc=0x%x",entering?"enter":"return",c,o->cls->name,v->cur_pc);}
 bool body=diag6j_class(v,self,"okhttp3/ResponseBody");
 if(body&&(!strcmp(m->name,"string")||!strcmp(m->name,"bytes")||!strcmp(m->name,"source")||!strcmp(m->name,"close"))){
  struct dvm_object *receiver=diag6j_obj(v,self);if(!receiver)return;uint64_t generation=receiver->identity_generation;
  unsigned index=128;
  for(unsigned j=0;j<128;j++){
   if(diag6j_bodies[j].vm&&diag6j_bodies[j].vm!=v->serial){memset(&diag6j_bodies[j],0,sizeof diag6j_bodies[j]);continue;}
   if(diag6j_bodies[j].vm==v->serial&&diag6j_bodies[j].ref==self){
    if(diag6j_bodies[j].generation==generation){index=j;break;}
    memset(&diag6j_bodies[j],0,sizeof diag6j_bodies[j]);
   }
  }
  if(index==128){index=diag6j_next++%128;memset(&diag6j_bodies[index],0,sizeof diag6j_bodies[index]);diag6j_bodies[index].vm=v->serial;diag6j_bodies[index].ref=self;diag6j_bodies[index].generation=generation;}
  if(entering){if(!strcmp(m->name,"string"))diag6j_bodies[index].strings++;else if(!strcmp(m->name,"bytes"))diag6j_bodies[index].bytes++;else if(!strcmp(m->name,"source"))diag6j_bodies[index].sources++;else diag6j_bodies[index].closes++;}
  if(!entering&&out&&!strcmp(m->name,"source")&&ok){
   struct dvm_object *source_object=diag6j_obj(v,out->l);
   diag6j_bodies[index].source=source_object?out->l:0;
   diag6j_bodies[index].source_generation=source_object?source_object->identity_generation:0;
  }
  if(diag6j_bodies[index].source&&!diag6j_identity(v,diag6j_bodies[index].source,diag6j_bodies[index].source_generation)){
   diag6j_bodies[index].source=0;diag6j_bodies[index].source_generation=0;
  }
  dvm_ref source=diag6j_bodies[index].source,buffer=diag6j_ref(v,source,"buffer");if(diag6j_class(v,source,"okio/Buffer"))buffer=source;
  if(strcmp(m->name,"source")||entering){diag6j_log(v,"kind=body stage=%s owner=%s method=%s self=%u string_calls=%u bytes_calls=%u source_calls=%u close_calls=%u source_ref=%u source_closed=%lld buffer_size=%lld caller=%s.%s pc=0x%x ok=%d exception=%u",entering?"enter":"return",c,m->name,self,diag6j_bodies[index].strings,diag6j_bodies[index].bytes,diag6j_bodies[index].sources,diag6j_bodies[index].closes,source,diag6j_number(v,source,"closed",-1),diag6j_number(v,buffer,"size",-1),v->cur_method&&v->cur_method->cls?v->cur_method->cls->name:"host",v->cur_method?v->cur_method->name:"host",v->cur_pc,ok,v->exception);}
  if(!entering&&out&&!strcmp(m->name,"string")){diag6j_body_string(v,self,out->l);diag6j_string(v,out->l,"ResponseBody.string.return");}
 }
 /* Slots contain primitives too. Resolving an int as an object ID changes
  * native-scope ownership (and may inspect an unrelated live object). */
 bool first_is_ref=m->sig&&m->sig[0]=='('&&(m->sig[1]=='L'||m->sig[1]=='[');
 bool callback_model=first_is_ref&&n&&a&&diag6j_class(v,a[0],"com/gsc/base/model/CallBackModel");
 bool string_parse=n>1&&a&&!strcmp(m->name,"fromJson")&&strstr(m->sig,"(Ljava/lang/String;");
 if(string_parse&&entering){struct dvm_object *t=diag6j_obj(v,a[1]);const char *target=t&&t->kind==DVM_OBJ_CLASS&&t->klass?t->klass->name:t&&t->cls?t->cls->name:"unknown";diag6j_log(v,"kind=fromJson_enter caller=%s.%s pc=0x%x target=%s",v->cur_method&&v->cur_method->cls?v->cur_method->cls->name:"host",v->cur_method?v->cur_method->name:"host",v->cur_pc,target);diag6j_compare(v,a[0],target);diag6j_string(v,a[0],"JSON.fromJson.actual_input");}
 if(!entering&&out&&n==1&&a&&!strcmp(m->sig,"(Lokhttp3/ResponseBody;)Ljava/lang/String;"))diag6j_body_string(v,a[0],out->l);
 if(!entering&&string_parse){struct dvm_object *type=diag6j_obj(v,a[1]);if(type&&type->kind==DVM_OBJ_CLASS&&type->klass&&!strcmp(type->klass->name,"com/gsc/base/model/UserInfoModel")){struct dvm_object *error=diag6j_obj(v,v->exception);diag6j_log(v,"kind=userinfo_parse_return ok=%d result_ref=%u exception_type=%s steps=%llu limit=%llu",ok,out?out->l:0,error&&error->cls?error->cls->name:"none",(unsigned long long)v->call_steps,(unsigned long long)v->step_limit);}}
 if(!entering&&(!strncmp(c,"com/gsc/base/",13)||!strncmp(c,"com/gsc/account_login/",22)||!strcmp(c,"java/util/Observable"))&&(!ok||v->exception)){struct dvm_object *error=diag6j_obj(v,v->exception);diag6j_log(v,"kind=post_login_call_failure owner=%s method=%s sig=%s caller_pc=0x%x exception_type=%s steps=%llu limit=%llu",c,m->name,m->sig,v->cur_pc,error&&error->cls?error->cls->name:"none",(unsigned long long)v->call_steps,(unsigned long long)v->step_limit);}
 bool parse=string_parse;
 struct dvm_object *type=parse?diag6j_obj(v,a[1]):NULL;parse=type&&type->kind==DVM_OBJ_CLASS&&type->klass&&!strcmp(type->klass->name,"com/gsc/base/model/CallBackModel");
 if(callback_model&&!strcmp(m->name,"toJson")){if(entering)diag6j_model(v,a[0],"CallBackModel.toJson.input");else if(out&&strstr(m->sig,")Ljava/lang/String;")){struct dvm_object *text=diag6j_obj(v,out->l);diag6j_log(v,"kind=callback_model_json_shape ok=%d bytes=%u",text&&text->kind==DVM_OBJ_STRING&&text->utf8_len&&text->utf8[0]=='{',text&&text->kind==DVM_OBJ_STRING?text->utf8_len:0);}}
 if(parse){if(entering)diag6j_string(v,a[0],"CallBackModel.fromJson.input");else if(out){diag6j_log(v,"kind=parse_return owner=%s method=%s ok=%d exception=%u",c,m->name,ok,v->exception);diag6j_model(v,out->l,"CallBackModel.fromJson.return");}}
 if(!strcmp(c,"com/gsc/pub/GSCPubCommon$d")&&!strcmp(m->name,"onFailed")&&entering&&n)diag6j_string(v,a[0],"GSCPubCommon$d.onFailed.input");
}
void task6j_diagnostic_catch(struct dvm *v,struct dvm_method *m,uint32_t pc,uint32_t handler){
 /* GC-STEP6: this operation owns all raw diagnostic/fixture borrows. */
 struct gc_operation gc6_operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&gc6_operation))return;

 if(!m||!m->cls||strcmp(m->cls->name,"com/gsc/pub/GSCPubCommon$d")||strcmp(m->name,"onFailed"))return;
 struct dvm_object *e=diag6j_obj(v,v->exception);diag6j_log(v,"kind=2001_caught owner=%s method=%s pc=0x%x handler=0x%x original_type=%s exception_ref=%u parked=%d",m->cls->name,m->name,pc,handler,e&&e->cls?e->cls->name:"unknown",v->exception,v->parked);
 dvm_ref message=diag6j_ref(v,v->exception,"detailMessage");diag6j_string(v,message,"2001.original_exception_message");
 for(int i=0;i<v->nexc_trace&&i<12;i++){struct dvm_method *f=v->exc_trace[i];diag6j_log(v,"kind=2001_frame depth=%d owner=%s method=%s sig=%s throw_pc=0x%x",i,f&&f->cls?f->cls->name:"?",f?f->name:"?",f&&f->sig?f->sig:"?",v->exc_pc);}
}
