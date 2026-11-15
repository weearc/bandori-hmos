#ifndef ANDROID_STDIO_SHIM_H
#define ANDROID_STDIO_SHIM_H
#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Only stdio calls are bridged; direct Android FILE field access is unsupported. */
extern unsigned char android_stdio_sF[3][152];
void *android_stdio_fopen(const char *,const char *);
int android_stdio_fclose(void *);
int android_stdio_fflush(void *);
size_t android_stdio_fread(void *,size_t,size_t,void *);
size_t android_stdio_fwrite(const void *,size_t,size_t,void *);
int android_stdio_fseek(void *,long,int);
int android_stdio_fseeko(void *,off_t,int);
off_t android_stdio_ftello(void *);
int android_stdio_fputc(int,void *);
int android_stdio_fprintf(void *,const char *,...);
int android_stdio_vfprintf(void *,const char *,va_list);
void *android_stdio_fdopen(int,const char *);
int android_stdio_fscanf(void *,const char *,...);
int android_stdio_vfscanf(void *,const char *,va_list);
char *android_stdio_fgets(char *,int,void *);
int android_stdio_fputs(const char *,void *);
int android_stdio_feof(void *);
int android_stdio_ferror(void *);
void android_stdio_clearerr(void *);
int android_stdio_fileno(void *);
long android_stdio_ftell(void *);
void android_stdio_setbuf(void *,char *);
int android_stdio_setvbuf(void *,char *,int,size_t);
size_t android_strlen_guard(const char *);
#ifdef __cplusplus
}
#endif
#endif
