#include "sqlite_runtime.h"
#include "sqlite3.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct { sqlite3 *db; int refs; } shared;
typedef struct { shared *s; int closed; } database;
typedef struct { shared *s; sqlite3_stmt *stmt; sqlite3_value **cells; char **names; int cols, pos, count, closed; } cursor;
typedef struct { int version; dvm_ref db; struct dvm *vm; } helper;
static void helper_destroy(void *p) { helper *h=p; if(h){if(h->db)dvm_unpin(h->vm,h->db);free(h);} }
static void release(shared *s) { if (s && !--s->refs) { sqlite3_close_v2(s->db); free(s); } }
static void db_destroy(void *p) { database *d=p; if(d){release(d->s);free(d);} }
static void cur_destroy(void *p) { cursor *c=p; if(c){sqlite3_finalize(c->stmt);for(size_t i=0;i<(size_t)c->count*(size_t)c->cols;i++)sqlite3_value_free(c->cells[i]);free(c->cells);if(c->names)for(int i=0;i<c->cols;i++)free(c->names[i]);free(c->names);release(c->s);free(c);} }
static void error(struct dvm *v, shared *s) { dvm__throw(v,"android/database/sqlite/SQLiteException","%s",s?sqlite3_errmsg(s->db):"SQLite failure"); }
#define IS(n,s) (!strcmp(m,n)&&!strcmp(sig,s))
bool sqlite_runtime_call(struct dvm *v,const char *cl,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,union dvm_value *out) {
 struct dvm_object *o=dvm__obj(v,self);
 if(!strcmp(cl,"android/database/sqlite/SQLiteOpenHelper")) {
  if(IS("<init>","(Landroid/content/Context;Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;I)V")) {
   if(!o||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","helper context");return true;}
   if(a[2].l){dvm__throw(v,"java/lang/UnsupportedOperationException","CursorFactory unsupported");return true;}
   if(a[3].i<1){dvm__throw(v,"java/lang/IllegalArgumentException","version must be positive");return true;}
   helper *h=calloc(1,sizeof *h);if(!h){dvm__throw(v,"java/lang/OutOfMemoryError","helper");return true;}h->vm=v;h->version=a[3].i;o->data=h;o->data_destroy=helper_destroy;return true;
  }
  helper *h=o?o->data:NULL;if(!h){dvm__throw(v,"java/lang/IllegalStateException","helper uninitialized");return true;}
  if(IS("close","()V")){if(h->db){struct dvm_object *dbo=dvm__obj(v,h->db);database *d=dbo?dbo->data:NULL;if(d)d->closed=1;dvm_unpin(v,h->db);h->db=0;}return true;}
  if(IS("getWritableDatabase","()Landroid/database/sqlite/SQLiteDatabase;")||IS("getReadableDatabase","()Landroid/database/sqlite/SQLiteDatabase;")) {
   if(!h->db){extern dvm_ref sqlite_runtime_open_memory(struct dvm *);dvm_ref ref=sqlite_runtime_open_memory(v);if(!ref){dvm__throw(v,"android/database/sqlite/SQLiteException","cannot open memory database");return true;}dvm_pin(v,ref);h->db=ref;database *d=dvm__obj(v,ref)->data;sqlite3_stmt *st=NULL;int old=0;
    if(sqlite3_prepare_v2(d->s->db,"PRAGMA user_version",-1,&st,NULL)!=SQLITE_OK||sqlite3_step(st)!=SQLITE_ROW){sqlite3_finalize(st);error(v,d->s);goto helper_fail;}old=sqlite3_column_int(st,0);sqlite3_finalize(st);
    if(sqlite3_exec(d->s->db,"BEGIN",NULL,NULL,NULL)!=SQLITE_OK){error(v,d->s);goto helper_fail;}
    const char *method=old==0?"onCreate":old<h->version?"onUpgrade":old>h->version?"onDowngrade":NULL;
    if(method){const char *msig=old==0?"(Landroid/database/sqlite/SQLiteDatabase;)V":"(Landroid/database/sqlite/SQLiteDatabase;II)V";struct dvm_method *fn=dvm_find_method(v,dvm_object_class(v,self),method,msig);union dvm_value args[3]={{.l=ref},{.i=old},{.i=h->version}}, ignored={0};if(!fn||(!fn->has_code&&!fn->builtin)){dvm__throw(v,"java/lang/UnsupportedOperationException","missing %s",method);sqlite3_exec(d->s->db,"ROLLBACK",NULL,NULL,NULL);goto helper_fail;}if(!dvm_call(v,fn,self,args,old==0?1:3,&ignored)){sqlite3_exec(d->s->db,"ROLLBACK",NULL,NULL,NULL);goto helper_fail;}}
    char pragma[64];snprintf(pragma,sizeof pragma,"PRAGMA user_version=%d",h->version);if(sqlite3_exec(d->s->db,pragma,NULL,NULL,NULL)!=SQLITE_OK||sqlite3_exec(d->s->db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){error(v,d->s);sqlite3_exec(d->s->db,"ROLLBACK",NULL,NULL,NULL);goto helper_fail;}
   }out->l=h->db;return true;
helper_fail: dvm_unpin(v,h->db);h->db=0;return true;
  }
  return false;
 }
 if(!strcmp(cl,"android/database/sqlite/SQLiteDatabase")) {
  database *d=o?o->data:NULL;
  if(IS("close","()V")){if(d)d->closed=1;return true;}
  if(IS("isOpen","()Z")){out->i=d&&!d->closed;return true;}
  if(!d||d->closed){dvm__throw(v,"java/lang/IllegalStateException","database closed");return true;}
  if(IS("execSQL","(Ljava/lang/String;)V")){const char *sql=dvm_string_utf8(v,a[0].l);if(!sql){dvm__throw(v,"java/lang/NullPointerException","sql");return true;}if(sqlite3_exec(d->s->db,sql,NULL,NULL,NULL)!=SQLITE_OK)error(v,d->s);return true;}
  if(IS("rawQuery","(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;")){
   const char *sql=dvm_string_utf8(v,a[0].l);sqlite3_stmt *st=NULL;if(!sql){dvm__throw(v,"java/lang/NullPointerException","sql");return true;}if(sqlite3_prepare_v2(d->s->db,sql,-1,&st,NULL)!=SQLITE_OK){error(v,d->s);return true;}
   struct dvm_object *ar=a[1].l?dvm__obj(v,a[1].l):NULL;if(a[1].l&&(!ar||ar->kind!=DVM_OBJ_ARRAY||ar->elem_kind!='L'||(ar->length&&!ar->data))){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","selectionArgs must be an object array");return true;}if(ar&&ar->length>1000000u){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","too many selectionArgs");return true;}int n=ar?(int)ar->length:0;if(n!=sqlite3_bind_parameter_count(st)){sqlite3_finalize(st);dvm__throw(v,"android/database/sqlite/SQLiteException","selection argument count mismatch");return true;}
   for(int i=0;i<n;i++){dvm_ref ref=((dvm_ref*)ar->data)[i];struct dvm_object *item=ref?dvm__obj(v,ref):NULL;if(ref&&(!item||item->kind!=DVM_OBJ_STRING)){sqlite3_finalize(st);dvm__throw(v,"java/lang/IllegalArgumentException","selectionArgs element must be a string or null");return true;}const char *s=ref?dvm_string_utf8(v,ref):NULL;if(ref&&!s){sqlite3_finalize(st);dvm__throw(v,"java/lang/OutOfMemoryError","selectionArgs string");return true;}int rc=s?sqlite3_bind_text(st,i+1,s,-1,SQLITE_TRANSIENT):sqlite3_bind_null(st,i+1);if(rc!=SQLITE_OK){sqlite3_finalize(st);error(v,d->s);return true;}}
   cursor *c=calloc(1,sizeof *c);if(!c){sqlite3_finalize(st);dvm__throw(v,"java/lang/OutOfMemoryError","cursor");return true;}c->s=d->s;c->s->refs++;c->stmt=st;c->pos=-1;c->cols=sqlite3_column_count(st);c->names=calloc((size_t)c->cols,sizeof *c->names);
   if(c->cols&&!c->names){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","columns");return true;}
   for(int i=0;i<c->cols;i++){const char *name=sqlite3_column_name(st,i);if(!name){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","column name");return true;}size_t len=strlen(name)+1;c->names[i]=malloc(len);if(!c->names[i]){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","column name");return true;}memcpy(c->names[i],name,len);}
   int rc;while((rc=sqlite3_step(st))==SQLITE_ROW){size_t old=(size_t)c->count*(size_t)c->cols;if(c->count>=1000000 || (size_t)c->cols>1000000u-old || (size_t)c->cols>(SIZE_MAX/sizeof *c->cells)-old){cur_destroy(c);dvm__throw(v,"android/database/sqlite/SQLiteException","cursor materialization limit exceeded");return true;}size_t total=old+(size_t)c->cols;sqlite3_value **cells=realloc(c->cells,total*sizeof *cells);if(total&&!cells){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","rows");return true;}c->cells=cells;for(int i=0;i<c->cols;i++){c->cells[old+i]=sqlite3_value_dup(sqlite3_column_value(st,i));if(!c->cells[old+i]){for(int j=0;j<i;j++)sqlite3_value_free(c->cells[old+j]);cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","value");return true;}}c->count++;}if(rc!=SQLITE_DONE){error(v,d->s);cur_destroy(c);return true;}sqlite3_finalize(st);c->stmt=NULL;
   struct dvm_class *cls=dvm_find_class_or_external(v,"android/database/Cursor");dvm_ref ref=cls?dvm_new_object(v,cls):0;struct dvm_object *co=ref?dvm__obj(v,ref):NULL;if(!co){cur_destroy(c);dvm__throw(v,"java/lang/OutOfMemoryError","cursor object");return true;}co->data=c;co->data_destroy=cur_destroy;out->l=ref;return true;
  }
 }
 if(!strcmp(cl,"android/database/Cursor")){
  cursor *c=o?o->data:NULL;if(!c){dvm__throw(v,"java/lang/IllegalStateException","cursor uninitialized");return true;}
  if(IS("close","()V")){if(!c->closed){sqlite3_finalize(c->stmt);c->stmt=NULL;c->closed=1;}return true;}
  if(IS("isClosed","()Z")){out->i=c->closed;return true;}
  if(c->closed){dvm__throw(v,"java/lang/IllegalStateException","cursor closed");return true;}
  if(IS("getCount","()I")){out->i=c->count;return true;}
  if(IS("moveToFirst","()Z")){c->pos=0;out->i=c->count>0;return true;}
  if(IS("moveToNext","()Z")){if(c->pos<c->count)c->pos++;out->i=c->pos<c->count;return true;}
  if(IS("getColumnCount","()I")){out->i=c->cols;return true;}
  if(IS("getColumnIndex","(Ljava/lang/String;)I")){const char *name=dvm_string_utf8(v,a[0].l);out->i=-1;for(int i=0;name&&i<c->cols;i++)if(!strcmp(name,c->names[i])){out->i=i;break;}return true;}
  if(IS("getInt","(I)I")||IS("getLong","(I)J")||IS("getDouble","(I)D")||IS("getFloat","(I)F")||IS("isNull","(I)Z")||IS("getString","(I)Ljava/lang/String;")){int i=a[0].i;if(i<0||i>=c->cols||c->pos<0||c->pos>=c->count){dvm__throw(v,"android/database/CursorIndexOutOfBoundsException","invalid cursor position or column");return true;}sqlite3_value *value=c->cells[(size_t)c->pos*(size_t)c->cols+(size_t)i];
   if(IS("getInt","(I)I"))out->i=sqlite3_value_int(value);else if(IS("getLong","(I)J"))out->j=sqlite3_value_int64(value);else if(IS("getDouble","(I)D"))out->d=sqlite3_value_double(value);else if(IS("getFloat","(I)F"))out->f=(float)sqlite3_value_double(value);else if(IS("isNull","(I)Z"))out->i=sqlite3_value_type(value)==SQLITE_NULL;else {const unsigned char *str=sqlite3_value_text(value);out->l=str?dvm_new_string(v,(const char*)str):0;}return true;
  }
 }
 return false;
}
dvm_ref sqlite_runtime_open_memory(struct dvm *v) {
 shared *s=calloc(1,sizeof *s);database *d=calloc(1,sizeof *d);if(!s||!d){free(s);free(d);return 0;}if(sqlite3_open(":memory:",&s->db)!=SQLITE_OK){sqlite3_close(s->db);free(s);free(d);return 0;}s->refs=1;d->s=s;struct dvm_class *cls=dvm_find_class_or_external(v,"android/database/sqlite/SQLiteDatabase");dvm_ref ref=cls?dvm_new_object(v,cls):0;struct dvm_object *o=ref?dvm__obj(v,ref):NULL;if(!o){db_destroy(d);return 0;}o->data=d;o->data_destroy=db_destroy;return ref;
}
