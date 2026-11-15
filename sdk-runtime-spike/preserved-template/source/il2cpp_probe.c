#define _GNU_SOURCE 1

#include "il2cpp_probe.h"
#include "elf_loader.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int il2cpp_library_preflight(const char *directory)
{
    char path[PATH_MAX];
    char error[1024] = {0};
    int n;
    elf_loader_t *loader;

    if (!directory || !directory[0]) {
        sdk_trace_fprintf(stderr, "IL2CPP_LIBRARY_PREFLIGHT mapped=0 initialization_requested=0 noJNI=1 engine_entry_called=0 status=invalid-directory\n");
        return -1;
    }
    n = snprintf(path, sizeof(path), "%s%s%s", directory,
                 directory[strlen(directory) - 1] == '/' ? "" : "/",
                 "libil2cpp.so");
    if (n < 0 || (size_t)n >= sizeof(path)) {
        sdk_trace_fprintf(stderr, "IL2CPP_LIBRARY_PREFLIGHT mapped=0 initialization_requested=0 noJNI=1 engine_entry_called=0 status=path-too-long\n");
        return -1;
    }

    loader = elf_loader_open(path, error, sizeof(error));
    if (!loader) {
        sdk_trace_fprintf(stderr, "IL2CPP_LIBRARY_PREFLIGHT mapped=0 initialization_requested=0 noJNI=1 engine_entry_called=0 status=error error=%s\n",
                error[0] ? error : "unspecified");
        return -1;
    }

    sdk_trace_fprintf(stderr, "IL2CPP_LIBRARY_PREFLIGHT mapped=1 initialization_requested=0 noJNI=1 engine_entry_called=0 status=%s\n",
            elf_loader_status(loader) ? elf_loader_status(loader) : "mapped");
    const char *names[]={"__sF","fprintf","sem_init","sem_wait","sigaction","sigemptyset","pthread_mutex_init","pthread_cond_init","pthread_attr_init","stat","dl_iterate_phdr"};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i){
        void *target=NULL;Dl_info info={0};int found=elf_loader_import_target(loader,names[i],&target);
        int described=found==1&&target&&dladdr(target,&info);
        sdk_trace_fprintf(stderr,"IL2CPP_IMPORT_BINDING name=%s found=%d target=%p module=%s symbol=%s actual_relocation=1 invoked=0\n",names[i],found,target,described&&info.dli_fname?info.dli_fname:"unknown",described&&info.dli_sname?info.dli_sname:"unknown");
    }
    /* close releases our reference; this loader retains mapped images until exit. */
    elf_loader_close(loader);
    return 0;
}
