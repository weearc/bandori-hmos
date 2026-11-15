#include "file_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
static char root[PATH_MAX], private_base[PATH_MAX]; static struct dvm *active;
int sdk_trace_fprintf(FILE *,const char *,...);
extern dvm_ref android_basics_application(struct dvm *);
static void release(void *p){free(p);}
static bool err(struct dvm *v,const char *c){dvm__throw(v,c,"file runtime");return true;}
static bool allowed(const char *p){size_t n=strlen(private_base);return n&&!strncmp(p,private_base,n)&&(p[n]==0||p[n]=='/');}
static bool safe(const char *p){if(!p||strstr(p,"//"))return false;const char *s=p;while(*s){while(*s=='/')s++;const char *e=strchr(s,'/');size_t n=e?(size_t)(e-s):strlen(s);if((n==1&&s[0]=='.')||(n==2&&s[0]=='.'&&s[1]=='.'))return false;if(!e)break;s=e+1;}return true;}
bool file_runtime_configure(const char *sdk){char path[PATH_MAX];if(!sdk||!realpath(sdk,path))return false;char *s=strrchr(path,'/');if(!s)return false;*s=0;if(snprintf(root,sizeof root,"%s/guest-files",path)>=(int)sizeof root)return false;if(mkdir(root,0700)&&access(root,F_OK))return false;struct stat st;if(lstat(root,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return false;return true;}
void file_runtime_vm_start(struct dvm *v){active=v;snprintf(private_base,sizeof private_base,"%s",root);char *last=strrchr(private_base,'/');if(last)*last=0;}
#define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))
static dvm_ref make(struct dvm *v,const char *p){dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/File"));struct dvm_object *o=dvm__obj(v,r);if(!o)return 0;o->data=strdup(p);o->data_destroy=release;return o->data?r:0;}
bool file_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){if(v!=active||!root[0])return false;
 if(IS("getFilesDir","()Ljava/io/File;")){struct dvm_object *o=dvm__obj(v,self);bool ok=false;for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)if(!strcmp(k->name,"android/content/Context"))ok=true;if(!ok)return false;out->l=make(v,root);return out->l?true:err(v,"java/lang/OutOfMemoryError");}
 if(strcmp(c,"java/io/File"))return false;struct dvm_object *o=dvm__obj(v,self);if(!o)return err(v,"java/lang/NullPointerException");
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
  char parent[PATH_MAX];snprintf(parent,sizeof parent,"%s",(char*)o->data);
  char *slash=strrchr(parent,'/');if(!slash){out->l=0;return true;}*slash=0;
  if(!allowed(parent))return err(v,"java/lang/UnsupportedOperationException");
  out->l=IS("getParentFile","()Ljava/io/File;")?make(v,parent):dvm_new_string(v,parent);
  return out->l?true:err(v,"java/lang/OutOfMemoryError");
 }
 if(!strcmp(m,"<init>")){const char *p=NULL;char buf[PATH_MAX];if(IS("<init>","(Ljava/lang/String;)V")&&n==1){p=dvm_string_utf8(v,a[0].l);if(p&&strlen(p)!=dvm_string_utf8_length(v,a[0].l))return err(v,"java/lang/UnsupportedOperationException");}else if(IS("<init>","(Ljava/io/File;Ljava/lang/String;)V")&&n==2){struct dvm_object *parent=dvm__obj(v,a[0].l);const char *child=dvm_string_utf8(v,a[1].l);if(child&&strlen(child)!=dvm_string_utf8_length(v,a[1].l))return err(v,"java/lang/UnsupportedOperationException");if(!parent||parent->data_destroy!=release||!child)return err(v,"java/lang/NullPointerException");if(snprintf(buf,sizeof buf,"%s/%s",(char*)parent->data,child)>=(int)sizeof buf)return err(v,"java/lang/UnsupportedOperationException");p=buf;}else return false;if(!p)return err(v,"java/lang/NullPointerException");if(!safe(p)||!allowed(p)||o->data)return err(v,"java/lang/UnsupportedOperationException");o->data=strdup(p);o->data_destroy=release;return o->data?true:err(v,"java/lang/OutOfMemoryError");}
 if(o->data_destroy!=release)return false;const char *p=o->data;if(IS("getPath","()Ljava/lang/String;")||IS("getAbsolutePath","()Ljava/lang/String;")){out->l=dvm_new_string(v,p);return true;}struct stat st;char resolved[PATH_MAX];int ok=realpath(p,resolved)!=NULL&&allowed(resolved)&&lstat(p,&st)==0&&!S_ISLNK(st.st_mode);if(IS("exists","()Z")){out->i=ok;return true;}if(IS("isDirectory","()Z")){out->i=ok&&S_ISDIR(st.st_mode);return true;}if(IS("isFile","()Z")){out->i=ok&&S_ISREG(st.st_mode);return true;}if(IS("length","()J")){out->j=ok&&S_ISREG(st.st_mode)?st.st_size:0;return true;}return false;}
bool file_runtime_selftest(struct dvm *v){
 union dvm_value out={0},a[2]={{0},{0}}; dvm_ref ctx=android_basics_application(v);
 if(!file_runtime_call(v,"android/content/Context","getFilesDir","()Ljava/io/File;",ctx,NULL,0,&out)||!out.l)return false;
 dvm_ref dir=out.l;
 if(!file_runtime_call(v,"java/io/File","getParentFile","()Ljava/io/File;",dir,NULL,0,&out)||!out.l)return false;
 dvm_ref parent=out.l;
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",parent,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),private_base))return false;
 if(!file_runtime_call(v,"java/io/File","isDirectory","()Z",parent,NULL,0,&out)||!out.i)return false;
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",dir,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),root))return false;
 if(!file_runtime_call(v,"java/io/File","isDirectory","()Z",dir,NULL,0,&out)||!out.i)return false;
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
 sdk_trace_fprintf(stderr,"FILE_RUNTIME_SELFTEST PASS recursive_mkdir=1 existing_false=1 exclusive_create=1 zero_size=1\n");return !dvm_exception(v);
}
