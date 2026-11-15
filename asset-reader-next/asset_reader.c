#include "gc_runtime.h"
#include "asset_reader.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
int sdk_trace_fprintf(FILE*,const char*,...);
typedef struct {const char* name;const unsigned char* bytes;size_t size;} asset;
#include "assets.inc"
typedef struct {struct dvm* vm;dvm_ref child;const asset* a;size_t pos;int closed;bool whole_input;} io;
/* Hot-path rule: no per-call fopen/fwrite/fclose.
 * OFF unless files/sdkdex/engine-probe-observe contains '1' (checked once);
 * when enabled the stream stays open and VM teardown closes it. */
static int asset_probe_enabled=-1;
static FILE *asset_probe_file;
static int asset_probe_ready(void){
 if(asset_probe_enabled<0){
  asset_probe_enabled=0;
  FILE *flag=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe-observe","r");
  if(flag){char b=0;if(fread(&b,1,1,flag)==1&&b=='1')asset_probe_enabled=1;fclose(flag);}
 }
 return asset_probe_enabled==1;
}
static void asset_probe_log(const char *fmt,...){
 if(!asset_probe_ready())return;
 if(!asset_probe_file){
  asset_probe_file=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
  if(!asset_probe_file){asset_probe_enabled=0;return;}
 }
 va_list a;va_start(a,fmt);vfprintf(asset_probe_file,fmt,a);va_end(a);fputc('\n',asset_probe_file);
}
static void asset_probe_close(void){
 if(asset_probe_file){fflush(asset_probe_file);fclose(asset_probe_file);asset_probe_file=NULL;}
}
static struct dvm* active;static dvm_ref resources,manager,charset;
static dvm_ref named_charsets[8];
static bool asset_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 (void)context;if(!dvm_gil_held(v)||active!=v)return false;
 dvm_ref *slots[]={&resources,&manager,&charset};for(unsigned i=0;i<3;i++)if(*slots[i])visit(slots[i],user);
 for(unsigned i=0;i<8;i++)if(named_charsets[i])visit(&named_charsets[i],user);return true;
}
void asset_reader_vm_end(struct dvm *v){if(active!=v)return;asset_probe_close();dvm_gc_root_provider_remove(v,"asset-reader/cache",&active);resources=manager=charset=0;memset(named_charsets,0,sizeof named_charsets);active=NULL;}
static const char* charset_names[]={"UTF-8","UTF-16BE","UTF-16LE","UTF-16","US-ASCII","ISO-8859-1","UTF-32BE","UTF-32LE"};
static void release(void* q){io* p=q;if(p){if(p->child)dvm_unpin(p->vm,p->child);free(p);}}
static bool error(struct dvm* v,const char* c){dvm__throw(v,c,"asset reader");return true;}
static dvm_ref object(struct dvm* v,const char* c){return dvm_new_object(v,dvm_find_class_or_external(v,c));}
bool asset_reader_is_manager(struct dvm *v,dvm_ref ref){return v==active&&ref&&ref==manager;}
bool asset_reader_vm_start(struct dvm* v){
 active=v;resources=manager=charset=0;
 memset(named_charsets,0,sizeof named_charsets);
 if(!dvm_gc_root_provider_add(v,"asset-reader/cache",DVM_GC_ROOT_STRONG,asset_gc_roots,&active))return false;
 struct dvm_class* reader=dvm_find_class_or_external(v,"java/io/Reader");
 struct dvm_class* input=dvm_find_class_or_external(v,"java/io/InputStreamReader");
 struct dvm_class* buffered=dvm_find_class_or_external(v,"java/io/BufferedReader");
 if(!reader||!input||!buffered)return false;input->super=reader;buffered->super=reader;return true;
}
/* Preserve valid UTF8, including NUL/non-BMP. Malformed decoding is explicitly
 * unsupported instead of silently producing a different SDK configuration. */
static bool valid_utf8(const unsigned char* p,size_t length){
 for(size_t i=0;i<length;){unsigned char a=p[i];if(a<128){i++;continue;}
  size_t n=a>=0xC2&&a<=0xDF?2:a>=0xE0&&a<=0xEF?3:a>=0xF0&&a<=0xF4?4:0;
  if(!n||n>length-i)return false;
  for(size_t j=1;j<n;j++)if((p[i+j]&0xC0)!=0x80)return false;
  if((a==0xE0&&p[i+1]<0xA0)||(a==0xED&&p[i+1]>=0xA0)||(a==0xF0&&p[i+1]<0x90)||(a==0xF4&&p[i+1]>0x8F))return false;
  i+=n;
 }return true;
}
#define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))
bool asset_reader_call(struct dvm* v,const char* c,const char* m,const char* sig,dvm_ref self,const union dvm_value* a,int n,union dvm_value* out){
 if(v!=active)return false;
 if(IS("getResources","()Landroid/content/res/Resources;")){
 struct dvm_object* o=dvm__obj(v,self);bool ok=false;for(struct dvm_class* k=o?o->cls:NULL;k;k=k->super)if(!strcmp(k->name,"android/content/Context"))ok=true;
 asset_probe_log("ASSET_READER getResources self=%u context=%d ok=%d",(unsigned)self,ok?1:0,ok?1:0);
 if(!ok)return error(v,"java/lang/UnsupportedOperationException");if(!resources)resources=object(v,"android/content/res/Resources");out->l=resources;return true;}
 if(self==resources&&resources&&IS("getAssets","()Landroid/content/res/AssetManager;")){if(!manager)manager=object(v,"android/content/res/AssetManager");asset_probe_log("ASSET_READER getAssets resources=%u manager=%u",(unsigned)self,(unsigned)manager);out->l=manager;return true;}
 /* Android Context.getAssets() delegates to Resources.getAssets(); the native
  * engine obtains its AAssetManager from this call. Only a real Context
  * receiver is accepted, and the returned object is the same AssetManager
  * instance the Resources facade exposes. */
 if(IS("getAssets","()Landroid/content/res/AssetManager;")&&n==0){
  struct dvm_object* o=dvm__obj(v,self);bool ok=false;
  for(struct dvm_class* k=o?o->cls:NULL;k;k=k->super)if(k->name&&!strcmp(k->name,"android/content/Context"))ok=true;
  asset_probe_log("ASSET_READER context_getAssets self=%u context=%d",(unsigned)self,ok?1:0);
  if(ok){if(!manager)manager=object(v,"android/content/res/AssetManager");out->l=manager;return true;}
 }
 if(!strcmp(c,"java/nio/charset/Charset")&&IS("defaultCharset","()Ljava/nio/charset/Charset;")){if(!charset)charset=object(v,c);out->l=charset;return true;}
 if(!strcmp(c,"java/nio/charset/Charset")&&IS("forName","(Ljava/lang/String;)Ljava/nio/charset/Charset;")&&n==1){
  const char* name=dvm_string_utf8(v,a[0].l);if(!name)return error(v,"java/lang/NullPointerException");
  if(strlen(name)!=dvm_string_utf8_length(v,a[0].l))return error(v,"java/nio/charset/IllegalCharsetNameException");
  int index=-1;for(int i=0;i<8;i++)if(!strcasecmp(name,charset_names[i]))index=i;
  if(!strcasecmp(name,"UTF8"))index=0;if(!strcasecmp(name,"ASCII"))index=4;if(!strcasecmp(name,"ISO8859_1"))index=5;
  if(index<0)return error(v,"java/nio/charset/UnsupportedCharsetException");
  if(index==0){if(!charset)charset=object(v,c);named_charsets[0]=charset;}
  else if(!named_charsets[index])named_charsets[index]=object(v,c);
  out->l=named_charsets[index];return true;
 }
 if(!strcmp(c,"java/nio/charset/Charset")&&(IS("name","()Ljava/lang/String;")||IS("toString","()Ljava/lang/String;"))){
  for(int i=0;i<8;i++)if(self&&(self==named_charsets[i]||(i==0&&self==charset))){out->l=dvm_new_string(v,charset_names[i]);return true;}
  return false;
 }
 if(self==manager&&manager&&IS("open","(Ljava/lang/String;)Ljava/io/InputStream;")&&n==1){
 const char* name=dvm_string_utf8(v,a[0].l);if(!name)return error(v,"java/lang/NullPointerException");const asset* found=NULL;for(size_t i=0;i<sizeof assets/sizeof assets[0];i++)if(!strcmp(name,assets[i].name)){found=&assets[i];break;}
 asset_probe_log("ASSET_READER open name=%s found=%d",name?name:"(null)",found?1:0);
 if(!found)return error(v,"java/io/FileNotFoundException");if(!found->bytes)return error(v,"java/lang/UnsupportedOperationException");
 out->l=object(v,"java/io/InputStream");struct dvm_object* o=dvm__obj(v,out->l);io* p=calloc(1,sizeof *p);if(!o||!p){free(p);return error(v,"java/lang/OutOfMemoryError");}p->vm=v;p->a=found;o->data=p;o->data_destroy=release;return true;}
 struct dvm_object* o=dvm__obj(v,self);
 if(!strcmp(c,"java/util/Scanner")&&IS("<init>","(Ljava/io/InputStream;Ljava/lang/String;)V")&&n==2){
  if(!a[0].l||!a[1].l)return error(v,"java/lang/NullPointerException");
  struct dvm_object *stream=dvm__obj(v,a[0].l);
  const char *encoding=dvm_string_utf8(v,a[1].l);
  if(!encoding||strlen(encoding)!=dvm_string_utf8_length(v,a[1].l)||
     (strcasecmp(encoding,"UTF-8")&&strcasecmp(encoding,"UTF8"))||
     !o||o->data||!stream||stream->data_destroy!=release||
     !((io*)stream->data)->a)
   return error(v,"java/lang/UnsupportedOperationException");
  io *scanner=calloc(1,sizeof *scanner);
  if(!scanner)return error(v,"java/lang/OutOfMemoryError");
  scanner->vm=v;
  scanner->child=a[0].l;
  dvm_pin(v,scanner->child);
  o->data=scanner;
  o->data_destroy=release;
  return true;
 }
 if(!strcmp(m,"<init>")&&((!strcmp(c,"java/io/InputStreamReader")&&!strcmp(sig,"(Ljava/io/InputStream;Ljava/nio/charset/Charset;)V")&&n==2&&a[1].l==charset)||(!strcmp(c,"java/io/BufferedReader")&&!strcmp(sig,"(Ljava/io/Reader;)V")&&n==1))){
 struct dvm_object* child=dvm__obj(v,a[0].l);if(!o||o->data||!child||child->data_destroy!=release)return error(v,"java/lang/UnsupportedOperationException");io* p=calloc(1,sizeof *p);if(!p)return error(v,"java/lang/OutOfMemoryError");p->vm=v;p->child=a[0].l;dvm_pin(v,p->child);o->data=p;o->data_destroy=release;return true;}
 if(!o||o->data_destroy!=release)return false;io* p=o->data;io* leaf=p;
 while(leaf->child){struct dvm_object* ch=dvm__obj(v,leaf->child);if(!ch||ch->data_destroy!=release)return error(v,"java/io/IOException");leaf=ch->data;}
 if(!strcmp(c,"java/util/Scanner")){
  if(IS("close","()V")){p->closed=1;leaf->closed=1;return true;}
  if(p->closed)return error(v,"java/lang/IllegalStateException");
  if(IS("useDelimiter","(Ljava/lang/String;)Ljava/util/Scanner;")&&n==1){
   const char *pattern=dvm_string_utf8(v,a[0].l);
   if(!pattern)return error(v,"java/lang/NullPointerException");
   if(dvm_string_utf8_length(v,a[0].l)!=2||strcmp(pattern,"\\z"))
    return error(v,"java/lang/UnsupportedOperationException");
   p->whole_input=true;
   out->l=self;
   return true;
  }
  if(IS("next","()Ljava/lang/String;")){
   if(!p->whole_input)return error(v,"java/lang/UnsupportedOperationException");
   if(leaf->closed||leaf->pos==leaf->a->size)return error(v,"java/util/NoSuchElementException");
   size_t length=leaf->a->size-leaf->pos;
   const unsigned char *bytes=leaf->a->bytes+leaf->pos;
   if(!valid_utf8(bytes,length))return error(v,"java/lang/UnsupportedOperationException");
   out->l=dvm_new_string_n(v,(const char*)bytes,length);
   if(!out->l)return error(v,"java/lang/OutOfMemoryError");
   leaf->pos=leaf->a->size;
   asset_probe_log("ASSET_READER scanner_next name=%s bytes=%zu",leaf->a->name,length);
   return true;
  }
  return false;
 }
 if(IS("close","()V")){p->closed=1;leaf->closed=1;return true;}if(p->closed||leaf->closed)return error(v,"java/io/IOException");
 if(IS("available","()I")){out->i=(int)(leaf->a->size-leaf->pos);return true;}
 if(IS("readLine","()Ljava/lang/String;")){
 const unsigned char* b=leaf->a->bytes;size_t end=leaf->pos;if(end==leaf->a->size){out->l=0;return true;}
 while(end<leaf->a->size&&b[end]!=10&&b[end]!=13)end++;
 if(!valid_utf8(b+leaf->pos,end-leaf->pos))return error(v,"java/lang/UnsupportedOperationException");
 out->l=dvm_new_string_n(v,(const char*)b+leaf->pos,end-leaf->pos);leaf->pos=end;if(end<leaf->a->size){unsigned char ch=b[leaf->pos++];if(ch==13&&leaf->pos<leaf->a->size&&b[leaf->pos]==10)leaf->pos++;}return true;}
 if(!p->child&&IS("read","()I")){out->i=leaf->pos<leaf->a->size?leaf->a->bytes[leaf->pos++]:-1;return true;}
 bool ranged=IS("read","([BII)I");
 if(!p->child&&(IS("read","([B)I")||ranged)){
  if(!a||n!=(ranged?3:1))return error(v,"java/lang/IllegalArgumentException");
  struct dvm_object *array=dvm__obj(v,a[0].l);
  if(!array)return error(v,"java/lang/NullPointerException");
  if(array->kind!=DVM_OBJ_ARRAY||array->elem_kind!='B')
   return error(v,"java/lang/IllegalArgumentException");
  int64_t offset=ranged?a[1].i:0;
  int64_t length=ranged?(int64_t)a[2].i:array->length;
  if(offset<0||length<0||offset+length>array->length)
   return error(v,"java/lang/IndexOutOfBoundsException");
  if(length==0){out->i=0;return true;}
  size_t remaining=leaf->a->size-leaf->pos;
  if(!remaining){out->i=-1;return true;}
  size_t copied=remaining<(size_t)length?remaining:(size_t)length;
  memcpy((unsigned char*)array->data+(size_t)offset,leaf->a->bytes+leaf->pos,copied);
  leaf->pos+=copied;
  out->i=(int32_t)copied;
  asset_probe_log("ASSET_READER read name=%s bytes=%zu position=%zu",leaf->a->name,copied,leaf->pos);
  return true;
 }
 return false;
}
bool asset_reader_selftest(struct dvm* v){
 extern dvm_ref android_basics_application(struct dvm*);
 union dvm_value out={0},a[2]={{0},{0}};dvm_ref context=android_basics_application(v);
 if(!asset_reader_call(v,"android/content/Context","getResources","()Landroid/content/res/Resources;",context,NULL,0,&out)||!out.l)return false;
 if(!asset_reader_call(v,"android/content/res/Resources","getAssets","()Landroid/content/res/AssetManager;",out.l,NULL,0,&out)||!out.l)return false;
 dvm_ref am=out.l;a[0].l=dvm_new_string(v,"ext.txt");
 union dvm_value ca={.l=dvm_new_string(v,"utf8")};
 if(!asset_reader_call(v,"java/nio/charset/Charset","forName","(Ljava/lang/String;)Ljava/nio/charset/Charset;",0,&ca,1,&out)||!out.l)return false;
 dvm_ref utf=out.l;
 if(!asset_reader_call(v,"java/nio/charset/Charset","defaultCharset","()Ljava/nio/charset/Charset;",0,NULL,0,&out)||out.l!=utf)return false;
 ca.l=dvm_new_string(v,"UTF-16LE");
 if(!asset_reader_call(v,"java/nio/charset/Charset","forName","(Ljava/lang/String;)Ljava/nio/charset/Charset;",0,&ca,1,&out)||!out.l||out.l==utf)return false;
 if(!asset_reader_call(v,"java/nio/charset/Charset","name","()Ljava/lang/String;",out.l,NULL,0,&out)||strcmp(dvm_string_utf8(v,out.l),"UTF-16LE"))return false;
 for(int i=6;i<8;i++){ca.l=dvm_new_string(v,charset_names[i]);
  if(!asset_reader_call(v,"java/nio/charset/Charset","forName","(Ljava/lang/String;)Ljava/nio/charset/Charset;",0,&ca,1,&out)||!out.l)return false;
  if(!asset_reader_call(v,"java/nio/charset/Charset","name","()Ljava/lang/String;",out.l,NULL,0,&out)||strcmp(dvm_string_utf8(v,out.l),charset_names[i]))return false;
 }
 if(!asset_reader_call(v,"android/content/res/AssetManager","open","(Ljava/lang/String;)Ljava/io/InputStream;",am,a,1,&out)||!out.l)return false;
 dvm_ref stream=out.l;
 if(!asset_reader_call(v,"java/nio/charset/Charset","defaultCharset","()Ljava/nio/charset/Charset;",0,NULL,0,&out)||!out.l)return false;
 a[0].l=stream;a[1].l=out.l;dvm_ref reader=object(v,"java/io/InputStreamReader"),buffer=object(v,"java/io/BufferedReader");
 if(!reader||!buffer||!asset_reader_call(v,"java/io/InputStreamReader","<init>","(Ljava/io/InputStream;Ljava/nio/charset/Charset;)V",reader,a,2,&out))return false;
 a[0].l=reader;
 if(!asset_reader_call(v,"java/io/BufferedReader","<init>","(Ljava/io/Reader;)V",buffer,a,1,&out))return false;
 if(!asset_reader_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",buffer,NULL,0,&out)||!out.l)return false;
 io* source=dvm__obj(v,stream)->data;size_t end=0;
 while(end<source->a->size&&source->a->bytes[end]!=10&&source->a->bytes[end]!=13)end++;
 if(dvm_string_utf8_length(v,out.l)!=end||memcmp(dvm_string_utf8(v,out.l),source->a->bytes,end))return false;
 static const unsigned char fixture[]={0x61,0,0xE4,0xB8,0xAD,0xF0,0x9F,0x98,0x80,13,10,0x62,10};
 static const asset test_asset={"fixture-only",fixture,sizeof fixture};
 source->a=&test_asset;source->pos=0;
 if(!asset_reader_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",buffer,NULL,0,&out)||!out.l||dvm_string_utf8_length(v,out.l)!=9||memcmp(dvm_string_utf8(v,out.l),fixture,9))return false;
 if(!asset_reader_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",buffer,NULL,0,&out)||!out.l||dvm_string_utf8_length(v,out.l)!=1||*dvm_string_utf8(v,out.l)!='b')return false;
 if(!asset_reader_call(v,"java/io/BufferedReader","readLine","()Ljava/lang/String;",buffer,NULL,0,&out)||out.l)return false;
 if(!asset_reader_call(v,"java/io/BufferedReader","close","()V",buffer,NULL,0,&out))return false;
 sdk_trace_fprintf(stderr,"ASSET_READER_SELFTEST PASS original_ext_bytes=1 UTF8_nonBMP_NUL=1 CRLF=1 EOF=1 charset_alias=1 named_descriptor=1\n");
 return !dvm_exception(v);
}

int asset_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 if(o->data_destroy!=release)return DVM_GC_PAYLOAD_UNKNOWN;io *p=o->data;if(!p||p->vm!=v)return DVM_GC_PAYLOAD_INVALID;
 if(visit)visit(&p->child,u);return DVM_GC_PAYLOAD_REFS;
}
