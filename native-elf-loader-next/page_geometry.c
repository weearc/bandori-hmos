#define _GNU_SOURCE 1
#include "page_geometry.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <inttypes.h>
static pthread_once_t host_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t config_lock=PTHREAD_MUTEX_INITIALIZER;
static size_t host_page;
static _Atomic size_t guest_align;
static int configured;
static char status[320];
static int power2(uint64_t n){return n&&!(n&(n-1));}
static void read_host(void){long n=sysconf(_SC_PAGESIZE);if(n>0&&power2((uint64_t)n))host_page=(size_t)n;}
size_t host_page_size(void){pthread_once(&host_once,read_host);return host_page;}
size_t guest_page_align(void){size_t n=atomic_load(&guest_align);return n?n:host_page_size();}
int page_align_down(uint64_t v,uint64_t a,uint64_t *out){if(!out||!power2(a))return 0;*out=v&~(a-1);return 1;}
int page_align_up(uint64_t v,uint64_t a,uint64_t *out){if(!out||!power2(a)||v>UINT64_MAX-(a-1))return 0;*out=(v+a-1)&~(a-1);return 1;}
void page_geometry_configure(const char *directory){
 pthread_mutex_lock(&config_lock);
 if(!configured){
  size_t host=host_page_size(),chosen=host;char value[64]="auto",path[2048];const char *reason="auto";
  if(directory&&snprintf(path,sizeof path,"%s/guest-page-size",directory)>0){
   FILE *f=fopen(path,"r");if(f){size_t n=fread(value,1,sizeof value-1,f);value[n]=0;fclose(f);
    while(n&&(value[n-1]=='\n'||value[n-1]=='\r'||value[n-1]==' '||value[n-1]=='\t'))value[--n]=0;
   }
  }
  size_t requested=!strcmp(value,"4096")?4096:!strcmp(value,"16384")?16384:!strcmp(value,"65536")?65536:0;
  if(!host)reason="E-HOST-PAGESIZE sysconf_invalid";
  else if(!strcmp(value,"auto"))reason="auto";
  else if(!requested||!power2(requested))reason="rejected_invalid_value fallback=auto";
  else if(requested<host)reason="rejected_smaller_than_host fallback=auto";
  else {chosen=requested;reason="override_alignment_only";}
  atomic_store(&guest_align,chosen);
  snprintf(status,sizeof status,"HOST_PAGESIZE=%zu\nGUEST_PAGE_ALIGN=%zu\npage_align_reason=%s",host,chosen,reason);
  configured=1;
 }
 pthread_mutex_unlock(&config_lock);
}
const char *page_geometry_status(void){return status;}
void *page_reserve(size_t bytes,size_t align){
 size_t host=host_page_size();
 if(!host||!power2(align)||align<host||!bytes||bytes%align||bytes>SIZE_MAX-align){errno=EINVAL;return MAP_FAILED;}
 /* mmap guarantees HOST alignment only. Reserve slack and unmap both ends;
  * all addresses/lengths passed to the kernel remain host-page multiples. */
 size_t total=bytes+align;void *raw=mmap(NULL,total,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(raw==MAP_FAILED)return raw;
 uint64_t rounded;
 if(!page_align_up((uintptr_t)raw,align,&rounded)){munmap(raw,total);errno=EOVERFLOW;return MAP_FAILED;}
 size_t head=(size_t)(rounded-(uintptr_t)raw),tail=total-head-bytes;
 if(head&&munmap(raw,head)){int e=errno;munmap(raw,total);errno=e;return MAP_FAILED;}
 if(tail&&munmap((void *)(uintptr_t)(rounded+bytes),tail)){int e=errno;munmap((void *)(uintptr_t)rounded,total-head);errno=e;return MAP_FAILED;}
 return (void *)(uintptr_t)rounded;
}
