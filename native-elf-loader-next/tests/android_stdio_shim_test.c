#define _POSIX_C_SOURCE 200809L
#include "../android_stdio_shim.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int callv(void *f,const char *fmt,...){va_list a;va_start(a,fmt);int n=android_stdio_vfprintf(f,fmt,a);va_end(a);return n;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"stdio test failed line=%d errno=%d\n",__LINE__,errno);unlink(path);return 1;}}while(0)
int main(void){char path[]="/tmp/android-stdio-XXXXXX";int fd=mkstemp(path);if(fd<0)return 1;close(fd);
 void *f=android_stdio_fopen(path,"w+");CHECK(f);CHECK(android_stdio_fprintf(f,"hello %d",42)==8);CHECK(callv(f," %s","world")==6);CHECK(android_stdio_fputc('!',f)=='!');CHECK(android_stdio_ftello(f)==15);CHECK(android_stdio_fflush(f)==0);CHECK(android_stdio_fseeko(f,0,SEEK_SET)==0);char b[32]={0};CHECK(android_stdio_fread(b,1,15,f)==15);CHECK(!strcmp(b,"hello 42 world!"));CHECK(android_stdio_fseek(f,0,SEEK_END)==0);CHECK(android_stdio_fwrite("z",1,1,f)==1);CHECK(android_stdio_fflush(NULL)==0);CHECK(android_stdio_fclose(f)==0);errno=0;CHECK(android_stdio_fclose(f)==EOF&&errno==EBADF);errno=0;CHECK(android_stdio_fputc('x',f)==EOF&&errno==EBADF);CHECK(android_stdio_fwrite("",1,0,&android_stdio_sF[1][0])==0);CHECK(android_stdio_fwrite("",1,0,&android_stdio_sF[2][0])==0);CHECK(android_stdio_fread(b,1,0,&android_stdio_sF[0][0])==0);errno=0;CHECK(android_stdio_fclose(&android_stdio_sF[1][0])==EOF&&errno==ENOTSUP);unlink(path);puts("ANDROID_STDIO_SHIM_TEST PASS file=1 format=1 seek=1 standards=1 invalid=1");return 0;}
