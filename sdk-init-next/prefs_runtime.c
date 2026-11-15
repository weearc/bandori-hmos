#include "prefs_runtime.h"
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "collections.h"
#include "set_runtime.h"
extern int sdk_trace_fprintf(FILE*,const char*,...);
extern bool java_basics_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
extern dvm_ref java_basics_boolean_box(struct dvm *v,int value);
/* Engine-stage diagnostic channel: sdk_trace_fprintf stops reaching the
 * progress file once the SDK runner returns, so append directly instead. */
static void prefs_probe_log(FILE *ignored,const char *fmt,...){
 if(strstr(fmt,"PREFPERSIST")){FILE *pf=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/prefpersist-runtime.txt","a");if(pf){va_list pa;va_start(pa,fmt);vfprintf(pf,fmt,pa);va_end(pa);fputc('\n',pf);fclose(pf);}}
 (void)ignored;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 va_list a;va_start(a,fmt);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fclose(f);
}
#define fprintf prefs_probe_log
typedef struct {sqlite3 *db;char *name;} pref;
typedef struct op {char *key,*value;int length,remove,kind;struct op *next;} op;
typedef struct {sqlite3 *db;char *name;int clear;op *head,*tail;} edit;
static char dbpath[PATH_MAX];static struct dvm *active;
static bool persistence_probe(struct dvm *);
static void clear_ops(edit *e){op *q=e->head;while(q){op *n=q->next;free(q->key);free(q->value);free(q);q=n;}e->head=e->tail=NULL;}
static void pref_free(void *x){pref *p=x;if(p){if(p->db)sqlite3_close(p->db);free(p->name);free(p);}}
static void edit_free(void *x){edit *e=x;if(e){clear_ops(e);if(e->db)sqlite3_close(e->db);free(e->name);free(e);}}
static bool error(struct dvm *v,const char *c){dvm__throw(v,c,"SharedPreferences");return true;}
static bool is(const char *m,const char *s,const char *x,const char *y){return !strcmp(m,x)&&!strcmp(s,y);}
static bool open_db(sqlite3 **d){*d=NULL;if(sqlite3_open(dbpath,d)!=SQLITE_OK)goto fail;sqlite3_busy_timeout(*d,1000);if(sqlite3_exec(*d,"PRAGMA synchronous=FULL",NULL,NULL,NULL)!=SQLITE_OK)goto fail;if(sqlite3_exec(*d,"CREATE TABLE IF NOT EXISTS prefs(name TEXT NOT NULL,key TEXT NOT NULL,value TEXT,PRIMARY KEY(name,key))",NULL,NULL,NULL)!=SQLITE_OK)goto fail;sqlite3_stmt *t=NULL;bool has=false;if(sqlite3_prepare_v2(*d,"PRAGMA table_info(prefs)",-1,&t,NULL)!=SQLITE_OK)goto fail;int rc;while((rc=sqlite3_step(t))==SQLITE_ROW)if(!strcmp((const char*)sqlite3_column_text(t,1),"kind"))has=true;if(sqlite3_finalize(t)!=SQLITE_OK||rc!=SQLITE_DONE)goto fail;if(!has&&sqlite3_exec(*d,"ALTER TABLE prefs ADD COLUMN kind TEXT NOT NULL DEFAULT 'string'",NULL,NULL,NULL)!=SQLITE_OK)goto fail;return true;fail:fprintf(stderr,"PREFPERSIST open_failed pid=%d path=%s rc=%d error=%s",getpid(),dbpath,*d?sqlite3_errcode(*d):-1,*d?sqlite3_errmsg(*d):"no_handle");if(*d)sqlite3_close(*d);*d=NULL;return false;}
static bool bind(sqlite3_stmt *t,int i,const char *p,int n){return sqlite3_bind_text(t,i,p,n,SQLITE_TRANSIENT)==SQLITE_OK;}
/* StringSet rows are hex(4-byte big-endian length + UTF-8 bytes per element).
 * Hex keeps SQLite TEXT free of embedded NULs and is deterministic. */
static char *set_encode(struct dvm *v,dvm_ref source){
 union dvm_value it={0},more={0},item={0};size_t cap=64,used=0;
 if(!set_runtime_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",source,NULL,0,&it)||!it.l){dvm__throw(v,"java/lang/IllegalArgumentException","StringSet source");return NULL;}
 unsigned char *raw=malloc(cap);if(!raw){dvm__throw(v,"java/lang/OutOfMemoryError","StringSet");return NULL;}
 for(;;){
  if(!set_runtime_call(v,"java/util/Set","hasNext","()Z",it.l,NULL,0,&more)||dvm_exception(v))goto fail;
  if(!more.i)break;
  if(!set_runtime_call(v,"java/util/Set","next","()Ljava/lang/Object;",it.l,NULL,0,&item)||!item.l||dvm_exception(v))goto fail;
  struct dvm_object *o=dvm__obj(v,item.l);
  if(!o||o->kind!=DVM_OBJ_STRING){dvm__throw(v,"java/lang/ClassCastException","StringSet element");goto fail;}
  size_t n=o->utf8_len;
  if(used+4+n>cap){size_t next=cap;while(next<used+4+n)next*=2;unsigned char *p=realloc(raw,next);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","StringSet");goto fail;}raw=p;cap=next;}
  raw[used++]=(unsigned char)(n>>24);raw[used++]=(unsigned char)(n>>16);raw[used++]=(unsigned char)(n>>8);raw[used++]=(unsigned char)n;
  memcpy(raw+used,o->utf8,n);used+=n;item.l=0;
 }
 {static const char digits[]="0123456789abcdef";char *hex=malloc(used*2+1);if(!hex){dvm__throw(v,"java/lang/OutOfMemoryError","StringSet");free(raw);return NULL;}for(size_t i=0;i<used;i++){hex[2*i]=digits[raw[i]>>4];hex[2*i+1]=digits[raw[i]&15];}hex[used*2]=0;free(raw);return hex;}
fail:
 free(raw);return NULL;
}
static int hex_value(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static dvm_ref set_decode(struct dvm *v,const char *hex,int len){
 if(len<0||len%2){dvm__throw(v,"java/lang/IllegalStateException","corrupt StringSet length");return 0;}
 struct dvm_class *hs=dvm_find_class_or_external(v,"java/util/HashSet");
 dvm_ref set=dvm_new_object(v,hs);union dvm_value ignored={0},item={0},out={0};
 if(!set||!set_runtime_call(v,"java/util/HashSet","<init>","()V",set,NULL,0,&ignored))return 0;
 for(int pos=0;pos<len;){
  if(pos+8>len){dvm__throw(v,"java/lang/IllegalStateException","corrupt StringSet header");return 0;}
  uint32_t n=0;for(int i=0;i<4;i++){int hi=hex_value(hex[pos++]),lo=hex_value(hex[pos++]);if(hi<0||lo<0){dvm__throw(v,"java/lang/IllegalStateException","corrupt StringSet hex");return 0;}n=(n<<8)|(uint32_t)((hi<<4)|lo);}
  if((uint64_t)n*2>(uint64_t)(len-pos)){dvm__throw(v,"java/lang/IllegalStateException","corrupt StringSet element");return 0;}
  char *bytes=malloc((size_t)n+1);if(!bytes){dvm__throw(v,"java/lang/OutOfMemoryError","StringSet element");return 0;}
  for(uint32_t i=0;i<n;i++){int hi=hex_value(hex[pos++]),lo=hex_value(hex[pos++]);if(hi<0||lo<0){free(bytes);dvm__throw(v,"java/lang/IllegalStateException","corrupt StringSet hex");return 0;}bytes[i]=(char)((hi<<4)|lo);}
  item.l=dvm_new_string_n(v,bytes,n);free(bytes);
  if(!item.l||!set_runtime_call(v,"java/util/HashSet","add","(Ljava/lang/Object;)Z",set,&item,1,&out)||dvm_exception(v))return 0;
 }
 return set;
}
static bool run(sqlite3 *d,const char *sql,const char *name,const char *key,const char *val,int len){sqlite3_stmt *t=NULL;bool ok=false;if(sqlite3_prepare_v2(d,sql,-1,&t,NULL)!=SQLITE_OK)goto end;if(!bind(t,1,name,-1)||(key&&!bind(t,2,key,-1))||(val&&!bind(t,3,val,len)))goto end;ok=sqlite3_step(t)==SQLITE_DONE;end:if(t&&sqlite3_finalize(t)!=SQLITE_OK)ok=false;return ok;}
static bool flush(edit *e){sqlite3 *d=e->db;for(op *q=e->head;q;q=q->next)if(!strcmp(q->key,"initial_agreement_tag"))fprintf(stderr,"PREFPERSIST agreement_flush pid=%d namespace=%s remove=%d kind=%d",getpid(),e->name,q->remove,q->kind);if(sqlite3_exec(d,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){fprintf(stderr,"PREFPERSIST begin_failed pid=%d rc=%d error=%s",getpid(),sqlite3_errcode(d),sqlite3_errmsg(d));return false;}bool ok=!e->clear||run(d,"DELETE FROM prefs WHERE name=?",e->name,NULL,NULL,0);for(op *q=e->head;ok&&q;q=q->next){if(q->remove)ok=run(d,"DELETE FROM prefs WHERE name=? AND key=?",e->name,q->key,NULL,0);else{sqlite3_stmt *t=NULL;ok=sqlite3_prepare_v2(d,"INSERT OR REPLACE INTO prefs(name,key,value,kind) VALUES(?,?,?,?)",-1,&t,NULL)==SQLITE_OK&&bind(t,1,e->name,-1)&&bind(t,2,q->key,-1)&&bind(t,3,q->value,q->length)&&bind(t,4,q->kind==1?"boolean":q->kind==2?"string_set":"string",-1)&&sqlite3_step(t)==SQLITE_DONE;if(t&&sqlite3_finalize(t)!=SQLITE_OK)ok=false;}}if(ok)ok=sqlite3_exec(d,"COMMIT",NULL,NULL,NULL)==SQLITE_OK;if(ok){clear_ops(e);e->clear=0;}else {fprintf(stderr,"PREFPERSIST flush_failed pid=%d rc=%d error=%s",getpid(),sqlite3_errcode(d),sqlite3_errmsg(d));sqlite3_exec(d,"ROLLBACK",NULL,NULL,NULL);}return ok;}
static bool raw(struct dvm *v,dvm_ref r,const char **p,int *n){*p=dvm_string_utf8(v,r);if(!*p)return false;uint32_t len=dvm_string_utf8_length(v,r);if(len>INT_MAX||memchr(*p,0,len))return false;*n=(int)len;return true;}
static bool context(struct dvm *v,const char *c,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)if(!strcmp(k->name,"android/content/Context"))return true;return !strcmp(c,"android/content/Context")||!strcmp(c,"android/app/Application")||!strcmp(c,"android/app/Activity");}
bool prefs_runtime_configure(const char *sdk){fprintf(stderr,"PREFPERSIST configure pid=%d sdk=%s",getpid(),sdk?sdk:"null");char p[PATH_MAX];if(!sdk||!realpath(sdk,p))return false;char *z=strrchr(p,'/');if(!z)return false;*z=0;if(access(p,W_OK)!=0){fprintf(stderr,"PREFPERSIST configure_failed pid=%d parent=%s errno=%d",getpid(),p,errno);return false;}return snprintf(dbpath,sizeof dbpath,"%s/privateprefs.sqlite",p)<(int)sizeof dbpath;}
void prefs_runtime_vm_start(struct dvm *v){active=v;persistence_probe(v);struct stat st={0};int rc=stat(dbpath,&st);fprintf(stderr,"PREFPERSIST start pid=%d dbpath=%s stat=%d bytes=%lld mtime=%lld writable=%d",getpid(),dbpath,rc,(long long)st.st_size,(long long)st.st_mtime,access(dbpath,W_OK)==0);}
static pref *getpref(struct dvm *v,dvm_ref r){struct dvm_object *o=dvm__obj(v,r);return o&&o->data_destroy==pref_free?o->data:NULL;}
static edit *getedit(struct dvm *v,dvm_ref r){struct dvm_object *o=dvm__obj(v,r);return o&&o->data_destroy==edit_free?o->data:NULL;}
bool prefs_runtime_call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){if(v!=active||!dbpath[0])return false;
 if(n==2&&context(v,c,self)&&is(m,s,"getSharedPreferences","(Ljava/lang/String;I)Landroid/content/SharedPreferences;")){const char *name;int len;if(!a[0].l)return error(v,"java/lang/NullPointerException");if(!raw(v,a[0].l,&name,&len)||a[1].i!=0)return error(v,"java/lang/IllegalArgumentException");fprintf(stderr,"PREFS_HOST_CALL getSharedPreferences name=%s mode=%d\n",name,a[1].i);pref *p=calloc(1,sizeof *p);if(!p)return error(v,"java/lang/OutOfMemoryError");p->name=strdup(name);if(!p->name||!open_db(&p->db)){pref_free(p);return error(v,"java/lang/IllegalStateException");}out->l=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/SharedPreferences"));struct dvm_object *o=dvm__obj(v,out->l);if(!o){pref_free(p);return error(v,"java/lang/OutOfMemoryError");}o->data=p;o->data_destroy=pref_free;return true;}
 pref *p=getpref(v,self);edit *e=getedit(v,self);
 if(p&&is(m,s,"edit","()Landroid/content/SharedPreferences$Editor;")){e=calloc(1,sizeof *e);if(!e)return error(v,"java/lang/OutOfMemoryError");e->name=strdup(p->name);if(!e->name||!open_db(&e->db)){edit_free(e);return error(v,"java/lang/IllegalStateException");}out->l=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/SharedPreferences$Editor"));struct dvm_object *o=dvm__obj(v,out->l);if(!o){edit_free(e);return error(v,"java/lang/OutOfMemoryError");}o->data=e;o->data_destroy=edit_free;return true;}
 if(p&&(is(m,s,"contains","(Ljava/lang/String;)Z")||is(m,s,"getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")||is(m,s,"getBoolean","(Ljava/lang/String;Z)Z")||is(m,s,"getStringSet","(Ljava/lang/String;Ljava/util/Set;)Ljava/util/Set;"))){bool has=!strcmp(m,"contains"),boolean=!strcmp(m,"getBoolean"),stringset=!strcmp(m,"getStringSet");const char *key;int len;if(!a[0].l)return error(v,"java/lang/NullPointerException");if(!raw(v,a[0].l,&key,&len))return error(v,"java/lang/IllegalArgumentException");if(!strcmp(m,"getString"))fprintf(stderr,"PREFS_HOST_CALL getString key=%s default_null=%d\n",key,a[1].l?0:1);sqlite3_stmt *t=NULL;int rc=sqlite3_prepare_v2(p->db,has?"SELECT 1 FROM prefs WHERE name=? AND key=?":"SELECT value,kind FROM prefs WHERE name=? AND key=?",-1,&t,NULL);bool badtype=false;if(rc==SQLITE_OK&&bind(t,1,p->name,-1)&&bind(t,2,key,len)){rc=sqlite3_step(t);if(rc==SQLITE_ROW){if(has)out->i=1;else{const char *kind=(const char*)sqlite3_column_text(t,1);if(!kind||strcmp(kind,stringset?"string_set":boolean?"boolean":"string"))badtype=true;else{const char *val=(const char*)sqlite3_column_text(t,0);int size=sqlite3_column_bytes(t,0);if(stringset){out->l=set_decode(v,val,size);}else if(boolean){if(!val||size!=1||(val[0]!='0'&&val[0]!='1'))badtype=true;else out->i=val[0]=='1';}else out->l=val?dvm_new_string_n(v,val,size):0;}}}else if(rc==SQLITE_DONE){if(has)out->i=0;else if(stringset)out->l=a[1].l;else if(boolean)out->i=a[1].i;else out->l=a[1].l;}}else rc=SQLITE_ERROR;if(t&&sqlite3_finalize(t)!=SQLITE_OK)rc=SQLITE_ERROR;if(dvm_exception(v))return true;if(badtype)return error(v,"java/lang/ClassCastException");if(!strcmp(key,"initial_agreement_tag"))fprintf(stderr,"PREFPERSIST agreement_read pid=%d namespace=%s method=%s rc=%d present=%d",getpid(),p->name,m,rc,rc==SQLITE_ROW);if(!strcmp(m,"getString"))fprintf(stderr,"PREFS_HOST_RESULT getString key=%s rc=%d out_null=%d\n",key,rc,out->l?0:1);return rc!=SQLITE_ROW&&rc!=SQLITE_DONE?error(v,"java/lang/IllegalStateException"):true;}
 if(p&&is(m,s,"getInt","(Ljava/lang/String;I)I")){
  if(!a[0].l)return error(v,"java/lang/NullPointerException");
  const char *key;int len;
  if(!raw(v,a[0].l,&key,&len))return error(v,"java/lang/IllegalArgumentException");
  sqlite3_stmt *t=NULL;int rc=sqlite3_prepare_v2(p->db,"SELECT value,kind FROM prefs WHERE name=? AND key=?",-1,&t,NULL);
  int32_t value=a[1].i;bool found=false,badtype=false;
  if(rc==SQLITE_OK&&bind(t,1,p->name,-1)&&bind(t,2,key,len)){
   rc=sqlite3_step(t);
   if(rc==SQLITE_ROW){
    found=true;
    const char *kind=(const char*)sqlite3_column_text(t,1);
    const char *val=(const char*)sqlite3_column_text(t,0);
    int size=sqlite3_column_bytes(t,0);
    if(!kind||strcmp(kind,"int")||!val||size<1||size>31)badtype=true;
    else{char buf[32];memcpy(buf,val,(size_t)size);buf[size]=0;char *end=NULL;long parsed=strtol(buf,&end,10);
     if(!end||*end||parsed<INT32_MIN||parsed>INT32_MAX)badtype=true;else value=(int32_t)parsed;}
   }else if(rc!=SQLITE_DONE)rc=SQLITE_ERROR;
  }else rc=SQLITE_ERROR;
  if(t&&sqlite3_finalize(t)!=SQLITE_OK)rc=SQLITE_ERROR;
  if(dvm_exception(v))return true;
  if(badtype)return error(v,"java/lang/ClassCastException");
  if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)return error(v,"java/lang/IllegalStateException");
  prefs_probe_log(stderr,"PREFS_HOST_CALL getInt key=%s found=%d value=%d",key,found,(int)value);
  out->i=value;return true;
 }
 if(p&&is(m,s,"getAll","()Ljava/util/Map;")){dvm_ref map=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashMap"));union dvm_value ignored={0};if(!map||!collections_call(v,"java/util/HashMap","<init>","()V",map,NULL,0,&ignored)||dvm_exception(v))return error(v,"java/lang/IllegalStateException");sqlite3_stmt *t=NULL;int rc=sqlite3_prepare_v2(p->db,"SELECT key,value,kind FROM prefs WHERE name=?",-1,&t,NULL);if(rc==SQLITE_OK&&bind(t,1,p->name,-1)){while((rc=sqlite3_step(t))==SQLITE_ROW){const char *key=(const char*)sqlite3_column_text(t,0),*val=(const char*)sqlite3_column_text(t,1),*kind=(const char*)sqlite3_column_text(t,2);int kn=sqlite3_column_bytes(t,0),vn=sqlite3_column_bytes(t,1);if(!key||!kind){rc=SQLITE_ERROR;break;}dvm_ref k=dvm_new_string_n(v,key,kn),value=0;if(!strcmp(kind,"string")){if(val)value=dvm_new_string_n(v,val,vn);}else if(!strcmp(kind,"boolean")){if(!val||vn!=1||(val[0]!='0'&&val[0]!='1')){rc=SQLITE_MISMATCH;break;}value=java_basics_boolean_box(v,val[0]=='1');}else if(!strcmp(kind,"string_set")){value=set_decode(v,val,vn);if(!value){rc=SQLITE_ERROR;break;}}else{rc=SQLITE_MISMATCH;break;}union dvm_value args[2]={{.l=k},{.l=value}};if(!k||(!value&&val)||!collections_call(v,"java/util/HashMap","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,args,2,&ignored)||dvm_exception(v)){rc=SQLITE_ERROR;break;}}}else rc=SQLITE_ERROR;if(t&&sqlite3_finalize(t)!=SQLITE_OK)rc=SQLITE_ERROR;if(dvm_exception(v))return true;if(rc==SQLITE_MISMATCH)return error(v,"java/lang/ClassCastException");if(rc!=SQLITE_DONE)return error(v,"java/lang/IllegalStateException");out->l=map;return true;}
 if(e&&is(m,s,"clear","()Landroid/content/SharedPreferences$Editor;")){e->clear=1;out->l=self;return true;}
 if(e&&(is(m,s,"putString","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;")||is(m,s,"putBoolean","(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;")||is(m,s,"putStringSet","(Ljava/lang/String;Ljava/util/Set;)Landroid/content/SharedPreferences$Editor;")||is(m,s,"remove","(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;"))){const char *key;int len;bool boolean=!strcmp(m,"putBoolean"),stringset=!strcmp(m,"putStringSet"),remove=!strcmp(m,"remove")||(!boolean&&!a[1].l);if(!a[0].l)return error(v,"java/lang/NullPointerException");if(!raw(v,a[0].l,&key,&len))return error(v,"java/lang/IllegalArgumentException");op *q=calloc(1,sizeof *q);if(!q)return error(v,"java/lang/OutOfMemoryError");q->key=strdup(key);q->remove=remove;q->kind=stringset?2:(boolean?1:0);if(!remove){if(boolean){q->value=strdup(a[1].i?"1":"0");q->length=1;}else if(stringset){q->value=set_encode(v,a[1].l);if(!q->value){free(q->key);free(q);return dvm_exception(v)?true:error(v,"java/lang/IllegalArgumentException");}q->length=(int)strlen(q->value);}else{const char *val=dvm_string_utf8(v,a[1].l);uint32_t size=dvm_string_utf8_length(v,a[1].l);if(!val||size>INT_MAX){free(q->key);free(q);return error(v,"java/lang/IllegalArgumentException");}q->value=malloc((size_t)size+1);if(q->value){memcpy(q->value,val,size);q->value[size]=0;q->length=(int)size;}}}if(!q->key||(!remove&&!q->value)){free(q->key);free(q->value);free(q);return error(v,"java/lang/OutOfMemoryError");}if(e->tail)e->tail->next=q;else e->head=q;e->tail=q;out->l=self;return true;}
 if(e&&(is(m,s,"commit","()Z")||is(m,s,"apply","()V"))){bool ok=flush(e);if(!strcmp(m,"apply")){if(!ok)return error(v,"java/lang/IllegalStateException");return true;}out->i=ok;return true;}return false;}

static bool persistence_probe(struct dvm *v){
 static int done; if(done++)return true;
 extern bool prefpersist_builder_selftest(struct dvm *);
 bool builder_ok=prefpersist_builder_selftest(v);
 fprintf(stderr,"PREFPERSIST builder_setCharAt_selftest pid=%d pass=%d ascii_map=1 unicode=1 nul=1 bounds=1",getpid(),builder_ok);
 if(!builder_ok)return false;
 union dvm_value a[2]={{0}},o={0};
 dvm_ref ctx=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/Context"));
 a[0].l=dvm_new_string(v,"__prefpersist_cross_launch_v1__");
 if(!prefs_runtime_call(v,"android/content/Context","getSharedPreferences","(Ljava/lang/String;I)Landroid/content/SharedPreferences;",ctx,a,2,&o)||!o.l)return false;
 dvm_ref pref=o.l;a[0].l=dvm_new_string(v,"probe_boolean");a[1].i=0;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getBoolean","(Ljava/lang/String;Z)Z",pref,a,2,&o)||dvm_exception(v))return false;
 int previous=o.i;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","edit","()Landroid/content/SharedPreferences$Editor;",pref,NULL,0,&o)||!o.l)return false;
 dvm_ref editor=o.l;a[1].i=1;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","putBoolean","(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;",editor,a,2,&o)||!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","commit","()Z",editor,NULL,0,&o)||o.i!=1)return false;
 struct stat st={0};stat(dbpath,&st);
 fprintf(stderr,"PREFPERSIST probe pid=%d previous=%d commit=1 bytes=%lld mtime=%lld path=%s",getpid(),previous,(long long)st.st_size,(long long)st.st_mtime,dbpath);
 return true;
}

static bool typed_selftest(struct dvm *,dvm_ref,sqlite3 *,const char *);
bool prefs_runtime_selftest(struct dvm *v){if(v!=active||!dbpath[0])return false;if(!persistence_probe(v))return false;const char *name="__prefs_runtime_selftest_v1__";sqlite3 *d=NULL;bool ok=false;union dvm_value a[2]={{0}},out={0};dvm_ref context_ref=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/Context"));if(!context_ref||!open_db(&d))return false;if(!run(d,"DELETE FROM prefs WHERE name=?",name,NULL,NULL,0))goto end;a[0].l=dvm_new_string(v,name);a[1].i=0;if(!prefs_runtime_call(v,"android/content/Context","getSharedPreferences","(Ljava/lang/String;I)Landroid/content/SharedPreferences;",context_ref,a,2,&out)||!out.l)goto end;dvm_ref first=out.l;if(!prefs_runtime_call(v,"android/content/SharedPreferences","edit","()Landroid/content/SharedPreferences$Editor;",first,NULL,0,&out)||!out.l)goto end;dvm_ref editor=out.l;struct dvm_object *first_obj=dvm__obj(v,first);if(!first_obj||first_obj->data_destroy!=pref_free)goto end;pref_free(first_obj->data);first_obj->data=NULL;first_obj->data_destroy=NULL;a[0].l=dvm_new_string(v,"k");a[1].l=dvm_new_string(v,"first");if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","putString","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;",editor,a,2,&out))goto end;a[1].l=dvm_new_string(v,"second");if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","putString","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;",editor,a,2,&out)||!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","commit","()Z",editor,NULL,0,&out)||out.i!=1)goto end;a[0].l=dvm_new_string(v,name);a[1].i=0;if(!prefs_runtime_call(v,"android/content/Context","getSharedPreferences","(Ljava/lang/String;I)Landroid/content/SharedPreferences;",context_ref,a,2,&out)||!out.l)goto end;dvm_ref reopened=out.l;a[0].l=dvm_new_string(v,"k");a[1].l=0;if(!prefs_runtime_call(v,"android/content/SharedPreferences","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",reopened,a,2,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"second"))goto end;if(!prefs_runtime_call(v,"android/content/SharedPreferences","edit","()Landroid/content/SharedPreferences$Editor;",reopened,NULL,0,&out)||!out.l)goto end;editor=out.l;if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","remove","(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;",editor,a,1,&out)||!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","commit","()Z",editor,NULL,0,&out)||out.i!=1)goto end;if(!prefs_runtime_call(v,"android/content/SharedPreferences","contains","(Ljava/lang/String;)Z",reopened,a,1,&out)||out.i!=0)goto end;if(!prefs_runtime_call(v,"android/content/SharedPreferences","edit","()Landroid/content/SharedPreferences$Editor;",reopened,NULL,0,&out)||!out.l)goto end;editor=out.l;a[1].l=dvm_new_string(v,"x");if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","putString","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;",editor,a,2,&out)||!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","commit","()Z",editor,NULL,0,&out)||out.i!=1)goto end;if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","clear","()Landroid/content/SharedPreferences$Editor;",editor,NULL,0,&out)||!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","apply","()V",editor,NULL,0,&out))goto end;if(!prefs_runtime_call(v,"android/content/SharedPreferences","contains","(Ljava/lang/String;)Z",reopened,a,1,&out)||out.i!=0)goto end;ok=typed_selftest(v,reopened,d,name);end:if(d){if(!run(d,"DELETE FROM prefs WHERE name=?",name,NULL,NULL,0))ok=false;if(sqlite3_close(d)!=SQLITE_OK)ok=false;}if(ok)fprintf(stderr,"PREFS_RUNTIME_SELFTEST PASS typed_migration=1 string_set=1 boolean_interop=1\n");return ok;}
static bool typed_selftest(struct dvm *v,dvm_ref prefs,sqlite3 *d,const char *name){
 union dvm_value a[2]={{0},{0}},out={0};
 a[0].l=dvm_new_string(v,"bool");a[1].i=1;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getBoolean","(Ljava/lang/String;Z)Z",prefs,a,2,&out)||out.i!=1)return false;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","edit","()Landroid/content/SharedPreferences$Editor;",prefs,NULL,0,&out)||!out.l)return false;
 dvm_ref e=out.l;a[1].i=0;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","putBoolean","(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;",e,a,2,&out)||!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","commit","()Z",e,NULL,0,&out)||out.i!=1)return false;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getBoolean","(Ljava/lang/String;Z)Z",prefs,a,2,&out)||out.i!=0)return false;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getAll","()Ljava/util/Map;",prefs,NULL,0,&out)||!out.l)return false;
 dvm_ref snapshot=out.l;union dvm_value key={.l=a[0].l};
 if(!collections_call(v,"java/util/HashMap","get","(Ljava/lang/Object;)Ljava/lang/Object;",snapshot,&key,1,&out)||!out.l)return false;
 dvm_ref boxed=out.l;
 if(!java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",boxed,NULL,0,&out)||out.i!=0)return false;
 if(boxed!=java_basics_boolean_box(v,0))return false;
 a[1].i=1;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","putBoolean","(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;",e,a,2,&out)||!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","commit","()Z",e,NULL,0,&out)||out.i!=1)return false;
 if(!collections_call(v,"java/util/HashMap","get","(Ljava/lang/Object;)Ljava/lang/Object;",snapshot,&key,1,&out)||out.l!=boxed)return false;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getAll","()Ljava/util/Map;",prefs,NULL,0,&out)||!out.l)return false;
 dvm_ref fresh=out.l;
 if(!collections_call(v,"java/util/HashMap","get","(Ljava/lang/Object;)Ljava/lang/Object;",fresh,&key,1,&out)||!out.l||out.l!=java_basics_boolean_box(v,1))return false;
 dvm_ref boxed_true=out.l;union dvm_value eq={.l=boxed_true};
 if(!java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",boxed_true,NULL,0,&out)||!out.i)return false;
 if(!java_basics_call(v,"java/lang/Boolean","equals","(Ljava/lang/Object;)Z",boxed_true,&eq,1,&out)||!out.i)return false;
 eq.l=java_basics_boolean_box(v,0);
 if(!java_basics_call(v,"java/lang/Boolean","equals","(Ljava/lang/Object;)Z",boxed_true,&eq,1,&out)||out.i)return false;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",prefs,a,2,&out)||!dvm_exception(v))return false;
 dvm_clear_exception(v);
 if(!run(d,"INSERT OR REPLACE INTO prefs(name,key,value) VALUES(?,?,?)",name,"old","legacy",6))return false;
 a[0].l=dvm_new_string(v,"old");a[1].l=0;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",prefs,a,2,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"legacy"))return false;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getBoolean","(Ljava/lang/String;Z)Z",prefs,a,2,&out)||!dvm_exception(v))return false;
 dvm_clear_exception(v);
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getAll","()Ljava/util/Map;",prefs,NULL,0,&out)||!out.l)return false;
 dvm_ref all=out.l;key.l=a[0].l;
 if(!collections_call(v,"java/util/HashMap","get","(Ljava/lang/Object;)Ljava/lang/Object;",all,&key,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"legacy"))return false;
 sqlite3_stmt *t=NULL;bool changed=sqlite3_prepare_v2(d,"UPDATE prefs SET kind='unknown' WHERE name=? AND key='old'",-1,&t,NULL)==SQLITE_OK&&bind(t,1,name,-1)&&sqlite3_step(t)==SQLITE_DONE;
 if(t&&sqlite3_finalize(t)!=SQLITE_OK)changed=false;if(!changed)return false;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getAll","()Ljava/util/Map;",prefs,NULL,0,&out)||!dvm_exception(v))return false;
 dvm_clear_exception(v);
 dvm_ref hs=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashSet"));
 union dvm_value sout={0},item={0};
 if(!hs||!set_runtime_call(v,"java/util/HashSet","<init>","()V",hs,NULL,0,&sout))return false;
 item.l=dvm_new_string(v,"alpha");
 if(!set_runtime_call(v,"java/util/HashSet","add","(Ljava/lang/Object;)Z",hs,&item,1,&sout)||!sout.i)return false;
 item.l=dvm_new_string(v,"beta");
 if(!set_runtime_call(v,"java/util/HashSet","add","(Ljava/lang/Object;)Z",hs,&item,1,&sout)||!sout.i)return false;
 a[0].l=dvm_new_string(v,"set");a[1].l=hs;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","putStringSet","(Ljava/lang/String;Ljava/util/Set;)Landroid/content/SharedPreferences$Editor;",e,a,2,&out))return false;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","commit","()Z",e,NULL,0,&out)||out.i!=1)return false;
 a[1].l=0;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getStringSet","(Ljava/lang/String;Ljava/util/Set;)Ljava/util/Set;",prefs,a,2,&out)||!out.l)return false;
 dvm_ref read=out.l;
 if(!set_runtime_call(v,"java/util/HashSet","size","()I",read,NULL,0,&sout)||sout.i!=2)return false;
 item.l=dvm_new_string(v,"gamma");
 if(!set_runtime_call(v,"java/util/HashSet","add","(Ljava/lang/Object;)Z",read,&item,1,&sout))return false;
 a[1].l=0;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getStringSet","(Ljava/lang/String;Ljava/util/Set;)Ljava/util/Set;",prefs,a,2,&out)||!out.l)return false;
 if(!set_runtime_call(v,"java/util/HashSet","size","()I",out.l,NULL,0,&sout)||sout.i!=2)return false;
 return true;
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int prefs_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;(void)visit;(void)user;return o->data_destroy==pref_free||o->data_destroy==edit_free?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
