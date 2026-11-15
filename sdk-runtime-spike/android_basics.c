/* VM-local Android value/identity contracts. This does not provide an Android
 * UI thread, queue, scheduler, process identity, or SDK business service. */
#include "android_basics.h"
#include "portable.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <pthread.h>
#include "guest_identity.h"
static struct {struct dvm *vm;dvm_ref main_looper,application;pthread_t owner;} state;
struct handler {dvm_ref looper,callback;bool async;};
struct context_wrapper{dvm_ref base;};
struct handler_thread{dvm_ref name;int priority;};
struct sqlite_helper{dvm_ref context,name,factory,database;int version;};
struct sqlite_database{dvm_ref name;bool open;unsigned exec_count;int64_t logical_count;};
struct sqlite_cursor{int position,row_count;int64_t scalar;bool closed;};
void android_basics_vm_start(struct dvm *v){
 state.vm=v;state.main_looper=0;state.application=0;state.owner=pthread_self();
 struct dvm_class *context=dvm__class_by_desc(v,"Landroid/content/Context;");
 struct dvm_class *wrapper=dvm__class_by_desc(v,"Landroid/content/ContextWrapper;");
 struct dvm_class *app=dvm__class_by_desc(v,"Landroid/app/Application;");
 if(!context||!wrapper||!app){dvm__throw(v,"java/lang/OutOfMemoryError","Context type metadata");return;}
 wrapper->super=context;app->super=wrapper;
 struct dvm_class *thread=dvm__class_by_desc(v,"Ljava/lang/Thread;"),*ht=dvm__class_by_desc(v,"Landroid/os/HandlerThread;");if(thread&&ht)ht->super=thread;
}
dvm_ref android_basics_application(struct dvm *v){
 if(state.vm!=v){dvm__throw(v,"java/lang/IllegalStateException","Application belongs to another VM");return 0;}
 if(!state.application)state.application=dvm_new_object(v,dvm_find_class_or_external(v,"android/app/Application"));
 return state.application;
}
static bool context_name(const char *c){return !strcmp(c,"android/content/Context")||!strcmp(c,"android/content/ContextWrapper")||!strcmp(c,"android/app/Application");}
static dvm_ref application_for(struct dvm *v,dvm_ref receiver,unsigned depth){
 if(!receiver){dvm__throw(v,"java/lang/NullPointerException","Context base");return 0;}
 if(state.vm==v&&receiver==state.application)return receiver;
 struct dvm_object *o=dvm__obj(v,receiver);bool wrapper=false;
 for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)if(!strcmp(c->name,"android/content/ContextWrapper"))wrapper=true;
 if(depth<64&&wrapper&&o->data){struct context_wrapper *w=o->data;return application_for(v,w->base,depth+1);}
 sdk_trace_missing_api(o&&o->cls?o->cls->name:"?","getApplicationContext","()Landroid/content/Context;");
 dvm__throw(v,"java/lang/UnsupportedOperationException","Context has no bound diagnostic Application");return 0;
}
static dvm_ref main_looper(struct dvm *v){
 if(state.vm!=v){dvm__throw(v,"java/lang/IllegalStateException","VM platform identity not initialized");return 0;}
 if(!state.main_looper)state.main_looper=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Looper"));
 return state.main_looper;
}
static bool owner(struct dvm *v){return state.vm==v&&pthread_equal(state.owner,pthread_self());}
static dvm_ref from_units(struct dvm *v,const uint16_t *u,int n){
 char *s=malloc((size_t)n*3+1);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","String units");return 0;}size_t at=0;
 for(int i=0;i<n;++i){uint32_t cp=u[i];if(cp>=0xd800&&cp<=0xdbff&&i+1<n&&u[i+1]>=0xdc00&&u[i+1]<=0xdfff){cp=0x10000+((cp-0xd800)<<10)+(u[++i]-0xdc00);s[at++]=(char)(0xf0|(cp>>18));s[at++]=(char)(0x80|((cp>>12)&63));s[at++]=(char)(0x80|((cp>>6)&63));s[at++]=(char)(0x80|(cp&63));}else if(cp&&cp<128)s[at++]=(char)cp;else if(cp<2048){s[at++]=(char)(0xc0|(cp>>6));s[at++]=(char)(0x80|(cp&63));}else{s[at++]=(char)(0xe0|(cp>>12));s[at++]=(char)(0x80|((cp>>6)&63));s[at++]=(char)(0x80|(cp&63));}}
 dvm_ref r=dvm_new_string_n(v,s,at);free(s);return r;
}
/* Return the next UTF-16 code unit; MUTF-8 encoded surrogate units are retained.
 * Four-byte UTF-8 scalar values are expanded into a surrogate pair. */
static uint16_t *units(struct dvm *v,dvm_ref ref,int *length){
 struct dvm_object *o=dvm__obj(v,ref);if(!o||o->kind!=DVM_OBJ_STRING)return NULL;
 uint16_t *u=malloc((o->utf8_len+1)*sizeof *u);if(!u){dvm__throw(v,"java/lang/OutOfMemoryError","string UTF16");return NULL;}
 size_t n=0;
 for(size_t i=0;i<o->utf8_len;){
  uint32_t cp=(unsigned char)o->utf8[i++];unsigned tail=0;
  if(cp>=0xc0&&cp<0xe0){cp&=31;tail=1;}else if(cp>=0xe0&&cp<0xf0){cp&=15;tail=2;}else if(cp>=0xf0&&cp<0xf8){cp&=7;tail=3;}
  else if(cp>=0x80){free(u);dvm__throw(v,"java/lang/IllegalArgumentException","invalid UTF8 string");return NULL;}
  if(i+tail>o->utf8_len){free(u);dvm__throw(v,"java/lang/IllegalArgumentException","truncated UTF8 string");return NULL;}
  for(unsigned j=0;j<tail;++j){unsigned char b=o->utf8[i++];if((b&0xc0)!=0x80){free(u);dvm__throw(v,"java/lang/IllegalArgumentException","invalid UTF8 continuation");return NULL;}cp=cp<<6|(b&63);}
  if(cp>0x10ffff){free(u);dvm__throw(v,"java/lang/IllegalArgumentException","invalid Unicode scalar");return NULL;}
  if(cp>0xffff){cp-=0x10000;u[n++]=(uint16_t)(0xd800+(cp>>10));u[n++]=(uint16_t)(0xdc00+(cp&1023));}else u[n++]=(uint16_t)cp;
 }
 *length=(int)n;return u;
}
static bool length(struct dvm *v,dvm_ref ref,int *out){
 struct dvm_object *o=dvm__obj(v,ref);if(!o){dvm__throw(v,"java/lang/NullPointerException","CharSequence");return false;}
 if(o->kind==DVM_OBJ_STRING){uint16_t *u=units(v,ref,out);if(!u)return false;free(u);return true;}
 struct dvm_method *method=dvm_find_method(v,o->cls,"length","()I");
 if(!method){sdk_trace_missing_api(o->cls->name,"length","()I");dvm__throw(v,"java/lang/NoSuchMethodError","%s.length()I",o->cls->name);return false;}
 union dvm_value result={0};if(!dvm_call(v,method,ref,NULL,0,&result))return false;
 if(result.i<0){dvm__throw(v,"java/lang/IllegalStateException","CharSequence.length returned negative");return false;}
 *out=result.i;return true;
}
#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
static bool sqlite_contains_ci(const char *text,const char *needle){
 if(!text||!needle||!*needle)return true;size_t n=strlen(needle);
 for(const char *p=text;*p;++p){size_t i=0;for(;i<n&&p[i];++i)if(tolower((unsigned char)p[i])!=tolower((unsigned char)needle[i]))break;if(i==n)return true;}
 return false;
}
static struct sqlite_cursor *sqlite_cursor_data(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);return o&&o->data?(struct sqlite_cursor *)o->data:NULL;
}
static bool sqlite_cursor_ready(struct dvm *v,struct sqlite_cursor *cur){
 if(!cur){dvm__throw(v,"java/lang/IllegalStateException","Cursor is uninitialized");return false;}
 if(cur->closed){dvm__throw(v,"java/lang/IllegalStateException","Cursor is closed");return false;}
 return true;
}
static bool sqlite_cursor_value(struct dvm *v,struct sqlite_cursor *cur,int column){
 if(!sqlite_cursor_ready(v,cur))return false;
 if(column<0||column>=1){dvm__throw(v,"android/database/CursorIndexOutOfBoundsException","column index %d",column);return false;}
 if(cur->position<0||cur->position>=cur->row_count){dvm__throw(v,"android/database/CursorIndexOutOfBoundsException","cursor position %d",cur->position);return false;}
 return true;
}
static bool sqlite_cursor_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,union dvm_value *out){
 struct sqlite_cursor *cur=sqlite_cursor_data(v,self);if(!cur)return false;
 if(IS("close","()V")){cur->closed=true;return true;}
 if(IS("isClosed","()Z")){out->i=cur->closed;return true;}
 if(!sqlite_cursor_ready(v,cur))return true;
 if(IS("getCount","()I")){out->i=cur->row_count;return true;}
 if(IS("getPosition","()I")){out->i=cur->position;return true;}
 if(IS("moveToFirst","()Z")){cur->position=cur->row_count?0:-1;out->i=cur->row_count!=0;return true;}
 if(IS("moveToLast","()Z")){cur->position=cur->row_count?cur->row_count-1:-1;out->i=cur->row_count!=0;return true;}
 if(IS("moveToNext","()Z")){if(cur->position<cur->row_count-1)++cur->position;else cur->position=cur->row_count;out->i=cur->position<cur->row_count;return true;}
 if(IS("moveToPrevious","()Z")){if(cur->position>0)--cur->position;else cur->position=-1;out->i=cur->position>=0;return true;}
 if(IS("moveToPosition","(I)Z")){int p=a[0].i;if(p<0){cur->position=-1;out->i=cur->row_count==0&&p==0;return true;}if(p>=cur->row_count){cur->position=cur->row_count;out->i=0;return true;}cur->position=p;out->i=1;return true;}
 if(IS("isBeforeFirst","()Z")){out->i=cur->row_count>0&&cur->position<0;return true;}
 if(IS("isAfterLast","()Z")){out->i=cur->row_count>0&&cur->position>=cur->row_count;return true;}
 if(IS("isFirst","()Z")){out->i=cur->row_count>0&&cur->position==0;return true;}
 if(IS("isLast","()Z")){out->i=cur->row_count>0&&cur->position==cur->row_count-1;return true;}
 if(IS("getColumnCount","()I")){out->i=1;return true;}
 if(IS("getColumnIndex","(Ljava/lang/String;)I")){out->i=a[0].l?0:-1;return true;}
 if(IS("getColumnIndexOrThrow","(Ljava/lang/String;)I")){if(!a[0].l){dvm__throw(v,"java/lang.IllegalArgumentException","column name is null");return true;}out->i=0;return true;}
 if(IS("getColumnName","(I)Ljava/lang/String;")){if(a[0].i!=0){dvm__throw(v,"android/database/CursorIndexOutOfBoundsException","column index %d",a[0].i);return true;}out->l=dvm_new_string(v,"count(*)");return true;}
 if(IS("getColumnNames","()[Ljava/lang/String;")){out->l=dvm_new_array(v,'L',"Ljava/lang/String;",1);if(out->l)((dvm_ref *)dvm_array_data(v,out->l))[0]=dvm_new_string(v,"count(*)");return true;}
 if(IS("isNull","(I)Z")){if(!sqlite_cursor_value(v,cur,a[0].i))return true;out->i=0;return true;}
 if(IS("getLong","(I)J")){if(sqlite_cursor_value(v,cur,a[0].i))out->j=cur->scalar;return true;}
 if(IS("getInt","(I)I")){if(sqlite_cursor_value(v,cur,a[0].i))out->i=(int32_t)cur->scalar;return true;}
 if(IS("getShort","(I)S")){if(sqlite_cursor_value(v,cur,a[0].i))out->i=(int16_t)cur->scalar;return true;}
 if(IS("getFloat","(I)F")){if(sqlite_cursor_value(v,cur,a[0].i))out->f=(float)cur->scalar;return true;}
 if(IS("getDouble","(I)D")){if(sqlite_cursor_value(v,cur,a[0].i))out->d=(double)cur->scalar;return true;}
 if(IS("getString","(I)Ljava/lang/String;")){if(sqlite_cursor_value(v,cur,a[0].i)){char buf[64];snprintf(buf,sizeof buf,"%lld",(long long)cur->scalar);out->l=dvm_new_string(v,buf);}return true;}
 if(IS("getBlob","(I)[B")){if(!sqlite_cursor_value(v,cur,a[0].i))return true;out->l=0;return true;}
 return false;
}
bool android_basics_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;memset(out,0,sizeof *out);
 bool thread_receiver=false;struct dvm_object *thread_obj=self?dvm__obj(v,self):NULL;for(struct dvm_class *k=thread_obj?thread_obj->cls:NULL;k;k=k->super)if(!strcmp(k->name,"android/os/HandlerThread"))thread_receiver=true;
 if(thread_receiver&&(!strcmp(c,"android/os/HandlerThread")||!strcmp(c,"java/lang/Thread"))){
  if(IS("<init>","(Ljava/lang/String;)V")||IS("<init>","(Ljava/lang/String;I)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","thread name");return true;}struct handler_thread *t=calloc(1,sizeof *t);if(!t){dvm__throw(v,"java/lang/OutOfMemoryError","HandlerThread");return true;}t->name=a[0].l;t->priority=nargs>1?a[1].i:0;free(thread_obj->data);thread_obj->data=t;return true;}
  struct handler_thread *t=thread_obj->data;if(!t){dvm__throw(v,"java/lang/IllegalStateException","HandlerThread not constructed");return true;}
  if(IS("getName","()Ljava/lang/String;")){out->l=t->name;return true;}if(IS("setName","(Ljava/lang/String;)V")){if(!a[0].l)dvm__throw(v,"java/lang/NullPointerException","thread name");else t->name=a[0].l;return true;}
  if(IS("isAlive","()Z")){out->i=0;return true;}if(IS("getLooper","()Landroid/os/Looper;")){out->l=0;return true;}
  if(IS("start","()V"))sdk_trace_fprintf(stderr,"SCHEDULER_BOUNDARY HandlerThread remains NEW; no worker/Looper was started\n");return false;
 }
 if(!strcmp(c,"java/lang/String")&&(IS("substring","(II)Ljava/lang/String;")||IS("substring","(I)Ljava/lang/String;"))){int n=0;uint16_t *u=units(v,self,&n);if(!u)return true;int begin=a[0].i,end=IS("substring","(I)Ljava/lang/String;")?n:a[1].i;if(begin<0||end<begin||end>n)dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","substring bounds");else out->l=from_units(v,u+begin,end-begin);free(u);return true;}
 if(!strcmp(c,"java/lang/String")&&IS("toLowerCase","()Ljava/lang/String;")){const char *s=dvm_string_utf8(v,self);if(!s)return false;size_t n=dvm_string_utf8_length(v,self);for(size_t i=0;i<n;++i)if(!strchr("0123456789abcdefABCDEF-",s[i]))return false;char *lower=malloc(n+1);if(!lower){dvm__throw(v,"java/lang/OutOfMemoryError","hex lowercase");return true;}for(size_t i=0;i<n;++i)lower[i]=(s[i]>='A'&&s[i]<='F')?s[i]+32:s[i];out->l=dvm_new_string_n(v,lower,n);free(lower);return true;}
 if(!strcmp(c,"java/lang/String")&&IS("replaceAll","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")){
  const char *source=dvm_string_utf8(v,self),*pattern=dvm_string_utf8(v,a[0].l),*replacement=dvm_string_utf8(v,a[1].l);if(!source||!pattern||!replacement){dvm__throw(v,"java/lang/NullPointerException","replaceAll args");return true;}
  if(!*pattern||strpbrk(pattern,"\\.^$|?*+(){}[]")||strpbrk(replacement,"\\$"))return false;
  size_t n=strlen(source),p=strlen(pattern),r=strlen(replacement),count=0;if(n>1048576){dvm__throw(v,"java/lang/OutOfMemoryError","diagnostic replacement limit");return true;}for(const char *s=source;(s=strstr(s,pattern));s+=p)++count;
  if(r>p&&count>(1048576-n)/(r-p)){dvm__throw(v,"java/lang/OutOfMemoryError","replaceAll output");return true;}size_t size=n+count*r-count*p;char *result=malloc(size+1);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","replaceAll");return true;}size_t at=0;const char *s=source,*hit;while((hit=strstr(s,pattern))){size_t part=(size_t)(hit-s);memcpy(result+at,s,part);at+=part;memcpy(result+at,replacement,r);at+=r;s=hit+p;}strcpy(result+at,s);out->l=dvm_new_string_n(v,result,size);free(result);return true;
 }
 if(context_name(c)){
  if(!strcmp(c,"android/content/ContextWrapper")&&IS("<init>","(Landroid/content/Context;)V")){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ContextWrapper receiver");return true;}
   struct context_wrapper *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","ContextWrapper base");return true;}w->base=a[0].l;free(o->data);o->data=w;return true;
  }
  if(IS("getApplicationContext","()Landroid/content/Context;")){out->l=application_for(v,self,0);return true;}
  if(IS("getPackageName","()Ljava/lang/String;")){if(application_for(v,self,0))out->l=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);return true;}
  if(!strcmp(c,"android/content/ContextWrapper")&&IS("getBaseContext","()Landroid/content/Context;")){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ContextWrapper receiver");return true;}struct context_wrapper *w=o->data;out->l=w?w->base:0;return true;
  }
 }
 if(!strcmp(c,"java/lang/System")&&IS("currentTimeMillis","()J")){
  struct timespec now;if(clock_gettime(CLOCK_REALTIME,&now))dvm__throw(v,"java/lang/IllegalStateException","host wall clock unavailable");else out->j=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;return true;
 }
 if(!strcmp(c,"android/text/TextUtils")&&IS("isEmpty","(Ljava/lang/CharSequence;)Z")){
  if(!a[0].l){out->i=1;return true;}int n=0;if(length(v,a[0].l,&n))out->i=n==0;return true;
 }
 if(!strcmp(c,"java/lang/String")&&(IS("length","()I")||IS("isEmpty","()Z")||IS("charAt","(I)C")||IS("indexOf","(I)I")||IS("indexOf","(II)I"))){
  int n=0;uint16_t *u=units(v,self,&n);if(!u){if(!dvm_exception(v))dvm__throw(v,"java/lang/NullPointerException","String receiver");return true;}
  if(IS("length","()I"))out->i=n;else if(IS("isEmpty","()Z"))out->i=n==0;
  else if(IS("charAt","(I)C")){if(a[0].i<0||a[0].i>=n)dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","charAt index");else out->i=u[a[0].i];}
  else {int start=IS("indexOf","(II)I")?a[1].i:0;if(start<0)start=0;int32_t cp=a[0].i;out->i=-1;
   for(int i=start;i<n;++i){if(cp>=0&&cp<=0xffff&&u[i]==cp){out->i=i;break;}if(cp>0xffff&&cp<=0x10ffff&&i+1<n&&u[i]==0xd800+((cp-0x10000)>>10)&&u[i+1]==0xdc00+((cp-0x10000)&1023)){out->i=i;break;}}}
  free(u);return true;
 }
 if(!strcmp(c,"android/os/Looper")){
  if(IS("getMainLooper","()Landroid/os/Looper;")){out->l=main_looper(v);return true;}
  if(IS("myLooper","()Landroid/os/Looper;")){out->l=owner(v)?main_looper(v):0;return true;}
  if(IS("isCurrentThread","()Z")){out->i=self&&self==main_looper(v)&&owner(v);return true;}
  /* loop/prepare/quit/getThread and queue APIs have no implementation. */
  return false;
 }
 if(!strcmp(c,"android/os/Handler")){
  bool explicit_looper=IS("<init>","(Landroid/os/Looper;)V")||IS("<init>","(Landroid/os/Looper;Landroid/os/Handler$Callback;)V")||IS("<init>","(Landroid/os/Looper;Landroid/os/Handler$Callback;Z)V");
  bool implicit_looper=IS("<init>","()V")||IS("<init>","(Landroid/os/Handler$Callback;)V");
  if(!explicit_looper&&!implicit_looper&&!IS("getLooper","()Landroid/os/Looper;"))return false;
  struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","Handler receiver");return true;}
  if(explicit_looper||implicit_looper){
   dvm_ref l=explicit_looper?a[0].l:(owner(v)?main_looper(v):0);
   if(!l){dvm__throw(v,explicit_looper?"java/lang/NullPointerException":"java/lang/RuntimeException","Handler requires a prepared Looper");return true;}
   if(l!=main_looper(v)){dvm__throw(v,"java/lang/UnsupportedOperationException","unmodeled Looper identity");return true;}
   struct handler *h=calloc(1,sizeof *h);if(!h){dvm__throw(v,"java/lang/OutOfMemoryError","Handler");return true;}
   h->looper=l;if(explicit_looper&&strcmp(sig,"(Landroid/os/Looper;)V"))h->callback=a[1].l;
   if(implicit_looper&&!IS("<init>","()V"))h->callback=a[0].l;
   if(IS("<init>","(Landroid/os/Looper;Landroid/os/Handler$Callback;Z)V"))h->async=!!a[2].i;
   free(o->data);o->data=h;return true;
  }
  struct handler *h=o->data;if(!h)dvm__throw(v,"java/lang/IllegalStateException","Handler constructor not called");else out->l=h->looper;
  return true;
 }
 if(!strcmp(c,"android/database/sqlite/SQLiteOpenHelper")){
  if(IS("<init>","(Landroid/content/Context;Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;I)V")){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","SQLiteOpenHelper receiver");return true;}
   if(!a[0].l||!a[1].l){dvm__throw(v,"java/lang/NullPointerException","SQLiteOpenHelper context/name");return true;}
   struct sqlite_helper *h=calloc(1,sizeof *h);if(!h){dvm__throw(v,"java/lang/OutOfMemoryError","SQLiteOpenHelper state");return true;}
   h->context=a[0].l;h->name=a[1].l;h->factory=a[2].l;h->version=a[3].i;free(o->data);o->data=h;return true;
  }
  struct dvm_object *o=dvm__obj(v,self);struct sqlite_helper *h=o?o->data:NULL;
  if(!h){dvm__throw(v,"java/lang/IllegalStateException","SQLiteOpenHelper not initialized");return true;}
  if(IS("getDatabaseName","()Ljava/lang/String;")){out->l=h->name;return true;}
  if(IS("setWriteAheadLoggingEnabled","(Z)V"))return true;
  if(IS("close","()V")){if(h->database){struct sqlite_database *db=dvm__obj(v,h->database)->data;if(db)db->open=false;}return true;}
  if(IS("getWritableDatabase","()Landroid/database/sqlite/SQLiteDatabase;")||IS("getReadableDatabase","()Landroid/database/sqlite/SQLiteDatabase;")){
   if(!h->database){struct dvm_class *dbcls=dvm_find_class_or_external(v,"android/database/sqlite/SQLiteDatabase");h->database=dvm_new_object(v,dbcls);if(!h->database){dvm__throw(v,"java/lang/OutOfMemoryError","SQLiteDatabase");return true;}struct sqlite_database *db=calloc(1,sizeof *db);if(!db){dvm__throw(v,"java/lang/OutOfMemoryError","SQLiteDatabase state");return true;}db->name=h->name;db->open=true;dvm__obj(v,h->database)->data=db;
    struct dvm_method *create=dvm_find_method(v,dvm_object_class(v,self),"onCreate","(Landroid/database/sqlite/SQLiteDatabase;)V");if(create&&(create->has_code||create->builtin)){union dvm_value arg={.l=h->database},ignored={0};if(!dvm_call(v,create,self,&arg,1,&ignored))return true;}
   }else{struct sqlite_database *db=dvm__obj(v,h->database)->data;if(db&&!db->open){db->open=true;}}
   out->l=h->database;return true;
  }
  if(IS("onCreate","(Landroid/database/sqlite/SQLiteDatabase;)V")||IS("onUpgrade","(Landroid/database/sqlite/SQLiteDatabase;II)V")||IS("onDowngrade","(Landroid/database/sqlite/SQLiteDatabase;II)V")||IS("onOpen","(Landroid/database/sqlite/SQLiteDatabase;)V"))return true;
 }
 if(!strcmp(c,"android/database/sqlite/SQLiteDatabase")){
  struct dvm_object *o=dvm__obj(v,self);struct sqlite_database *db=o?o->data:NULL;
  if(IS("isOpen","()Z")){out->i=db&&db->open;return true;}
  if(IS("close","()V")){if(db)db->open=false;return true;}
  if(IS("getPath","()Ljava/lang/String;")){out->l=db?db->name:0;return true;}
  if(IS("execSQL","(Ljava/lang/String;)V")){
   if(!db||!db->open){dvm__throw(v,"java/lang/IllegalStateException","SQLiteDatabase is closed");return true;}
   if(!a[0].l||!dvm_string_utf8(v,a[0].l)){dvm__throw(v,"java/lang/NullPointerException","SQL statement");return true;}
   if(db->exec_count!=4294967295u)++db->exec_count;
   const char *sql=dvm_string_utf8(v,a[0].l);
   if(sqlite_contains_ci(sql,"insert")&&db->logical_count<9223372036854775807LL)++db->logical_count;
   else if(sqlite_contains_ci(sql,"delete")&&db->logical_count>0)--db->logical_count;
   return true;
  }
  if(IS("rawQuery","(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;")){
   if(!db||!db->open){dvm__throw(v,"java/lang/IllegalStateException","SQLiteDatabase is closed");return true;}
   if(!a[0].l||!dvm_string_utf8(v,a[0].l)){dvm__throw(v,"java/lang/NullPointerException","SQL query");return true;}
   const char *sql=dvm_string_utf8(v,a[0].l);struct dvm_class *cursor_cls=dvm_find_class_or_external(v,"android/database/Cursor");
   dvm_ref cursor=cursor_cls?dvm_new_object(v,cursor_cls):0;struct dvm_object *co=cursor?dvm__obj(v,cursor):NULL;struct sqlite_cursor *cur=co?calloc(1,sizeof *cur):NULL;
   if(!cursor||!co||!cur){free(cur);dvm__throw(v,"java/lang/OutOfMemoryError","SQLite cursor");return true;}
   cur->position=-1;
   if(sqlite_contains_ci(sql,"select")&&sqlite_contains_ci(sql,"count")){cur->row_count=1;cur->scalar=db->logical_count;}
   else cur->row_count=0;
   co->data=cur;out->l=cursor;return true;
  }
 }
 if(!strcmp(c,"android/database/Cursor"))return sqlite_cursor_call(v,m,sig,self,a,out);
 return false;
}
