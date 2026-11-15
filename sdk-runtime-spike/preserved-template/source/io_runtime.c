#include "io_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))

typedef struct {char *buf;size_t len,cap;} sw_data;
typedef struct {dvm_ref writer;} pw_data;
typedef struct {unsigned char *buf;size_t len,cap;} baos_data;
static void baos_free(void *p){baos_data*b=p;if(b){free(b->buf);free(b);}}
static baos_data *baos_of(struct dvm *v,dvm_ref ref){struct dvm_object*o=dvm__obj(v,ref);return o&&o->data_destroy==baos_free?o->data:NULL;}
static bool baos_append(struct dvm *v,baos_data*b,const unsigned char*data,size_t n){
 if(n>SIZE_MAX-b->len){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return false;}
 size_t need=b->len+n;
 if(need>b->cap){
  size_t cap=b->cap?b->cap:64;
  while(cap<need){if(cap>SIZE_MAX/2){cap=need;break;}cap*=2;}
  unsigned char *grown=realloc(b->buf,cap);
  if(!grown){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return false;}
  b->buf=grown;b->cap=cap;
 }
 if(n)memcpy(b->buf+b->len,data,n);
 b->len=need;return true;
}
static struct dvm *active;
static void sw_free(void *p){sw_data*s=p;if(s){free(s->buf);free(s);}}
static void pw_free(void *p){free(p);}
/* ObjectInputStream's constructor in AOSP only stores the underlying stream;
 * it performs no reads.  This keeps that contract honestly: the guest stream
 * reference is retained, and any later readObject() remains unimplemented and
 * fails closed instead of fabricating a deserialized object. */
typedef struct {dvm_ref in;} ois_data;
static void ois_free(void *p){free(p);}

/* ---------------- java/io/FileReader and java/io/BufferedReader ----------
 * FileReader owns a POSIX descriptor opened read-only and decodes UTF-8 into
 * the UTF-16 code units java.io.Reader exposes.  BufferedReader is a thin
 * line/character layer over a FileReader; readers over AssetManager streams
 * keep going to asset_reader.c, which owns the external-call chain entry for
 * java/io/BufferedReader, so io_runtime_vm_start installs these methods on the
 * class itself.  A missing peer runtime fails closed instead of succeeding. */
typedef struct {
 int fd;                     /* -1 once closed */
 int closed;
 int eof;
 int pending_char;           /* low surrogate of a supplementary code point */
 unsigned char buf[4096];
 size_t pos, len;
} fr_data;
typedef struct {
 struct dvm *vm;             /* for dvm_unpin() in the destructor */
 dvm_ref reader;             /* pinned java/io/FileReader */
 int closed;
 int pushback;               /* one UTF-16 unit read past a line terminator */
} br_data;

/* These peers are part of the same SDK image.  Weak declarations keep this
 * translation unit loadable on its own; when absent the dependent path is
 * refused with a real exception rather than a silent success. */
extern dvm_ref android_basics_application(struct dvm *) __attribute__((weak));
extern bool file_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *) __attribute__((weak));
extern bool asset_reader_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *) __attribute__((weak));

static void fr_free(void *p){fr_data*f=p;if(f){if(f->fd>=0)close(f->fd);free(f);}}
static void br_free(void *p){br_data*b=p;if(b){if(b->reader)dvm_unpin(b->vm,b->reader);free(b);}}
static fr_data *fr_of(struct dvm *v,dvm_ref ref){struct dvm_object*o=dvm__obj(v,ref);return o&&o->data_destroy==fr_free?o->data:NULL;}
static br_data *br_of(struct dvm *v,dvm_ref ref){struct dvm_object*o=dvm__obj(v,ref);return o&&o->data_destroy==br_free?o->data:NULL;}

/* The sandbox base is the directory containing file_runtime's guest-files/
 * guest-cache roots.  FileReader may read beneath it; read-only system trees
 * are separately whitelisted.  Everything else is refused. */
static char io_sandbox_base[PATH_MAX];
static int io_sandbox_state;   /* 0 untried, 1 available, 2 unavailable */
/* Exact file the selftest created with mkstemp when file_runtime is not
 * configured (host tests).  Production paths never consult this. */
static char io_selftest_allow[PATH_MAX];
static void io_discover_sandbox(struct dvm *v){
 io_sandbox_state=2;io_sandbox_base[0]=0;
 if(!android_basics_application||!file_runtime_call)return;
 dvm_ref app=android_basics_application(v);
 if(!app)return;
 union dvm_value file={0},path={0};
 if(!file_runtime_call(v,"android/content/Context","getFilesDir","()Ljava/io/File;",app,NULL,0,&file)||!file.l)goto done;
 if(!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",file.l,NULL,0,&path)||!path.l)goto done;
 const char *text=dvm_string_utf8(v,path.l);
 if(!text||!*text||strlen(text)!=dvm_string_utf8_length(v,path.l))goto done;
 if(snprintf(io_sandbox_base,sizeof io_sandbox_base,"%s",text)>=(int)sizeof io_sandbox_base){io_sandbox_base[0]=0;goto done;}
 char *slash=strrchr(io_sandbox_base,'/');
 if(!slash||slash==io_sandbox_base){io_sandbox_base[0]=0;goto done;}
 *slash=0;io_sandbox_state=1;
done:
 if(dvm_exception(v))dvm_clear_exception(v);
}
static bool io_readonly_path(const char *p){
 static const char *const roots[]={"/proc/","/sys/","/system/","/vendor/","/product/","/odm/","/etc/"};
 if(!p||p[0]!='/')return false;
 for(size_t i=0;i<sizeof roots/sizeof roots[0];++i)
  if(!strncmp(p,roots[i],strlen(roots[i])))return true;
 if(io_sandbox_state==1&&io_sandbox_base[0]){
  size_t n=strlen(io_sandbox_base);
  if(!strncmp(p,io_sandbox_base,n)&&(p[n]==0||p[n]=='/'))return true;
 }
 if(io_selftest_allow[0]&&!strcmp(p,io_selftest_allow))return true;
 return false;
}
/* Writable paths are deliberately much narrower than readable ones: only the
 * sandbox base (the guest files/cache root) and the exact host-selftest file
 * are accepted.  The read-only system trees are never writable. */
static bool io_writable_path(const char *p){
 if(!p||p[0]!='/')return false;
 if(io_sandbox_state==1&&io_sandbox_base[0]){
  size_t n=strlen(io_sandbox_base);
  if(!strncmp(p,io_sandbox_base,n)&&(p[n]==0||p[n]=='/'))return true;
 }
 if(io_selftest_allow[0]&&!strcmp(p,io_selftest_allow))return true;
 return false;
}

/* Read one byte from the descriptor's buffered window.  Returns false with an
 * exception pending on a real I/O error; *out is -1 at EOF. */
static bool fr_read_byte(struct dvm *v,fr_data *f,int *out){
 if(f->closed){dvm__throw(v,"java/io/IOException","Stream closed");return false;}
 for(;;){
  if(f->pos<f->len){*out=f->buf[f->pos++];return true;}
  if(f->eof){*out=-1;return true;}
  ssize_t got=read(f->fd,f->buf,sizeof f->buf);
  if(got<0){
   if(errno==EINTR)continue;
   dvm__throw(v,"java/io/IOException","read failed");
   return false;
  }
  if(got==0){f->eof=1;f->pos=f->len=0;*out=-1;return true;}
  f->pos=0;f->len=(size_t)got;
 }
}
/* Decode one UTF-8 code point into one or two UTF-16 code units.  Malformed
 * or overlong sequences and lone surrogates are IOException, never silently
 * replaced or truncated. */
static bool fr_read_unit(struct dvm *v,fr_data *f,int *out){
 if(f->closed){dvm__throw(v,"java/io/IOException","Stream closed");return false;}
 if(f->pending_char>=0){*out=f->pending_char;f->pending_char=-1;return true;}
 int b=0;
 if(!fr_read_byte(v,f,&b))return false;
 if(b<0){*out=-1;return true;}
 if(b<0x80){*out=b;return true;}
 int ncont;uint32_t cp;
 if(b>=0xC2&&b<=0xDF){ncont=1;cp=(uint32_t)(b&0x1F);}
 else if(b>=0xE0&&b<=0xEF){ncont=2;cp=(uint32_t)(b&0x0F);}
 else if(b>=0xF0&&b<=0xF4){ncont=3;cp=(uint32_t)(b&0x07);}
 else{dvm__throw(v,"java/io/IOException","malformed UTF-8");return false;}
 for(int i=0;i<ncont;++i){
  int c=0;
  if(!fr_read_byte(v,f,&c))return false;
  if(c<0){dvm__throw(v,"java/io/IOException","truncated UTF-8 sequence");return false;}
  if((c&0xC0)!=0x80){dvm__throw(v,"java/io/IOException","malformed UTF-8 continuation");return false;}
  cp=(cp<<6)|(uint32_t)(c&0x3F);
 }
 if((ncont==2&&cp<0x800)||(ncont==3&&cp<0x10000)||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF)){
  dvm__throw(v,"java/io/IOException","malformed UTF-8 code point");return false;
 }
 if(cp<=0xFFFF){*out=(int)cp;return true;}
 cp-=0x10000;
 *out=(int)(0xD800+(cp>>10));
 f->pending_char=(int)(0xDC00+(cp&0x3FF));
 return true;
}
static bool fr_ready(struct dvm *v,fr_data *f,int *out){
 if(f->closed){dvm__throw(v,"java/io/IOException","Stream closed");return false;}
 if(f->pending_char>=0||f->pos<f->len){*out=1;return true;}
 if(f->eof){*out=0;return true;}
 int b=0;
 if(!fr_read_byte(v,f,&b))return false;
 if(b<0){*out=0;return true;}
 /* Put the byte back; fr_read_byte() only advances the in-memory window. */
 f->buf[--f->pos]=(unsigned char)b;
 *out=1;return true;
}
static bool fr_close_data(struct dvm *v,fr_data *f){
 if(f->closed)return true;
 f->closed=1;
 if(f->fd>=0){
  int fd=f->fd;f->fd=-1;
  if(close(fd)){dvm__throw(v,"java/io/IOException","close failed");return false;}
 }
 return true;
}
static bool fr_skip(struct dvm *v,fr_data *f,long long want,union dvm_value *out){
 /* Reader.skip: a negative count is IllegalArgumentException before the
  * closed check; zero is a successful no-op.  Do not fold them together. */
 if(want<0){dvm__throw(v,"java/lang/IllegalArgumentException","skip value is negative");return true;}
 if(f->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
 if(want==0){out->j=0;return true;}
 long long skipped=0;
 while(skipped<want){
  int unit=0;
  if(!fr_read_unit(v,f,&unit))return true;
  if(unit<0)break;
  ++skipped;
 }
 out->j=skipped;return true;
}
static struct dvm_object *fr_char_array(struct dvm *v,dvm_ref array,int off,int len,bool *ok){
 *ok=false;
 struct dvm_object *o=dvm__obj(v,array);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","char array is null");return NULL;}
 if(o->kind!=DVM_OBJ_ARRAY||o->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","not a char array");return NULL;}
 if(off<0||len<0||(uint32_t)off>o->length||(uint32_t)len>o->length-(uint32_t)off){
  dvm__throw(v,"java/lang/IndexOutOfBoundsException","off=%d len=%d length=%u",off,len,o->length);return NULL;
 }
 *ok=true;return o;
}
static bool fr_read_chars(struct dvm *v,fr_data *f,dvm_ref array,int off,int len,union dvm_value *out){
 if(f->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
 bool ok=false;
 struct dvm_object *o=fr_char_array(v,array,off,len,&ok);
 if(!ok)return true;
 if(!len){out->i=0;return true;}
 uint16_t *dst=o->data;int count=0;
 while(count<len){
  int unit=0;
  if(!fr_read_unit(v,f,&unit))return true;
  if(unit<0){if(!count){out->i=-1;return true;}break;}
  dst[off+count]=(uint16_t)unit;
  ++count;
 }
 out->i=count;return true;
}
/* Open follows java.io.FileInputStream: any refusal or open failure becomes
 * FileNotFoundException.  The resolved path must be a regular file beneath the
 * sandbox base or a whitelisted read-only system tree. */
static bool fr_construct(struct dvm *v,dvm_ref self,dvm_ref path_ref,union dvm_value *out){
 (void)out;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","FileReader receiver");return true;}
 if(o->data){dvm__throw(v,"java/lang/IllegalStateException","FileReader already initialized");return true;}
 if(!path_ref){dvm__throw(v,"java/lang/NullPointerException","path is null");return true;}
 const char *path=dvm_string_utf8(v,path_ref);
 if(!path){dvm__throw(v,"java/lang/IllegalArgumentException","path is not a String");return true;}
 if(strlen(path)!=dvm_string_utf8_length(v,path_ref)){dvm__throw(v,"java/lang/IllegalArgumentException","path contains NUL");return true;}
 if(!*path){dvm__throw(v,"java/lang/IllegalArgumentException","empty path");return true;}
 if(path[0]!='/'){dvm__throw(v,"java/io/FileNotFoundException","relative path is not allowed: %s",path);return true;}
 if(io_sandbox_state==0)io_discover_sandbox(v);
 char resolved[PATH_MAX];
 if(!realpath(path,resolved)){dvm__throw(v,"java/io/FileNotFoundException","%s",path);return true;}
 if(!io_readonly_path(resolved)){dvm__throw(v,"java/io/FileNotFoundException","path is outside the sandbox: %s",path);return true;}
 struct stat st;
 if(lstat(resolved,&st)||!S_ISREG(st.st_mode)){dvm__throw(v,"java/io/FileNotFoundException","not a readable file: %s",path);return true;}
 int fd=open(resolved,O_RDONLY|O_CLOEXEC);
 if(fd<0){dvm__throw(v,"java/io/FileNotFoundException","%s",path);return true;}
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);dvm__throw(v,"java/io/FileNotFoundException","not a readable file: %s",path);return true;}
 fr_data *f=calloc(1,sizeof *f);
 if(!f){close(fd);dvm__throw(v,"java/lang/OutOfMemoryError","FileReader");return true;}
 f->fd=fd;f->pending_char=-1;
 o->data=f;o->data_destroy=fr_free;
 return true;
}

/* ---------------- java/io/FileOutputStream ----------------
 * FileOutputStream(File) and FileOutputStream(File,boolean) own a real POSIX
 * descriptor.  append=false means O_WRONLY|O_CREAT|O_TRUNC, append=true means
 * O_WRONLY|O_CREAT|O_APPEND and never truncates, exactly as Android's
 * FileOutputStream.  The resolved target must be a regular file beneath the
 * writable sandbox; a missing leaf is created only when its real parent is
 * allowed.  O_NOFOLLOW keeps a final symlink from escaping the sandbox. */
typedef struct {int fd;int closed;} fos_data;
static void fos_free(void *p){fos_data*f=p;if(f){if(f->fd>=0)close(f->fd);free(f);}}
static fos_data *fos_of(struct dvm *v,dvm_ref ref){struct dvm_object*o=dvm__obj(v,ref);return o&&o->data_destroy==fos_free?o->data:NULL;}
static bool fos_is_class(struct dvm_class *c){
 for(;c;c=c->super)if(c->name&&!strcmp(c->name,"java/io/FileOutputStream"))return true;
 return false;
}
static bool fos_construct(struct dvm *v,dvm_ref self,dvm_ref path_ref,bool append,union dvm_value *out){
 (void)out;
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","FileOutputStream receiver");return true;}
 if(o->data){dvm__throw(v,"java/lang/IllegalStateException","FileOutputStream already initialized");return true;}
 if(!path_ref){dvm__throw(v,"java/lang/NullPointerException","path is null");return true;}
 const char *path=dvm_string_utf8(v,path_ref);
 if(!path){dvm__throw(v,"java/lang/IllegalArgumentException","path is not a String");return true;}
 if(strlen(path)!=dvm_string_utf8_length(v,path_ref)){dvm__throw(v,"java/lang/IllegalArgumentException","path contains NUL");return true;}
 if(!*path){dvm__throw(v,"java/lang/IllegalArgumentException","empty path");return true;}
 if(path[0]!='/'){dvm__throw(v,"java/io/FileNotFoundException","relative path is not allowed: %s",path);return true;}
 if(io_sandbox_state==0)io_discover_sandbox(v);
 char resolved[PATH_MAX];
 struct stat st;
 if(!realpath(path,resolved)){
  /* The leaf may not exist yet.  Resolve its parent and require the parent
   * itself to be writable; the leaf is then opened with O_NOFOLLOW. */
  const char *slash=strrchr(path,'/');
  if(!slash||slash==path){dvm__throw(v,"java/io/FileNotFoundException","%s",path);return true;}
  size_t parent_len=(size_t)(slash-path);
  if(parent_len>=sizeof resolved){dvm__throw(v,"java/io/FileNotFoundException","%s",path);return true;}
  char parent[PATH_MAX];
  memcpy(parent,path,parent_len);parent[parent_len]=0;
  if(!realpath(parent,resolved)){dvm__throw(v,"java/io/FileNotFoundException","%s",path);return true;}
  if(!io_writable_path(resolved)){dvm__throw(v,"java/io/FileNotFoundException","path is outside the sandbox: %s",path);return true;}
  size_t base_len=strlen(resolved);
  if(base_len+strlen(slash)>=sizeof resolved){dvm__throw(v,"java/io/FileNotFoundException","path is too long: %s",path);return true;}
  memcpy(resolved+base_len,slash,strlen(slash)+1);
 }else{
  if(!io_writable_path(resolved)){dvm__throw(v,"java/io/FileNotFoundException","path is outside the sandbox: %s",path);return true;}
 }
 if(lstat(resolved,&st)==0&&!S_ISREG(st.st_mode)){dvm__throw(v,"java/io/FileNotFoundException","not a regular file: %s",path);return true;}
 int flags=O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW|(append?O_APPEND:O_TRUNC);
 int fd=open(resolved,flags,0600);
 if(fd<0){dvm__throw(v,"java/io/FileNotFoundException","%s",path);return true;}
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);dvm__throw(v,"java/io/FileNotFoundException","not a regular file: %s",path);return true;}
 fos_data *f=calloc(1,sizeof *f);
 if(!f){close(fd);dvm__throw(v,"java/lang/OutOfMemoryError","FileOutputStream");return true;}
 f->fd=fd;o->data=f;o->data_destroy=fos_free;
 return true;
}
static bool fos_write_bytes(struct dvm *v,fos_data *f,const unsigned char *data,size_t len){
 if(f->closed||f->fd<0){dvm__throw(v,"java/io/IOException","Stream closed");return false;}
 size_t off=0;
 while(off<len){
  ssize_t n=write(f->fd,data+off,len-off);
  if(n<0){
   if(errno==EINTR)continue;
   dvm__throw(v,"java/io/IOException","FileOutputStream write failed");return false;
  }
  if(n==0){dvm__throw(v,"java/io/IOException","FileOutputStream write failed");return false;}
  off+=(size_t)n;
 }
 return true;
}
static bool fos_dispatch(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 fos_data *f=fos_of(v,self);
 if(!f){dvm__throw(v,"java/lang/IllegalStateException","FileOutputStream not initialized");return true;}
 if(IS("write","(I)V")){
  if(!a||n<1)return false;
  unsigned char byte=(unsigned char)a[0].i;
  return fos_write_bytes(v,f,&byte,1)?true:(dvm_exception(v)!=0);
 }
 bool ranged=IS("write","([BII)V");
 if(ranged||IS("write","([B)V")){
  if(!a||(ranged?n<3:n<1))return false;
  struct dvm_object *arr=dvm__obj(v,a[0].l);
  if(!arr){dvm__throw(v,"java/lang/NullPointerException","FileOutputStream.write");return true;}
  if(arr->kind!=DVM_OBJ_ARRAY||arr->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","not a byte array");return true;}
  int32_t off=ranged?a[1].i:0;
  int32_t len=ranged?a[2].i:(int32_t)arr->length;
  if(off<0||len<0||(uint64_t)(uint32_t)off+(uint32_t)len>arr->length){dvm__throw(v,"java/lang/IndexOutOfBoundsException","FileOutputStream.write range");return true;}
  if(!len)return true;
  return fos_write_bytes(v,f,(const unsigned char*)arr->data+(size_t)off,(size_t)len)?true:(dvm_exception(v)!=0);
 }
 if(IS("flush","()V"))return true;
 if(IS("close","()V")){
  if(!f->closed){
   f->closed=1;
   if(f->fd>=0){
    int fd=f->fd;f->fd=-1;
    if(close(fd)){dvm__throw(v,"java/io/IOException","FileOutputStream close failed");return true;}
   }
  }
  return true;
 }
 return false;
}

/* ---- BufferedReader over a FileReader ---- */
static bool br_child_read(struct dvm *v,br_data *b,int *out){
 struct dvm_object *child=b->reader?dvm__obj(v,b->reader):NULL;
 if(!child||child->data_destroy!=fr_free){dvm__throw(v,"java/io/IOException","BufferedReader reader is unavailable");return false;}
 return fr_read_unit(v,child->data,out);
}
static bool br_append_cp(struct dvm *v,uint32_t cp,unsigned char **buf,size_t *len,size_t *cap){
 size_t need=cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;
 if(*len+need>*cap){
  size_t next=*cap?*cap:128;
  while(next<*len+need){if(next>524288){next=*len+need;break;}next*=2;}
  if(next>1048576){dvm__throw(v,"java/lang/OutOfMemoryError","BufferedReader line");return false;}
  unsigned char *grown=realloc(*buf,next);
  if(!grown){dvm__throw(v,"java/lang/OutOfMemoryError","BufferedReader line");return false;}
  *buf=grown;*cap=next;
 }
 unsigned char *p=*buf+*len;
 if(cp<0x80)*p++=(unsigned char)cp;
 else if(cp<0x800){*p++=(unsigned char)(0xC0|(cp>>6));*p++=(unsigned char)(0x80|(cp&0x3F));}
 else if(cp<0x10000){*p++=(unsigned char)(0xE0|(cp>>12));*p++=(unsigned char)(0x80|((cp>>6)&0x3F));*p++=(unsigned char)(0x80|(cp&0x3F));}
 else{*p++=(unsigned char)(0xF0|(cp>>18));*p++=(unsigned char)(0x80|((cp>>12)&0x3F));*p++=(unsigned char)(0x80|((cp>>6)&0x3F));*p++=(unsigned char)(0x80|(cp&0x3F));}
 *len+=need;return true;
}
/* readLine(): CR, LF and CRLF terminate; the terminator is not returned.  A
 * bare EOF returns null, while EOF after a terminator returns the (possibly
 * empty) line that preceded it. */
static bool br_readline(struct dvm *v,br_data *b,union dvm_value *out){
 unsigned char *line=NULL;size_t len=0,cap=0;
 for(;;){
  int unit=0;
  if(b->pushback>=0){unit=b->pushback;b->pushback=-1;}
  else if(!br_child_read(v,b,&unit)){free(line);return false;}
  if(unit<0){if(!len){out->l=0;return true;}break;}
  if(unit=='\n')break;
  if(unit=='\r'){
   int next=0;
   if(!br_child_read(v,b,&next)){free(line);return false;}
   if(next>=0&&next!='\n')b->pushback=next;
   break;
  }
  uint32_t cp=(uint32_t)unit;
  if(unit>=0xD800&&unit<=0xDBFF){
   int low=0;
   if(b->pushback>=0){low=b->pushback;b->pushback=-1;}
   else if(!br_child_read(v,b,&low)){free(line);return false;}
   if(low>=0xDC00&&low<=0xDFFF)cp=0x10000+(((uint32_t)unit-0xD800)<<10)+((uint32_t)low-0xDC00);
   else{
    if(low>=0)b->pushback=low;
    dvm__throw(v,"java/io/IOException","unpaired UTF-16 surrogate");
    free(line);return false;
   }
  }else if(unit>=0xDC00&&unit<=0xDFFF){
   dvm__throw(v,"java/io/IOException","unpaired UTF-16 surrogate");
   free(line);return false;
  }
  if(!br_append_cp(v,cp,&line,&len,&cap)){free(line);return false;}
 }
 out->l=dvm_new_string_n(v,line?(const char*)line:"",len);
 free(line);
 if(!out->l){dvm__throw(v,"java/lang/OutOfMemoryError","BufferedReader.readLine");return false;}
 return true;
}
static bool br_read_single(struct dvm *v,br_data *b,int *out){
 if(b->pushback>=0){*out=b->pushback;b->pushback=-1;return true;}
 return br_child_read(v,b,out);
}
static bool br_read_chars(struct dvm *v,br_data *b,dvm_ref array,int off,int len,union dvm_value *out){
 bool ok=false;
 struct dvm_object *o=fr_char_array(v,array,off,len,&ok);
 if(!ok)return true;
 if(!len){out->i=0;return true;}
 uint16_t *dst=o->data;int count=0;
 while(count<len){
  int unit=0;
  if(!br_read_single(v,b,&unit))return true;
  if(unit<0){if(!count){out->i=-1;return true;}break;}
  dst[off+count]=(uint16_t)unit;
  ++count;
 }
 out->i=count;return true;
}
static bool br_ready_data(struct dvm *v,br_data *b,int *out){
 if(b->closed){dvm__throw(v,"java/io/IOException","Stream closed");return false;}
 if(b->pushback>=0){*out=1;return true;}
 struct dvm_object *child=b->reader?dvm__obj(v,b->reader):NULL;
 if(!child||child->data_destroy!=fr_free){dvm__throw(v,"java/io/IOException","BufferedReader reader is unavailable");return false;}
 return fr_ready(v,child->data,out);
}
static bool br_skip(struct dvm *v,br_data *b,long long want,union dvm_value *out){
 /* BufferedReader.skip: negative is IllegalArgumentException, zero is a
  * successful no-op, and the stream position must not move on the throw. */
 if(want<0){dvm__throw(v,"java/lang/IllegalArgumentException","skip value is negative");return true;}
 if(b->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
 if(want==0){out->j=0;return true;}
 long long skipped=0;
 while(skipped<want){
  int unit=0;
  if(!br_read_single(v,b,&unit))return true;
  if(unit<0)break;
  ++skipped;
 }
 out->j=skipped;return true;
}
static bool br_dispatch(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","BufferedReader receiver");return true;}
 if(IS("<init>","(Ljava/io/Reader;)V")||IS("<init>","(Ljava/io/Reader;I)V")){
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","BufferedReader already initialized");return true;}
  if(n<1||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","reader is null");return true;}
  if(IS("<init>","(Ljava/io/Reader;I)V")&&(n<2||a[1].i<=0)){dvm__throw(v,"java/lang/IllegalArgumentException","buffer size must be positive");return true;}
  struct dvm_object *child=dvm__obj(v,a[0].l);
  if(!child){dvm__throw(v,"java/lang/NullPointerException","reader is null");return true;}
  if(child->data_destroy==fr_free){
   br_data *b=calloc(1,sizeof *b);
   if(!b){dvm__throw(v,"java/lang/OutOfMemoryError","BufferedReader");return true;}
   b->vm=v;b->reader=a[0].l;b->pushback=-1;
   dvm_pin(v,b->reader);
   o->data=b;o->data_destroy=br_free;return true;
  }
  /* AssetManager-backed readers belong to asset_reader.c. */
  if(asset_reader_call&&asset_reader_call(v,"java/io/BufferedReader",m,sig,self,a,n,out))return true;
  dvm__throw(v,"java/lang/UnsupportedOperationException","BufferedReader requires a FileReader or an asset reader");return true;
 }
 br_data *b=br_of(v,self);
 if(!b){
  if(asset_reader_call&&asset_reader_call(v,"java/io/BufferedReader",m,sig,self,a,n,out))return true;
  dvm__throw(v,"java/lang/IllegalStateException","BufferedReader not initialized");return true;
 }
 if(IS("readLine","()Ljava/lang/String;")){
  if(b->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
  br_readline(v,b,out);return true;
 }
 if(IS("read","()I")){
  if(b->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
  int unit=0;
  if(!br_read_single(v,b,&unit))return true;
  out->i=unit;return true;
 }
 if(IS("read","([C)I")){
  if(b->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
  if(n<1||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","char array is null");return true;}
  struct dvm_object *arr=dvm__obj(v,a[0].l);
  if(!arr){dvm__throw(v,"java/lang/NullPointerException","char array is null");return true;}
  if(arr->kind!=DVM_OBJ_ARRAY||arr->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","not a char array");return true;}
  return br_read_chars(v,b,a[0].l,0,(int)arr->length,out);
 }
 if(IS("read","([CII)I")){
  if(b->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
  if(n<3||!a){dvm__throw(v,"java/lang/IllegalArgumentException","missing read range");return true;}
  return br_read_chars(v,b,a[0].l,a[1].i,a[2].i,out);
 }
 if(IS("close","()V")){
  if(b->closed)return true;
  b->closed=1;
  struct dvm_object *child=b->reader?dvm__obj(v,b->reader):NULL;
  if(child&&child->data_destroy==fr_free)(void)fr_close_data(v,child->data);
  return true;
 }
 if(IS("ready","()Z")){int ready=0;if(!br_ready_data(v,b,&ready))return true;out->i=ready;return true;}
 if(IS("skip","(J)J")){
  if(b->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
  if(n<1||!a){dvm__throw(v,"java/lang/IllegalArgumentException","missing skip count");return true;}
  return br_skip(v,b,a[0].j,out);
 }
 return false;
}
static bool br_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active){dvm__throw(v,"java/lang/IllegalStateException","io runtime is not active");return true;}
 struct dvm_method *method=dvm__builtin_method();
 if(!method||!method->name||!method->sig){
  dvm__throw(v,"java/lang/UnsupportedOperationException","BufferedReader dispatch");return true;
 }
 if(br_dispatch(v,method->name,method->sig,self,a,n,out))return true;
 dvm__throw(v,"java/lang/UnsupportedOperationException","BufferedReader.%s%s",method->name,method->sig);
 return true;
}
/* The external-call chain reaches asset_reader_call() before io_runtime_call(),
 * so BufferedReader methods must be visible on the class or they are lost. */
static bool br_methods_install(struct dvm *v){
 struct dvm_class *br=dvm_find_class_or_external(v,"java/io/BufferedReader");
 if(!br||!br->external||br->nmethods)return true;
 static const struct {const char *name,*sig;char ret;int args,slots;} defs[]={
  {"<init>","(Ljava/io/Reader;)V",'V',1,1},
  {"<init>","(Ljava/io/Reader;I)V",'V',2,2},
  {"readLine","()Ljava/lang/String;",'L',0,0},
  {"read","()I",'I',0,0},
  {"read","([C)I",'I',1,1},
  {"read","([CII)I",'I',3,3},
  {"close","()V",'V',0,0},
  {"ready","()Z",'Z',0,0},
  {"skip","(J)J",'J',1,2},
 };
 size_t count=sizeof defs/sizeof defs[0];
 struct dvm_method *methods=calloc(count,sizeof *methods);
 if(!methods)return false;
 for(size_t i=0;i<count;++i){
  methods[i].cls=br;methods[i].name=defs[i].name;methods[i].sig=strdup(defs[i].sig);
  if(!methods[i].sig){free(methods);return false;}
  methods[i].builtin=br_builtin;methods[i].ret_kind=defs[i].ret;
  methods[i].arg_count=defs[i].args;methods[i].arg_slots=defs[i].slots;
 }
 br->methods=methods;br->nmethods=(int)count;
 return true;
}

void io_runtime_vm_start(struct dvm *v){
 active=v;io_sandbox_state=0;io_sandbox_base[0]=0;io_selftest_allow[0]=0;
 struct dvm_class *writer=dvm_find_class_or_external(v,"java/io/Writer");
 struct dvm_class *reader=dvm_find_class_or_external(v,"java/io/Reader");
 struct dvm_class *input=dvm_find_class_or_external(v,"java/io/InputStreamReader");
 struct dvm_class *file_reader=dvm_find_class_or_external(v,"java/io/FileReader");
 struct dvm_class *buffered=dvm_find_class_or_external(v,"java/io/BufferedReader");
 struct dvm_class *sw=dvm_find_class_or_external(v,"java/io/StringWriter");
 struct dvm_class *pw=dvm_find_class_or_external(v,"java/io/PrintWriter");
 struct dvm_class *output=dvm_find_class_or_external(v,"java/io/OutputStream");
 struct dvm_class *file_output=dvm_find_class_or_external(v,"java/io/FileOutputStream");
 if(writer&&sw&&sw->external)sw->super=writer;
 if(writer&&pw&&pw->external)pw->super=writer;
 if(reader&&input&&input->external)input->super=reader;
 if(reader&&file_reader&&file_reader->external)file_reader->super=input&&input->external?input:reader;
 if(reader&&buffered&&buffered->external)buffered->super=reader;
 if(output&&file_output&&file_output->external)file_output->super=output;
 (void)br_methods_install(v);
}
static sw_data *sw_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);return o&&o->data_destroy==sw_free?o->data:NULL;
}
static pw_data *pw_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);return o&&o->data_destroy==pw_free?o->data:NULL;
}
static bool append_bytes(struct dvm *v,dvm_ref sw,const char *bytes,size_t n){
 sw_data *s=sw_of(v,sw);if(!s){dvm__throw(v,"java/lang/IllegalStateException","StringWriter not initialized");return false;}
 if(s->len+n>s->cap){size_t cap=s->cap?s->cap*2:128;while(cap<s->len+n)cap*=2;if(cap>1048576){dvm__throw(v,"java/lang/OutOfMemoryError","StringWriter");return false;}char *p=realloc(s->buf,cap);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","StringWriter");return false;}s->buf=p;s->cap=cap;}
 memcpy(s->buf+s->len,bytes,n);s->len+=n;return true;
}
static bool append_text(struct dvm *v,dvm_ref pw,const char *bytes,size_t n){
 pw_data *w=pw_of(v,pw);if(!w){dvm__throw(v,"java/lang/IllegalStateException","PrintWriter not initialized");return false;}
 if(!w->writer){dvm__throw(v,"java/lang/IllegalStateException","PrintWriter has no writer");return false;}
 return append_bytes(v,w->writer,bytes,n);
}
static bool append_string(struct dvm *v,dvm_ref pw,dvm_ref text){
 if(!text)return append_text(v,pw,"null",4);
 const char *s=dvm_string_utf8(v,text);size_t n=dvm_string_utf8_length(v,text);
 if(!s){dvm__throw(v,"java/lang/IllegalArgumentException","PrintWriter string");return false;}
 return append_text(v,pw,s,n);
}
static bool append_char(struct dvm *v,dvm_ref pw,uint32_t cp){
 char out[4];size_t n=0;
 if(cp<0x80)out[n++]=(char)cp;
 else if(cp<0x800){out[n++]=(char)(0xc0|(cp>>6));out[n++]=(char)(0x80|(cp&63));}
 else if(cp<0x10000){out[n++]=(char)(0xe0|(cp>>12));out[n++]=(char)(0x80|((cp>>6)&63));out[n++]=(char)(0x80|(cp&63));}
 else{out[n++]=(char)(0xf0|(cp>>18));out[n++]=(char)(0x80|((cp>>12)&63));out[n++]=(char)(0x80|((cp>>6)&63));out[n++]=(char)(0x80|(cp&63));}
 return append_text(v,pw,out,n);
}
/* java/io/OutputStream: Android's base has an empty constructor, empty
 * flush()/close(), and an abstract write(int).  write(byte[]) and
 * write(byte[],int,int) are defaults that dispatch back through the receiver,
 * so a guest override (DiskLruCache$2.write(int)) wins.  A receiver with no
 * concrete write(int) must fail closed, never be answered with a no-op. */
static bool os_check(struct dvm *v,dvm_ref array,struct dvm_object **out_obj){
 struct dvm_object *o=dvm__obj(v,array);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","byte array is null");return false;}
 if(o->kind!=DVM_OBJ_ARRAY||o->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","not a byte array");return false;}
 if(out_obj)*out_obj=o;return true;
}
static bool os_check_range(struct dvm *v,dvm_ref array,int off,int len,struct dvm_object **out_obj){
 struct dvm_object *o=NULL;
 if(!os_check(v,array,&o))return false;
 if(off<0||len<0||(uint32_t)off>o->length||(uint32_t)len>o->length-(uint32_t)off){
  dvm__throw(v,"java/lang/IndexOutOfBoundsException","off=%d len=%d length=%u",off,len,o->length);return false;
 }
 if(out_obj)*out_obj=o;return true;
}
static bool os_write_one(struct dvm *v,dvm_ref self,int value,bool *invoked){
 struct dvm_class *rc=dvm_object_class(v,self);
 struct dvm_method *m=rc?dvm_find_method(v,rc,"write","(I)V"):NULL;
 *invoked=false;
 if(!m||(!m->has_code&&!m->builtin))return false;
 *invoked=true;
 union dvm_value arg={.i=value},r={0};
 return dvm_call(v,m,self,&arg,1,&r);
}
static bool os_write_bytes(struct dvm *v,dvm_ref self,const uint8_t *bytes,uint32_t len){
 for(uint32_t i=0;i<len;i++){
  bool invoked=false;
  if(!os_write_one(v,self,(int)bytes[i],&invoked)){
   if(!invoked)return false;              /* no concrete override: fail closed */
   return dvm_exception(v)!=0;            /* an override threw: the call was handled */
  }
 }
 return true;
}
/* OutputStream.write(byte[]) is write(b,0,b.length) with virtual dispatch. */
static bool os_write_array(struct dvm *v,dvm_ref self,dvm_ref array,uint32_t len){
 struct dvm_class *rc=dvm_object_class(v,self);
 struct dvm_method *m=rc?dvm_find_method(v,rc,"write","([BII)V"):NULL;
 if(m&&(m->has_code||m->builtin)){
  union dvm_value a[3]={{.l=array},{.i=0},{.i=(int)len}},r={0};
  (void)dvm_call(v,m,self,a,3,&r);return true;
 }
 return os_write_bytes(v,self,dvm_array_data(v,array),len);
}
static bool fis_read_one(struct dvm *v,fr_data *f,union dvm_value *out){
 if(f->closed||f->fd<0){dvm__throw(v,"java/io/IOException","FileInputStream closed");return false;}
 unsigned char byte;
 ssize_t got;
 do{got=read(f->fd,&byte,1);}while(got<0&&errno==EINTR);
 if(got<0){dvm__throw(v,"java/io/IOException","FileInputStream read failed");return false;}
 out->i=got==0?-1:(int)byte;return true;
}
bool io_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active)return false;
 if(!strcmp(c,"java/io/ObjectInputStream")){
  if(IS("<init>","(Ljava/io/InputStream;)V")){
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","ObjectInputStream stream");return true;}
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ObjectInputStream receiver");return true;}
   ois_data *d=calloc(1,sizeof *d);if(!d){dvm__throw(v,"java/lang/OutOfMemoryError","ObjectInputStream");return true;}
   d->in=a[0].l;if(o->data_destroy==ois_free)free(o->data);o->data=d;o->data_destroy=ois_free;return true;
  }
  /* AOSP ObjectInputStream.close closes the underlying stream and is
   * idempotent; the reference is cleared so a second close does not call the
   * peer again.  A failure from the peer's close propagates unchanged. */
  if(IS("close","()V")){
   struct dvm_object *o=dvm__obj(v,self);ois_data *d=o&&o->data_destroy==ois_free?o->data:NULL;
   if(!d){dvm__throw(v,"java/lang/IllegalStateException","ObjectInputStream not initialized");return true;}
   dvm_ref in=d->in;d->in=0;
   if(in){struct dvm_class *ic=dvm_object_class(v,in);struct dvm_method *m=ic?dvm_find_method(v,ic,"close","()V"):NULL;
    if(m&&(m->has_code||m->builtin)){union dvm_value r={0};if(!dvm_call(v,m,in,NULL,0,&r))return true;}}
   return true;
  }
  return false;
 }
 if(!strcmp(c,"java/io/FileInputStream")){
  if(IS("<init>","(Ljava/lang/String;)V")){
   if(n<1||!a){dvm__throw(v,"java/lang/NullPointerException","path is null");return true;}
   return fr_construct(v,self,a[0].l,out);
  }
  if(IS("<init>","(Ljava/io/File;)V")){
   if(n<1||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","file is null");return true;}
   union dvm_value path={0};
   if(!file_runtime_call||!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",a[0].l,NULL,0,&path)||!path.l){
    dvm__throw(v,"java/io/FileNotFoundException","file path is unavailable");return true;
   }
   return fr_construct(v,self,path.l,out);
  }
  fr_data *f=fr_of(v,self);
  if(!f){dvm__throw(v,"java/lang/IllegalStateException","FileInputStream not initialized");return true;}
  if(IS("read","()I")){return fis_read_one(v,f,out);}
  if(IS("read","([B)I")||IS("read","([BII)I")){
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","byte array");return true;}
   struct dvm_object *arr=dvm__obj(v,a[0].l);
   if(!arr){dvm__throw(v,"java/lang/NullPointerException","byte array");return true;}
   if(arr->kind!=DVM_OBJ_ARRAY||arr->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","not a byte array");return true;}
   int32_t off=IS("read","([BII)I")?a[1].i:0;
   int32_t len=IS("read","([BII)I")?a[2].i:(int32_t)arr->length;
   if(off<0||len<0||(uint64_t)(uint32_t)off+(uint32_t)len>arr->length){dvm__throw(v,"java/lang/IndexOutOfBoundsException","FileInputStream.read range");return true;}
   if(!len){out->i=0;return true;}
   unsigned char *dst=(unsigned char*)arr->data+(size_t)off;
   ssize_t got;
   do{got=read(f->fd,dst,(size_t)len);}while(got<0&&errno==EINTR);
   if(got<0){dvm__throw(v,"java/io/IOException","FileInputStream read failed");return true;}
   out->i=got==0?-1:(int32_t)got;return true;
  }
  if(IS("skip","(J)J")){
   if(n<1||!a){dvm__throw(v,"java/lang/IllegalArgumentException","missing skip count");return true;}
   off_t now=lseek(f->fd,0,SEEK_CUR);
   off_t next=now<0?-1:lseek(f->fd,a[0].j,SEEK_CUR);
   if(next<0&&now>=0){dvm__throw(v,"java/io/IOException","FileInputStream.skip failed");return true;}
   out->j=next<0?0:(int64_t)(next-now);return true;
  }
  if(IS("available","()I")){out->i=0;return true;}
  if(IS("close","()V")){
   if(!f->closed&&f->fd>=0){close(f->fd);f->fd=-1;}
   f->closed=1;return true;
  }
  return false;
 }
 if(!strcmp(c,"java/io/FileReader")){
  if(IS("<init>","(Ljava/lang/String;)V")){
   if(n<1||!a){dvm__throw(v,"java/lang/NullPointerException","path is null");return true;}
   return fr_construct(v,self,a[0].l,out);
  }
  if(IS("<init>","(Ljava/io/File;)V")){
   if(n<1||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","file is null");return true;}
   union dvm_value path={0};
   if(!file_runtime_call||!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",a[0].l,NULL,0,&path)||!path.l){
    dvm__throw(v,"java/io/FileNotFoundException","file path is unavailable");return true;
   }
   return fr_construct(v,self,path.l,out);
  }
  fr_data *f=fr_of(v,self);
  if(!f){dvm__throw(v,"java/lang/IllegalStateException","FileReader not initialized");return true;}
  if(IS("read","()I")){int unit=0;if(!fr_read_unit(v,f,&unit))return true;out->i=unit;return true;}
  if(IS("read","([C)I")){
   if(n<1||!a){dvm__throw(v,"java/lang/NullPointerException","char array is null");return true;}
   struct dvm_object *arr=dvm__obj(v,a[0].l);
   if(!arr){dvm__throw(v,"java/lang/NullPointerException","char array is null");return true;}
   if(arr->kind!=DVM_OBJ_ARRAY||arr->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","not a char array");return true;}
   return fr_read_chars(v,f,a[0].l,0,(int)arr->length,out);
  }
  if(IS("read","([CII)I")){
   if(n<3||!a){dvm__throw(v,"java/lang/IllegalArgumentException","missing read range");return true;}
   return fr_read_chars(v,f,a[0].l,a[1].i,a[2].i,out);
  }
  if(IS("close","()V")){fr_close_data(v,f);return true;}
  if(IS("ready","()Z")){int ready=0;if(!fr_ready(v,f,&ready))return true;out->i=ready;return true;}
  if(IS("skip","(J)J")){
   if(n<1||!a){dvm__throw(v,"java/lang/IllegalArgumentException","missing skip count");return true;}
   return fr_skip(v,f,a[0].j,out);
  }
  return false;
 }
 if(!strcmp(c,"java/io/BufferedReader"))return br_dispatch(v,m,sig,self,a,n,out);
 if(!strcmp(c,"java/io/FileOutputStream")){
  if(IS("<init>","(Ljava/lang/String;)V")){
   if(n<1||!a)return false;
   return fos_construct(v,self,a[0].l,false,out);
  }
  if(IS("<init>","(Ljava/lang/String;Z)V")){
   if(n<2||!a)return false;
   return fos_construct(v,self,a[0].l,a[1].i!=0,out);
  }
  if(IS("<init>","(Ljava/io/File;)V")||IS("<init>","(Ljava/io/File;Z)V")){
   bool append=IS("<init>","(Ljava/io/File;Z)V");
   if(n<(append?2:1)||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","file is null");return true;}
   union dvm_value path={0};
   if(!file_runtime_call||!file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",a[0].l,NULL,0,&path)||!path.l){
    dvm__throw(v,"java/io/FileNotFoundException","file path is unavailable");return true;
   }
   return fos_construct(v,self,path.l,append&&a[1].i!=0,out);
  }
  return fos_dispatch(v,m,sig,self,a,n,out);
 }
 if(!strcmp(c,"java/io/OutputStream")){
  /* A receiver whose runtime class is FileOutputStream must use its real
   * descriptor even when the DEX call site is typed as OutputStream.  Without
   * this, the base no-op close/flush would leak the descriptor. */
  struct dvm_class *receiver=dvm_object_class(v,self);
  if(receiver&&fos_is_class(receiver)&&
     (IS("write","(I)V")||IS("write","([B)V")||IS("write","([BII)V")||IS("flush","()V")||IS("close","()V")))
   return fos_dispatch(v,m,sig,self,a,n,out);
  if(IS("<init>","()V"))return true;              /* Object.<init> is the VM's */
  if(IS("flush","()V")||IS("close","()V"))return true;
  if(IS("write","(I)V"))return false;             /* abstract in Android: fail closed */
  if(IS("write","([B)V")&&n>=1&&a){
   struct dvm_object *o=NULL;
   if(!os_check(v,a[0].l,&o))return true;         /* NPE / wrong array type is a throw, not a miss */
   return os_write_array(v,self,a[0].l,o->length);
  }
  if(IS("write","([BII)V")&&n>=3&&a){
   struct dvm_object *o=NULL;
   if(!os_check_range(v,a[0].l,a[1].i,a[2].i,&o))return true;
   if(!a[2].i)return true;
   return os_write_bytes(v,self,(const uint8_t*)o->data+(size_t)a[1].i,(uint32_t)a[2].i);
  }
  return false;
 }
 if(!strcmp(c,"java/io/ByteArrayOutputStream")){
  if(IS("<init>","()V")||IS("<init>","(I)V")){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ByteArrayOutputStream receiver");return true;}
   if(IS("<init>","(I)V")&&a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative ByteArrayOutputStream size");return true;}
   baos_data *b=calloc(1,sizeof *b);if(!b){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return true;}
   if(IS("<init>","(I)V")&&a[0].i>0){b->buf=malloc((size_t)a[0].i);if(!b->buf){free(b);dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return true;}b->cap=(size_t)a[0].i;}
   o->data=b;o->data_destroy=baos_free;return true;
  }
  baos_data *b=baos_of(v,self);if(!b){dvm__throw(v,"java/lang/IllegalStateException","ByteArrayOutputStream not initialized");return true;}
  if(IS("write","(I)V")){unsigned char byte=(unsigned char)a[0].i;return baos_append(v,b,&byte,1)?true:(dvm_exception(v)!=0);}
  if(IS("write","([B)V")||IS("write","([BII)V")){
   struct dvm_object *arr=dvm__obj(v,a[0].l);
   if(!arr){dvm__throw(v,"java/lang/NullPointerException","ByteArrayOutputStream.write");return true;}
   if(arr->kind!=DVM_OBJ_ARRAY||arr->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","not a byte array");return true;}
   int32_t off=IS("write","([BII)V")?a[1].i:0;
   int32_t len=IS("write","([BII)V")?a[2].i:(int32_t)arr->length;
   if(off<0||len<0||(uint64_t)(uint32_t)off+(uint32_t)len>arr->length){dvm__throw(v,"java/lang/IndexOutOfBoundsException","ByteArrayOutputStream.write range");return true;}
   if(!len)return true;
   return baos_append(v,b,(const unsigned char*)arr->data+(size_t)off,(size_t)len)?true:(dvm_exception(v)!=0);
  }
  if(IS("size","()I")){if(b->len>INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream size");return true;}out->i=(int32_t)b->len;return true;}
  if(IS("reset","()V")||IS("close","()V")){if(IS("reset","()V"))b->len=0;return true;}
  if(IS("toByteArray","()[B")){
   if(b->len>INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return true;}
   dvm_ref array=dvm_new_array(v,'B',"B",(uint32_t)b->len);
   if(!array){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return true;}
   if(b->len)memcpy(dvm_array_data(v,array),b->buf,b->len);
   out->l=array;return true;
  }
  if(IS("toString","()Ljava/lang/String;")){out->l=dvm_new_string_n(v,(const char*)(b->buf?b->buf:(const unsigned char*)""),b->len);return true;}
  return false;
 }
 if(!strcmp(c,"java/io/StringWriter")){
  if(IS("<init>","()V")||IS("<init>","(I)V")){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","StringWriter receiver");return true;}
   if(IS("<init>","(I)V")&&a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative StringWriter size");return true;}
   sw_data *s=calloc(1,sizeof *s);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","StringWriter");return true;}
   if(IS("<init>","(I)V")&&a[0].i>128){s->buf=malloc((size_t)a[0].i);if(!s->buf){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","StringWriter");return true;}s->cap=(size_t)a[0].i;}
   o->data=s;o->data_destroy=sw_free;return true;
  }
  sw_data *s=sw_of(v,self);if(!s){dvm__throw(v,"java/lang/IllegalStateException","StringWriter not initialized");return true;}
  if(IS("write","(Ljava/lang/String;)V")||IS("append","(Ljava/lang/CharSequence;)Ljava/io/StringWriter;")){
   if(!a[0].l){if(!append_bytes(v,self,"null",4))return true;}
   else{const char *p=dvm_string_utf8(v,a[0].l);size_t len=dvm_string_utf8_length(v,a[0].l);if(!p){dvm__throw(v,"java/lang/UnsupportedOperationException","StringWriter CharSequence");return true;}if(!append_bytes(v,self,p,len))return true;}
   if(IS("append","(Ljava/lang/CharSequence;)Ljava/io/StringWriter;"))out->l=self;return true;
  }
  if(IS("write","(Ljava/lang/String;II)V")){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","StringWriter.write");return true;}
   const char *p=dvm_string_utf8(v,a[0].l);size_t len=dvm_string_utf8_length(v,a[0].l);
   if(!p||a[1].i<0||a[2].i<0||(size_t)(a[1].i+a[2].i)>len){dvm__throw(v,"java/lang/IndexOutOfBoundsException","StringWriter.write range");return true;}
   if(!append_bytes(v,self,p+a[1].i,(size_t)a[2].i))return true;return true;
  }
  if(IS("write","(I)V")||IS("append","(C)Ljava/io/StringWriter;")){
   char ch=(char)a[0].i;if(!append_bytes(v,self,&ch,1))return true;
   if(IS("append","(C)Ljava/io/StringWriter;"))out->l=self;return true;
  }
  if(IS("flush","()V")||IS("close","()V"))return true;
  if(IS("toString","()Ljava/lang/String;")){out->l=dvm_new_string_n(v,s->buf?s->buf:"",s->len);return true;}
  return false;
 }
 if(!strcmp(c,"java/io/PrintWriter")){
  if(IS("<init>","(Ljava/io/Writer;)V")||IS("<init>","(Ljava/io/Writer;Z)V")){
   struct dvm_object *o=dvm__obj(v,self);if(!o||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","PrintWriter writer");return true;}
   pw_data *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","PrintWriter");return true;}
   w->writer=a[0].l;o->data=w;o->data_destroy=pw_free;return true;
  }
  if(IS("flush","()V")||IS("close","()V"))return true;
  if(IS("print","(Ljava/lang/String;)V")||IS("write","(Ljava/lang/String;)V")||IS("println","(Ljava/lang/String;)V")){
   if(!append_string(v,self,a[0].l))return true;
   if(!strcmp(m,"println")&&!append_text(v,self,"\n",1))return true;return true;
  }
  if(IS("println","()V"))return append_text(v,self,"\n",1);
  if(IS("print","(Ljava/lang/Object;)V")||IS("println","(Ljava/lang/Object;)V")){
   const char *p=NULL;size_t len=0;
   if(!a[0].l){p="null";len=4;}
   else{struct dvm_object *o=dvm__obj(v,a[0].l);if(o&&o->kind==DVM_OBJ_STRING){p=o->utf8;len=o->utf8_len;}else{struct dvm_method *t=dvm_find_method(v,o->cls,"toString","()Ljava/lang/String;");union dvm_value r={0};if(!t||!(t->has_code||t->builtin)||!dvm_call(v,t,a[0].l,NULL,0,&r)||!r.l){dvm__throw(v,"java/lang/UnsupportedOperationException","PrintWriter.toString");return true;}struct dvm_object *s=dvm__obj(v,r.l);if(!s||s->kind!=DVM_OBJ_STRING){dvm__throw(v,"java/lang/IllegalStateException","PrintWriter.toString result");return true;}p=s->utf8;len=s->utf8_len;}}
   if(!append_text(v,self,p,len))return true;
   if(!strcmp(m,"println")&&!append_text(v,self,"\n",1))return true;return true;
  }
  if(IS("write","(I)V"))return append_char(v,self,(uint32_t)a[0].i);
  if(IS("append","(Ljava/lang/CharSequence;)Ljava/io/PrintWriter;")){
   if(!append_string(v,self,a[0].l))return true;out->l=self;return true;
  }
  if(IS("append","(C)Ljava/io/PrintWriter;")){
   if(!append_char(v,self,(uint32_t)a[0].i))return true;out->l=self;return true;
  }
  return false;
 }
 if(!strcmp(c,"java/lang/Throwable")&&IS("printStackTrace","(Ljava/io/PrintWriter;)V")){
  if(!a[0].l)return true;
  char why[8192];dvm_describe_exception(v,self,why,sizeof why);
  if(!append_text(v,a[0].l,why,strlen(why)))return true;
  return append_text(v,a[0].l,"\n",1);
 }
 if(!strcmp(c,"java/lang/Throwable")&&IS("printStackTrace","()V")){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","printStackTrace receiver");return true;}
  char why[8192];dvm_describe_exception(v,self,why,sizeof why);
  sdk_trace_fprintf(stderr,"%s\n",why);
  return true;
 }
 return false;
}
static bool io_selftest_exception_named(struct dvm *v,const char *name){
 dvm_ref e=dvm_exception(v);
 struct dvm_class *c=e?dvm_object_class(v,e):NULL;
 bool ok=c&&c->name&&!strcmp(c->name,name);
 dvm_clear_exception(v);
 return ok;
}
static bool io_selftest_string(struct dvm *v,dvm_ref ref,const char *bytes,size_t len){
 if(!ref||dvm_string_utf8_length(v,ref)!=len)return false;
 const char *got=dvm_string_utf8(v,ref);
 return got&&!memcmp(got,bytes,len);
}
/* The selftest fixture lives under file_runtime's guest-cache root, which is
 * inside the sandbox base FileReader enforces.  mkstemp gives an exclusive
 * file; no caller-controlled path is opened. */
static bool io_selftest_cache_dir(struct dvm *v,char *buf,size_t cap,bool *from_runtime){
 if(from_runtime)*from_runtime=false;
 if(android_basics_application&&file_runtime_call){
  dvm_ref app=android_basics_application(v);
  if(app){
   union dvm_value file={0},path={0};
   if(file_runtime_call(v,"android/content/Context","getCacheDir","()Ljava/io/File;",app,NULL,0,&file)&&file.l&&
      file_runtime_call(v,"java/io/File","getPath","()Ljava/lang/String;",file.l,NULL,0,&path)&&path.l){
    const char *text=dvm_string_utf8(v,path.l);
    size_t len=dvm_string_utf8_length(v,path.l);
    if(text&&*text&&strlen(text)==len&&len+16<cap){
     memcpy(buf,text,len);buf[len]=0;
     while(len>1&&buf[len-1]=='/')buf[--len]=0;
     if(from_runtime)*from_runtime=true;
     return true;
    }
   }
   if(dvm_exception(v))dvm_clear_exception(v);
  }
 }
 const char *fallback=getenv("TMPDIR");
 if(!fallback||!*fallback)fallback="/tmp";
 size_t len=strlen(fallback);
 if(len+16>=cap||access(fallback,W_OK))return false;
 memcpy(buf,fallback,len);buf[len]=0;
 while(len>1&&buf[len-1]=='/')buf[--len]=0;
 return true;
}
static bool io_selftest_readers(struct dvm *v){
 bool ok=false,wrote=true,file_runtime_ready=false;
 char dir[PATH_MAX],tmp[PATH_MAX],missing[PATH_MAX];
 tmp[0]=0;missing[0]=0;
 struct dvm_class *fr_cls=dvm_find_class_or_external(v,"java/io/FileReader");
 struct dvm_class *br_cls=dvm_find_class_or_external(v,"java/io/BufferedReader");
 struct dvm_class *file_cls=dvm_find_class_or_external(v,"java/io/File");
 union dvm_value out={0},arg={0},range[3]={{0},{0},{0}},skiparg={0};
 dvm_ref fr=0,fr2=0,fr3=0,fr4=0,fr5=0,br=0,br5=0,file=0,bad=0,chars=0,chars2=0,chars4=0,bytes=0;
 uint16_t *cp=NULL;
 struct dvm_method *bm=NULL,*bi=NULL,*bclose=NULL,*bread=NULL,*brange=NULL,*bskip=NULL;
 static const unsigned char fixture[]={'A',0xC3,0xA9,0xE4,0xB8,0xAD,0xF0,0x9F,0x98,0x80,'\n','o','k'};
 static const int units[]={0x41,0xE9,0x4E2D,0xD83D,0xDE00,'\n','o','k'};
 static const char line1[]="A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";
 static const char line2[]="ok";
 if(!fr_cls||!br_cls||!file_cls)return false;
 if(!io_selftest_cache_dir(v,dir,sizeof dir,&file_runtime_ready))return false;
 if(snprintf(tmp,sizeof tmp,"%s/file-reader-XXXXXX",dir)>=(int)sizeof tmp)return false;
 if(snprintf(missing,sizeof missing,"%s.absent",tmp)>=(int)sizeof missing)return false;
 int fd=mkstemp(tmp);
 if(fd<0)return false;
 char resolved_tmp[PATH_MAX];
 io_selftest_allow[0]=0;
 if(realpath(tmp,resolved_tmp))
  (void)snprintf(io_selftest_allow,sizeof io_selftest_allow,"%s",resolved_tmp);
 for(size_t off=0;off<sizeof fixture;){
  ssize_t n=write(fd,fixture+off,sizeof fixture-off);
  if(n<0&&errno==EINTR)continue;
  if(n<=0){wrote=false;break;}
  off+=(size_t)n;
 }
 if(close(fd))wrote=false;
 if(!wrote){unlink(tmp);io_selftest_allow[0]=0;return false;}
 /* UTF-8 decode, surrogate pairs, repeated EOF and close semantics. */
 fr=dvm_new_object(v,fr_cls);
 if(!fr)goto cleanup;
 arg.l=dvm_new_string(v,tmp);
 if(!arg.l)goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/lang/String;)V",fr,&arg,1,&out)||dvm_exception(v))goto cleanup;
 for(size_t i=0;i<sizeof units/sizeof units[0];++i)
  if(!io_runtime_call(v,"java/io/FileReader","read","()I",fr,NULL,0,&out)||dvm_exception(v)||out.i!=units[i])goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","read","()I",fr,NULL,0,&out)||dvm_exception(v)||out.i!=-1)goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","read","()I",fr,NULL,0,&out)||dvm_exception(v)||out.i!=-1)goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","ready","()Z",fr,NULL,0,&out)||dvm_exception(v)||out.i!=0)goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","close","()V",fr,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","close","()V",fr,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","read","()I",fr,NULL,0,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/io/IOException"))goto cleanup;
 /* Char-array reads, range validation, skip and wrong-array refusal. */
 fr2=dvm_new_object(v,fr_cls);
 if(!fr2)goto cleanup;
 arg.l=dvm_new_string(v,tmp);
 if(!arg.l)goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/lang/String;)V",fr2,&arg,1,&out)||dvm_exception(v))goto cleanup;
 /* FileReader.skip(-1) is IllegalArgumentException and leaves the position
  * at the first character; the following char-array read verifies that. */
 skiparg.j=-1;
 if(!io_runtime_call(v,"java/io/FileReader","skip","(J)J",fr2,&skiparg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/IllegalArgumentException"))goto cleanup;
 chars=dvm_new_array(v,'C',"C",8);
 if(!chars)goto cleanup;
 range[0].l=chars;range[1].i=-1;range[2].i=1;
 if(!io_runtime_call(v,"java/io/FileReader","read","([CII)I",fr2,range,3,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/IndexOutOfBoundsException"))goto cleanup;
 range[1].i=0;range[2].i=999;
 if(!io_runtime_call(v,"java/io/FileReader","read","([CII)I",fr2,range,3,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/IndexOutOfBoundsException"))goto cleanup;
 range[1].i=0;range[2].i=0;
 if(!io_runtime_call(v,"java/io/FileReader","read","([CII)I",fr2,range,3,&out)||dvm_exception(v)||out.i!=0)goto cleanup;
 range[0].l=0;range[2].i=1;
 if(!io_runtime_call(v,"java/io/FileReader","read","([CII)I",fr2,range,3,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/NullPointerException"))goto cleanup;
 bytes=dvm_new_array(v,'B',"B",1);
 if(!bytes)goto cleanup;
 arg.l=bytes;
 if(!io_runtime_call(v,"java/io/FileReader","read","([C)I",fr2,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/IllegalArgumentException"))goto cleanup;
 /* read([C) fills exactly the four units it is given; the fourth is the high
  * surrogate, whose low half stays pending for the next call. */
 chars4=dvm_new_array(v,'C',"C",4);
 if(!chars4)goto cleanup;
 arg.l=chars4;
 if(!io_runtime_call(v,"java/io/FileReader","read","([C)I",fr2,&arg,1,&out)||dvm_exception(v)||out.i!=4)goto cleanup;
 cp=dvm_array_data(v,chars4);
 if(cp[0]!=0x41||cp[1]!=0xE9||cp[2]!=0x4E2D||cp[3]!=0xD83D)goto cleanup;
 range[0].l=chars;range[1].i=2;range[2].i=3;
 if(!io_runtime_call(v,"java/io/FileReader","read","([CII)I",fr2,range,3,&out)||dvm_exception(v)||out.i!=3)goto cleanup;
 cp=dvm_array_data(v,chars);
 if(cp[2]!=0xDE00||cp[3]!=0x0A||cp[4]!=0x6F)goto cleanup;
 skiparg.j=1;
 if(!io_runtime_call(v,"java/io/FileReader","skip","(J)J",fr2,&skiparg,1,&out)||dvm_exception(v)||out.j!=1)goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","read","()I",fr2,NULL,0,&out)||dvm_exception(v)||out.i!=-1)goto cleanup;
 if(!io_runtime_call(v,"java/io/FileReader","close","()V",fr2,NULL,0,&out)||dvm_exception(v))goto cleanup;
 /* File-backed constructor (needs a java/io/File payload from file_runtime). */
 if(file_runtime_ready){
  file=dvm_new_object(v,file_cls);
  if(!file)goto cleanup;
  arg.l=dvm_new_string(v,tmp);
  if(!arg.l||!file_runtime_call(v,"java/io/File","<init>","(Ljava/lang/String;)V",file,&arg,1,&out)||dvm_exception(v))goto cleanup;
  fr3=dvm_new_object(v,fr_cls);
  if(!fr3)goto cleanup;
  arg.l=file;
  if(!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/io/File;)V",fr3,&arg,1,&out)||dvm_exception(v))goto cleanup;
  if(!io_runtime_call(v,"java/io/FileReader","read","()I",fr3,NULL,0,&out)||dvm_exception(v)||out.i!=0x41)goto cleanup;
  if(!io_runtime_call(v,"java/io/FileReader","close","()V",fr3,NULL,0,&out)||dvm_exception(v))goto cleanup;
 }
 /* Constructors: NULL, empty, relative and missing paths fail closed. */
 bad=dvm_new_object(v,fr_cls);
 if(!bad)goto cleanup;
 arg.l=0;
 if(!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/NullPointerException"))goto cleanup;
 arg.l=dvm_new_string(v,"");
 if(!arg.l||!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/IllegalArgumentException"))goto cleanup;
 arg.l=dvm_new_string(v,"relative/path");
 if(!arg.l||!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/io/FileNotFoundException"))goto cleanup;
 arg.l=dvm_new_string(v,missing);
 if(!arg.l||!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/io/FileNotFoundException"))goto cleanup;
 /* BufferedReader.readLine over the same file. */
 fr4=dvm_new_object(v,fr_cls);
 if(!fr4)goto cleanup;
 arg.l=dvm_new_string(v,tmp);
 if(!arg.l||!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/lang/String;)V",fr4,&arg,1,&out)||dvm_exception(v))goto cleanup;
 br=dvm_new_object(v,br_cls);
 if(!br)goto cleanup;
 arg.l=fr4;
 if(!io_runtime_call(v,"java/io/BufferedReader","<init>","(Ljava/io/Reader;)V",br,&arg,1,&out)||dvm_exception(v))goto cleanup;
 /* Negative skip must throw before consuming; the next readLine still sees
  * the first line. */
 skiparg.j=-1;
 if(!io_runtime_call(v,"java/io/BufferedReader","skip","(J)J",br,&skiparg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/IllegalArgumentException"))goto cleanup;
 if(!io_runtime_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",br,NULL,0,&out)||dvm_exception(v)||!io_selftest_string(v,out.l,line1,sizeof line1-1))goto cleanup;
 if(!io_runtime_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",br,NULL,0,&out)||dvm_exception(v)||!io_selftest_string(v,out.l,line2,sizeof line2-1))goto cleanup;
 if(!io_runtime_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",br,NULL,0,&out)||dvm_exception(v)||out.l)goto cleanup;
 if(!io_runtime_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",br,NULL,0,&out)||dvm_exception(v)||out.l)goto cleanup;
 if(!io_runtime_call(v,"java/io/BufferedReader","close","()V",br,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",br,NULL,0,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/io/IOException"))goto cleanup;
 /* The installed builtin method table must answer the real invoke path. */
 bm=dvm_find_method(v,br_cls,"readLine","()Ljava/lang/String;");
 bi=dvm_find_method(v,br_cls,"<init>","(Ljava/io/Reader;)V");
 bclose=dvm_find_method(v,br_cls,"close","()V");
 bread=dvm_find_method(v,br_cls,"read","()I");
 brange=dvm_find_method(v,br_cls,"read","([CII)I");
 bskip=dvm_find_method(v,br_cls,"skip","(J)J");
 if(!bm||bm->builtin!=br_builtin||!bi||bi->builtin!=br_builtin||!bclose||bclose->builtin!=br_builtin||!bread||bread->builtin!=br_builtin||!brange||brange->builtin!=br_builtin||!bskip||bskip->builtin!=br_builtin)goto cleanup;
 fr5=dvm_new_object(v,fr_cls);
 if(!fr5)goto cleanup;
 arg.l=dvm_new_string(v,tmp);
 if(!arg.l||!io_runtime_call(v,"java/io/FileReader","<init>","(Ljava/lang/String;)V",fr5,&arg,1,&out)||dvm_exception(v))goto cleanup;
 br5=dvm_new_object(v,br_cls);
 if(!br5)goto cleanup;
 arg.l=fr5;
 if(!dvm_call(v,bi,br5,&arg,1,&out)||dvm_exception(v))goto cleanup;
 /* Negative skip through the real builtin dispatch must throw and must not
  * consume the first line; the following readLine proves the position. */
 skiparg.j=-1;
 /* dvm_call returns false when the callee throws; inspect and clear the
  * pending exception instead of treating that return as a test failure. */
 (void)dvm_call(v,bskip,br5,&skiparg,1,&out);
 if(!io_selftest_exception_named(v,"java/lang/IllegalArgumentException"))goto cleanup;
 if(!dvm_call(v,bm,br5,NULL,0,&out)||dvm_exception(v)||!io_selftest_string(v,out.l,line1,sizeof line1-1))goto cleanup;
 if(!dvm_call(v,bread,br5,NULL,0,&out)||dvm_exception(v)||out.i!=0x6F)goto cleanup;
 chars2=dvm_new_array(v,'C',"C",2);
 if(!chars2)goto cleanup;
 range[0].l=chars2;range[1].i=0;range[2].i=1;
 if(!dvm_call(v,brange,br5,range,3,&out)||dvm_exception(v)||out.i!=1)goto cleanup;
 if(((uint16_t*)dvm_array_data(v,chars2))[0]!='k')goto cleanup;
 if(!dvm_call(v,bread,br5,NULL,0,&out)||dvm_exception(v)||out.i!=-1)goto cleanup;
 if(!dvm_call(v,bclose,br5,NULL,0,&out)||dvm_exception(v))goto cleanup;
 ok=true;
cleanup:
 if(dvm_exception(v))dvm_clear_exception(v);
 if(tmp[0])unlink(tmp);
 io_selftest_allow[0]=0;
 return ok;
}
static bool io_selftest_file_equals(const char *path,const void *expect,size_t len){
 int fd=open(path,O_RDONLY|O_CLOEXEC);
 if(fd<0)return false;
 unsigned char buf[64];
 size_t off=0;bool ok=true;
 while(off<sizeof buf){
  ssize_t n=read(fd,buf+off,sizeof buf-off);
  if(n<0&&errno==EINTR)continue;
  if(n<0){ok=false;break;}
  if(n==0)break;
  off+=(size_t)n;
 }
 close(fd);
 return ok&&off==len&&!memcmp(buf,expect,len);
}
/* FileOutputStream: a real descriptor, truncate vs append, the OutputStream
 * static-type interception, close idempotence and sandbox refusals. */
static bool io_selftest_file_output(struct dvm *v){
 bool ok=false,file_runtime_ready=false;
 char dir[PATH_MAX],tmp[PATH_MAX];
 tmp[0]=0;
 struct dvm_class *fos_cls=dvm_find_class_or_external(v,"java/io/FileOutputStream");
 struct dvm_class *file_cls=dvm_find_class_or_external(v,"java/io/File");
 if(!fos_cls)return false;
 if(!io_selftest_cache_dir(v,dir,sizeof dir,&file_runtime_ready))return false;
 if(snprintf(tmp,sizeof tmp,"%s/file-output-XXXXXX",dir)>=(int)sizeof tmp)return false;
 int fd=mkstemp(tmp);
 if(fd<0)return false;
 if(close(fd)){unlink(tmp);return false;}
 char resolved[PATH_MAX];
 io_selftest_allow[0]=0;
 if(realpath(tmp,resolved))(void)snprintf(io_selftest_allow,sizeof io_selftest_allow,"%s",resolved);
 union dvm_value out={0},arg={0},range[3]={{0},{0},{0}},one={0};
 union dvm_value append_arg[2]={{0},{.i=1}};
 dvm_ref fos=0,fos2=0,fos3=0,fos4=0,fos5=0,bad=0,bytes=0,file=0;
 static const unsigned char abcd[]={'a','b','c','d'};
 static const unsigned char xy[]={'X','Y'};
 static const unsigned char xyab[]={'X','Y','a','b'};
 static const unsigned char xyabcd[]={'X','Y','a','b','C','D'};
 static const unsigned char xyabcdz[]={'X','Y','a','b','C','D','Z'};
 static const unsigned char ab[]={'a','b'};
 /* 1. default constructor truncates; write(byte[]) reaches the descriptor */
 fos=dvm_new_object(v,fos_cls);
 if(!fos)goto cleanup;
 arg.l=dvm_new_string(v,tmp);
 if(!arg.l)goto cleanup;
 if(!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;)V",fos,&arg,1,&out)||dvm_exception(v))goto cleanup;
 bytes=dvm_new_array(v,'B',"B",4);
 if(!bytes)goto cleanup;
 memcpy(dvm_array_data(v,bytes),abcd,sizeof abcd);
 arg.l=bytes;
 if(!io_runtime_call(v,"java/io/FileOutputStream","write","([B)V",fos,&arg,1,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileOutputStream","close","()V",fos,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_selftest_file_equals(tmp,abcd,sizeof abcd))goto cleanup;
 /* 2. reopening without append truncates the previous longer content */
 fos2=dvm_new_object(v,fos_cls);
 if(!fos2)goto cleanup;
 arg.l=dvm_new_string(v,tmp);
 if(!arg.l||!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;)V",fos2,&arg,1,&out)||dvm_exception(v))goto cleanup;
 arg.l=dvm_new_array(v,'B',"B",2);
 if(!arg.l)goto cleanup;
 memcpy(dvm_array_data(v,arg.l),xy,sizeof xy);
 if(!io_runtime_call(v,"java/io/FileOutputStream","write","([B)V",fos2,&arg,1,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileOutputStream","close","()V",fos2,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_selftest_file_equals(tmp,xy,sizeof xy))goto cleanup;
 /* 3. append=true keeps prior bytes; write(int) and write([BII) append */
 fos3=dvm_new_object(v,fos_cls);
 if(!fos3)goto cleanup;
 append_arg[0].l=dvm_new_string(v,tmp);
 if(!append_arg[0].l||!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;Z)V",fos3,append_arg,2,&out)||dvm_exception(v))goto cleanup;
 arg.l=dvm_new_array(v,'B',"B",2);
 if(!arg.l)goto cleanup;
 memcpy(dvm_array_data(v,arg.l),ab,sizeof ab);
 if(!io_runtime_call(v,"java/io/FileOutputStream","write","([B)V",fos3,&arg,1,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileOutputStream","close","()V",fos3,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_selftest_file_equals(tmp,xyab,sizeof xyab))goto cleanup;
 fos4=dvm_new_object(v,fos_cls);
 if(!fos4)goto cleanup;
 append_arg[0].l=dvm_new_string(v,tmp);
 if(!append_arg[0].l||!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;Z)V",fos4,append_arg,2,&out)||dvm_exception(v))goto cleanup;
 one.i='C';
 if(!io_runtime_call(v,"java/io/FileOutputStream","write","(I)V",fos4,&one,1,&out)||dvm_exception(v))goto cleanup;
 range[0].l=dvm_new_array(v,'B',"B",2);
 if(!range[0].l)goto cleanup;
 ((uint8_t*)dvm_array_data(v,range[0].l))[0]='D';
 ((uint8_t*)dvm_array_data(v,range[0].l))[1]='!';
 range[1].i=0;range[2].i=1;
 if(!io_runtime_call(v,"java/io/FileOutputStream","write","([BII)V",fos4,range,3,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileOutputStream","flush","()V",fos4,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileOutputStream","close","()V",fos4,NULL,0,&out)||dvm_exception(v))goto cleanup;
 /* close is idempotent; a write after close is IOException */
 if(!io_runtime_call(v,"java/io/FileOutputStream","close","()V",fos4,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_selftest_file_equals(tmp,xyabcd,sizeof xyabcd))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileOutputStream","write","(I)V",fos4,&one,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/io/IOException"))goto cleanup;
 /* 4. a call statically typed as OutputStream reaches the same descriptor */
 fos5=dvm_new_object(v,fos_cls);
 if(!fos5)goto cleanup;
 append_arg[0].l=dvm_new_string(v,tmp);
 if(!append_arg[0].l||!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;Z)V",fos5,append_arg,2,&out)||dvm_exception(v))goto cleanup;
 one.i='Z';
 if(!io_runtime_call(v,"java/io/OutputStream","write","(I)V",fos5,&one,1,&out)||dvm_exception(v))goto cleanup;
 if(!io_runtime_call(v,"java/io/OutputStream","close","()V",fos5,NULL,0,&out)||dvm_exception(v))goto cleanup;
 if(!io_selftest_file_equals(tmp,xyabcdz,sizeof xyabcdz))goto cleanup;
 /* 5. File-backed constructor */
 if(file_runtime_ready&&file_cls){
  file=dvm_new_object(v,file_cls);
  if(!file)goto cleanup;
  arg.l=dvm_new_string(v,tmp);
  if(!arg.l||!file_runtime_call(v,"java/io/File","<init>","(Ljava/lang/String;)V",file,&arg,1,&out)||dvm_exception(v))goto cleanup;
  dvm_ref fos6=dvm_new_object(v,fos_cls);
  if(!fos6)goto cleanup;
  arg.l=file;
  if(!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/io/File;)V",fos6,&arg,1,&out)||dvm_exception(v))goto cleanup;
  arg.l=dvm_new_array(v,'B',"B",2);
  if(!arg.l)goto cleanup;
  memcpy(dvm_array_data(v,arg.l),ab,sizeof ab);
  if(!io_runtime_call(v,"java/io/FileOutputStream","write","([B)V",fos6,&arg,1,&out)||dvm_exception(v))goto cleanup;
  if(!io_runtime_call(v,"java/io/FileOutputStream","close","()V",fos6,NULL,0,&out)||dvm_exception(v))goto cleanup;
  if(!io_selftest_file_equals(tmp,ab,sizeof ab))goto cleanup;
 }
 /* 6. refusals: null, empty, relative, outside the sandbox, uninitialized */
 bad=dvm_new_object(v,fos_cls);
 if(!bad)goto cleanup;
 arg.l=0;
 if(!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/NullPointerException"))goto cleanup;
 arg.l=dvm_new_string(v,"");
 if(!arg.l||!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/IllegalArgumentException"))goto cleanup;
 arg.l=dvm_new_string(v,"relative/out");
 if(!arg.l||!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/io/FileNotFoundException"))goto cleanup;
 arg.l=dvm_new_string(v,"/system/fos-selftest-denied");
 if(!arg.l||!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/io/FileNotFoundException"))goto cleanup;
 arg.l=dvm_new_string(v,"/fos-selftest-missing-parent/file");
 if(!arg.l||!io_runtime_call(v,"java/io/FileOutputStream","<init>","(Ljava/lang/String;)V",bad,&arg,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/io/FileNotFoundException"))goto cleanup;
 if(!io_runtime_call(v,"java/io/FileOutputStream","write","(I)V",bad,&one,1,&out))goto cleanup;
 if(!io_selftest_exception_named(v,"java/lang/IllegalStateException"))goto cleanup;
 ok=true;
cleanup:
 if(dvm_exception(v))dvm_clear_exception(v);
 if(tmp[0])unlink(tmp);
 io_selftest_allow[0]=0;
 return ok;
}
static unsigned os_probe_calls,os_probe_sum;
static bool os_probe_write(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)v;(void)self;(void)out;
 if(n!=1)return false;
 ++os_probe_calls;os_probe_sum+=(unsigned)(a[0].i&0xff);return true;
}
bool io_runtime_selftest(struct dvm *v){
 union dvm_value out={0},arg={0};
 dvm_ref sw=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/StringWriter"));
 if(!sw||!io_runtime_call(v,"java/io/StringWriter","<init>","()V",sw,NULL,0,&out))return false;
 dvm_ref pw=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/PrintWriter"));
 arg.l=sw;if(!io_runtime_call(v,"java/io/PrintWriter","<init>","(Ljava/io/Writer;)V",pw,&arg,1,&out))return false;
 arg.l=dvm_new_string(v,"abc");
 if(!io_runtime_call(v,"java/io/PrintWriter","print","(Ljava/lang/String;)V",pw,&arg,1,&out))return false;
 if(!io_runtime_call(v,"java/io/StringWriter","toString","()Ljava/lang/String;",sw,NULL,0,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"abc"))return false;
 dvm_ref exc=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/RuntimeException"));
 arg.l=pw;
 if(!io_runtime_call(v,"java/lang/Throwable","printStackTrace","(Ljava/io/PrintWriter;)V",exc,&arg,1,&out))return false;
 if(!io_runtime_call(v,"java/io/StringWriter","toString","()Ljava/lang/String;",sw,NULL,0,&out)||!out.l||dvm_string_utf8_length(v,out.l)<=3)return false;
 /* java/io/OutputStream: constructor, no-op flush/close, byte[] defaults and
  * the abstract write(int) boundary. */
 struct dvm_class *os_cls=dvm_find_class_or_external(v,"java/io/OutputStream");
 dvm_ref os=dvm_new_object(v,os_cls);
 if(!os||!io_runtime_call(v,"java/io/OutputStream","<init>","()V",os,NULL,0,&out))return false;
 if(dvm_exception(v))return false;
 if(!io_runtime_call(v,"java/io/OutputStream","flush","()V",os,NULL,0,&out)||
    !io_runtime_call(v,"java/io/OutputStream","close","()V",os,NULL,0,&out))return false;
 /* No implementation for write(int) on the base: fail closed, no exception. */
 if(io_runtime_call(v,"java/io/OutputStream","write","(I)V",os,&arg,1,&out))return false;
 dvm_ref bytes=dvm_new_array(v,'B',"B",3);
 if(!bytes)return false;
 ((uint8_t*)dvm_array_data(v,bytes))[0]=1;
 ((uint8_t*)dvm_array_data(v,bytes))[1]=2;
 ((uint8_t*)dvm_array_data(v,bytes))[2]=3;
 /* An empty byte[] is a valid no-op even with no write(int) override. */
 dvm_ref empty=dvm_new_array(v,'B',"B",0);
 if(!empty)return false;
 arg.l=empty;
 if(!io_runtime_call(v,"java/io/OutputStream","write","([B)V",os,&arg,1,&out))return false;
 if(dvm_exception(v))return false;
 /* A non-empty byte[] cannot reach an override on the raw base. */
 arg.l=bytes;
 if(io_runtime_call(v,"java/io/OutputStream","write","([B)V",os,&arg,1,&out))return false;
 if(dvm_exception(v))return false;
 /* Null array is a NullPointerException, not a silent miss. */
 arg.l=0;
 if(!io_runtime_call(v,"java/io/OutputStream","write","([B)V",os,&arg,1,&out))return false;
 struct dvm_class *exc_cls=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 if(!exc_cls||strcmp(exc_cls->name,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 /* Offsets outside the array are IndexOutOfBoundsException. */
 union dvm_value range[3]={{.l=bytes},{.i=2},{.i=2}};
 if(!io_runtime_call(v,"java/io/OutputStream","write","([BII)V",os,range,3,&out))return false;
 exc_cls=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 if(!exc_cls||strcmp(exc_cls->name,"java/lang/IndexOutOfBoundsException"))return false;
 dvm_clear_exception(v);
 range[1].i=-1;range[2].i=1;
 if(!io_runtime_call(v,"java/io/OutputStream","write","([BII)V",os,range,3,&out))return false;
 exc_cls=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 if(!exc_cls||strcmp(exc_cls->name,"java/lang/IndexOutOfBoundsException"))return false;
 dvm_clear_exception(v);
 /* Empty range at the end is legal and calls no write(int). */
 range[1].i=3;range[2].i=0;
 if(!io_runtime_call(v,"java/io/OutputStream","write","([BII)V",os,range,3,&out))return false;
 if(dvm_exception(v))return false;
 /* Unknown signatures stay unhandled. */
 union dvm_value unknown={.l=bytes};
 if(io_runtime_call(v,"java/io/OutputStream","write","([C)V",os,&unknown,1,&out))return false;
 if(io_runtime_call(v,"java/io/OutputStream","read","()I",os,NULL,0,&out))return false;
 /* A minimal guest subclass: the byte[] default must reach its write(int). */
 struct dvm_class *os_probe=dvm__register_builtin(v,"Lsdktrace/tests/OutputStreamProbe;");
 if(!os_probe)return false;
 os_probe->super=os_cls;
 os_probe->methods=calloc(1,sizeof *os_probe->methods);
 if(!os_probe->methods)return false;
 os_probe->nmethods=1;
 os_probe->methods[0].cls=os_probe;os_probe->methods[0].name="write";
 os_probe->methods[0].sig=strdup("(I)V");
 os_probe->methods[0].builtin=os_probe_write;
 os_probe->methods[0].ret_kind='V';os_probe->methods[0].arg_count=1;os_probe->methods[0].arg_slots=1;
 if(!os_probe->methods[0].sig)return false;
 dvm_ref probe_obj=dvm_new_object(v,os_probe);
 if(!probe_obj||!io_runtime_call(v,"java/io/OutputStream","<init>","()V",probe_obj,NULL,0,&out))return false;
 os_probe_calls=0;os_probe_sum=0;
 arg.l=bytes;
 if(!io_runtime_call(v,"java/io/OutputStream","write","([B)V",probe_obj,&arg,1,&out))return false;
 if(dvm_exception(v)||os_probe_calls!=3||os_probe_sum!=6)return false;
 if(dvm_exception(v))return false;
 if(!io_selftest_file_output(v))return false;
 if(!io_selftest_readers(v))return false;
 sdk_trace_fprintf(stderr,"IO_RUNTIME_SELFTEST PASS StringWriter=1 PrintWriter=1 throwable_stack=1 OutputStream=1 FileOutputStream=1 FileReader=1 FileReader_UTF8=1 FileReader_EOF=1 FileReader_errors=1 BufferedReader=1 BufferedReader_builtin=1\n");
 return true;
}
