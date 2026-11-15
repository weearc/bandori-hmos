#ifndef FRAME_IO_PROBE_H
#define FRAME_IO_PROBE_H
/* Instrument the actual libc operation, not the surrounding Java parser.
 * Captures duration only: no paths, file contents, addresses or credentials. */
#include "frame_block_probe.h"
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#define FRAME_IO_SCOPE struct perf_block_scope io_probe __attribute__((cleanup(frame_probe_finish)))=frame_probe_begin(PERF_BLOCK_FILE)
static inline ssize_t frame_io_read(int fd,void *p,size_t n){FRAME_IO_SCOPE;return read(fd,p,n);}
static inline ssize_t frame_io_write(int fd,const void *p,size_t n){FRAME_IO_SCOPE;return write(fd,p,n);}
static inline int frame_io_close(int fd){FRAME_IO_SCOPE;return close(fd);}
static inline int frame_io_open(const char *p,int flags,...){
 mode_t mode=0;int needs_mode=(flags&O_CREAT)!=0;
#ifdef O_TMPFILE
 needs_mode|=(flags&O_TMPFILE)==O_TMPFILE;
#endif
 if(needs_mode){va_list a;va_start(a,flags);mode=(mode_t)va_arg(a,int);va_end(a);}
 FRAME_IO_SCOPE;return needs_mode?open(p,flags,mode):open(p,flags);
}
static inline FILE *frame_io_fopen(const char*p,const char*m){FRAME_IO_SCOPE;return fopen(p,m);}
static inline int frame_io_fclose(FILE*f){FRAME_IO_SCOPE;return fclose(f);}
static inline int frame_io_fflush(FILE*f){FRAME_IO_SCOPE;return fflush(f);}
static inline size_t frame_io_fread(void*p,size_t z,size_t n,FILE*f){FRAME_IO_SCOPE;return fread(p,z,n,f);}
static inline size_t frame_io_fwrite(const void*p,size_t z,size_t n,FILE*f){FRAME_IO_SCOPE;return fwrite(p,z,n,f);}
static inline int frame_io_fseek(FILE*f,long o,int w){FRAME_IO_SCOPE;return fseek(f,o,w);}
#undef fopen
#undef fclose
#undef write
#define read(fd,p,n) frame_io_read(fd,p,n)
#define write(fd,p,n) frame_io_write(fd,p,n)
#define close(fd) frame_io_close(fd)
#define open(...) frame_io_open(__VA_ARGS__)
#define fopen(p,m) frame_io_fopen(p,m)
#define fclose(f) frame_io_fclose(f)
#define fflush(f) frame_io_fflush(f)
#define fread(p,z,n,f) frame_io_fread(p,z,n,f)
#define fwrite(p,z,n,f) frame_io_fwrite(p,z,n,f)
#define fseek(f,o,w) frame_io_fseek(f,o,w)
#endif
