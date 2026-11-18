#include <time.h>
#include <unistd.h>
#include "platform_runtime.h"
#include "gc_operation.h"
#include "java_basics.h"
#include "locale_list.h"
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

/* capture_count is the regexec nmatch slot count (whole match + POSIX
 * groups).  java_groups is the Java Pattern groupCount(), which may differ
 * because POSIX cannot express non-capturing groups; group_map translates a
 * Java group index to its POSIX slot (identity unless remapped). */
#define MATCH_SLOTS 8
typedef struct { regex_t regex; int compiled; int capture_count; int java_groups; int group_map[MATCH_SLOTS]; } pattern_data;
typedef struct { struct dvm *vm; dvm_ref pattern; dvm_ref input; size_t next;
                 size_t region_start, region_end, match_start, match_end;
                 regmatch_t groups[MATCH_SLOTS]; int ngroups; int matched; } matcher_data;
static struct dvm *active;
static dvm_ref gmt, utc;
typedef bool (*timezone_host_getter)(char *,size_t,int *);
static timezone_host_getter timezone_host;
__attribute__((visibility("default"))) void sdk_set_timezone_host(timezone_host_getter fn){timezone_host=fn;}
struct host_timezone {char id[128];int raw_offset;};
static void host_timezone_free(void *p){free(p);}
static dvm_ref new_host_timezone(struct dvm *v,const struct host_timezone *source){
 /* source may be an object payload: snapshot before any VM call. */
 const struct host_timezone saved=*source;
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op))return 0;
 dvm_ref ref=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/TimeZone"));if(!ref)return 0;
 struct host_timezone *copy=malloc(sizeof *copy);if(!copy){dvm__discard_temporary(v,ref);dvm__throw(v,"java/lang/OutOfMemoryError","TimeZone snapshot");return 0;}
 *copy=saved;struct dvm_object *o=gc_operation_object(v,ref);if(!o){free(copy);return 0;}o->data=copy;o->data_destroy=host_timezone_free;gc_operation_result(&op,"()Ljava/util/TimeZone;",ref);return ref;
}
bool platform_timezone_selftest(struct dvm *v){
 dvm_ref saved=v->exception;bool parked=v->parked;uint64_t steps=v->call_steps;v->exception=0;v->parked=false;v->call_steps=0;
 struct host_timezone source={0};unsigned checks=0;bool ok=timezone_host&&timezone_host(source.id,sizeof source.id,&source.raw_offset);union dvm_value zone={0},other={0},q={0};
 struct dvm_class *k=dvm_find_class_or_external(v,"java/util/TimeZone");
#define TZCHECK(expr) do{if(!(expr)){ok=false;goto done;}checks++;}while(0)
 TZCHECK(ok&&dvm_class_exists(v,"Ljava/util/TimeZone;"));
 struct dvm_method *get=dvm_find_method(v,k,"getDefault","()Ljava/util/TimeZone;"),*raw=dvm_find_method(v,k,"getRawOffset","()I"),*id=dvm_find_method(v,k,"getID","()Ljava/lang/String;"),*clone=dvm_find_method(v,k,"clone","()Ljava/lang/Object;");
 TZCHECK(get&&raw&&id&&clone&&(get->access&DEX_ACC_STATIC));
 TZCHECK(dvm_call(v,get,0,NULL,0,&zone)&&zone.l&&!v->exception);
 TZCHECK(dvm_call(v,get,0,NULL,0,&other)&&other.l&&other.l!=zone.l&&!v->exception);
 TZCHECK(dvm_call(v,raw,zone.l,NULL,0,&q)&&q.i==source.raw_offset&&!v->exception);
 TZCHECK(dvm_call(v,id,zone.l,NULL,0,&q)&&q.l&&!strcmp(dvm_string_utf8(v,q.l),source.id)&&!v->exception);
 TZCHECK(dvm_call(v,clone,zone.l,NULL,0,&q)&&q.l&&q.l!=zone.l&&!v->exception);
 dvm_ref copied=q.l;TZCHECK(dvm_call(v,raw,copied,NULL,0,&q)&&q.i==source.raw_offset&&!v->exception);
done:
 {FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/timezone-selftest.txt","w");if(f){fprintf(f,"TIMEZONE_SELFTEST ok=%d checks=%u host_id=%s raw_offset_ms=%d exception=%u\n",ok,checks,source.id,source.raw_offset,v->exception);fclose(f);}}
 v->exception=saved;v->parked=parked;v->call_steps=steps;return ok;
#undef TZCHECK
}
static const char java_ip_regex[] = "([0-9a-fA-F]*:[0-9a-fA-F:.]*)|([\\d.]+)";
static const char posix_ip_regex[] = "([0-9a-fA-F]*:[0-9a-fA-F:.]*)|([0-9.]+)";
/* AndroidX MultiDex.isVMMultidexCapable() compiles this exact expression to
 * decide whether the interpreter can load several DEX files itself. */
static const char java_vm_version_regex[] = "(\\d+)\\.(\\d+)(\\.\\d+)?";
static const char posix_vm_version_regex[] = "([0-9]+)\\.([0-9]+)(\\.[0-9]+)?";
/* DiskLruCache (com/base/cachelib/core/diskcache) compiles this exact
 * expression in <clinit> for LEGAL_KEY_PATTERN.  It is also a portable POSIX
 * ERE, byte-for-byte: '-' is the final bracket member (literal in both Java
 * and POSIX), there are no escapes or Java-only constructs, and {1,120} is a
 * standard ERE interval (POSIX guarantees RE_DUP_MAX >= 255).  The class is
 * ASCII-only, so Java UTF-16 code units and POSIX bytes accept exactly the
 * same inputs; the Matcher bridge adds Java's whole-input matches() rule. */
static const char disk_lru_cache_key_regex[] = "[a-zA-Z0-9_-]{1,120}";
/* OkHttp MediaType compiles this exact TYPE/SUBTYPE expression.  Java treats
 * the '-' after 9 as a literal here; POSIX ERE would read it as a range, so
 * the POSIX form moves the hyphen to the end of the bracket expression.  The
 * two capture groups are type and subtype. */
static const char http_media_type_java_regex[] =
 "([a-zA-Z0-9-!#$%&'*+.^_`{|}~]+)/([a-zA-Z0-9-!#$%&'*+.^_`{|}~]+)";
static const char http_media_type_posix_regex[] =
 "([a-zA-Z0-9!#$%&'*+.^_`{|}~-]+)/([a-zA-Z0-9!#$%&'*+.^_`{|}~-]+)";
/* OkHttp MediaType compiles this exact PARAMETER expression.  Java's '-' in
 * the token set is literal, while POSIX ERE would read it as a range, so the
 * POSIX form moves the hyphen to the end of the bracket expression.  The two
 * Java non-capturing groups (?:...) become plain POSIX groups; the explicit
 * group map restores Java numbering: 1=name, 2=unquoted value, 3=quoted
 * value.  POSIX slots: 0=whole, 1=outer ?, 2=name, 3=value alternation,
 * 4=unquoted value, 5=quoted value. */
static const char http_media_parameter_java_regex[] =
 ";\\s*(?:([a-zA-Z0-9-!#$%&'*+.^_`{|}~]+)=(?:([a-zA-Z0-9-!#$%&'*+.^_`{|}~]+)|\"([^\"]*)\"))?";
static const char http_media_parameter_posix_regex[] =
 ";[[:space:]]*(([a-zA-Z0-9!#$%&'*+.^_`{|}~-]+)=(([a-zA-Z0-9!#$%&'*+.^_`{|}~-]+)|\"([^\"]*)\"))?";

/* ---- HarmonyOS display host bridge -------------------------------------
 * Real screen data can only come from the HarmonyOS host.  The host publishes
 * a snapshot built from:
 *   @ohos.display  display.getDefaultDisplaySync():
 *       width, height (px), densityDPI, densityPixels, scaledDensity,
 *       xDPI, yDPI, orientation
 *   @ohos.window   windowStage.getMainWindowSync().getWindowProperties():
 *       windowRect.width/height (visible px) when the caller wants the
 *       visible-window size instead of the whole display for screen*Dp.
 *
 * The snapshot is expressed in Android terms:
 *   densityPixels -> DisplayMetrics.density
 *   densityDPI    -> DisplayMetrics.densityDpi
 *   scaledDensity -> DisplayMetrics.scaledDensity
 *   xDPI/yDPI     -> DisplayMetrics.xdpi/ydpi
 *   orientation   -> Configuration.ORIENTATION_* (the host maps
 *                    @ohos.display PORTRAIT/PORTRAIT_INVERTED to 1,
 *                    LANDSCAPE/LANDSCAPE_INVERTED to 2, else 0)
 *   screen*Dp     = round(visible px / densityPixels)
 *   smallestScreenWidthDp = min(widthDp, heightDp) over both orientations
 *
 * `source` is an informational label such as
 * "@ohos.display.getDefaultDisplaySync + @ohos.window.getMainWindowSync";
 * the string is copied, so a temporary host buffer is safe.  Passing NULL
 * restores the explicit diagnostic fallback.  The call may happen at startup
 * or after an orientation change; cached DisplayMetrics/Configuration pick
 * the new generation up on their next read.  No VM object is pinned.
 * The field order below is the C ABI the native @ohos bridge declares. */
typedef struct platform_display_snapshot {
 uint32_t struct_size;                /* sizeof(platform_display_snapshot) */
 int32_t width_pixels,height_pixels,density_dpi,orientation;
 int32_t screen_width_dp,screen_height_dp,smallest_screen_width_dp;
 float density,scaled_density,xdpi,ydpi;
 const char *source;                  /* informational, copied; may be NULL */
} platform_display_snapshot;

/* Fallback only: Android's mdpi baseline with an explicitly unknown screen
 * size.  These are diagnostic placeholders, not measurements of the device. */
#define DISPLAY_FALLBACK_SOURCE \
 "diagnostic-fallback:no @ohos.display/@ohos.window snapshot (mdpi baseline, screen size unknown)"
static pthread_mutex_t display_lock = PTHREAD_MUTEX_INITIALIZER;
static char display_source[192] = DISPLAY_FALLBACK_SOURCE;
static platform_display_snapshot display_values = {
 .struct_size = sizeof(platform_display_snapshot),
 .density_dpi = 160,
 .density = 1.0f,
 .scaled_density = 1.0f,
 .source = display_source
};
static bool display_host_present;
static uint64_t display_generation;
static dvm_ref resources_metrics_ref, resources_config_ref;
static uint64_t metrics_generation = UINT64_MAX, config_generation = UINT64_MAX;

/* VM-lifetime Java singletons, distinct from matcher owner->input edges. */
static bool platform_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context)
{
 (void)context;if(active!=v)return true;
 dvm_ref *refs[]={&gmt,&utc,&resources_metrics_ref,&resources_config_ref};
 for(unsigned i=0;i<sizeof refs/sizeof refs[0];++i)if(*refs[i])visit(refs[i],user);
 return true;
}
void platform_runtime_gc_vm_end(struct dvm *v)
{
 if(active!=v)return;
 dvm_gc_root_provider_remove(v,"platform/singletons",&active);
 gmt=utc=resources_metrics_ref=resources_config_ref=0;
 metrics_generation=config_generation=UINT64_MAX;active=NULL;
}

/* Guest-visible failures used to carry only the type ("platform runtime"),
 * which made the final EXCEPTION line unattributable once the verbose JAVA_API
 * trace was capped.  Keep the dispatch coordinates in thread-local pointers
 * (no formatting or allocation on the hot path) so the exact unsupported call
 * is observable when it fails. */
static _Thread_local const char *fail_class, *fail_method, *fail_sig;
static bool fail(struct dvm *v, const char *type) {
 sdk_trace_fprintf(stderr,"PLATFORM_FAIL %s.%s%s type=%s\n",fail_class?fail_class:"?",fail_method?fail_method:"?",fail_sig?fail_sig:"",type);
 dvm__throw(v,type,"platform runtime");return true;
}
/* Same as fail(), but with a diagnostic message kept separate from the
 * exception class name.  Passing a sentence as the class name would create
 * a non-standard exception type that callers cannot catch by its real name. */
static bool fail_msg(struct dvm *v,const char *type,const char *message) {
 sdk_trace_fprintf(stderr,"PLATFORM_FAIL %s.%s%s type=%s message=%s\n",fail_class?fail_class:"?",fail_method?fail_method:"?",fail_sig?fail_sig:"",type,message);
 dvm__throw(v,type,"%s",message);return true;
}
static dvm_ref object(struct dvm *v,const char *name) { return dvm_new_object(v,dvm_find_class_or_external(v,name)); }
static void pattern_free(void *p) { pattern_data *d=p; if(d){if(d->compiled)regfree(&d->regex);free(d);} }
static void matcher_free(void *p) { matcher_data *d=p; if(d){dvm_unpin(d->vm,d->pattern);dvm_unpin(d->vm,d->input);free(d);} }
static void matcher_clear_match(matcher_data *d) { d->matched=0;d->ngroups=0;d->match_start=d->match_end=0; }
/* regexec reports offsets relative to the NUL-terminated copy that starts at
 * `base`; store absolute input offsets so group()/start()/end() never need the
 * temporary copy. */
static void matcher_store(matcher_data *d,const regmatch_t *m,int count,size_t base) {
 d->matched=1;d->ngroups=count;
 for(int i=0;i<count&&i<MATCH_SLOTS;i++){
  d->groups[i]=m[i];
  if(m[i].rm_so>=0){d->groups[i].rm_so+=(regoff_t)base;d->groups[i].rm_eo+=(regoff_t)base;}
 }
 d->match_start=base+(size_t)m[0].rm_so;d->match_end=base+(size_t)m[0].rm_eo;
}
/* bionic's regexec has no REG_STARTEND, so copy the region remainder into a
 * temporary NUL-terminated buffer.  The caller frees it right after regexec. */
static char *matcher_region_copy(const char *s,size_t base,size_t end,size_t *span) {
 if(end<base)end=base;
 size_t n=end-base;char *buf=malloc(n+1);
 if(!buf)return NULL;
 if(n)memcpy(buf,s+base,n);
 buf[n]='\0';*span=n;return buf;
}

/* Resolve one snapshot under the lock so DisplayMetrics and Configuration
 * always describe the same host generation.  source is copied, never pinned. */
static void display_snapshot_read(platform_display_snapshot *s,char *source,size_t cap,uint64_t *generation){
 pthread_mutex_lock(&display_lock);
 *s=display_values;
 if(source&&cap)snprintf(source,cap,"%s",display_source);
 if(generation)*generation=display_generation;
 pthread_mutex_unlock(&display_lock);
 s->source=NULL;
}
static bool display_host_present_read(void){
 pthread_mutex_lock(&display_lock);bool present=display_host_present;pthread_mutex_unlock(&display_lock);return present;
}
/* Host bridge entry point.  Returns false for an ABI/size mismatch or values
 * that cannot describe a real display; the previous generation stays intact.
 * A NULL snapshot selects the labelled diagnostic fallback. */
__attribute__((visibility("default"))) bool sdk_set_display_snapshot(const platform_display_snapshot *snapshot){
 char source[sizeof display_source];
 if(snapshot){
  if(snapshot->struct_size!=sizeof *snapshot)return false;
  if(snapshot->width_pixels<0||snapshot->height_pixels<0||snapshot->density_dpi<0||
     snapshot->orientation<0||snapshot->orientation>2||
     snapshot->screen_width_dp<0||snapshot->screen_height_dp<0||snapshot->smallest_screen_width_dp<0||
     !(snapshot->density>0.0f)||snapshot->density>32.0f||
     !(snapshot->scaled_density>0.0f)||snapshot->scaled_density>32.0f||
     !(snapshot->xdpi>=0.0f)||!(snapshot->ydpi>=0.0f))return false;
  snprintf(source,sizeof source,"%s",snapshot->source&&*snapshot->source?snapshot->source:"host snapshot (source not provided)");
 }else snprintf(source,sizeof source,"%s",DISPLAY_FALLBACK_SOURCE);
 pthread_mutex_lock(&display_lock);
 if(snapshot){display_values=*snapshot;display_values.struct_size=sizeof display_values;}
 else{
  memset(&display_values,0,sizeof display_values);
  display_values.struct_size=sizeof display_values;
  display_values.density=1.0f;display_values.scaled_density=1.0f;display_values.density_dpi=160;
 }
 snprintf(display_source,sizeof display_source,"%s",source);
 display_values.source=display_source;
 display_host_present=snapshot!=NULL;
 ++display_generation;
 pthread_mutex_unlock(&display_lock);
 return true;
}
/* Informational only: the returned pointer is valid until the next setter
 * call.  No real host value is ever replaced by the fallback silently. */
__attribute__((visibility("default"))) const char *sdk_display_source(void){return display_source;}

/* Real display direction from the current host snapshot.  The value uses the
 * Android Configuration.orientation encoding (1=portrait, 2=landscape,
 * 0=unknown) and is copied under the same lock as every other display field.
 * *host_present reports whether a verified host snapshot is installed; the
 * diagnostic fallback never claims a real direction. */
__attribute__((visibility("default"))) int32_t sdk_get_display_orientation(bool *host_present){
 platform_display_snapshot snapshot;
 display_snapshot_read(&snapshot,NULL,0,NULL);
 if(host_present)*host_present=display_host_present_read();
 return snapshot.orientation;
}

/* Field order here is the ABI; slots start after the external superclass. */
static bool define_instance_fields(struct dvm *v,const char *name,const char *const *names,const char *const *types,int count){
 struct dvm_class *c=dvm_find_class_or_external(v,name);
 if(!c||!c->external)return false;
 int base=c->super?c->super->islots:0;
 if(c->ifields){
  if(c->nifields!=count||c->islots!=base+count)return false;
  for(int i=0;i<count;i++)if(strcmp(c->ifields[i].name,names[i])||strcmp(c->ifields[i].type,types[i])||c->ifields[i].slot!=base+i)return false;
  return true;
 }
 if(c->nifields||c->islots)return false;
 struct dvm_field *fields=calloc((size_t)count,sizeof *fields);if(!fields)return false;
 for(int i=0;i<count;i++){fields[i].cls=c;fields[i].name=names[i];fields[i].type=types[i];fields[i].kind=types[i][0];fields[i].width=1;fields[i].access=1;fields[i].slot=(uint16_t)(base+i);}
 c->ifields=fields;c->nifields=count;c->islots=base+count;
 return true;
}
static bool instance_named(struct dvm *v,dvm_ref ref,const char *name){
 struct dvm_object *o=dvm__obj(v,ref);if(!o||!o->cls)return false;
 for(struct dvm_class *k=o->cls;k;k=k->super)if(!strcmp(k->name,name))return true;
 return false;
}
static bool context_receiver(struct dvm *v,dvm_ref ref){
 return instance_named(v,ref,"android/content/Context")||instance_named(v,ref,"android/content/ContextWrapper")||
        instance_named(v,ref,"android/app/Application")||instance_named(v,ref,"android/app/Service");
}
typedef struct {int32_t width,height,dpi;float density,scaled,xdpi,ydpi;} metrics_value_set;
typedef struct {int32_t orientation,width_dp,height_dp,smallest_dp,dpi;} config_value_set;
static bool apply_metrics_values(struct dvm *v,dvm_ref ref,const metrics_value_set *m){
 union dvm_value x;
 x.i=m->width;if(!dvm_set_field(v,ref,"widthPixels","I",x))return false;
 x.i=m->height;if(!dvm_set_field(v,ref,"heightPixels","I",x))return false;
 x.f=m->density;if(!dvm_set_field(v,ref,"density","F",x))return false;
 x.i=m->dpi;if(!dvm_set_field(v,ref,"densityDpi","I",x))return false;
 x.f=m->scaled;if(!dvm_set_field(v,ref,"scaledDensity","F",x))return false;
 x.f=m->xdpi;if(!dvm_set_field(v,ref,"xdpi","F",x))return false;
 x.f=m->ydpi;if(!dvm_set_field(v,ref,"ydpi","F",x))return false;
 return true;
}
static bool read_metrics_values(struct dvm *v,dvm_ref ref,metrics_value_set *m){
 union dvm_value x;
 if(!dvm_get_field(v,ref,"widthPixels","I",&x))return false;m->width=x.i;
 if(!dvm_get_field(v,ref,"heightPixels","I",&x))return false;m->height=x.i;
 if(!dvm_get_field(v,ref,"density","F",&x))return false;m->density=x.f;
 if(!dvm_get_field(v,ref,"densityDpi","I",&x))return false;m->dpi=x.i;
 if(!dvm_get_field(v,ref,"scaledDensity","F",&x))return false;m->scaled=x.f;
 if(!dvm_get_field(v,ref,"xdpi","F",&x))return false;m->xdpi=x.f;
 if(!dvm_get_field(v,ref,"ydpi","F",&x))return false;m->ydpi=x.f;
 return true;
}
static bool apply_config_values(struct dvm *v,dvm_ref ref,const config_value_set *c){
 union dvm_value x;
 x.i=c->orientation;if(!dvm_set_field(v,ref,"orientation","I",x))return false;
 x.i=c->width_dp;if(!dvm_set_field(v,ref,"screenWidthDp","I",x))return false;
 x.i=c->height_dp;if(!dvm_set_field(v,ref,"screenHeightDp","I",x))return false;
 x.i=c->smallest_dp;if(!dvm_set_field(v,ref,"smallestScreenWidthDp","I",x))return false;
 x.i=c->dpi;if(!dvm_set_field(v,ref,"densityDpi","I",x))return false;
 return true;
}
static bool read_config_values(struct dvm *v,dvm_ref ref,config_value_set *c){
 union dvm_value x;
 if(!dvm_get_field(v,ref,"orientation","I",&x))return false;c->orientation=x.i;
 if(!dvm_get_field(v,ref,"screenWidthDp","I",&x))return false;c->width_dp=x.i;
 if(!dvm_get_field(v,ref,"screenHeightDp","I",&x))return false;c->height_dp=x.i;
 if(!dvm_get_field(v,ref,"smallestScreenWidthDp","I",&x))return false;c->smallest_dp=x.i;
 if(!dvm_get_field(v,ref,"densityDpi","I",&x))return false;c->dpi=x.i;
 return true;
}
static bool apply_display_metrics(struct dvm *v,dvm_ref ref,const platform_display_snapshot *s){
 metrics_value_set m={s->width_pixels,s->height_pixels,s->density_dpi,s->density,s->scaled_density,s->xdpi,s->ydpi};
 return apply_metrics_values(v,ref,&m);
}
static bool apply_display_configuration(struct dvm *v,dvm_ref ref,const platform_display_snapshot *s){
 config_value_set c={s->orientation,s->screen_width_dp,s->screen_height_dp,s->smallest_screen_width_dp,s->density_dpi};
 return apply_config_values(v,ref,&c);
}
static uint64_t display_current_generation(void){
 pthread_mutex_lock(&display_lock);uint64_t generation=display_generation;pthread_mutex_unlock(&display_lock);return generation;
}
static bool ensure_metrics_object(struct dvm *v,dvm_ref *out){
 platform_display_snapshot s;char source[sizeof display_source];uint64_t generation;
 display_snapshot_read(&s,source,sizeof source,&generation);
 if(!resources_metrics_ref){
  resources_metrics_ref=object(v,"android/util/DisplayMetrics");
  if(!resources_metrics_ref){dvm__throw(v,"java/lang/OutOfMemoryError","DisplayMetrics");return false;}
  metrics_generation=UINT64_MAX;
 }
 if(metrics_generation!=generation){
  if(!apply_display_metrics(v,resources_metrics_ref,&s)){dvm__throw(v,"java/lang/IllegalStateException","DisplayMetrics fields unavailable");return false;}
  metrics_generation=generation;
 }
 *out=resources_metrics_ref;return true;
}
static bool ensure_config_object(struct dvm *v,dvm_ref *out){
 platform_display_snapshot s;char source[sizeof display_source];uint64_t generation;
 display_snapshot_read(&s,source,sizeof source,&generation);
 if(!resources_config_ref){
  resources_config_ref=object(v,"android/content/res/Configuration");
  if(!resources_config_ref){dvm__throw(v,"java/lang/OutOfMemoryError","Configuration");return false;}
  config_generation=UINT64_MAX;
 }
 if(config_generation!=generation){
  if(!apply_display_configuration(v,resources_config_ref,&s)){dvm__throw(v,"java/lang/IllegalStateException","Configuration fields unavailable");return false;}
  config_generation=generation;
 }
 *out=resources_config_ref;return true;
}
/* Copy a guest Configuration into a Resources-owned Configuration.  Scalar
 * fields use the same accessors as Configuration.<init>(Configuration), and
 * localeList is cloned through locale_runtime so LocaleList identities and
 * the locale selftest keep working. */
static bool copy_configuration_values(struct dvm *v,dvm_ref dst,dvm_ref src){
 config_value_set c;
 if(!read_config_values(v,src,&c)){dvm__throw(v,"java/lang/IllegalStateException","Configuration fields unavailable");return false;}
 if(!apply_config_values(v,dst,&c)){dvm__throw(v,"java/lang/IllegalStateException","Configuration fields unavailable");return false;}
 if(!locale_runtime_configuration_copy_locales(v,dst,src)){
  dvm__throw(v,"java/lang/IllegalStateException","Configuration.localeList unavailable");return false;
 }
 return true;
}
static bool copy_metrics_values(struct dvm *v,dvm_ref dst,dvm_ref src){
 metrics_value_set m;
 if(!read_metrics_values(v,src,&m)){dvm__throw(v,"java/lang/IllegalStateException","DisplayMetrics fields unavailable");return false;}
 if(!apply_metrics_values(v,dst,&m)){dvm__throw(v,"java/lang/IllegalStateException","DisplayMetrics fields unavailable");return false;}
 return true;
}
/* Resources stores its constructor references in the mAssets/mMetrics/
 * mConfiguration fields defined by platform_runtime_vm_start.  The real
 * Android constructor wraps the caller's metrics/configuration in
 * Resources-owned copies, which this runtime mirrors; AssetManager is the
 * host facade object shared with asset_reader.  A null argument is rejected
 * with NullPointerException instead of building an empty resource view. */
static bool construct_resources(struct dvm *v,dvm_ref self,dvm_ref assets,dvm_ref metrics,dvm_ref config){
 if(!assets||!metrics||!config)return fail(v,"java/lang/NullPointerException");
 if(!instance_named(v,assets,"android/content/res/AssetManager")||
    !instance_named(v,metrics,"android/util/DisplayMetrics")||
    !instance_named(v,config,"android/content/res/Configuration"))
  return fail(v,"java/lang/IllegalArgumentException");
 dvm_ref owned_metrics=object(v,"android/util/DisplayMetrics");
 dvm_ref owned_config=object(v,"android/content/res/Configuration");
 if(!owned_metrics||!owned_config)return fail(v,"java/lang/OutOfMemoryError");
 if(!copy_metrics_values(v,owned_metrics,metrics))return true;
 if(!copy_configuration_values(v,owned_config,config))return true;
 union dvm_value value={.l=assets};
 if(!dvm_set_field(v,self,"mAssets","Landroid/content/res/AssetManager;",value))
  return fail_msg(v,"java/lang/IllegalStateException","Resources.mAssets field unavailable");
 value.l=owned_metrics;
 if(!dvm_set_field(v,self,"mMetrics","Landroid/util/DisplayMetrics;",value))
  return fail_msg(v,"java/lang/IllegalStateException","Resources.mMetrics field unavailable");
 value.l=owned_config;
 if(!dvm_set_field(v,self,"mConfiguration","Landroid/content/res/Configuration;",value))
  return fail_msg(v,"java/lang/IllegalStateException","Resources.mConfiguration field unavailable");
 return true;
}
static bool resources_field_ref(struct dvm *v,dvm_ref self,const char *name,const char *type,dvm_ref *out){
 union dvm_value value={0};
 *out=0;
 if(!dvm_get_field(v,self,name,type,&value))return false;
 *out=value.l;
 return *out!=0;
}
/* Copy guest-supplied values into an explicit Resources-owned pair.  Either
 * argument may be null, matching Resources.updateConfiguration semantics. */
static bool update_configuration_target(struct dvm *v,dvm_ref target_config,dvm_ref target_metrics,dvm_ref config,dvm_ref metrics){
 if(config&&!instance_named(v,config,"android/content/res/Configuration")){dvm__throw(v,"java/lang/IllegalArgumentException","foreign Configuration");return false;}
 if(metrics&&!instance_named(v,metrics,"android/util/DisplayMetrics")){dvm__throw(v,"java/lang/IllegalArgumentException","foreign DisplayMetrics");return false;}
 if(config&&!copy_configuration_values(v,target_config,config))return false;
 if(metrics&&!copy_metrics_values(v,target_metrics,metrics))return false;
 return true;
}
/* Resources.updateConfiguration on the shared asset_reader facade updates the
 * host-generation caches; on a Resources created by the three-argument
 * constructor it updates that instance's own copies and keeps their object
 * identity stable. */
static bool update_resources_configuration(struct dvm *v,dvm_ref self,dvm_ref config,dvm_ref metrics){
 dvm_ref own_metrics=0,own_config=0;
 if(resources_field_ref(v,self,"mMetrics","Landroid/util/DisplayMetrics;",&own_metrics)&&
    resources_field_ref(v,self,"mConfiguration","Landroid/content/res/Configuration;",&own_config))
  return update_configuration_target(v,own_config,own_metrics,config,metrics);
 dvm_ref cached_metrics=0,cached_config=0;
 if(!ensure_metrics_object(v,&cached_metrics)||!ensure_config_object(v,&cached_config))return false;
 if(!update_configuration_target(v,cached_config,cached_metrics,config,metrics))return false;
 uint64_t generation=display_current_generation();
 if(config)config_generation=generation;
 if(metrics)metrics_generation=generation;
 return true;
}
/* The generated table contains the extracted SDK literals.  It is never
 * copied into logs; only membership and non-emptiness are observed. */
#if defined(__has_include)
# if __has_include("activity_resources.inc")
#  define PLATFORM_RESOURCE_TABLE 1
# else
#  define PLATFORM_RESOURCE_TABLE 0
# endif
#else
# define PLATFORM_RESOURCE_TABLE 0
#endif
static const char *resource_value(int id){
#if PLATFORM_RESOURCE_TABLE
 const char *value=NULL;
 switch(id){
#include "activity_resources.inc"
#include "unity_resource_strings.inc"
#include "freeze180_resource_strings.inc"
 default: break;
 }
 return value;
#else
 (void)id;return NULL;
#endif
}
/* Public identifiers for the extracted SDK string resources.  The generated
 * table owns the values; this table only answers getIdentifier() and the
 * getResource*() metadata calls, so no configuration value is ever logged or
 * duplicated here.  PLATFORM_RESOURCE_PACKAGE is the original guest package
 * recorded in apk-analysis.json. */
#define PLATFORM_RESOURCE_PACKAGE "com.bilibili.star.bili"
typedef struct { int32_t id; const char *type; const char *name; } platform_resource_name;
static const platform_resource_name platform_resource_names[] = {
 {2131558826,"string","sdk_app_id"},
 {2131558827,"string","sdk_app_key"},
 {2131558829,"string","sdk_merchant_id"},
 {2131558830,"string","sdk_server_id"},
};

/* Real APK resource metadata and color values generated directly from the
 * original APK's resources.arsc by resource-table-next/gen_apk_resource_table.py.
 * The include records the APK and resources.arsc SHA-256 digests, and every
 * row is a byte-observed entry from that archive; nothing here is invented.
 * The table is sorted by resource id, so lookups below are binary searches. */
typedef struct { int32_t id; const char *type; const char *name; } platform_apk_resource;
typedef struct { int32_t id; uint32_t argb; } platform_apk_color;
typedef struct { int32_t id; const char *reason; } platform_apk_reason;
#if defined(__has_include)
# if __has_include("apk_resource_table.inc")
#  define PLATFORM_APK_RESOURCE_TABLE 1
#  include "apk_resource_table.inc"
# else
#  define PLATFORM_APK_RESOURCE_TABLE 0
# endif
#else
# define PLATFORM_APK_RESOURCE_TABLE 0
#endif

#if PLATFORM_APK_RESOURCE_TABLE
static const platform_apk_resource *apk_resource_by_id(int32_t id){
 size_t lo=0,hi=PLATFORM_APK_RESOURCE_COUNT;
 while(lo<hi){
  size_t mid=lo+(hi-lo)/2;
  int32_t mid_id=platform_apk_resource_names[mid].id;
  if(mid_id==id)return &platform_apk_resource_names[mid];
  if(mid_id<id)lo=mid+1;else hi=mid;
 }
 return NULL;
}
static const platform_apk_color *apk_color_by_id(int32_t id){
 size_t lo=0,hi=PLATFORM_APK_COLOR_COUNT;
 while(lo<hi){
  size_t mid=lo+(hi-lo)/2;
  int32_t mid_id=platform_apk_resource_colors[mid].id;
  if(mid_id==id)return &platform_apk_resource_colors[mid];
  if(mid_id<id)lo=mid+1;else hi=mid;
 }
 return NULL;
}
static const platform_apk_reason *apk_reason_by_id(int32_t id){
 size_t lo=0,hi=PLATFORM_APK_REASON_COUNT;
 while(lo<hi){
  size_t mid=lo+(hi-lo)/2;
  int32_t mid_id=platform_apk_resource_reasons[mid].id;
  if(mid_id==id)return &platform_apk_resource_reasons[mid];
  if(mid_id<id)lo=mid+1;else hi=mid;
 }
 return NULL;
}
static bool apk_resource_by_name(const char *type,const char *name,platform_resource_name *out){
 /* Linear scan: getIdentifier() is not on the UI hot path, and the table is
  * only a few thousand rows.  An empty defType accepts any type. */
 for(size_t i=0;i<PLATFORM_APK_RESOURCE_COUNT;i++){
  const platform_apk_resource *r=&platform_apk_resource_names[i];
  if(strcmp(r->name,name))continue;
  if(type&&*type&&strcmp(r->type,type))continue;
  out->id=r->id;out->type=r->type;out->name=r->name;
  return true;
 }
 return false;
}
#endif

static bool resource_view_by_id(int32_t id,platform_resource_name *out){
 for(size_t i=0;i<sizeof platform_resource_names/sizeof platform_resource_names[0];i++){
  const platform_resource_name *r=&platform_resource_names[i];
  /* A name is only claimed when the generated value table really contains
   * the id; otherwise metadata must not advertise a resource that getString
   * would reject. */
  if(r->id==id&&resource_value(id)){
   *out=*r;return true;
  }
 }
#if PLATFORM_APK_RESOURCE_TABLE
 {
  const platform_apk_resource *r=apk_resource_by_id(id);
  if(r){out->id=r->id;out->type=r->type;out->name=r->name;return true;}
 }
#endif
 return false;
}
static bool resource_view_by_resource(const char *package,const char *type,const char *name,platform_resource_name *out){
 /* An absent/empty defPackage means the application package, and an
  * absent/empty defType allows any type.  A non-empty qualifier must match
  * exactly. */
 if(!package||!*package||!name||strcmp(package,PLATFORM_RESOURCE_PACKAGE))return false;
 for(size_t i=0;i<sizeof platform_resource_names/sizeof platform_resource_names[0];i++){
  const platform_resource_name *r=&platform_resource_names[i];
  if((!type||!*type||!strcmp(type,"string"))&&!strcmp(r->name,name)&&resource_value(r->id)){
   *out=*r;return true;
  }
 }
#if PLATFORM_APK_RESOURCE_TABLE
 return apk_resource_by_name(type,name,out);
#else
 (void)type;(void)name;return false;
#endif
}
static bool resource_metadata_string(struct dvm *v,const platform_resource_name *r,int package,int type,dvm_ref *out){
 char text[160];
 if(package&&type)snprintf(text,sizeof text,"%s:%s/%s",PLATFORM_RESOURCE_PACKAGE,r->type,r->name);
 else if(package)snprintf(text,sizeof text,"%s",PLATFORM_RESOURCE_PACKAGE);
 else if(type)snprintf(text,sizeof text,"%s",r->type);
 else snprintf(text,sizeof text,"%s",r->name);
 *out=dvm_new_string(v,text);
 if(!*out){dvm__throw(v,"java/lang/OutOfMemoryError","resource name");return false;}
 return true;
}
static bool get_string_value(struct dvm *v,int id,dvm_ref format_args,dvm_ref *out){
 const char *value=resource_value(id);
 if(!value||id==0x7f0d01ae||id==0x7f0d01af){FILE *trace=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/freeze180-resources.txt","a");
  if(trace){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);fprintf(trace,"pid=%d mono_ns=%llu getString id=0x%08x resolved=%d source=original_APK\n",getpid(),(unsigned long long)ts.tv_sec*1000000000ull+ts.tv_nsec,(unsigned)id,value!=NULL);fclose(trace);}}
 if(!value){dvm__throw(v,"android/content/res/Resources$NotFoundException","unknown resource 0x%08x",(unsigned)id);return false;}
 if(format_args&&strchr(value,'%')){dvm__throw(v,"java/lang/UnsupportedOperationException","resource format arguments are not implemented");return false;}
 *out=dvm_new_string(v,value);
 if(!*out){dvm__throw(v,"java/lang/OutOfMemoryError","resource string");return false;}
 return true;
}
/* Android Resources.getIdentifier() accepts "name", "type/name" and
 * "package:type/name", falling back to defType/defPackage (the app package
 * when defPackage is null).  A resource that is not in the extracted table
 * returns 0, exactly as AOSP does; it is never claimed as present. */
static bool resources_get_identifier(struct dvm *v,dvm_ref name_ref,dvm_ref type_ref,dvm_ref package_ref,union dvm_value *out){
 if(!name_ref)return fail(v,"java/lang/NullPointerException");
 const char *name=dvm_string_utf8(v,name_ref);
 size_t name_len=dvm_string_utf8_length(v,name_ref);
 if(!name||strlen(name)!=name_len||name_len>=256)return fail(v,"java/lang/UnsupportedOperationException");
 char spec[256];
 memcpy(spec,name,name_len);spec[name_len]='\0';
 /* Android first accepts a decimal resource id in `name`.  This runtime only
  * accepts it when the extracted table really contains that id; an unknown
  * numeric name returns 0 instead of claiming a resource that cannot be
  * read.  Malformed/oversized text falls through to the name lookup. */
 bool decimal=spec[0]!=0;
 for(const char *p=spec;*p;p++)if(*p<'0'||*p>'9'){decimal=false;break;}
 if(decimal){
  char *end=NULL;
  unsigned long parsed=strtoul(spec,&end,10);
  if(end&&!*end&&parsed<=(unsigned long)INT32_MAX&&resource_value((int32_t)parsed)){
   out->i=(int32_t)parsed;return true;
  }
 }
 char *package=NULL,*type=NULL,*entry=spec;
 char *colon=strchr(spec,':');
 if(colon){*colon='\0';package=spec;entry=colon+1;}
 char *slash=strchr(entry,'/');
 if(slash){*slash='\0';type=entry;entry=slash+1;}
 const char *def_type=NULL,*def_package=NULL;
 if(type_ref){
  def_type=dvm_string_utf8(v,type_ref);
  if(!def_type||strlen(def_type)!=dvm_string_utf8_length(v,type_ref))return fail(v,"java/lang/UnsupportedOperationException");
 }
 if(package_ref){
  def_package=dvm_string_utf8(v,package_ref);
  if(!def_package||strlen(def_package)!=dvm_string_utf8_length(v,package_ref))return fail(v,"java/lang/UnsupportedOperationException");
 }
 if(!type||!*type)type=(char*)((def_type&&*def_type)?def_type:NULL);
 if(!package||!*package)package=(char*)((def_package&&*def_package)?def_package:PLATFORM_RESOURCE_PACKAGE);
 platform_resource_name r;
 if(!resource_view_by_resource(package,type,entry,&r)){out->i=0;return true;}
 out->i=r.id;
 return true;
}
/* Resolve Resources.getColor(I) from the byte-observed APK table.
 *
 * Precedence:
 *   1. a scalar color entry resolved by the generator -> real ARGB;
 *   2. a color/drawable row that exists but is a state-list XML, a drawable
 *      file, a theme attribute or a reference outside resources.arsc -> the
 *      exact reason recorded while parsing the APK, as NotFoundException;
 *   3. a real APK row of another type -> NotFoundException naming that type;
 *   4. an extracted SDK string value (legacy table only) -> NotFoundException;
 *   5. anything else -> NotFoundException with the unknown id.
 *
 * No branch guesses a color.  Package 0x01 platform colors are outside this
 * APK's resources.arsc and therefore fall through to the unknown-id error. */
static bool get_color_value(struct dvm *v,int32_t id,union dvm_value *out){
#if PLATFORM_APK_RESOURCE_TABLE
 {
  const platform_apk_color *color=apk_color_by_id(id);
  if(color){out->i=(int32_t)color->argb;return true;}
 }
 {
  const platform_apk_reason *reason=apk_reason_by_id(id);
  if(reason)return fail_msg(v,"android/content/res/Resources$NotFoundException",reason->reason);
 }
 {
  const platform_apk_resource *r=apk_resource_by_id(id);
  if(r){
   char message[256];
   snprintf(message,sizeof message,"resource %s/%s (0x%08x) is not a color",r->type,r->name,(unsigned)id);
   return fail_msg(v,"android/content/res/Resources$NotFoundException",message);
  }
 }
#endif
 if(resource_value(id))
  return fail_msg(v,"android/content/res/Resources$NotFoundException","resource is an extracted string, not a color");
 {
  char message[128];
  if(((uint32_t)id>>24)==0x01)
   snprintf(message,sizeof message,"platform package resource 0x%08x is not present in the application resources.arsc",(unsigned)id);
  else
   snprintf(message,sizeof message,"unknown resource id 0x%08x",(unsigned)id);
  return fail_msg(v,"android/content/res/Resources$NotFoundException",message);
 }
}
static bool exception_named(struct dvm *v,const char *name){
 struct dvm_object *o=dvm__obj(v,dvm_exception(v));return o&&o->cls&&!strcmp(o->cls->name,name);
}
static bool check_int_field(struct dvm *v,dvm_ref ref,const char *name,int32_t expected){
 union dvm_value x;
 if(!dvm_get_field(v,ref,name,"I",&x)){dvm__throw(v,"java/lang/IllegalStateException","PLATFORM_SELFTEST missing %s",name);return false;}
 if(x.i!=expected){dvm__throw(v,"java/lang/IllegalStateException","PLATFORM_SELFTEST mismatch %s",name);return false;}
 return true;
}
static bool check_float_field(struct dvm *v,dvm_ref ref,const char *name,float expected){
 union dvm_value x;
 if(!dvm_get_field(v,ref,name,"F",&x)){dvm__throw(v,"java/lang/IllegalStateException","PLATFORM_SELFTEST missing %s",name);return false;}
 if(x.f!=expected){dvm__throw(v,"java/lang/IllegalStateException","PLATFORM_SELFTEST mismatch %s",name);return false;}
 return true;
}
static bool check_metrics(struct dvm *v,dvm_ref ref,const metrics_value_set *m){
 return check_int_field(v,ref,"widthPixels",m->width)&&check_int_field(v,ref,"heightPixels",m->height)&&
        check_float_field(v,ref,"density",m->density)&&check_int_field(v,ref,"densityDpi",m->dpi)&&
        check_float_field(v,ref,"scaledDensity",m->scaled)&&check_float_field(v,ref,"xdpi",m->xdpi)&&
        check_float_field(v,ref,"ydpi",m->ydpi);
}
static bool check_config(struct dvm *v,dvm_ref ref,const config_value_set *c){
 return check_int_field(v,ref,"orientation",c->orientation)&&check_int_field(v,ref,"screenWidthDp",c->width_dp)&&
        check_int_field(v,ref,"screenHeightDp",c->height_dp)&&check_int_field(v,ref,"smallestScreenWidthDp",c->smallest_dp)&&
        check_int_field(v,ref,"densityDpi",c->dpi);
}
bool platform_runtime_vm_start(struct dvm *v) {
 static const char *const metrics_names[]={"widthPixels","heightPixels","density","densityDpi","scaledDensity","xdpi","ydpi"};
 static const char *const metrics_types[]={"I","I","F","I","F","F","F"};
 static const char *const config_names[]={"orientation","screenWidthDp","screenHeightDp","smallestScreenWidthDp","densityDpi","localeList"};
 static const char *const config_types[]={"I","I","I","I","I","Landroid/os/LocaleList;"};
 active=v;gmt=utc=0;
 resources_metrics_ref=resources_config_ref=0;
 metrics_generation=config_generation=UINT64_MAX;
 if(!dvm_gc_root_provider_add(v,"platform/singletons",DVM_GC_ROOT_STRONG,platform_gc_roots,&active))return false;
 if(!locale_runtime_vm_start(v)){
  sdk_trace_fprintf(stderr,"PLATFORM_RUNTIME_START FAIL stage=locale_runtime_start line=%d\n",__LINE__);
  return false;
 }
 if(!define_instance_fields(v,"android/util/DisplayMetrics",metrics_names,metrics_types,7)){
  sdk_trace_fprintf(stderr,"PLATFORM_RUNTIME_START FAIL stage=define_display_metrics line=%d\n",__LINE__);
  return false;
 }
 if(!define_instance_fields(v,"android/content/res/Configuration",config_names,config_types,6)){
  sdk_trace_fprintf(stderr,"PLATFORM_RUNTIME_START FAIL stage=define_configuration line=%d\n",__LINE__);
  return false;
 }
 {
  /* These fields must exist before any guest DEX class is loaded: a guest
   * subclass of Resources (com/gsc/skin/base/c) computes its own field slots
   * from Resources.islots.  Defining them later would silently shift the
   * subclass layout. */
  static const char *const resources_names[]={"mAssets","mMetrics","mConfiguration"};
  static const char *const resources_types[]={"Landroid/content/res/AssetManager;","Landroid/util/DisplayMetrics;","Landroid/content/res/Configuration;"};
  if(!define_instance_fields(v,"android/content/res/Resources",resources_names,resources_types,3)){
   sdk_trace_fprintf(stderr,"PLATFORM_RUNTIME_START FAIL stage=define_resources line=%d\n",__LINE__);
   return false;
  }
 }
 {
  static const char *const point_names[]={"x","y"};
  static const char *const point_types[]={"I","I"};
  if(!define_instance_fields(v,"android/graphics/Point",point_names,point_types,2)){
   sdk_trace_fprintf(stderr,"PLATFORM_RUNTIME_START FAIL stage=define_point line=%d\n",__LINE__);
   return false;
  }
 }
 sdk_trace_fprintf(stderr,"PLATFORM_DISPLAY_SOURCE source=%s host=%d\n",sdk_display_source(),display_host_present_read()?1:0);
 return true;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
static bool platform_runtime_call_step8_operation(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out) {
 if(v!=active)return false;
 fail_class=c;fail_method=m;fail_sig=sig;
 /* Locale and LocaleList own their real payloads; Configuration.getLocales()
  * below uses locale_list.h helpers so the LocaleList is cached per
  * Configuration object.  Dispatch first so java_basics' external statics and
  * the real Locale objects share one implementation. */
 if(locale_runtime_call(v,c,m,sig,self,a,n,out))return true;
 if(!strcmp(c,"android/content/res/Resources")){
  if(!instance_named(v,self,"android/content/res/Resources"))return false;
  /* com/gsc/skin/base/c.<init> calls this exact three-argument super
   * constructor.  The fields are real references: the AssetManager is the
   * asset_reader facade and the metrics/configuration are Resources-owned
   * copies of the caller's objects. */
  if(IS("<init>","(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V")&&n==3)
   return construct_resources(v,self,a[0].l,a[1].l,a[2].l);
  if(IS("getAssets","()Landroid/content/res/AssetManager;")&&n==0){
   dvm_ref ref;
   if(resources_field_ref(v,self,"mAssets","Landroid/content/res/AssetManager;",&ref)){out->l=ref;return true;}
   /* Context.getResources() is served by asset_reader, which owns the
    * Application-facing Resources singleton and its AssetManager facade.
    * Returning false lets that facade answer; do not invent an empty one. */
   return false;
  }
  if(IS("getDisplayMetrics","()Landroid/util/DisplayMetrics;")&&n==0){
   dvm_ref ref;
   if(resources_field_ref(v,self,"mMetrics","Landroid/util/DisplayMetrics;",&ref)){out->l=ref;return true;}
   if(!ensure_metrics_object(v,&ref))return true;out->l=ref;return true;
  }
  if(IS("getConfiguration","()Landroid/content/res/Configuration;")&&n==0){
   dvm_ref ref;
   if(resources_field_ref(v,self,"mConfiguration","Landroid/content/res/Configuration;",&ref)){out->l=ref;return true;}
   if(!ensure_config_object(v,&ref))return true;out->l=ref;return true;
  }
  if(IS("updateConfiguration","(Landroid/content/res/Configuration;Landroid/util/DisplayMetrics;)V")&&n==2)
   return update_resources_configuration(v,self,a[0].l,a[1].l);
  if(IS("getString","(I)Ljava/lang/String;")&&n==1){
   dvm_ref text;if(get_string_value(v,a[0].i,0,&text))out->l=text;return true;
  }
  if(IS("getString","(I[Ljava/lang/Object;)Ljava/lang/String;")&&n==2){
   dvm_ref text;if(get_string_value(v,a[0].i,a[1].l,&text))out->l=text;return true;
  }
  if(IS("getText","(I)Ljava/lang/CharSequence;")&&n==1){
   dvm_ref text;if(get_string_value(v,a[0].i,0,&text))out->l=text;return true;
  }
  /* Android's two-argument getText returns the caller's fallback when the
   * resource is absent; that fallback is not a fabricated resource value. */
  if(IS("getText","(ILjava/lang/CharSequence;)Ljava/lang/CharSequence;")&&n==2){
   const char *value=resource_value(a[0].i);
   if(!value){out->l=a[1].l;return true;}
   out->l=dvm_new_string(v,value);
   if(!out->l)return fail(v,"java/lang/OutOfMemoryError");
   return true;
  }
  if(IS("getIdentifier","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I")&&n==3)
   return resources_get_identifier(v,a[0].l,a[1].l,a[2].l,out);
  if(IS("getResourceEntryName","(I)Ljava/lang/String;")&&n==1){
   platform_resource_name r;
   if(!resource_view_by_id(a[0].i,&r))return fail_msg(v,"android/content/res/Resources$NotFoundException","unknown resource id");
   dvm_ref text;if(!resource_metadata_string(v,&r,0,0,&text))return true;out->l=text;return true;
  }
  if(IS("getResourceTypeName","(I)Ljava/lang/String;")&&n==1){
   platform_resource_name r;
   if(!resource_view_by_id(a[0].i,&r))return fail_msg(v,"android/content/res/Resources$NotFoundException","unknown resource id");
   dvm_ref text;if(!resource_metadata_string(v,&r,0,1,&text))return true;out->l=text;return true;
  }
  if(IS("getResourcePackageName","(I)Ljava/lang/String;")&&n==1){
   platform_resource_name r;
   if(!resource_view_by_id(a[0].i,&r))return fail_msg(v,"android/content/res/Resources$NotFoundException","unknown resource id");
   dvm_ref text;if(!resource_metadata_string(v,&r,1,0,&text))return true;out->l=text;return true;
  }
  if(IS("getResourceName","(I)Ljava/lang/String;")&&n==1){
   platform_resource_name r;
   if(!resource_view_by_id(a[0].i,&r))return fail_msg(v,"android/content/res/Resources$NotFoundException","unknown resource id");
   dvm_ref text;if(!resource_metadata_string(v,&r,1,1,&text))return true;out->l=text;return true;
  }
  /* getColor() is answered from byte-observed resources.arsc color entries.
   * A resource that exists but is not a resolvable scalar color fails closed
   * with the real observed reason, never a fabricated ARGB. */
  if(IS("getColor","(I)I")&&n==1)return get_color_value(v,a[0].i,out);
  if(IS("getColor","(ILandroid/content/res/Resources$Theme;)I")&&n==2)return get_color_value(v,a[0].i,out);
  /* The remaining typed readers have no real TypedValue/parser/asset-file
   * representation in this facade.  They fail with the Android not-found
   * exception instead of returning 0, false, null or an empty array. */
  if((IS("getAnimation","(I)Landroid/content/res/XmlResourceParser;")&&n==1)||
     (IS("getBoolean","(I)Z")&&n==1)||
     (IS("getColorStateList","(I)Landroid/content/res/ColorStateList;")&&n==1)||
     (IS("getColorStateList","(ILandroid/content/res/Resources$Theme;)Landroid/content/res/ColorStateList;")&&n==2)||
     (IS("getDimension","(I)F")&&n==1)||
     (IS("getDimensionPixelOffset","(I)I")&&n==1)||
     (IS("getDimensionPixelSize","(I)I")&&n==1)||
     (IS("getDrawable","(I)Landroid/graphics/drawable/Drawable;")&&n==1)||
     (IS("getDrawable","(ILandroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;")&&n==2)||
     (IS("getDrawableForDensity","(II)Landroid/graphics/drawable/Drawable;")&&n==2)||
     (IS("getFraction","(III)F")&&n==3)||
     (IS("getIntArray","(I)[I")&&n==1)||
     (IS("getInteger","(I)I")&&n==1)||
     (IS("getLayout","(I)Landroid/content/res/XmlResourceParser;")&&n==1)||
     (IS("getMovie","(I)Landroid/graphics/Movie;")&&n==1)||
     (IS("getQuantityString","(II)Ljava/lang/String;")&&n==2)||
     (IS("getQuantityString","(II[Ljava/lang/Object;)Ljava/lang/String;")&&n==3)||
     (IS("getQuantityText","(II)Ljava/lang/CharSequence;")&&n==2)||
     (IS("getStringArray","(I)[Ljava/lang/String;")&&n==1)||
     (IS("getTextArray","(I)[Ljava/lang/CharSequence;")&&n==1)||
     (IS("getXml","(I)Landroid/content/res/XmlResourceParser;")&&n==1))
   return fail_msg(v,"android/content/res/Resources$NotFoundException","resource id has no value of the requested type in the extracted table");
  /* These APIs need a real TypedValue/parser/asset-file object.  The facade
   * cannot represent them, so fail explicitly rather than hand back a
   * placeholder object. */
  if((IS("getValue","(ILandroid/util/TypedValue;Z)V")&&n==3)||
     (IS("getValue","(Ljava/lang/String;Landroid/util/TypedValue;Z)V")&&n==3)||
     (IS("obtainAttributes","(Landroid/util/AttributeSet;[I)Landroid/content/res/TypedArray;")&&n==2)||
     (IS("obtainTypedArray","(I)Landroid/content/res/TypedArray;")&&n==1)||
     (IS("openRawResource","(I)Ljava/io/InputStream;")&&n==1)||
     (IS("openRawResource","(ILandroid/util/TypedValue;)Ljava/io/InputStream;")&&n==2)||
     (IS("openRawResourceFd","(I)Landroid/content/res/AssetFileDescriptor;")&&n==1)||
     (IS("parseBundleExtra","(Ljava/lang/String;Landroid/util/AttributeSet;Landroid/os/Bundle;)V")&&n==3)||
     (IS("parseBundleExtras","(Landroid/content/res/XmlResourceParser;Landroid/os/Bundle;)V")&&n==2))
   return fail_msg(v,"java/lang/UnsupportedOperationException","typed Android resource parsers are not available in this runtime");
  return false;
 }
 if(!strcmp(c,"android/util/DisplayMetrics")){
  if(IS("<init>","()V")&&n==0){
   if(!instance_named(v,self,"android/util/DisplayMetrics"))return false;
   platform_display_snapshot s;char source[sizeof display_source];
   display_snapshot_read(&s,source,sizeof source,NULL);
   return apply_display_metrics(v,self,&s)?true:fail_msg(v,"java/lang/IllegalStateException","DisplayMetrics fields unavailable");
  }
  return false;
 }
 if(!strcmp(c,"android/graphics/Point")){
  if(!instance_named(v,self,"android/graphics/Point"))return false;
  if(IS("<init>","()V")&&n==0){
   union dvm_value zero={.i=0};
   return dvm_set_field(v,self,"x","I",zero)&&dvm_set_field(v,self,"y","I",zero);
  }
  if(IS("<init>","(II)V")&&n==2){
   union dvm_value x={.i=a[0].i},y={.i=a[1].i};
   return dvm_set_field(v,self,"x","I",x)&&dvm_set_field(v,self,"y","I",y);
  }
  if(IS("set","(II)V")&&n==2){
   union dvm_value x={.i=a[0].i},y={.i=a[1].i};
   return dvm_set_field(v,self,"x","I",x)&&dvm_set_field(v,self,"y","I",y);
  }
  union dvm_value xv={0},yv={0};
  if(!dvm_get_field(v,self,"x","I",&xv)||!dvm_get_field(v,self,"y","I",&yv))return false;
  if(IS("toString","()Ljava/lang/String;")){
   char text[64];snprintf(text,sizeof text,"Point(%d, %d)",xv.i,yv.i);
   out->l=dvm_new_string(v,text);return true;
  }
  if(IS("equals","(Ljava/lang/Object;)Z")&&n==1){
   if(!a[0].l){out->i=0;return true;}
   union dvm_value ox={0},oy={0};
   if(!instance_named(v,a[0].l,"android/graphics/Point")||
      !dvm_get_field(v,a[0].l,"x","I",&ox)||!dvm_get_field(v,a[0].l,"y","I",&oy)){out->i=0;return true;}
   out->i=ox.i==xv.i&&oy.i==yv.i;return true;
  }
  if(IS("hashCode","()I")){out->i=(int32_t)(31u*(uint32_t)xv.i+(uint32_t)yv.i);return true;}
  return false;
 }
 if(!strcmp(c,"android/content/res/Configuration")){
  if(!instance_named(v,self,"android/content/res/Configuration"))return false;
  if(IS("<init>","()V")&&n==0){
   config_value_set defaults={0,0,0,0,0};
   if(!apply_config_values(v,self,&defaults))return fail_msg(v,"java/lang/IllegalStateException","Configuration fields unavailable");
   return locale_runtime_configuration_clear_locales(v,self)?true:fail_msg(v,"java/lang/IllegalStateException","Configuration.localeList unavailable");
  }
  if(IS("<init>","(Landroid/content/res/Configuration;)V")&&n==1){
   if(!a[0].l)return fail(v,"java/lang/NullPointerException");
   if(!instance_named(v,a[0].l,"android/content/res/Configuration"))return fail(v,"java/lang/IllegalArgumentException");
   config_value_set values;if(!read_config_values(v,a[0].l,&values))return fail_msg(v,"java/lang/IllegalStateException","Configuration fields unavailable");
   if(!apply_config_values(v,self,&values))return fail_msg(v,"java/lang/IllegalStateException","Configuration fields unavailable");
   return locale_runtime_configuration_copy_locales(v,self,a[0].l)?true:fail_msg(v,"java/lang/IllegalStateException","Configuration.localeList unavailable");
  }
  if(IS("setToDefaults","()V")&&n==0){
   config_value_set defaults={0,0,0,0,0};
   if(!apply_config_values(v,self,&defaults))return fail_msg(v,"java/lang/IllegalStateException","Configuration fields unavailable");
   return locale_runtime_configuration_clear_locales(v,self)?true:fail_msg(v,"java/lang/IllegalStateException","Configuration.localeList unavailable");
  }
  if(IS("getLocales","()Landroid/os/LocaleList;")&&n==0){
   dvm_ref list;
   if(!locale_runtime_configuration_locales(v,self,&list))return true;
   out->l=list;return true;
  }
  return false;
 }
 if((!strcmp(c,"android/content/Context")||!strcmp(c,"android/content/ContextWrapper")||
     !strcmp(c,"android/app/Application")||!strcmp(c,"android/app/Service"))&&
    ((IS("getString","(I)Ljava/lang/String;")&&n==1)||(IS("getString","(I[Ljava/lang/Object;)Ljava/lang/String;")&&n==2))){
  if(!context_receiver(v,self))return false;
  dvm_ref text;if(get_string_value(v,a[0].i,n==2?a[1].l:0,&text))out->l=text;return true;
 }
 if(!strcmp(c,"java/util/TimeZone")) {
  if(IS("getDefault","()Ljava/util/TimeZone;")&&n==0){
   struct host_timezone source={0};
   if(!timezone_host||!timezone_host(source.id,sizeof source.id,&source.raw_offset)||!source.id[0]){dvm__throw(v,"java/lang/IllegalStateException","host TimeZone snapshot unavailable");return true;}
   source.id[sizeof source.id-1]=0;out->l=new_host_timezone(v,&source);return true;
  }
  struct dvm_object *zone=dvm__obj(v,self);struct host_timezone *snapshot=zone&&zone->data_destroy==host_timezone_free?zone->data:NULL;
  if(snapshot){
   if(IS("getRawOffset","()I")){out->i=snapshot->raw_offset;return true;}
   if(IS("getID","()Ljava/lang/String;")){out->l=dvm_new_string(v,snapshot->id);return true;}
   if(IS("clone","()Ljava/lang/Object;")){out->l=new_host_timezone(v,snapshot);return true;}
  }
  if(IS("getTimeZone","(Ljava/lang/String;)Ljava/util/TimeZone;")&&n==1){
   if(!a[0].l)return fail(v,"java/lang/NullPointerException");
   const char *id=dvm_string_utf8(v,a[0].l);size_t len=dvm_string_utf8_length(v,a[0].l);
   if(!id||strlen(id)!=len||((len!=3||strcmp(id,"GMT"))&&(len!=3||strcmp(id,"UTC"))))return fail(v,"java/lang/UnsupportedOperationException");
   dvm_ref *slot=!strcmp(id,"GMT")?&gmt:&utc;if(!*slot)*slot=object(v,c);out->l=*slot;return true;
  }
  if(self&&(self==gmt||self==utc)){
   if(IS("getID","()Ljava/lang/String;")){out->l=dvm_new_string(v,self==gmt?"GMT":"UTC");return true;}
   if(IS("getRawOffset","()I")){out->i=0;return true;}
  }
  return false;
 }
 if(!strcmp(c,"java/util/regex/Pattern")) {
  if((IS("compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;")&&n==1)||(IS("compile","(Ljava/lang/String;I)Ljava/util/regex/Pattern;")&&n==2)){
   if(!a[0].l)return fail(v,"java/lang/NullPointerException");
   if(n==2&&a[1].i)return fail(v,"java/lang/UnsupportedOperationException");
   const char *expr=dvm_string_utf8(v,a[0].l);size_t len=dvm_string_utf8_length(v,a[0].l);
   if(!expr||strlen(expr)!=len)return fail(v,"java/lang/UnsupportedOperationException");
   /* JAVA groups: 2=IP, 3=VM version, 0=DiskLruCache key, 2=MediaType
    * type/subtype, 3=MediaType parameter.  capture_count is the regexec
    * nmatch slot count (whole match plus POSIX groups). */
   const char *posix=NULL;int captures=0,java_groups=0;const int *map=NULL;
   static const int parameter_group_map[MATCH_SLOTS]={0,2,4,5,0,0,0,0};
   if(!strcmp(expr,java_ip_regex)){posix=posix_ip_regex;captures=3;java_groups=2;}
   else if(!strcmp(expr,java_vm_version_regex)){posix=posix_vm_version_regex;captures=4;java_groups=3;}
   /* The key pattern has no capture groups.  Slot 0 is the whole match, so
    * group(1) stays out of range exactly as in Java (groupCount() == 0). */
   else if(!strcmp(expr,disk_lru_cache_key_regex)){posix=disk_lru_cache_key_regex;captures=1;java_groups=0;}
   else if(!strcmp(expr,http_media_type_java_regex)){posix=http_media_type_posix_regex;captures=3;java_groups=2;} /* whole + 2 groups */
   else if(!strcmp(expr,http_media_parameter_java_regex)){posix=http_media_parameter_posix_regex;captures=6;java_groups=3;map=parameter_group_map;}
   else {
    /* The pattern is configuration, not user input; bound the trace so a
     * malformed string cannot flood the report. */
    int shown=len>200?(int)200:(int)len;
    sdk_trace_fprintf(stderr,"PLATFORM_REGEX_UNSUPPORTED pattern=%.*s flags=%d\n",shown,expr,n==2?a[1].i:0);
    return fail(v,!strcmp(expr,"[")?"java/util/regex/PatternSyntaxException":"java/lang/UnsupportedOperationException");
   }
   pattern_data *d=calloc(1,sizeof *d);if(!d)return fail(v,"java/lang/OutOfMemoryError");
   d->capture_count=captures;d->java_groups=java_groups;
   for(int i=0;i<MATCH_SLOTS;i++)d->group_map[i]=i;
   if(map)for(int i=0;i<=java_groups&&i<MATCH_SLOTS;i++)d->group_map[i]=map[i];
   if(regcomp(&d->regex,posix,REG_EXTENDED)){free(d);return fail(v,"java/util/regex/PatternSyntaxException");}d->compiled=1;
   dvm_ref ref=object(v,c);struct dvm_object *o=dvm__obj(v,ref);if(!o){pattern_free(d);return fail(v,"java/lang/OutOfMemoryError");}o->data=d;o->data_destroy=pattern_free;out->l=ref;return true;
  }
  if(IS("matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;")&&n==1){
   struct dvm_object *p=dvm__obj(v,self);if(!p||p->data_destroy!=pattern_free)return false;
   if(!a[0].l)return fail(v,"java/lang/NullPointerException");
   if(!dvm_string_utf8(v,a[0].l))return fail(v,"java/lang/UnsupportedOperationException");
   matcher_data *d=calloc(1,sizeof *d);if(!d)return fail(v,"java/lang/OutOfMemoryError");d->vm=v;d->pattern=self;d->input=a[0].l;d->region_end=dvm_string_utf8_length(v,a[0].l);dvm_pin(v,d->pattern);dvm_pin(v,d->input);
   dvm_ref ref=object(v,"java/util/regex/Matcher");struct dvm_object *o=dvm__obj(v,ref);if(!o){matcher_free(d);return fail(v,"java/lang/OutOfMemoryError");}o->data=d;o->data_destroy=matcher_free;out->l=ref;return true;
  }
  return false;
 }
 if(!strcmp(c,"java/util/regex/Matcher")){
  struct dvm_object *o=dvm__obj(v,self);if(!o||o->data_destroy!=matcher_free)return false;matcher_data *d=o->data;
  struct dvm_object *p=dvm__obj(v,d->pattern);if(!p||p->data_destroy!=pattern_free)return fail(v,"java/lang/IllegalStateException");
  const char *s=dvm_string_utf8(v,d->input);size_t len=dvm_string_utf8_length(v,d->input);
  if(!s||strlen(s)!=len)return fail(v,"java/lang/UnsupportedOperationException");
  pattern_data *pd=p->data;regex_t *re=&pd->regex;regmatch_t match[MATCH_SLOTS];
  if(IS("matches","()Z")){
   size_t span=0;char *buf=matcher_region_copy(s,d->region_start,d->region_end,&span);
   if(!buf)return fail(v,"java/lang/OutOfMemoryError");
   int rc=regexec(re,buf,(size_t)pd->capture_count,match,0);
   bool ok=rc==0&&match[0].rm_so==0&&(size_t)match[0].rm_eo==span;
   free(buf);
   if(ok)matcher_store(d,match,pd->capture_count,d->region_start);
   else matcher_clear_match(d);
   out->i=ok;return true;
  }
  if(IS("lookingAt","()Z")){
   size_t span=0;char *buf=matcher_region_copy(s,d->region_start,d->region_end,&span);
   if(!buf)return fail(v,"java/lang/OutOfMemoryError");
   int rc=regexec(re,buf,(size_t)pd->capture_count,match,0);
   bool ok=rc==0&&match[0].rm_so==0;
   free(buf);
   if(ok){matcher_store(d,match,pd->capture_count,d->region_start);d->next=d->match_end;}
   else matcher_clear_match(d);
   out->i=ok;return true;
  }
  if(IS("find","()Z")){
   if(d->next>d->region_end){matcher_clear_match(d);out->i=0;return true;}
   size_t span=0;char *buf=matcher_region_copy(s,d->next,d->region_end,&span);
   if(!buf)return fail(v,"java/lang/OutOfMemoryError");
   int flags=d->next>d->region_start?REG_NOTBOL:0;
   int rc=regexec(re,buf,(size_t)pd->capture_count,match,flags);
   free(buf);
   if(rc==0){
    matcher_store(d,match,pd->capture_count,d->next);
    /* Java advances past a zero-length match by one code unit. */
    d->next=d->match_end>d->match_start?d->match_end:d->match_start+1;
    out->i=1;
   }else{matcher_clear_match(d);d->next=d->region_end+1;out->i=0;}
   return true;
  }
  if(IS("group","(I)Ljava/lang/String;")&&n==1){
   if(!d->matched)return fail(v,"java/lang/IllegalStateException");
   int index=a[0].i;
   /* Valid Java group indices are 0..groupCount(), not regexec slots. */
   if(index<0||index>pd->java_groups)return fail(v,"java/lang/IndexOutOfBoundsException");
   int slot=pd->group_map[index];
   if(slot<0||slot>=pd->capture_count||slot>=MATCH_SLOTS){out->l=0;return true;}
   regmatch_t g=d->groups[slot];
   if(g.rm_so<0||g.rm_eo<g.rm_so){out->l=0;return true;}
   out->l=dvm_new_string_n(v,s+g.rm_so,(size_t)(g.rm_eo-g.rm_so));return true;
  }
  if(IS("start","()I")){
   if(!d->matched)return fail(v,"java/lang/IllegalStateException");
   out->i=(int32_t)d->match_start;return true;
  }
  if(IS("end","()I")){
   if(!d->matched)return fail(v,"java/lang/IllegalStateException");
   out->i=(int32_t)d->match_end;return true;
  }
  if(IS("region","(II)Ljava/util/regex/Matcher;")&&n==2){
   int32_t start=a[0].i,end=a[1].i;
   if(start<0||end<start||(size_t)end>len)return fail(v,"java/lang/IndexOutOfBoundsException");
   d->region_start=(size_t)start;d->region_end=(size_t)end;d->next=d->region_start;
   matcher_clear_match(d);out->l=self;return true;
  }
  if(IS("reset","()Ljava/util/regex/Matcher;")){
   /* Java reset() clears match state and restores the 0..length region. */
   d->region_start=0;d->region_end=len;d->next=0;matcher_clear_match(d);out->l=self;return true;
  }
 }
 return false;
}
/* Every early selftest failure is labelled with a stage and source line.
 * The exception is described and cleared before returning: java_basics_selftest
 * runs every runtime selftest on one shared VM and continues after a failure,
 * so a pending exception must not leak into socket_runtime_selftest(). */
static bool selftest_failure(struct dvm *v,const char *stage,int line){
 if(dvm_exception(v)){
  char why[512];
  dvm_describe_exception(v,dvm_exception(v),why,sizeof why);
  sdk_trace_fprintf(stderr,"PLATFORM_SELFTEST FAIL stage=%s line=%d exception=%s\n",stage,line,why);
  dvm_clear_exception(v);
 }else{
  sdk_trace_fprintf(stderr,"PLATFORM_SELFTEST FAIL stage=%s line=%d\n",stage,line);
 }
 return false;
}
#define PLATFORM_FAIL(stage) do{ \
 if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","PLATFORM_SELFTEST %s failed",(stage)); \
 return selftest_failure(v,(stage),__LINE__); \
}while(0)
bool platform_runtime_selftest(struct dvm *v){
 union dvm_value out={0},arg[3]={{0},{0},{0}};
 sdk_trace_fprintf(stderr,"PLATFORM_SELFTEST BEGIN resource_table=%d\n",PLATFORM_RESOURCE_TABLE);
 if(!v){sdk_trace_fprintf(stderr,"PLATFORM_SELFTEST FAIL stage=null_vm line=%d\n",__LINE__);return false;}
 if(v!=active)PLATFORM_FAIL("vm_identity");
 arg[0].l=dvm_new_string(v,"GMT");
 if(!arg[0].l)PLATFORM_FAIL("alloc_gmt");
 if(!platform_runtime_call(v,"java/util/TimeZone","getTimeZone","(Ljava/lang/String;)Ljava/util/TimeZone;",0,arg,1,&out)||!out.l)PLATFORM_FAIL("timezone_gmt");
 dvm_ref zone=out.l;
 if(!platform_runtime_call(v,"java/util/TimeZone","getRawOffset","()I",zone,NULL,0,&out)||out.i)PLATFORM_FAIL("timezone_raw_offset");
 if(!platform_runtime_call(v,"java/util/TimeZone","getID","()Ljava/lang/String;",zone,NULL,0,&out)||!out.l)PLATFORM_FAIL("timezone_id_call");
 const char *zone_id=dvm_string_utf8(v,out.l);
 if(!zone_id||strcmp(zone_id,"GMT"))PLATFORM_FAIL("timezone_id_value");
 arg[0].l=dvm_new_string(v,"UTC");
 if(!arg[0].l)PLATFORM_FAIL("alloc_utc");
 if(!platform_runtime_call(v,"java/util/TimeZone","getTimeZone","(Ljava/lang/String;)Ljava/util/TimeZone;",0,arg,1,&out)||!out.l||out.l==zone)PLATFORM_FAIL("timezone_utc");
 arg[0].l=dvm_new_string(v,"Europe/Paris");
 if(!arg[0].l)PLATFORM_FAIL("alloc_paris");
 bool paris_rejected=platform_runtime_call(v,"java/util/TimeZone","getTimeZone","(Ljava/lang/String;)Ljava/util/TimeZone;",0,arg,1,&out)&&exception_named(v,"java/lang/UnsupportedOperationException");
 dvm_clear_exception(v);
 if(!paris_rejected)PLATFORM_FAIL("timezone_reject_paris");
 arg[0].l=dvm_new_string(v,java_ip_regex);
 if(!arg[0].l)PLATFORM_FAIL("alloc_ip_regex");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)||!out.l||dvm_exception(v))PLATFORM_FAIL("regex_compile_ip");
 dvm_ref pattern=out.l;
 static const struct {const char *input;int matches,find;} cases[]={ {"2001:db8::1",1,1},{"127.0.0.1",1,1},{"x127.0.0.1y",0,1},{"xyz",0,0} };
 for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
  arg[0].l=dvm_new_string(v,cases[i].input);
  if(!arg[0].l)PLATFORM_FAIL("alloc_regex_input");
  if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("regex_matcher");
  dvm_ref matcher=out.l;
  if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",matcher,NULL,0,&out)||out.i!=cases[i].matches)PLATFORM_FAIL("regex_matches");
  if(!platform_runtime_call(v,"java/util/regex/Matcher","find","()Z",matcher,NULL,0,&out)||out.i!=cases[i].find)PLATFORM_FAIL("regex_find");
 }
 arg[0].l=dvm_new_string(v,java_vm_version_regex);
 if(!arg[0].l)PLATFORM_FAIL("alloc_vm_version_regex");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)||!out.l||dvm_exception(v))PLATFORM_FAIL("regex_compile_vm_version");
 dvm_ref version_pattern=out.l;
 arg[0].l=dvm_new_string(v,"2.1.0");
 if(!arg[0].l)PLATFORM_FAIL("alloc_vm_version");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",version_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("regex_version_matcher");
 dvm_ref version_matcher=out.l;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",version_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("regex_version_matches");
 union dvm_value group={.i=1};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",version_matcher,&group,1,&out)||!out.l)PLATFORM_FAIL("regex_group_1_call");
 const char *group_one=dvm_string_utf8(v,out.l);
 if(!group_one||strcmp(group_one,"2"))PLATFORM_FAIL("regex_group_1_value");
 group.i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",version_matcher,&group,1,&out)||!out.l)PLATFORM_FAIL("regex_group_2_call");
 const char *group_two=dvm_string_utf8(v,out.l);
 if(!group_two||strcmp(group_two,"1"))PLATFORM_FAIL("regex_group_2_value");
 /* DiskLruCache.<clinit> compiles its key regex.  Exercise full-match,
    substring find, group(0), the 120-character interval bound, and the
    absence of any capture group. */
 arg[0].l=dvm_new_string(v,disk_lru_cache_key_regex);
 if(!arg[0].l)PLATFORM_FAIL("alloc_disk_lru_cache_regex");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)||!out.l||dvm_exception(v))PLATFORM_FAIL("regex_compile_disk_lru_cache");
 dvm_ref key_pattern=out.l;
 static const struct {const char *input;int matches,find;size_t find_len;} key_cases[]={
  {"a",1,1,1},{"A0_-z",1,1,5},{"abc/def",0,1,3},{"",0,0,0}
 };
 for(size_t i=0;i<sizeof key_cases/sizeof key_cases[0];i++){
  arg[0].l=dvm_new_string(v,key_cases[i].input);
  if(!arg[0].l)PLATFORM_FAIL("alloc_disk_lru_cache_input");
  if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",key_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("regex_disk_lru_cache_matcher");
  dvm_ref key_matcher=out.l;
  if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",key_matcher,NULL,0,&out)||out.i!=key_cases[i].matches)PLATFORM_FAIL("regex_disk_lru_cache_matches");
  if(!platform_runtime_call(v,"java/util/regex/Matcher","find","()Z",key_matcher,NULL,0,&out)||out.i!=key_cases[i].find)PLATFORM_FAIL("regex_disk_lru_cache_find");
  if(key_cases[i].find){
   union dvm_value whole={.i=0};
   if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",key_matcher,&whole,1,&out)||!out.l)PLATFORM_FAIL("regex_disk_lru_cache_group0");
   if(dvm_string_utf8_length(v,out.l)!=key_cases[i].find_len)PLATFORM_FAIL("regex_disk_lru_cache_group0_len");
  }
 }
 char key_boundary[122];
 memset(key_boundary,'a',121);key_boundary[121]='\0';
 arg[0].l=dvm_new_string(v,key_boundary);
 if(!arg[0].l)PLATFORM_FAIL("alloc_disk_lru_cache_boundary");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",key_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("regex_disk_lru_cache_boundary_matcher");
 dvm_ref key_boundary_matcher=out.l;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",key_boundary_matcher,NULL,0,&out)||out.i)PLATFORM_FAIL("regex_disk_lru_cache_boundary_matches");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","find","()Z",key_boundary_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("regex_disk_lru_cache_boundary_find");
 union dvm_value boundary_group={.i=0};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",key_boundary_matcher,&boundary_group,1,&out)||!out.l||dvm_string_utf8_length(v,out.l)!=120)PLATFORM_FAIL("regex_disk_lru_cache_boundary_group0");
 key_boundary[120]='\0';
 arg[0].l=dvm_new_string(v,key_boundary);
 if(!arg[0].l)PLATFORM_FAIL("alloc_disk_lru_cache_max");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",key_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("regex_disk_lru_cache_max_matcher");
 dvm_ref key_max_matcher=out.l;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",key_max_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("regex_disk_lru_cache_max_matches");
 union dvm_value max_group={.i=0};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",key_max_matcher,&max_group,1,&out)||!out.l||dvm_string_utf8_length(v,out.l)!=120)PLATFORM_FAIL("regex_disk_lru_cache_max_group0");
 union dvm_value no_such_group={.i=1};
 bool key_group_rejected=platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",key_max_matcher,&no_such_group,1,&out)&&exception_named(v,"java/lang/IndexOutOfBoundsException");
 dvm_clear_exception(v);
 if(!key_group_rejected)PLATFORM_FAIL("regex_disk_lru_cache_group1_rejected");
 /* OkHttp MediaType TYPE/SUBTYPE regex: exact Java set, including '-', with
  * two capture groups; matches() is whole-input. */
 arg[0].l=dvm_new_string(v,http_media_type_java_regex);
 if(!arg[0].l)PLATFORM_FAIL("alloc_http_media_regex");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)||!out.l||dvm_exception(v))PLATFORM_FAIL("regex_compile_http_media");
 dvm_ref media_pattern=out.l;
 arg[0].l=dvm_new_string(v,"application/json");
 if(!arg[0].l)PLATFORM_FAIL("alloc_http_media_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",media_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("regex_http_media_matcher");
 dvm_ref media_matcher=out.l;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",media_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("regex_http_media_matches");
 union dvm_value media_group={.i=1};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",media_matcher,&media_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"application"))PLATFORM_FAIL("regex_http_media_group1");
 media_group.i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",media_matcher,&media_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"json"))PLATFORM_FAIL("regex_http_media_group2");
 arg[0].l=dvm_new_string(v,"a-b/c");
 if(!arg[0].l)PLATFORM_FAIL("alloc_http_media_hyphen");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",media_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("regex_http_media_hyphen_matcher");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",out.l,NULL,0,&out)||!out.i)PLATFORM_FAIL("regex_http_media_hyphen_matches");
 arg[0].l=dvm_new_string(v,"text/plain; charset=utf-8");
 if(!arg[0].l)PLATFORM_FAIL("alloc_http_media_param");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",media_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("regex_http_media_param_matcher");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",out.l,NULL,0,&out)||out.i)PLATFORM_FAIL("regex_http_media_param_rejected");
 /* OkHttp MediaType.get calls TYPE_SUBTYPE.lookingAt(), group(1)/group(2),
  * and end(); reproduce that exact sequence. */
 const char *media_text="text/plain; charset=utf-8";
 const int media_len=(int)strlen(media_text);
 arg[0].l=dvm_new_string(v,media_text);
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_looking_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",media_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("media_looking_matcher");
 dvm_ref media_looking=out.l;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",media_looking,NULL,0,&out)||!out.i)PLATFORM_FAIL("media_looking_at");
 union dvm_value media_looking_group={.i=1};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",media_looking,&media_looking_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"text"))PLATFORM_FAIL("media_looking_group1");
 media_looking_group.i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",media_looking,&media_looking_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"plain"))PLATFORM_FAIL("media_looking_group2");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","end","()I",media_looking,NULL,0,&out)||out.i!=10)PLATFORM_FAIL("media_looking_end");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","start","()I",media_looking,NULL,0,&out)||out.i!=0)PLATFORM_FAIL("media_looking_start");
 /* region() keeps matches/find inside the span and reports absolute
  * offsets.  In "xx text/plain yy" the type/subtype occupies 3..13. */
 const char *padded_text="xx text/plain yy";
 arg[0].l=dvm_new_string(v,padded_text);
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_scoped_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",media_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("media_scoped_matcher");
 dvm_ref scoped_matcher=out.l;
 union dvm_value scoped_region[2]={{.i=3},{.i=13}};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",scoped_matcher,scoped_region,2,&out)||out.l!=scoped_matcher)PLATFORM_FAIL("media_scoped_region");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","matches","()Z",scoped_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("media_scoped_matches");
 union dvm_value scoped_group={.i=1};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",scoped_matcher,&scoped_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"text"))PLATFORM_FAIL("media_scoped_group1");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","start","()I",scoped_matcher,NULL,0,&out)||out.i!=3)PLATFORM_FAIL("media_scoped_start");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","end","()I",scoped_matcher,NULL,0,&out)||out.i!=13)PLATFORM_FAIL("media_scoped_end");
 arg[0].l=dvm_new_string(v,"text/plain");
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_truncated_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",media_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("media_truncated_matcher");
 dvm_ref truncated_matcher=out.l;
 union dvm_value truncated_region[2]={{.i=0},{.i=5}};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",truncated_matcher,truncated_region,2,&out)||out.l!=truncated_matcher)PLATFORM_FAIL("media_truncated_region");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","find","()Z",truncated_matcher,NULL,0,&out)||out.i)PLATFORM_FAIL("media_find_crosses_region");
 /* OkHttp MediaType PARAMETER regex: exact Java pattern is accepted; POSIX
  * group remapping must present charset/utf-8/null for group(1..3). */
 arg[0].l=dvm_new_string(v,http_media_parameter_java_regex);
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_parameter_regex");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)||!out.l||dvm_exception(v))PLATFORM_FAIL("regex_compile_media_parameter");
 dvm_ref parameter_pattern=out.l;
 arg[0].l=dvm_new_string(v,media_text);
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_parameter_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",parameter_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("media_parameter_matcher");
 dvm_ref parameter_matcher=out.l;
 union dvm_value parameter_region[2]={{.i=10},{.i=media_len}};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",parameter_matcher,parameter_region,2,&out)||out.l!=parameter_matcher)PLATFORM_FAIL("media_parameter_region");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",parameter_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("media_parameter_looking_at");
 union dvm_value parameter_group={.i=1};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",parameter_matcher,&parameter_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"charset"))PLATFORM_FAIL("media_parameter_group1");
 parameter_group.i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",parameter_matcher,&parameter_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"utf-8"))PLATFORM_FAIL("media_parameter_group2");
 parameter_group.i=3;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",parameter_matcher,&parameter_group,1,&out)||out.l)PLATFORM_FAIL("media_parameter_group3_absent");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","end","()I",parameter_matcher,NULL,0,&out)||out.i!=media_len)PLATFORM_FAIL("media_parameter_end");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","start","()I",parameter_matcher,NULL,0,&out)||out.i!=10)PLATFORM_FAIL("media_parameter_start");
 /* Walk two parameters, including the quoted value, with the same
  * region/lookingAt/end loop MediaType.get uses. */
 const char *parameter_text="; charset=utf-8; boundary=\"abc\"";
 const int parameter_len=(int)strlen(parameter_text);
 arg[0].l=dvm_new_string(v,parameter_text);
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_parameter_loop_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",parameter_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("media_parameter_loop_matcher");
 dvm_ref loop_matcher=out.l;
 union dvm_value loop_region[2]={{.i=0},{.i=parameter_len}};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",loop_matcher,loop_region,2,&out)||out.l!=loop_matcher)PLATFORM_FAIL("media_parameter_loop_region1");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",loop_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("media_parameter_loop_looking1");
 union dvm_value loop_group={.i=1};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",loop_matcher,&loop_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"charset"))PLATFORM_FAIL("media_parameter_loop_group1a");
 loop_group.i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",loop_matcher,&loop_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"utf-8"))PLATFORM_FAIL("media_parameter_loop_group2a");
 loop_group.i=3;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",loop_matcher,&loop_group,1,&out)||out.l)PLATFORM_FAIL("media_parameter_loop_group3a_absent");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","end","()I",loop_matcher,NULL,0,&out)||out.i!=15)PLATFORM_FAIL("media_parameter_loop_end1");
 int parameter_end=out.i;
 loop_region[0].i=parameter_end;loop_region[1].i=parameter_len;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",loop_matcher,loop_region,2,&out)||out.l!=loop_matcher)PLATFORM_FAIL("media_parameter_loop_region2");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",loop_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("media_parameter_loop_looking2");
 loop_group.i=1;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",loop_matcher,&loop_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"boundary"))PLATFORM_FAIL("media_parameter_loop_group1b");
 loop_group.i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",loop_matcher,&loop_group,1,&out)||out.l)PLATFORM_FAIL("media_parameter_loop_group2b_absent");
 loop_group.i=3;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",loop_matcher,&loop_group,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"abc"))PLATFORM_FAIL("media_parameter_loop_group3b");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","start","()I",loop_matcher,NULL,0,&out)||out.i!=parameter_end)PLATFORM_FAIL("media_parameter_loop_start2");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","end","()I",loop_matcher,NULL,0,&out)||out.i!=parameter_len)PLATFORM_FAIL("media_parameter_loop_end2");
 loop_region[0].i=parameter_len;loop_region[1].i=parameter_len;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",loop_matcher,loop_region,2,&out)||out.l!=loop_matcher)PLATFORM_FAIL("media_parameter_loop_region3");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",loop_matcher,NULL,0,&out)||out.i)PLATFORM_FAIL("media_parameter_loop_exhausted");
 /* reset() drops the explicit region and all match state. */
 if(!platform_runtime_call(v,"java/util/regex/Matcher","reset","()Ljava/util/regex/Matcher;",loop_matcher,NULL,0,&out)||out.l!=loop_matcher)PLATFORM_FAIL("media_parameter_loop_reset");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",loop_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("media_parameter_loop_reset_looking");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","end","()I",loop_matcher,NULL,0,&out)||out.i!=15)PLATFORM_FAIL("media_parameter_loop_reset_end");
 /* A valueless parameter only consumes ';' plus whitespace; OkHttp's loop
  * then restarts at the bare name, where lookingAt must fail. */
 const char *flag_text="; flag";
 const int flag_len=(int)strlen(flag_text);
 arg[0].l=dvm_new_string(v,flag_text);
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_parameter_flag_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",parameter_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("media_parameter_flag_matcher");
 dvm_ref flag_matcher=out.l;
 union dvm_value flag_region[2]={{.i=0},{.i=flag_len}};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",flag_matcher,flag_region,2,&out)||out.l!=flag_matcher)PLATFORM_FAIL("media_parameter_flag_region");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",flag_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("media_parameter_flag_looking");
 union dvm_value flag_group={.i=1};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",flag_matcher,&flag_group,1,&out)||out.l)PLATFORM_FAIL("media_parameter_flag_name_absent");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","end","()I",flag_matcher,NULL,0,&out)||out.i!=2)PLATFORM_FAIL("media_parameter_flag_end");
 flag_region[0].i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",flag_matcher,flag_region,2,&out)||out.l!=flag_matcher)PLATFORM_FAIL("media_parameter_flag_rest_region");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",flag_matcher,NULL,0,&out)||out.i)PLATFORM_FAIL("media_parameter_flag_rest_looking");
 /* Unterminated quoted values and misplaced ';' also fail the next
  * lookingAt exactly where MediaType.get would raise. */
 const char *bad_quote_text="; boundary=\"abc";
 const int bad_quote_len=(int)strlen(bad_quote_text);
 arg[0].l=dvm_new_string(v,bad_quote_text);
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_parameter_bad_quote_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",parameter_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("media_parameter_bad_quote_matcher");
 dvm_ref bad_quote_matcher=out.l;
 union dvm_value bad_quote_region[2]={{.i=0},{.i=bad_quote_len}};
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",bad_quote_matcher,bad_quote_region,2,&out)||out.l!=bad_quote_matcher)PLATFORM_FAIL("media_parameter_bad_quote_region");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",bad_quote_matcher,NULL,0,&out)||!out.i)PLATFORM_FAIL("media_parameter_bad_quote_looking");
 bad_quote_region[0].i=2;
 if(!platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",bad_quote_matcher,bad_quote_region,2,&out)||out.l!=bad_quote_matcher)PLATFORM_FAIL("media_parameter_bad_quote_rest_region");
 if(!platform_runtime_call(v,"java/util/regex/Matcher","lookingAt","()Z",bad_quote_matcher,NULL,0,&out)||out.i)PLATFORM_FAIL("media_parameter_bad_quote_rest_looking");
 /* Java group(4) is out of range for the 3-group PARAMETER pattern. */
 union dvm_value extra_group={.i=4};
 bool parameter_group4_rejected=platform_runtime_call(v,"java/util/regex/Matcher","group","(I)Ljava/lang/String;",parameter_matcher,&extra_group,1,&out)&&exception_named(v,"java/lang/IndexOutOfBoundsException");
 dvm_clear_exception(v);
 if(!parameter_group4_rejected)PLATFORM_FAIL("media_parameter_group4_rejected");
 /* region() validates 0 <= start <= end <= length. */
 union dvm_value invalid_region[2]={{.i=5},{.i=3}};
 bool region_order_rejected=platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",parameter_matcher,invalid_region,2,&out)&&exception_named(v,"java/lang/IndexOutOfBoundsException");
 dvm_clear_exception(v);
 if(!region_order_rejected)PLATFORM_FAIL("media_parameter_region_order");
 invalid_region[0].i=0;invalid_region[1].i=media_len+1;
 bool region_overflow_rejected=platform_runtime_call(v,"java/util/regex/Matcher","region","(II)Ljava/util/regex/Matcher;",parameter_matcher,invalid_region,2,&out)&&exception_named(v,"java/lang/IndexOutOfBoundsException");
 dvm_clear_exception(v);
 if(!region_overflow_rejected)PLATFORM_FAIL("media_parameter_region_overflow");
 /* end() before any successful match throws IllegalStateException. */
 arg[0].l=dvm_new_string(v,"text/plain");
 if(!arg[0].l)PLATFORM_FAIL("alloc_media_end_state_input");
 if(!platform_runtime_call(v,"java/util/regex/Pattern","matcher","(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;",media_pattern,arg,1,&out)||!out.l)PLATFORM_FAIL("media_end_state_matcher");
 dvm_ref end_state_matcher=out.l;
 bool end_state_rejected=platform_runtime_call(v,"java/util/regex/Matcher","end","()I",end_state_matcher,NULL,0,&out)&&exception_named(v,"java/lang/IllegalStateException");
 dvm_clear_exception(v);
 if(!end_state_rejected)PLATFORM_FAIL("media_end_without_match");
 arg[0].l=dvm_new_string(v,"[a-z]+");
 if(!arg[0].l)PLATFORM_FAIL("alloc_unknown_regex");
 bool unknown_regex=platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)&&exception_named(v,"java/lang/UnsupportedOperationException");
 dvm_clear_exception(v);
 if(!unknown_regex)PLATFORM_FAIL("regex_reject_unknown");
 arg[0].l=dvm_new_string(v,disk_lru_cache_key_regex);arg[1].i=2;
 if(!arg[0].l)PLATFORM_FAIL("alloc_disk_lru_cache_flags");
 bool key_flags_rejected=platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;I)Ljava/util/regex/Pattern;",0,arg,2,&out)&&exception_named(v,"java/lang/UnsupportedOperationException");
 dvm_clear_exception(v);
 if(!key_flags_rejected)PLATFORM_FAIL("regex_disk_lru_cache_reject_flags");
 arg[0].l=dvm_new_string(v,"[");
 if(!arg[0].l)PLATFORM_FAIL("alloc_bad_regex");
 bool invalid_regex=platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;)Ljava/util/regex/Pattern;",0,arg,1,&out)&&exception_named(v,"java/util/regex/PatternSyntaxException");
 dvm_clear_exception(v);
 if(!invalid_regex)PLATFORM_FAIL("regex_reject_invalid");
 arg[0].l=dvm_new_string(v,java_ip_regex);arg[1].i=2;
 if(!arg[0].l)PLATFORM_FAIL("alloc_flags_regex");
 bool flags_rejected=platform_runtime_call(v,"java/util/regex/Pattern","compile","(Ljava/lang/String;I)Ljava/util/regex/Pattern;",0,arg,2,&out)&&exception_named(v,"java/lang/UnsupportedOperationException");
 dvm_clear_exception(v);
 if(!flags_rejected)PLATFORM_FAIL("regex_reject_flags");
 if(!platform_runtime_call(v,"java/util/Locale","getDefault","()Ljava/util/Locale;",0,NULL,0,&out)||!out.l)PLATFORM_FAIL("locale_get_default");
 dvm_ref loc=out.l;
 if(!platform_runtime_call(v,"java/util/Locale","getDefault","()Ljava/util/Locale;",0,NULL,0,&out)||out.l!=loc)PLATFORM_FAIL("locale_identity");
 if(!platform_runtime_call(v,"java/util/Locale","toString","()Ljava/lang/String;",loc,NULL,0,&out)||!out.l)PLATFORM_FAIL("locale_to_string_call");
 const char *locale_name=dvm_string_utf8(v,out.l);
 if(!locale_name||strcmp(locale_name,"en_US"))PLATFORM_FAIL("locale_to_string_value");
 /* Locale.US/Locale.ROOT are external static fields read by sget-object.
  * The VM's get_external_static hook reaches java_basics_static, which must
  * return real, cached java/util/Locale objects with the Java language and
  * country values; they are separate from the getDefault singleton above.
  * Unknown fields and wrong descriptors must remain fail-closed. */
 union dvm_value locale_static={0};
 if(!java_basics_static(v,"java/util/Locale","US","Ljava/util/Locale;",&locale_static)||!locale_static.l)PLATFORM_FAIL("locale_us_static");
 dvm_ref locale_us=locale_static.l;
 if(locale_us==loc)PLATFORM_FAIL("locale_us_not_default_identity");
 if(!java_basics_static(v,"java/util/Locale","US","Ljava/util/Locale;",&locale_static)||locale_static.l!=locale_us)PLATFORM_FAIL("locale_us_identity");
 if(!java_basics_call(v,"java/util/Locale","getLanguage","()Ljava/lang/String;",locale_us,NULL,0,&out)||!out.l)PLATFORM_FAIL("locale_us_language_call");
 const char *us_language=dvm_string_utf8(v,out.l);
 if(!us_language||strcmp(us_language,"en"))PLATFORM_FAIL("locale_us_language_value");
 if(!java_basics_call(v,"java/util/Locale","getCountry","()Ljava/lang/String;",locale_us,NULL,0,&out)||!out.l)PLATFORM_FAIL("locale_us_country_call");
 const char *us_country=dvm_string_utf8(v,out.l);
 if(!us_country||strcmp(us_country,"US"))PLATFORM_FAIL("locale_us_country_value");
 if(!java_basics_call(v,"java/util/Locale","toString","()Ljava/lang/String;",locale_us,NULL,0,&out)||!out.l)PLATFORM_FAIL("locale_us_to_string_call");
 const char *us_name=dvm_string_utf8(v,out.l);
 if(!us_name||strcmp(us_name,"en_US"))PLATFORM_FAIL("locale_us_to_string_value");
 if(!java_basics_static(v,"java/util/Locale","ROOT","Ljava/util/Locale;",&locale_static)||!locale_static.l)PLATFORM_FAIL("locale_root_static");
 dvm_ref locale_root=locale_static.l;
 if(locale_root==locale_us)PLATFORM_FAIL("locale_root_identity_distinct");
 if(!java_basics_call(v,"java/util/Locale","getLanguage","()Ljava/lang/String;",locale_root,NULL,0,&out)||!out.l)PLATFORM_FAIL("locale_root_language_call");
 const char *root_language=dvm_string_utf8(v,out.l);
 if(!root_language||strcmp(root_language,""))PLATFORM_FAIL("locale_root_language_value");
 if(!java_basics_call(v,"java/util/Locale","getCountry","()Ljava/lang/String;",locale_root,NULL,0,&out)||!out.l)PLATFORM_FAIL("locale_root_country_call");
 const char *root_country=dvm_string_utf8(v,out.l);
 if(!root_country||strcmp(root_country,""))PLATFORM_FAIL("locale_root_country_value");
 if(!java_basics_call(v,"java/util/Locale","toString","()Ljava/lang/String;",locale_root,NULL,0,&out)||!out.l)PLATFORM_FAIL("locale_root_to_string_call");
 const char *root_name=dvm_string_utf8(v,out.l);
 if(!root_name||strcmp(root_name,""))PLATFORM_FAIL("locale_root_to_string_value");
 bool unknown_locale_static=java_basics_static(v,"java/util/Locale","UK","Ljava/util/Locale;",&locale_static);
 dvm_clear_exception(v);
 if(unknown_locale_static)PLATFORM_FAIL("locale_unknown_field_claimed");
 bool wrong_type_static=java_basics_static(v,"java/util/Locale","US","Ljava/lang/Object;",&locale_static);
 dvm_clear_exception(v);
 if(wrong_type_static)PLATFORM_FAIL("locale_wrong_type_claimed");
 /* LocaleList and Configuration.getLocales() are exercised with a synthetic
  * host snapshot that is restored before returning.  The default fallback is
  * the VM's real Locale.getDefault(), not an invented host value. */
 if(!locale_runtime_selftest(v))PLATFORM_FAIL("locale_runtime");
 /* DisplayMetrics/Configuration/Resources are exercised on VM objects.  The
  * real Application/asset Resources path is used when available; otherwise a
  * fixture Context/Resources keeps this diagnostic independent of when the
  * original Application is bound.  The synthetic display fixture proves the
  * host-bridge path without ever claiming its numbers are the device's. */
 extern dvm_ref android_basics_application(struct dvm*);
 extern bool asset_reader_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
 dvm_ref context=android_basics_application(v);
 bool app_context=context!=0;
 if(dvm_exception(v)){
  sdk_trace_fprintf(stderr,"PLATFORM_SELFTEST NOTE clearing exception before display/resources checks\n");
  dvm_clear_exception(v);
  app_context=false;
 }
 if(!app_context){
  sdk_trace_fprintf(stderr,"PLATFORM_SELFTEST NOTE Application unavailable; using fixture Context and Resources\n");
  context=object(v,"android/content/Context");
  if(!context)PLATFORM_FAIL("fixture_context");
 }
 platform_display_snapshot saved;char saved_source[sizeof display_source];
 display_snapshot_read(&saved,saved_source,sizeof saved_source,NULL);
 bool saved_host=display_host_present_read();
 bool ok=true;int fail_line=0;
#define REQUIRE(cond) do{if(!(cond)){fail_line=__LINE__;if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","PLATFORM_SELFTEST display/resources check failed at line %d",__LINE__);ok=false;goto restore;}}while(0)
 dvm_ref resources=0;
 if(app_context){
  REQUIRE(asset_reader_call(v,"android/content/Context","getResources","()Landroid/content/res/Resources;",context,NULL,0,&out)&&out.l&&!dvm_exception(v));
  resources=out.l;
  REQUIRE(asset_reader_call(v,"android/content/Context","getResources","()Landroid/content/res/Resources;",context,NULL,0,&out)&&out.l==resources&&!dvm_exception(v));
 }else{
  resources=object(v,"android/content/res/Resources");
  REQUIRE(resources);
 }
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getDisplayMetrics","()Landroid/util/DisplayMetrics;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l);
 dvm_ref metrics=out.l;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getDisplayMetrics","()Landroid/util/DisplayMetrics;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l==metrics);
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getConfiguration","()Landroid/content/res/Configuration;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l);
 dvm_ref config=out.l;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getConfiguration","()Landroid/content/res/Configuration;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l==config);
 platform_display_snapshot current;
 display_snapshot_read(&current,NULL,0,NULL);
 metrics_value_set current_metrics={current.width_pixels,current.height_pixels,current.density_dpi,current.density,current.scaled_density,current.xdpi,current.ydpi};
 config_value_set current_config={current.orientation,current.screen_width_dp,current.screen_height_dp,current.smallest_screen_width_dp,current.density_dpi};
 REQUIRE(check_metrics(v,metrics,&current_metrics));
 REQUIRE(check_config(v,config,&current_config));
 if(!saved_host)REQUIRE(strstr(sdk_display_source(),"diagnostic-fallback")!=NULL);
 platform_display_snapshot fixture={
  .struct_size=sizeof fixture,
  .width_pixels=1080,.height_pixels=2340,.density_dpi=480,.orientation=1,
  .screen_width_dp=360,.screen_height_dp=780,.smallest_screen_width_dp=360,
  .density=3.0f,.scaled_density=3.0f,.xdpi=440.0f,.ydpi=440.0f,
  .source="PLATFORM_SELFTEST synthetic fixture (bridge check; not a device measurement)"
 };
 REQUIRE(sdk_set_display_snapshot(&fixture));
 REQUIRE(display_host_present_read());
 REQUIRE(strstr(sdk_display_source(),"synthetic fixture")!=NULL);
 metrics_value_set fixture_metrics={1080,2340,480,3.0f,3.0f,440.0f,440.0f};
 config_value_set fixture_config={1,360,780,360,480};
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getDisplayMetrics","()Landroid/util/DisplayMetrics;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l==metrics);
 REQUIRE(check_metrics(v,metrics,&fixture_metrics));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getConfiguration","()Landroid/content/res/Configuration;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l==config);
 REQUIRE(check_config(v,config,&fixture_config));
 dvm_ref fresh=object(v,"android/util/DisplayMetrics");
 REQUIRE(fresh);
 REQUIRE(platform_runtime_call(v,"android/util/DisplayMetrics","<init>","()V",fresh,NULL,0,&out)&&!dvm_exception(v));
 REQUIRE(check_metrics(v,fresh,&fixture_metrics));
 dvm_ref defaults=object(v,"android/content/res/Configuration");
 REQUIRE(defaults);
 REQUIRE(platform_runtime_call(v,"android/content/res/Configuration","<init>","()V",defaults,NULL,0,&out)&&!dvm_exception(v));
 config_value_set zero_config={0,0,0,0,0};
 REQUIRE(check_config(v,defaults,&zero_config));
 dvm_ref copied=object(v,"android/content/res/Configuration");
 REQUIRE(copied);
 arg[0].l=config;
 REQUIRE(platform_runtime_call(v,"android/content/res/Configuration","<init>","(Landroid/content/res/Configuration;)V",copied,arg,1,&out)&&!dvm_exception(v));
 REQUIRE(check_config(v,copied,&fixture_config));
 REQUIRE(platform_runtime_call(v,"android/content/res/Configuration","setToDefaults","()V",copied,NULL,0,&out)&&!dvm_exception(v));
 REQUIRE(check_config(v,copied,&zero_config));
 arg[0].l=0;
 bool npe=platform_runtime_call(v,"android/content/res/Configuration","<init>","(Landroid/content/res/Configuration;)V",copied,arg,1,&out)&&exception_named(v,"java/lang/NullPointerException");
 dvm_clear_exception(v);
 REQUIRE(npe);
 config_value_set override_config={2,800,1280,800,240};
 metrics_value_set override_metrics={1920,1080,240,1.5f,1.5f,240.0f,240.0f};
 REQUIRE(apply_config_values(v,copied,&override_config));
 REQUIRE(apply_metrics_values(v,fresh,&override_metrics));
 arg[0].l=copied;arg[1].l=fresh;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","updateConfiguration","(Landroid/content/res/Configuration;Landroid/util/DisplayMetrics;)V",resources,arg,2,&out)&&!dvm_exception(v));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getConfiguration","()Landroid/content/res/Configuration;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l==config);
 REQUIRE(check_config(v,config,&override_config));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getDisplayMetrics","()Landroid/util/DisplayMetrics;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l==metrics);
 REQUIRE(check_metrics(v,metrics,&override_metrics));
 REQUIRE(sdk_set_display_snapshot(NULL));
 REQUIRE(!display_host_present_read());
 REQUIRE(strstr(sdk_display_source(),"diagnostic-fallback")!=NULL);
 metrics_value_set fallback_metrics={0,0,160,1.0f,1.0f,0.0f,0.0f};
 config_value_set fallback_config={0,0,0,0,160};
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getDisplayMetrics","()Landroid/util/DisplayMetrics;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l==metrics);
 REQUIRE(check_metrics(v,metrics,&fallback_metrics));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getConfiguration","()Landroid/content/res/Configuration;",resources,NULL,0,&out)&&!dvm_exception(v)&&out.l==config);
 REQUIRE(check_config(v,config,&fallback_config));
#if PLATFORM_RESOURCE_TABLE
 arg[0].i=2131558827;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getString","(I)Ljava/lang/String;",resources,arg,1,&out)&&!dvm_exception(v)&&out.l&&dvm_string_utf8_length(v,out.l)>0);
 dvm_ref known=out.l;
 const char *known_text=dvm_string_utf8(v,known);
 REQUIRE(known_text&&known_text[0]);
 REQUIRE(platform_runtime_call(v,"android/content/Context","getString","(I)Ljava/lang/String;",context,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *context_text=dvm_string_utf8(v,out.l);
 REQUIRE(context_text&&!strcmp(context_text,known_text));
 REQUIRE(platform_runtime_call(v,"android/app/Application","getString","(I)Ljava/lang/String;",context,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *application_text=dvm_string_utf8(v,out.l);
 REQUIRE(application_text&&!strcmp(application_text,known_text));
 arg[1].l=0;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getString","(I[Ljava/lang/Object;)Ljava/lang/String;",resources,arg,2,&out)&&!dvm_exception(v)&&out.l);
 const char *formatted_text=dvm_string_utf8(v,out.l);
 REQUIRE(formatted_text&&!strcmp(formatted_text,known_text));
 arg[0].i=2131558828;
 bool missing=platform_runtime_call(v,"android/content/res/Resources","getString","(I)Ljava/lang/String;",resources,arg,1,&out)&&exception_named(v,"android/content/res/Resources$NotFoundException");
 dvm_clear_exception(v);
 REQUIRE(missing);
#endif
 /* com/gsc/skin/base/c.<init> calls the three-argument Resources constructor
  * with getAssets()/getDisplayMetrics()/getConfiguration() of the wrapped
  * Resources.  Bind the new instance to the real asset_reader AssetManager
  * facade when the Application is present; the fixture branch only runs when
  * no Application/asset Resources exists and still uses a real VM object. */
 dvm_ref ctor_assets=0;
 if(app_context){
  REQUIRE(asset_reader_call(v,"android/content/res/Resources","getAssets","()Landroid/content/res/AssetManager;",resources,NULL,0,&out)&&out.l&&!dvm_exception(v));
  ctor_assets=out.l;
 }else{
  ctor_assets=object(v,"android/content/res/AssetManager");
  REQUIRE(ctor_assets);
 }
 dvm_ref ctor_metrics=object(v,"android/util/DisplayMetrics");
 dvm_ref ctor_config=object(v,"android/content/res/Configuration");
 REQUIRE(ctor_metrics&&ctor_config);
 REQUIRE(platform_runtime_call(v,"android/util/DisplayMetrics","<init>","()V",ctor_metrics,NULL,0,&out)&&!dvm_exception(v));
 REQUIRE(platform_runtime_call(v,"android/content/res/Configuration","<init>","()V",ctor_config,NULL,0,&out)&&!dvm_exception(v));
 metrics_value_set ctor_metrics_values={1440,3120,560,3.5f,3.5f,500.0f,500.0f};
 config_value_set ctor_config_values={2,411,891,411,560};
 REQUIRE(apply_metrics_values(v,ctor_metrics,&ctor_metrics_values));
 REQUIRE(apply_config_values(v,ctor_config,&ctor_config_values));
 dvm_ref constructed=object(v,"android/content/res/Resources");
 REQUIRE(constructed);
 union dvm_value ctor_args[3]={{.l=ctor_assets},{.l=ctor_metrics},{.l=ctor_config}};
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","<init>","(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V",constructed,ctor_args,3,&out)&&!dvm_exception(v));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getAssets","()Landroid/content/res/AssetManager;",constructed,NULL,0,&out)&&!dvm_exception(v)&&out.l==ctor_assets);
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getDisplayMetrics","()Landroid/util/DisplayMetrics;",constructed,NULL,0,&out)&&!dvm_exception(v)&&out.l&&out.l!=ctor_metrics);
 dvm_ref constructed_metrics=out.l;
 REQUIRE(check_metrics(v,constructed_metrics,&ctor_metrics_values));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getDisplayMetrics","()Landroid/util/DisplayMetrics;",constructed,NULL,0,&out)&&!dvm_exception(v)&&out.l==constructed_metrics);
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getConfiguration","()Landroid/content/res/Configuration;",constructed,NULL,0,&out)&&!dvm_exception(v)&&out.l&&out.l!=ctor_config);
 dvm_ref constructed_config=out.l;
 REQUIRE(check_config(v,constructed_config,&ctor_config_values));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getConfiguration","()Landroid/content/res/Configuration;",constructed,NULL,0,&out)&&!dvm_exception(v)&&out.l==constructed_config);
 /* updateConfiguration must update the constructed instance's own copies,
  * not the shared host-generation caches, and keep their identities. */
 metrics_value_set updated_metrics={2560,1440,320,2.0f,2.0f,320.0f,320.0f};
 config_value_set updated_config={1,800,1280,800,320};
 REQUIRE(apply_metrics_values(v,ctor_metrics,&updated_metrics));
 REQUIRE(apply_config_values(v,ctor_config,&updated_config));
 arg[0].l=ctor_config;arg[1].l=ctor_metrics;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","updateConfiguration","(Landroid/content/res/Configuration;Landroid/util/DisplayMetrics;)V",constructed,arg,2,&out)&&!dvm_exception(v));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getConfiguration","()Landroid/content/res/Configuration;",constructed,NULL,0,&out)&&!dvm_exception(v)&&out.l==constructed_config);
 REQUIRE(check_config(v,constructed_config,&updated_config));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getDisplayMetrics","()Landroid/util/DisplayMetrics;",constructed,NULL,0,&out)&&!dvm_exception(v)&&out.l==constructed_metrics);
 REQUIRE(check_metrics(v,constructed_metrics,&updated_metrics));
 /* Each null constructor argument is a NullPointerException; a failed
  * construction leaves the target without Resources fields. */
 dvm_ref rejected=object(v,"android/content/res/Resources");
 REQUIRE(rejected);
 union dvm_value null_args[3]={{.l=0},{.l=ctor_metrics},{.l=ctor_config}};
 bool null_assets=platform_runtime_call(v,"android/content/res/Resources","<init>","(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V",rejected,null_args,3,&out)&&exception_named(v,"java/lang/NullPointerException");
 dvm_clear_exception(v);
 REQUIRE(null_assets);
 null_args[0].l=ctor_assets;null_args[1].l=0;null_args[2].l=ctor_config;
 bool null_metrics=platform_runtime_call(v,"android/content/res/Resources","<init>","(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V",rejected,null_args,3,&out)&&exception_named(v,"java/lang/NullPointerException");
 dvm_clear_exception(v);
 REQUIRE(null_metrics);
 null_args[0].l=ctor_assets;null_args[1].l=ctor_metrics;null_args[2].l=0;
 bool null_config=platform_runtime_call(v,"android/content/res/Resources","<init>","(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V",rejected,null_args,3,&out)&&exception_named(v,"java/lang/NullPointerException");
 dvm_clear_exception(v);
 REQUIRE(null_config);
 bool unset_after_reject=!platform_runtime_call(v,"android/content/res/Resources","getAssets","()Landroid/content/res/AssetManager;",rejected,NULL,0,&out);
 REQUIRE(!dvm_exception(v));
 REQUIRE(unset_after_reject);
 int32_t identifier_known=0;
#if PLATFORM_RESOURCE_TABLE
 REQUIRE(arg[0].l=dvm_new_string(v,"sdk_app_id"));
 REQUIRE(arg[1].l=dvm_new_string(v,"string"));
 REQUIRE(arg[2].l=dvm_new_string(v,PLATFORM_RESOURCE_PACKAGE));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getIdentifier","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",constructed,arg,3,&out)&&!dvm_exception(v));
 identifier_known=out.i;
 REQUIRE(identifier_known==2131558826);
 REQUIRE(arg[0].l=dvm_new_string(v,"com.bilibili.star.bili:string/sdk_app_id"));
 arg[1].l=0;arg[2].l=0;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getIdentifier","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",constructed,arg,3,&out)&&!dvm_exception(v)&&out.i==2131558826);
 /* A decimal name is an id (AOSP parses it before the resource lookup), and
  * an absent type/package falls back to the extracted table. */
 REQUIRE(arg[0].l=dvm_new_string(v,"2131558826"));
 arg[1].l=0;arg[2].l=0;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getIdentifier","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",constructed,arg,3,&out)&&!dvm_exception(v)&&out.i==2131558826);
 REQUIRE(arg[0].l=dvm_new_string(v,"sdk_app_id"));
 arg[1].l=0;arg[2].l=0;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getIdentifier","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",constructed,arg,3,&out)&&!dvm_exception(v)&&out.i==2131558826);
 int32_t unknown_identifier=0;
 REQUIRE(arg[0].l=dvm_new_string(v,"no_such_resource"));
 arg[1].l=dvm_new_string(v,"string");arg[2].l=dvm_new_string(v,PLATFORM_RESOURCE_PACKAGE);
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getIdentifier","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",constructed,arg,3,&out)&&!dvm_exception(v));
 unknown_identifier=out.i;
 REQUIRE(!unknown_identifier);
 REQUIRE(arg[0].l=dvm_new_string(v,"sdk_app_id"));
 arg[1].l=dvm_new_string(v,"drawable");arg[2].l=dvm_new_string(v,PLATFORM_RESOURCE_PACKAGE);
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getIdentifier","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",constructed,arg,3,&out)&&!dvm_exception(v)&&!out.i);
 REQUIRE(arg[0].l=dvm_new_string(v,"sdk_app_id"));
 arg[1].l=dvm_new_string(v,"string");arg[2].l=dvm_new_string(v,"guest.package");
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getIdentifier","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",constructed,arg,3,&out)&&!dvm_exception(v)&&!out.i);
 /* Resource metadata is real for table ids and NotFoundException otherwise;
  * the values themselves are never printed. */
 arg[0].i=2131558827;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getResourceEntryName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *entry_name=dvm_string_utf8(v,out.l);
 REQUIRE(entry_name&&!strcmp(entry_name,"sdk_app_key"));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getResourceTypeName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *type_name=dvm_string_utf8(v,out.l);
 REQUIRE(type_name&&!strcmp(type_name,"string"));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getResourcePackageName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *package_name=dvm_string_utf8(v,out.l);
 REQUIRE(package_name&&!strcmp(package_name,PLATFORM_RESOURCE_PACKAGE));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getResourceName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *full_name=dvm_string_utf8(v,out.l);
 REQUIRE(full_name&&!strcmp(full_name,PLATFORM_RESOURCE_PACKAGE":string/sdk_app_key"));
#if PLATFORM_APK_RESOURCE_TABLE
 /* Real APK rows: 0x7f05001e bili_dynamic_main_color is a scalar color in
  * resources.arsc (APK SHA-256 recorded in apk_resource_table.inc), value
  * 0xfffb7299.  Metadata and value come from the same byte-observed row. */
 arg[0].i=0x7f05001e;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getColor","(I)I",constructed,arg,1,&out)&&!dvm_exception(v));
 REQUIRE(out.i==(int32_t)0xfffb7299u);
 arg[1].l=0;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getColor","(ILandroid/content/res/Resources$Theme;)I",constructed,arg,2,&out)&&!dvm_exception(v));
 REQUIRE(out.i==(int32_t)0xfffb7299u);
 arg[0].i=0x7f05001e;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getResourceEntryName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *real_entry=dvm_string_utf8(v,out.l);
 REQUIRE(real_entry&&!strcmp(real_entry,"bili_dynamic_main_color"));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getResourceTypeName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *real_type=dvm_string_utf8(v,out.l);
 REQUIRE(real_type&&!strcmp(real_type,"color"));
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getResourceName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *real_full=dvm_string_utf8(v,out.l);
 REQUIRE(real_full&&!strcmp(real_full,PLATFORM_RESOURCE_PACKAGE":color/bili_dynamic_main_color"));
 /* A color state-list XML has real metadata but no scalar ARGB: getColor
  * must fail closed with the recorded reason, never invent a color. */
 arg[0].i=0x7f050000;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getResourceEntryName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l);
 const char *xml_entry=dvm_string_utf8(v,out.l);
 REQUIRE(xml_entry&&!strcmp(xml_entry,"abc_background_cache_hint_selector_material_dark"));
 bool xml_color=platform_runtime_call(v,"android/content/res/Resources","getColor","(I)I",constructed,arg,1,&out)&&exception_named(v,"android/content/res/Resources$NotFoundException");
 dvm_clear_exception(v);
 REQUIRE(xml_color);
#endif
 /* 0x7f7fffff is absent from the APK table: metadata and color both throw
  * NotFoundException, exactly as Android does for an unknown id. */
 arg[0].i=0x7f7fffff;
 bool missing_metadata=platform_runtime_call(v,"android/content/res/Resources","getResourceEntryName","(I)Ljava/lang/String;",constructed,arg,1,&out)&&exception_named(v,"android/content/res/Resources$NotFoundException");
 dvm_clear_exception(v);
 REQUIRE(missing_metadata);
 bool missing_color=platform_runtime_call(v,"android/content/res/Resources","getColor","(I)I",constructed,arg,1,&out)&&exception_named(v,"android/content/res/Resources$NotFoundException");
 dvm_clear_exception(v);
 REQUIRE(missing_color);
 /* Typed readers reject a string id; they must never return a fabricated
  * 0/false/null. */
 arg[0].i=2131558826;
 bool typed_dimension=platform_runtime_call(v,"android/content/res/Resources","getDimensionPixelSize","(I)I",constructed,arg,1,&out)&&exception_named(v,"android/content/res/Resources$NotFoundException");
 dvm_clear_exception(v);
 REQUIRE(typed_dimension);
 bool typed_color=platform_runtime_call(v,"android/content/res/Resources","getColor","(I)I",constructed,arg,1,&out)&&exception_named(v,"android/content/res/Resources$NotFoundException");
 dvm_clear_exception(v);
 REQUIRE(typed_color);
 /* getText(id, fallback) returns the caller's fallback for a missing id,
  * exactly as Android does; getText(id) still fails closed. */
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getString","(I)Ljava/lang/String;",constructed,arg,1,&out)&&!dvm_exception(v)&&out.l&&dvm_string_utf8_length(v,out.l)>0);
 dvm_ref text_fallback=dvm_new_string(v,"selftest-fallback");
 REQUIRE(text_fallback);
 arg[0].i=0x7f7fffff;arg[1].l=text_fallback;
 REQUIRE(platform_runtime_call(v,"android/content/res/Resources","getText","(ILjava/lang/CharSequence;)Ljava/lang/CharSequence;",constructed,arg,2,&out)&&!dvm_exception(v)&&out.l==text_fallback);
 bool text_missing=platform_runtime_call(v,"android/content/res/Resources","getText","(I)Ljava/lang/CharSequence;",constructed,arg,1,&out)&&exception_named(v,"android/content/res/Resources$NotFoundException");
 dvm_clear_exception(v);
 REQUIRE(text_missing);
#endif
 if(dvm_exception(v)){fail_line=__LINE__;ok=false;goto restore;}
restore:
 saved.source=saved_source;
 sdk_set_display_snapshot(saved_host?&saved:NULL);
 if(!ok)return selftest_failure(v,"display_resources",fail_line);
 sdk_trace_fprintf(stderr,"PLATFORM_DISPLAY_SOURCE source=%s host=%d\n",sdk_display_source(),saved_host?1:0);
 sdk_trace_fprintf(stderr,"PLATFORM_SELFTEST PASS GMT_UTC=1 exact_IP_regex=1 matches_find=1 VM_version_regex_captures=1 DiskLruCache_key_regex=1 http_media_regex=1 matcher_region=1 matcher_looking_at=1 matcher_end=1 media_parameter_regex=1 Locale_default=1 Locale_US=1 Locale_ROOT=1 Locale_statics_fail_closed=1 LocaleList_subset=1 Configuration_locales=1 Locale_host_bridge=1 unsupported_rejected=1 DisplayMetrics_fields=1 Configuration_fields=1 Resources_identity=1 display_host_bridge=1 Resources_ctor3=1 Resources_fields=1 Resources_null_npe=1 Resources_getAssets=1 Resources_updateConfiguration=1 getIdentifier_known=%d getIdentifier_unknown_zero=1 getText_fallback=1 resource_metadata=1 typed_resource_refused=1 resource_table=%d apk_resource_table=%d apk_color_argb=1 apk_color_xml_fail_closed=1\n",(int)identifier_known,PLATFORM_RESOURCE_TABLE,PLATFORM_APK_RESOURCE_TABLE);
 return true;
}
#undef REQUIRE
#undef PLATFORM_FAIL

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int platform_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;if(o->data_destroy==host_timezone_free||o->data_destroy==pattern_free)return DVM_GC_PAYLOAD_ATOMIC;
 if(o->data_destroy==matcher_free){matcher_data *p=o->data;if(visit){visit(&p->pattern,user);visit(&p->input,user);}return DVM_GC_PAYLOAD_REFS;}return DVM_GC_PAYLOAD_UNKNOWN;
}

bool platform_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active)return false;
 if(strcmp(c,"java/util/TimeZone")&&strcmp(c,"java/util/regex/Pattern")&&strcmp(c,"java/util/regex/Matcher"))
  return platform_runtime_call_step8_operation(v,c,m,sig,self,a,n,out);
 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,self))return true;
 bool handled=platform_runtime_call_step8_operation(v,c,m,sig,self,a,n,out);
 if(handled&&!dvm_exception(v))gc_operation_result(&op,sig,out->l);
 return handled;
}
