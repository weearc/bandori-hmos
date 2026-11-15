#include "sqlite_runtime.h"
#include "java_basics.h"
#include "small_boxes.h"
#include "android_util.h"
#include "collections.h"
#include "sqlite3.h"
#include "file_runtime.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

/* SQLiteDatabase.execSQL(String,Object[]) binding, as actually implemented:
 *   - null                          -> SQL NULL
 *   - java.lang.String              -> TEXT bound with its real UTF-8 byte
 *                                      length (embedded NUL bytes preserved)
 *   - byte[]                        -> BLOB, including zero-length arrays
 *   - java.lang.Integer/Long        -> INTEGER via java_basics longValue
 *   - java.lang.Boolean             -> INTEGER 0/1 via java_basics
 *                                      booleanValue (a foreign payload such
 *                                      as the prefs-owned Boolean fails
 *                                      through java_basics_call and throws)
 *   - java.lang.Float/Double        -> NOT implemented; rejected explicitly
 *   - every other type              -> SQLiteException (no silent null)
 * execSQL(String,Object[]) also follows Android's non-query contract: a null
 * bindArgs array, a bind-argument count mismatch and row-returning SQL are
 * rejected instead of being consumed and reported as success.
 * Cursor.getString() keeps the full stored byte length, including NULs. */

typedef struct { sqlite3 *db; int refs; } shared;
typedef struct { shared *s; int closed; } database;
typedef struct cursor { shared *s; sqlite3_stmt *stmt; sqlite3_value **cells; char **names; int cols, pos, count, closed; size_t cleanup_at; struct cursor *cleanup_next; } cursor;
typedef struct { int version; dvm_ref db,context,name; struct dvm *vm; } helper;
static void helper_destroy(void *p) { helper *h=p; if(h){if(h->db)dvm_unpin(h->vm,h->db);if(h->name)dvm_unpin(h->vm,h->name);if(h->context)dvm_unpin(h->vm,h->context);free(h);} }
static void release(shared *s) { if (s && !--s->refs) { sqlite3_close_v2(s->db); free(s); } }
static void db_destroy(void *p) { database *d=p; if(d){release(d->s);free(d);} }
/* Timing only: no query/cell content or credential is recorded. Records are
 * buffered by the collector and written only after STW has ended. */
extern void dvm_gc_cursor_destroy_note(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,size_t,int) __attribute__((weak));
static uint64_t cursor_destroy_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
extern bool dvm_gc_native_destroy_defer_enabled(void) __attribute__((weak));
static cursor *cursor_cleanup_head,*cursor_cleanup_tail;
static size_t cursor_cleanup_count,cursor_cleanup_cells;
static void cur_destroy(void *p) {
 cursor *c=p;if(!c)return;
 size_t cells=(size_t)c->count*(size_t)c->cols;
 /* This node contains native SQLite resources only (no guest references).
  * Retain its shared DB ownership until cleanup actually completes. Guest
  * close/error handling stays synchronous; only GC destruction can defer. */
 if(cells>256&&dvm_gc_native_destroy_defer_enabled&&dvm_gc_native_destroy_defer_enabled()){
  c->cleanup_at=0;c->cleanup_next=NULL;
  if(cursor_cleanup_tail)cursor_cleanup_tail->cleanup_next=c;else cursor_cleanup_head=c;
  cursor_cleanup_tail=c;cursor_cleanup_count++;cursor_cleanup_cells+=cells;return;
 }
 int had_stmt=c->stmt!=NULL;
 uint64_t start=cursor_destroy_ns();sqlite3_finalize(c->stmt);uint64_t finalized=cursor_destroy_ns();
 for(size_t i=0;i<cells;i++)sqlite3_value_free(c->cells[i]);uint64_t values_done=cursor_destroy_ns();
 free(c->cells);if(c->names)for(int i=0;i<c->cols;i++)free(c->names[i]);free(c->names);uint64_t arrays_done=cursor_destroy_ns();
 release(c->s);uint64_t shared_done=cursor_destroy_ns();free(c);uint64_t end=cursor_destroy_ns();
 if(dvm_gc_cursor_destroy_note)dvm_gc_cursor_destroy_note(start,finalized,values_done,arrays_done,shared_done,end,cells,had_stmt);
}
/* Called under the DVM GIL: shared.refs is not atomic. No worker thread or
 * SQLite shared ownership mutation can race this loop. Individual native
 * operations are not preemptible; measured overruns must remain visible. */
int sqlite_gc_deferred_step(uint64_t budget_ns,uint64_t out[7]){
 memset(out,0,7*sizeof *out);if(!cursor_cleanup_head)return 0;
 uint64_t start=cursor_destroy_ns(),max_batch=0,processed=0,finished=0;
 do{
  cursor *c=cursor_cleanup_head;size_t cells=(size_t)c->count*(size_t)c->cols;
  uint64_t batch_start=cursor_destroy_ns();
  if(c->stmt){sqlite3_finalize(c->stmt);c->stmt=NULL;}
  size_t stop=c->cleanup_at+256;if(stop>cells)stop=cells;
  while(c->cleanup_at<stop){sqlite3_value_free(c->cells[c->cleanup_at]);c->cells[c->cleanup_at++]=NULL;processed++;cursor_cleanup_cells--;}
  if(c->cleanup_at==cells){
   free(c->cells);if(c->names)for(int i=0;i<c->cols;i++)free(c->names[i]);free(c->names);release(c->s);
   cursor_cleanup_head=c->cleanup_next;if(!cursor_cleanup_head)cursor_cleanup_tail=NULL;cursor_cleanup_count--;free(c);finished++;
  }
  uint64_t span=cursor_destroy_ns()-batch_start;if(span>max_batch)max_batch=span;
 }while(cursor_cleanup_head&&cursor_destroy_ns()-start<budget_ns);
 out[0]=processed;out[1]=finished;out[2]=cursor_cleanup_count;out[3]=cursor_cleanup_cells;
 out[4]=cursor_destroy_ns()-start;out[5]=max_batch;out[6]=budget_ns;return 1;
}

static void error(struct dvm *v, shared *s) { dvm__throw(v,"android/database/sqlite/SQLiteException","%s",s?sqlite3_errmsg(s->db):"SQLite failure"); }
#define IS(n,s) (!strcmp(m,n)&&!strcmp(sig,s))
static void persistence_log(const char *fmt,...){
 int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/sqlite-storage.txt","a");
 if(f){fprintf(f,"SQLITE_STORAGE pid=%d epoch=%lld ",getpid(),(long long)time(NULL));va_list ap;va_start(ap,fmt);vfprintf(f,fmt,ap);va_end(ap);fputc('\n',f);fclose(f);}errno=saved;
}
static void database_close(database *d){if(d&&!d->closed){d->closed=1;release(d->s);d->s=NULL;}}
static dvm_ref sqlite_open_path(struct dvm *v,const char *path){
 char resolved[PATH_MAX];bool memory=path&&!strcmp(path,":memory:");
 if(!path||(!memory&&!file_runtime_private_sqlite_path(v,path,resolved,sizeof resolved))){dvm__throw(v,"android/database/sqlite/SQLiteException","database path must be a private non-symlink file");return 0;}
 bool created=false;
 if(!memory){int fd=open(path,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600);if(fd>=0){created=true;if(close(fd)){dvm__throw(v,"android/database/sqlite/SQLiteException","cannot close new database file");return 0;}}else if(errno!=EEXIST){persistence_log("event=open path=%s rc=-1 errno=%d",path,errno);dvm__throw(v,"android/database/sqlite/SQLiteException","cannot create database: errno=%d",errno);return 0;}}
 shared *s=calloc(1,sizeof *s);database *d=calloc(1,sizeof *d);
 if(!s||!d){free(s);free(d);dvm__throw(v,"java/lang/OutOfMemoryError","SQLite database handle");return 0;}
 int rc=sqlite3_open_v2(path,&s->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);
 struct stat st;long long bytes=!memory&&!stat(path,&st)?st.st_size:0;
 persistence_log("event=open path=%s rc=%d created=%d bytes=%lld memory=%d",path,rc,created,bytes,memory);
 if(rc!=SQLITE_OK){error(v,s);sqlite3_close_v2(s->db);free(s);free(d);return 0;}
 sqlite3_busy_timeout(s->db,5000);s->refs=1;d->s=s;
 struct dvm_class *cls=dvm_find_class_or_external(v,"android/database/sqlite/SQLiteDatabase");dvm_ref ref=cls?dvm_new_object(v,cls):0;struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
 if(!o){db_destroy(d);if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","SQLite database object");return 0;}o->data=d;o->data_destroy=db_destroy;return ref;
}
static dvm_ref sqlite_open_context(struct dvm *v,dvm_ref context,dvm_ref name){
 if(!name)return sqlite_open_path(v,":memory:");
 union dvm_value a={.l=name},file={0},text={0};
 if(!file_runtime_call(v,"android/content/Context","getDatabasePath","(Ljava/lang/String;)Ljava/io/File;",context,&a,1,&file)||dvm_exception(v)||!file.l){if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","Context.getDatabasePath unavailable");return 0;}
 if(!file_runtime_call(v,"java/io/File","getAbsolutePath","()Ljava/lang/String;",file.l,NULL,0,&text)||dvm_exception(v)||!text.l)return 0;
 return sqlite_open_path(v,dvm_string_utf8(v,text.l));
}

static void bind_sqlite_error(struct dvm *v,int index,int rc) {
 dvm__throw(v,"android/database/sqlite/SQLiteException","cannot bind argument %d: %s",index,sqlite3_errstr(rc));
}

/* Bind one Java bind argument. All failures are reported through dvm__throw;
 * a pending VM exception (for example a Boolean with a foreign payload) is
 * left untouched and the caller finalizes the statement without stepping it. */
static void bind_arg(struct dvm *v,sqlite3_stmt *st,int index,dvm_ref ref) {
 if(!ref) {
  int rc=sqlite3_bind_null(st,index);
  if(rc!=SQLITE_OK)bind_sqlite_error(v,index,rc);
  return;
 }
 struct dvm_object *item=dvm__obj(v,ref);
 if(!item||!item->cls) {
  dvm__throw(v,"android/database/sqlite/SQLiteException","invalid bind argument at index %d",index);
  return;
 }
 if(item->kind==DVM_OBJ_STRING) {
  const char *s=dvm_string_utf8(v,ref);
  uint32_t len=dvm_string_utf8_length(v,ref);
  if(!s) {
   dvm__throw(v,"android/database/sqlite/SQLiteException","bind argument %d is not a valid String",index);
   return;
  }
  if(len>(uint32_t)INT_MAX) {
   dvm__throw(v,"java/lang/IllegalArgumentException","String bind argument %d exceeds INT_MAX UTF-8 bytes",index);
   return;
  }
  /* Explicit byte length, not -1: a Java String may contain U+0000. */
  int rc=sqlite3_bind_text(st,index,s,(int)len,SQLITE_TRANSIENT);
  if(rc!=SQLITE_OK)bind_sqlite_error(v,index,rc);
  return;
 }
 if(item->kind==DVM_OBJ_ARRAY) {
  if(item->elem_kind!='B') {
   dvm__throw(v,"android/database/sqlite/SQLiteException","unsupported bind argument type at index %d: %s (only byte[] arrays are supported)",index,item->cls->name?item->cls->name:"array");
   return;
  }
  uint32_t len=dvm_array_length(v,ref);
  void *data=dvm_array_data(v,ref);
  if(len>(uint32_t)INT_MAX) {
   dvm__throw(v,"java/lang/IllegalArgumentException","byte[] bind argument %d exceeds INT_MAX bytes",index);
   return;
  }
  if(len&&!data) {
   dvm__throw(v,"android/database/sqlite/SQLiteException","byte[] bind argument %d has null storage",index);
   return;
  }
  /* A non-NULL pointer with length 0 keeps an empty array a zero-length BLOB
   * instead of turning it into SQL NULL. */
  int rc=sqlite3_bind_blob(st,index,len?data:(const void*)"",(int)len,SQLITE_TRANSIENT);
  if(rc!=SQLITE_OK)bind_sqlite_error(v,index,rc);
  return;
 }
 const char *name=item->cls->name?item->cls->name:"";
 union dvm_value num={0};
 if(!strcmp(name,"java/lang/Integer")||!strcmp(name,"java/lang/Long")) {
  if(!java_basics_call(v,name,"longValue","()J",ref,NULL,0,&num)) {
   if(dvm_exception(v))return;
   dvm__throw(v,"android/database/sqlite/SQLiteException","unsupported bind argument type at index %d: %s",index,name);
   return;
  }
  if(dvm_exception(v))return;
  int rc=sqlite3_bind_int64(st,index,num.j);
  if(rc!=SQLITE_OK)bind_sqlite_error(v,index,rc);
  return;
 }
 if(!strcmp(name,"java/lang/Boolean")) {
  if(!java_basics_call(v,name,"booleanValue","()Z",ref,NULL,0,&num)) {
   if(dvm_exception(v))return;
   dvm__throw(v,"android/database/sqlite/SQLiteException","unsupported bind argument type at index %d: %s",index,name);
   return;
  }
  /* A prefs-owned Boolean has a different payload layout. java_basics_call
   * reports the mismatch as a pending VM exception; do not inspect the
   * payload here and do not bind a fabricated null/zero. */
  if(dvm_exception(v))return;
  int rc=sqlite3_bind_int64(st,index,num.i?1:0);
  if(rc!=SQLITE_OK)bind_sqlite_error(v,index,rc);
  return;
 }
 if(!strcmp(name,"java/lang/Float")||!strcmp(name,"java/lang/Double")) {
  dvm__throw(v,"java/lang/UnsupportedOperationException","SQLite bind of %s is not implemented; Float/Double are not supported",name);
  return;
 }
 dvm__throw(v,"android/database/sqlite/SQLiteException","unsupported bind argument type at index %d: %s",index,name[0]?name:"unknown");
}

/* Nine-argument SQLiteDatabase.query builds one SQL statement and then uses
 * the existing rawQuery prepare/bind path.  The buffer is bounded (1 MiB) and
 * every over-limit append fails closed before any SQL is prepared. */
#define SQLITE_QUERY_SQL_LIMIT (1024u*1024u)
static bool query_text(struct dvm *v,dvm_ref ref,const char *what,const char **out,size_t *len) {
 *out=NULL;*len=0;
 if(!ref)return true;
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||o->kind!=DVM_OBJ_STRING) {
  dvm__throw(v,"java/lang/IllegalArgumentException","%s must be a String",what);
  return false;
 }
 const char *s=dvm_string_utf8(v,ref);
 uint32_t n=dvm_string_utf8_length(v,ref);
 if(!s||strlen(s)!=(size_t)n) {
  dvm__throw(v,"java/lang/IllegalArgumentException","%s contains an embedded NUL",what);
  return false;
 }
 *out=s;*len=n;return true;
}
static bool query_put(struct dvm *v,char *buf,size_t *used,const char *s,size_t n) {
 if(n>SQLITE_QUERY_SQL_LIMIT-1-*used) {
  dvm__throw(v,"java/lang/IllegalArgumentException","query statement exceeds %u bytes",(unsigned)SQLITE_QUERY_SQL_LIMIT);
  return false;
 }
 if(n)memcpy(buf+*used,s,n);
 *used+=n;return true;
}
static char *query_build_sql(struct dvm *v,int distinct,dvm_ref table_ref,dvm_ref columns_ref,
 dvm_ref selection_ref,dvm_ref group_ref,dvm_ref having_ref,dvm_ref order_ref,dvm_ref limit_ref) {
 const char *table=NULL;size_t table_len=0;
 if(!table_ref) {
  dvm__throw(v,"java/lang/IllegalArgumentException","table is null");
  return NULL;
 }
 if(!query_text(v,table_ref,"table",&table,&table_len))return NULL;
 char *buf=malloc(SQLITE_QUERY_SQL_LIMIT);
 if(!buf) {
  dvm__throw(v,"java/lang/OutOfMemoryError","query SQL buffer");
  return NULL;
 }
 size_t used=0;
 if(!query_put(v,buf,&used,"SELECT ",7))goto fail;
 if(distinct&&!query_put(v,buf,&used,"DISTINCT ",9))goto fail;
 if(columns_ref) {
  struct dvm_object *cols=dvm__obj(v,columns_ref);
  if(!cols||cols->kind!=DVM_OBJ_ARRAY||cols->elem_kind!='L'||(cols->length&&!cols->data)) {
   dvm__throw(v,"java/lang/IllegalArgumentException","columns must be a String array");
   goto fail;
  }
  if(cols->length>1000000u) {
   dvm__throw(v,"java/lang/IllegalArgumentException","columns array is too large");
   goto fail;
  }
  if(!cols->length) {
   if(!query_put(v,buf,&used,"*",1))goto fail;
  } else for(uint32_t i=0;i<cols->length;i++) {
   if(i&&!query_put(v,buf,&used,",",1))goto fail;
   dvm_ref item=((dvm_ref*)cols->data)[i];
   if(!item) {
    dvm__throw(v,"java/lang/IllegalArgumentException","columns contains null");
    goto fail;
   }
   const char *text=NULL;size_t len=0;
   if(!query_text(v,item,"columns element",&text,&len)||!query_put(v,buf,&used,text,len))goto fail;
  }
 } else if(!query_put(v,buf,&used,"*",1))goto fail;
 if(!query_put(v,buf,&used," FROM ",6)||!query_put(v,buf,&used,table,table_len))goto fail;
 {
  static const char *const clauses[5]={" WHERE "," GROUP BY "," HAVING "," ORDER BY "," LIMIT "};
  static const char *const names[5]={"selection","groupBy","having","orderBy","limit"};
  dvm_ref refs[5]={selection_ref,group_ref,having_ref,order_ref,limit_ref};
  for(int i=0;i<5;i++) {
   const char *text=NULL;size_t len=0;
   if(!query_text(v,refs[i],names[i],&text,&len))goto fail;
   if(!text||!len)continue;
   if(!query_put(v,buf,&used,clauses[i],strlen(clauses[i]))||!query_put(v,buf,&used,text,len))goto fail;
  }
 }
 buf[used]=0;return buf;
fail:
 free(buf);return NULL;
}

/* SQLiteDatabase.replace(table,nullColumnHack,values) follows AOSP's
 * insertWithOnConflict(..., CONFLICT_REPLACE): it builds
 *   INSERT OR REPLACE INTO table(col,...) VALUES (?,...)
 * and returns last_insert_rowid().  An empty ContentValues produces
 *   INSERT OR REPLACE INTO table(<nullColumnHack or literal null>) VALUES (NULL)
 * exactly like AOSP; a null nullColumnHack is therefore the literal token
 * "null" and the resulting prepare failure is reported as -1.  replace()
 * catches SQLException and returns -1, but an OutOfMemoryError stays pending.
 * This runtime's ContentValues owns a real HashMap and exposes keySet()/get()
 * rather than entrySet(), so each key is read from the backed KeySet view and
 * its value through get(String); the same iteration order is used for the
 * column list and for the bind values. */
static bool replace_put(char *buf,size_t *used,const char *s,size_t n){
 if(n>SQLITE_QUERY_SQL_LIMIT-1-*used)return false;
 if(n)memcpy(buf+*used,s,n);
 *used+=n;return true;
}
static bool replace_sql_exception(struct dvm *v){
 dvm_ref e=dvm_exception(v);
 if(!e)return false;
 for(struct dvm_class *c=dvm_object_class(v,e);c;c=c->super)
  if(c->name&&(!strcmp(c->name,"android/database/sqlite/SQLiteException")||
               !strcmp(c->name,"android/database/SQLException")))return true;
 return false;
}
/* Byte/Short live in small_boxes_call and Float/Double in java_basics_call;
 * neither module registers those methods in the external class method table,
 * so dvm_find_method()/dvm_call() cannot reach them from here.  Dispatch the
 * unboxing call through the same hooks the interpreter uses. */
static bool replace_boxed(struct dvm *v,dvm_ref ref,const char *method,const char *sig,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||!o->cls||!o->cls->name)return false;
 if(!strcmp(o->cls->name,"java/lang/Byte")||!strcmp(o->cls->name,"java/lang/Short"))
  return small_boxes_call(v,o->cls->name,method,sig,ref,NULL,0,out);
 if(!strcmp(o->cls->name,"java/lang/Float")||!strcmp(o->cls->name,"java/lang/Double"))
  return java_basics_call(v,o->cls->name,method,sig,ref,NULL,0,out);
 return false;
}
/* AOSP DatabaseUtils.bindObjectToProgram binds Byte/Short/Integer/Long as
 * INTEGER and Float/Double as REAL.  bind_arg() intentionally rejects
 * Float/Double for execSQL(String,Object[]) and has no Byte/Short branches, so
 * the ContentValues path adds those types here without changing execSQL. */
static bool bind_content_value(struct dvm *v,sqlite3_stmt *st,int index,dvm_ref ref){
 if(!ref){
  int rc=sqlite3_bind_null(st,index);
  if(rc!=SQLITE_OK)bind_sqlite_error(v,index,rc);
  return !dvm_exception(v);
 }
 struct dvm_object *item=dvm__obj(v,ref);
 const char *name=item&&item->cls&&item->cls->name?item->cls->name:"";
 union dvm_value num={0};
 int rc=SQLITE_OK;
 if(!strcmp(name,"java/lang/Byte")||!strcmp(name,"java/lang/Short")){
  if(!replace_boxed(v,ref,"intValue","()I",&num)){
   if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","cannot read %s.intValue",name);
   return false;
  }
  rc=sqlite3_bind_int64(st,index,(sqlite3_int64)num.i);
 } else if(!strcmp(name,"java/lang/Float")){
  if(!replace_boxed(v,ref,"floatValue","()F",&num)){
   if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","cannot read java/lang/Float.floatValue");
   return false;
  }
  rc=sqlite3_bind_double(st,index,(double)num.f);
 } else if(!strcmp(name,"java/lang/Double")){
  if(!replace_boxed(v,ref,"doubleValue","()D",&num)){
   if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","cannot read java/lang/Double.doubleValue");
   return false;
  }
  rc=sqlite3_bind_double(st,index,num.d);
 } else {
  bind_arg(v,st,index,ref);
  return !dvm_exception(v);
 }
 if(dvm_exception(v))return false;
 if(rc!=SQLITE_OK)bind_sqlite_error(v,index,rc);
 return !dvm_exception(v);
}
/* Shared ContentValues snapshot for replace/update. Caller owns the arrays,
 * including on failure; SQL values are always bound, never interpolated. */
static bool content_values_collect(struct dvm *v,dvm_ref values,int *count,dvm_ref **out_keys,dvm_ref **out_vals){
 int size=0;dvm_ref *keys=NULL,*vals=NULL;
 *count=0;*out_keys=NULL;*out_vals=NULL;
 if(values){
  union dvm_value sz={0};
  if(!android_util_call(v,"android/content/ContentValues","size","()I",values,NULL,0,&sz)){
   if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalArgumentException","invalid ContentValues");
   return false;
  }
  if(dvm_exception(v))return false;
  if(sz.i<0||sz.i>1000000){dvm__throw(v,"java/lang/IllegalArgumentException","ContentValues size exceeds limit");return false;}
  size=sz.i;
 }
 if(size){
  keys=calloc((size_t)size,sizeof *keys);
  vals=calloc((size_t)size,sizeof *vals);
  *out_keys=keys;*out_vals=vals;
  if(!keys||!vals){dvm__throw(v,"java/lang/OutOfMemoryError","ContentValues snapshot");return false;}
  union dvm_value set={0},iter={0};
  if(!android_util_call(v,"android/content/ContentValues","keySet","()Ljava/util/Set;",values,NULL,0,&set)||!set.l){if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues.keySet returned null");return false;}
  if(!collections_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",set.l,NULL,0,&iter)||!iter.l){if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues.keySet iterator returned null");return false;}
  int got=0;
  for(;;){
   union dvm_value has={0},key={0},gotv={0};
   if(!collections_call(v,"java/util/Iterator","hasNext","()Z",iter.l,NULL,0,&has)){if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues key iterator failed");return false;}
   if(!has.i)break;
   if(got>=size){dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues size changed while iterating");return false;}
   if(!collections_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",iter.l,NULL,0,&key)){if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues key iterator failed");return false;}
   struct dvm_object *ko=key.l?dvm__obj(v,key.l):NULL;
   if(!ko||ko->kind!=DVM_OBJ_STRING){dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues key is not a String");return false;}
   const char *k=dvm_string_utf8(v,key.l);
   uint32_t klen=dvm_string_utf8_length(v,key.l);
   if(!k||memchr(k,0,klen)){dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues key contains an embedded NUL");return false;}
   union dvm_value kg={.l=key.l};
   if(!android_util_call(v,"android/content/ContentValues","get","(Ljava/lang/String;)Ljava/lang/Object;",values,&kg,1,&gotv)){if(!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues.get failed");return false;}
   keys[got]=key.l;
   vals[got]=gotv.l;
   ++got;
  }
  if(got!=size){dvm__throw(v,"android/database/sqlite/SQLiteException","ContentValues size changed while iterating");return false;}
 }
 *count=size;return true;
}
static bool sqlite_replace(struct dvm *v,database *d,const union dvm_value *a,union dvm_value *out){
 out->j=-1;
 const char *table=NULL,*hack=NULL;
 size_t table_len=0,hack_len=0;
 dvm_ref values=a[2].l;
 struct dvm_object *to=a[0].l?dvm__obj(v,a[0].l):NULL;
 if(!to||to->kind!=DVM_OBJ_STRING)return true;
 table=dvm_string_utf8(v,a[0].l);
 table_len=dvm_string_utf8_length(v,a[0].l);
 if(!table||memchr(table,0,table_len))return true;
 if(a[1].l){
  struct dvm_object *ho=dvm__obj(v,a[1].l);
  if(!ho||ho->kind!=DVM_OBJ_STRING)return true;
  hack=dvm_string_utf8(v,a[1].l);
  hack_len=dvm_string_utf8_length(v,a[1].l);
  if(!hack||memchr(hack,0,hack_len))return true;
 }
 int size=0;size_t used=0;
 char *sql=NULL;dvm_ref *keys=NULL,*vals=NULL;sqlite3_stmt *st=NULL;
 if(!content_values_collect(v,values,&size,&keys,&vals))goto fail;
 sql=malloc(SQLITE_QUERY_SQL_LIMIT);
 if(!sql){dvm__throw(v,"java/lang/OutOfMemoryError","replace SQL buffer");goto fail;}
 if(!replace_put(sql,&used,"INSERT OR REPLACE INTO ",23)||
    !replace_put(sql,&used,table,table_len)||
    !replace_put(sql,&used,"(",1))goto too_large;
 if(size){
  for(int i=0;i<size;++i){
   if(i&&!replace_put(sql,&used,",",1))goto too_large;
   const char *k=dvm_string_utf8(v,keys[i]);
   uint32_t klen=dvm_string_utf8_length(v,keys[i]);
   if(!k||memchr(k,0,klen)||!replace_put(sql,&used,k,klen))goto too_large;
  }
  if(!replace_put(sql,&used,") VALUES (",10))goto too_large;
  for(int i=0;i<size;++i)if(!replace_put(sql,&used,i?",?":"?",i?2:1))goto too_large;
  if(!replace_put(sql,&used,")",1))goto too_large;
 } else {
  if(!replace_put(sql,&used,hack?hack:"null",hack?hack_len:4)||
     !replace_put(sql,&used,") VALUES (NULL)",15))goto too_large;
 }
 sql[used]=0;
 if(sqlite3_prepare_v2(d->s->db,sql,-1,&st,NULL)!=SQLITE_OK){error(v,d->s);goto fail;}
 if(sqlite3_bind_parameter_count(st)!=size){dvm__throw(v,"android/database/sqlite/SQLiteException","replace bind argument count mismatch");goto fail;}
 for(int i=0;i<size;++i)if(!bind_content_value(v,st,i+1,vals[i]))goto fail;
 {
  int rc=sqlite3_step(st);
  if(rc!=SQLITE_DONE){error(v,d->s);goto fail;}
 }
 if(sqlite3_finalize(st)!=SQLITE_OK){st=NULL;dvm__throw(v,"android/database/sqlite/SQLiteException","replace finalize failed");goto fail;}
 st=NULL;
 out->j=sqlite3_last_insert_rowid(d->s->db);
 persistence_log("event=replace path=%s rc=0 changed=%d",sqlite3_db_filename(d->s->db,"main"),sqlite3_changes(d->s->db));
 free(sql);
 free(keys);
 free(vals);
 return true;
too_large:
 dvm__throw(v,"android/database/sqlite/SQLiteException","replace SQL exceeds %u bytes",(unsigned)SQLITE_QUERY_SQL_LIMIT);
fail:
 sqlite3_finalize(st);
 free(sql);
 free(keys);
 free(vals);
 if(replace_sql_exception(v))dvm_clear_exception(v);
 return true;
}

/* Android update delegates to updateWithOnConflict(CONFLICT_NONE): nonempty
 * ContentValues, SET values first followed by whereArgs, affected row count.
 * Unlike replace(), SQL errors remain pending exceptions. */
static bool sqlite_update(struct dvm *v,database *d,const union dvm_value *a,union dvm_value *out){
 const char *table=NULL,*where=NULL;size_t table_len=0,where_len=0,used=0;
 char *sql=NULL;dvm_ref *keys=NULL,*vals=NULL;sqlite3_stmt *st=NULL;int size=0,nwhere=0;
 out->i=0;
 if(!a[0].l){dvm__throw(v,"java/lang/IllegalArgumentException","table is null");goto fail;}
 if(!query_text(v,a[0].l,"table",&table,&table_len)||
    !query_text(v,a[2].l,"whereClause",&where,&where_len))goto fail;
 if(!content_values_collect(v,a[1].l,&size,&keys,&vals))goto fail;
 if(!size){dvm__throw(v,"java/lang/IllegalArgumentException","Empty values");goto fail;}
 struct dvm_object *args=a[3].l?dvm__obj(v,a[3].l):NULL;
 if(a[3].l&&(!args||args->kind!=DVM_OBJ_ARRAY||args->elem_kind!='L'||(args->length&&!args->data))){dvm__throw(v,"java/lang/IllegalArgumentException","whereArgs must be a String array");goto fail;}
 if(args&&args->length>1000000u){dvm__throw(v,"java/lang/IllegalArgumentException","too many whereArgs");goto fail;}
 nwhere=args?(int)args->length:0;
 sql=malloc(SQLITE_QUERY_SQL_LIMIT);
 if(!sql){dvm__throw(v,"java/lang/OutOfMemoryError","update SQL buffer");goto fail;}
 if(!query_put(v,sql,&used,"UPDATE ",7)||!query_put(v,sql,&used,table,table_len)||!query_put(v,sql,&used," SET ",5))goto fail;
 for(int i=0;i<size;i++){
  const char *key=dvm_string_utf8(v,keys[i]);size_t len=dvm_string_utf8_length(v,keys[i]);
  if((i&&!query_put(v,sql,&used,",",1))||!query_put(v,sql,&used,key,len)||!query_put(v,sql,&used,"=?",2))goto fail;
 }
 if(where_len&&(!query_put(v,sql,&used," WHERE ",7)||!query_put(v,sql,&used,where,where_len)))goto fail;
 sql[used]=0;
 if(sqlite3_prepare_v2(d->s->db,sql,-1,&st,NULL)!=SQLITE_OK){error(v,d->s);goto fail;}
 if(!st||sqlite3_bind_parameter_count(st)!=size+nwhere){dvm__throw(v,"android/database/sqlite/SQLiteException","update bind argument count mismatch");goto fail;}
 for(int i=0;i<size;i++)if(!bind_content_value(v,st,i+1,vals[i]))goto fail;
 for(int i=0;i<nwhere;i++){
  dvm_ref ref=((dvm_ref*)args->data)[i];struct dvm_object *item=ref?dvm__obj(v,ref):NULL;
  if(ref&&(!item||item->kind!=DVM_OBJ_STRING)){dvm__throw(v,"java/lang/IllegalArgumentException","whereArgs element must be a String or null");goto fail;}
  bind_arg(v,st,size+i+1,ref);if(dvm_exception(v))goto fail;
 }
 if(sqlite3_step(st)!=SQLITE_DONE){error(v,d->s);goto fail;}
 if(sqlite3_finalize(st)!=SQLITE_OK){st=NULL;error(v,d->s);goto fail;}st=NULL;
 out->i=sqlite3_changes(d->s->db);
 persistence_log("event=update path=%s rc=0 changed=%d",sqlite3_db_filename(d->s->db,"main"),out->i);
 free(sql);free(keys);free(vals);return true;
fail:
 persistence_log("event=update path=%s rc=-1 changed=0",sqlite3_db_filename(d->s->db,"main"));
 sqlite3_finalize(st);free(sql);free(keys);free(vals);return true;
}

bool sqlite_runtime_call(struct dvm *v,const char *cl,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,union dvm_value *out) {
 struct dvm_object *o=dvm__obj(v,self);
 bool context=false;for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)if(!strcmp(k->name,"android/content/Context")){context=true;break;}
 if(context&&(IS("openOrCreateDatabase","(Ljava/lang/String;ILandroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;")||IS("openOrCreateDatabase","(Ljava/lang/String;ILandroid/database/sqlite/SQLiteDatabase$CursorFactory;Landroid/database/DatabaseErrorHandler;)Landroid/database/sqlite/SQLiteDatabase;"))){
  if(a[1].i||a[2].l||(strstr(sig,"DatabaseErrorHandler;")&&a[3].l)){dvm__throw(v,"java/lang/UnsupportedOperationException","SQLite mode/factory/error handler not supported");return true;}
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","database name");return true;}
  out->l=sqlite_open_context(v,self,a[0].l);return true;
 }
 if(!strcmp(cl,"android/database/sqlite/SQLiteOpenHelper")) {
  if(IS("<init>","(Landroid/content/Context;Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;I)V")) {
   if(!o||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","helper context");return true;}
   if(a[2].l){dvm__throw(v,"java/lang/UnsupportedOperationException","CursorFactory unsupported");return true;}
   if(a[3].i<1){dvm__throw(v,"java/lang/IllegalArgumentException","version must be positive");return true;}
   helper *h=calloc(1,sizeof *h);if(!h){dvm__throw(v,"java/lang/OutOfMemoryError","helper");return true;}h->vm=v;h->version=a[3].i;h->context=a[0].l;dvm_pin(v,h->context);h->name=a[1].l;if(h->name)dvm_pin(v,h->name);o->data=h;o->data_destroy=helper_destroy;return true;
  }
  helper *h=o?o->data:NULL;if(!h){dvm__throw(v,"java/lang/IllegalStateException","helper uninitialized");return true;}
  if(IS("getDatabaseName","()Ljava/lang/String;")){out->l=h->name;return true;}
  if(IS("close","()V")){if(h->db){struct dvm_object *dbo=dvm__obj(v,h->db);database *d=dbo?dbo->data:NULL;database_close(d);dvm_unpin(v,h->db);h->db=0;}return true;}
  if(IS("getWritableDatabase","()Landroid/database/sqlite/SQLiteDatabase;")||IS("getReadableDatabase","()Landroid/database/sqlite/SQLiteDatabase;")) {
   if(h->db){struct dvm_object *existing=dvm__obj(v,h->db);database *ed=existing?existing->data:NULL;if(!ed||ed->closed){dvm_unpin(v,h->db);h->db=0;}}
   if(!h->db){dvm_ref ref=sqlite_open_context(v,h->context,h->name);if(!ref)return true;dvm_pin(v,ref);h->db=ref;database *d=dvm__obj(v,ref)->data;sqlite3_stmt *st=NULL;int old=0;
    if(sqlite3_prepare_v2(d->s->db,"PRAGMA user_version",-1,&st,NULL)!=SQLITE_OK||sqlite3_step(st)!=SQLITE_ROW){sqlite3_finalize(st);error(v,d->s);goto helper_fail;}old=sqlite3_column_int(st,0);sqlite3_finalize(st);
    if(sqlite3_exec(d->s->db,"BEGIN",NULL,NULL,NULL)!=SQLITE_OK){error(v,d->s);goto helper_fail;}
    const char *method=old==0?"onCreate":old<h->version?"onUpgrade":old>h->version?"onDowngrade":NULL;
    if(method){const char *msig=old==0?"(Landroid/database/sqlite/SQLiteDatabase;)V":"(Landroid/database/sqlite/SQLiteDatabase;II)V";struct dvm_method *fn=dvm_find_method(v,dvm_object_class(v,self),method,msig);union dvm_value args[3]={{.l=ref},{.i=old},{.i=h->version}}, ignored={0};if(!fn||(!fn->has_code&&!fn->builtin)){dvm__throw(v,"java/lang/UnsupportedOperationException","missing %s",method);sqlite3_exec(d->s->db,"ROLLBACK",NULL,NULL,NULL);goto helper_fail;}dvm_pin(v,ref);bool callback_ok=dvm_call(v,fn,self,args,old==0?1:3,&ignored);dvm_unpin(v,ref);if(!callback_ok){if(!d->closed&&d->s)sqlite3_exec(d->s->db,"ROLLBACK",NULL,NULL,NULL);goto helper_fail;}if(d->closed||!d->s){dvm__throw(v,"java/lang/IllegalStateException","database closed during helper callback");goto helper_fail;}}
    char pragma[64];snprintf(pragma,sizeof pragma,"PRAGMA user_version=%d",h->version);if(sqlite3_exec(d->s->db,pragma,NULL,NULL,NULL)!=SQLITE_OK||sqlite3_exec(d->s->db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){error(v,d->s);sqlite3_exec(d->s->db,"ROLLBACK",NULL,NULL,NULL);goto helper_fail;}
   }out->l=h->db;return true;
helper_fail: {struct dvm_object *bad=dvm__obj(v,h->db);database_close(bad?bad->data:NULL);}dvm_unpin(v,h->db);h->db=0;return true;
  }
  return false;
 }
 if(!strcmp(cl,"android/database/sqlite/SQLiteDatabase")) {
  if(IS("openOrCreateDatabase","(Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;")||IS("openOrCreateDatabase","(Ljava/io/File;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;")||IS("openOrCreateDatabase","(Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;Landroid/database/DatabaseErrorHandler;)Landroid/database/sqlite/SQLiteDatabase;")){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","database path");return true;}
   if(a[1].l||(strstr(sig,"DatabaseErrorHandler;")&&a[2].l)){dvm__throw(v,"java/lang/UnsupportedOperationException","SQLite factory/error handler not supported");return true;}
   dvm_ref path=a[0].l;if(!strncmp(sig,"(Ljava/io/File;",15)){union dvm_value text={0};if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",path,NULL,0,&text)||dvm_exception(v))return true;path=text.l;}
   const char *value=dvm_string_utf8(v,path);if(!value||strlen(value)!=dvm_string_utf8_length(v,path)){dvm__throw(v,"java/lang/IllegalArgumentException","invalid database path");return true;}
   out->l=sqlite_open_path(v,value);return true;
  }
  database *d=o?o->data:NULL;
  if(IS("close","()V")){database_close(d);return true;}
  if(IS("isOpen","()Z")){out->i=d&&!d->closed;return true;}
  if(!d||d->closed){dvm__throw(v,"java/lang/IllegalStateException","database closed");return true;}
  if(IS("getPath","()Ljava/lang/String;")){const char *p=sqlite3_db_filename(d->s->db,"main");out->l=dvm_new_string(v,p&&*p?p:":memory:");return true;}
  if(IS("replace","(Ljava/lang/String;Ljava/lang/String;Landroid/content/ContentValues;)J"))return sqlite_replace(v,d,a,out);
  if(IS("update","(Ljava/lang/String;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I"))return sqlite_update(v,d,a,out);
  if(IS("execSQL","(Ljava/lang/String;)V")){const char *sql=dvm_string_utf8(v,a[0].l);if(!sql){dvm__throw(v,"java/lang/NullPointerException","sql");return true;}int rc=sqlite3_exec(d->s->db,sql,NULL,NULL,NULL);persistence_log("event=exec path=%s rc=%d changed=%d",sqlite3_db_filename(d->s->db,"main"),rc,sqlite3_changes(d->s->db));if(rc!=SQLITE_OK)error(v,d->s);return true;}
  if(IS("execSQL","(Ljava/lang/String;[Ljava/lang/Object;)V")){
   /* Android's two-argument overload rejects a null bindArgs array before it
    * looks at the SQL text. */
   if(!a[1].l){dvm__throw(v,"java/lang/IllegalArgumentException","Empty bindArgs");return true;}
   const char *sql=dvm_string_utf8(v,a[0].l);if(!sql){dvm__throw(v,"java/lang/NullPointerException","sql");return true;}
   sqlite3_stmt *st=NULL;if(sqlite3_prepare_v2(d->s->db,sql,-1,&st,NULL)!=SQLITE_OK){error(v,d->s);return true;}
   struct dvm_object *ar=dvm__obj(v,a[1].l);
   if(!ar||ar->kind!=DVM_OBJ_ARRAY||ar->elem_kind!='L'||(ar->length&&!ar->data)){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","bindArgs must be an object array");return true;}
   if(ar->length>(uint32_t)INT_MAX){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","bindArgs array is too large");return true;}
   int n=(int)ar->length;
   int expected=sqlite3_bind_parameter_count(st);
   if(n!=expected){sqlite3_finalize(st);dvm__throw(v,"android/database/sqlite/SQLiteException","bind argument count mismatch: expected %d, got %d",expected,n);return true;}
   /* Empty or comment-only SQL prepares to a NULL statement. Android reports
    * a failure here; do not treat a statement that was never executed as a
    * success. */
   if(!st){sqlite3_finalize(st);dvm__throw(v,"android/database/sqlite/SQLiteException","execSQL requires a SQL statement");return true;}
   for(int i=0;i<n;i++){
    bind_arg(v,st,i+1,((dvm_ref*)ar->data)[i]);
    if(dvm_exception(v)){sqlite3_finalize(st);return true;}
   }
   /* execSQL is not a query API.  Android throws when the statement returns
    * rows; consuming them here would silently turn a caller error into
    * success. */
   int rc=sqlite3_step(st);
   if(rc==SQLITE_ROW){sqlite3_finalize(st);dvm__throw(v,"android/database/sqlite/SQLiteException","Queries can be performed using SQLiteDatabase query or rawQuery methods only.");return true;}
   if(rc!=SQLITE_DONE){char detail[256];snprintf(detail,sizeof detail,"%s",sqlite3_errmsg(d->s->db));sqlite3_finalize(st);dvm__throw(v,"android/database/sqlite/SQLiteException","execSQL failed: %s",detail);return true;}
   int frc=sqlite3_finalize(st);
   if(frc!=SQLITE_OK)dvm__throw(v,"android/database/sqlite/SQLiteException","execSQL finalize failed: %s",sqlite3_errstr(frc));
   return true;
  }
  if(IS("rawQuery","(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;")){
   const char *sql=dvm_string_utf8(v,a[0].l);sqlite3_stmt *st=NULL;if(!sql){dvm__throw(v,"java/lang/NullPointerException","sql");return true;}if(sqlite3_prepare_v2(d->s->db,sql,-1,&st,NULL)!=SQLITE_OK){error(v,d->s);return true;}
   struct dvm_object *ar=a[1].l?dvm__obj(v,a[1].l):NULL;if(a[1].l&&(!ar||ar->kind!=DVM_OBJ_ARRAY||ar->elem_kind!='L'||(ar->length&&!ar->data))){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","selectionArgs must be an object array");return true;}if(ar&&ar->length>1000000u){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","too many selectionArgs");return true;}int n=ar?(int)ar->length:0;if(n!=sqlite3_bind_parameter_count(st)){sqlite3_finalize(st);dvm__throw(v,"android/database/sqlite/SQLiteException","selection argument count mismatch");return true;}
   for(int i=0;i<n;i++){dvm_ref ref=((dvm_ref*)ar->data)[i];if(!ref){int brc=sqlite3_bind_null(st,i+1);if(brc!=SQLITE_OK){error(v,d->s);sqlite3_finalize(st);return true;}continue;}struct dvm_object *item=dvm__obj(v,ref);if(!item||item->kind!=DVM_OBJ_STRING){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","selectionArgs element must be a string or null");return true;}const char *s=dvm_string_utf8(v,ref);uint32_t len=dvm_string_utf8_length(v,ref);if(!s){sqlite3_finalize(st);dvm__throw(v,"java/lang/OutOfMemoryError","selectionArgs string");return true;}if(len>(uint32_t)INT_MAX){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","selectionArgs string exceeds INT_MAX UTF-8 bytes");return true;}int rc=sqlite3_bind_text(st,i+1,s,(int)len,SQLITE_TRANSIENT);if(rc!=SQLITE_OK){error(v,d->s);sqlite3_finalize(st);return true;}}
   cursor *c=calloc(1,sizeof *c);if(!c){sqlite3_finalize(st);dvm__throw(v,"java/lang/OutOfMemoryError","cursor");return true;}c->s=d->s;c->s->refs++;c->stmt=st;c->pos=-1;c->cols=sqlite3_column_count(st);c->names=calloc((size_t)c->cols,sizeof *c->names);
   if(c->cols&&!c->names){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","columns");return true;}
   for(int i=0;i<c->cols;i++){const char *name=sqlite3_column_name(st,i);if(!name){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","column name");return true;}size_t len=strlen(name)+1;c->names[i]=malloc(len);if(!c->names[i]){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","column name");return true;}memcpy(c->names[i],name,len);}
   int rc;while((rc=sqlite3_step(st))==SQLITE_ROW){size_t old=(size_t)c->count*(size_t)c->cols;if(c->count>=1000000 || (size_t)c->cols>1000000u-old || (size_t)c->cols>(SIZE_MAX/sizeof *c->cells)-old){cur_destroy(c);dvm__throw(v,"android/database/sqlite/SQLiteException","cursor materialization limit exceeded");return true;}size_t total=old+(size_t)c->cols;sqlite3_value **cells=realloc(c->cells,total*sizeof *cells);if(total&&!cells){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","rows");return true;}c->cells=cells;for(int i=0;i<c->cols;i++){c->cells[old+i]=sqlite3_value_dup(sqlite3_column_value(st,i));if(!c->cells[old+i]){for(int j=0;j<i;j++)sqlite3_value_free(c->cells[old+j]);cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","value");return true;}}c->count++;}if(rc!=SQLITE_DONE){error(v,d->s);cur_destroy(c);return true;}sqlite3_finalize(st);c->stmt=NULL;
   struct dvm_class *cls=dvm_find_class_or_external(v,"android/database/Cursor");dvm_ref ref=cls?dvm_new_object(v,cls):0;struct dvm_object *co=ref?dvm__obj(v,ref):NULL;if(!co){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","cursor object");return true;}co->data=c;co->data_destroy=cur_destroy;out->l=ref;return true;
  }
  if(IS("query","(ZLjava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;")){
   char *sql=query_build_sql(v,a[0].i,a[1].l,a[2].l,a[3].l,a[5].l,a[6].l,a[7].l,a[8].l);
   if(!sql)return true;
   dvm_ref sql_ref=dvm_new_string(v,sql);free(sql);
   if(!sql_ref){dvm__throw(v,"java/lang/OutOfMemoryError","query SQL");return true;}
   union dvm_value raw[2]={{0}};raw[0].l=sql_ref;raw[1].l=a[4].l;
   if(!sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","rawQuery","(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;",self,raw,out)&&!dvm_exception(v))dvm__throw(v,"android/database/sqlite/SQLiteException","query execution failed");
   return true;
  }
 }
 if(!strcmp(cl,"android/database/Cursor")){
  cursor *c=o?o->data:NULL;if(!c){dvm__throw(v,"java/lang/IllegalStateException","cursor uninitialized");return true;}
  if(IS("close","()V")){if(!c->closed){sqlite3_finalize(c->stmt);c->stmt=NULL;c->closed=1;release(c->s);c->s=NULL;}return true;}
  if(IS("isClosed","()Z")){out->i=c->closed;return true;}
  if(c->closed){dvm__throw(v,"java/lang/IllegalStateException","cursor closed");return true;}
  if(IS("getCount","()I")){out->i=c->count;return true;}
  if(IS("moveToFirst","()Z")){c->pos=0;out->i=c->count>0;return true;}
  if(IS("moveToNext","()Z")){if(c->pos<c->count)c->pos++;out->i=c->pos<c->count;return true;}
  if(IS("getColumnCount","()I")){out->i=c->cols;return true;}
  if(IS("getColumnName","(I)Ljava/lang/String;")){
   int i=a[0].i;
   if(i<0||i>=c->cols){dvm__throw(v,"android/database/CursorIndexOutOfBoundsException","invalid cursor column");return true;}
   out->l=dvm_new_string(v,c->names[i]);return true;
  }
  if(IS("getColumnIndex","(Ljava/lang/String;)I")){const char *name=dvm_string_utf8(v,a[0].l);out->i=-1;for(int i=0;name&&i<c->cols;i++)if(!strcmp(name,c->names[i])){out->i=i;break;}return true;}
  if(IS("getInt","(I)I")||IS("getLong","(I)J")||IS("getDouble","(I)D")||IS("getFloat","(I)F")||IS("isNull","(I)Z")||IS("getString","(I)Ljava/lang/String;")){int i=a[0].i;if(i<0||i>=c->cols||c->pos<0||c->pos>=c->count){dvm__throw(v,"android/database/CursorIndexOutOfBoundsException","invalid cursor position or column");return true;}sqlite3_value *value=c->cells[(size_t)c->pos*(size_t)c->cols+(size_t)i];
   if(IS("getInt","(I)I"))out->i=sqlite3_value_int(value);else if(IS("getLong","(I)J"))out->j=sqlite3_value_int64(value);else if(IS("getDouble","(I)D"))out->d=sqlite3_value_double(value);else if(IS("getFloat","(I)F"))out->f=(float)sqlite3_value_double(value);else if(IS("isNull","(I)Z"))out->i=sqlite3_value_type(value)==SQLITE_NULL;else {const unsigned char *str=sqlite3_value_text(value);int len=sqlite3_value_bytes(value);out->l=str?dvm_new_string_n(v,(const char*)str,(size_t)len):0;if(str&&!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","cursor string");}return true;
  }
 }
 return false;
}
dvm_ref sqlite_runtime_open_memory(struct dvm *v) {
 return sqlite_open_path(v,":memory:");
}

/* Real in-memory SQLite coverage for the binding change. No value is read
 * through a second C-side table: every check goes through prepare/bind/step
 * and the public Cursor facade. */
static bool st_exec(struct dvm *v,dvm_ref db,const char *sql,dvm_ref args) {
 union dvm_value a[2]={{0}},out={0};
 a[0].l=dvm_new_string(v,sql);a[1].l=args;
 return a[0].l&&sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","execSQL","(Ljava/lang/String;[Ljava/lang/Object;)V",db,a,&out);
}
static bool st_exec_ok(struct dvm *v,dvm_ref db,const char *sql,dvm_ref args) {
 return st_exec(v,db,sql,args)&&!dvm_exception(v);
}
static bool st_exec_rejected(struct dvm *v,dvm_ref db,const char *sql,dvm_ref args,const char *want_class,const char *want_text) {
 if(!st_exec(v,db,sql,args))return false;
 dvm_ref e=dvm_exception(v);if(!e)return false;
 bool match=true;
 if(want_class){struct dvm_class *c=dvm_object_class(v,e);if(!c||!c->name||strcmp(c->name,want_class))match=false;}
 if(match&&want_text){char why[512];dvm_describe_exception(v,e,why,sizeof why);if(!strstr(why,want_text))match=false;}
 dvm_clear_exception(v);
 return match;
}
static dvm_ref st_args(struct dvm *v,int n,const dvm_ref *items) {
 dvm_ref arr=dvm_new_array(v,'L',"Ljava/lang/Object;",(uint32_t)n);
 if(!arr)return 0;
 dvm_ref *slot=dvm_array_data(v,arr);
 if(n&&!slot)return 0;
 for(int i=0;i<n;i++)slot[i]=items[i];
 return arr;
}
static dvm_ref st_blob(struct dvm *v,const unsigned char *bytes,uint32_t n) {
 dvm_ref arr=dvm_new_array(v,'B',"B",n);
 if(!arr)return 0;
 if(n){unsigned char *p=dvm_array_data(v,arr);if(!p)return 0;memcpy(p,bytes,n);}
 return arr;
}
static dvm_ref st_int(struct dvm *v,int32_t value) {
 dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Integer"));
 union dvm_value a={.i=value},out={0};
 if(!r||!java_basics_call(v,"java/lang/Integer","<init>","(I)V",r,&a,1,&out)||dvm_exception(v))return 0;
 return r;
}
static dvm_ref st_long(struct dvm *v,int64_t value) {
 dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Long"));
 union dvm_value a={.j=value},out={0};
 if(!r||!java_basics_call(v,"java/lang/Long","<init>","(J)V",r,&a,1,&out)||dvm_exception(v))return 0;
 return r;
}
static dvm_ref st_bool(struct dvm *v,int value) {
 union dvm_value a={.i=value?1:0},out={0};
 if(!java_basics_call(v,"java/lang/Boolean","valueOf","(Z)Ljava/lang/Boolean;",0,&a,1,&out)||dvm_exception(v))return 0;
 return out.l;
}
static bool st_query(struct dvm *v,dvm_ref db,const char *sql,dvm_ref args,dvm_ref *cursor) {
 union dvm_value a[2]={{0}},out={0};
 a[0].l=dvm_new_string(v,sql);a[1].l=args;
 if(!a[0].l||!sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","rawQuery","(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;",db,a,&out))return false;
 if(dvm_exception(v)||!out.l)return false;
 *cursor=out.l;return true;
}
static bool st_first(struct dvm *v,dvm_ref c) {
 union dvm_value out={0};
 return sqlite_runtime_call(v,"android/database/Cursor","moveToFirst","()Z",c,NULL,&out)&&!dvm_exception(v)&&out.i;
}
static bool st_int_col(struct dvm *v,dvm_ref c,int col,int *value) {
 union dvm_value a={.i=col},out={0};
 if(!sqlite_runtime_call(v,"android/database/Cursor","getInt","(I)I",c,&a,&out)||dvm_exception(v))return false;
 *value=out.i;return true;
}
static bool st_long_col(struct dvm *v,dvm_ref c,int col,int64_t *value) {
 union dvm_value a={.i=col},out={0};
 if(!sqlite_runtime_call(v,"android/database/Cursor","getLong","(I)J",c,&a,&out)||dvm_exception(v))return false;
 *value=out.j;return true;
}
static bool st_double_col(struct dvm *v,dvm_ref c,int col,double *value) {
 union dvm_value a={.i=col},out={0};
 if(!sqlite_runtime_call(v,"android/database/Cursor","getDouble","(I)D",c,&a,&out)||dvm_exception(v))return false;
 *value=out.d;return true;
}
static bool st_string_col(struct dvm *v,dvm_ref c,int col,dvm_ref *value) {
 union dvm_value a={.i=col},out={0};
 if(!sqlite_runtime_call(v,"android/database/Cursor","getString","(I)Ljava/lang/String;",c,&a,&out)||dvm_exception(v))return false;
 *value=out.l;return true;
}
static bool st_is_null_col(struct dvm *v,dvm_ref c,int col,bool *value) {
 union dvm_value a={.i=col},out={0};
 if(!sqlite_runtime_call(v,"android/database/Cursor","isNull","(I)Z",c,&a,&out)||dvm_exception(v))return false;
 *value=out.i!=0;return true;
}
static bool st_text_is(struct dvm *v,dvm_ref c,int col,const char *want) {
 dvm_ref s=0;
 return st_string_col(v,c,col,&s)&&s&&dvm_string_utf8(v,s)&&!strcmp(dvm_string_utf8(v,s),want);
}
static bool st_count(struct dvm *v,dvm_ref c,int *value) {
 union dvm_value out={0};
 if(!sqlite_runtime_call(v,"android/database/Cursor","getCount","()I",c,NULL,&out)||dvm_exception(v))return false;
 *value=out.i;return true;
}
static bool st_col_count(struct dvm *v,dvm_ref c,int *value) {
 union dvm_value out={0};
 if(!sqlite_runtime_call(v,"android/database/Cursor","getColumnCount","()I",c,NULL,&out)||dvm_exception(v))return false;
 *value=out.i;return true;
}
static bool st_next(struct dvm *v,dvm_ref c,bool *value) {
 union dvm_value out={0};
 if(!sqlite_runtime_call(v,"android/database/Cursor","moveToNext","()Z",c,NULL,&out)||dvm_exception(v))return false;
 *value=out.i!=0;return true;
}
static bool st_query9(struct dvm *v,dvm_ref db,int distinct,dvm_ref table,dvm_ref columns,
 dvm_ref selection,dvm_ref selection_args,dvm_ref group_by,dvm_ref having,dvm_ref order_by,
 dvm_ref limit,dvm_ref *cursor) {
 union dvm_value a[9]={{0}},out={0};
 a[0].i=distinct;a[1].l=table;a[2].l=columns;a[3].l=selection;a[4].l=selection_args;
 a[5].l=group_by;a[6].l=having;a[7].l=order_by;a[8].l=limit;
 if(!sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","query","(ZLjava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",db,a,&out))return false;
 if(dvm_exception(v)||!out.l)return false;
 if(cursor)*cursor=out.l;return true;
}
static bool st_query9_rejected(struct dvm *v,dvm_ref db,int distinct,dvm_ref table,dvm_ref columns,
 dvm_ref selection,dvm_ref selection_args,dvm_ref group_by,dvm_ref having,dvm_ref order_by,
 dvm_ref limit,const char *want_class,const char *want_text) {
 union dvm_value a[9]={{0}},out={0};
 a[0].i=distinct;a[1].l=table;a[2].l=columns;a[3].l=selection;a[4].l=selection_args;
 a[5].l=group_by;a[6].l=having;a[7].l=order_by;a[8].l=limit;
 if(!sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","query","(ZLjava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",db,a,&out))return false;
 dvm_ref e=dvm_exception(v);if(!e)return false;
 bool match=true;
 if(want_class){struct dvm_class *c=dvm_object_class(v,e);if(!c||!c->name||strcmp(c->name,want_class))match=false;}
 if(match&&want_text){char why[512];dvm_describe_exception(v,e,why,sizeof why);if(!strstr(why,want_text))match=false;}
 dvm_clear_exception(v);
 return match;
}
static const char *sqlite_selftest_exception_name(struct dvm *v){
 dvm_ref ref=dvm_exception(v);
 if(!ref)return "none";
 struct dvm_class *cls=dvm_object_class(v,ref);
 if(cls&&cls->name)return cls->name;
 return "unknown";
}

/* ContentValues fixtures for SQLiteDatabase.replace.  The values object owns a
 * real HashMap, so put() goes through the same android_util dispatcher the
 * application uses. */
static dvm_ref st_values_new(struct dvm *v) {
 dvm_ref cv=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/ContentValues"));
 union dvm_value out={0};
 if(!cv||!android_util_call(v,"android/content/ContentValues","<init>","()V",cv,NULL,0,&out)||dvm_exception(v))return 0;
 return cv;
}
static bool st_values_put(struct dvm *v,dvm_ref cv,dvm_ref key,const char *sig,dvm_ref value) {
 union dvm_value a[2]={{0}},out={0};
 a[0].l=key;a[1].l=value;
 if(!key)return false;
 return android_util_call(v,"android/content/ContentValues","put",sig,cv,a,2,&out)&&!dvm_exception(v);
}
static bool st_replace(struct dvm *v,dvm_ref db,dvm_ref table,dvm_ref hack,dvm_ref values,int64_t *rowid) {
 union dvm_value a[3]={{0}},out={0};
 a[0].l=table;a[1].l=hack;a[2].l=values;
 if(!sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","replace","(Ljava/lang/String;Ljava/lang/String;Landroid/content/ContentValues;)J",db,a,&out))return false;
 if(dvm_exception(v))return false;
 *rowid=out.j;return true;
}
static bool st_update(struct dvm *v,dvm_ref db,dvm_ref table,dvm_ref values,dvm_ref where,dvm_ref args,int *changed){
 union dvm_value a[4]={{.l=table},{.l=values},{.l=where},{.l=args}},out={0};
 if(!sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","update","(Ljava/lang/String;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I",db,a,&out)||dvm_exception(v))return false;
 *changed=out.i;return true;
}
static bool st_update_rejected(struct dvm *v,dvm_ref db,dvm_ref table,dvm_ref values,dvm_ref where,dvm_ref args,const char *want_class,const char *want_text){
 union dvm_value a[4]={{.l=table},{.l=values},{.l=where},{.l=args}},out={0};
 if(!sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","update","(Ljava/lang/String;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I",db,a,&out))return false;
 dvm_ref e=dvm_exception(v);if(!e)return false;
 struct dvm_class *c=dvm_object_class(v,e);bool match=c&&c->name&&!strcmp(c->name,want_class);
 if(match&&want_text){char why[512];dvm_describe_exception(v,e,why,sizeof why);match=strstr(why,want_text)!=NULL;}
 dvm_clear_exception(v);return match;
}
static dvm_ref st_byte_box(struct dvm *v,int value) {
 union dvm_value a={.i=value},out={0};
 if(!small_boxes_call(v,"java/lang/Byte","valueOf","(B)Ljava/lang/Byte;",0,&a,1,&out)||dvm_exception(v))return 0;
 return out.l;
}
static dvm_ref st_short_box(struct dvm *v,int value) {
 union dvm_value a={.i=value},out={0};
 if(!small_boxes_call(v,"java/lang/Short","valueOf","(S)Ljava/lang/Short;",0,&a,1,&out)||dvm_exception(v))return 0;
 return out.l;
}
static dvm_ref st_float_box(struct dvm *v,float value) {
 dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Float"));
 union dvm_value a={.f=value},out={0};
 if(!r||!java_basics_call(v,"java/lang/Float","<init>","(F)V",r,&a,1,&out)||dvm_exception(v))return 0;
 return r;
}
static dvm_ref st_double_box(struct dvm *v,double value) {
 dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Double"));
 union dvm_value a={.d=value},out={0};
 if(!r||!java_basics_call(v,"java/lang/Double","<init>","(D)V",r,&a,1,&out)||dvm_exception(v))return 0;
 return r;
}

static void sqlite_selftest_fail(struct dvm *v,const char *stage,
 const char *expected){
 persistence_log("event=runtime_regression_failure stage=%s expected=%s exception=%s",stage,expected,sqlite_selftest_exception_name(v));
 sdk_trace_fprintf(stderr,
  "SQLITE_RUNTIME_SELFTEST FAIL stage=%s expected=%s actual=check_failed "
  "exception=%s\n",stage,expected,sqlite_selftest_exception_name(v));
}

#define SQLITE_ST_CHECK(stage_,expected_,expr_) do { \
 if(!(expr_)){ \
  stage=(stage_); \
  expected=(expected_); \
  goto fail; \
 } \
} while(0)

bool sqlite_runtime_selftest(struct dvm *v) {
 static const char nul_text[]={'a',0,'b'};
 static const unsigned char blob_bytes[]={0x00,0x7f,0x80,0xff};
 bool ok=false;
 const char *stage="begin";
 const char *expected="true";
 sdk_trace_fprintf(stderr,"SQLITE_RUNTIME_SELFTEST BEGIN stage=begin\n");
 dvm_ref db=sqlite_runtime_open_memory(v);
 if(!db){
  sdk_trace_fprintf(stderr,
   "SQLITE_RUNTIME_SELFTEST FAIL stage=open_memory expected=database "
   "actual=null exception=%s\n",sqlite_selftest_exception_name(v));
  return false;
 }
 dvm_pin(v,db);
 dvm_ref nul_string=dvm_new_string_n(v,nul_text,sizeof nul_text);
 dvm_ref empty_string=dvm_new_string(v,"");
 dvm_ref empty_blob=st_blob(v,NULL,0);
 dvm_ref blob=st_blob(v,blob_bytes,sizeof blob_bytes);
 dvm_ref integer=st_int(v,42);
 dvm_ref wide=st_long(v,INT64_C(1234567890123));
 dvm_ref boolean_true=st_bool(v,1);
 dvm_ref boolean_false=st_bool(v,0);
 dvm_ref row1_items[8]={0,0,0,0,0,0,0,0};
 row1_items[0]=dvm_new_string(v,"nul");
 row1_items[1]=nul_string;
 row1_items[2]=empty_blob;
 row1_items[3]=blob;
 row1_items[4]=0; /* SQL NULL */
 row1_items[5]=integer;
 row1_items[6]=wide;
 row1_items[7]=boolean_true;
 dvm_ref row1=st_args(v,8,row1_items);
 dvm_ref no_args=st_args(v,0,NULL);
 SQLITE_ST_CHECK("fixtures_row1","all_row1_objects_non_null",
  row1_items[0]&&nul_string&&empty_string&&empty_blob&&blob&&integer&&wide&&
  boolean_true&&boolean_false&&row1&&no_args);
 SQLITE_ST_CHECK("create_table","execSQL_ok",
  st_exec_ok(v,db,"CREATE TABLE r6_probe(k TEXT,s TEXT,empty BLOB,blob BLOB,n TEXT,i INTEGER,l INTEGER,b INTEGER)",no_args));
 SQLITE_ST_CHECK("insert_row1","execSQL_ok",
  st_exec_ok(v,db,"INSERT INTO r6_probe VALUES(?,?,?,?,?,?,?,?)",row1));
 dvm_ref row2_items[5]={0,0,0,0,0};
 row2_items[0]=dvm_new_string(v,"empty");
 row2_items[1]=empty_string;
 row2_items[2]=boolean_false;
 row2_items[3]=st_int(v,-7);
 row2_items[4]=st_long(v,INT64_C(9007199254740993));
 dvm_ref row2=st_args(v,5,row2_items);
 SQLITE_ST_CHECK("fixtures_row2","all_row2_objects_non_null",
  row2_items[0]&&row2_items[3]&&row2_items[4]&&row2);
 SQLITE_ST_CHECK("insert_row2","execSQL_ok",
  st_exec_ok(v,db,"INSERT INTO r6_probe(k,s,b,i,l) VALUES(?,?,?,?,?)",row2));
 dvm_ref cur=0;
 int ival=0;int64_t lval=0;bool isnull=false;dvm_ref str=0;
 SQLITE_ST_CHECK("query_count","query_ok_and_first_row",
  st_query(v,db,"SELECT COUNT(*) FROM r6_probe",0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("count_rows","count_equals_2",
  st_int_col(v,cur,0,&ival)&&ival==2);
 /* length(TEXT) stops at the first NUL by SQLite contract, so cast to BLOB
  * when the assertion is about stored UTF-8 byte count. */
 SQLITE_ST_CHECK("query_nul_row","query_ok_and_first_row",
  st_query(v,db,"SELECT typeof(s),length(CAST(s AS BLOB)),hex(s),s,typeof(empty),length(empty),hex(empty),typeof(blob),length(blob),hex(blob),typeof(n),n,n IS NULL,i,l,b FROM r6_probe WHERE k='nul'",0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("nul_typeof_text","typeof_s_equals_text",
  st_text_is(v,cur,0,"text"));
 SQLITE_ST_CHECK("nul_length_bytes","stored_utf8_length_equals_3",
  st_int_col(v,cur,1,&ival)&&ival==3);
 SQLITE_ST_CHECK("nul_hex","hex_equals_610062",
  st_text_is(v,cur,2,"610062"));
 SQLITE_ST_CHECK("nul_getstring","utf8_length_3_and_embedded_nul_preserved",
  st_string_col(v,cur,3,&str)&&str&&dvm_string_utf8_length(v,str)==3&&memcmp(dvm_string_utf8(v,str),nul_text,3)==0);
 SQLITE_ST_CHECK("empty_blob_typeof","typeof_empty_equals_blob",
  st_text_is(v,cur,4,"blob"));
 SQLITE_ST_CHECK("empty_blob_length","length_empty_equals_0",
  st_int_col(v,cur,5,&ival)&&ival==0);
 SQLITE_ST_CHECK("empty_blob_hex","hex_empty_equals_empty_string",
  st_text_is(v,cur,6,""));
 SQLITE_ST_CHECK("blob_typeof","typeof_blob_equals_blob",
  st_text_is(v,cur,7,"blob"));
 SQLITE_ST_CHECK("blob_length","length_blob_equals_4",
  st_int_col(v,cur,8,&ival)&&ival==4);
 SQLITE_ST_CHECK("blob_hex","hex_blob_equals_007F80FF",
  st_text_is(v,cur,9,"007F80FF"));
 SQLITE_ST_CHECK("null_typeof","typeof_n_equals_null",
  st_text_is(v,cur,10,"null"));
 SQLITE_ST_CHECK("null_is_null","n_is_null_true",
  st_is_null_col(v,cur,11,&isnull)&&isnull);
 SQLITE_ST_CHECK("blob_is_not_null","blob_is_null_false",
  st_is_null_col(v,cur,3,&isnull)&&!isnull);
 SQLITE_ST_CHECK("boolean_true_binding","b_equals_1",
  st_int_col(v,cur,12,&ival)&&ival==1);
 SQLITE_ST_CHECK("integer_binding","i_equals_42",
  st_long_col(v,cur,13,&lval)&&lval==42);
 SQLITE_ST_CHECK("long_binding","l_equals_1234567890123",
  st_long_col(v,cur,14,&lval)&&lval==INT64_C(1234567890123));
 SQLITE_ST_CHECK("boolean_true_long_binding","b_as_long_equals_1",
  st_long_col(v,cur,15,&lval)&&lval==1);
 SQLITE_ST_CHECK("query_empty_row","query_ok_and_first_row",
  st_query(v,db,"SELECT typeof(s),length(s),b,i,l,s FROM r6_probe WHERE k='empty'",0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("empty_string_typeof","typeof_s_equals_text",
  st_text_is(v,cur,0,"text"));
 SQLITE_ST_CHECK("empty_string_length","length_s_equals_0",
  st_int_col(v,cur,1,&ival)&&ival==0);
 SQLITE_ST_CHECK("empty_string_boolean","b_equals_0",
  st_long_col(v,cur,2,&lval)&&lval==0);
 SQLITE_ST_CHECK("negative_integer","i_equals_minus_7",
  st_long_col(v,cur,3,&lval)&&lval==-7);
 SQLITE_ST_CHECK("wide_long","l_equals_9007199254740993",
  st_long_col(v,cur,4,&lval)&&lval==INT64_C(9007199254740993));
 SQLITE_ST_CHECK("empty_string_getstring","utf8_length_0",
  st_string_col(v,cur,5,&str)&&str&&dvm_string_utf8_length(v,str)==0);
 /* The same NUL-preserving rule applies to rawQuery selection arguments. */
 dvm_ref sel_nul=dvm_new_string_n(v,nul_text,sizeof nul_text);
 dvm_ref sel_arr=dvm_new_array(v,'L',"Ljava/lang/String;",1);
 SQLITE_ST_CHECK("selection_args_alloc","selection_args_non_null",
  sel_nul&&sel_arr);
 ((dvm_ref*)dvm_array_data(v,sel_arr))[0]=sel_nul;
 SQLITE_ST_CHECK("query_selection_nul","query_ok_and_first_row",
  st_query(v,db,"SELECT ?",sel_arr,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("selection_nul_value","utf8_length_3_and_embedded_nul_preserved",
  st_string_col(v,cur,0,&str)&&str&&dvm_string_utf8_length(v,str)==3&&memcmp(dvm_string_utf8(v,str),nul_text,3)==0);
 /* Float/Double wrappers have no java_basics backend and must not be
  * reported as supported or silently coerced. */
 dvm_ref flt=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Float"));
 dvm_ref dbl=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Double"));
 dvm_ref fitem[1]={0},ditem[1]={0};fitem[0]=flt;ditem[0]=dbl;
 dvm_ref fargs=st_args(v,1,fitem),dargs=st_args(v,1,ditem);
 SQLITE_ST_CHECK("float_double_fixtures","float_double_args_non_null",
  flt&&dbl&&fargs&&dargs);
 SQLITE_ST_CHECK("float_reject","UnsupportedOperationException_not_implemented",
  st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",fargs,"java/lang/UnsupportedOperationException","not implemented"));
 SQLITE_ST_CHECK("double_reject","UnsupportedOperationException_not_implemented",
  st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",dargs,"java/lang/UnsupportedOperationException","not implemented"));
 /* Other boxed numbers are outside the implemented set: explicit mismatch. */
 dvm_ref shrt=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Short"));
 dvm_ref sitem[1]={0};sitem[0]=shrt;
 dvm_ref sargs=st_args(v,1,sitem);
 SQLITE_ST_CHECK("short_fixtures","short_args_non_null",shrt&&sargs);
 SQLITE_ST_CHECK("short_reject","SQLiteException_unsupported_bind_type",
  st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",sargs,"android/database/sqlite/SQLiteException","unsupported bind argument type at index 1: java/lang/Short"));
 /* Simulate a prefs-owned Boolean: class is java/lang/Boolean but the data
  * payload is not java_basics' PAY_BOOL layout. It must throw, not bind null. */
 struct dvm_class *bool_cls=dvm_find_class_or_external(v,"java/lang/Boolean");
 dvm_ref bad_bool=dvm_new_object(v,bool_cls);
 struct dvm_object *bad_obj=bad_bool?dvm__obj(v,bad_bool):NULL;
 int *bad_payload=bad_obj?calloc(1,sizeof *bad_payload):NULL;
 if(!bad_obj||!bad_payload){
  free(bad_payload);
  stage="bad_boolean_fixture";
  expected="bad_boolean_object_and_payload";
  goto fail;
 }
 bad_obj->data=bad_payload;bad_obj->data_destroy=free;
 dvm_ref bitem[1]={0};bitem[0]=bad_bool;
 dvm_ref bargs=st_args(v,1,bitem);
 SQLITE_ST_CHECK("bad_boolean_reject","IllegalStateException_payload_kind_mismatch",
  bargs&&st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",bargs,"java/lang/IllegalStateException","payload kind mismatch"));
 /* Android's two-argument execSQL rejects a null bindArgs array itself; a
  * non-null but short array is the bind-count check. */
 SQLITE_ST_CHECK("null_bindargs_reject","IllegalArgumentException_Empty_bindArgs",
  st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",0,"java/lang/IllegalArgumentException","Empty bindArgs"));
 SQLITE_ST_CHECK("bind_count_zero","SQLiteException_bind_count_mismatch",
  st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",no_args,"android/database/sqlite/SQLiteException","bind argument count mismatch"));
 dvm_ref two_items[2]={0,0};two_items[0]=dvm_new_string(v,"x");two_items[1]=dvm_new_string(v,"y");
 dvm_ref two=st_args(v,2,two_items);
 SQLITE_ST_CHECK("two_args_fixtures","two_args_non_null",
  two_items[0]&&two_items[1]&&two);
 SQLITE_ST_CHECK("bind_count_two","SQLiteException_bind_count_mismatch",
  st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",two,"android/database/sqlite/SQLiteException","bind argument count mismatch"));
 dvm_ref not_array=dvm_new_string(v,"not-an-array");
 SQLITE_ST_CHECK("non_array_args","IllegalArgumentException_object_array",
  not_array&&st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",not_array,"java/lang/IllegalArgumentException","object array"));
 /* execSQL is not a query API: a row-returning statement must not be stepped
  * to completion and reported as success. */
 SQLITE_ST_CHECK("execsql_nonquery_reject","SQLiteException_query_or_rawQuery",
  st_exec_rejected(v,db,"SELECT 1",no_args,"android/database/sqlite/SQLiteException","query or rawQuery"));
 /* Empty or comment-only SQL prepares to no statement; that is not success. */
 SQLITE_ST_CHECK("empty_statement_reject","SQLiteException_requires_statement",
  st_exec_rejected(v,db,"",no_args,"android/database/sqlite/SQLiteException","requires a SQL statement"));
 dvm_ref huge_items[1]={0};huge_items[0]=dvm_new_string(v,"x");
 dvm_ref huge=st_args(v,1,huge_items);
 struct dvm_object *huge_obj=huge?dvm__obj(v,huge):NULL;
 SQLITE_ST_CHECK("huge_args_fixtures","huge_args_non_null",
  huge_items[0]&&huge_obj);
 uint32_t saved_length=huge_obj->length;
 huge_obj->length=UINT32_MAX;
 bool huge_rejected=st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",huge,"java/lang/IllegalArgumentException","too large");
 huge_obj->length=saved_length;
 SQLITE_ST_CHECK("huge_args_reject","IllegalArgumentException_too_large",
  huge_rejected);
 /* String byte-length conversion guard: synthesize a >INT_MAX UTF-8 length
  * without allocating 2 GiB. The binder must reject before sqlite3_bind_*. */
 dvm_ref oversize=dvm_new_string(v,"oversize");
 dvm_ref oitem[1]={0};oitem[0]=oversize;
 dvm_ref oargs=st_args(v,1,oitem);
 struct dvm_object *oversize_obj=oversize?dvm__obj(v,oversize):NULL;
 SQLITE_ST_CHECK("oversize_string_fixtures","oversize_string_args_non_null",
  oversize_obj&&oargs);
 uint32_t saved_utf8_len=oversize_obj->utf8_len;
 oversize_obj->utf8_len=UINT32_MAX;
 bool oversize_rejected=st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",oargs,"java/lang/IllegalArgumentException","exceeds INT_MAX UTF-8 bytes");
 oversize_obj->utf8_len=saved_utf8_len;
 SQLITE_ST_CHECK("oversize_string_reject","IllegalArgumentException_exceeds_INT_MAX",
  oversize_rejected);
 /* byte[] length conversion guard: the binder must reject before it can pass
  * a wrapped negative int length to sqlite3_bind_blob. */
 dvm_ref big_blob=st_blob(v,NULL,0);
 dvm_ref big_item[1]={0};big_item[0]=big_blob;
 dvm_ref big_args=st_args(v,1,big_item);
 struct dvm_object *big_obj=big_blob?dvm__obj(v,big_blob):NULL;
 SQLITE_ST_CHECK("big_blob_fixtures","big_blob_args_non_null",
  big_obj&&big_args);
 uint32_t saved_blob_len=big_obj->length;
 big_obj->length=UINT32_MAX;
 bool blob_rejected=st_exec_rejected(v,db,"INSERT INTO r6_probe(k) VALUES(?)",big_args,"java/lang/IllegalArgumentException","byte[] bind argument 1 exceeds INT_MAX bytes");
 big_obj->length=saved_blob_len;
 SQLITE_ST_CHECK("big_blob_reject","IllegalArgumentException_exceeds_INT_MAX",
  blob_rejected);
 SQLITE_ST_CHECK("final_count_query","query_ok_and_first_row",
  st_query(v,db,"SELECT COUNT(*) FROM r6_probe",0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("final_count_value","count_equals_2",
  st_int_col(v,cur,0,&ival)&&ival==2);
 /* Nine-argument SQLiteDatabase.query: distinct/columns/selection/args/
  * groupBy/having/orderBy/limit combinations through a real Cursor. */
 dvm_ref q9_table=dvm_new_string(v,"q9_probe");
 dvm_ref q9_grp=dvm_new_string(v,"grp"),q9_val=dvm_new_string(v,"val"),q9_id=dvm_new_string(v,"id");
 dvm_ref q9_sum=dvm_new_string(v,"SUM(val)");
 dvm_ref q9_sel_grp=dvm_new_string(v,"grp=?"),q9_sel_id=dvm_new_string(v,"id=?");
 dvm_ref q9_having=dvm_new_string(v,"SUM(val)>20");
 dvm_ref q9_order_grp_desc=dvm_new_string(v,"grp DESC"),q9_order_id=dvm_new_string(v,"id DESC"),q9_order_id_asc=dvm_new_string(v,"id");
 dvm_ref q9_limit1=dvm_new_string(v,"1"),q9_limit2=dvm_new_string(v,"2");
 dvm_ref q9_a=dvm_new_string(v,"a"),q9_b=dvm_new_string(v,"b"),q9_arg3=dvm_new_string(v,"3");
 dvm_ref q9_grp_items[1]={q9_grp},q9_val_items[1]={q9_val};
 dvm_ref q9_id_val_items[2]={q9_id,q9_val},q9_grp_sum_items[2]={q9_grp,q9_sum},q9_null_col_items[2]={q9_grp,0};
 dvm_ref q9_col_grp=st_args(v,1,q9_grp_items),q9_col_val=st_args(v,1,q9_val_items);
 dvm_ref q9_col_id_val=st_args(v,2,q9_id_val_items),q9_col_grp_sum=st_args(v,2,q9_grp_sum_items);
 dvm_ref q9_col_empty=st_args(v,0,NULL),q9_col_null=st_args(v,2,q9_null_col_items);
 dvm_ref q9_arg_a_items[1]={q9_a},q9_arg_b_items[1]={q9_b},q9_arg_id_items[1]={q9_arg3};
 dvm_ref q9_args_a=st_args(v,1,q9_arg_a_items),q9_args_b=st_args(v,1,q9_arg_b_items);
 dvm_ref q9_args_id=st_args(v,1,q9_arg_id_items),q9_args_none=st_args(v,0,NULL);
 SQLITE_ST_CHECK("q9_fixtures","q9_objects_non_null",
  q9_table&&q9_grp&&q9_val&&q9_id&&q9_sum&&q9_sel_grp&&q9_sel_id&&q9_having&&
  q9_order_grp_desc&&q9_order_id&&q9_order_id_asc&&q9_limit1&&q9_limit2&&
  q9_a&&q9_b&&q9_arg3&&q9_col_grp&&q9_col_val&&q9_col_id_val&&q9_col_grp_sum&&
  q9_col_empty&&q9_col_null&&q9_args_a&&q9_args_b&&q9_args_id&&q9_args_none);
 SQLITE_ST_CHECK("q9_create","execSQL_ok",
  st_exec_ok(v,db,"CREATE TABLE q9_probe(id INTEGER,grp TEXT,val INTEGER)",no_args));
 SQLITE_ST_CHECK("q9_insert","execSQL_ok",
  st_exec_ok(v,db,"INSERT INTO q9_probe VALUES(1,'a',10),(2,'a',10),(3,'b',20),(4,'b',30),(5,'c',40)",no_args));
 /* columns=null -> *, selection + selectionArgs through rawQuery. */
 SQLITE_ST_CHECK("q9_star_selection","query_ok_and_first_row",
  st_query9(v,db,0,q9_table,0,q9_sel_id,q9_args_id,0,0,0,0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("q9_star_count","count_equals_1",st_count(v,cur,&ival)&&ival==1);
 SQLITE_ST_CHECK("q9_star_columns","column_count_3",st_col_count(v,cur,&ival)&&ival==3);
 SQLITE_ST_CHECK("q9_star_id","id_equals_3",st_int_col(v,cur,0,&ival)&&ival==3);
 SQLITE_ST_CHECK("q9_star_grp","grp_equals_b",st_text_is(v,cur,1,"b"));
 SQLITE_ST_CHECK("q9_star_val","val_equals_20",st_int_col(v,cur,2,&ival)&&ival==20);
 /* orderBy + limit without a selection. */
 SQLITE_ST_CHECK("q9_order_limit","query_ok_and_first_row",
  st_query9(v,db,0,q9_table,0,0,0,0,0,q9_order_id,q9_limit2,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("q9_order_limit_count","count_equals_2",st_count(v,cur,&ival)&&ival==2);
 SQLITE_ST_CHECK("q9_order_limit_first","id_equals_5",st_int_col(v,cur,0,&ival)&&ival==5);
 bool q9_moved=false;
 SQLITE_ST_CHECK("q9_order_limit_next","moveToNext_true",st_next(v,cur,&q9_moved)&&q9_moved);
 SQLITE_ST_CHECK("q9_order_limit_second","id_equals_4",st_int_col(v,cur,0,&ival)&&ival==4);
 /* DISTINCT columns and selection arguments. */
 SQLITE_ST_CHECK("q9_distinct_off","query_ok_and_first_row",
  st_query9(v,db,0,q9_table,q9_col_val,q9_sel_grp,q9_args_a,0,0,q9_order_id_asc,0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("q9_distinct_off_count","count_equals_2",st_count(v,cur,&ival)&&ival==2);
 SQLITE_ST_CHECK("q9_distinct_off_val","val_equals_10",st_int_col(v,cur,0,&ival)&&ival==10);
 SQLITE_ST_CHECK("q9_distinct_on","query_ok_and_first_row",
  st_query9(v,db,1,q9_table,q9_col_val,q9_sel_grp,q9_args_a,0,0,q9_order_id_asc,0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("q9_distinct_on_count","count_equals_1",st_count(v,cur,&ival)&&ival==1);
 SQLITE_ST_CHECK("q9_distinct_on_val","val_equals_10",st_int_col(v,cur,0,&ival)&&ival==10);
 /* groupBy + having + orderBy + limit over two result columns. */
 SQLITE_ST_CHECK("q9_group_having","query_ok_and_first_row",
  st_query9(v,db,0,q9_table,q9_col_grp_sum,0,0,q9_grp,q9_having,q9_order_grp_desc,q9_limit1,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("q9_group_having_count","count_equals_1",st_count(v,cur,&ival)&&ival==1);
 /* grp DESC leaves c (sum 40) first after HAVING; b (sum 50) is second. */
 SQLITE_ST_CHECK("q9_group_having_grp","grp_equals_c",st_text_is(v,cur,0,"c"));
 SQLITE_ST_CHECK("q9_group_having_sum","sum_equals_40",st_int_col(v,cur,1,&ival)&&ival==40);
 /* columns array empty is *, not a zero-column select. */
 SQLITE_ST_CHECK("q9_empty_columns","query_ok_and_first_row",
  st_query9(v,db,0,q9_table,q9_col_empty,0,0,0,0,0,0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("q9_empty_columns_count","count_equals_5",st_count(v,cur,&ival)&&ival==5);
 /* Fail-closed boundaries. */
 SQLITE_ST_CHECK("q9_null_table","IllegalArgumentException_table",
  st_query9_rejected(v,db,0,0,0,0,0,0,0,0,0,"java/lang/IllegalArgumentException","table"));
 SQLITE_ST_CHECK("q9_null_column","IllegalArgumentException_columns_contains_null",
  st_query9_rejected(v,db,0,q9_table,q9_col_null,0,0,0,0,0,0,"java/lang/IllegalArgumentException","null"));
 SQLITE_ST_CHECK("q9_columns_type","IllegalArgumentException_columns",
  st_query9_rejected(v,db,0,q9_table,q9_table,0,0,0,0,0,0,"java/lang/IllegalArgumentException","columns"));
 SQLITE_ST_CHECK("q9_arg_count","SQLiteException_mismatch",
  st_query9_rejected(v,db,0,q9_table,q9_col_grp,q9_sel_grp,q9_args_none,0,0,0,0,"android/database/sqlite/SQLiteException","mismatch"));
 char *q9_huge_text=malloc(SQLITE_QUERY_SQL_LIMIT+1);
 dvm_ref q9_huge=0;
 if(q9_huge_text){memset(q9_huge_text,'x',SQLITE_QUERY_SQL_LIMIT+1);q9_huge=dvm_new_string_n(v,q9_huge_text,SQLITE_QUERY_SQL_LIMIT+1);free(q9_huge_text);}
 SQLITE_ST_CHECK("q9_oversize_fixture","huge_selection_non_null",q9_huge);
 SQLITE_ST_CHECK("q9_oversize","IllegalArgumentException_exceeds",
  st_query9_rejected(v,db,0,q9_table,0,q9_huge,0,0,0,0,0,"java/lang/IllegalArgumentException","exceeds"));
 /* SQLiteDatabase.replace is AOSP insertWithOnConflict(CONFLICT_REPLACE):
  * the same primary key is overwritten instead of duplicated, the affected
  * row id is returned, and a SQL failure is reported as -1 (not fabricated
  * success). */
 dvm_ref rp_table=dvm_new_string(v,"replace_probe");
 dvm_ref rp_missing_table=dvm_new_string(v,"replace_probe_missing");
 dvm_ref rp_key_id=dvm_new_string(v,"id"),rp_key_name=dvm_new_string(v,"name"),rp_key_hits=dvm_new_string(v,"hits");
 dvm_ref rp_first=dvm_new_string(v,"first"),rp_second=dvm_new_string(v,"second"),rp_hack=dvm_new_string(v,"name");
 dvm_ref rp_cv_first=st_values_new(v),rp_cv_second=st_values_new(v),rp_cv_empty=st_values_new(v);
 dvm_ref rp_int1=st_int(v,1),rp_int2=st_int(v,2);
 dvm_ref rp_byte=st_byte_box(v,7),rp_short=st_short_box(v,300);
 dvm_ref rp_float=st_float_box(v,1.5f),rp_double=st_double_box(v,2.25);
 SQLITE_ST_CHECK("replace_fixtures","replace_fixtures_non_null",
  rp_table&&rp_missing_table&&rp_key_id&&rp_key_name&&rp_key_hits&&rp_first&&rp_second&&rp_hack&&
  rp_cv_first&&rp_cv_second&&rp_cv_empty&&rp_int1&&rp_int2&&rp_byte&&rp_short&&rp_float&&rp_double);
 SQLITE_ST_CHECK("replace_create","execSQL_ok",
  st_exec_ok(v,db,"CREATE TABLE replace_probe(id INTEGER PRIMARY KEY,name TEXT,hits INTEGER)",no_args));
 SQLITE_ST_CHECK("replace_put_first","put_ok",
  st_values_put(v,rp_cv_first,rp_key_id,"(Ljava/lang/String;Ljava/lang/Integer;)V",rp_int1)&&
  st_values_put(v,rp_cv_first,rp_key_name,"(Ljava/lang/String;Ljava/lang/String;)V",rp_first)&&
  st_values_put(v,rp_cv_first,rp_key_hits,"(Ljava/lang/String;Ljava/lang/Integer;)V",rp_int1));
 int64_t rp_rowid=0;
 SQLITE_ST_CHECK("replace_insert_first","rowid_equals_1",
  st_replace(v,db,rp_table,0,rp_cv_first,&rp_rowid)&&rp_rowid==1);
 SQLITE_ST_CHECK("replace_first_count","count_1_name_first_hits_1",
  st_query(v,db,"SELECT COUNT(*),name,hits FROM replace_probe WHERE id=1",0,&cur)&&st_first(v,cur)&&
  st_int_col(v,cur,0,&ival)&&ival==1&&st_text_is(v,cur,1,"first")&&st_int_col(v,cur,2,&ival)&&ival==1);
 SQLITE_ST_CHECK("replace_put_second","put_ok",
  st_values_put(v,rp_cv_second,rp_key_id,"(Ljava/lang/String;Ljava/lang/Integer;)V",rp_int1)&&
  st_values_put(v,rp_cv_second,rp_key_name,"(Ljava/lang/String;Ljava/lang/String;)V",rp_second)&&
  st_values_put(v,rp_cv_second,rp_key_hits,"(Ljava/lang/String;Ljava/lang/Integer;)V",rp_int2));
 SQLITE_ST_CHECK("replace_overwrite","rowid_equals_1",
  st_replace(v,db,rp_table,0,rp_cv_second,&rp_rowid)&&rp_rowid==1);
 SQLITE_ST_CHECK("replace_overwrite_count","count_1_name_second_hits_2",
  st_query(v,db,"SELECT COUNT(*),name,hits FROM replace_probe WHERE id=1",0,&cur)&&st_first(v,cur)&&
  st_int_col(v,cur,0,&ival)&&ival==1&&st_text_is(v,cur,1,"second")&&st_int_col(v,cur,2,&ival)&&ival==2);
 SQLITE_ST_CHECK("replace_missing_table","rowid_minus_1_no_exception",
  st_replace(v,db,rp_missing_table,0,rp_cv_first,&rp_rowid)&&rp_rowid==-1&&dvm_exception(v)==0);
 SQLITE_ST_CHECK("replace_empty_hack","rowid_equals_2",
  st_replace(v,db,rp_table,rp_hack,rp_cv_empty,&rp_rowid)&&rp_rowid==2);
 SQLITE_ST_CHECK("replace_empty_hack_count","count_2",
  st_query(v,db,"SELECT COUNT(*) FROM replace_probe",0,&cur)&&st_first(v,cur)&&st_int_col(v,cur,0,&ival)&&ival==2);
 SQLITE_ST_CHECK("replace_empty_null_hack","rowid_minus_1_no_exception",
  st_replace(v,db,rp_table,0,rp_cv_empty,&rp_rowid)&&rp_rowid==-1&&dvm_exception(v)==0);
 /* ContentValues numbers bind per AOSP DatabaseUtils.bindObjectToProgram:
  * Byte/Short as INTEGER and Float/Double as REAL. */
 dvm_ref rp_types=dvm_new_string(v,"replace_types");
 dvm_ref rp_key_b=dvm_new_string(v,"b"),rp_key_s=dvm_new_string(v,"s");
 dvm_ref rp_key_f=dvm_new_string(v,"f"),rp_key_d=dvm_new_string(v,"d");
 dvm_ref rp_cv_types=st_values_new(v);
 SQLITE_ST_CHECK("replace_types_fixtures","replace_types_non_null",
  rp_types&&rp_key_b&&rp_key_s&&rp_key_f&&rp_key_d&&rp_cv_types);
 SQLITE_ST_CHECK("replace_types_create","execSQL_ok",
  st_exec_ok(v,db,"CREATE TABLE replace_types(id INTEGER PRIMARY KEY,b INTEGER,s INTEGER,f REAL,d REAL)",no_args));
 SQLITE_ST_CHECK("replace_types_put","put_ok",
  st_values_put(v,rp_cv_types,rp_key_id,"(Ljava/lang/String;Ljava/lang/Integer;)V",rp_int1)&&
  st_values_put(v,rp_cv_types,rp_key_b,"(Ljava/lang/String;Ljava/lang/Byte;)V",rp_byte)&&
  st_values_put(v,rp_cv_types,rp_key_s,"(Ljava/lang/String;Ljava/lang/Short;)V",rp_short)&&
  st_values_put(v,rp_cv_types,rp_key_f,"(Ljava/lang/String;Ljava/lang/Float;)V",rp_float)&&
  st_values_put(v,rp_cv_types,rp_key_d,"(Ljava/lang/String;Ljava/lang/Double;)V",rp_double));
 SQLITE_ST_CHECK("replace_types_insert","rowid_equals_1",
  st_replace(v,db,rp_types,0,rp_cv_types,&rp_rowid)&&rp_rowid==1);
 double rp_dval=0;
 SQLITE_ST_CHECK("replace_types_query","query_ok_and_first_row",
  st_query(v,db,"SELECT typeof(b),typeof(s),typeof(f),typeof(d),b,s,f,d FROM replace_types WHERE id=1",0,&cur)&&st_first(v,cur));
 SQLITE_ST_CHECK("replace_byte_type","typeof_b_integer",st_text_is(v,cur,0,"integer"));
 SQLITE_ST_CHECK("replace_short_type","typeof_s_integer",st_text_is(v,cur,1,"integer"));
 SQLITE_ST_CHECK("replace_float_type","typeof_f_real",st_text_is(v,cur,2,"real"));
 SQLITE_ST_CHECK("replace_double_type","typeof_d_real",st_text_is(v,cur,3,"real"));
 SQLITE_ST_CHECK("replace_byte_value","b_equals_7",st_int_col(v,cur,4,&ival)&&ival==7);
 SQLITE_ST_CHECK("replace_short_value","s_equals_300",st_int_col(v,cur,5,&ival)&&ival==300);
 SQLITE_ST_CHECK("replace_float_value","f_equals_1_5",st_double_col(v,cur,6,&rp_dval)&&rp_dval==1.5);
 SQLITE_ST_CHECK("replace_double_value","d_equals_2_25",st_double_col(v,cur,7,&rp_dval)&&rp_dval==2.25);
 /* update uses its own non-authentication table; SET bindings precede WHERE
  * bindings, NUL-containing text survives and SQL errors are not swallowed. */
 dvm_ref up_table=dvm_new_string(v,"update_probe"),up_where=dvm_new_string(v,"id=?");
 dvm_ref up_key_t=dvm_new_string(v,"t"),up_key_n=dvm_new_string(v,"n");
 dvm_ref up_values=st_values_new(v),up_all=st_values_new(v),up_constraint=st_values_new(v);
 dvm_ref up_args=dvm_new_array(v,'L',"Ljava/lang/String;",1);
 dvm_ref up_one=dvm_new_string(v,"1"),up_missing=dvm_new_string(v,"999"),up_occupied=dvm_new_string(v,"occupied");
 SQLITE_ST_CHECK("update_fixtures","fixtures_non_null",up_table&&up_where&&up_key_t&&up_key_n&&up_values&&up_all&&up_constraint&&up_args&&up_one&&up_missing&&up_occupied);
 SQLITE_ST_CHECK("update_create","execSQL_ok",st_exec_ok(v,db,"CREATE TABLE update_probe(id INTEGER PRIMARY KEY,t TEXT UNIQUE,n INTEGER)",no_args)&&st_exec_ok(v,db,"INSERT INTO update_probe VALUES(1,'original',1),(2,'occupied',2)",no_args));
 SQLITE_ST_CHECK("update_values","put_ok",st_values_put(v,up_values,up_key_t,"(Ljava/lang/String;Ljava/lang/String;)V",nul_string)&&st_values_put(v,up_values,up_key_n,"(Ljava/lang/String;Ljava/lang/String;)V",0)&&st_values_put(v,up_all,up_key_n,"(Ljava/lang/String;Ljava/lang/Integer;)V",integer)&&st_values_put(v,up_constraint,up_key_t,"(Ljava/lang/String;Ljava/lang/String;)V",up_occupied));
 ((dvm_ref*)dvm_array_data(v,up_args))[0]=up_one;
 SQLITE_ST_CHECK("update_one","changed_1",st_update(v,db,up_table,up_values,up_where,up_args,&ival)&&ival==1);
 SQLITE_ST_CHECK("update_bound_query","fixture_row",st_query(v,db,"SELECT hex(t),n IS NULL FROM update_probe WHERE id=1",0,&cur)&&st_first(v,cur));
 dvm_ref up_hex=0;SQLITE_ST_CHECK("update_bound_columns","fixture_columns",st_string_col(v,cur,0,&up_hex)&&st_int_col(v,cur,1,&ival));
 persistence_log("event=update_fixture_values hex=%.*s sql_null=%d auth_fixture=0",20,dvm_string_utf8(v,up_hex),ival);
 SQLITE_ST_CHECK("update_bound_values","nul_text_and_SQL_NULL",st_text_is(v,cur,0,"610062")&&ival==1);
 ((dvm_ref*)dvm_array_data(v,up_args))[0]=up_missing;
 SQLITE_ST_CHECK("update_zero","changed_0",st_update(v,db,up_table,up_values,up_where,up_args,&ival)&&ival==0);
 SQLITE_ST_CHECK("update_all","null_where_changed_2",st_update(v,db,up_table,up_all,0,0,&ival)&&ival==2);
 SQLITE_ST_CHECK("update_empty_where","empty_where_changed_2",st_update(v,db,up_table,up_all,empty_string,0,&ival)&&ival==2);
 SQLITE_ST_CHECK("update_all_values","both_rows_42",st_query(v,db,"SELECT COUNT(*) FROM update_probe WHERE n=42",0,&cur)&&st_first(v,cur)&&st_int_col(v,cur,0,&ival)&&ival==2);
 ((dvm_ref*)dvm_array_data(v,up_args))[0]=up_one;
 SQLITE_ST_CHECK("update_constraint_error","SQLiteException_not_swallowed",st_update_rejected(v,db,up_table,up_constraint,up_where,up_args,"android/database/sqlite/SQLiteException","UNIQUE"));
 SQLITE_ST_CHECK("update_error_atomic","row_unchanged",st_query(v,db,"SELECT hex(t),n FROM update_probe WHERE id=1",0,&cur)&&st_first(v,cur)&&st_text_is(v,cur,0,"610062")&&st_int_col(v,cur,1,&ival)&&ival==42);
 SQLITE_ST_CHECK("update_missing_table","SQLiteException_not_swallowed",st_update_rejected(v,db,rp_missing_table,up_all,0,0,"android/database/sqlite/SQLiteException","no such table"));
 SQLITE_ST_CHECK("update_null_table","IllegalArgumentException",st_update_rejected(v,db,0,up_all,0,0,"java/lang/IllegalArgumentException","table"));
 SQLITE_ST_CHECK("update_null_values","IllegalArgumentException",st_update_rejected(v,db,up_table,0,0,0,"java/lang/IllegalArgumentException","Empty values"));
 SQLITE_ST_CHECK("update_empty_values","IllegalArgumentException",st_update_rejected(v,db,up_table,rp_cv_empty,0,0,"java/lang/IllegalArgumentException","Empty values"));
 SQLITE_ST_CHECK("update_arg_mismatch","SQLiteException",st_update_rejected(v,db,up_table,up_all,up_where,0,"android/database/sqlite/SQLiteException","mismatch"));
 SQLITE_ST_CHECK("update_extra_arg","SQLiteException",st_update_rejected(v,db,up_table,up_all,0,up_args,"android/database/sqlite/SQLiteException","mismatch"));
 SQLITE_ST_CHECK("update_args_type","IllegalArgumentException",st_update_rejected(v,db,up_table,up_all,up_where,up_one,"java/lang/IllegalArgumentException","String array"));
 ((dvm_ref*)dvm_array_data(v,up_args))[0]=integer;
 SQLITE_ST_CHECK("update_arg_element","IllegalArgumentException",st_update_rejected(v,db,up_table,up_all,up_where,up_args,"java/lang/IllegalArgumentException","element"));
 ((dvm_ref*)dvm_array_data(v,up_args))[0]=0;
 SQLITE_ST_CHECK("update_null_where_arg","changed_0",st_update(v,db,up_table,up_all,up_where,up_args,&ival)&&ival==0);
 struct dvm_object *up_array=dvm__obj(v,up_args);uint32_t up_saved_length=up_array->length;up_array->length=UINT32_MAX;
 bool up_bounds=st_update_rejected(v,db,up_table,up_all,up_where,up_args,"java/lang/IllegalArgumentException","too many");up_array->length=up_saved_length;
 SQLITE_ST_CHECK("update_array_bounds","IllegalArgumentException_before_access",up_bounds);
 SQLITE_ST_CHECK("update_sql_bound","IllegalArgumentException",st_update_rejected(v,db,up_table,up_all,q9_huge,0,"java/lang/IllegalArgumentException","exceeds"));
 SQLITE_ST_CHECK("final_pending_exception","no_pending_exception",
  dvm_exception(v)==0);
 /* SQLiteOpenHelper.getDatabaseName returns the exact constructor name; a
  * null constructor name stays null (in-memory helper), never a fabricated
  * default string. */
 struct dvm_class *helper_cls=dvm_find_class_or_external(v,"android/database/sqlite/SQLiteOpenHelper");
 struct dvm_class *helper_ctx_cls=dvm_find_class_or_external(v,"android/content/Context");
 dvm_ref helper_ctx=helper_ctx_cls?dvm_new_object(v,helper_ctx_cls):0;
 dvm_ref helper=helper_cls?dvm_new_object(v,helper_cls):0;
 dvm_ref helper_name=dvm_new_string(v,"neuron_report_data.db");
 union dvm_value helper_args[4]={{.l=helper_ctx},{.l=helper_name},{.l=0},{.i=3}},helper_out={0};
 bool helper_init=helper_ctx&&helper&&helper_name&&sqlite_runtime_call(v,"android/database/sqlite/SQLiteOpenHelper","<init>","(Landroid/content/Context;Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;I)V",helper,helper_args,&helper_out);
 SQLITE_ST_CHECK("helper_database_name_init","helper_constructed",helper_init&&!dvm_exception(v));
 bool helper_name_ok=helper_init&&sqlite_runtime_call(v,"android/database/sqlite/SQLiteOpenHelper","getDatabaseName","()Ljava/lang/String;",helper,NULL,&helper_out)&&helper_out.l==helper_name&&dvm_string_utf8(v,helper_out.l)&&!strcmp(dvm_string_utf8(v,helper_out.l),"neuron_report_data.db");
 SQLITE_ST_CHECK("helper_database_name_value","constructor_name",helper_name_ok&&!dvm_exception(v));
 dvm_ref memory_helper=helper_cls?dvm_new_object(v,helper_cls):0;
 union dvm_value memory_args[4]={{.l=helper_ctx},{.l=0},{.l=0},{.i=1}};
 bool memory_init=helper_ctx&&memory_helper&&sqlite_runtime_call(v,"android/database/sqlite/SQLiteOpenHelper","<init>","(Landroid/content/Context;Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;I)V",memory_helper,memory_args,&helper_out);
 bool memory_name_ok=memory_init&&sqlite_runtime_call(v,"android/database/sqlite/SQLiteOpenHelper","getDatabaseName","()Ljava/lang/String;",memory_helper,NULL,&helper_out)&&!helper_out.l;
 SQLITE_ST_CHECK("helper_database_name_null","null_name_stays_null",memory_name_ok&&!dvm_exception(v));
 sdk_trace_fprintf(stderr,"SQLITE_RUNTIME_SELFTEST PASS embedded_nul=1 empty_blob=1 blob=1 null=1 empty_string=1 integer=1 long=1 boolean=1 float_rejected=1 double_rejected=1 bool_payload_rejected=1 arg_count=1 array_bounds=1 string_length_guard=1 blob_length_guard=1 unsupported_type=1 null_bindargs=1 execsql_nonquery=1 empty_statement=1 helper_database_name=1 query9=1 query9_limits=1 replace=1 replace_types=1 update=1 update_bounds=1 update_errors=1\n");
 ok=true;
 goto done;
fail:
 sqlite_selftest_fail(v,stage,expected);
done:
 dvm_unpin(v,db);
 return ok;
}

/* Separate from SDK users: non-secret disk fixture, plus a read-only audit
 * through the ORIGINAL guest helper and login-state reader. Never seeds auth. */
void sqlite_runtime_persistence_probe(struct dvm *v,dvm_ref context){
 dvm_ref saved=v->exception;uint64_t steps=v->call_steps;bool parked=v->parked;
 v->exception=0;v->call_steps=0;v->parked=false;
 dvm_ref db=0,c=0,h=0;union dvm_value q={0};const char *stage="context_open";int previous=-1,current=-1;
 char path[PATH_MAX]={0};
#define P_CHECK(x) do{if(!(x)||v->exception)goto finish;}while(0)
 stage="content_values_regression";bool values_regression=android_util_selftest(v);persistence_log("event=content_values_regression ok=%d",values_regression);P_CHECK(values_regression);
 stage="runtime_regression";bool regression=sqlite_runtime_selftest(v);persistence_log("event=runtime_regression ok=%d",regression);P_CHECK(regression);
 stage="context_open";
 union dvm_value args[4]={{.l=dvm_new_string(v,"__task6n_sqlite_probe.db")},{.i=0},{.l=0},{.l=0}};
 P_CHECK(sqlite_runtime_call(v,"android/content/Context","openOrCreateDatabase","(Ljava/lang/String;ILandroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;",context,args,&q)&&q.l);db=q.l;dvm_pin(v,db);
 P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","getPath","()Ljava/lang/String;",db,NULL,&q));snprintf(path,sizeof path,"%s",dvm_string_utf8(v,q.l));
 stage="create_fixture";args[0].l=dvm_new_string(v,"CREATE TABLE IF NOT EXISTS probe(id INTEGER PRIMARY KEY, runs INTEGER NOT NULL, text TEXT NOT NULL)");
 P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","execSQL","(Ljava/lang/String;)V",db,args,&q));
 P_CHECK(st_query(v,db,"SELECT COALESCE(MAX(runs),0) FROM probe",0,&c)&&st_first(v,c)&&st_int_col(v,c,0,&previous));
 sqlite_runtime_call(v,"android/database/Cursor","close","()V",c,NULL,&q);c=0;
 stage="write_fixture";char sql[256];snprintf(sql,sizeof sql,"INSERT OR REPLACE INTO probe VALUES(1,%d,'\xe6\x8c\x81\xe4\xb9\x85\xe5\x8c\x96\xf0\x9f\x98\x80')",previous+1);args[0].l=dvm_new_string(v,sql);
 P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","execSQL","(Ljava/lang/String;)V",db,args,&q));
 sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","close","()V",db,NULL,&q);dvm_unpin(v,db);db=0;
 stage="static_reopen";args[0].l=dvm_new_string(v,path);args[1].l=0;
 P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","openOrCreateDatabase","(Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;",0,args,&q)&&q.l);db=q.l;dvm_pin(v,db);
 P_CHECK(st_query(v,db,"SELECT runs,text FROM probe WHERE id=1",0,&c)&&st_first(v,c)&&st_int_col(v,c,0,&current)&&current==previous+1&&st_text_is(v,c,1,"\xe6\x8c\x81\xe4\xb9\x85\xe5\x8c\x96\xf0\x9f\x98\x80"));
 sqlite_runtime_call(v,"android/database/Cursor","close","()V",c,NULL,&q);c=0;
 sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","close","()V",db,NULL,&q);dvm_unpin(v,db);db=0;
 persistence_log("event=probe_pass previous=%d current=%d path=%s utf8=1 reopen=1 auth_fixture=0",previous,current,path);
 stage="file_overload";args[0].l=dvm_new_string(v,"__task6n_sqlite_probe.db");
 P_CHECK(file_runtime_call(v,"android/content/Context","getDatabasePath","(Ljava/lang/String;)Ljava/io/File;",context,args,1,&q)&&q.l);args[0].l=q.l;args[1].l=0;
 P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","openOrCreateDatabase","(Ljava/io/File;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;",0,args,&q)&&q.l);db=q.l;dvm_pin(v,db);
 P_CHECK(st_query(v,db,"SELECT runs FROM probe WHERE id=1",0,&c)&&st_first(v,c)&&st_int_col(v,c,0,&current)&&current==previous+1);
 sqlite_runtime_call(v,"android/database/Cursor","close","()V",c,NULL,&q);c=0;
 sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","close","()V",db,NULL,&q);
 P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","isOpen","()Z",db,NULL,&q)&&!q.i);dvm_unpin(v,db);db=0;
 stage="invalid_private_path";args[0].l=dvm_new_string(v,"/data/local/tmp/task6n-not-allowed.db");
 if(!sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","openOrCreateDatabase","(Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;",0,args,&q))goto finish;
 if(!v->exception||strcmp(sqlite_selftest_exception_name(v),"android/database/sqlite/SQLiteException"))goto finish;v->exception=0;
 stage="memory_database";db=sqlite_runtime_open_memory(v);P_CHECK(db);dvm_pin(v,db);
 P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","getPath","()Ljava/lang/String;",db,NULL,&q)&&!strcmp(dvm_string_utf8(v,q.l),":memory:"));
 sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","close","()V",db,NULL,&q);dvm_unpin(v,db);db=0;
 persistence_log("event=probe_boundaries_pass file_overload=1 closed_isOpen_false=1 outside_private_rejected=1 memory_path=1");
 stage="original_helper_constructor";
 struct dvm_class *cls=dvm_find_class(v,"com/gsc/base/db/c");P_CHECK(cls);h=dvm_new_object(v,cls);P_CHECK(h);dvm_pin(v,h);
 struct dvm_method *method=dvm_find_method(v,cls,"<init>","(Landroid/content/Context;)V");args[0].l=context;P_CHECK(method&&dvm_call(v,method,h,args,1,&q));
 stage="original_helper_getWritableDatabase";P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteOpenHelper","getWritableDatabase","()Landroid/database/sqlite/SQLiteDatabase;",h,NULL,&q)&&q.l);db=q.l;dvm_pin(v,db);
 stage="users_metadata";P_CHECK(st_query(v,db,"SELECT COUNT(*),COALESCE(SUM(length(access_key)>0 AND length(uid)>0 AND length(info)>0),0) FROM users",0,&c)&&st_first(v,c));
 int rows=0,eligible=0;P_CHECK(st_int_col(v,c,0,&rows)&&st_int_col(v,c,1,&eligible));
 persistence_log("event=users_metadata rows=%d token_uid_info_present=%d original_schema=1",rows,eligible);
 sqlite_runtime_call(v,"android/database/Cursor","close","()V",c,NULL,&q);c=0;
 P_CHECK(sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","getPath","()Ljava/lang/String;",db,NULL,&q));
 char users_path[PATH_MAX];snprintf(users_path,sizeof users_path,"%s",dvm_string_utf8(v,q.l));struct stat disk;
 persistence_log("event=users_file path=%s bytes=%lld",users_path,stat(users_path,&disk)?-1LL:(long long)disk.st_size);
 P_CHECK(st_query(v,db,"SELECT COUNT(*) FROM pragma_table_info('users')",0,&c)&&st_first(v,c));int columns=0;P_CHECK(st_int_col(v,c,0,&columns));
 persistence_log("event=users_schema columns=%d access_key_uid_info_query_ok=1",columns);
 sqlite_runtime_call(v,"android/database/Cursor","close","()V",c,NULL,&q);c=0;
 char *suffix=strstr(users_path,"/databases/users.db");if(suffix){*suffix=0;char blkv_path[PATH_MAX];snprintf(blkv_path,sizeof blkv_path,"%s/app_blkv/rpc_info.blkv",users_path);int rc=stat(blkv_path,&disk),why=errno;persistence_log("event=blkv_file path=%s exists=%d bytes=%lld errno=%d",blkv_path,rc==0,rc?-1LL:(long long)disk.st_size,rc?why:0);}
 stage="original_isLogin";cls=dvm_find_class(v,"com/gsc/base/utils/UserInfoUtils");method=cls?dvm_find_method(v,cls,"isLogin","()Lcom/gsc/base/model/UserInfoModel;"):NULL;P_CHECK(method&&dvm_call(v,method,0,NULL,0,&q));
 persistence_log("event=original_isLogin model_present=%d original_dispatch=1",q.l!=0);stage="pass";
finish:
 persistence_log("event=probe_end stage=%s exception=%s previous=%d current=%d",stage,sqlite_selftest_exception_name(v),previous,current);
 v->exception=0;if(c)sqlite_runtime_call(v,"android/database/Cursor","close","()V",c,NULL,&q);
 if(db){sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","close","()V",db,NULL,&q);dvm_unpin(v,db);}if(h)dvm_unpin(v,h);
 v->exception=saved;v->call_steps=steps;v->parked=parked;
#undef P_CHECK
}

/* Isolated HAP probe: no access to the user's database or account fields. */
void sqlite_runtime_column_name_selftest(struct dvm *v){
 dvm_ref saved=v->exception;uint64_t steps=v->call_steps;bool parked=v->parked;
 v->exception=0;v->call_steps=0;v->parked=false;
 dvm_ref db=sqlite_runtime_open_memory(v),c=0;union dvm_value q={0},arg={.i=0};
 int name_ok=0,low_ok=0,high_ok=0,closed_ok=0;
 if(db){dvm_pin(v,db);if(st_query(v,db,"SELECT 7 AS column_probe WHERE 0",0,&c)&&c){
  dvm_pin(v,c);
  bool handled=sqlite_runtime_call(v,"android/database/Cursor","getColumnName","(I)Ljava/lang/String;",c,&arg,&q);
  const char *name=q.l?dvm_string_utf8(v,q.l):NULL;name_ok=handled&&!v->exception&&name&&!strcmp(name,"column_probe");
  dvm_clear_exception(v);arg.i=-1;
  handled=sqlite_runtime_call(v,"android/database/Cursor","getColumnName","(I)Ljava/lang/String;",c,&arg,&q);
  low_ok=handled&&v->exception&&!strcmp(sqlite_selftest_exception_name(v),"android/database/CursorIndexOutOfBoundsException");
  dvm_clear_exception(v);arg.i=1;
  handled=sqlite_runtime_call(v,"android/database/Cursor","getColumnName","(I)Ljava/lang/String;",c,&arg,&q);
  high_ok=handled&&v->exception&&!strcmp(sqlite_selftest_exception_name(v),"android/database/CursorIndexOutOfBoundsException");
  dvm_clear_exception(v);sqlite_runtime_call(v,"android/database/Cursor","close","()V",c,NULL,&q);arg.i=0;
  handled=sqlite_runtime_call(v,"android/database/Cursor","getColumnName","(I)Ljava/lang/String;",c,&arg,&q);
  closed_ok=handled&&v->exception&&!strcmp(sqlite_selftest_exception_name(v),"java/lang/IllegalStateException");
  dvm_clear_exception(v);dvm_unpin(v,c);
 }dvm_clear_exception(v);sqlite_runtime_call(v,"android/database/sqlite/SQLiteDatabase","close","()V",db,NULL,&q);dvm_unpin(v,db);}
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/captcha-home-cursor-selftest.txt","w");
 if(f){fprintf(f,"pid=%d empty_rows_name=%d negative_index_exception=%d upper_index_exception=%d closed_exception=%d user_database_accessed=0 result=%s\n",getpid(),name_ok,low_ok,high_ok,closed_ok,name_ok&&low_ok&&high_ok&&closed_ok?"PASS":"FAIL");fclose(f);}
 dvm_clear_exception(v);v->exception=saved;v->call_steps=steps;v->parked=parked;
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int sqlite_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;if(o->data_destroy==db_destroy||o->data_destroy==cur_destroy)return DVM_GC_PAYLOAD_ATOMIC;
 if(o->data_destroy==helper_destroy){helper *p=o->data;if(visit){visit(&p->db,user);visit(&p->context,user);visit(&p->name,user);}return DVM_GC_PAYLOAD_REFS;}return DVM_GC_PAYLOAD_UNKNOWN;
}
