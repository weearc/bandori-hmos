/* Android logging subset mapped to Harmony HiLog. Not an Android logd service.
 * Contract: https://developer.android.com/ndk/reference/group/logging
 * Compile only; explicit HAP integration and device verification remain pending.
 */
#include <hilog/log.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int write_main(int priority, const char* tag, const char* text) {
    LogLevel level;
    switch(priority) {
        case 2: case 3: level=LOG_DEBUG; break;
        case 4: level=LOG_INFO; break;
        case 5: level=LOG_WARN; break;
        case 6: level=LOG_ERROR; break;
        case 7: level=LOG_FATAL; break;
        default: return -EINVAL;
    }
    if(!text) return -EINVAL;
    /* Guest strings are arguments, never interpreted as HiLog format strings. */
    int result=OH_LOG_Print(LOG_APP,level,0xB011,tag?tag:"AndroidGuest","%{private}s",text);
    return result>=0?1:-EPERM;
}
int __android_log_write(int priority,const char* tag,const char* text) {
    return write_main(priority,tag,text);
}
int __android_log_buf_write(int buffer,int priority,const char* tag,const char* text) {
    if(buffer!=0) return -EPERM;
    return write_main(priority,tag,text);
}
int __android_log_vprint(int priority,const char* tag,const char* format,va_list args) {
    if(!format) return -EINVAL;
    char message[4096];
    if(vsnprintf(message,sizeof(message),format,args)<0) return -EINVAL;
    return write_main(priority,tag,message);
}
int __android_log_print(int priority,const char* tag,const char* format,...) {
    va_list args; va_start(args,format);
    int result=__android_log_vprint(priority,tag,format,args);
    va_end(args); return result;
}
__attribute__((noreturn)) void __android_log_assert(const char* condition,const char* tag,const char* format,...) {
    if(format) {
        va_list args; va_start(args,format);
        __android_log_vprint(7,tag,format,args); va_end(args);
    } else write_main(7,tag,condition?condition:"Android assertion failed");
    abort();
}
