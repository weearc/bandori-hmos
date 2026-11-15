#define _POSIX_C_SOURCE 200809L
#include "unity_constructor_probe.h"
#include "elf_loader.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifndef LUNARIA_CONSTRUCTOR_CANDIDATE
#define LUNARIA_CONSTRUCTOR_CANDIDATE "unlabeled"
#endif

extern int sdk_trace_fprintf(FILE *, const char *, ...);

/* A fresh PID-specific file is never truncated. Each complete record is
 * flushed before entering foreign code, so a crash leaves the last boundary. */
struct trace { FILE *file; int failed; };
static int sync_record(struct trace *t)
{
    if (fflush(t->file) != 0 || fsync(fileno(t->file)) != 0) t->failed=1;
    return t->failed ? -1 : 0;
}
static int observe(const char *path, size_t index, uintptr_t address,
                   int entering, void *userdata)
{
    struct trace *t=userdata;
    if (fprintf(t->file,"event=%s path=%s kind=%s index=%zu address=0x%llx thread=%llu\n",
                entering?"enter":"returned",path,index==SIZE_MAX?"DT_INIT":"DT_INIT_ARRAY",
                index,(unsigned long long)address,(unsigned long long)(uintptr_t)pthread_self())<0)
        t->failed=1;
    return sync_record(t);
}
int unity_constructor_probe_run(const char *directory, const char *report_directory)
{
    char library[4096], report[4096], index_path[4096], basename[128], error[1024];
    if (!directory || !*directory || !report_directory || !*report_directory) return -1;
    int n=snprintf(library,sizeof(library),"%s/libunity.so",directory);
    if(n<0 || (size_t)n>=sizeof(library)) return -1;
    n=snprintf(basename,sizeof(basename),"unity-constructors-%ld.log",(long)getpid());
    if(n<0 || (size_t)n>=sizeof(basename)) return -1;
    n=snprintf(report,sizeof(report),"%s/%s",report_directory,basename);
    if(n<0 || (size_t)n>=sizeof(report)) return -1;
    n=snprintf(index_path,sizeof(index_path),"%s/unity-constructors-current.txt",report_directory);
    if(n<0 || (size_t)n>=sizeof(index_path)) return -1;
    int fd=open(report,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0) return -1;
    FILE *file=fdopen(fd,"w");
    if(!file){close(fd);return -1;}
    struct trace t={file,0};
    if(fprintf(file,"event=begin no_jni=1 engine_entry_called=0 candidate=%s pid=%ld thread=%llu path=%s\n",LUNARIA_CONSTRUCTOR_CANDIDATE,(long)getpid(),
               (unsigned long long)(uintptr_t)pthread_self(),library)<0)t.failed=1;
    if(sync_record(&t)){fclose(file);return -1;}
    int index_fd=open(index_path,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(index_fd<0){fclose(file);return -1;}
    FILE *index=fdopen(index_fd,"w");
    if(!index){close(index_fd);fclose(file);return -1;}
    int index_failed=fprintf(index,"candidate=%s\nfile=%s\n",LUNARIA_CONSTRUCTOR_CANDIDATE,basename)<0;
    if(fflush(index)!=0 || fsync(fileno(index))!=0)index_failed=1;
    if(fclose(index)!=0)index_failed=1;
    if(index_failed){fclose(file);return -1;}
    sdk_trace_fprintf(stderr,"UNITY_CONSTRUCTOR_PROBE begin no_jni=1 engine_entry_called=0 report=%s\n",report);
    error[0]='\0';
    elf_loader_t *loader=elf_loader_open_initialized_traced(library,error,sizeof(error),observe,&t);
    if(fprintf(file,"event=%s no_jni=1 engine_entry_called=0 status=%s\n",loader?"open_returned":"open_failed",
               loader?elf_loader_status(loader):error)<0)t.failed=1;
    (void)sync_record(&t);
    if(loader)elf_loader_close(loader);
    int failed=t.failed || !loader;
    if(fclose(file)!=0)failed=1;
    sdk_trace_fprintf(stderr,"UNITY_CONSTRUCTOR_PROBE result=%s no_jni=1 engine_entry_called=0 report=%s\n",failed?"FAIL":"PASS",report);
    return failed ? -1 : 0;
}
