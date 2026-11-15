#include "game_sqlite.h"
#include "sqlite3.h"
#include <stdlib.h>
struct gs_db { sqlite3 *p; };
struct gs_stmt { gs_db *db; sqlite3_stmt *p; };
int gs_open(const char *path,gs_db **out){ if(!out)return SQLITE_MISUSE;*out=NULL;gs_db*d=calloc(1,sizeof *d);if(!d)return SQLITE_NOMEM;int rc=sqlite3_open_v2(path,&d->p,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);if(rc){sqlite3_close_v2(d->p);free(d);return rc;}*out=d;return SQLITE_OK;}
int gs_close(gs_db*d){if(!d)return SQLITE_MISUSE;int rc=sqlite3_close(d->p);if(rc==SQLITE_OK)free(d);return rc;}
const char *gs_error(gs_db*d){return d?sqlite3_errmsg(d->p):"invalid database";}
int gs_exec(gs_db*d,const char*q){return d&&q?sqlite3_exec(d->p,q,NULL,NULL,NULL):SQLITE_MISUSE;}
int gs_prepare(gs_db*d,const char*q,gs_stmt**out){if(!d||!q||!out)return SQLITE_MISUSE;*out=NULL;gs_stmt*s=calloc(1,sizeof*s);if(!s)return SQLITE_NOMEM;int rc=sqlite3_prepare_v2(d->p,q,-1,&s->p,NULL);if(rc){free(s);return rc;}s->db=d;*out=s;return SQLITE_OK;}
int gs_bind_null(gs_stmt*s,int i){return s?sqlite3_bind_null(s->p,i):SQLITE_MISUSE;}
int gs_bind_int64(gs_stmt*s,int i,int64_t v){return s?sqlite3_bind_int64(s->p,i,v):SQLITE_MISUSE;}
int gs_bind_double(gs_stmt*s,int i,double v){return s?sqlite3_bind_double(s->p,i,v):SQLITE_MISUSE;}
int gs_bind_text(gs_stmt*s,int i,const char*v,int n){return s?sqlite3_bind_text(s->p,i,v,n,SQLITE_TRANSIENT):SQLITE_MISUSE;}
int gs_bind_blob(gs_stmt*s,int i,const void*v,int n){return s?sqlite3_bind_blob(s->p,i,v,n,SQLITE_TRANSIENT):SQLITE_MISUSE;}
int gs_step(gs_stmt*s){return s?sqlite3_step(s->p):SQLITE_MISUSE;}
int gs_column_count(gs_stmt*s){return s?sqlite3_column_count(s->p):0;}
const char*gs_column_name(gs_stmt*s,int i){return s?sqlite3_column_name(s->p,i):NULL;}
int gs_column_type(gs_stmt*s,int i){return s?sqlite3_column_type(s->p,i):SQLITE_NULL;}
int64_t gs_column_int64(gs_stmt*s,int i){return s?sqlite3_column_int64(s->p,i):0;}
double gs_column_double(gs_stmt*s,int i){return s?sqlite3_column_double(s->p,i):0;}
const unsigned char*gs_column_text(gs_stmt*s,int i){return s?sqlite3_column_text(s->p,i):NULL;}
const void*gs_column_blob(gs_stmt*s,int i){return s?sqlite3_column_blob(s->p,i):NULL;}
int gs_column_bytes(gs_stmt*s,int i){return s?sqlite3_column_bytes(s->p,i):0;}
int gs_finalize(gs_stmt*s){if(!s)return SQLITE_MISUSE;int rc=sqlite3_finalize(s->p);free(s);return rc;}
