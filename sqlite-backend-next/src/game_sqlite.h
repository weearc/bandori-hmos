#ifndef GAME_SQLITE_H
#define GAME_SQLITE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gs_db gs_db;
typedef struct gs_stmt gs_stmt;
int gs_open(const char *path, gs_db **out);
int gs_close(gs_db *db);
const char *gs_error(gs_db *db);
int gs_exec(gs_db *db, const char *sql);
int gs_prepare(gs_db *db, const char *sql, gs_stmt **out);
int gs_bind_null(gs_stmt *s,int i);
int gs_bind_int64(gs_stmt *s,int i,int64_t v);
int gs_bind_double(gs_stmt *s,int i,double v);
int gs_bind_text(gs_stmt *s,int i,const char *v,int n);
int gs_bind_blob(gs_stmt *s,int i,const void *v,int n);
int gs_step(gs_stmt *s);
int gs_column_count(gs_stmt *s);
const char *gs_column_name(gs_stmt *s,int i);
int gs_column_type(gs_stmt *s,int i);
int64_t gs_column_int64(gs_stmt *s,int i);
double gs_column_double(gs_stmt *s,int i);
const unsigned char *gs_column_text(gs_stmt *s,int i);
const void *gs_column_blob(gs_stmt *s,int i);
int gs_column_bytes(gs_stmt *s,int i);
int gs_finalize(gs_stmt *s);
#ifdef __cplusplus
}
#endif
#endif
