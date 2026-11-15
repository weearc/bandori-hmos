#include "file_runtime.h"
#include "java_basics.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
static char root[PATH_MAX], cache_root[PATH_MAX], private_base[PATH_MAX]; static struct dvm *active;
int sdk_trace_fprintf(FILE *,const char *,...);
#include <stdarg.h>
/* Engine-stage diagnostic channel: sdk_trace_fprintf stops reaching the
 * progress file once the SDK runner returns, so append directly instead. */
static void file_probe_log(FILE *ignored,const char *fmt,...){
 (void)ignored;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 va_list a;va_start(a,fmt);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fclose(f);
}
extern dvm_ref android_basics_application(struct dvm *);
static void release(void *p){free(p);}
static bool err(struct dvm *v,const char *c){dvm__throw(v,c,"file runtime");return true;}
static bool allowed(const char *p){size_t n=strlen(private_base);return n&&!strncmp(p,private_base,n)&&(p[n]==0||p[n]=='/');}
static bool safe(const char *p){if(!p||strstr(p,"//"))return false;const char *s=p;while(*s){while(*s=='/')s++;const char *e=strchr(s,'/');size_t n=e?(size_t)(e-s):strlen(s);if((n==1&&s[0]=='.')||(n==2&&s[0]=='.'&&s[1]=='.'))return false;if(!e)break;s=e+1;}return true;}
/* Plain UTF-8 including NUL and non-BMP; malformed, overlong, surrogate and
 * out-of-range sequences are rejected instead of being normalised. */
static bool utf8_valid(const unsigned char *p,size_t n){for(size_t i=0;i<n;){unsigned char a=p[i];if(a<0x80){i++;continue;}size_t k=a>=0xC2&&a<=0xDF?2:a>=0xE0&&a<=0xEF?3:a>=0xF0&&a<=0xF4?4:0;if(!k||k>n-i)return false;for(size_t j=1;j<k;j++)if((p[i+j]&0xC0)!=0x80)return false;if((a==0xE0&&p[i+1]<0xA0)||(a==0xED&&p[i+1]>=0xA0)||(a==0xF0&&p[i+1]<0x90)||(a==0xF4&&p[i+1]>0x8F))return false;i+=k;}return true;}
/* getDir names are one path component: no empty string, NUL, slash, "." or
 * "..", and valid UTF-8. */
static bool dir_component(const char *s,size_t n){if(!s||!n||memchr(s,0,n)||memchr(s,'/',n)||!utf8_valid((const unsigned char*)s,n))return false;if(n==1&&s[0]=='.')return false;if(n==2&&s[0]=='.'&&s[1]=='.')return false;return true;}
static bool context_receiver(struct dvm *v,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)if(k->name&&!strcmp(k->name,"android/content/Context"))return true;return false;}
/* Create path as 0700 only if it is absent; an existing directory is reused.
 * A file or symlink at that exact path is an error, never followed. */
static bool private_dir(struct dvm *v,const char *path){struct stat st;if(lstat(path,&st)==0){if(S_ISLNK(st.st_mode)||!S_ISDIR(st.st_mode))return err(v,"java/io/IOException");return true;}if(errno!=ENOENT)return err(v,"java/io/IOException");if(mkdir(path,0700)!=0){if(errno!=EEXIST)return err(v,"java/io/IOException");if(lstat(path,&st)||S_ISLNK(st.st_mode)||!S_ISDIR(st.st_mode))return err(v,"java/io/IOException");return true;}if(lstat(path,&st)||S_ISLNK(st.st_mode)||!S_ISDIR(st.st_mode))return err(v,"java/io/IOException");return true;}
bool file_runtime_configure(const char *sdk){char path[PATH_MAX];if(!sdk||!realpath(sdk,path))return false;char *s=strrchr(path,'/');if(!s)return false;*s=0;if(snprintf(root,sizeof root,"%s/guest-files",path)>=(int)sizeof root)return false;if(snprintf(cache_root,sizeof cache_root,"%s/guest-cache",path)>=(int)sizeof cache_root)return false;if(mkdir(root,0700)&&access(root,F_OK))return false;if(mkdir(cache_root,0700)&&access(cache_root,F_OK))return false;struct stat st;if(lstat(root,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return false;if(lstat(cache_root,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return false;return true;}
/* Publish host implementations as real class declarations before Unity's
 * ReflectionHelper enumerates constructors/methods. Lazy platform lookup's
 * mcache is not a declaration table, and warming it cannot fix enumeration.
 * Keep these entries stable for the VM lifetime (JNI IDs retain pointers). */
static bool statfs_install_declarations(struct dvm *v){
 static const struct {const char *name,*sig;uint32_t access;} declarations[]={
  {"<init>","(Ljava/lang/String;)V",0x10001},
  {"restat","(Ljava/lang/String;)V",0x1},
  {"getBlockSizeLong","()J",0x1},{"getBlockCountLong","()J",0x1},
  {"getAvailableBlocksLong","()J",0x1},{"getFreeBlocksLong","()J",0x1},
  {"getAvailableBytes","()J",0x1},{"getFreeBytes","()J",0x1},
  {"getTotalBytes","()J",0x1},
  {"getBlockSize","()I",0x1},{"getBlockCount","()I",0x1},
  {"getAvailableBlocks","()I",0x1},{"getFreeBlocks","()I",0x1}
 };
 struct dvm_class *c=dvm_find_class_or_external(v,"android/os/StatFs");
 if(!c)return false;
 /* A genuine pre-existing class table is authoritative; never replace it or
  * invalidate method pointers. Normal installation encounters an empty
  * external platform class exactly once during VM initialization. */
 if(c->nmethods>0)return true;
 size_t count=sizeof declarations/sizeof declarations[0];
 struct dvm_method *methods=calloc(count,sizeof *methods);
 if(!methods){dvm__throw(v,"java/lang/OutOfMemoryError","StatFs declarations");return false;}
 for(size_t i=0;i<count;i++){
  struct dvm_method *m=&methods[i];
  m->cls=c;m->name=declarations[i].name;m->sig=strdup(declarations[i].sig);
  if(!m->sig){for(size_t j=0;j<i;j++)free(methods[j].sig);free(methods);dvm__throw(v,"java/lang/OutOfMemoryError","StatFs declaration signature");return false;}
  m->access=declarations[i].access;
  m->arg_count=dvm_sig_arg_count(m->sig);m->arg_slots=dvm_sig_arg_slots(m->sig);
  m->ret_kind=dvm_sig_return_kind(m->sig);
 }
 c->methods=methods;c->nmethods=(int)count;
 return true;
}
static bool file_install_declarations(struct dvm *);
void file_runtime_vm_start(struct dvm *v){active=v;snprintf(private_base,sizeof private_base,"%s",root);char *last=strrchr(private_base,'/');if(last)*last=0;statfs_install_declarations(v);file_install_declarations(v);}
#define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))
static dvm_ref make(struct dvm *v,const char *p){dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/File"));struct dvm_object *o=dvm__obj(v,r);if(!o)return 0;o->data=strdup(p);o->data_destroy=release;return o->data?r:0;}
#include "freeze180_file.inc"
static bool canonical_path(const char *p,char result[PATH_MAX]){return file_canonical(p,result)&&allowed(result);}
bool file_runtime_private_sqlite_path(struct dvm *v,const char *path,char *out,size_t cap){
 if(v!=active||!path||path[0]!='/'||!root[0]||!out||cap<PATH_MAX)return false;
 if(!canonical_path(path,out)||strcmp(path,out))return false;
 struct stat st;if(!lstat(path,&st)&&(!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)))return false;
 return true;
}
bool file_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){if(v!=active||!root[0])return false;
 if(!strcmp(c,"android/os/StatFs")){
  struct dvm_object *o=dvm__obj(v,self);if(!o)return err(v,"java/lang/NullPointerException");
  if(IS("<init>","(Ljava/lang/String;)V")||IS("restat","(Ljava/lang/String;)V")){
   if(n!=1||!a[0].l)return err(v,"java/lang/NullPointerException");
   const char *p=dvm_string_utf8(v,a[0].l);char resolved[PATH_MAX];struct statvfs disk;
   int path_ok=p&&realpath(p,resolved)!=NULL,path_errno=errno,inside=path_ok&&allowed(resolved),disk_ok=inside&&statvfs(resolved,&disk)==0;
   static unsigned statfs_inputs;if(statfs_inputs++<8)file_probe_log(stderr,"STATFS_INPUT path=%s realpath=%d realpath_errno=%d allowed=%d statvfs=%d errno=%d",p?p:"(null)",path_ok,path_errno,inside,disk_ok,errno);if(!disk_ok)return err(v,"java/lang/IllegalArgumentException");
   if(o->data&&o->data_destroy!=release)return false;
   if(!o->data){o->data=malloc(sizeof disk);if(!o->data)return err(v,"java/lang/OutOfMemoryError");o->data_destroy=release;}
   memcpy(o->data,&disk,sizeof disk);
   file_probe_log(stderr,"STATFS_REAL path=%s block_size=%llu blocks=%llu available=%llu",resolved,(unsigned long long)disk.f_frsize,(unsigned long long)disk.f_blocks,(unsigned long long)disk.f_bavail);
   return true;
  }
  if(o->data_destroy!=release||!o->data)return err(v,"java/lang/IllegalStateException");
  struct statvfs *disk=o->data;uint64_t value;
  if(!strcmp(m,"getBlockSize")||!strcmp(m,"getBlockSizeLong"))value=disk->f_frsize;
  else if(!strcmp(m,"getBlockCount")||!strcmp(m,"getBlockCountLong"))value=disk->f_blocks;
  else if(!strcmp(m,"getAvailableBlocks")||!strcmp(m,"getAvailableBlocksLong"))value=disk->f_bavail;
  else if(!strcmp(m,"getFreeBlocks")||!strcmp(m,"getFreeBlocksLong"))value=disk->f_bfree;
  else if(!strcmp(m,"getAvailableBytes"))value=(uint64_t)disk->f_frsize*disk->f_bavail;
  else if(!strcmp(m,"getFreeBytes"))value=(uint64_t)disk->f_frsize*disk->f_bfree;
  else if(!strcmp(m,"getTotalBytes"))value=(uint64_t)disk->f_frsize*disk->f_blocks;
  else return false;
  if(!strcmp(sig,"()J"))out->j=(int64_t)value;
  else if(!strcmp(sig,"()I"))out->i=(int32_t)value;
  else return false;
  static unsigned statfs_logs;if(statfs_logs++<16)file_probe_log(stderr,"STATFS_READ method=%s value=%llu",m,(unsigned long long)value);
  return true;
 }
 if(IS("getExternalCacheDir","()Ljava/io/File;")){
  if(!context_receiver(v,self))return false;
  char path[PATH_MAX];if(snprintf(path,sizeof path,"%s/external_cache",cache_root)>=(int)sizeof path)return err(v,"java/io/IOException");
  if(!safe(path)||!allowed(path))return err(v,"java/io/IOException");
  if(!private_dir(v,path))return true;
  out->l=make(v,path);file_probe_log(stderr,"FILE_RUNTIME_HOST_CALL getExternalCacheDir path=%s result=%d",path,out->l?1:0);
  return out->l?true:err(v,"java/lang/OutOfMemoryError");
 }
 if(IS("getFilesDir","()Ljava/io/File;")||IS("getCacheDir","()Ljava/io/File;")){bool cache=IS("getCacheDir","()Ljava/io/File;");struct dvm_object *o=dvm__obj(v,self);bool ok=false;for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)if(!strcmp(k->name,"android/content/Context"))ok=true;if(!ok)return false;out->l=make(v,cache?cache_root:root);file_probe_log(stderr,"FILE_RUNTIME_HOST_CALL %s path=%s result=%d\n",cache?"getCacheDir":"getFilesDir",cache?cache_root:root,out->l?1:0);return out->l?true:err(v,"java/lang/OutOfMemoryError");}
 /* Context.getExternalFilesDir(type): Android 返回外部私有可写目录（按需创建，
  * null type 为根）。HarmonyOS 沙箱没有 Android 的外部卷，这里映射到同一应用
  * 私有可写根下的 external_files[/type]，创建 0700；保持真实可写目录语义。 */
 if(IS("getExternalFilesDir","(Ljava/lang/String;)Ljava/io/File;")){
  if(!context_receiver(v,self))return false;
  char ext_base[PATH_MAX],path[PATH_MAX];
  if(snprintf(ext_base,sizeof ext_base,"%s/external_files",root)>=(int)sizeof ext_base)return err(v,"java/io/IOException");
  if(!safe(ext_base)||!allowed(ext_base))return err(v,"java/io/IOException");
  if(!private_dir(v,ext_base))return true;
  if(n>=1&&a[0].l){
   struct dvm_object *no=dvm__obj(v,a[0].l);
   if(!no||no->kind!=DVM_OBJ_STRING)return err(v,"java/lang/IllegalArgumentException");
   const char *type=dvm_string_utf8(v,a[0].l);uint32_t tl=dvm_string_utf8_length(v,a[0].l);
   if(!type||strlen(type)!=(size_t)tl||!dir_component(type,(size_t)tl))return err(v,"java/lang/IllegalArgumentException");
   if(snprintf(path,sizeof path,"%s/%s",ext_base,type)>=(int)sizeof path)return err(v,"java/io/IOException");
   if(!safe(path)||!allowed(path))return err(v,"java/io/IOException");
   if(!private_dir(v,path))return true;
  } else {
   if(snprintf(path,sizeof path,"%s",ext_base)>=(int)sizeof path)return err(v,"java/io/IOException");
  }
  out->l=make(v,path);
  file_probe_log(stderr,"FILE_RUNTIME_HOST_CALL getExternalFilesDir path=%s result=%d\n",path,out->l?1:0);
  return out->l?true:err(v,"java/lang/OutOfMemoryError");
 }
 if(IS("getObbDirs","()[Ljava/io/File;")){
  if(!context_receiver(v,self))return false;
  /* The primary guest OBB directory is backed by the app sandbox. */
  char path[PATH_MAX];
  if(snprintf(path,sizeof path,"%s/obb",root)>=(int)sizeof path)
   return err(v,"java/io/IOException");
  if(!safe(path)||!allowed(path)||!private_dir(v,path))
   return err(v,"java/io/IOException");
  dvm_ref files=dvm_new_array(v,'L',"Ljava/io/File;",1);
  if(!files)return err(v,"java/lang/OutOfMemoryError");
  dvm_pin(v,files);
  dvm_ref dir=make(v,path);
  if(dir)((dvm_ref*)dvm_array_data(v,files))[0]=dir;
  dvm_unpin(v,files);
  if(!dir)return err(v,"java/lang/OutOfMemoryError");
  out->l=files;
  file_probe_log(stderr,"FILE_RUNTIME_HOST_CALL getObbDirs path=%s result=1\n",path);
  return true;
 }
 if(IS("getDir","(Ljava/lang/String;I)Ljava/io/File;")){
  if(!context_receiver(v,self))return false;
  if(n<2)return err(v,"java/lang/IllegalArgumentException");
  if(!a[0].l)return err(v,"java/lang/NullPointerException");
  struct dvm_object *no=dvm__obj(v,a[0].l);if(!no||no->kind!=DVM_OBJ_STRING)return err(v,"java/lang/IllegalArgumentException");
  const char *name=dvm_string_utf8(v,a[0].l);uint32_t name_len=dvm_string_utf8_length(v,a[0].l);
  if(!name||strlen(name)!=(size_t)name_len||!dir_component(name,(size_t)name_len))return err(v,"java/lang/IllegalArgumentException");
  if(a[1].i)return err(v,"java/lang/UnsupportedOperationException");
  if(!root[0])return false;
  char path[PATH_MAX];if(snprintf(path,sizeof path,"%s/app_%s",root,name)>=(int)sizeof path)return err(v,"java/io/IOException");
  if(!safe(path)||!allowed(path))return err(v,"java/io/IOException");
  struct stat st;if(lstat(private_base,&st)||S_ISLNK(st.st_mode)||!S_ISDIR(st.st_mode))return err(v,"java/io/IOException");
  if(lstat(root,&st)||S_ISLNK(st.st_mode)||!S_ISDIR(st.st_mode))return err(v,"java/io/IOException");
  if(!private_dir(v,path))return true;
  out->l=make(v,path);return out->l?true:err(v,"java/lang/OutOfMemoryError");
 }
 if(IS("getDatabasePath","(Ljava/lang/String;)Ljava/io/File;")){
  /* Android ContextImpl.getDatabasePath(name) = new File(getDatabasesDir(),
   * name); getDatabasesDir() ensures <dataDir>/databases exists (0700) but no
   * database file is created here.  Names are single path components: null is
   * a NullPointerException, and absolute/traversal names are refused instead
   * of being joined. */
  if(!context_receiver(v,self))return false;
  if(n<1)return err(v,"java/lang/IllegalArgumentException");
  if(!a[0].l)return err(v,"java/lang/NullPointerException");
  struct dvm_object *no=dvm__obj(v,a[0].l);if(!no||no->kind!=DVM_OBJ_STRING)return err(v,"java/lang/IllegalArgumentException");
  const char *name=dvm_string_utf8(v,a[0].l);uint32_t name_len=dvm_string_utf8_length(v,a[0].l);
  if(!name||strlen(name)!=(size_t)name_len||!dir_component(name,(size_t)name_len))return err(v,"java/lang/IllegalArgumentException");
  char dir[PATH_MAX];if(snprintf(dir,sizeof dir,"%s/databases",root)>=(int)sizeof dir)return err(v,"java/io/IOException");
  if(!safe(dir)||!allowed(dir))return err(v,"java/io/IOException");
  struct stat st;if(lstat(private_base,&st)||S_ISLNK(st.st_mode)||!S_ISDIR(st.st_mode))return err(v,"java/io/IOException");
  if(lstat(root,&st)||S_ISLNK(st.st_mode)||!S_ISDIR(st.st_mode))return err(v,"java/io/IOException");
  if(!private_dir(v,dir))return true;
  char path[PATH_MAX];if(snprintf(path,sizeof path,"%s/%s",dir,name)>=(int)sizeof path)return err(v,"java/io/IOException");
  if(!safe(path)||!allowed(path))return err(v,"java/io/IOException");
  out->l=make(v,path);return out->l?true:err(v,"java/lang/OutOfMemoryError");
 }
 if(strcmp(c,"java/io/File"))return false;struct dvm_object *o=dvm__obj(v,self);
 if(IS("createTempFile","(Ljava/lang/String;Ljava/lang/String;Ljava/io/File;)Ljava/io/File;"))return file_other_extras(v,m,sig,self,o,a,out);
 if(!o)return err(v,"java/lang/NullPointerException");
 if(IS("createNewFile","()Z")){
  if(o->data_destroy!=release)return false;const char *target=o->data;out->i=0;
  if(!safe(target)||!allowed(target))return err(v,"java/lang/UnsupportedOperationException");
  char path[PATH_MAX];if(snprintf(path,sizeof path,"%s",target)>=(int)sizeof path)return err(v,"java/io/IOException");
  char *slash=strrchr(path,'/');if(!slash||slash<path+(int)strlen(private_base))return err(v,"java/io/IOException");
  *slash=0;struct stat st;if(lstat(private_base,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return err(v,"java/io/IOException");
  size_t base=strlen(private_base);for(char *q=path+base+1;*q;){char *end=strchr(q,'/');if(end)*end=0;if(lstat(path,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return err(v,"java/io/IOException");if(!end)break;*end='/';q=end+1;}
  *slash='/';int fd=open(target,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600);if(fd<0){if(errno==EEXIST)return true;return err(v,"java/io/IOException");}if(close(fd))return err(v,"java/io/IOException");out->i=1;return true;
 }
 if(IS("mkdirs","()Z")||IS("mkdir","()Z")){
  if(o->data_destroy!=release)return false;const char *target=o->data;out->i=0;
  if(!safe(target)||!allowed(target))return err(v,"java/lang/UnsupportedOperationException");
  struct stat st;if(!lstat(target,&st))return true;
  char path[PATH_MAX];snprintf(path,sizeof path,"%s",target);size_t base=strlen(private_base);
  if(lstat(private_base,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return true;
  for(char *p=path+base+1;;p++)if(*p=='/'||!*p){char saved=*p;*p=0;
   if(lstat(path,&st)){if((saved&&IS("mkdir","()Z"))||mkdir(path,0700)){*p=saved;return true;}}
   else if(S_ISLNK(st.st_mode)||!S_ISDIR(st.st_mode)){*p=saved;return true;}
   *p=saved;if(!saved)break;
  }
  out->i=1;return true;
 }
 if(IS("getParentFile","()Ljava/io/File;")||IS("getParent","()Ljava/lang/String;")){
  if(o->data_destroy!=release)return false;
  if(!strcmp(o->data,"/")){out->l=0;return true;}
  char parent[PATH_MAX];snprintf(parent,sizeof parent,"%s",(char*)o->data);
  char *slash=strrchr(parent,'/');if(!slash){out->l=0;return true;}*slash=0;
  if(slash==parent)strcpy(parent,"/");
  out->l=IS("getParentFile","()Ljava/io/File;")?make(v,parent):dvm_new_string(v,parent);
  return out->l?true:err(v,"java/lang/OutOfMemoryError");
 }
 if(!strcmp(m,"<init>")){
  const char *p=NULL;char buf[PATH_MAX];
  if(IS("<init>","(Ljava/lang/String;)V")&&n==1){
   p=dvm_string_utf8(v,a[0].l);
   if(p&&strlen(p)!=dvm_string_utf8_length(v,a[0].l))return err(v,"java/lang/UnsupportedOperationException");
  }else if(IS("<init>","(Ljava/lang/String;Ljava/lang/String;)V")&&n==2){
   const char *parent=a[0].l?dvm_string_utf8(v,a[0].l):NULL,*child=dvm_string_utf8(v,a[1].l);
   if(!child)return err(v,"java/lang/NullPointerException");
   if(parent&&*parent){if(snprintf(buf,sizeof buf,"%s/%s",parent,child)>=(int)sizeof buf)return err(v,"java/io/IOException");p=buf;}else p=child;
  }else if(IS("<init>","(Ljava/io/File;Ljava/lang/String;)V")&&n==2){
   struct dvm_object *parent=dvm__obj(v,a[0].l);const char *child=dvm_string_utf8(v,a[1].l);
   if(child&&strlen(child)!=dvm_string_utf8_length(v,a[1].l))return err(v,"java/lang/UnsupportedOperationException");
   if(!child)return err(v,"java/lang/NullPointerException");
   if(!a[0].l)p=child;
   else{
    if(!parent||parent->data_destroy!=release)return err(v,"java/lang/IllegalArgumentException");
    if(snprintf(buf,sizeof buf,"%s/%s",(char*)parent->data,child)>=(int)sizeof buf)return err(v,"java/lang/UnsupportedOperationException");
    p=buf;
   }
  }else return false;
  if(!p)return err(v,"java/lang/NullPointerException");
  if(o->data)return err(v,"java/lang/IllegalStateException");
  /* java.io.File construction only represents a pathname, it does no I/O.
   * Normalize Unix duplicate/trailing separators, without resolving dot paths
   * or requiring the represented file to exist or belong to the application.
   * All mutating/open-for-write operations retain their private-path guards. */
  size_t length=strlen(p);char *path=malloc(length+1);
  if(!path)return err(v,"java/lang/OutOfMemoryError");
  size_t used=0;for(size_t i=0;i<length;i++)if(p[i]!='/'||!used||path[used-1]!='/')path[used++]=p[i];
  if(used>1&&path[used-1]=='/')used--;path[used]=0;
  o->data=path;o->data_destroy=release;
  static unsigned path_logs;if(path_logs++<48)file_probe_log(stderr,"FILE_PATH_CREATED path=%s constructor_only=1 filesystem_access=0",path);
  return true;
 }
 if(o->data_destroy!=release)return false;const char *p=o->data;
 if(file_read_extras(v,m,sig,self,o,a,out)||file_other_extras(v,m,sig,self,o,a,out))return true;
 if(IS("getCanonicalPath","()Ljava/lang/String;")){
  char canonical[PATH_MAX];if(!file_canonical(p,canonical))return err(v,"java/io/IOException");
  out->l=dvm_new_string(v,canonical);
  static unsigned canonical_logs;if(canonical_logs++<16)file_probe_log(stderr,"FILE_CANONICAL path=%s result=%s",p,canonical);
  return out->l?true:err(v,"java/lang/OutOfMemoryError");
 }
 if(IS("getPath","()Ljava/lang/String;")||IS("getAbsolutePath","()Ljava/lang/String;")){out->l=dvm_new_string(v,p);return true;}
 if(IS("canRead","()Z")){
  int rc=access(p,R_OK),saved=errno;out->i=rc==0;
  static unsigned readable_logs;if(readable_logs++<32)file_probe_log(stderr,"FILE_PATH_QUERY method=canRead path=%s access_rc=%d errno=%d readable=%d",p,rc,rc?saved:0,out->i);
  return true;
 }
 /* Metadata queries use the OS's actual visibility and permissions. stat
  * follows symlinks, as Java File.exists/isFile/isDirectory do. A denied or
  * absent pathname returns false/zero rather than failing its constructor. */
 struct stat st;int rc=stat(p,&st),saved=errno,ok=rc==0;
 static unsigned query_logs;if(query_logs++<48)file_probe_log(stderr,"FILE_PATH_QUERY method=%s path=%s stat_rc=%d errno=%d exists=%d",m,p,rc,rc?saved:0,ok);
 if(IS("exists","()Z")){out->i=ok;return true;}
 if(IS("isDirectory","()Z")){out->i=ok&&S_ISDIR(st.st_mode);return true;}
 if(IS("isFile","()Z")){out->i=ok&&S_ISREG(st.st_mode);return true;}
 if(IS("length","()J")){out->j=ok?st.st_size:0;return true;}return false;}
static bool dir_rejected(struct dvm *v,dvm_ref ctx,dvm_ref name,int mode,const char *want){union dvm_value args[2]={{0}},out={0};args[0].l=name;args[1].i=mode;if(!file_runtime_call(v,"android/content/Context","getDir","(Ljava/lang/String;I)Ljava/io/File;",ctx,args,2,&out))return false;dvm_ref e=dvm_exception(v);if(!e)return false;struct dvm_class *c=dvm_object_class(v,e);bool match=c&&c->name&&!strcmp(c->name,want);dvm_clear_exception(v);return match;}
static bool dir_not_taken(struct dvm *v,dvm_ref receiver,dvm_ref name,int mode){union dvm_value args[2]={{0}},out={0};args[0].l=name;args[1].i=mode;return !file_runtime_call(v,"android/content/Context","getDir","(Ljava/lang/String;I)Ljava/io/File;",receiver,args,2,&out)&&!dvm_exception(v);}
static bool database_rejected(struct dvm *v,dvm_ref ctx,dvm_ref name,const char *want){union dvm_value args[1]={{0}},out={0};args[0].l=name;if(!file_runtime_call(v,"android/content/Context","getDatabasePath","(Ljava/lang/String;)Ljava/io/File;",ctx,args,1,&out))return false;dvm_ref e=dvm_exception(v);if(!e)return false;struct dvm_class *c=dvm_object_class(v,e);bool match=c&&c->name&&!strcmp(c->name,want);dvm_clear_exception(v);return match;}
static bool database_not_taken(struct dvm *v,dvm_ref receiver,dvm_ref name){union dvm_value args[1]={{0}},out={0};args[0].l=name;return !file_runtime_call(v,"android/content/Context","getDatabasePath","(Ljava/lang/String;)Ljava/io/File;",receiver,args,1,&out)&&!dvm_exception(v);}
bool file_runtime_selftest(struct dvm *v){
 union dvm_value out={0},a[2]={{0},{0}}; dvm_ref ctx=android_basics_application(v);
 if(!file_runtime_call(v,"android/content/Context","getFilesDir","()Ljava/io/File;",ctx,NULL,0,&out)||!out.l)return false;
 dvm_ref dir=out.l;
 if(!file_runtime_call(v,"android/content/Context","getCacheDir","()Ljava/io/File;",ctx,NULL,0,&out)||!out.l)return false;
 dvm_ref cache=out.l;
 if(!file_runtime_call(v,"android/content/Context","getExternalCacheDir","()Ljava/io/File;",ctx,NULL,0,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref external_cache=out.l;
 if(!file_runtime_call(v,"java/io/File","getCanonicalPath","()Ljava/lang/String;",external_cache,NULL,0,&out)||!out.l)return false;
 char expected_external[PATH_MAX];if(snprintf(expected_external,sizeof expected_external,"%s/external_cache",cache_root)>=(int)sizeof expected_external||strcmp(dvm_string_utf8(v,out.l),expected_external))return false;
 struct stat external_st;if(lstat(expected_external,&external_st)||!S_ISDIR(external_st.st_mode)||S_ISLNK(external_st.st_mode)||(external_st.st_mode&0777)!=0700)return false;
 if(!file_runtime_call(v,"android/content/Context","getExternalCacheDir","()Ljava/io/File;",ctx,NULL,0,&out)||!out.l||dvm_exception(v))return false;
 file_probe_log(stderr,"EXTERNAL_CACHE_SELFTEST PASS real_directory=1 private_mode=1 canonical=1 reuse=1");
 /* Compare StatFs snapshot values against an actual host filesystem query. */
 struct statvfs expected_disk;if(statvfs(cache_root,&expected_disk))return false;
 dvm_ref fs=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/StatFs"));
 union dvm_value fs_arg={.l=dvm_new_string(v,cache_root)};
 if(!fs||!fs_arg.l||!file_runtime_call(v,"android/os/StatFs","<init>","(Ljava/lang/String;)V",fs,&fs_arg,1,&out)||dvm_exception(v))return false;
 if(!file_runtime_call(v,"android/os/StatFs","getBlockSizeLong","()J",fs,NULL,0,&out)||(uint64_t)out.j!=expected_disk.f_frsize)return false;
 if(!file_runtime_call(v,"android/os/StatFs","getBlockCountLong","()J",fs,NULL,0,&out)||(uint64_t)out.j!=expected_disk.f_blocks)return false;
 if(!file_runtime_call(v,"android/os/StatFs","getTotalBytes","()J",fs,NULL,0,&out)||(uint64_t)out.j!=(uint64_t)expected_disk.f_frsize*expected_disk.f_blocks)return false;
 file_probe_log(stderr,"STATFS_SELFTEST PASS real_block_size=1 real_block_count=1 real_total_bytes=1");
 char canonical_cache[PATH_MAX],canonical_missing[PATH_MAX];
 if(!realpath(cache_root,canonical_cache))return false;
 if(!file_runtime_call(v,"java/io/File","getCanonicalPath","()Ljava/lang/String;",cache,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),canonical_cache))return false;
 if(snprintf(canonical_missing,sizeof canonical_missing,"%s/canonical-missing-%ld/child",cache_root,(long)getpid())>=(int)sizeof canonical_missing)return false;
 struct stat canonical_st;if(!lstat(canonical_missing,&canonical_st)||errno!=ENOENT)return false;
 dvm_ref missing_canonical=make(v,canonical_missing);
 if(!missing_canonical||!file_runtime_call(v,"java/io/File","getCanonicalPath","()Ljava/lang/String;",missing_canonical,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),canonical_missing))return false;
 if(!lstat(canonical_missing,&canonical_st)||errno!=ENOENT)return false;
 file_probe_log(stderr,"FILE_CANONICAL_SELFTEST PASS existing=1 missing_descendant=1 no_create=1");
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",cache,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),cache_root))return false;
 if(!file_runtime_call(v,"java/io/File","isDirectory","()Z",cache,NULL,0,&out)||!out.i)return false;
 if(!file_runtime_call(v,"java/io/File","getParentFile","()Ljava/io/File;",dir,NULL,0,&out)||!out.l)return false;
 dvm_ref parent=out.l;
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",parent,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),private_base))return false;
 if(!file_runtime_call(v,"java/io/File","isDirectory","()Z",parent,NULL,0,&out)||!out.i)return false;
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",dir,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),root))return false;
 if(!file_runtime_call(v,"java/io/File","isDirectory","()Z",dir,NULL,0,&out)||!out.i)return false;
 /* Context.getDir(name, MODE_PRIVATE): real <root>/app_<name>, 0700, reused. */
 dvm_ref app_name=dvm_new_string(v,"selftest-getdir");if(!app_name)return false;
 char app_path[PATH_MAX];if(snprintf(app_path,sizeof app_path,"%s/app_selftest-getdir",root)>=(int)sizeof app_path)return false;
 rmdir(app_path);
 a[0].l=app_name;a[1].i=0;
 if(!file_runtime_call(v,"android/content/Context","getDir","(Ljava/lang/String;I)Ljava/io/File;",ctx,a,2,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref app_dir=out.l;
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",app_dir,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),app_path))return false;
 if(!file_runtime_call(v,"java/io/File","exists","()Z",app_dir,NULL,0,&out)||!out.i)return false;
 if(!file_runtime_call(v,"java/io/File","isDirectory","()Z",app_dir,NULL,0,&out)||!out.i)return false;
 struct stat app_st;if(lstat(app_path,&app_st)||S_ISLNK(app_st.st_mode)||!S_ISDIR(app_st.st_mode)||(app_st.st_mode&0777)!=0700)return false;
 if(!file_runtime_call(v,"android/content/Context","getDir","(Ljava/lang/String;I)Ljava/io/File;",ctx,a,2,&out)||!out.l||out.l==app_dir)return false;
 dvm_ref app_again=out.l;
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",app_again,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),app_path))return false;
 if(!file_runtime_call(v,"java/io/File","isDirectory","()Z",app_again,NULL,0,&out)||!out.i)return false;
 dvm_ref bad_nul=dvm_new_string_n(v,"bad\0name",8),slash_name=dvm_new_string(v,"a/b"),dotdot=dvm_new_string(v,".."),dot=dvm_new_string(v,"."),blank=dvm_new_string(v,"");
 unsigned char bad_byte=0xff;dvm_ref bad_utf8=dvm_new_string_n(v,(const char*)&bad_byte,1);
 bool getdir_negative=bad_nul&&slash_name&&dotdot&&dot&&blank&&bad_utf8&&
  dir_rejected(v,ctx,0,0,"java/lang/NullPointerException")&&
  dir_rejected(v,ctx,slash_name,0,"java/lang/IllegalArgumentException")&&
  dir_rejected(v,ctx,dotdot,0,"java/lang/IllegalArgumentException")&&
  dir_rejected(v,ctx,dot,0,"java/lang/IllegalArgumentException")&&
  dir_rejected(v,ctx,blank,0,"java/lang/IllegalArgumentException")&&
  dir_rejected(v,ctx,bad_nul,0,"java/lang/IllegalArgumentException")&&
  dir_rejected(v,ctx,bad_utf8,0,"java/lang/IllegalArgumentException")&&
  dir_rejected(v,ctx,app_name,1,"java/lang/UnsupportedOperationException")&&
  dir_not_taken(v,dir,app_name,0);
 rmdir(app_path);
 if(!getdir_negative)return false;
 /* Context.getDatabasePath(name): real <root>/databases/<name>.  Android
  * ensures the databases directory but does not create the database file. */
 dvm_ref db_name=dvm_new_string(v,"selftest-getdatabasepath.db");
 dvm_ref db_escape=dvm_new_string(v,"../escape.db");
 dvm_ref db_absolute=dvm_new_string(v,"/absolute.db");
 dvm_ref db_nested=dvm_new_string(v,"sub/dir.db");
 dvm_ref db_blank=dvm_new_string(v,"");
 unsigned char db_bad_byte=0xff;dvm_ref db_bad_utf8=dvm_new_string_n(v,(const char*)&db_bad_byte,1);
 char db_dir[PATH_MAX],db_path[PATH_MAX];
 if(!(db_name&&db_escape&&db_absolute&&db_nested&&db_blank&&db_bad_utf8&&
      snprintf(db_dir,sizeof db_dir,"%s/databases",root)<(int)sizeof db_dir&&
      snprintf(db_path,sizeof db_path,"%s/selftest-getdatabasepath.db",db_dir)<(int)sizeof db_path))return false;
 unlink(db_path); /* test-only path; getDatabasePath must not create it */
 a[0].l=db_name;
 if(!file_runtime_call(v,"android/content/Context","getDatabasePath","(Ljava/lang/String;)Ljava/io/File;",ctx,a,1,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref db_file=out.l;
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",db_file,NULL,0,&out)||!out.l||!dvm_string_utf8(v,out.l)||strcmp(dvm_string_utf8(v,out.l),db_path))return false;
 if(!file_runtime_call(v,"java/io/File","exists","()Z",db_file,NULL,0,&out)||out.i)return false;
 struct stat db_st;if(lstat(db_dir,&db_st)||S_ISLNK(db_st.st_mode)||!S_ISDIR(db_st.st_mode))return false;
 if(!lstat(db_path,&db_st))return false;
 bool database_negative=
  database_rejected(v,ctx,0,"java/lang/NullPointerException")&&
  database_rejected(v,ctx,db_escape,"java/lang/IllegalArgumentException")&&
  database_rejected(v,ctx,db_absolute,"java/lang/IllegalArgumentException")&&
  database_rejected(v,ctx,db_nested,"java/lang/IllegalArgumentException")&&
  database_rejected(v,ctx,db_blank,"java/lang/IllegalArgumentException")&&
  database_rejected(v,ctx,db_bad_utf8,"java/lang/IllegalArgumentException")&&
  database_not_taken(v,dir,db_name);
 if(!database_negative)return false;
 dvm_ref child=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/File"));a[0].l=dir;a[1].l=dvm_new_string(v,"missing-file");
 if(!child||!file_runtime_call(v,"java/io/File","<init>","(Ljava/io/File;Ljava/lang/String;)V",child,a,2,&out))return false;
 if(!file_runtime_call(v,"java/io/File","exists","()Z",child,NULL,0,&out)||out.i)return false;
 if(file_runtime_call(v,"java/io/File","unknown","()V",dir,NULL,0,&out))return false;
 char temporary[PATH_MAX];if(snprintf(temporary,sizeof temporary,"%s/file-check-XXXXXX",root)>=(int)sizeof temporary||!mkdtemp(temporary))return false;
 char nested[PATH_MAX];if(snprintf(nested,sizeof nested,"%s/a/b",temporary)>=(int)sizeof nested){rmdir(temporary);return false;}
 dvm_ref probe=make(v,nested);bool made=probe&&file_runtime_call(v,"java/io/File","mkdirs","()Z",probe,NULL,0,&out)&&out.i;
 bool existing=made&&file_runtime_call(v,"java/io/File","mkdirs","()Z",probe,NULL,0,&out)&&!out.i;
 bool queried=existing&&file_runtime_call(v,"java/io/File","isDirectory","()Z",probe,NULL,0,&out)&&out.i;
 char file[PATH_MAX];bool created=false;if(snprintf(file,sizeof file,"%s/empty",nested)<(int)sizeof file){
  dvm_ref f=make(v,file);created=queried&&f&&file_runtime_call(v,"java/io/File","createNewFile","()Z",f,NULL,0,&out)&&out.i;
  created=created&&file_runtime_call(v,"java/io/File","createNewFile","()Z",f,NULL,0,&out)&&!out.i;
  struct stat st;created=created&&!lstat(file,&st)&&S_ISREG(st.st_mode)&&st.st_size==0;
  unlink(file);
 }
 rmdir(nested);char *last=strrchr(nested,'/');if(last){*last=0;rmdir(nested);}rmdir(temporary);
 if(!queried||!created)return false;
 /* java/io/File 静态字段必须走真实的 external-static 分派
  * (java_basics_static)，返回真实值而不是调用方常量。 */
 union dvm_value s={0};
 if(!java_basics_static(v,"java/io/File","separatorChar","C",&s)||s.i!='/')return false;
 if(!java_basics_static(v,"java/io/File","separator","Ljava/lang/String;",&s)||!s.l)return false;
 const char *sep=dvm_string_utf8(v,s.l);if(!sep||strcmp(sep,"/"))return false;
 if(!java_basics_static(v,"java/io/File","pathSeparatorChar","C",&s)||s.i!=':')return false;
 if(!java_basics_static(v,"java/io/File","pathSeparator","Ljava/lang/String;",&s)||!s.l)return false;
 const char *path_sep=dvm_string_utf8(v,s.l);if(!path_sep||strcmp(path_sep,":"))return false;
 if(java_basics_static(v,"java/io/File","missingStatic","C",&s))return false;
 if(dvm_exception(v))return false;
 file_probe_log(stderr,"FILE_RUNTIME_SELFTEST PASS recursive_mkdir=1 existing_false=1 exclusive_create=1 zero_size=1 get_dir=1 get_dir_reuse=1 get_dir_rejected=1 get_dir_receiver=1 get_database_path=1 get_database_path_rejected=1 get_database_receiver=1 file_statics=4\n");return !dvm_exception(v);
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int file_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;(void)visit;(void)user;return o->data_destroy==release?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
bool file_freeze180_selftest(struct dvm *v){
 union dvm_value out={0};bool ok=true;char path[PATH_MAX],expected[PATH_MAX];
 snprintf(path,sizeof path,"%s/./not-created/../freeze180-absent",root);
 dvm_ref f=make(v,path);if(f)dvm_pin(v,f);else ok=false;
 struct dvm_class *c=dvm_find_class_or_external(v,"java/io/File");
 ok=ok&&dvm_find_method(v,c,"getCanonicalPath","()Ljava/lang/String;")!=NULL&&dvm_find_method(v,c,"getCanonicalFile","()Ljava/io/File;")!=NULL;
 snprintf(expected,sizeof expected,"%s/freeze180-absent",root);
 ok=ok&&file_runtime_call(v,"java/io/File","getCanonicalPath","()Ljava/lang/String;",f,NULL,0,&out)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),expected);
 ok=ok&&file_runtime_call(v,"java/io/File","getCanonicalFile","()Ljava/io/File;",f,NULL,0,&out)&&out.l;
 if(ok){dvm_ref canonical=out.l;dvm_pin(v,canonical);ok=file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",canonical,NULL,0,&out)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),expected);dvm_unpin(v,canonical);}
 char relative[PATH_MAX];ok=ok&&file_absolute("freeze180-absent",relative)&&relative[0]=='/'&&file_canonical(path,relative)&&!strcmp(relative,expected);
 /* Mutating tests touch only an exclusive temporary fixture under cache. */
 union dvm_value args[3]={{.l=dvm_new_string(v,"freeze180-")},{.l=dvm_new_string(v,".tmp")},{.l=0}};
 dvm_ref fixture=0;bool extras=ok&&file_runtime_call(v,"java/io/File","createTempFile","(Ljava/lang/String;Ljava/lang/String;Ljava/io/File;)Ljava/io/File;",0,args,3,&out)&&out.l&&!dvm_exception(v);
 if(extras){fixture=out.l;dvm_pin(v,fixture);
  extras=file_runtime_call(v,"java/io/File","exists","()Z",fixture,NULL,0,&out)&&out.i;
  union dvm_value stamp={.j=1700000000123ll};extras=extras&&file_runtime_call(v,"java/io/File","setLastModified","(J)Z",fixture,&stamp,1,&out)&&out.i;
  extras=extras&&file_runtime_call(v,"java/io/File","lastModified","()J",fixture,NULL,0,&out)&&out.j==stamp.j;
  extras=extras&&file_runtime_call(v,"java/io/File","toURI","()Ljava/net/URI;",fixture,NULL,0,&out)&&out.l&&!dvm_exception(v);
  extras=extras&&file_runtime_call(v,"java/io/File","hashCode","()I",fixture,NULL,0,&out)&&!dvm_exception(v);
  struct dvm_object *fo=dvm__obj(v,fixture);char rename_path[PATH_MAX];snprintf(rename_path,sizeof rename_path,"%s.renamed",(char*)fo->data);
  dvm_ref dest=make(v,rename_path);if(dest)dvm_pin(v,dest);else extras=false;
  union dvm_value arg={.l=dest};extras=extras&&file_runtime_call(v,"java/io/File","renameTo","(Ljava/io/File;)Z",fixture,&arg,1,&out)&&out.i;
  extras=extras&&file_runtime_call(v,"java/io/File","setReadOnly","()Z",dest,NULL,0,&out)&&out.i;
  extras=extras&&file_runtime_call(v,"java/io/File","delete","()Z",dest,NULL,0,&out)&&out.i;
  /* Always clean up the fixture even if an assertion failed. */
  unlink(fo->data);unlink(rename_path);if(dest)dvm_unpin(v,dest);dvm_unpin(v,fixture);
 }
 ok=ok&&extras;
 if(f)dvm_unpin(v,f);
 FILE *log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/freeze180-file-selftest.txt","w");
 if(log){fprintf(log,"pid=%d declarations=%d canonical_dot_missing=%d canonical_file=%d relative_absolute=%d temp_timestamp_uri_hash_rename_readonly_delete=%d result=%s HAP=1 moving=0\n",getpid(),c?c->nmethods:0,ok,ok,ok,extras,ok?"PASS":"FAIL");fclose(log);}else ok=false;
 return ok&&!dvm_exception(v);
}
