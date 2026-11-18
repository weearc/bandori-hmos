#include "dvm/dvm_internal.h"
#include "gc_operation.h"
#include "io_runtime.h"
#include "java_basics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include "frame_io_probe.h"
extern int sdk_trace_fprintf(FILE*,const char*,...);
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
/* All pointer-returning peers below require an operation-local native scope.
 * Direct host callers receive one here; normal DVM dispatch reuses its scope.
 * A result is transferred before an owned scope is left. */
struct io_scope_guard {struct dvm_gc_native_scope scope;bool owned;};
static bool io_scope_begin(struct dvm *v,struct io_scope_guard *g,dvm_ref self){
 memset(g,0,sizeof *g);if(dvm_gc_payload_scope_active(v))return true;
 g->owned=true;return dvm_gc_native_enter(v,&g->scope,self,"()V",NULL,0);
}
static void io_scope_cleanup(struct io_scope_guard *g){if(g->owned)dvm_gc_native_leave(&g->scope);}
static void *io_payload_of(struct dvm *v,dvm_ref ref,void (*destroy)(void *)){
 struct dvm_object *o=dvm__obj(v,ref);if(!o||o->data_destroy!=destroy)return NULL;
 if(!dvm_gc_require_payload_borrow(v,ref))return NULL;
 /* Re-resolve after admission: the only returned pointer is scope-borrowed. */
 o=dvm__obj(v,ref);return o?o->data:NULL;
}

typedef struct {uint16_t *buf;size_t len,cap;} sw_data;
typedef struct {struct dvm *vm;dvm_ref writer;bool output,auto_flush,trouble,closed;uint16_t high;} pw_data;
/* A real guest byte[] is the backing store. writeTo exposes that same array,
 * as Java does; guest overrides may retain/mutate it. A grow creates a new
 * backing array but never frees an old array that has escaped to guest code. */
/* Persist guest identity only. Every operation resolves its current payload;
 * native call scopes protect operation-local pointers across guest allocation. */
typedef struct {struct dvm *vm;dvm_ref array;size_t len,cap;bool exposed;} baos_data;
static void baos_free(void *p){baos_data *b=p;if(b){if(b->array){dvm_unpin(b->vm,b->array);if(!b->exposed)dvm__discard_temporary(b->vm,b->array);}free(b);}}
static baos_data *baos_of(struct dvm *v,dvm_ref ref){return io_payload_of(v,ref,baos_free);}
static unsigned char *baos_buffer(struct dvm *v,baos_data *b){
 struct dvm_object *array=b?dvm__obj(v,b->array):NULL;
 if(!array||array->kind!=DVM_OBJ_ARRAY||array->elem_kind!='B'||
    array->length!=b->cap||b->len>b->cap||!array->data){
  dvm__throw(v,"java/lang/IllegalStateException","ByteArrayOutputStream backing unavailable");return NULL;
 }
 if(!dvm_gc_require_payload_borrow(v,b->array))return NULL;
 return dvm_array_data(v,b->array);
}
static bool baos_append(struct dvm *v,baos_data *b,const unsigned char *data,size_t n){
 if(n>INT32_MAX-b->len){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return false;}
 size_t need=b->len+n;
 if(need>b->cap){
  size_t cap=b->cap?b->cap:1;while(cap<need)cap=cap>INT32_MAX/2?need:cap*2;
  dvm_ref array=dvm_new_array(v,'B',"B",(uint32_t)cap);if(!array)return false;
  dvm_pin(v,array);unsigned char *grown=dvm_array_data(v,array);
  unsigned char *previous_data=baos_buffer(v,b);
  if(!grown||!previous_data){dvm_unpin(v,array);dvm__discard_temporary(v,array);return false;}
  if(b->len)memcpy(grown,previous_data,b->len);
  /* Incoming data can alias the previous backing (writeTo(this)); finish
   * both copies before releasing the former, unexposed backing. */
  if(n)memmove(grown+b->len,data,n);
  dvm_ref previous=b->array;bool exposed=b->exposed;
  b->array=array;b->cap=cap;b->len=need;b->exposed=false;
  if(previous){dvm_unpin(v,previous);if(!exposed)dvm__discard_temporary(v,previous);}
  return true;
 }
 unsigned char *buffer=baos_buffer(v,b);if(!buffer)return false;
 if(n)memmove(buffer+b->len,data,n);b->len=need;return true;
}
static struct dvm *active;
typedef struct {struct dvm *vm;dvm_ref array;} bais_data;
static void bais_free(void *p){bais_data *s=p;if(s){dvm_unpin(s->vm,s->array);free(s);}}
static void bais_int(struct dvm *v,dvm_ref self,const char *name,int32_t value){union dvm_value q={.i=value};dvm_set_field(v,self,name,"I",q);}
static bool bais_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","ByteArrayInputStream receiver");return true;}
 bool ctor=IS("<init>","([B)V")||IS("<init>","([BII)V");
 if(ctor&&(n==1||n==3)){
  struct dvm_object *array=dvm__obj(v,a[0].l);
  if(!array){dvm__throw(v,"java/lang/NullPointerException","ByteArrayInputStream buffer");return true;}
  if(array->kind!=DVM_OBJ_ARRAY||array->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","ByteArrayInputStream byte buffer");return true;}
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","ByteArrayInputStream already initialized");return true;}
  bais_data *s=calloc(1,sizeof *s);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayInputStream");return true;}
  s->vm=v;s->array=a[0].l;dvm_pin(v,s->array);o->data=s;o->data_destroy=bais_free;
  union dvm_value q={.l=s->array};dvm_set_field(v,self,"buf","[B",q);
  int32_t pos=n==3?a[1].i:0;
  /* Match Java's int addition and min, including overflow; constructors do
   * not invent range validation absent from ByteArrayInputStream. */
  int32_t count=n==3?(int32_t)((uint32_t)pos+(uint32_t)a[2].i):(int32_t)array->length;
  if(count>(int32_t)array->length)count=(int32_t)array->length;
  bais_int(v,self,"pos",pos);bais_int(v,self,"mark",pos);bais_int(v,self,"count",count);return true;
 }
 if(o->data_destroy!=bais_free)return false;
 if(IS("close","()V"))return true; /* Java close leaves this stream usable. */
 if(IS("markSupported","()Z")){out->i=1;return true;}
 union dvm_value p={0},count={0},buf={0},mark={0};
 if(!dvm_get_field(v,self,"pos","I",&p)||!dvm_get_field(v,self,"count","I",&count)||!dvm_get_field(v,self,"buf","[B",&buf))return true;
 if(IS("available","()I")){out->i=(int32_t)((uint32_t)count.i-(uint32_t)p.i);return true;}
 if(IS("mark","(I)V")&&n==1){bais_int(v,self,"mark",p.i);return true;}
 if(IS("reset","()V")){if(dvm_get_field(v,self,"mark","I",&mark))bais_int(v,self,"pos",mark.i);return true;}
 if(IS("skip","(J)J")&&n==1){int64_t left=(int32_t)((uint32_t)count.i-(uint32_t)p.i),k=a[0].j;if(k>left)k=left;if(k<0)k=0;bais_int(v,self,"pos",(int32_t)((uint32_t)p.i+(uint32_t)k));out->j=k;return true;}
 bool one=IS("read","()I"),whole=IS("read","([B)I"),range=IS("read","([BII)I");
 if(!one&&!whole&&!range)return false;
 int32_t off=0,len=1;struct dvm_object *dst=NULL;
 if(!one){
  dst=dvm__obj(v,a[0].l);if(!dst){dvm__throw(v,"java/lang/NullPointerException","ByteArrayInputStream destination");return true;}
  if(dst->kind!=DVM_OBJ_ARRAY||dst->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","ByteArrayInputStream destination");return true;}
  off=whole?0:a[1].i;len=whole?(int32_t)dst->length:a[2].i;
  if(off<0||len<0||(uint32_t)off>dst->length||(uint32_t)len>dst->length-(uint32_t)off){dvm__throw(v,"java/lang/IndexOutOfBoundsException","ByteArrayInputStream destination range");return true;}
 }
 if(p.i>=count.i){out->i=-1;return true;}
 if(!len){out->i=0;return true;}
 int64_t left=(int64_t)count.i-p.i;if(len>left)len=(int32_t)left;
 struct dvm_object *source=dvm__obj(v,buf.l);
 if(!source){dvm__throw(v,"java/lang/NullPointerException","ByteArrayInputStream source");return true;}
 if(p.i<0||(uint64_t)(uint32_t)p.i+(uint32_t)len>source->length){dvm__throw(v,"java/lang/ArrayIndexOutOfBoundsException","ByteArrayInputStream source range");return true;}
 uint8_t *data=dvm_array_data(v,buf.l);
 if(one)out->i=data[p.i];else{memmove((uint8_t*)dvm_array_data(v,a[0].l)+off,data+p.i,(size_t)len);out->i=len;}
 bais_int(v,self,"pos",(int32_t)((uint32_t)p.i+(uint32_t)len));return true;
}
extern bool string_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
typedef struct {struct dvm *vm;dvm_ref text;int32_t position,length,mark;bool closed;} sr_data;
static void sr_free(void *data){sr_data *r=data;if(r){dvm_unpin(r->vm,r->text);free(r);}}
static bool sr_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","StringReader receiver");return true;}
 if(IS("<init>","(Ljava/lang/String;)V")&&n==1){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","StringReader text");return true;}
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","StringReader already initialized");return true;}
  union dvm_value length={0};if(!string_runtime_call(v,"java/lang/String","length","()I",a[0].l,NULL,0,&length)||dvm_exception(v))return true;
  sr_data *r=calloc(1,sizeof *r);if(!r){dvm__throw(v,"java/lang/OutOfMemoryError","StringReader");return true;}r->vm=v;r->text=a[0].l;r->length=length.i;dvm_pin(v,r->text);o->data=r;o->data_destroy=sr_free;return true;
 }
 sr_data *r=o->data_destroy==sr_free?o->data:NULL;if(!r)return false;
 if(IS("close","()V")){r->closed=true;return true;}
 if(IS("markSupported","()Z")){out->i=1;return true;}
 if(r->closed){dvm__throw(v,"java/io/IOException","StringReader closed");return true;}
 if(IS("ready","()Z")){out->i=1;return true;}
 if(IS("mark","(I)V")&&n==1){if(a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative readAheadLimit");return true;}r->mark=r->position;return true;}
 if(IS("reset","()V")){r->position=r->mark;return true;}
 if(IS("skip","(J)J")&&n==1){int64_t amount=a[0].j;if(r->position>=r->length){out->j=0;return true;}if(amount>r->length-r->position)amount=r->length-r->position;if(amount<-(int64_t)r->position)amount=-r->position;r->position+=(int32_t)amount;out->j=amount;return true;}
 if(IS("read","()I")){if(r->position>=r->length){out->i=-1;return true;}union dvm_value index={.i=r->position};if(string_runtime_call(v,"java/lang/String","charAt","(I)C",r->text,&index,1,out)&&!dvm_exception(v))r->position++;return true;}
 bool whole=IS("read","([C)I"),range=IS("read","([CII)I");if((whole&&n==1)||(range&&n==3)){
  struct dvm_object *array=dvm__obj(v,a[0].l);if(!array){dvm__throw(v,"java/lang/NullPointerException","StringReader buffer");return true;}
  if(array->kind!=DVM_OBJ_ARRAY||array->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","StringReader char buffer");return true;}
  int32_t offset=whole?0:a[1].i,length=whole?(int32_t)array->length:a[2].i;
  if(offset<0||length<0||(uint64_t)(uint32_t)offset+(uint32_t)length>array->length){dvm__throw(v,"java/lang/IndexOutOfBoundsException","StringReader range");return true;}
  if(!length){out->i=0;return true;}if(r->position>=r->length){out->i=-1;return true;}if(length>r->length-r->position)length=r->length-r->position;
  for(int32_t i=0;i<length;i++){union dvm_value index={.i=r->position+i},unit={0};if(!string_runtime_call(v,"java/lang/String","charAt","(I)C",r->text,&index,1,&unit)||dvm_exception(v))return true;
   /* charAt can reenter/yield. Never retain the destination payload over it. */
   uint16_t *data=dvm_array_data(v,a[0].l);if(!data)return true;data[offset+i]=(uint16_t)unit.i;
  }r->position+=length;out->i=length;return true;
 }
 return false;
}
static void sw_free(void *p){sw_data*s=p;if(s){free(s->buf);free(s);}}
static void pw_free(void *p){pw_data*w=p;if(w){dvm_unpin(w->vm,w->writer);free(w);}}
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
static fr_data *fr_of(struct dvm *v,dvm_ref ref){return io_payload_of(v,ref,fr_free);}
static br_data *br_of(struct dvm *v,dvm_ref ref){return io_payload_of(v,ref,br_free);}

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
static fos_data *fos_of(struct dvm *v,dvm_ref ref){return io_payload_of(v,ref,fos_free);}
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

/* InputStreamReader owns its input reference, not a snapshot of its bytes.
 * Decode incrementally, including streams that yield one byte per read. */
typedef struct {struct dvm *vm;dvm_ref input;int pushed,low;bool closed;} isr_data;
static void isr_free(void *p){isr_data *s=p;if(s){dvm_unpin(s->vm,s->input);free(s);}}
static bool isr_input(struct dvm *v,isr_data *s,const char *name,const char *sig,union dvm_value *q){
 struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,s->input),name,sig);
 if(!m){dvm__throw(v,"java/lang/NoSuchMethodError","InputStream.%s%s",name,sig);return false;}
 return dvm_call(v,m,s->input,NULL,0,q)&&!dvm_exception(v);
}
static bool isr_byte(struct dvm *v,isr_data *s,int *out){
 if(s->pushed>=0){*out=s->pushed;s->pushed=-1;return true;}
 union dvm_value q={0};if(!isr_input(v,s,"read","()I",&q))return false;*out=q.i;return true;
}
static bool isr_unit(struct dvm *v,isr_data *s,int *out){
 if(s->closed){dvm__throw(v,"java/io/IOException","Stream closed");return false;}
 if(s->low>=0){*out=s->low;s->low=-1;return true;}
 int b;if(!isr_byte(v,s,&b))return false;
 if(b<128){*out=b;return true;}
 int count=b>=0xC2&&b<=0xDF?2:b>=0xE0&&b<=0xEF?3:b>=0xF0&&b<=0xF4?4:0;
 if(!count){*out=0xFFFD;return true;}
 uint32_t cp=b&((1u<<(7-count))-1);
 for(int i=1;i<count;i++){
  int next;if(!isr_byte(v,s,&next))return false;
  if(next<0){*out=0xFFFD;return true;}
  /* Overlong prefixes and >U+10FFFF consume only their lead byte. A
   * UTF-8 encoded surrogate consumes its complete three-byte sequence. */
  if((next&0xC0)!=0x80||(i==1&&((b==0xE0&&next<0xA0)||(b==0xF0&&next<0x90)||(b==0xF4&&next>0x8F)))){s->pushed=next;*out=0xFFFD;return true;}
  cp=(cp<<6)|(next&63);
 }
 if(cp>=0xD800&&cp<=0xDFFF){*out=0xFFFD;return true;}
 if(cp>0xFFFF){cp-=0x10000;s->low=0xDC00+(cp&1023);*out=0xD800+(cp>>10);}else *out=(int)cp;
 return true;
}
static bool isr_dispatch(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","InputStreamReader receiver");return true;}
 bool ctor=IS("<init>","(Ljava/io/InputStream;)V")||IS("<init>","(Ljava/io/InputStream;Ljava/lang/String;)V")||IS("<init>","(Ljava/io/InputStream;Ljava/nio/charset/Charset;)V");
 if(ctor){
  if(!n||!a||!a[0].l||(n==2&&!a[1].l)){dvm__throw(v,"java/lang/NullPointerException","InputStreamReader input/charset");return true;}
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","InputStreamReader already initialized");return true;}
  if(n==2){
   dvm_ref name=a[1].l;
   if(strstr(sig,"Ljava/nio/charset/Charset;")){struct dvm_method *method=dvm_find_method(v,dvm_object_class(v,name),"name","()Ljava/lang/String;");union dvm_value q={0};if(!method){dvm__throw(v,"java/lang/NoSuchMethodError","Charset.name");return true;}if(!dvm_call(v,method,name,NULL,0,&q)||dvm_exception(v))return true;name=q.l;}
   const char *encoding=dvm_string_utf8(v,name);
   if(!encoding||(strcasecmp(encoding,"UTF-8")&&strcasecmp(encoding,"UTF8")&&strcasecmp(encoding,"unicode-1-1-utf-8"))){dvm__throw(v,"java/io/UnsupportedEncodingException","InputStreamReader charset");return true;}
  }
  isr_data *s=calloc(1,sizeof *s);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","InputStreamReader");return true;}
  s->vm=v;s->input=a[0].l;s->pushed=s->low=-1;dvm_pin(v,s->input);
  /* Charset.name may allocate; reacquire the object-table entry afterwards. */
  o=dvm__obj(v,self);o->data=s;o->data_destroy=isr_free;return true;
 }
 isr_data *s=o->data_destroy==isr_free?o->data:NULL;if(!s)return false;
 if(IS("getEncoding","()Ljava/lang/String;")){out->l=s->closed?0:dvm_new_string(v,"UTF8");return true;}
 if(IS("close","()V")){if(!s->closed){s->closed=true;union dvm_value q={0};(void)isr_input(v,s,"close","()V",&q);}return true;}
 if(s->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
 if(IS("ready","()Z")){if(s->pushed>=0||s->low>=0){out->i=1;return true;}union dvm_value q={0};if(isr_input(v,s,"available","()I",&q))out->i=q.i>0;return true;}
 if(IS("read","()I")){int unit;if(isr_unit(v,s,&unit))out->i=unit;return true;}
 bool whole=IS("read","([C)I"),range=IS("read","([CII)I");
 if(whole||range){
  struct dvm_object *array=dvm__obj(v,a[0].l);int off=whole?0:a[1].i,len=whole?(array?(int)array->length:0):a[2].i;bool valid=false;
  array=fr_char_array(v,a[0].l,off,len,&valid);if(!valid)return true;
  if(!len){out->i=0;return true;}int got=0;
  while(got<len){int unit;if(!isr_unit(v,s,&unit))return true;if(unit<0)break;((uint16_t*)dvm_array_data(v,a[0].l))[off+got++]=(uint16_t)unit;
   /* Do not wait to fill the caller's array after producing a character. */
   if(got<len&&s->low<0&&s->pushed<0){union dvm_value q={0};if(!isr_input(v,s,"available","()I",&q))return true;if(q.i<=0)break;}
  }
  out->i=got?got:-1;return true;
 }
 return false;
}
/* ---- BufferedReader over a FileReader or native InputStreamReader ---- */
static bool br_child_read(struct dvm *v,br_data *b,int *out){
 struct dvm_object *child=b->reader?dvm__obj(v,b->reader):NULL;
 if(child&&child->data_destroy==isr_free)return isr_unit(v,child->data,out);
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
 if(child&&child->data_destroy==isr_free){union dvm_value q={0};if(!isr_dispatch(v,"ready","()Z",b->reader,NULL,0,&q)||dvm_exception(v))return false;*out=q.i;return true;}
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
  if(child->data_destroy==fr_free||child->data_destroy==isr_free){
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
  if(child&&child->data_destroy==isr_free){union dvm_value q={0};(void)isr_dispatch(v,"close","()V",b->reader,NULL,0,&q);}
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
static bool isr_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;

 struct dvm_method *m=dvm__builtin_method();
 const dvm_ref gc5_ref_o=(self); if(!gc_operation_admit(v,gc5_ref_o))return false;
 /* FileReader extends InputStreamReader but owns a different native peer. */
 if(m&&dvm__obj(v,gc5_ref_o)&&dvm__obj(v,gc5_ref_o)->data_destroy==fr_free)return io_runtime_call(v,"java/io/FileReader",m->name,m->sig,self,a,n,out);
 if(m&&isr_dispatch(v,m->name,m->sig,self,a,n,out))return true;
 dvm__throw(v,"java/lang/UnsupportedOperationException","InputStreamReader dispatch");return true;
}
static void isr_methods_install(struct dvm *v){
 struct dvm_class *c=dvm_find_class_or_external(v,"java/io/InputStreamReader");if(!c||!c->external||c->nmethods)return;
 static const struct {const char *name,*sig;char ret;int args;} defs[]={
  {"<init>","(Ljava/io/InputStream;)V",'V',1},{"<init>","(Ljava/io/InputStream;Ljava/lang/String;)V",'V',2},
  {"<init>","(Ljava/io/InputStream;Ljava/nio/charset/Charset;)V",'V',2},{"read","()I",'I',0},
  {"read","([C)I",'I',1},{"read","([CII)I",'I',3},{"ready","()Z",'Z',0},{"close","()V",'V',0},{"getEncoding","()Ljava/lang/String;",'L',0}};
 size_t count=sizeof defs/sizeof defs[0];struct dvm_method *methods=calloc(count,sizeof *methods);if(!methods)return;
 for(size_t i=0;i<count;i++){methods[i].cls=c;methods[i].name=defs[i].name;methods[i].sig=strdup(defs[i].sig);if(!methods[i].sig){for(size_t j=0;j<i;j++)free(methods[j].sig);free(methods);return;}methods[i].builtin=isr_builtin;methods[i].ret_kind=defs[i].ret;methods[i].arg_count=methods[i].arg_slots=defs[i].args;}
 c->methods=methods;c->nmethods=(int)count;
}
static bool pw_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *q){
 struct dvm_method *m=dvm__builtin_method();
 return m&&io_runtime_call(v,"java/io/PrintWriter",m->name,m->sig,self,a,n,q);
}
static void pw_methods_install(struct dvm *v){
 struct dvm_class *c=dvm_find_class_or_external(v,"java/io/PrintWriter");if(!c||!c->external||c->nmethods)return;
 static const struct {const char *name,*sig;} defs[]={
  {"<init>","(Ljava/io/Writer;)V"},{"<init>","(Ljava/io/Writer;Z)V"},{"<init>","(Ljava/io/OutputStream;)V"},{"<init>","(Ljava/io/OutputStream;Z)V"},
  {"write","(I)V"},{"write","([C)V"},{"write","([CII)V"},{"write","(Ljava/lang/String;)V"},{"write","(Ljava/lang/String;II)V"},
  {"print","(Ljava/lang/String;)V"},{"print","(Ljava/lang/Object;)V"},{"print","([C)V"},{"print","(I)V"},{"print","(J)V"},{"print","(Z)V"},{"print","(C)V"},{"print","(F)V"},{"print","(D)V"},
  {"println","()V"},{"println","(Ljava/lang/String;)V"},{"println","(Ljava/lang/Object;)V"},{"println","([C)V"},{"println","(I)V"},{"println","(J)V"},{"println","(Z)V"},{"println","(C)V"},{"println","(F)V"},{"println","(D)V"},
  {"flush","()V"},{"close","()V"},{"checkError","()Z"},
  {"append","(Ljava/lang/CharSequence;)Ljava/io/PrintWriter;"},{"append","(C)Ljava/io/PrintWriter;"}};
 size_t count=sizeof defs/sizeof defs[0];struct dvm_method *methods=calloc(count,sizeof *methods);if(!methods)return;
 for(size_t i=0;i<count;i++){
  methods[i].cls=c;methods[i].name=defs[i].name;methods[i].sig=strdup(defs[i].sig);
  if(!methods[i].sig){for(size_t j=0;j<i;j++)free(methods[j].sig);free(methods);return;}
  methods[i].builtin=pw_builtin;methods[i].access=1;methods[i].ret_kind=dvm_sig_return_kind(defs[i].sig);
  methods[i].arg_count=dvm_sig_arg_count(defs[i].sig);methods[i].arg_slots=dvm_sig_arg_slots(defs[i].sig);
 }
 c->methods=methods;c->nmethods=(int)count;
}

void io_runtime_vm_start(struct dvm *v){
 active=v;io_sandbox_state=0;io_sandbox_base[0]=0;io_selftest_allow[0]=0;
 struct dvm_class *bais=dvm_find_class_or_external(v,"java/io/ByteArrayInputStream");
 struct dvm_class *base_input=dvm_find_class_or_external(v,"java/io/InputStream");
 struct dvm_class *file_input=dvm_find_class_or_external(v,"java/io/FileInputStream");
 if(file_input&&file_input->external)file_input->super=base_input;
 if(bais&&bais->external){
  bais->super=base_input;
  if(!bais->ifields){
   struct dvm_field *fields=calloc(4,sizeof *fields);
   if(fields){const char *names[]={"buf","pos","mark","count"};uint32_t start=base_input?base_input->islots:0;
    for(int i=0;i<4;i++){fields[i].cls=bais;fields[i].name=names[i];fields[i].type=i?"I":"[B";fields[i].kind=i?'I':'L';fields[i].width=1;fields[i].access=4;fields[i].slot=start+i;}
    bais->ifields=fields;bais->nifields=4;bais->islots=start+4;
   }
  }
 }
 struct dvm_class *writer=dvm_find_class_or_external(v,"java/io/Writer");
 struct dvm_class *reader=dvm_find_class_or_external(v,"java/io/Reader");
 if(reader&&reader->external&&!reader->ifields){
  struct dvm_field *field=calloc(1,sizeof *field);
  if(field){field->cls=reader;field->name="lock";field->type="Ljava/lang/Object;";field->kind='L';field->width=1;field->access=4;field->slot=reader->super?reader->super->islots:0;reader->ifields=field;reader->nifields=1;reader->islots=field->slot+1;}
 }
 struct dvm_class *input=dvm_find_class_or_external(v,"java/io/InputStreamReader");
 struct dvm_class *file_reader=dvm_find_class_or_external(v,"java/io/FileReader");
 struct dvm_class *buffered=dvm_find_class_or_external(v,"java/io/BufferedReader");
 struct dvm_class *sw=dvm_find_class_or_external(v,"java/io/StringWriter");
 struct dvm_class *pw=dvm_find_class_or_external(v,"java/io/PrintWriter");
 struct dvm_class *output=dvm_find_class_or_external(v,"java/io/OutputStream");
 struct dvm_class *file_output=dvm_find_class_or_external(v,"java/io/FileOutputStream");
  struct dvm_class *byte_output=dvm_find_class_or_external(v,"java/io/ByteArrayOutputStream");
 if(writer&&sw&&sw->external)sw->super=writer;
 if(writer&&pw&&pw->external)pw->super=writer;
 if(reader&&input&&input->external)input->super=reader;
 if(reader&&file_reader&&file_reader->external)file_reader->super=input&&input->external?input:reader;
 if(reader&&buffered&&buffered->external)buffered->super=reader;
 struct dvm_class *string_reader=dvm_find_class_or_external(v,"java/io/StringReader");if(reader&&string_reader&&string_reader->external)string_reader->super=reader;
 struct dvm_class *readers[]={input,file_reader,buffered,string_reader};
 for(int i=0;i<4;i++)if(reader&&readers[i]&&readers[i]->external&&readers[i]->islots<reader->islots)readers[i]->islots=reader->islots;
 if(output&&file_output&&file_output->external)file_output->super=output;
  if(output&&byte_output&&byte_output->external)byte_output->super=output;
 struct dvm_class *ca_reader=dvm_find_class_or_external(v,"java/io/CharArrayReader"),*ca_writer=dvm_find_class_or_external(v,"java/io/CharArrayWriter");
 if(ca_reader&&ca_reader->external&&reader){ca_reader->super=reader;if(ca_reader->islots<reader->islots)ca_reader->islots=reader->islots;}
 if(ca_writer&&ca_writer->external&&writer){ca_writer->super=writer;if(ca_writer->islots<writer->islots)ca_writer->islots=writer->islots;}
 (void)br_methods_install(v);
 isr_methods_install(v);
  pw_methods_install(v);
}
static sw_data *sw_of(struct dvm *v,dvm_ref ref){
 return io_payload_of(v,ref,sw_free);
}
static pw_data *pw_of(struct dvm *v,dvm_ref ref){
 return io_payload_of(v,ref,pw_free);
}
/* UTF-16 storage preserves code units, including split surrogate writes. */
static bool io_units_append(struct dvm *v,sw_data *s,const uint16_t *units,size_t n){
 if(n>INT32_MAX-s->len){dvm__throw(v,"java/lang/OutOfMemoryError","character writer size");return false;}
 size_t need=s->len+n;if(need>s->cap){size_t cap=s->cap?s->cap:32;while(cap<need)cap=cap>INT32_MAX/2?need:cap*2;uint16_t *q=realloc(s->buf,cap*sizeof *q);if(!q){dvm__throw(v,"java/lang/OutOfMemoryError","character writer");return false;}s->buf=q;s->cap=cap;}
 if(n)memcpy(s->buf+s->len,units,n*sizeof *units);s->len=need;return true;
}
static dvm_ref io_units_array(struct dvm *v,const uint16_t *units,size_t n){dvm_ref a=dvm_new_array(v,'C',"C",(uint32_t)n);if(a&&n)memcpy(dvm_array_data(v,a),units,n*sizeof *units);return a;}
static dvm_ref io_units_string(struct dvm *v,const uint16_t *units,size_t n){
 dvm_ref a=io_units_array(v,units,n);if(!a)return 0;dvm_pin(v,a);dvm_ref text=dvm_new_string(v,"");union dvm_value q={0},arg={.l=a};if(text&&!string_runtime_call(v,"java/lang/String","<init>","([C)V",text,&arg,1,&q))text=0;dvm_unpin(v,a);dvm__discard_temporary(v,a);return dvm_exception(v)?0:text;
}
static bool io_text_units(struct dvm *v,dvm_ref text,dvm_ref *array){union dvm_value q={0};if(!string_runtime_call(v,"java/lang/String","toCharArray","()[C",text,NULL,0,&q)||dvm_exception(v))return false;*array=q.l;return q.l!=0;}
static bool append_bytes(struct dvm *v,dvm_ref sw,const char *bytes,size_t n){
 sw_data *s=sw_of(v,sw);if(!s){dvm__throw(v,"java/lang/IllegalStateException","StringWriter not initialized");return false;}
 dvm_ref text=dvm_new_string_n(v,bytes,n),array=0;if(!text)return false;dvm_pin(v,text);bool ok=io_text_units(v,text,&array);if(ok){struct dvm_object *a=dvm__obj(v,array);ok=io_units_append(v,s,dvm_array_data(v,array),a->length);}if(array)dvm__discard_temporary(v,array);dvm_unpin(v,text);dvm__discard_temporary(v,text);return ok;
}
static bool append_text(struct dvm *v,dvm_ref pw,const char *bytes,size_t n){
 pw_data *w=pw_of(v,pw);if(!w){dvm__throw(v,"java/lang/IllegalStateException","PrintWriter not initialized");return false;}
 if(w->closed){w->trouble=true;return false;}
 if(w->output&&w->high&&n){w->high=0;if(!append_text(v,pw,"?",1))return false;}
 if(sw_of(v,w->writer))return append_bytes(v,w->writer,bytes,n);
 if(n>INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","PrintWriter write length");return false;}
 union dvm_value a[3]={{0}},q={0};const char *sig;dvm_ref temporary=0;
 if(w->output){
  if(baos_of(v,w->writer)){
   bool ok=baos_append(v,baos_of(v,w->writer),(const unsigned char*)bytes,n);
   return ok; /* OOM is not IOException and must remain pending. */
  }
  temporary=dvm_new_array(v,'B',"B",(uint32_t)n);if(temporary&&n)memcpy(dvm_array_data(v,temporary),bytes,n);
  a[0].l=temporary;a[2].i=(int32_t)n;sig="([BII)V";
 }else{
  dvm_ref text=dvm_new_string_n(v,bytes,n);
  if(text&&!string_runtime_call(v,"java/lang/String","toCharArray","()[C",text,NULL,0,&q))return false;
  temporary=q.l;if(text)dvm__discard_temporary(v,text);
  a[0].l=temporary;a[2].i=temporary?(int32_t)dvm_array_length(v,temporary):0;sig="([CII)V";
 }
 if(!temporary){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","PrintWriter write buffer");return false;}
 struct dvm_class *target=dvm_object_class(v,w->writer);bool handled=target&&io_runtime_call(v,target->name,"write",sig,w->writer,a,3,&q);
 if(!handled&&!dvm_exception(v)){
  struct dvm_method *method=target?dvm_find_method(v,target,"write",sig):NULL;
  if(method)handled=dvm_call(v,method,w->writer,a,3,&q);
  else dvm__throw(v,"java/lang/NoSuchMethodError","PrintWriter target write%s",sig);
 }
 /* A custom Writer/OutputStream may retain the argument; it is not discarded. */
 dvm_ref exception=dvm_exception(v);struct dvm_class *error=exception?dvm_object_class(v,exception):NULL;
 if(error&&dvm__class_assignable(v,error,dvm_find_class_or_external(v,"java/io/IOException"))){w->trouble=true;dvm_clear_exception(v);return false;}
 return handled&&!dvm_exception(v);
}
static bool pw_io_result(struct dvm *v,pw_data *w,bool result){
 dvm_ref e=dvm_exception(v);struct dvm_class *k=e?dvm_object_class(v,e):NULL;
 if(k&&dvm__class_assignable(v,k,dvm_find_class_or_external(v,"java/io/IOException"))){w->trouble=true;dvm_clear_exception(v);return false;}
 return result&&!dvm_exception(v);
}
static bool pw_control(struct dvm *v,pw_data *w,const char *operation){
 if(w->closed){w->trouble=true;return false;}
 if(baos_of(v,w->writer))return true;
 if(sw_of(v,w->writer))return true;
 union dvm_value q={0};struct dvm_class *target=dvm_object_class(v,w->writer);
 bool handled=target&&io_runtime_call(v,target->name,operation,"()V",w->writer,NULL,0,&q);
 if(!handled&&!dvm_exception(v)){
  struct dvm_method *m=target?dvm_find_method(v,target,operation,"()V"):NULL;
  if(m)handled=dvm_call(v,m,w->writer,NULL,0,&q);else dvm__throw(v,"java/lang/NoSuchMethodError","PrintWriter target %s",operation);
 }
 return pw_io_result(v,w,handled);
}
static bool append_char(struct dvm *v,dvm_ref pw,uint32_t cp);
static bool pw_string_range(struct dvm *v,dvm_ref pw,dvm_ref text,int32_t offset,int32_t length){
 if(!text){dvm__throw(v,"java/lang/NullPointerException","PrintWriter string");return false;}
 union dvm_value size={0};if(!string_runtime_call(v,"java/lang/String","length","()I",text,NULL,0,&size)||dvm_exception(v))return false;
 if(offset<0||length<0||(uint64_t)(uint32_t)offset+(uint32_t)length>(uint32_t)size.i){dvm__throw(v,"java/lang/IndexOutOfBoundsException","PrintWriter string range");return false;}
 for(int32_t i=0;i<length;i++){
  union dvm_value index={.i=offset+i},unit={0};if(!string_runtime_call(v,"java/lang/String","charAt","(I)C",text,&index,1,&unit)||dvm_exception(v))return false;
  uint32_t cp=(uint16_t)unit.i;
  if(cp>=0xD800&&cp<=0xDBFF&&i+1<length){index.i++;union dvm_value low={0};if(!string_runtime_call(v,"java/lang/String","charAt","(I)C",text,&index,1,&low)||dvm_exception(v))return false;if(low.i>=0xDC00&&low.i<=0xDFFF){cp=0x10000+((cp-0xD800)<<10)+(low.i-0xDC00);i++;}}
  if(!append_char(v,pw,cp))return false;
 }
 return true;
}
static bool append_string(struct dvm *v,dvm_ref pw,dvm_ref text){
 if(!text)return append_text(v,pw,"null",4);
 pw_data *w=pw_of(v,pw);const char *raw=dvm_string_utf8(v,text);size_t bytes=dvm_string_utf8_length(v,text);bool surrogate=false;
 for(size_t i=0;raw&&i+2<bytes;i++)if((uint8_t)raw[i]==0xED&&((uint8_t)raw[i+1]&0xE0)==0xA0){surrogate=true;break;}
 if(raw&&w&&!w->high&&!surrogate)return append_text(v,pw,raw,bytes);
 union dvm_value size={0};if(!string_runtime_call(v,"java/lang/String","length","()I",text,NULL,0,&size)||dvm_exception(v))return false;
 return pw_string_range(v,pw,text,0,size.i);
}
static bool append_char(struct dvm *v,dvm_ref pw,uint32_t cp){
 pw_data *w=pw_of(v,pw);if(!w){dvm__throw(v,"java/lang/IllegalStateException","PrintWriter not initialized");return false;}
 if(w->closed){w->trouble=true;return false;}
 if(w->output){
  if(w->high){uint16_t high=w->high;w->high=0;if(cp>=0xDC00&&cp<=0xDFFF)cp=0x10000+((high-0xD800)<<10)+(cp-0xDC00);else if(!append_text(v,pw,"?",1))return false;}
  if(cp>=0xD800&&cp<=0xDBFF){w->high=(uint16_t)cp;return true;}
  if(cp>=0xDC00&&cp<=0xDFFF)cp='?';
 }
 char out[4];size_t n=0;
 if(cp<0x80)out[n++]=(char)cp;
 else if(cp<0x800){out[n++]=(char)(0xc0|(cp>>6));out[n++]=(char)(0x80|(cp&63));}
 else if(cp<0x10000){out[n++]=(char)(0xe0|(cp>>12));out[n++]=(char)(0x80|((cp>>6)&63));out[n++]=(char)(0x80|(cp&63));}
 else{out[n++]=(char)(0xf0|(cp>>18));out[n++]=(char)(0x80|((cp>>12)&63));out[n++]=(char)(0x80|((cp>>6)&63));out[n++]=(char)(0x80|(cp&63));}
 return append_text(v,pw,out,n);
}
static bool pw_chars(struct dvm *v,dvm_ref pw,dvm_ref array,int32_t offset,int32_t length){
 struct dvm_object *o=dvm__obj(v,array);if(!o){dvm__throw(v,"java/lang/NullPointerException","PrintWriter chars");return false;}
 if(o->kind!=DVM_OBJ_ARRAY||o->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","PrintWriter char array");return false;}
 if(offset<0||length<0||(uint64_t)(uint32_t)offset+(uint32_t)length>o->length){dvm__throw(v,"java/lang/IndexOutOfBoundsException","PrintWriter chars range");return false;}
 uint16_t *units=o->data;
 for(int32_t i=0;i<length;i++){uint32_t cp=units[offset+i];if(cp>=0xD800&&cp<=0xDBFF&&i+1<length&&units[offset+i+1]>=0xDC00&&units[offset+i+1]<=0xDFFF){cp=0x10000+((cp-0xD800)<<10)+(units[offset+i+1]-0xDC00);i++;}if(!append_char(v,pw,cp))return false;}
 return true;
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
typedef struct {struct dvm *vm;dvm_ref buffer;int32_t pos,mark,count;bool closed;} car_data;
static void car_free(void *p){car_data *r=p;if(r){dvm_unpin(r->vm,r->buffer);free(r);}}
static bool car_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","CharArrayReader receiver");return true;}
 if(IS("<init>","([C)V")||IS("<init>","([CII)V")){
  struct dvm_object *b=dvm__obj(v,a[0].l);if(!b){dvm__throw(v,"java/lang/NullPointerException","CharArrayReader buffer");return true;}if(b->kind!=DVM_OBJ_ARRAY||b->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","CharArrayReader char buffer");return true;}
  int32_t off=n==3?a[1].i:0,len=n==3?a[2].i:(int32_t)b->length;
  if(off<0||(uint32_t)off>b->length||len<0||(uint64_t)(uint32_t)off+(uint32_t)len>INT32_MAX){dvm__throw(v,"java/lang/IllegalArgumentException","CharArrayReader range");return true;}
  car_data *r=calloc(1,sizeof *r);if(!r){dvm__throw(v,"java/lang/OutOfMemoryError","CharArrayReader");return true;}r->vm=v;r->buffer=a[0].l;r->pos=r->mark=off;int64_t end=(int64_t)off+len;r->count=end>b->length?(int32_t)b->length:(int32_t)end;dvm_pin(v,r->buffer);o->data=r;o->data_destroy=car_free;return true;
 }
 car_data *r=o->data_destroy==car_free?o->data:NULL;if(!r)return false;
 if(IS("close","()V")){r->closed=true;return true;}if(IS("markSupported","()Z")){out->i=1;return true;}
 if(r->closed){dvm__throw(v,"java/io/IOException","Stream closed");return true;}
 if(IS("ready","()Z")){out->i=r->count-r->pos>0;return true;}
 if(IS("mark","(I)V")){r->mark=r->pos;return true;}if(IS("reset","()V")){r->pos=r->mark;return true;}
 if(IS("skip","(J)J")){int64_t k=a[0].j;if(k<0)k=0;if(k>r->count-r->pos)k=r->count-r->pos;r->pos+=(int32_t)k;out->j=k;return true;}
 if(IS("read","()I")){out->i=r->pos>=r->count?-1:((uint16_t*)dvm_array_data(v,r->buffer))[r->pos++];return true;}
 bool whole=IS("read","([C)I"),range=IS("read","([CII)I");if(whole||range){
  struct dvm_object *b=dvm__obj(v,a[0].l);if(!b){dvm__throw(v,"java/lang/NullPointerException","CharArrayReader destination");return true;}if(b->kind!=DVM_OBJ_ARRAY||b->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","CharArrayReader char buffer");return true;}
  int32_t off=whole?0:a[1].i,len=whole?(int32_t)b->length:a[2].i;if(off<0||len<0||(uint32_t)off>b->length||(uint32_t)len>b->length-(uint32_t)off){dvm__throw(v,"java/lang/IndexOutOfBoundsException","CharArrayReader destination range");return true;}
  if(!len){out->i=0;return true;}if(r->pos>=r->count){out->i=-1;return true;}if(len>r->count-r->pos)len=r->count-r->pos;
  memmove((uint16_t*)dvm_array_data(v,a[0].l)+off,(uint16_t*)dvm_array_data(v,r->buffer)+r->pos,(size_t)len*2);r->pos+=len;out->i=len;return true;
 }
 return false;
}
/* Exact platform declaration catalog: only methods with actual C semantics.
 * The resolver consults this before searching a superclass default. */
bool io_runtime_method_supported(const char *c,const char *m,const char *sig){
 if(!c||!m||!sig)return false;
 bool sw=!strcmp(c,"java/io/StringWriter"),cw=!strcmp(c,"java/io/CharArrayWriter");
 if(sw||cw){
  if(IS("<init>","()V")||IS("<init>","(I)V")||IS("toString","()Ljava/lang/String;")||IS("write","(I)V")||IS("write","(Ljava/lang/String;)V")||IS("write","(Ljava/lang/String;II)V")||IS("write","([C)V")||IS("write","([CII)V")||IS("flush","()V")||IS("close","()V"))return true;
  if(cw&&(IS("toCharArray","()[C")||IS("size","()I")||IS("reset","()V")||IS("writeTo","(Ljava/io/Writer;)V")))return true;
  if(!strcmp(m,"append")){char expected[128];const char *args[]={"(C)","(Ljava/lang/CharSequence;)","(Ljava/lang/CharSequence;II)"};for(unsigned i=0;i<3;i++){snprintf(expected,sizeof expected,"%sL%s;",args[i],c);if(!strcmp(sig,expected))return true;}}
  return false;
 }
 if(!strcmp(c,"java/io/StringReader")||!strcmp(c,"java/io/CharArrayReader")){
  bool cr=!strcmp(c,"java/io/CharArrayReader");if(!strcmp(m,"<init>"))return cr?(!strcmp(sig,"([C)V")||!strcmp(sig,"([CII)V")):!strcmp(sig,"(Ljava/lang/String;)V");
  return IS("read","()I")||IS("read","([C)I")||IS("read","([CII)I")||IS("close","()V")||IS("ready","()Z")||IS("skip","(J)J")||IS("mark","(I)V")||IS("reset","()V")||IS("markSupported","()Z");
 }
 if(!strcmp(c,"java/io/ByteArrayInputStream"))return IS("<init>","([B)V")||IS("<init>","([BII)V")||IS("read","()I")||IS("read","([B)I")||IS("read","([BII)I")||IS("available","()I")||IS("skip","(J)J")||IS("mark","(I)V")||IS("reset","()V")||IS("markSupported","()Z")||IS("close","()V");
 if(!strcmp(c,"java/io/FileInputStream"))return IS("<init>","(Ljava/lang/String;)V")||IS("<init>","(Ljava/io/File;)V")||IS("read","()I")||IS("read","([B)I")||IS("read","([BII)I")||IS("available","()I")||IS("skip","(J)J")||IS("close","()V");
 if(!strcmp(c,"java/io/ByteArrayOutputStream"))return IS("<init>","()V")||IS("<init>","(I)V")||IS("write","(I)V")||IS("write","([B)V")||IS("write","([BII)V")||IS("toByteArray","()[B")||IS("toString","()Ljava/lang/String;")||IS("toString","(Ljava/lang/String;)Ljava/lang/String;")||IS("writeTo","(Ljava/io/OutputStream;)V")||IS("size","()I")||IS("reset","()V")||IS("close","()V");
 if(!strcmp(c,"java/io/FileReader"))return IS("<init>","(Ljava/lang/String;)V")||IS("<init>","(Ljava/io/File;)V")||IS("read","()I")||IS("read","([C)I")||IS("read","([CII)I")||IS("close","()V")||IS("ready","()Z")||IS("skip","(J)J");
 /* BufferedReader/InputStreamReader/PrintWriter already install concrete
  * declarations with builtins in io_runtime_vm_start. No toString override
  * exists on these Java classes: inheriting Object.toString is correct. */
 return false;
}
static bool io_runtime_call_impl(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool io_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct io_scope_guard guard __attribute__((cleanup(io_scope_cleanup)))={0};
 if(!io_scope_begin(v,&guard,self))return false;
 bool handled=io_runtime_call_impl(v,c,m,sig,self,a,n,out);
 if(guard.owned){dvm_gc_native_leave_result(&guard.scope,sig,handled&&out?out->l:0);guard.owned=false;}
 return handled;
}
static bool io_runtime_call_impl(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!strcmp(c,"java/io/CharArrayReader"))return car_call(v,m,sig,self,a,n,out);
 const dvm_ref gc5_ref_isr_object=(self?(self):0); if(!gc_operation_admit(v,gc5_ref_isr_object))return true;
 if((!strcmp(c,"java/io/InputStreamReader")&&(!dvm__obj(v,gc5_ref_isr_object)||!dvm__obj(v,gc5_ref_isr_object)->data||dvm__obj(v,gc5_ref_isr_object)->data_destroy==isr_free))||(dvm__obj(v,gc5_ref_isr_object)&&dvm__obj(v,gc5_ref_isr_object)->data_destroy==isr_free&&strcmp(m,"<init>")))return isr_dispatch(v,m,sig,self,a,n,out);
 const dvm_ref gc5_ref_bais_object=(self?(self):0); if(!gc_operation_admit(v,gc5_ref_bais_object))return true;
 if(!strcmp(c,"java/io/ByteArrayInputStream")||(dvm__obj(v,gc5_ref_bais_object)&&dvm__obj(v,gc5_ref_bais_object)->data_destroy==bais_free&&strcmp(m,"<init>")))return bais_call(v,m,sig,self,a,n,out);
 if(!strcmp(c,"java/io/Reader")&&!strcmp(m,"<init>")&&((!strcmp(sig,"()V")&&n==0)||(!strcmp(sig,"(Ljava/lang/Object;)V")&&n==1))){
  dvm_ref lock=n?a[0].l:self;
  if(!self||!lock){dvm__throw(v,"java/lang/NullPointerException","Reader lock/receiver");return true;}
  union dvm_value value={.l=lock};dvm_set_field(v,self,"lock","Ljava/lang/Object;",value);return true;
 }
 if(!strcmp(c,"java/io/Writer")&&strcmp(m,"<init>")){
  const dvm_ref gc5_ref_object=(self); if(!gc_operation_admit(v,gc5_ref_object))return true;
  if(dvm__obj(v,gc5_ref_object)&&(dvm__obj(v,gc5_ref_object)->data_destroy==sw_free||dvm__obj(v,gc5_ref_object)->data_destroy==pw_free)&&!strcmp(m,"append")){
   bool string_writer=dvm__obj(v,gc5_ref_object)->data_destroy==sw_free;
   const char *target_sig=NULL;
   if(!strcmp(sig,"(Ljava/lang/CharSequence;)Ljava/io/Writer;"))target_sig=string_writer?"(Ljava/lang/CharSequence;)Ljava/io/StringWriter;":"(Ljava/lang/CharSequence;)Ljava/io/PrintWriter;";
   if(!strcmp(sig,"(C)Ljava/io/Writer;"))target_sig=string_writer?"(C)Ljava/io/StringWriter;":"(C)Ljava/io/PrintWriter;";
   if(target_sig)return io_runtime_call(v,string_writer?"java/io/StringWriter":"java/io/PrintWriter",m,target_sig,self,a,n,out);
  }
  if(dvm__obj(v,gc5_ref_object)&&dvm__obj(v,gc5_ref_object)->data_destroy==sw_free)return io_runtime_call(v,"java/io/StringWriter",m,sig,self,a,n,out);
  if(dvm__obj(v,gc5_ref_object)&&dvm__obj(v,gc5_ref_object)->data_destroy==pw_free)return io_runtime_call(v,"java/io/PrintWriter",m,sig,self,a,n,out);
 }
 const dvm_ref gc5_ref_sr_object=(self?(self):0); if(!gc_operation_admit(v,gc5_ref_sr_object))return true;
 if(!strcmp(c,"java/io/StringReader")||(dvm__obj(v,gc5_ref_sr_object)&&dvm__obj(v,gc5_ref_sr_object)->data_destroy==sr_free))return sr_call(v,m,sig,self,a,n,out);
 if(v!=active)return false;
 if(!strcmp(c,"java/io/ObjectInputStream")){
  if(IS("<init>","(Ljava/io/InputStream;)V")){
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","ObjectInputStream stream");return true;}
   const dvm_ref gc5_ref_o=(self); if(!gc_operation_admit(v,gc5_ref_o))return true;if(!dvm__obj(v,gc5_ref_o)){dvm__throw(v,"java/lang/NullPointerException","ObjectInputStream receiver");return true;}
   ois_data *d=calloc(1,sizeof *d);if(!d){dvm__throw(v,"java/lang/OutOfMemoryError","ObjectInputStream");return true;}
   d->in=a[0].l;if(dvm__obj(v,gc5_ref_o)->data_destroy==ois_free)free(dvm__obj(v,gc5_ref_o)->data);dvm__obj(v,gc5_ref_o)->data=d;dvm__obj(v,gc5_ref_o)->data_destroy=ois_free;return true;
  }
  /* AOSP ObjectInputStream.close closes the underlying stream and is
   * idempotent; the reference is cleared so a second close does not call the
   * peer again.  A failure from the peer's close propagates unchanged. */
  if(IS("close","()V")){
   const dvm_ref gc5_ref_o=(self); if(!gc_operation_admit(v,gc5_ref_o))return true;ois_data *d=dvm__obj(v,gc5_ref_o)&&dvm__obj(v,gc5_ref_o)->data_destroy==ois_free?dvm__obj(v,gc5_ref_o)->data:NULL;
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
   const dvm_ref gc5_ref_arr=(a[0].l); if(!gc_operation_admit(v,gc5_ref_arr))return true;
   if(!dvm__obj(v,gc5_ref_arr)){dvm__throw(v,"java/lang/NullPointerException","byte array");return true;}
   if(dvm__obj(v,gc5_ref_arr)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc5_ref_arr)->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","not a byte array");return true;}
   int32_t off=IS("read","([BII)I")?a[1].i:0;
   int32_t len=IS("read","([BII)I")?a[2].i:(int32_t)dvm__obj(v,gc5_ref_arr)->length;
   if(off<0||len<0||(uint64_t)(uint32_t)off+(uint32_t)len>dvm__obj(v,gc5_ref_arr)->length){dvm__throw(v,"java/lang/IndexOutOfBoundsException","FileInputStream.read range");return true;}
   if(!len){out->i=0;return true;}
   unsigned char *dst=(unsigned char*)dvm__obj(v,gc5_ref_arr)->data+(size_t)off;
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
   const dvm_ref gc5_ref_arr=(a[0].l); if(!gc_operation_admit(v,gc5_ref_arr))return true;
   if(!dvm__obj(v,gc5_ref_arr)){dvm__throw(v,"java/lang/NullPointerException","char array is null");return true;}
   if(dvm__obj(v,gc5_ref_arr)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc5_ref_arr)->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","not a char array");return true;}
   return fr_read_chars(v,f,a[0].l,0,(int)dvm__obj(v,gc5_ref_arr)->length,out);
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
   if(baos_of(v,self)&&(IS("write","(I)V")||IS("write","([B)V")||IS("write","([BII)V")||IS("close","()V")))
    return io_runtime_call(v,"java/io/ByteArrayOutputStream",m,sig,self,a,n,out);
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
   b->vm=v;b->cap=n?(size_t)a[0].i:32;b->array=dvm_new_array(v,'B',"B",(uint32_t)b->cap);
   if(!b->array){free(b);return true;}dvm_pin(v,b->array);
   o=dvm__obj(v,self);o->data=b;o->data_destroy=baos_free;return true;
  }
  baos_data *b=baos_of(v,self);if(!b){dvm__throw(v,"java/lang/IllegalStateException","ByteArrayOutputStream not initialized");return true;}
  if(IS("write","(I)V")){unsigned char byte=(unsigned char)a[0].i;return baos_append(v,b,&byte,1)?true:(dvm_exception(v)!=0);}
  if(IS("write","([B)V")||IS("write","([BII)V")){
   const dvm_ref gc5_ref_arr=(a[0].l); if(!gc_operation_admit(v,gc5_ref_arr))return true;
   if(!dvm__obj(v,gc5_ref_arr)){dvm__throw(v,"java/lang/NullPointerException","ByteArrayOutputStream.write");return true;}
   if(dvm__obj(v,gc5_ref_arr)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc5_ref_arr)->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","not a byte array");return true;}
   int32_t off=IS("write","([BII)V")?a[1].i:0;
   int32_t len=IS("write","([BII)V")?a[2].i:(int32_t)dvm__obj(v,gc5_ref_arr)->length;
   if(off<0||len<0||(uint64_t)(uint32_t)off+(uint32_t)len>dvm__obj(v,gc5_ref_arr)->length){dvm__throw(v,"java/lang/IndexOutOfBoundsException","ByteArrayOutputStream.write range");return true;}
   if(!len)return true;
   return baos_append(v,b,(const unsigned char*)dvm__obj(v,gc5_ref_arr)->data+(size_t)off,(size_t)len)?true:(dvm_exception(v)!=0);
  }
  if(IS("writeTo","(Ljava/io/OutputStream;)V")){
   if(n!=1||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","ByteArrayOutputStream.writeTo target");return true;}
   struct dvm_class *target=dvm_object_class(v,a[0].l);
   struct dvm_method *method=target?dvm_find_method(v,target,"write","([BII)V"):NULL;
   if(!method){dvm__throw(v,"java/lang/NoSuchMethodError","OutputStream.write([BII)V");return true;}
   if(b->len>INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream.writeTo size");return true;}
   uint32_t count=(uint32_t)b->len;dvm_ref bytes=b->array;
   b->exposed=true;dvm_pin(v,bytes); /* survives a reentrant grow/reset */
   union dvm_value args[3]={{.l=bytes},{.i=0},{.i=(int32_t)count}},ignored={0};
   (void)dvm_call(v,method,a[0].l,args,3,&ignored);
   dvm_unpin(v,bytes); /* peer retains current backing; escaped old backing lives */
   return true;
  }
  if(IS("size","()I")){if(b->len>INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream size");return true;}out->i=(int32_t)b->len;return true;}
  if(IS("reset","()V")||IS("close","()V")){if(IS("reset","()V"))b->len=0;return true;}
  if(IS("toByteArray","()[B")){
   if(b->len>INT32_MAX){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return true;}
   dvm_ref array=dvm_new_array(v,'B',"B",(uint32_t)b->len);
   if(!array){dvm__throw(v,"java/lang/OutOfMemoryError","ByteArrayOutputStream");return true;}
   unsigned char *buffer=baos_buffer(v,b);
   if(!buffer){dvm__discard_temporary(v,array);return true;}
   if(b->len)memcpy(dvm_array_data(v,array),buffer,b->len);
   out->l=array;return true;
  }
  if(IS("toString","()Ljava/lang/String;")||IS("toString","(Ljava/lang/String;)Ljava/lang/String;")){
   const char *enc="UTF-8";if(n){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","charset name");return true;}enc=dvm_string_utf8(v,a[0].l);if(!enc)return false;}
   if(!strcasecmp(enc,"UTF-8")||!strcasecmp(enc,"UTF8")||!strcasecmp(enc,"unicode-1-1-utf-8")){
    dvm_ref arr=b->array;int32_t count=(int32_t)b->len;dvm_ref name=dvm_new_string(v,"UTF-8");if(!name)return true;dvm_pin(v,name);dvm_ref text=dvm_new_string(v,"");union dvm_value args[4]={{.l=arr},{.i=0},{.i=count},{.l=name}},q={0};if(text&&!string_runtime_call(v,"java/lang/String","<init>","([BIILjava/lang/String;)V",text,args,4,&q))dvm__throw(v,"java/lang/NoSuchMethodError","String byte range charset constructor");dvm_unpin(v,name);dvm__discard_temporary(v,name);out->l=dvm_exception(v)?0:text;return true;
   }
   bool latin=!strcasecmp(enc,"ISO-8859-1")||!strcasecmp(enc,"ISO8859_1")||!strcasecmp(enc,"latin1"),ascii=!strcasecmp(enc,"US-ASCII")||!strcasecmp(enc,"ASCII"),le=!strcasecmp(enc,"UTF-16LE"),be=!strcasecmp(enc,"UTF-16BE"),auto16=!strcasecmp(enc,"UTF-16");
   if(!latin&&!ascii&&!le&&!be&&!auto16){dvm__throw(v,"java/io/UnsupportedEncodingException","%s",enc);return true;}
   uint16_t *units=malloc((b->len+1)*sizeof *units);if(!units){dvm__throw(v,"java/lang/OutOfMemoryError","byte stream decode");return true;}size_t count=0,i=0;
   unsigned char *buffer=baos_buffer(v,b);if(!buffer){free(units);return true;}
   if(latin||ascii){for(;i<b->len;i++)units[count++]=ascii&&buffer[i]>=128?0xfffd:buffer[i];}
   else{if(auto16&&b->len>=2){if(buffer[0]==0xff&&buffer[1]==0xfe){le=true;i=2;}else if(buffer[0]==0xfe&&buffer[1]==0xff)i=2;}for(;i+1<b->len;i+=2){uint16_t u=le?(uint16_t)(buffer[i]|buffer[i+1]<<8):(uint16_t)(buffer[i]<<8|buffer[i+1]);if(u>=0xd800&&u<=0xdbff){if(i+3<b->len){uint16_t low=le?(uint16_t)(buffer[i+2]|buffer[i+3]<<8):(uint16_t)(buffer[i+2]<<8|buffer[i+3]);if(low>=0xdc00&&low<=0xdfff){units[count++]=u;units[count++]=low;i+=2;continue;}}u=0xfffd;}else if(u>=0xdc00&&u<=0xdfff)u=0xfffd;units[count++]=u;}if(i<b->len)units[count++]=0xfffd;}
   out->l=io_units_string(v,units,count);free(units);return true;
  }
  return false;
 }
 if(!strcmp(c,"java/io/StringWriter")||!strcmp(c,"java/io/CharArrayWriter")){
  bool chars=!strcmp(c,"java/io/CharArrayWriter");
  if(IS("<init>","()V")||IS("<init>","(I)V")){
   const dvm_ref gc5_ref_o=(self); if(!gc_operation_admit(v,gc5_ref_o))return true;if(!dvm__obj(v,gc5_ref_o)){dvm__throw(v,"java/lang/NullPointerException","character writer receiver");return true;}
   if(n&&a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative writer size");return true;}
   sw_data *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","character writer");return true;}
   if(n&&a[0].i){w->buf=malloc((size_t)a[0].i*sizeof *w->buf);if(!w->buf){free(w);dvm__throw(v,"java/lang/OutOfMemoryError","character writer");return true;}w->cap=a[0].i;}dvm__obj(v,gc5_ref_o)->data=w;dvm__obj(v,gc5_ref_o)->data_destroy=sw_free;return true;
  }
  sw_data *w=sw_of(v,self);if(!w){dvm__throw(v,"java/lang/IllegalStateException","character writer not initialized");return true;}
  if(IS("flush","()V")||IS("close","()V"))return true;
  if(IS("toString","()Ljava/lang/String;")){out->l=io_units_string(v,w->buf,w->len);return true;}
  if(chars&&IS("toCharArray","()[C")){out->l=io_units_array(v,w->buf,w->len);return true;}
  if(chars&&IS("size","()I")){out->i=(int32_t)w->len;return true;}
  if(chars&&IS("reset","()V")){w->len=0;return true;}
  bool app=!strcmp(m,"append");
  if(IS("write","(I)V")||(app&&sig[1]=='C')){uint16_t u=(uint16_t)a[0].i;(void)io_units_append(v,w,&u,1);if(app)out->l=self;return true;}
  bool str=IS("write","(Ljava/lang/String;)V"),strrange=IS("write","(Ljava/lang/String;II)V"),arr=IS("write","([C)V"),arrrange=IS("write","([CII)V");
  if(str||strrange||arr||arrrange||app){
   dvm_ref array=0;int32_t off=0,len=0;bool temporary=false;
   if(str||strrange||app){dvm_ref text=a[0].l;if(!text){if(!app){dvm__throw(v,"java/lang/NullPointerException","writer text");return true;}text=dvm_new_string(v,"null");}if(!io_text_units(v,text,&array))return true;temporary=true;}
   else array=a[0].l;
   const dvm_ref gc5_ref_b=(array); if(!gc_operation_admit(v,gc5_ref_b))return true;if(!dvm__obj(v,gc5_ref_b)){dvm__throw(v,"java/lang/NullPointerException","writer array");return true;}if(dvm__obj(v,gc5_ref_b)->kind!=DVM_OBJ_ARRAY||dvm__obj(v,gc5_ref_b)->elem_kind!='C'){dvm__throw(v,"java/lang/IllegalArgumentException","writer char array");return true;}
   len=(int32_t)dvm__obj(v,gc5_ref_b)->length;if(strrange||arrrange){off=a[1].i;len=a[2].i;}else if(app&&n==3){off=a[1].i;int64_t count=(int64_t)a[2].i-off;if(count<0||count>INT32_MAX){dvm__throw(v,"java/lang/IndexOutOfBoundsException","writer append range");return true;}len=(int32_t)count;}
   if(off<0||len<0||(uint32_t)off>dvm__obj(v,gc5_ref_b)->length||(uint32_t)len>dvm__obj(v,gc5_ref_b)->length-(uint32_t)off){dvm__throw(v,"java/lang/IndexOutOfBoundsException","writer range");return true;}
   (void)io_units_append(v,w,(uint16_t*)dvm_array_data(v,array)+off,(size_t)len);if(temporary)dvm__discard_temporary(v,array);if(app)out->l=self;return true;
  }
  if(chars&&IS("writeTo","(Ljava/io/Writer;)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","writer target");return true;}dvm_ref array=io_units_array(v,w->buf,w->len);if(!array)return true;dvm_pin(v,array);union dvm_value args[3]={{.l=array},{.i=0},{.i=(int32_t)w->len}};struct dvm_method *method=dvm_find_method(v,dvm_object_class(v,a[0].l),"write","([CII)V");if(method)(void)dvm_call(v,method,a[0].l,args,3,out);else dvm__throw(v,"java/lang/NoSuchMethodError","Writer.write([CII)V");dvm_unpin(v,array);if(method&&!method->has_code&&method->cls&&(!strcmp(method->cls->name,"java/io/StringWriter")||!strcmp(method->cls->name,"java/io/CharArrayWriter")||!strcmp(method->cls->name,"java/io/PrintWriter")))dvm__discard_temporary(v,array);return true;}
  return false;
 }
 if(!strcmp(c,"java/io/PrintWriter")){
  bool writer_ctor=IS("<init>","(Ljava/io/Writer;)V")||IS("<init>","(Ljava/io/Writer;Z)V");
  bool stream_ctor=IS("<init>","(Ljava/io/OutputStream;)V")||IS("<init>","(Ljava/io/OutputStream;Z)V");
  if(writer_ctor||stream_ctor){
   const dvm_ref gc5_ref_o=(self); if(!gc_operation_admit(v,gc5_ref_o))return true;if(!dvm__obj(v,gc5_ref_o)||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","PrintWriter target");return true;}
   if(dvm__obj(v,gc5_ref_o)->data){dvm__throw(v,"java/lang/IllegalStateException","PrintWriter already initialized");return true;}
   struct dvm_class *target=dvm_object_class(v,a[0].l),*base=dvm_find_class_or_external(v,stream_ctor?"java/io/OutputStream":"java/io/Writer");
   if(a[0].l==self||!target||!base||!dvm__class_assignable(v,target,base)){dvm__throw(v,"java/lang/IllegalArgumentException","PrintWriter target type");return true;}
   pw_data *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","PrintWriter");return true;}
   w->vm=v;w->writer=a[0].l;w->output=stream_ctor;w->auto_flush=n>1&&a[1].i;dvm_pin(v,w->writer);dvm__obj(v,gc5_ref_o)->data=w;dvm__obj(v,gc5_ref_o)->data_destroy=pw_free;return true;
  }
  pw_data *w=pw_of(v,self);if(!w){dvm__throw(v,"java/lang/IllegalStateException","PrintWriter not initialized");return true;}
  if(IS("flush","()V")){(void)pw_control(v,w,"flush");return true;}
  if(IS("close","()V")){
   if(!w->closed){if(w->high){w->high=0;(void)append_text(v,self,"?",1);}if(!dvm_exception(v))(void)pw_control(v,w,"close");w->closed=true;}
   return true;
  }
  if(IS("checkError","()Z")){
   if(!w->closed)(void)pw_control(v,w,"flush");if(dvm_exception(v))return true;
   pw_data *nested=pw_of(v,w->writer);out->i=w->trouble||(nested&&nested->trouble);return true;
  }
  bool newline=!strcmp(m,"println"),printed=false;
  if(IS("print","(Ljava/lang/String;)V")||IS("write","(Ljava/lang/String;)V")||IS("println","(Ljava/lang/String;)V")||IS("append","(Ljava/lang/CharSequence;)Ljava/io/PrintWriter;")){
   if(!strcmp(m,"write")&&!a[0].l){dvm__throw(v,"java/lang/NullPointerException","PrintWriter string");return true;}
   printed=true;(void)append_string(v,self,a[0].l);if(!strcmp(m,"append"))out->l=self;
  }else if(IS("write","(Ljava/lang/String;II)V")){printed=true;(void)pw_string_range(v,self,a[0].l,a[1].i,a[2].i);
  }else if(IS("write","([C)V")||IS("print","([C)V")||IS("println","([C)V")){
   printed=true;const dvm_ref gc5_ref_array=(a[0].l); if(!gc_operation_admit(v,gc5_ref_array))return true;(void)pw_chars(v,self,a[0].l,0,dvm__obj(v,gc5_ref_array)?(int32_t)dvm__obj(v,gc5_ref_array)->length:0);
  }else if(IS("write","([CII)V")){printed=true;(void)pw_chars(v,self,a[0].l,a[1].i,a[2].i);
  }else if(IS("write","(I)V")||IS("print","(C)V")||IS("println","(C)V")||IS("append","(C)Ljava/io/PrintWriter;")){
   printed=true;(void)append_char(v,self,(uint16_t)a[0].i);if(!strcmp(m,"append"))out->l=self;
  }else if(IS("print","(Ljava/lang/Object;)V")||IS("println","(Ljava/lang/Object;)V")){
   printed=true;dvm_ref text=0;
   if(a[0].l){const dvm_ref gc5_ref_object=(a[0].l); if(!gc_operation_admit(v,gc5_ref_object))return true;if(dvm__obj(v,gc5_ref_object)&&dvm__obj(v,gc5_ref_object)->kind==DVM_OBJ_STRING)text=a[0].l;
    else{struct dvm_method *method=dvm__obj(v,gc5_ref_object)?dvm_find_method(v,dvm__obj(v,gc5_ref_object)->cls,"toString","()Ljava/lang/String;"):NULL;union dvm_value value={0};
     if(method){if(!dvm_call(v,method,a[0].l,NULL,0,&value)||dvm_exception(v))return true;text=value.l;}
     else{dvm__throw(v,"java/lang/NoSuchMethodError","PrintWriter object.toString");return true;}}}
   (void)append_string(v,self,text);
  }else if((!strcmp(m,"print")||newline)&&(!strcmp(sig,"(I)V")||!strcmp(sig,"(J)V")||!strcmp(sig,"(F)V")||!strcmp(sig,"(D)V")||!strcmp(sig,"(Z)V"))){
   printed=true;if(sig[1]=='Z')(void)append_text(v,self,a[0].i?"true":"false",a[0].i?4:5);
   else{const char *owner=sig[1]=='I'?"java/lang/Integer":sig[1]=='J'?"java/lang/Long":sig[1]=='F'?"java/lang/Float":"java/lang/Double";
    const char *signature=sig[1]=='I'?"(I)Ljava/lang/String;":sig[1]=='J'?"(J)Ljava/lang/String;":sig[1]=='F'?"(F)Ljava/lang/String;":"(D)Ljava/lang/String;";
    union dvm_value text={0};bool converted=false;
    if(sig[1]=='F'||sig[1]=='D'){
     union dvm_value box={0};const char *factory=sig[1]=='F'?"(F)Ljava/lang/Float;":"(D)Ljava/lang/Double;";
     if(java_basics_call(v,owner,"valueOf",factory,0,a,1,&box)&&box.l&&!dvm_exception(v)){
      converted=java_basics_call(v,owner,"toString","()Ljava/lang/String;",box.l,NULL,0,&text);dvm__discard_temporary(v,box.l);
     }
    }else converted=java_basics_call(v,owner,"toString",signature,0,a,1,&text);
    if(!converted){if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","PrintWriter numeric toString");return true;}
    if(!dvm_exception(v))(void)append_string(v,self,text.l);}
  }else if(IS("println","()V"))printed=true;
  if(!printed)return false;
  if(newline&&!dvm_exception(v)){(void)append_text(v,self,"\n",1);if(w->auto_flush&&!dvm_exception(v))(void)pw_control(v,w,"flush");}
  return true;
 }
 if(!strcmp(c,"java/lang/Throwable")&&IS("printStackTrace","(Ljava/io/PrintWriter;)V")){
  if(!a[0].l)return true;
  char why[8192];dvm_describe_exception(v,self,why,sizeof why);
  if(!append_text(v,a[0].l,why,strlen(why)))return true;
   (void)append_text(v,a[0].l,"\n",1);return true;
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
static bool baos_test_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm__builtin_method();
 return m&&io_runtime_call(v,"java/io/ByteArrayOutputStream",m->name,m->sig,self,a,n,out);
}
static bool baos_test_peer_write(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=3||a[1].i!=0||a[2].i<1)return false;
 union dvm_value value={.l=a[0].l};
 if(!dvm_set_field(v,self,"saved","[B",value))return false;
 unsigned char *data=dvm_array_data(v,a[0].l);if(!data)return false;
 data[0]='X';out->i=0;return true;
}
static bool baos_test_call(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,self),name,sig);
 return m&&dvm_call(v,m,self,a,n,out)&&!dvm_exception(v);
}
/* Callable separately by the integration driver, and included in the existing
 * IO suite below. All operations execute real VM builtin methods via dvm_call. */
bool io_runtime_baos_selftest(struct dvm *v){
 struct io_scope_guard guard __attribute__((cleanup(io_scope_cleanup)))={0};if(!io_scope_begin(v,&guard,0))return false;
 if(!v||dvm_exception(v))return false;
 unsigned checks=0;bool ok=false;const char *stage="setup";
 dvm_ref stream=0,input=0,peer=0,snapshot=0,text=0,empty=0,escaped=0,current=0;
 union dvm_value out={0},arg={0};
 struct dvm_class *base=dvm_find_class_or_external(v,"java/io/ByteArrayOutputStream");
 struct dvm_class *fixture=dvm__register_builtin(v,"Lsdktrace/tests/BaosGc2;");
 struct dvm_class *sink=dvm__register_builtin(v,"Lsdktrace/tests/BaosGc2Peer;");
#define BAOS_CHECK(label,expr) do{stage=label;if(!(expr))goto cleanup;++checks;sdk_trace_fprintf(stderr,"GC2_BAOS_CHECK PASS %s\n",label);}while(0)
 BAOS_CHECK("fixture_classes",base&&fixture&&sink);
 fixture->super=base;
 static const struct {const char *name,*sig;} defs[]={
  {"<init>","(I)V"},{"write","(I)V"},{"write","([B)V"},{"write","([BII)V"},
  {"toByteArray","()[B"},{"writeTo","(Ljava/io/OutputStream;)V"},
  {"toString","(Ljava/lang/String;)Ljava/lang/String;"},{"reset","()V"},{"close","()V"}};
 if(!fixture->nmethods){
  fixture->methods=calloc(sizeof defs/sizeof defs[0],sizeof *fixture->methods);if(!fixture->methods)goto cleanup;
  for(unsigned i=0;i<sizeof defs/sizeof defs[0];i++){
   struct dvm_method *m=&fixture->methods[i];m->cls=fixture;m->name=defs[i].name;m->sig=strdup(defs[i].sig);
   if(!m->sig)goto cleanup;fixture->nmethods++;m->builtin=baos_test_builtin;m->access=1;
   m->ret_kind=dvm_sig_return_kind(m->sig);m->arg_count=dvm_sig_arg_count(m->sig);m->arg_slots=dvm_sig_arg_slots(m->sig);
  }
 }
 if(!sink->nmethods){
  sink->super=dvm_find_class_or_external(v,"java/io/OutputStream");
  sink->ifields=calloc(1,sizeof *sink->ifields);sink->methods=calloc(1,sizeof *sink->methods);
  if(!sink->ifields||!sink->methods)goto cleanup;
  sink->nifields=1;sink->islots=(sink->super?sink->super->islots:0)+1;
  *sink->ifields=(struct dvm_field){.cls=sink,.name="saved",.type="[B",.kind='L',.width=1,.slot=(uint16_t)(sink->islots-1),.access=1};
  struct dvm_method *m=sink->methods;m->cls=sink;m->name="write";m->sig=strdup("([BII)V");
  if(!m->sig)goto cleanup;sink->nmethods=1;m->builtin=baos_test_peer_write;m->access=1;m->ret_kind='V';m->arg_count=m->arg_slots=3;
 }
 stream=dvm_new_object(v,fixture);peer=dvm_new_object(v,sink);arg.i=0;
 BAOS_CHECK("zero_capacity_constructor",stream&&peer&&baos_test_call(v,stream,"<init>","(I)V",&arg,1,&out)&&baos_of(v,stream)->cap==0);
 input=dvm_new_array(v,'B',"B",3);BAOS_CHECK("source_array",input);
 memcpy(dvm_array_data(v,input),"abc",3);arg.l=input;
 BAOS_CHECK("growth_from_zero_uses_real_guest_array",baos_test_call(v,stream,"write","([B)V",&arg,1,&out)&&baos_of(v,stream)->len==3&&baos_of(v,stream)->cap>=3);
 /* A self write exposes the old backing, then reenters append while growing. */
 escaped=baos_of(v,stream)->array;arg.l=stream;
 BAOS_CHECK("reentrant_self_write_growth_preserves_source",baos_test_call(v,stream,"writeTo","(Ljava/io/OutputStream;)V",&arg,1,&out)&&baos_of(v,stream)->len==6&&!memcmp(baos_buffer(v,baos_of(v,stream)),"abcabc",6));
 BAOS_CHECK("escaped_old_backing_survives_growth",baos_of(v,stream)->array!=escaped&&dvm__obj(v,escaped)&&!memcmp(dvm_array_data(v,escaped),"abc",3));
 arg.l=peer;
 BAOS_CHECK("writeTo_peer_receives_actual_backing",baos_test_call(v,stream,"writeTo","(Ljava/io/OutputStream;)V",&arg,1,&out)&&dvm_get_field(v,peer,"saved","[B",&out)&&out.l==baos_of(v,stream)->array&&baos_buffer(v,baos_of(v,stream))[0]=='X');
 BAOS_CHECK("snapshot_reads_current_backing",baos_test_call(v,stream,"toByteArray","()[B",NULL,0,&out)&&out.l);snapshot=out.l;
 BAOS_CHECK("snapshot_is_copy",snapshot!=baos_of(v,stream)->array&&dvm_array_length(v,snapshot)==6&&!memcmp(dvm_array_data(v,snapshot),"Xbcabc",6));
 arg.l=dvm_new_string(v,"ISO-8859-1");text=arg.l;
 BAOS_CHECK("charset_decode_reads_current_backing",text&&baos_test_call(v,stream,"toString","(Ljava/lang/String;)Ljava/lang/String;",&arg,1,&out)&&io_selftest_string(v,out.l,"Xbcabc",6));
 if(out.l)dvm__discard_temporary(v,out.l);
 BAOS_CHECK("reset_preserves_backing_identity",baos_test_call(v,stream,"reset","()V",NULL,0,&out)&&!baos_of(v,stream)->len);
 arg.i='Z';
 BAOS_CHECK("no_growth_write_re_resolves_backing",baos_test_call(v,stream,"write","(I)V",&arg,1,&out)&&baos_of(v,stream)->len==1&&baos_buffer(v,baos_of(v,stream))[0]=='Z'&&((unsigned char*)dvm_array_data(v,snapshot))[0]=='X');
 BAOS_CHECK("close_is_not_stream_invalidation",baos_test_call(v,stream,"close","()V",NULL,0,&out));arg.i='Q';
 BAOS_CHECK("write_after_close",baos_test_call(v,stream,"write","(I)V",&arg,1,&out)&&baos_of(v,stream)->len==2&&!memcmp(baos_buffer(v,baos_of(v,stream)),"ZQ",2));
 /* Force a refusal before allocation without changing the live stream. */
 baos_data *state=baos_of(v,stream);dvm_ref before=state->array;size_t old_len=state->len,old_cap=state->cap;
 bool refused=baos_append(v,state,(const unsigned char*)"!",(size_t)INT32_MAX);
 BAOS_CHECK("append_overflow_preserves_old_state",!refused&&dvm_exception(v)&&state->array==before&&state->len==old_len&&state->cap==old_cap&&!memcmp(baos_buffer(v,state),"ZQ",2));
 dvm_clear_exception(v);
 BAOS_CHECK("reset_to_empty_snapshot",baos_test_call(v,stream,"reset","()V",NULL,0,&out)&&baos_test_call(v,stream,"toByteArray","()[B",NULL,0,&out)&&out.l&&dvm_array_length(v,out.l)==0);empty=out.l;
 ok=true;
cleanup:
 if(dvm_exception(v)){ok=false;dvm_clear_exception(v);}
 /* The peer intentionally retains the current backing. Releasing the owner
  * must not destroy that escaped byte[]; the VM owns fixture references. */
 if(stream&&baos_of(v,stream)){
  dvm_ref retained=baos_of(v,stream)->array;current=retained;
  /* Test the real registered destructor without depending on GC collection. */
  struct dvm_object *owner=dvm__obj(v,stream);
  if(owner){baos_free(owner->data);owner->data=NULL;owner->data_destroy=NULL;}
  if(ok){
   stage="destructor_preserves_escaped_peer_backing";
   if(!dvm__obj(v,retained)||!dvm_get_field(v,peer,"saved","[B",&out)||out.l!=retained)ok=false;
   else{++checks;sdk_trace_fprintf(stderr,"GC2_BAOS_CHECK PASS %s\n",stage);}
  }
 }
 dvm_ref refs[]={stream,input,peer,snapshot,text,empty,escaped,current};
 for(unsigned i=0;i<sizeof refs/sizeof refs[0];i++)if(refs[i])dvm__discard_temporary(v,refs[i]);
 sdk_trace_fprintf(stderr,"GC2_BAOS_SELFTEST %s checks=%u failed_stage=%s persistent_payload_pointer=0 actual_dvm_call=1 moving_tested=0\n",ok?"PASS":"FAIL",checks,ok?"none":stage);
#undef BAOS_CHECK
 return ok;
}
bool io_runtime_selftest(struct dvm *v){
 struct io_scope_guard guard __attribute__((cleanup(io_scope_cleanup)))={0};if(!io_scope_begin(v,&guard,0))return false;
 if(!io_runtime_baos_selftest(v))return false;
 union dvm_value out={0},arg={0};
 {
 dvm_ref reader=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/Reader"));
 if(!io_runtime_call(v,"java/io/Reader","<init>","()V",reader,NULL,0,&out)||dvm_exception(v)||!dvm_get_field(v,reader,"lock","Ljava/lang/Object;",&out)||out.l!=reader)return false;
 arg.l=dvm_new_string(v,"reader-lock");
 if(!io_runtime_call(v,"java/io/Reader","<init>","(Ljava/lang/Object;)V",reader,&arg,1,&out)||dvm_exception(v)||!dvm_get_field(v,reader,"lock","Ljava/lang/Object;",&out)||out.l!=arg.l)return false;
 arg.l=0;if(!io_runtime_call(v,"java/io/Reader","<init>","(Ljava/lang/Object;)V",reader,&arg,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"TASK5_READER_SELFTEST PASS default_lock_self=1 explicit_lock=1 null_lock_throws=1\n");
 }
 {
 dvm_ref string_reader=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/StringReader"));arg.l=dvm_new_string(v,"A\xf0\x9f\x98\x80Z");
 if(!sr_call(v,"<init>","(Ljava/lang/String;)V",string_reader,&arg,1,&out)||dvm_exception(v))return false;
 dvm_ref chars=dvm_new_array(v,'C',"C",6);union dvm_value range[3]={{.l=chars},{.i=1},{.i=4}};
 if(!sr_call(v,"read","([CII)I",string_reader,range,3,&out)||out.i!=4)return false;
 uint16_t *units=dvm_array_data(v,chars);if(units[0]||units[1]!='A'||units[2]!=0xd83d||units[3]!=0xde00||units[4]!='Z'||units[5])return false;
 if(!sr_call(v,"read","()I",string_reader,NULL,0,&out)||out.i!=-1)return false;
 range[2].i=0;if(!sr_call(v,"read","([CII)I",string_reader,range,3,&out)||out.i!=0)return false;
 if(!sr_call(v,"reset","()V",string_reader,NULL,0,&out)||!sr_call(v,"read","()I",string_reader,NULL,0,&out)||out.i!='A')return false;
 range[1].i=-1;if(!sr_call(v,"read","([CII)I",string_reader,range,3,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 if(!sr_call(v,"close","()V",string_reader,NULL,0,&out)||!sr_call(v,"read","()I",string_reader,NULL,0,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 }
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
#include "byte_array_input_stream_selftest.inc"
#include "input_stream_reader_selftest.inc"

int io_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 struct gc_operation_pin gc5_pin __attribute__((cleanup(gc_operation_pin_end)));
 if(!gc_operation_pin_begin(v,&gc5_pin,o))return DVM_GC_PAYLOAD_INVALID;
 o=dvm__obj(v,gc5_pin.ref);

 (void)v;void *p=o->data;
 if(o->data_destroy==reader_fixture_free)return DVM_GC_PAYLOAD_ATOMIC;
 if(o->data_destroy==sw_free||o->data_destroy==fr_free||o->data_destroy==fos_free)return DVM_GC_PAYLOAD_ATOMIC;
#define GC_IO(destroy,type,field) if(o->data_destroy==destroy){if(visit)visit(&((type*)p)->field,user);return DVM_GC_PAYLOAD_REFS;}
 GC_IO(pw_free,pw_data,writer)
 GC_IO(baos_free,baos_data,array)
 GC_IO(bais_free,bais_data,array)
 GC_IO(sr_free,sr_data,text)
 GC_IO(ois_free,ois_data,in)
 GC_IO(br_free,br_data,reader)
 GC_IO(isr_free,isr_data,input)
 GC_IO(car_free,car_data,buffer)
#undef GC_IO
 return DVM_GC_PAYLOAD_UNKNOWN;
}
