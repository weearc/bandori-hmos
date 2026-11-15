#pragma once
#include "dvm/dvm_internal.h"
#include <stddef.h>
#include <stdint.h>
/* Success=1, backend failure=-1, unsupported=-2. String outputs must be NUL
 * terminated; insufficient capacity is a backend failure, never truncation. */
typedef int (*android_web_host_fn)(int operation,uint32_t web,const char *arg0,const char *arg1,char *out,size_t capacity);
enum android_web_operation {
 ANDROID_WEB_SET_COOKIE=1,ANDROID_WEB_GET_COOKIE=2,ANDROID_WEB_REMOVE_ALL=3,
 ANDROID_WEB_REMOVE_SESSION=4,ANDROID_WEB_FLUSH=5,ANDROID_WEB_SET_ACCEPT=6,
 ANDROID_WEB_GET_ACCEPT=7,ANDROID_WEB_HAS_COOKIES=8,ANDROID_WEB_DEFAULT_UA=9,
 ANDROID_WEB_SET_UA=10,ANDROID_WEB_GET_UA=11,ANDROID_WEB_RELEASE_UA=12,
 ANDROID_WEB_PAGE_PROBE=13 /* explicit diagnostic; Boolean metadata only */
};
void android_web_set_host(android_web_host_fn fn);
void android_web_poll(struct dvm *vm);
void android_web_unbind(struct dvm *vm);
bool android_web_release(struct dvm *vm,dvm_ref web);
bool android_web_selftest(struct dvm *vm,dvm_ref existing_web,const char *url,unsigned *checks);
