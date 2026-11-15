#include "json_runtime.h"
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
extern bool java_basics_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
static void fail(struct dvm *v){dvm__throw(v,"org/json/JSONException","Invalid JSON document or incompatible/missing value");}
void json_runtime_vm_start(struct dvm *v){
 struct dvm_class *c=dvm_find_class_or_external(v,"org/json/JSONException"),*p=dvm_find_class_or_external(v,"java/lang/Exception");if(c&&p&&c->external)c->super=p;
 /* Real implemented members, including the virtual Object.toString override.
  * Without metadata, VM lookup inherited Object.toString and sent the
  * JSONObject identity string into the original game's JSON parser. */
 static const struct {const char *name,*sig;} rows[]={
  {"<init>","()V"},{"<init>","(Ljava/lang/String;)V"},
  {"toString","()Ljava/lang/String;"},{"length","()I"},
  {"getJSONObject","(Ljava/lang/String;)Lorg/json/JSONObject;"},
  {"getString","(Ljava/lang/String;)Ljava/lang/String;"},
  {"optString","(Ljava/lang/String;)Ljava/lang/String;"},
  {"optString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"},
  {"has","(Ljava/lang/String;)Z"},{"isNull","(Ljava/lang/String;)Z"},
  {"put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;"},
  {"put","(Ljava/lang/String;I)Lorg/json/JSONObject;"},
  {"put","(Ljava/lang/String;J)Lorg/json/JSONObject;"},
  {"put","(Ljava/lang/String;D)Lorg/json/JSONObject;"},
  {"put","(Ljava/lang/String;Z)Lorg/json/JSONObject;"}};
 c=dvm_find_class_or_external(v,"org/json/JSONObject");if(!c||!c->external||c->nmethods)return;
 size_t count=sizeof rows/sizeof rows[0];struct dvm_method *ms=calloc(count,sizeof *ms);
 if(!ms){dvm__throw(v,"java/lang/OutOfMemoryError","JSONObject method metadata");return;}
 for(size_t i=0;i<count;i++){
  ms[i].cls=c;ms[i].name=rows[i].name;ms[i].sig=strdup(rows[i].sig);
  if(!ms[i].sig){for(size_t j=0;j<i;j++)free(ms[j].sig);free(ms);dvm__throw(v,"java/lang/OutOfMemoryError","JSONObject signature");return;}
  ms[i].access=DEX_ACC_PUBLIC|(!strcmp(rows[i].name,"<init>")?0x10000:0);
  ms[i].arg_slots=dvm_sig_arg_slots(ms[i].sig);ms[i].arg_count=dvm_sig_arg_count(ms[i].sig);ms[i].ret_kind=dvm_sig_return_kind(ms[i].sig);
 }
 free(c->mcache);c->mcache=NULL;c->mcache_cap=c->mcache_len=0;
 c->methods=ms;c->nmethods=(int)count;
}
static void destroy_json(void *p){free(p);}
typedef struct {char *p;size_t n,cap;} buffer;
static bool append(buffer *b,const char *s,size_t n){if(n>1048576-b->n)return false;if(b->n+n+1>b->cap){size_t cap=(b->n+n+1)*2;char *p=realloc(b->p,cap);if(!p)return false;b->p=p;b->cap=cap;}memcpy(b->p+b->n,s,n);b->n+=n;b->p[b->n]=0;return true;}
/* JSON string escaping for JSONObject.put(String,Object).  UTF-8 bytes are
 * passed through unchanged; control characters and quote/backslash use the
 * JSON escapes so the appended member remains valid JSON. */
static bool append_json_string(buffer *b,const char *s,size_t n){
 if(!append(b,"\"",1))return false;
 for(size_t i=0;i<n;i++){
  unsigned char c=(unsigned char)s[i];
  if(c=='"'||c=='\\'){char e[2]={'\\',(char)c};if(!append(b,e,2))return false;}
  else if(c=='\n'){if(!append(b,"\\n",2))return false;}
  else if(c=='\r'){if(!append(b,"\\r",2))return false;}
  else if(c=='\t'){if(!append(b,"\\t",2))return false;}
  else if(c<0x20){char u[7];snprintf(u,sizeof u,"\\u%04x",c);if(!append(b,u,6))return false;}
  else if(!append(b,(const char*)&s[i],1))return false;
 }
 return append(b,"\"",1);
}
/* Rebuild objects recursively: JSONObject keeps the last duplicate key. */
static bool serialize(sqlite3 *db,const char *doc,buffer *b,unsigned depth){
 if(depth>128)return false;sqlite3_stmt *s=NULL;bool ok=false;bool array=doc[0]=='[';
 const char *sql=array?"SELECT key,type,value FROM json_each(?1) ORDER BY key":"SELECT key,type,value FROM json_each(?1) e WHERE id=(SELECT max(id) FROM json_each(?1) x WHERE x.key=e.key) ORDER BY id";
 if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return false;sqlite3_bind_text(s,1,doc,-1,SQLITE_TRANSIENT);
 if(!append(b,array?"[":"{",1))goto end;bool first=true;int rc;
 while((rc=sqlite3_step(s))==SQLITE_ROW){
  if(!first&&!append(b,",",1))goto end;first=false;
  if(!array){sqlite3_stmt *q=NULL;if(sqlite3_prepare_v2(db,"SELECT json_quote(?1)",-1,&q,NULL)!=SQLITE_OK)goto end;sqlite3_bind_value(q,1,sqlite3_column_value(s,0));bool good=sqlite3_step(q)==SQLITE_ROW&&append(b,(const char*)sqlite3_column_text(q,0),(size_t)sqlite3_column_bytes(q,0));sqlite3_finalize(q);if(!good||!append(b,":",1))goto end;}
  const char *type=(const char*)sqlite3_column_text(s,1);
  if(!strcmp(type,"object")||!strcmp(type,"array")){if(!serialize(db,(const char*)sqlite3_column_text(s,2),b,depth+1))goto end;}
  else if(!strcmp(type,"true")||!strcmp(type,"false")||!strcmp(type,"null")){if(!append(b,type,strlen(type)))goto end;}
  else {sqlite3_stmt *q=NULL;if(sqlite3_prepare_v2(db,"SELECT json_quote(?1)",-1,&q,NULL)!=SQLITE_OK)goto end;sqlite3_bind_value(q,1,sqlite3_column_value(s,2));bool good=sqlite3_step(q)==SQLITE_ROW&&append(b,(const char*)sqlite3_column_text(q,0),(size_t)sqlite3_column_bytes(q,0));sqlite3_finalize(q);if(!good)goto end;}
 }
 if(rc!=SQLITE_DONE)goto end;ok=append(b,array?"]":"}",1);
 end:sqlite3_finalize(s);return ok;
}
static bool append_quoted(sqlite3 *db,sqlite3_stmt *row,int col,buffer *b){
 sqlite3_stmt *q=NULL;if(sqlite3_prepare_v2(db,"SELECT json_quote(?1)",-1,&q,NULL)!=SQLITE_OK)return false;
 sqlite3_bind_value(q,1,sqlite3_column_value(row,col));
 bool good=sqlite3_step(q)==SQLITE_ROW&&append(b,(const char*)sqlite3_column_text(q,0),(size_t)sqlite3_column_bytes(q,0));
 sqlite3_finalize(q);return good;
}
/* Rebuild an object without every member named `key`.  json_each walks members
 * in document order, so non-target members keep their order and duplicate
 * non-target keys stay untouched; every duplicate of the target key is dropped. */
static bool delete_key(sqlite3 *db,const char *doc,const char *key,buffer *b){
 sqlite3_stmt *s=NULL;bool ok=false;
 if(sqlite3_prepare_v2(db,"SELECT key,type,value FROM json_each(?1) ORDER BY id",-1,&s,NULL)!=SQLITE_OK)return false;
 sqlite3_bind_text(s,1,doc,-1,SQLITE_TRANSIENT);
 if(!append(b,"{",1))goto end;bool first=true;int rc;
 while((rc=sqlite3_step(s))==SQLITE_ROW){
  const char *k=(const char*)sqlite3_column_text(s,0);
  if(k&&!strcmp(k,key))continue;
  if(!first&&!append(b,",",1))goto end;first=false;
  if(!append_quoted(db,s,0,b)||!append(b,":",1))goto end;
  const char *type=(const char*)sqlite3_column_text(s,1);
  if(!strcmp(type,"object")||!strcmp(type,"array")){if(!append(b,(const char*)sqlite3_column_text(s,2),(size_t)sqlite3_column_bytes(s,2)))goto end;}
  else if(!strcmp(type,"true")||!strcmp(type,"false")||!strcmp(type,"null")){if(!append(b,type,strlen(type)))goto end;}
  else if(!append_quoted(db,s,2,b))goto end;
 }
 if(rc!=SQLITE_DONE)goto end;ok=append(b,"}",1);
 end:sqlite3_finalize(s);return ok;
}
static sqlite3_stmt *query(sqlite3 *db,const char *sql,const char *doc,const char *key){sqlite3_stmt *s=NULL;if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return NULL;sqlite3_bind_text(s,1,doc,-1,SQLITE_TRANSIENT);if(key)sqlite3_bind_text(s,2,key,-1,SQLITE_TRANSIENT);return s;}
static int attach(struct dvm *v,dvm_ref ref,const char *doc){struct dvm_object *o=dvm__obj(v,ref);if(!o)return 0;char *p=strdup(doc);if(!p)return 0;if(o->data && o->data_destroy!=destroy_json){free(p);return 0;}if(o->data_destroy)o->data_destroy(o->data);o->data=p;o->data_destroy=destroy_json;return 1;}
bool json_runtime_call(struct dvm *v,const char *cl,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(strcmp(cl,"org/json/JSONObject"))return false;
 if(!strcmp(m,"toString")&&!strcmp(sig,"()Ljava/lang/String;")){
  struct dvm_object *o=dvm__obj(v,self);if(!o||o->data_destroy!=destroy_json)return false;
  sqlite3 *db=NULL;buffer b={0};bool good=sqlite3_open(":memory:",&db)==SQLITE_OK&&serialize(db,o->data,&b,0);
  if(good)out->l=dvm_new_string_n(v,b.p,b.n);free(b.p);sqlite3_close(db);
  if(!good)dvm__throw(v,"java/lang/UnsupportedOperationException","JSON serialization limit or failure");return true;
 }
 #define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))
 if(!(IS("<init>","()V")||IS("<init>","(Ljava/lang/String;)V")||IS("length","()I")||IS("getJSONObject","(Ljava/lang/String;)Lorg/json/JSONObject;")||IS("getString","(Ljava/lang/String;)Ljava/lang/String;")||IS("optString","(Ljava/lang/String;)Ljava/lang/String;")||IS("optString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")||IS("has","(Ljava/lang/String;)Z")||IS("isNull","(Ljava/lang/String;)Z")||IS("put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;")||IS("put","(Ljava/lang/String;I)Lorg/json/JSONObject;")||IS("put","(Ljava/lang/String;J)Lorg/json/JSONObject;")||IS("put","(Ljava/lang/String;D)Lorg/json/JSONObject;")||IS("put","(Ljava/lang/String;Z)Lorg/json/JSONObject;")))return false;
 struct dvm_object *o=dvm__obj(v,self);sqlite3 *db=NULL;sqlite3_stmt *s=NULL;bool ok=false,rejected_sentinel=false;
 if(!o||!o->cls||strcmp(o->cls->name,"org/json/JSONObject"))return false;
 if(sqlite3_open(":memory:",&db)!=SQLITE_OK){sqlite3_close(db);fail(v);return true;}
 if(!strcmp(m,"<init>")&&(!strcmp(sig,"()V")||!strcmp(sig,"(Ljava/lang/String;)V"))){const char *doc=n?dvm_string_utf8(v,a[0].l):"{}";
 if(doc&&(!n||strlen(doc)==dvm_string_utf8_length(v,a[0].l))){s=query(db,"SELECT CASE WHEN json_valid(?1) THEN json_type(?1)='object' ELSE 0 END",doc,NULL);if(s&&sqlite3_step(s)==SQLITE_ROW&&sqlite3_column_int(s,0))ok=attach(v,self,doc);}goto done;}
 const char *doc=o&&o->data_destroy==destroy_json?o->data:NULL;if(!doc)goto done;
 /* Java 的原始类型 put 重载语义就是把参数装箱后调用 put(String,Object)；
  * 这里用 VM 自己的 valueOf 装箱再走同一条 JSON 合并路径，避免第二份实现。
  * 游戏 Java 桥 (UnityAgent.SendMessageToUnity) 使用 put(String,int)。 */
 if(IS("put","(Ljava/lang/String;I)Lorg/json/JSONObject;")||
    IS("put","(Ljava/lang/String;J)Lorg/json/JSONObject;")||
    IS("put","(Ljava/lang/String;D)Lorg/json/JSONObject;")||
    IS("put","(Ljava/lang/String;Z)Lorg/json/JSONObject;")){
  if(n!=2)goto done;
  const char *wrapcls,*wrapsig;
  if(IS("put","(Ljava/lang/String;I)Lorg/json/JSONObject;")){wrapcls="java/lang/Integer";wrapsig="(I)Ljava/lang/Integer;";}
  else if(IS("put","(Ljava/lang/String;J)Lorg/json/JSONObject;")){wrapcls="java/lang/Long";wrapsig="(J)Ljava/lang/Long;";}
  else if(IS("put","(Ljava/lang/String;D)Lorg/json/JSONObject;")){wrapcls="java/lang/Double";wrapsig="(D)Ljava/lang/Double;";}
  else{wrapcls="java/lang/Boolean";wrapsig="(Z)Ljava/lang/Boolean;";}
  union dvm_value boxed={0};
  if(!java_basics_call(v,wrapcls,"valueOf",wrapsig,0,&a[1],1,&boxed)||dvm_exception(v)||!boxed.l)goto done;
  union dvm_value two[2]={a[0],{.l=boxed.l}};
  if(json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",self,two,2,out)&&out->l==self&&!dvm_exception(v))ok=true;
  goto done;
 }
 if(IS("put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;")){
  if(n!=2)goto done;
  struct dvm_object *ko=dvm__obj(v,a[0].l);
  if(!ko||ko->kind!=DVM_OBJ_STRING)goto done;
  const char *key=dvm_string_utf8(v,a[0].l);size_t klen=dvm_string_utf8_length(v,a[0].l);
  if(!key||strlen(key)!=klen)goto done;
  buffer kb={0},vb={0};bool good=false;
  if(!append_json_string(&kb,key,klen))goto put_done;
  if(!a[1].l){
   /* Android contract: put(name, Java null) removes the mapping. */
   sqlite3_stmt *pk=NULL;bool present=false;
   if(sqlite3_prepare_v2(db,"SELECT 1 FROM json_each(?1) WHERE key=?2 LIMIT 1",-1,&pk,NULL)!=SQLITE_OK)goto put_done;
   sqlite3_bind_text(pk,1,doc,-1,SQLITE_TRANSIENT);sqlite3_bind_text(pk,2,key,-1,SQLITE_TRANSIENT);
   present=sqlite3_step(pk)==SQLITE_ROW;sqlite3_finalize(pk);
   if(!present)good=true; /* absent key: no-op, no error */
   else{buffer nb={0};good=delete_key(db,doc,key,&nb);if(good)good=nb.p&&attach(v,self,nb.p);free(nb.p);}
   goto put_done; /* deletion/no-op replaces the document; do not append */
  }
  else{
   struct dvm_object *vo=dvm__obj(v,a[1].l);
   if(!vo||!vo->cls)goto put_done;
   const char *cn=vo->cls->name?vo->cls->name:"";
   /* JSONObject.NULL is a framework sentinel this VM does not model as a
    * singleton.  Do not guess: reject it explicitly instead of storing JSON
    * null, and never confuse it with Java null (handled above). */
   if(!strcmp(cn,"org/json/JSONObject$NULL")){sdk_trace_fprintf(stderr,"JSON_RUNTIME_BOUNDARY put value=org/json/JSONObject$NULL null_sentinel=unsupported\n");rejected_sentinel=true;goto put_done;}
   if(vo->kind==DVM_OBJ_STRING){const char *sv=dvm_string_utf8(v,a[1].l);size_t sn=dvm_string_utf8_length(v,a[1].l);if(sv)good=append_json_string(&vb,sv,sn);}
   else if(!strcmp(cn,"java/lang/Boolean")){union dvm_value r={0};if(java_basics_call(v,cn,"booleanValue","()Z",a[1].l,NULL,0,&r)&&!dvm_exception(v))good=append(&vb,r.i?"true":"false",r.i?4:5);}
   else if(!strcmp(cn,"java/lang/Integer")){union dvm_value r={0};char num[32];if(java_basics_call(v,cn,"intValue","()I",a[1].l,NULL,0,&r)&&!dvm_exception(v)){int l=snprintf(num,sizeof num,"%d",r.i);good=l>0&&append(&vb,num,(size_t)l);}}
   else if(!strcmp(cn,"java/lang/Long")){union dvm_value r={0};char num[32];if(java_basics_call(v,cn,"longValue","()J",a[1].l,NULL,0,&r)&&!dvm_exception(v)){int l=snprintf(num,sizeof num,"%lld",(long long)r.j);good=l>0&&append(&vb,num,(size_t)l);}}
   else if(!strcmp(cn,"java/lang/Float")){union dvm_value r={0};char num[40];if(java_basics_call(v,cn,"floatValue","()F",a[1].l,NULL,0,&r)&&!dvm_exception(v)){int l=snprintf(num,sizeof num,"%.9g",(double)r.f);good=l>0&&append(&vb,num,(size_t)l);}}
   else if(!strcmp(cn,"java/lang/Double")){union dvm_value r={0};char num[48];if(java_basics_call(v,cn,"doubleValue","()D",a[1].l,NULL,0,&r)&&!dvm_exception(v)){int l=snprintf(num,sizeof num,"%.17g",r.d);good=l>0&&append(&vb,num,(size_t)l);}}
   else if(!strcmp(cn,"org/json/JSONObject")){if(vo->data_destroy==destroy_json&&vo->data){size_t dn=strlen(vo->data);good=dn<=1048576&&append(&vb,vo->data,dn);}}
   else goto put_done; /* unsupported value type: JSONException via done */
  }
  if(good){
   const char *open=strchr(doc,'{');const char *close=strrchr(doc,'}');
   if(open&&close&&close>open){
    size_t interior=(size_t)(close-open-1);bool blank=true;
    for(size_t i=0;i<interior;i++){char ch=open[1+i];if(ch!=' '&&ch!='\t'&&ch!='\r'&&ch!='\n'){blank=false;break;}}
    if(blank)interior=0;
    if(interior>1048576)good=false;
    else{
     size_t need=2+interior+(interior?1:0)+kb.n+1+vb.n;char *nd=malloc(need+1);
     if(!nd)good=false;
     else{
      size_t at=0;nd[at++]='{';if(interior){memcpy(nd+at,open+1,interior);at+=interior;nd[at++]=',';}memcpy(nd+at,kb.p,kb.n);at+=kb.n;nd[at++]=':';memcpy(nd+at,vb.p,vb.n);at+=vb.n;nd[at++]='}';nd[at]=0;
      sqlite3_stmt *chk=NULL;bool valid=false;
      if(sqlite3_prepare_v2(db,"SELECT CASE WHEN json_valid(?1) THEN json_type(?1)='object' ELSE 0 END",-1,&chk,NULL)==SQLITE_OK){sqlite3_bind_text(chk,1,nd,-1,SQLITE_TRANSIENT);valid=sqlite3_step(chk)==SQLITE_ROW&&sqlite3_column_int(chk,0);}
      sqlite3_finalize(chk);
      good=valid&&attach(v,self,nd); /* attach() copies nd; caller frees it below */
      free(nd);
     }
    }
   }else good=false;
  }
 put_done:free(kb.p);free(vb.p);if(good){out->l=self;ok=true;}goto done;
 }
 if(!strcmp(m,"length")&&!strcmp(sig,"()I")){s=query(db,"SELECT count(DISTINCT key) FROM json_each(?1)",doc,NULL);if(s&&sqlite3_step(s)==SQLITE_ROW){out->i=sqlite3_column_int(s,0);ok=true;}goto done;}
 if(n<1)goto done;const char *key=dvm_string_utf8(v,a[0].l);if(!key||strlen(key)!=dvm_string_utf8_length(v,a[0].l))goto done;
 s=query(db,"SELECT type,value FROM json_each(?1) WHERE key=?2 ORDER BY id DESC LIMIT 1",doc,key);if(!s)goto done;int rc=sqlite3_step(s);bool found=rc==SQLITE_ROW;const char *type=found?(const char *)sqlite3_column_text(s,0):NULL;
 if(!strcmp(m,"has")&&!strcmp(sig,"(Ljava/lang/String;)Z")){out->i=found;ok=true;goto done;}
 if(!strcmp(m,"isNull")&&!strcmp(sig,"(Ljava/lang/String;)Z")){out->i=!found||!strcmp(type,"null");ok=true;goto done;}
 if(!strcmp(m,"getJSONObject")&&!strcmp(sig,"(Ljava/lang/String;)Lorg/json/JSONObject;")&&found&&!strcmp(type,"object")){struct dvm_class *c=dvm_find_class_or_external(v,"org/json/JSONObject");out->l=c?dvm_new_object(v,c):0;ok=out->l&&attach(v,out->l,(const char *)sqlite3_column_text(s,1));goto done;}
 if((!strcmp(m,"getString")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;"))||(!strcmp(m,"optString")&&(!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;")||!strcmp(sig,"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")))){if(found){const char *value=(const char *)sqlite3_column_text(s,1);int len=sqlite3_column_bytes(s,1);if(!strcmp(type,"true")){value="true";len=4;}if(!strcmp(type,"false")){value="false";len=5;}if(!strcmp(type,"null")){value="null";len=4;}out->l=dvm_new_string_n(v,value,(size_t)len);ok=out->l!=0;}else if(!strcmp(m,"optString")){out->l=n>1?a[1].l:dvm_new_string(v,"");ok=true;}goto done;}
 done:sqlite3_finalize(s);sqlite3_close(db);if(!ok){if(rejected_sentinel)dvm__throw(v,"org/json/JSONException","JSONObject.NULL sentinel is not supported");else fail(v);}return true;
}
static bool json_exception_is(struct dvm *v,const char *name){dvm_ref e=dvm_exception(v);if(!e)return false;struct dvm_object *o=dvm__obj(v,e);return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,name);}
bool json_runtime_selftest(struct dvm *v){
 struct dvm_class *c=dvm_find_class_or_external(v,"org/json/JSONObject");
 dvm_ref obj=c?dvm_new_object(v,c):0; union dvm_value out={0}, a[2]={{0}};
 if(!obj)return false;
 a[0].l=dvm_new_string(v,"{\"x\":1,\"x\":2,\"nested\":{\"s\":\"a\\u0000b\"},\"nil\":null}");
 if(!json_runtime_call(v,"org/json/JSONObject","<init>","(Ljava/lang/String;)V",obj,a,1,&out)||dvm_exception(v))return false;
 if(!json_runtime_call(v,"org/json/JSONObject","length","()I",obj,0,0,&out)||out.i!=3)return false;
 if(!json_runtime_call(v,"org/json/JSONObject","toString","()Ljava/lang/String;",obj,NULL,0,&out)||!out.l)return false;
 const char *serialized=dvm_string_utf8(v,out.l);
 if(!strstr(serialized,"\"x\":2")||strstr(serialized,"\"x\":1")||!strstr(serialized,"\\u0000"))return false;
 a[0].l=dvm_new_string(v,"x");if(!json_runtime_call(v,"org/json/JSONObject","getString","(Ljava/lang/String;)Ljava/lang/String;",obj,a,1,&out)||strcmp(dvm_string_utf8(v,out.l),"2"))return false;
 a[0].l=dvm_new_string(v,"nested");if(!json_runtime_call(v,"org/json/JSONObject","getJSONObject","(Ljava/lang/String;)Lorg/json/JSONObject;",obj,a,1,&out)||!out.l)return false;
 obj=out.l;a[0].l=dvm_new_string(v,"s");if(!json_runtime_call(v,"org/json/JSONObject","getString","(Ljava/lang/String;)Ljava/lang/String;",obj,a,1,&out)||dvm_string_utf8_length(v,out.l)!=3||memcmp(dvm_string_utf8(v,out.l),"a\0b",3))return false;
 if(json_runtime_call(v,"org/json/JSONObject","unknown","()V",obj,0,0,&out))return false;
 a[0].l=dvm_new_string(v,"missing");if(!json_runtime_call(v,"org/json/JSONObject","getString","(Ljava/lang/String;)Ljava/lang/String;",obj,a,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 obj=dvm_new_object(v,c);a[0].l=dvm_new_string(v,"{");if(!json_runtime_call(v,"org/json/JSONObject","<init>","(Ljava/lang/String;)V",obj,a,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
#define PUT_ASSERT(stage,cond) do{if(!(cond)){sdk_trace_fprintf(stderr,"JSON_RUNTIME_SELFTEST FAIL stage=%s\n",(stage));return false;}}while(0)
 union dvm_value pa[2]={{0}};
 obj=dvm_new_object(v,c);pa[0].l=dvm_new_string(v,"{\"x\":1}");
 PUT_ASSERT("put_init",obj&&json_runtime_call(v,"org/json/JSONObject","<init>","(Ljava/lang/String;)V",obj,pa,1,&out)&&!dvm_exception(v));
 pa[0].l=dvm_new_string(v,"k");pa[1].l=dvm_new_string(v,"v");
 PUT_ASSERT("put_string_call",json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",obj,pa,2,&out)&&!dvm_exception(v)&&out.l==obj);
 pa[0].l=dvm_new_string(v,"k");
 PUT_ASSERT("put_string_get",json_runtime_call(v,"org/json/JSONObject","getString","(Ljava/lang/String;)Ljava/lang/String;",obj,pa,1,&out)&&!dvm_exception(v)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"v"));
 PUT_ASSERT("put_string_serialize",json_runtime_call(v,"org/json/JSONObject","toString","()Ljava/lang/String;",obj,NULL,0,&out)&&!dvm_exception(v)&&out.l&&(serialized=dvm_string_utf8(v,out.l))&&strstr(serialized,"\"k\":\"v\""));
 pa[0].l=dvm_new_string(v,"x");pa[1].l=dvm_new_string(v,"2");
 PUT_ASSERT("put_overwrite_call",json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",obj,pa,2,&out)&&!dvm_exception(v)&&out.l==obj);
 pa[0].l=dvm_new_string(v,"x");
 PUT_ASSERT("put_overwrite_get",json_runtime_call(v,"org/json/JSONObject","getString","(Ljava/lang/String;)Ljava/lang/String;",obj,pa,1,&out)&&!dvm_exception(v)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"2"));
 PUT_ASSERT("put_overwrite_serialize",json_runtime_call(v,"org/json/JSONObject","toString","()Ljava/lang/String;",obj,NULL,0,&out)&&!dvm_exception(v)&&out.l&&(serialized=dvm_string_utf8(v,out.l))&&strstr(serialized,"\"x\":\"2\"")&&!strstr(serialized,"\"x\":1"));
 /* Java null deletes the mapping; it must not create a JSON null value. */
 PUT_ASSERT("put_null_length_before",json_runtime_call(v,"org/json/JSONObject","length","()I",obj,0,0,&out)&&!dvm_exception(v)&&out.i==2);
 pa[0].l=dvm_new_string(v,"k");pa[1].l=0;
 PUT_ASSERT("put_null_delete_call",json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",obj,pa,2,&out)&&!dvm_exception(v)&&out.l==obj);
 pa[0].l=dvm_new_string(v,"k");
 PUT_ASSERT("put_null_has",json_runtime_call(v,"org/json/JSONObject","has","(Ljava/lang/String;)Z",obj,pa,1,&out)&&!dvm_exception(v)&&out.i==0);
 PUT_ASSERT("put_null_length_after",json_runtime_call(v,"org/json/JSONObject","length","()I",obj,0,0,&out)&&!dvm_exception(v)&&out.i==1);
 PUT_ASSERT("put_null_serialize_absent",json_runtime_call(v,"org/json/JSONObject","toString","()Ljava/lang/String;",obj,NULL,0,&out)&&!dvm_exception(v)&&out.l&&(serialized=dvm_string_utf8(v,out.l))&&!strstr(serialized,"\"k\""));
 PUT_ASSERT("put_null_get_throws",json_runtime_call(v,"org/json/JSONObject","getString","(Ljava/lang/String;)Ljava/lang/String;",obj,pa,1,&out)&&json_exception_is(v,"org/json/JSONException"));
 dvm_clear_exception(v);
 /* Deleting an absent key is a silent no-op and must not rewrite the document. */
 pa[0].l=dvm_new_string(v,"ghost");
 PUT_ASSERT("put_null_absent_before_tostring",json_runtime_call(v,"org/json/JSONObject","toString","()Ljava/lang/String;",obj,NULL,0,&out)&&!dvm_exception(v)&&out.l);
 {dvm_ref before_tostring=out.l;
  pa[0].l=dvm_new_string(v,"ghost");pa[1].l=0;
  PUT_ASSERT("put_null_absent_call",json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",obj,pa,2,&out)&&!dvm_exception(v)&&out.l==obj);
  pa[0].l=dvm_new_string(v,"ghost");
  PUT_ASSERT("put_null_absent_has",json_runtime_call(v,"org/json/JSONObject","has","(Ljava/lang/String;)Z",obj,pa,1,&out)&&!dvm_exception(v)&&out.i==0);
  PUT_ASSERT("put_null_absent_length",json_runtime_call(v,"org/json/JSONObject","length","()I",obj,0,0,&out)&&!dvm_exception(v)&&out.i==1);
  PUT_ASSERT("put_null_absent_tostring_unchanged",json_runtime_call(v,"org/json/JSONObject","toString","()Ljava/lang/String;",obj,NULL,0,&out)&&!dvm_exception(v)&&out.l&&!strcmp(dvm_string_utf8(v,before_tostring),dvm_string_utf8(v,out.l)));}
 /* JSONObject.NULL sentinel: this VM has no reliable singleton, so put() must
  * reject it explicitly rather than storing JSON null. */
 {struct dvm_class *nc=dvm_find_class_or_external(v,"org/json/JSONObject$NULL");dvm_ref sentinel=nc?dvm_new_object(v,nc):0;
  PUT_ASSERT("null_sentinel_setup",sentinel!=0);
  pa[0].l=dvm_new_string(v,"n");pa[1].l=sentinel;
  PUT_ASSERT("null_sentinel_rejected",json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",obj,pa,2,&out)&&json_exception_is(v,"org/json/JSONException"));
  dvm_clear_exception(v);
  pa[0].l=dvm_new_string(v,"n");
  PUT_ASSERT("null_sentinel_absent_has",json_runtime_call(v,"org/json/JSONObject","has","(Ljava/lang/String;)Z",obj,pa,1,&out)&&!dvm_exception(v)&&out.i==0);
  PUT_ASSERT("null_sentinel_length",json_runtime_call(v,"org/json/JSONObject","length","()I",obj,0,0,&out)&&!dvm_exception(v)&&out.i==1);
  PUT_ASSERT("null_sentinel_no_fake_value",json_runtime_call(v,"org/json/JSONObject","toString","()Ljava/lang/String;",obj,NULL,0,&out)&&!dvm_exception(v)&&out.l&&(serialized=dvm_string_utf8(v,out.l))&&!strstr(serialized,"\"n\""));}
 /* Re-insert after deletion still works, and overwrite keeps the last value. */
 pa[0].l=dvm_new_string(v,"k");pa[1].l=dvm_new_string(v,"v");
 PUT_ASSERT("put_reinsert_after_delete",json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",obj,pa,2,&out)&&!dvm_exception(v)&&out.l==obj);
 dvm_ref nested=dvm_new_object(v,c);pa[0].l=dvm_new_string(v,"{\"a\":1}");
 PUT_ASSERT("put_object_init",nested&&json_runtime_call(v,"org/json/JSONObject","<init>","(Ljava/lang/String;)V",nested,pa,1,&out)&&!dvm_exception(v));
 pa[0].l=dvm_new_string(v,"o");pa[1].l=nested;
 PUT_ASSERT("put_object_call",json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",obj,pa,2,&out)&&!dvm_exception(v)&&out.l==obj);
 pa[0].l=dvm_new_string(v,"o");
 PUT_ASSERT("put_object_get",json_runtime_call(v,"org/json/JSONObject","getJSONObject","(Ljava/lang/String;)Lorg/json/JSONObject;",obj,pa,1,&out)&&!dvm_exception(v)&&out.l);
 PUT_ASSERT("put_object_serialize",json_runtime_call(v,"org/json/JSONObject","toString","()Ljava/lang/String;",obj,NULL,0,&out)&&!dvm_exception(v)&&out.l&&(serialized=dvm_string_utf8(v,out.l))&&strstr(serialized,"\"o\":{\"a\":1}"));
 dvm_ref arr=dvm_new_array(v,'I',"I",1);
 PUT_ASSERT("put_unsupported_setup",arr!=0);
 pa[0].l=dvm_new_string(v,"bad");pa[1].l=arr;
 PUT_ASSERT("put_unsupported_call",json_runtime_call(v,"org/json/JSONObject","put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;",obj,pa,2,&out)&&json_exception_is(v,"org/json/JSONException"));
 dvm_clear_exception(v);
 pa[0].l=dvm_new_string(v,"k");
 PUT_ASSERT("put_unsupported_atomic",json_runtime_call(v,"org/json/JSONObject","getString","(Ljava/lang/String;)Ljava/lang/String;",obj,pa,1,&out)&&!dvm_exception(v)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"v"));
#undef PUT_ASSERT
 sdk_trace_fprintf(stderr,"JSON_RUNTIME_SELFTEST PASS nested=1 escaped_NUL=1 duplicate_last=1 invalid_rejected=1 strict_unknown=1 serialization=1 put=1 put_null_delete=1 null_sentinel=unsupported\n");
 return !dvm_exception(v);
}
