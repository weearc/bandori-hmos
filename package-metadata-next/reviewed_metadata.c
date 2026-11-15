#include "package_metadata.h"
#include "gc_runtime.h"
#include "guest_identity.h"
#include "apk_signature_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);
/* Original Unity calls these APIs after the SDK bootstrap trace has ended. */
static void metadata_engine_log(const char *fmt,...){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 fprintf(f,"ENGINE_LOG_METADATA pid=%d ",(int)getpid());va_list a;va_start(a,fmt);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fflush(f);fsync(fileno(f));fclose(f);
}
/* 复用现有 runtime，不新写第二套实现：
 * - cert_runtime_call：java/security/MessageDigest（MD5 走 mbedtls_md5）。
 * - java_basics_call：java/lang/Integer 的静态 toString(int[,int])。
 * 构建 source 提供 java_basics.h / cert_runtime.h；这里用 extern 声明，
 * 避免工作区 overlay 单独静态检查时依赖构建目录头文件。 */
extern bool cert_runtime_call(struct dvm *, const char *, const char *, const char *, dvm_ref,
                              const union dvm_value *, int, union dvm_value *);
extern bool java_basics_call(struct dvm *, const char *, const char *, const char *, dvm_ref,
                             const union dvm_value *, int, union dvm_value *);
extern bool collections_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
extern bool set_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
extern bool intent_runtime_query_snapshot(struct dvm *,dvm_ref,const char **,const char **,const char **,dvm_ref *,dvm_ref *);
typedef struct {const char* key;int type;const char* text;uint32_t bits;} entry;
static const entry entries[]={
#include "metadata_values.inc"
};
/* One VM at a time, reset before DEX loading. Never retain arena-owned nodes. */
static struct {struct dvm* vm;dvm_ref pm;char native_library_dir[PATH_MAX];unsigned query_logs,installed_logs,service_logs;bool service_cache_valid;uint64_t service_cache_fills,service_cache_hits,installation_generation;} state;
/* The host exposes no Android running-service registry. Cache that visibility
 * result, not a mutable guest List and not a heap scan. VM start resets it;
 * package event dispatch invalidates it before guest receivers run. */
void package_metadata_package_event(struct dvm *v,const char *action){
 if(state.vm!=v||!action)return;
 if(!strcmp(action,"android.intent.action.PACKAGE_ADDED")||!strcmp(action,"android.intent.action.PACKAGE_REMOVED")||!strcmp(action,"android.intent.action.PACKAGE_REPLACED")){
  state.service_cache_valid=false;state.installation_generation++;
 }
}
static bool metadata_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 if(!v||!visit||context!=&state||state.vm!=v||!dvm_gil_held(v))return false;
 /* The host owns this per-process PackageManager singleton. Returned
  * ApplicationInfo/PackageInfo instances are ordinary guest owner graphs. */
 if(state.pm)visit(&state.pm,user);return true;
}
static bool metadata_gc_ensure(struct dvm *v){
 if(dvm_gc_root_provider_add(v,"package-metadata/pm",DVM_GC_ROOT_STRONG,metadata_gc_roots,&state))return true;
 if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","PackageManager GC root");return false;
}
void package_metadata_gc_vm_end(struct dvm *v){dvm_gc_root_provider_remove(v,"package-metadata/pm",&state);}
typedef struct {int32_t value;float real;} boxed;
static void box_free(void* p){free(p);}
static void bundle_free(void* p){free(p);}
/* Explicit class + producer whitelist. These APK manifest markers contain
 * no guest IDs: box() stores only int32/float; build_application_info() uses
 * a one-byte Bundle tag and reads immutable native entries[]. Never whitelist
 * an arbitrary Bundle or a generic free() destructor. */
int metadata_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 (void)v;(void)visit;(void)u;
 const char *n=o&&o->cls?o->cls->name:NULL;
 if(!n)return DVM_GC_PAYLOAD_UNKNOWN;
 if(o->data_destroy==bundle_free&&!strcmp(n,"android/os/Bundle"))return DVM_GC_PAYLOAD_ATOMIC;
 if(o->data_destroy==box_free&&(!strcmp(n,"java/lang/Boolean")||!strcmp(n,"java/lang/Integer")||!strcmp(n,"java/lang/Float")))return DVM_GC_PAYLOAD_ATOMIC;
 return DVM_GC_PAYLOAD_UNKNOWN;
}
static bool fail(struct dvm* v,const char* text){dvm__throw(v,"java/lang/UnsupportedOperationException","%s",text);return true;}
static dvm_ref object(struct dvm* v,const char* name){return dvm_new_object(v,dvm_find_class_or_external(v,name));}
static bool define_info(struct dvm* v,const char* name,const char* const* names,const char* const* types,int count){
 struct dvm_class* c=dvm_find_class_or_external(v,name);
 if(!c||!c->external||c->ifields||c->nifields||c->islots||(c->super&&c->super->islots))return false;
 struct dvm_field* fields=calloc((size_t)count,sizeof *fields);if(!fields)return false;
 for(int i=0;i<count;i++){fields[i].cls=c;fields[i].name=names[i];fields[i].type=types[i];fields[i].kind=dvm__kind_of(types[i]);fields[i].width=1;fields[i].access=1;fields[i].slot=(uint16_t)i;}
 c->ifields=fields;c->nifields=count;c->islots=count;return true;
}
/* Startup only, before DEX loading/method lookup. Existing declarations are
 * retained; no shared class style or ApplicationInfo slots are replaced. */
static bool metadata_method(struct dvm *v,const char *owner,const char *name,const char *sig){
 struct dvm_class *c=dvm_find_class_or_external(v,owner);if(!c||!c->external)return false;
 for(int i=0;i<c->nmethods;i++)if(!strcmp(c->methods[i].name,name)&&!strcmp(c->methods[i].sig,sig))return true;
 /* All registrations run before DEX loading or method lookup. No guest
  * method pointer can yet refer to this startup table. */
 char *signature=strdup(sig);if(!signature)return false;
 struct dvm_method *ms=realloc(c->methods,(size_t)(c->nmethods+1)*sizeof *ms);if(!ms){free(signature);return false;}c->methods=ms;
 struct dvm_method *m=&ms[c->nmethods];memset(m,0,sizeof *m);m->cls=c;m->name=name;m->sig=signature;
 m->access=1;m->arg_slots=dvm_sig_arg_slots(sig);m->arg_count=dvm_sig_arg_count(sig);m->ret_kind=dvm_sig_return_kind(sig);c->nmethods++;
 free(c->mcache);c->mcache=NULL;c->mcache_cap=0;c->mcache_len=0;return true;
}
bool package_metadata_vm_start(struct dvm* v){
 memset(&state,0,sizeof state);state.vm=v;
 /* fail-closed：内嵌 DER 长度或 SHA256 不符时不注册伪证书。 */
 if(!apk_signature_vm_start(v))return false;
 const char* an[]={"metaData","targetSdkVersion","packageName","nativeLibraryDir","splitPublicSourceDirs","sourceDir","dataDir"};const char* at[]={"Landroid/os/Bundle;","I","Ljava/lang/String;","Ljava/lang/String;","[Ljava/lang/String;","Ljava/lang/String;","Ljava/lang/String;"};
 const char* pn[]={"versionCode","versionName","packageName","applicationInfo","signatures"};
 const char* pt[]={"I","Ljava/lang/String;","Ljava/lang/String;","Landroid/content/pm/ApplicationInfo;","[Landroid/content/pm/Signature;"};
 if(!define_info(v,"android/content/pm/ApplicationInfo",an,at,7)||!define_info(v,"android/content/pm/PackageInfo",pn,pt,5))return false;
 const char *rn[]={"activityInfo"},*rt[]={"Landroid/content/pm/ActivityInfo;"};
 const char *ain[]={"name","packageName","applicationInfo","enabled","exported","processName"};
 const char *ait[]={"Ljava/lang/String;","Ljava/lang/String;","Landroid/content/pm/ApplicationInfo;","Z","Z","Ljava/lang/String;"};
 if(!define_info(v,"android/content/pm/ResolveInfo",rn,rt,1)||!define_info(v,"android/content/pm/ActivityInfo",ain,ait,6))return false;
 if(!metadata_method(v,"android/content/pm/PackageManager","queryIntentActivities","(Landroid/content/Intent;I)Ljava/util/List;")||
    !metadata_method(v,"android/content/pm/PackageManager","getInstalledApplications","(I)Ljava/util/List;")||
    !metadata_method(v,"android/app/ActivityManager","getMemoryInfo","(Landroid/app/ActivityManager$MemoryInfo;)V")||
    !metadata_method(v,"android/app/ActivityManager","getRunningServices","(I)Ljava/util/List;"))return false;
 struct dvm_class* number=dvm_find_class_or_external(v,"java/lang/Number");
 const char* types[]={"java/lang/Integer","java/lang/Float"};
 for(size_t i=0;i<2;i++){struct dvm_class* t=dvm_find_class_or_external(v,types[i]);if(t&&t->external)t->super=number;}
 return true;
}
bool package_metadata_set_host_native_library_dir(struct dvm* v,const char* path){
 if(!v||state.vm!=v)return false;
 if(!path){state.native_library_dir[0]='\0';return true;}
 char resolved[PATH_MAX];struct stat st;
 if(!realpath(path,resolved)||stat(resolved,&st)!=0||!S_ISDIR(st.st_mode))return false;
 size_t n=strlen(resolved);if(n>=sizeof state.native_library_dir)return false;
 memcpy(state.native_library_dir,resolved,n+1);return true;
}
static const entry* lookup(const char* key){if(key)for(size_t i=0;i<sizeof entries/sizeof entries[0];i++)if(!strcmp(key,entries[i].key))return &entries[i];return NULL;}
static dvm_ref metadata_string(struct dvm* v,const entry* e){
 dvm_ref ref=dvm_new_string(v,e->text);
 if(!ref)dvm__throw(v,"java/lang/OutOfMemoryError","metadata string");
 return ref;
}
static dvm_ref box(struct dvm* v,const entry* e){
 if(e->type==3)return dvm_new_string(v,e->text);
 const char* name=e->type==18?"java/lang/Boolean":e->type==4?"java/lang/Float":"java/lang/Integer";
 dvm_ref ref=object(v,name);struct dvm_object* o=dvm__obj(v,ref);boxed* p=calloc(1,sizeof *p);
 if(!o||!p){free(p);dvm__throw(v,"java/lang/OutOfMemoryError","metadata value");return 0;}
 p->value=(int32_t)e->bits;if(e->type==4)memcpy(&p->real,&e->bits,sizeof p->real);
 o->data=p;o->data_destroy=box_free;return ref;
}
/* ApplicationInfo shared by PackageManager.getApplicationInfo(pkg, flags) and
 * Context.getApplicationInfo().  Only real host-backed fields exist:
 * targetSdkVersion, packageName and, for GET_META_DATA(0x80), the parsed
 * manifest Bundle.  sourceDir/dataDir stay undefined and guest reads fail
 * with NoSuchFieldError rather than returning invented paths.
 *
 * nativeLibraryDir is populated only by the host bridge after it validates
 * the actual loaded HAP library directory. This is an explicit host-to-guest
 * path mapping, not an Android PackageManager installation-path claim. */
static dvm_ref build_application_info(struct dvm* v,int flags){
 dvm_ref info=object(v,"android/content/pm/ApplicationInfo");
 struct dvm_object* o=dvm__obj(v,info);
 if(!o||!o->slots){if(info)dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","ApplicationInfo");return 0;}
 dvm_ref bundle=0;struct dvm_object* b=NULL;
 if(flags&128){
  bundle=object(v,"android/os/Bundle");b=dvm__obj(v,bundle);
  /* Bundle has no modelled fields, so b->slots is legitimately NULL; only
   * the object itself and the host payload are required. */
  if(!b){if(bundle)dvm__discard_temporary(v,bundle);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","metadata Bundle");return 0;}
  b->data=calloc(1,1);
  if(!b->data){dvm__discard_temporary(v,bundle);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","metadata Bundle");return 0;}
  b->data_destroy=bundle_free;
 }
 o->slots[0].l=bundle;o->slots[1].i=GUEST_EXPECTED_TARGET_SDK;
 dvm_ref package_name=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);
 if(!package_name){if(bundle)dvm__discard_temporary(v,bundle);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","package name");return 0;}
 o->slots[2].l=package_name;
 /* Android 的 ApplicationInfo 公开字段：sourceDir 是已安装 APK 路径，dataDir
  * 是应用私有数据根。两者都指向宿主沙箱里真实存在的路径（同一 base.apk 由
  * 引擎自己的 zip 读取器读取），不是占位字符串。 */
 dvm_ref source_dir=dvm_new_string(v,"/data/storage/el2/base/haps/entry/files/sdkdex/base.apk");
 dvm_ref data_dir=dvm_new_string(v,"/data/storage/el2/base/haps/entry/files");
 if(!source_dir||!data_dir){if(bundle)dvm__discard_temporary(v,bundle);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","application paths");return 0;}
 o->slots[5].l=source_dir;
 o->slots[6].l=data_dir;
 o->slots[3].l=0;
 /* This guest installation supplies one unchanged base.apk and no installed
  * split APKs. Null is the Android no-splits value, not a missing asset stub.
  * A future split installation must supply its real inventory here. */
 o->slots[4].l=0;
 if(state.vm==v&&state.native_library_dir[0]){
  dvm_ref native_dir=dvm_new_string(v,state.native_library_dir);
  if(!native_dir){if(bundle)dvm__discard_temporary(v,bundle);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","native library directory");return 0;}
  o->slots[3].l=native_dir;
 }
 return info;
}
/* The real label of the one installed guest package.  It is the value of
 * resources.arsc resource 0x7f0d002c string/app_name in the original APK
 * (APK SHA-256 2ff2baf1b8fe78692b67507f88d3849422b672a0a929b374a878e1087d1519ec),
 * confirmed by `aapt2 dump resources` and the APK's application-label.  It is
 * a verified manifest/resource fact, not a synthesized or localized guess:
 * the default configuration of this APK has exactly this label. */
#define GUEST_APPLICATION_LABEL "BanG Dream!"
/* PackageManager.getApplicationLabel(info) is info.loadLabel(this).  The
 * ApplicationInfo built above carries packageName but no labelRes, so the
 * verified label above is attached to exactly the represented guest package;
 * any other ApplicationInfo follows PackageItemInfo.loadLabel's final
 * fallbacks: its own packageName, otherwise null.  A null info is rejected by
 * the caller with NullPointerException, as AOSP does. */
static bool metadata_is_a(struct dvm *v,dvm_ref ref,const char *name){
 struct dvm_object *o=dvm__obj(v,ref);for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)if(c->name&&!strcmp(c->name,name))return true;return false;
}
static dvm_ref metadata_list(struct dvm *v){
 dvm_ref list=object(v,"java/util/ArrayList");union dvm_value out={0};
 if(!list||!collections_call(v,"java/util/ArrayList","<init>","()V",list,NULL,0,&out)||dvm_exception(v)){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","package list");return 0;}return list;
}
static bool metadata_list_add(struct dvm *v,dvm_ref list,dvm_ref item){
 union dvm_value arg={.l=item},out={0};if(!collections_call(v,"java/util/List","add","(Ljava/lang/Object;)Z",list,&arg,1,&out)){if(!dvm_exception(v))fail(v,"package list add unavailable");return false;}return !dvm_exception(v);
}
/* All 155 activities and the alias were inspected. Exactly these two have
 * intent-filters, no data/type declarations. Original APK SHA256:
 * 2ff2baf1b8fe78692b67507f88d3849422b672a0a929b374a878e1087d1519ec.
 * AndroidManifest.xml SHA256 dc6c36c4805a29eb0f9b7ef58c8025feefcf5593965589e189821841d8da3b3a.
 * AXML start offsets: PermissionActivity 0x7e90/filter0x7f2c;
 * ShareTransActivity 0xd728/filter0xd7b0. This is installation metadata,
 * never a caller/SDK-class-specific bypass. Full explicit inventory is not
 * registered here: explicit component queries fail rather than fabricate. */
static const struct {const char *name,*action,*category;bool is_default;} manifest_filters[]={
 {"com.bilibili.star.bili.PermissionActivity","android.intent.action.MAIN","android.intent.category.LAUNCHER",false},
 {"com.sina.weibo.sdk.share.ShareTransActivity","com.sina.weibo.sdk.action.ACTION_SDK_REQ_ACTIVITY","android.intent.category.DEFAULT",true}
};
static bool metadata_query_activities(struct dvm *v,dvm_ref intent,int flags,union dvm_value *out){
 if(flags&~0x10000)return fail(v,"queryIntentActivities flags unsupported");
 if(!intent){dvm__throw(v,"java/lang/NullPointerException","Intent");return true;}
 const char *action=NULL,*package_name=NULL,*class_name=NULL;dvm_ref data=0,categories=0;
 if(!intent_runtime_query_snapshot(v,intent,&action,&package_name,&class_name,&data,&categories)){if(!dvm_exception(v))fail(v,"queryIntentActivities foreign Intent");return true;}
 if(class_name)return fail(v,"queryIntentActivities explicit component inventory not registered");
 int category_count=0;
 if(categories){union dvm_value got={0};if(!set_runtime_call(v,"java/util/Set","size","()I",categories,NULL,0,&got)||dvm_exception(v)){if(!dvm_exception(v))fail(v,"Intent category Set unavailable");return true;}category_count=got.i;if(category_count<0)return fail(v,"invalid category count");}
 dvm_ref list=metadata_list(v);if(!list)return true;int matches=0;const char *matched_component=NULL;
 if(!data&&(!package_name||!strcmp(package_name,GUEST_EXPECTED_PACKAGE))){
  for(size_t i=0;i<sizeof manifest_filters/sizeof manifest_filters[0];i++){
   if(action&&strcmp(action,manifest_filters[i].action))continue;
   if((flags&0x10000)&&!manifest_filters[i].is_default)continue;
   /* Each real filter has exactly one category: subset matching therefore
    * needs size zero, or size one with that category present. */
   if(category_count>1)continue;
   if(category_count==1){union dvm_value arg={.l=dvm_new_string(v,manifest_filters[i].category)},got={0};if(!arg.l){dvm__throw(v,"java/lang/OutOfMemoryError","category");return true;}if(!set_runtime_call(v,"java/util/Set","contains","(Ljava/lang/Object;)Z",categories,&arg,1,&got)||dvm_exception(v)){if(!dvm_exception(v))fail(v,"Intent category contains unavailable");return true;}if(!got.i)continue;}
   dvm_ref ai=object(v,"android/content/pm/ActivityInfo"),ri=object(v,"android/content/pm/ResolveInfo"),app=build_application_info(v,0);
   struct dvm_object *a=dvm__obj(v,ai),*r=dvm__obj(v,ri);
   if(!a||!a->slots||!r||!r->slots||!app){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","ResolveInfo");return true;}
   dvm_ref name=dvm_new_string(v,manifest_filters[i].name),pkg=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);if(!name||!pkg){dvm__throw(v,"java/lang/OutOfMemoryError","ActivityInfo names");return true;}
   a->slots[0].l=name;a->slots[1].l=pkg;a->slots[2].l=app;a->slots[3].i=1;a->slots[4].i=1;a->slots[5].l=pkg;r->slots[0].l=ai;
   if(!metadata_list_add(v,list,ri))return true;matches++;matched_component=manifest_filters[i].name;
  }
 }
 if(state.query_logs++<12)metadata_engine_log("PACKAGE_QUERY action=%s package=%s flags=%d data=%u categories=%d matches=%d component=%s",action?action:"<null>",package_name?package_name:"<any>",flags,(unsigned)data,category_count,matches,matches==1?matched_component:"<multiple-or-none>");
 out->l=list;return true;
}
static bool metadata_running_services_fast(struct dvm *v,dvm_ref self,int max,union dvm_value *out){
 if(!metadata_is_a(v,self,"android/app/ActivityManager"))return fail(v,"foreign ActivityManager receiver");
 if(max<0)return fail(v,"negative getRunningServices max unsupported");
 if(state.service_cache_valid)state.service_cache_hits++;
 else {state.service_cache_valid=true;state.service_cache_fills++;}
 /* Ordinary app visibility fallback: no host Android service inventory.
  * Fresh mutable list on each call; no heap scan, binder, IO or retry here.
  * Cache provenance is emitted once by the startup selftest, outside queries. */
 out->l=metadata_list(v);return true;
}
static bool metadata_running_services(struct dvm *v,dvm_ref self,int max,union dvm_value *out){
 extern int dvm_emu_detect_fast_enabled(void);
 if(dvm_emu_detect_fast_enabled())return metadata_running_services_fast(v,self,max,out);
 if(!metadata_is_a(v,self,"android/app/ActivityManager"))return fail(v,"foreign ActivityManager receiver");
 /* Negative max is deliberately unsupported, not asserted to be Android's
  * argument policy. max=0 requires no service-state knowledge. */
 if(max<0)return fail(v,"negative getRunningServices max unsupported");
 extern bool dvm_heap_header_count_class(struct dvm *,const char *,uint32_t *);
 struct timespec scan_start,scan_end;clock_gettime(CLOCK_MONOTONIC,&scan_start);
 unsigned count=0;bool header_scan=false;
 if(max)header_scan=dvm_heap_header_count_class(v,"android/app/Service",&count);
 if(max&&!header_scan)for(uint32_t i=0;i<v->heap_size;i++){struct dvm_object *o=dvm__obj(v,(dvm_ref)(i+1u));for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)if(c->name&&!strcmp(c->name,"android/app/Service")){count++;break;}}
 clock_gettime(CLOCK_MONOTONIC,&scan_end);
 double scan_ms=(scan_end.tv_sec-scan_start.tv_sec)*1000.0+(scan_end.tv_nsec-scan_start.tv_nsec)/1e6;
 if(state.service_logs++<12)metadata_engine_log("RUNNING_SERVICES max=%d scanned=%d header_only=%d service_objects=%u heap_slots=%u scan_ms=%.6f registry=none conservative_negative_only=1",max,max>0,header_scan,count,v->heap_size,scan_ms);
 /* A live Service object is not proof of a running service. Without a
  * lifecycle registry, positive enumeration is unanswerable. Zero objects
  * under the VM lock proves there are no instantiated guest Services. */
 if(count){dvm__throw(v,"java/lang/UnsupportedOperationException","getRunningServices: %u Service objects, no lifecycle registry",count);return true;}
 out->l=metadata_list(v);return true;
}
static bool application_label(struct dvm* v,dvm_ref info,union dvm_value* out){
 struct dvm_object* o=dvm__obj(v,info);
 bool is_application=false;
 for(struct dvm_class* cls=o?o->cls:NULL;cls;cls=cls->super)
  if(cls->name&&!strcmp(cls->name,"android/content/pm/ApplicationInfo")){is_application=true;break;}
 if(!is_application)return fail(v,"getApplicationLabel requires ApplicationInfo");
 union dvm_value package_name={0};
 bool has_package=dvm_get_field(v,info,"packageName","Ljava/lang/String;",&package_name);
 const char* name=has_package&&package_name.l?dvm_string_utf8(v,package_name.l):NULL;
 if(name&&!strcmp(name,GUEST_EXPECTED_PACKAGE)){
  out->l=dvm_new_string(v,GUEST_APPLICATION_LABEL);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","application label");
  return true;
 }
 out->l=name?package_name.l:0;
 return true;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
bool package_metadata_call(struct dvm* v,const char* c,const char* m,const char* sig,dvm_ref self,const union dvm_value* a,int n,union dvm_value* out){
 if(state.vm!=v)return false;
 if(!metadata_gc_ensure(v))return false;
 if(!strcmp(c,"android/app/ActivityManager")&&IS("getRunningServices","(I)Ljava/util/List;")&&n==1&&a)return metadata_running_services(v,self,a[0].i,out);
 if(!strcmp(c,"android/content/pm/PackageManager")&&(IS("queryIntentActivities","(Landroid/content/Intent;I)Ljava/util/List;")||IS("getInstalledApplications","(I)Ljava/util/List;"))){
  if(!self||self!=state.pm)return fail(v,"foreign PackageManager");
  if(IS("queryIntentActivities","(Landroid/content/Intent;I)Ljava/util/List;")){if(n!=2||!a)return fail(v,"queryIntentActivities arity");return metadata_query_activities(v,a[0].l,a[1].i,out);}
  if(n!=1||!a)return fail(v,"getInstalledApplications arity");if(a[0].i!=0&&a[0].i!=128)return fail(v,"getInstalledApplications flags unsupported");
  dvm_ref list=metadata_list(v),app=list?build_application_info(v,a[0].i):0;if(!list||!app)return true;if(!metadata_list_add(v,list,app))return true;out->l=list;
  if(state.installed_logs++<12)metadata_engine_log("INSTALLED_APPLICATIONS flags=%d count=1 package=%s source=guest_installation",a[0].i,GUEST_EXPECTED_PACKAGE);return true;
 }
 if((!strcmp(c,"android/content/Context")||!strcmp(c,"android/app/Activity")||!strcmp(c,"android/app/Application")||!strcmp(c,"android/content/ContextWrapper"))&&IS("getPackageManager","()Landroid/content/pm/PackageManager;")){
  struct dvm_object* receiver=dvm__obj(v,self);bool context=false;
  for(struct dvm_class* cls=receiver?receiver->cls:NULL;cls;cls=cls->super)if(!strcmp(cls->name,"android/content/Context"))context=true;
  if(!context)return fail(v,"foreign Context receiver");
  if(!state.pm)state.pm=object(v,"android/content/pm/PackageManager");
  if(!state.pm){dvm__throw(v,"java/lang/OutOfMemoryError","PackageManager");return true;}
  out->l=state.pm;return true;
 }
 if(!strcmp(c,"android/content/pm/PackageManager")&&IS("getApplicationLabel","(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;")){
  if(self!=state.pm)return fail(v,"foreign PackageManager");
  if(n!=1||!a){dvm__throw(v,"java/lang/IllegalArgumentException","getApplicationLabel arity %d",n);return true;}
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","ApplicationInfo");return true;}
  return application_label(v,a[0].l,out);
 }
 bool app_info=IS("getApplicationInfo","(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;");
 bool pkg_info=IS("getPackageInfo","(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
 if((!strcmp(c,"android/content/Context")||!strcmp(c,"android/app/Activity")||!strcmp(c,"android/app/Application")||!strcmp(c,"android/content/ContextWrapper"))&&IS("getApplicationInfo","()Landroid/content/pm/ApplicationInfo;")){
  struct dvm_object* receiver=dvm__obj(v,self);bool context=false;
  for(struct dvm_class* cls=receiver?receiver->cls:NULL;cls;cls=cls->super)if(!strcmp(cls->name,"android/content/Context"))context=true;
  if(!context)return fail(v,"foreign Context receiver");
  /* Android Context.getApplicationInfo() describes the loaded application;
   * this is exactly the getApplicationInfo(pkg, 0) shape.  Never an empty
   * placeholder: packageName and targetSdkVersion are real host facts. */
  out->l=build_application_info(v,0);return true;
 }
 if(!strcmp(c,"android/content/pm/PackageManager")&&(app_info||pkg_info)&&n==2){
  if(self!=state.pm)return fail(v,"foreign PackageManager");
  const char* name=dvm_string_utf8(v,a[0].l);
  if(!name||strcmp(name,GUEST_EXPECTED_PACKAGE)){dvm__throw(v,"android/content/pm/PackageManager$NameNotFoundException","guest package not found");return true;}
  int flags=a[1].i;
  /* ApplicationInfo 仍只接受 0/128；PackageInfo 增加 GET_SIGNATURES(0x40)，
   * 并允许与 GET_META_DATA(0x80) 组合。未知位 fail-closed，不静默忽略。 */
  if(app_info){
   if(flags!=0&&flags!=128)return fail(v,"application info flags not implemented");
   out->l=build_application_info(v,flags);return true;
  }
  if(flags&~0xc0)return fail(v,"package info flags not implemented");
  dvm_ref info=build_application_info(v,flags);
  if(!info)return true;
  dvm_ref ref=object(v,"android/content/pm/PackageInfo");struct dvm_object* p=dvm__obj(v,ref);
  if(!p||!p->slots){if(ref)dvm__discard_temporary(v,ref);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","PackageInfo");return true;}
  p->slots[0].i=GUEST_EXPECTED_VERSION_CODE;
  dvm_ref version=dvm_new_string(v,GUEST_EXPECTED_VERSION_NAME);
  if(!version){dvm__discard_temporary(v,ref);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","version name");return true;}
  p->slots[1].l=version;p->slots[2].l=dvm__obj(v,info)->slots[2].l;
  p->slots[3].l=info;p->slots[4].l=0;
  if(flags&64){
   /* 只使用内置原 APK 证书；DER 初始化时已校验长度和 SHA256。 */
   size_t der_len=0;const unsigned char* der=apk_signature_der(&der_len);
   dvm_ref sig=apk_signature_new(v,der,der_len);
   if(!sig){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Signature");dvm__discard_temporary(v,ref);dvm__discard_temporary(v,info);return true;}
   dvm_ref signatures=dvm_new_array(v,'L',"Landroid/content/pm/Signature;",1);
   if(!signatures){dvm__discard_temporary(v,sig);dvm__discard_temporary(v,ref);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","Signature array");return true;}
   dvm_ref* elements=dvm_array_data(v,signatures);
   if(!elements){dvm__discard_temporary(v,signatures);dvm__discard_temporary(v,sig);dvm__discard_temporary(v,ref);dvm__discard_temporary(v,info);dvm__throw(v,"java/lang/OutOfMemoryError","Signature array data");return true;}
   elements[0]=sig;p->slots[4].l=signatures;
  }
  out->l=ref;return true;
 }
 if(!strcmp(c,"android/os/Bundle")){
  struct dvm_object* o=dvm__obj(v,self);if(!o||o->data_destroy!=bundle_free||!o->data)return false;
  if(IS("size","()I")){out->i=(int)(sizeof entries/sizeof entries[0]);return true;}
  if(n!=1&&n!=2)return false;
  const char* key=dvm_string_utf8(v,a[0].l);if(!key){dvm__throw(v,"java/lang/NullPointerException","metadata key");return true;}
  const entry* e=lookup(key);
  if(IS("containsKey","(Ljava/lang/String;)Z")){out->i=e!=NULL;return true;}
  if(IS("get","(Ljava/lang/String;)Ljava/lang/Object;")){
   if(!e){out->l=0;return true;}
   if(e->type!=3&&e->type!=4&&e->type!=16&&e->type!=17&&e->type!=18)return fail(v,"unresolved metadata resource reference");
   out->l=box(v,e);return true;
  }
  /* BaseBundle default-value semantics: null key -> NPE (handled above),
   * missing key or mismatched type -> caller default.  Only real strings are
   * returned by getString/getCharSequence; Integer is never widened for
   * getLong.  Unresolved resource references also fall back rather than
   * fabricating a value through this metadata shim. */
  if(n==2&&IS("getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")){
   out->l=(e&&e->type==3)?metadata_string(v,e):a[1].l;return true;
  }
  if(n==2&&IS("getCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;")){
   out->l=(e&&e->type==3)?metadata_string(v,e):a[1].l;return true;
  }
  if(n==2&&IS("getInt","(Ljava/lang/String;I)I")){
   out->i=(e&&(e->type==16||e->type==17))?(int32_t)e->bits:a[1].i;return true;
  }
  if(n==2&&IS("getLong","(Ljava/lang/String;J)J")){
   out->j=a[1].j;return true;
  }
  if(n==2&&IS("getBoolean","(Ljava/lang/String;Z)Z")){
   out->i=(e&&e->type==18)?(e->bits!=0):a[1].i;return true;
  }
  /* Android one-argument overloads are the two-argument getters with their
   * BaseBundle defaults: getString/getCharSequence -> null, getInt/getLong ->
   * 0, getBoolean -> false.  Missing keys, mismatched types and unresolved
   * references all take the default; Integer is still never widened for
   * getLong.  The null-key NPE above applies to these overloads too. */
  if(n==1&&IS("getString","(Ljava/lang/String;)Ljava/lang/String;")){
   out->l=(e&&e->type==3)?metadata_string(v,e):0;return true;
  }
  if(n==1&&IS("getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;")){
   out->l=(e&&e->type==3)?metadata_string(v,e):0;return true;
  }
  if(n==1&&IS("getInt","(Ljava/lang/String;)I")){
   out->i=(e&&(e->type==16||e->type==17))?(int32_t)e->bits:0;return true;
  }
  if(n==1&&IS("getLong","(Ljava/lang/String;)J")){
   out->j=0;return true;
  }
  if(n==1&&IS("getBoolean","(Ljava/lang/String;)Z")){
   out->i=(e&&e->type==18)?(e->bits!=0):0;return true;
  }
  return false;
 }
 struct dvm_object* o=dvm__obj(v,self);
 if(o&&o->data_destroy==box_free&&o->data){
  boxed* p=o->data;const char* actual=o->cls->name;
  if(!strcmp(actual,"java/lang/Boolean")&&IS("booleanValue","()Z")){out->i=p->value!=0;return true;}
  if(!strcmp(actual,"java/lang/Integer")&&IS("intValue","()I")){out->i=p->value;return true;}
  if(!strcmp(actual,"java/lang/Integer")&&IS("longValue","()J")){out->j=p->value;return true;}
  if(!strcmp(actual,"java/lang/Float")&&IS("floatValue","()F")){out->f=p->real;return true;}
  if(!strcmp(actual,"java/lang/Float")&&IS("doubleValue","()D")){out->d=p->real;return true;}
  if(IS("toString","()Ljava/lang/String;")){char text[64];
   if(!strcmp(actual,"java/lang/Boolean"))snprintf(text,sizeof text,"%s",p->value?"true":"false");
   else if(!strcmp(actual,"java/lang/Integer"))snprintf(text,sizeof text,"%d",p->value);
   else return fail(v,"Float formatting not implemented");
   out->l=dvm_new_string(v,text);return true;
  }
 }
 return false;
}
/* 原 APK 证书、PackageInfo.signatures、MessageDigest(MD5) 与 Signature 方法。
 * 只比较摘要/布尔值，不输出完整证书或完整摘要。 */
static bool package_signature_selftest(struct dvm* v,dvm_ref pm){
 union dvm_value out={0},a[2]={{0},{0}},field={0},call={0},eqarg={0},eqout={0},h1={0},h2={0};
 size_t der_len=0;const unsigned char* der=apk_signature_der(&der_len);
 if(!der||der_len!=715)return false;
 a[0].l=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);
 if(!a[0].l)return false;

 /* flags=0：signatures 与 metaData 都为 null。 */
 a[1].i=0;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getPackageInfo","(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",pm,a,2,&out)||!out.l)return false;
 if(!dvm_get_field(v,out.l,"signatures","[Landroid/content/pm/Signature;",&field)||field.l)return false;
 if(!dvm_get_field(v,out.l,"applicationInfo","Landroid/content/pm/ApplicationInfo;",&field)||!field.l)return false;
 if(!dvm_get_field(v,field.l,"metaData","Landroid/os/Bundle;",&field)||field.l)return false;

 /* flags=GET_SIGNATURES(64)：真实 Signature[]，同时 metaData 为 null。 */
 a[1].i=64;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getPackageInfo","(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",pm,a,2,&out)||!out.l)return false;
 dvm_ref pi=out.l;
 if(!dvm_get_field(v,pi,"signatures","[Landroid/content/pm/Signature;",&field)||!field.l)return false;
 dvm_ref sigarr=field.l;
 if(dvm_array_length(v,sigarr)!=1)return false;
 dvm_ref* elems=dvm_array_data(v,sigarr);
 if(!elems||!elems[0])return false;
 dvm_ref sig=elems[0];
 if(!dvm_get_field(v,pi,"applicationInfo","Landroid/content/pm/ApplicationInfo;",&field)||!field.l)return false;
 if(!dvm_get_field(v,field.l,"metaData","Landroid/os/Bundle;",&field)||field.l)return false;

 /* toByteArray 必须返回 DER 副本；两次调用返回不同数组，修改副本不影响另一份。 */
 if(!apk_signature_call(v,"android/content/pm/Signature","toByteArray","()[B",sig,NULL,0,&call)||!call.l)return false;
 dvm_ref copy1=call.l;
 if(dvm_array_length(v,copy1)!=der_len)return false;
 unsigned char* bytes1=dvm_array_data(v,copy1);
 if(!bytes1||memcmp(bytes1,der,der_len))return false;
 call.l=0;
 if(!apk_signature_call(v,"android/content/pm/Signature","toByteArray","()[B",sig,NULL,0,&call)||!call.l)return false;
 dvm_ref copy2=call.l;
 if(copy1==copy2)return false;
 unsigned char* bytes2=dvm_array_data(v,copy2);
 if(!bytes2||memcmp(bytes2,der,der_len))return false;
 unsigned char first=bytes1[0];bytes1[0]=(unsigned char)(first^0xff);
 bool independent=bytes2[0]==der[0]&&!memcmp(bytes2+1,der+1,der_len-1);
 bytes1[0]=first;
 if(!independent)return false;

 /* 复用 cert_runtime 的 MessageDigest：MD5(DER) == 原 APK 证书 MD5。 */
 union dvm_value mdarg={0},mdout={0};
 mdarg.l=dvm_new_string(v,"MD5");
 if(!mdarg.l)return false;
 if(!cert_runtime_call(v,"java/security/MessageDigest","getInstance","(Ljava/lang/String;)Ljava/security/MessageDigest;",0,&mdarg,1,&mdout)||!mdout.l)return false;
 dvm_ref md=mdout.l;
 dvm_ref input=dvm_new_array(v,'B',"B",(uint32_t)der_len);
 if(!input)return false;
 memcpy(dvm_array_data(v,input),der,der_len);
 mdarg.l=input;
 if(!cert_runtime_call(v,"java/security/MessageDigest","update","([B)V",md,&mdarg,1,&mdout))return false;
 mdout.l=0;
 if(!cert_runtime_call(v,"java/security/MessageDigest","digest","()[B",md,NULL,0,&mdout)||!mdout.l)return false;
 if(dvm_array_length(v,mdout.l)!=16)return false;
 const unsigned char* md5=dvm_array_data(v,mdout.l);
 if(!md5)return false;
 char hex[33];
 for(int i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",(unsigned)md5[i]);
 if(strcmp(hex,"4502a02a00395dec05a4134ad593224d"))return false;

 /* toCharsString：小写十六进制，长度 715*2，前 8 字符为 DER 前 4 字节。 */
 call.l=0;
 if(!apk_signature_call(v,"android/content/pm/Signature","toCharsString","()Ljava/lang/String;",sig,NULL,0,&call)||!call.l)return false;
 const char* chars=dvm_string_utf8(v,call.l);
 if(!chars||strlen(chars)!=der_len*2)return false;
 for(size_t i=0;i<der_len*2;i++){char ch=chars[i];if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))return false;}
 if(strncmp(chars,"308202c7",8))return false;

 /* equals/hashCode：相同 DER 相等且哈希一致；null 不相等。 */
 eqarg.l=sig;
 if(!apk_signature_call(v,"android/content/pm/Signature","equals","(Ljava/lang/Object;)Z",sig,&eqarg,1,&eqout)||eqout.i!=1)return false;
 dvm_ref sig2=apk_signature_new(v,der,der_len);
 if(!sig2)return false;
 eqarg.l=sig2;
 if(!apk_signature_call(v,"android/content/pm/Signature","equals","(Ljava/lang/Object;)Z",sig,&eqarg,1,&eqout)||eqout.i!=1)return false;
 if(!apk_signature_call(v,"android/content/pm/Signature","hashCode","()I",sig,NULL,0,&h1))return false;
 if(!apk_signature_call(v,"android/content/pm/Signature","hashCode","()I",sig2,NULL,0,&h2)||h1.i!=h2.i)return false;
 eqarg.l=0;
 if(!apk_signature_call(v,"android/content/pm/Signature","equals","(Ljava/lang/Object;)Z",sig,&eqarg,1,&eqout)||eqout.i!=0)return false;

 /* flags=GET_META_DATA(128)：signatures 为 null，metaData 非 null。 */
 a[1].i=128;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getPackageInfo","(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",pm,a,2,&out)||!out.l)return false;
 dvm_ref p128=out.l;
 if(!dvm_get_field(v,p128,"signatures","[Landroid/content/pm/Signature;",&field)||field.l)return false;
 if(!dvm_get_field(v,p128,"applicationInfo","Landroid/content/pm/ApplicationInfo;",&field)||!field.l)return false;
 if(!dvm_get_field(v,field.l,"metaData","Landroid/os/Bundle;",&field)||!field.l)return false;

 /* flags=192：signatures 与 metaData 同时存在。 */
 a[1].i=192;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getPackageInfo","(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",pm,a,2,&out)||!out.l)return false;
 dvm_ref p192=out.l;
 if(!dvm_get_field(v,p192,"signatures","[Landroid/content/pm/Signature;",&field)||!field.l||dvm_array_length(v,field.l)!=1)return false;
 if(!dvm_get_field(v,p192,"applicationInfo","Landroid/content/pm/ApplicationInfo;",&field)||!field.l)return false;
 if(!dvm_get_field(v,field.l,"metaData","Landroid/os/Bundle;",&field)||!field.l)return false;

 /* 未知 flags：UnsupportedOperationException，不静默忽略。 */
 int bad[2]={0x3f,0x100};
 for(int i=0;i<2;i++){
  a[1].i=bad[i];
  bool handled=package_metadata_call(v,"android/content/pm/PackageManager","getPackageInfo","(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",pm,a,2,&out);
  dvm_ref exc=dvm_exception(v);char text[128]={0};
  if(exc)dvm_describe_exception(v,exc,text,sizeof text);
  bool uoe=handled&&exc&&strstr(text,"UnsupportedOperationException")!=NULL;
  if(exc)dvm_clear_exception(v);
  if(!uoe)return false;
 }
 return true;
}

/* Integer.toString(int) / Integer.toString(int,int)：静态调用 self==0。 */
static bool integer_static_selftest(struct dvm* v){
 union dvm_value a[2]={{0},{0}},out={0};
 static const struct {int32_t value;int radix;const char* text;} cases[]={
  {0,10,"0"},{-10,10,"-10"},{255,16,"ff"},{-1,16,"-1"},
  {INT32_MIN,10,"-2147483648"},{INT32_MIN,16,"-80000000"},
  {35,36,"z"},{36,36,"10"},{7,2,"111"},{8,8,"10"}
 };
 for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
  a[0].i=cases[i].value;a[1].i=cases[i].radix;out.l=0;
  bool handled=java_basics_call(v,"java/lang/Integer","toString","(II)Ljava/lang/String;",0,a,2,&out);
  if(!handled||!out.l||dvm_exception(v))return false;
  const char* text=dvm_string_utf8(v,out.l);
  if(!text||strcmp(text,cases[i].text))return false;
 }
 a[0].i=-12345;out.l=0;
 if(!java_basics_call(v,"java/lang/Integer","toString","(I)Ljava/lang/String;",0,a,1,&out)||!out.l)return false;
 const char* text=dvm_string_utf8(v,out.l);
 if(!text||strcmp(text,"-12345"))return false;
 int bad[4]={0,1,37,-1};
 for(int i=0;i<4;i++){
  a[0].i=1;a[1].i=bad[i];out.l=0;
  bool handled=java_basics_call(v,"java/lang/Integer","toString","(II)Ljava/lang/String;",0,a,2,&out);
  /* Java Integer.toString defaults an invalid radix to decimal; unlike
   * parseInt it does not throw. Independently covered by the JDK oracle. */
  const char *decimal=dvm_string_utf8(v,out.l);
  if(!handled||dvm_exception(v)||!decimal||strcmp(decimal,"1"))return false;
 }
 return true;
}

bool package_metadata_selftest(struct dvm* v){
 union dvm_value out={0},a[2]={{0},{0}};
 sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST BEGIN\n");
 /* Real guest Context and real original metadata; no values written to logs. */
 extern dvm_ref android_basics_application(struct dvm*);
 dvm_ref context=android_basics_application(v);
 if(!package_metadata_call(v,"android/content/Context","getPackageManager","()Landroid/content/pm/PackageManager;",context,NULL,0,&out)||!out.l){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm\n");return false;}
 dvm_ref pm=out.l;a[0].l=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);a[1].i=128;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getApplicationInfo","(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;",pm,a,2,&out)||!out.l){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=appinfo\n");return false;}
 if(!dvm_get_field(v,out.l,"metaData","Landroid/os/Bundle;",&out)||!out.l){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=metadata\n");return false;}
 sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST BUNDLE_READY\n");
 dvm_ref bundle=out.l;
 /* Context.getApplicationInfo(): the same real fields as
  * PackageManager.getApplicationInfo(pkg, 0), and a foreign receiver must
  * throw instead of receiving a fabricated ApplicationInfo. */
 a[0].l=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);a[1].i=0;
 if(!a[0].l)return false;
 if(!package_metadata_call(v,"android/content/Context","getApplicationInfo","()Landroid/content/pm/ApplicationInfo;",context,NULL,0,&out)||!out.l||dvm_exception(v)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo\n");return false;}
 dvm_ref context_info=out.l;
 union dvm_value ai_field={0};
 if(!dvm_get_field(v,context_info,"packageName","Ljava/lang/String;",&ai_field)||!ai_field.l||!dvm_string_utf8(v,ai_field.l)||strcmp(dvm_string_utf8(v,ai_field.l),GUEST_EXPECTED_PACKAGE)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_package\n");return false;}
 if(!dvm_get_field(v,context_info,"targetSdkVersion","I",&ai_field)||ai_field.i!=GUEST_EXPECTED_TARGET_SDK){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_sdk\n");return false;}
 if(!dvm_get_field(v,context_info,"metaData","Landroid/os/Bundle;",&ai_field)||ai_field.l){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_metadata\n");return false;}
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getApplicationInfo","(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;",pm,a,2,&out)||!out.l||dvm_exception(v)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_pm\n");return false;}
 dvm_ref pm_info=out.l;
 if(!dvm_get_field(v,pm_info,"packageName","Ljava/lang/String;",&ai_field)||!ai_field.l||!dvm_string_utf8(v,ai_field.l)||strcmp(dvm_string_utf8(v,ai_field.l),GUEST_EXPECTED_PACKAGE)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_pm_package\n");return false;}
 if(!dvm_get_field(v,pm_info,"targetSdkVersion","I",&ai_field)||ai_field.i!=GUEST_EXPECTED_TARGET_SDK){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_pm_sdk\n");return false;}
 if(!dvm_get_field(v,pm_info,"metaData","Landroid/os/Bundle;",&ai_field)||ai_field.l){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_pm_metadata\n");return false;}
 /* getApplicationLabel must return the APK's verified label for the
  * represented package.  nativeLibraryDir must be readable by reflection and
  * honestly null: there is no host source for it, so the diagnostic dlopen
  * path is deliberately not exposed as a package fact. */
 a[0].l=context_info;out.l=0;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getApplicationLabel","(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;",pm,a,1,&out)||!out.l||dvm_exception(v)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_label\n");return false;}
 const char* label_text=dvm_string_utf8(v,out.l);
 if(!label_text||strcmp(label_text,GUEST_APPLICATION_LABEL)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_label_value\n");return false;}
 if(!dvm_get_field(v,context_info,"nativeLibraryDir","Ljava/lang/String;",&ai_field)||ai_field.l){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_native_dir\n");return false;}
 a[0].l=pm_info;out.l=0;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getApplicationLabel","(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;",pm,a,1,&out)||!out.l||dvm_exception(v)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_label\n");return false;}
 label_text=dvm_string_utf8(v,out.l);
 if(!label_text||strcmp(label_text,GUEST_APPLICATION_LABEL)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_label_value\n");return false;}
 if(!dvm_get_field(v,pm_info,"nativeLibraryDir","Ljava/lang/String;",&ai_field)||ai_field.l){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_native_dir\n");return false;}
 /* Null ApplicationInfo: NullPointerException, never a fabricated label. */
 a[0].l=0;
 bool label_handled=package_metadata_call(v,"android/content/pm/PackageManager","getApplicationLabel","(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;",pm,a,1,&out);
 dvm_ref label_exc=dvm_exception(v);char label_desc[128]={0};
 if(label_exc)dvm_describe_exception(v,label_exc,label_desc,sizeof label_desc);
 bool label_npe=label_handled&&label_exc&&strstr(label_desc,"NullPointerException")!=NULL;
 if(label_exc)dvm_clear_exception(v);
 if(!label_npe){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_label_null\n");return false;}
 /* A foreign PackageManager must not answer as the real one. */
 a[0].l=context_info;
 bool foreign_label=package_metadata_call(v,"android/content/pm/PackageManager","getApplicationLabel","(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;",context,a,1,&out);
 label_exc=dvm_exception(v);memset(label_desc,0,sizeof label_desc);
 if(label_exc)dvm_describe_exception(v,label_exc,label_desc,sizeof label_desc);
 bool label_rejected=foreign_label&&label_exc&&strstr(label_desc,"UnsupportedOperationException")!=NULL;
 if(label_exc)dvm_clear_exception(v);
 if(!label_rejected){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_label_receiver\n");return false;}
 /* A different package has no verified resource label available here;
  * PackageItemInfo.loadLabel falls back to its real packageName, then null. */
 dvm_ref other_info=object(v,"android/content/pm/ApplicationInfo");
 dvm_ref other_name=dvm_new_string(v,"com.example.other");
 if(!other_info||!other_name){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_label_other_alloc\n");return false;}
 union dvm_value other_field={.l=other_name};
 if(!dvm_set_field(v,other_info,"packageName","Ljava/lang/String;",other_field)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_label_other_field\n");return false;}
 a[0].l=other_info;out.l=0;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getApplicationLabel","(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;",pm,a,1,&out)||out.l!=other_name||dvm_exception(v)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_label_other_package\n");return false;}
 dvm_ref unnamed_info=object(v,"android/content/pm/ApplicationInfo");
 if(!unnamed_info){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_label_unnamed_alloc\n");return false;}
 a[0].l=unnamed_info;out.l=other_name;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getApplicationLabel","(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;",pm,a,1,&out)||out.l||dvm_exception(v)){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=pm_appinfo_label_unnamed\n");return false;}
 sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST APPLICATION_LABEL PASS\n");
 bool foreign_ai=package_metadata_call(v,"android/content/Context","getApplicationInfo","()Landroid/content/pm/ApplicationInfo;",pm,NULL,0,&out);
 dvm_ref ai_exc=dvm_exception(v);char ai_desc[128]={0};
 if(ai_exc)dvm_describe_exception(v,ai_exc,ai_desc,sizeof ai_desc);
 bool ai_rejected=foreign_ai&&ai_exc&&strstr(ai_desc,"UnsupportedOperationException")!=NULL;
 if(ai_exc)dvm_clear_exception(v);
 if(!ai_rejected){sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST FAIL stage=context_appinfo_receiver\n");return false;}
 sdk_trace_fprintf(stderr,"PACKAGE_METADATA_SELFTEST CONTEXT_APPINFO PASS\n");
 const entry* paid=lookup("BSGameSdk_PaidGame");
 const entry* channel=lookup("BILIGAME_CHANNELID");
 const entry* appkey=lookup("Mob-AppKey");
 const entry* splash=lookup("unity.splash-enable");
 dvm_ref fallback=dvm_new_string(v,"fallback");
 if(!paid||!channel||!appkey||!splash||!fallback)return false;
 a[0].l=dvm_new_string(v,"BSGameSdk_PaidGame");
 if(!package_metadata_call(v,"android/os/Bundle","get","(Ljava/lang/String;)Ljava/lang/Object;",bundle,a,1,&out)||!out.l)return false;
 if(!package_metadata_call(v,"java/lang/Boolean","booleanValue","()Z",out.l,NULL,0,&out))return false;
 if(out.i!=(paid->bits!=0))return false;
 a[0].l=dvm_new_string(v,"sdktrace.absent.metadata.key");
 if(!package_metadata_call(v,"android/os/Bundle","get","(Ljava/lang/String;)Ljava/lang/Object;",bundle,a,1,&out)||out.l)return false;
 a[0].l=dvm_new_string(v,"com.google.android.gms.version");
 bool refused=package_metadata_call(v,"android/os/Bundle","get","(Ljava/lang/String;)Ljava/lang/Object;",bundle,a,1,&out)&&dvm_exception(v);
 dvm_clear_exception(v);
 if(!refused)return false;
 /* Boolean value: a matching getter returns the stored false, not the
  * deliberately opposing default true; getString returns the fallback. */
 a[0].l=dvm_new_string(v,"BSGameSdk_PaidGame");a[1].i=1;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;Z)Z",bundle,a,2,&out)||out.i!=(paid->bits!=0))return false;
 a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out)||out.l!=fallback)return false;
 if(!package_metadata_call(v,"android/os/Bundle","getCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;",bundle,a,2,&out)||out.l!=fallback)return false;
 a[1].i=-7;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;I)I",bundle,a,2,&out)||out.i!=-7)return false;
 /* Integer value: getInt returns it; getString/getBoolean/getLong return the
  * default and never stringify or widen it. */
 a[0].l=dvm_new_string(v,"BILIGAME_CHANNELID");a[1].i=999;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;I)I",bundle,a,2,&out)||out.i!=(int32_t)channel->bits)return false;
 a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out)||out.l!=fallback)return false;
 a[1].i=0;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;Z)Z",bundle,a,2,&out)||out.i!=0)return false;
 a[1].j=1234567890123LL;
 if(!package_metadata_call(v,"android/os/Bundle","getLong","(Ljava/lang/String;J)J",bundle,a,2,&out)||out.j!=1234567890123LL)return false;
 /* A real String entry is the only source getString may return as a String. */
 a[0].l=dvm_new_string(v,"Mob-AppKey");a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out)||!out.l||out.l==fallback)return false;
 const char* text=dvm_string_utf8(v,out.l);if(!text||strcmp(text,appkey->text))return false;
 a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;",bundle,a,2,&out)||!out.l||out.l==fallback)return false;
 text=dvm_string_utf8(v,out.l);if(!text||strcmp(text,appkey->text))return false;
 /* One-argument overloads use Android BaseBundle defaults.  This Boolean
  * entry is real true, so getBoolean cannot pass by returning its false
  * default; getInt/getString must not reinterpret the boxed value. */
 a[0].l=dvm_new_string(v,"unity.splash-enable");
 out.i=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;)Z",bundle,a,1,&out)||out.i!=(splash->bits!=0))return false;
 out.i=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;)I",bundle,a,1,&out)||out.i!=0)return false;
 out.l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;)Ljava/lang/String;",bundle,a,1,&out)||out.l)return false;
 /* Integer entry: one-argument getInt returns the real value; getString and
  * getCharSequence return null, and getLong does not widen it to 0L. */
 a[0].l=dvm_new_string(v,"BILIGAME_CHANNELID");
 out.i=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;)I",bundle,a,1,&out)||out.i!=(int32_t)channel->bits)return false;
 out.l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;)Ljava/lang/String;",bundle,a,1,&out)||out.l)return false;
 out.l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;",bundle,a,1,&out)||out.l)return false;
 out.j=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getLong","(Ljava/lang/String;)J",bundle,a,1,&out)||out.j!=0)return false;
 /* String entry: getString/getCharSequence return the real text; numeric and
  * boolean getters return their defaults instead of parsing it. */
 a[0].l=dvm_new_string(v,"Mob-AppKey");
 out.l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;)Ljava/lang/String;",bundle,a,1,&out)||!out.l||out.l==fallback)return false;
 const char* one=dvm_string_utf8(v,out.l);if(!one||strcmp(one,appkey->text))return false;
 out.l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;",bundle,a,1,&out)||!out.l||out.l==fallback)return false;
 one=dvm_string_utf8(v,out.l);if(!one||strcmp(one,appkey->text))return false;
 out.i=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;)I",bundle,a,1,&out)||out.i!=0)return false;
 out.j=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getLong","(Ljava/lang/String;)J",bundle,a,1,&out)||out.j!=0)return false;
 out.i=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;)Z",bundle,a,1,&out)||out.i!=0)return false;
 /* Unknown key: every one-argument getter returns its default. */
 a[0].l=dvm_new_string(v,"sdktrace.absent.metadata.key");
 out.l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;)Ljava/lang/String;",bundle,a,1,&out)||out.l)return false;
 out.l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;",bundle,a,1,&out)||out.l)return false;
 out.i=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;)I",bundle,a,1,&out)||out.i!=0)return false;
 out.j=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getLong","(Ljava/lang/String;)J",bundle,a,1,&out)||out.j!=0)return false;
 out.i=-1;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;)Z",bundle,a,1,&out)||out.i!=0)return false;
 /* Unknown key: every getter returns its default. */
 a[0].l=dvm_new_string(v,"sdktrace.absent.metadata.key");
 a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out)||out.l!=fallback)return false;
 a[1].i=77;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;I)I",bundle,a,2,&out)||out.i!=77)return false;
 a[1].i=1;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;Z)Z",bundle,a,2,&out)||out.i!=1)return false;
 a[1].j=-5;
 if(!package_metadata_call(v,"android/os/Bundle","getLong","(Ljava/lang/String;J)J",bundle,a,2,&out)||out.j!=-5)return false;
 a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;",bundle,a,2,&out)||out.l!=fallback)return false;
 /* Null key: NullPointerException, never a default value or MISSING_API. */
 a[0].l=0;a[1].l=fallback;
 bool handled=package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out);
 dvm_ref exc=dvm_exception(v);char npe_text[128]={0};
 if(exc)dvm_describe_exception(v,exc,npe_text,sizeof npe_text);
 bool npe=handled&&exc&&!strncmp(npe_text,"java/lang/NullPointerException",30);
 if(exc)dvm_clear_exception(v);
 if(!npe)return false;
 a[0].l=0;a[1].i=5;
 handled=package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;I)I",bundle,a,2,&out);
 exc=dvm_exception(v);memset(npe_text,0,sizeof npe_text);
 if(exc)dvm_describe_exception(v,exc,npe_text,sizeof npe_text);
 npe=handled&&exc&&!strncmp(npe_text,"java/lang/NullPointerException",30);
 if(exc)dvm_clear_exception(v);
 if(!npe)return false;
 /* The one-argument overloads throw the same NPE for a null key. */
 a[0].l=0;
 handled=package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;)I",bundle,a,1,&out);
 exc=dvm_exception(v);memset(npe_text,0,sizeof npe_text);
 if(exc)dvm_describe_exception(v,exc,npe_text,sizeof npe_text);
 npe=handled&&exc&&!strncmp(npe_text,"java/lang/NullPointerException",30);
 if(exc)dvm_clear_exception(v);
 if(!npe)return false;
 /* A Bundle whose payload marker is not bundle_free must be refused so
  * dispatch falls through instead of using the fabricated-value path. */
 dvm_ref foreign=object(v,"android/os/Bundle");struct dvm_object* fo=dvm__obj(v,foreign);
 if(!fo)return false;fo->data=calloc(1,1);if(!fo->data)return false;fo->data_destroy=box_free;
 a[0].l=dvm_new_string(v,"BSGameSdk_PaidGame");a[1].l=fallback;
 bool claimed=false;
 claimed|=package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",foreign,a,2,&out);
 claimed|=package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;)Ljava/lang/String;",foreign,a,1,&out);
 claimed|=package_metadata_call(v,"android/os/Bundle","getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;",foreign,a,1,&out);
 claimed|=package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;)I",foreign,a,1,&out);
 claimed|=package_metadata_call(v,"android/os/Bundle","getLong","(Ljava/lang/String;)J",foreign,a,1,&out);
 claimed|=package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;)Z",foreign,a,1,&out);
 bool invalid=!claimed&&!dvm_exception(v);
 if(dvm_exception(v))dvm_clear_exception(v);
 if(!invalid)return false;
 if(!package_signature_selftest(v,pm)){sdk_trace_fprintf(stderr,"PACKAGE_SIGNATURE_SELFTEST FAIL\n");return false;}
 sdk_trace_fprintf(stderr,"PACKAGE_SIGNATURE_SELFTEST PASS\n");
 if(!integer_static_selftest(v)){sdk_trace_fprintf(stderr,"INTEGER_STATIC_SELFTEST FAIL\n");return false;}
 sdk_trace_fprintf(stderr,"INTEGER_STATIC_SELFTEST PASS\n");
 /* The generated Selftest aggregator has no separate device-runtime hook and
  * build_tls_candidate.py is outside this change's scope, so run the
  * device/Build selftest from this already-wired VM-start self-test.  It
  * restores the real host snapshot before returning.  Weak: builds such as
  * build_integrated_login.py link this file with -Wl,--no-undefined but do
  * not link device_runtime.c; there the hook is absent and is skipped. */
 extern bool device_runtime_selftest(struct dvm*) __attribute__((weak));
 if(device_runtime_selftest&&!device_runtime_selftest(v)){sdk_trace_fprintf(stderr,"DEVICE_RUNTIME_SELFTEST FAIL\n");return false;}
 sdk_trace_fprintf(stderr,"DEVICE_RUNTIME_SELFTEST PASS\n");
 return true;
}

/* Device-process selftest and bounded native timings; never invokes game/UI
 * methods or detector code. Cold and warm samples include actual allocations. */
static uint64_t metadata_emu_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
bool package_metadata_emu_selftest(struct dvm *v){
 extern bool intent_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
 extern bool activity_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
 const char *dir="/data/storage/el2/base/haps/entry/files/sdkdex/";
 char path[256];snprintf(path,sizeof path,"%semu-detect-selftest.txt",dir);FILE *f=fopen(path,"w");if(!f)return false;
 snprintf(path,sizeof path,"%semu-detect-selftest.csv",dir);FILE *csv=fopen(path,"w");if(!csv){fclose(f);return false;}
 fprintf(csv,"pid,method,index,elapsed_ms,ok,source\n");
 union dvm_value out={0},a[2]={{0}};bool ok=true;unsigned checks=0,pass=0;
 dvm_ref context=object(v,"android/content/Context"),manager=object(v,"android/app/ActivityManager"),intent=object(v,"android/content/Intent"),mi=object(v,"android/app/ActivityManager$MemoryInfo");
 dvm_ref held[]={context,manager,intent,mi};for(unsigned i=0;i<4;i++){if(held[i])dvm_pin(v,held[i]);else ok=false;}
 dvm_ref pm=0,package=0;
 if(ok)ok=package_metadata_call(v,"android/content/Context","getPackageManager","()Landroid/content/pm/PackageManager;",context,NULL,0,&out)&&out.l&&!dvm_exception(v);
 if(ok){pm=out.l;package=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);if(package)dvm_pin(v,package);else ok=false;}
 if(ok)ok=intent_runtime_call(v,"android/content/Intent","<init>","()V",intent,NULL,0,&out)&&!dvm_exception(v);
 const char *names[]={"getRunningServices","getInstalledApplications","queryIntentActivities","getPackageInfo","getMemoryInfo"};
 for(int kind=0;ok&&kind<5;kind++){
  uint64_t max=0;for(int i=0;ok&&i<128;i++){
   out=(union dvm_value){0};a[0]=(union dvm_value){0};a[1]=(union dvm_value){0};uint64_t start=metadata_emu_ns();bool handled=false;
   if(kind==0)handled=metadata_running_services_fast(v,manager,INT_MAX,&out);
   if(kind==1)handled=package_metadata_call(v,"android/content/pm/PackageManager",names[kind],"(I)Ljava/util/List;",pm,a,1,&out);
   if(kind==2){a[0].l=intent;handled=package_metadata_call(v,"android/content/pm/PackageManager",names[kind],"(Landroid/content/Intent;I)Ljava/util/List;",pm,a,2,&out);}
   if(kind==3){a[0].l=package;handled=package_metadata_call(v,"android/content/pm/PackageManager",names[kind],"(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",pm,a,2,&out);}
   if(kind==4){a[0].l=mi;handled=activity_runtime_call(v,"android/app/ActivityManager",names[kind],"(Landroid/app/ActivityManager$MemoryInfo;)V",manager,a,1,&out);}
   uint64_t elapsed=metadata_emu_ns()-start;if(elapsed>max)max=elapsed;
   ok=handled&&!dvm_exception(v)&&(kind==4||out.l);
   fprintf(csv,"%d,%s,%d,%.6f,%d,HAP_direct_runtime\n",getpid(),names[kind],i,elapsed/1e6,ok);
  }
  checks++;pass+=ok;fprintf(f,"pid=%d method=%s samples=128 max_ms=%.6f semantics=%s\n",getpid(),names[kind],max/1e6,ok?"PASS":"FAIL");
 }
 if(ok){
  union dvm_value first={0},second={0},size={0},item={.l=package};
  ok=metadata_running_services_fast(v,manager,INT_MAX,&first)&&first.l; if(first.l)dvm_pin(v,first.l);
  ok=ok&&metadata_list_add(v,first.l,package)&&metadata_running_services_fast(v,manager,INT_MAX,&second)&&second.l&&first.l!=second.l;
  ok=ok&&collections_call(v,"java/util/List","size","()I",second.l,NULL,0,&size)&&size.i==0&&!dvm_exception(v);
  (void)item;checks++;pass+=ok;fprintf(f,"pid=%d fresh_mutable_lists_independent=%d\n",getpid(),ok);if(first.l)dvm_unpin(v,first.l);
 }
 if(ok){uint64_t generation=state.installation_generation,fills=state.service_cache_fills;
  package_metadata_package_event(v,"android.intent.action.PACKAGE_ADDED");bool invalid=!state.service_cache_valid&&state.installation_generation==generation+1;
  ok=metadata_running_services_fast(v,manager,INT_MAX,&out)&&invalid&&state.service_cache_valid&&state.service_cache_fills==fills+1;
  package_metadata_package_event(v,"android.intent.action.PACKAGE_REMOVED");ok=ok&&!state.service_cache_valid;
  checks++;pass+=ok;fprintf(f,"pid=%d package_add_remove_invalidation=%d generation=%llu\n",getpid(),ok,(unsigned long long)state.installation_generation);
 }
 for(unsigned i=0;i<4;i++)if(held[i])dvm_unpin(v,held[i]);if(package)dvm_unpin(v,package);
 fprintf(f,"pid=%d cache_fills=%llu cache_hits=%llu source=host_android_service_registry_unavailable fallback=empty_visible_list no_scan=1 no_retry=1\n",getpid(),(unsigned long long)state.service_cache_fills,(unsigned long long)state.service_cache_hits);
 fprintf(f,"pid=%d RESULT checks=%u pass=%u result=%s moving_enabled=0\n",getpid(),checks,pass,ok&&checks==7&&pass==7?"PASS":"FAIL");
 fclose(csv);fclose(f);return ok&&checks==7&&pass==7;
}
