#include "uri_runtime.h"
#include "gc_operation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
extern bool set_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);

#define URI_MAGIC 0x55524950u /* 'URIP' */
typedef struct uri_payload {
 unsigned magic;
 char *text;     /* the original Uri string, returned by toString() */
 char *scheme;   /* encoded scheme, or NULL for a relative URI */
 char *authority; /* complete encoded [userinfo@]host[:port], or NULL */
 char *host;     /* encoded host, or NULL when there is no authority */
 char *path;     /* encoded path; NULL for an opaque (non-hierarchical) URI */
 char *query;    /* encoded query without the '?'; NULL when '?' is absent */
 char *fragment; /* encoded fragment without the '#'; NULL when absent */
 bool hierarchical;
} uri_payload;
static void uri_payload_free(void *ptr){
 uri_payload *u=ptr;if(!u)return;
 free(u->text);free(u->scheme);free(u->authority);free(u->host);free(u->path);free(u->query);free(u->fragment);free(u);
}
static char *copy_range(struct dvm *v,const char *s,size_t n){
 char *copy=malloc(n+1);
 if(!copy){dvm__throw(v,"java/lang/OutOfMemoryError","Uri string");return NULL;}
 if(n)memcpy(copy,s,n);
 copy[n]=0;return copy;
}
static char *copy_string(struct dvm *v,const char *s){return s?copy_range(v,s,strlen(s)):NULL;}
/* authority = [userinfo@]host[:port]; IPv6 literals keep the bracket content. */
static char *parse_host(struct dvm *v,const char *start,const char *end){
 const char *host=start;
 for(const char *p=start;p<end;++p)if(*p=='@')host=p+1;
 if(host>=end)return copy_range(v,host,0);
 if(*host=='['){
  const char *close=memchr(host+1,']',(size_t)(end-host-1));
  if(close)return copy_range(v,host+1,(size_t)(close-host-1));
  return copy_range(v,host+1,(size_t)(end-host-1));
 }
 const char *colon=memchr(host,':',(size_t)(end-host));
 return copy_range(v,host,(size_t)((colon?colon:end)-host));
}
/* RFC 3986 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":".
 * The colon must occur before the query/fragment separators; otherwise the
 * text is a relative path (for example "/a://b" has no scheme).  The checks
 * are spelled out in ASCII on purpose: this file must not depend on the host
 * libc's locale tables just to classify a scheme. */
static bool ascii_alpha(unsigned char c){
 return (c>='A'&&c<='Z')||(c>='a'&&c<='z');
}
static bool ascii_alnum(unsigned char c){
 return ascii_alpha(c)||(c>='0'&&c<='9');
}
static const char *scheme_colon(const char *text,size_t head_end){
 if(!head_end||!ascii_alpha((unsigned char)text[0]))return NULL;
 size_t at=1;
 while(at<head_end){
  unsigned char c=(unsigned char)text[at];
  if(ascii_alnum(c)||c=='+'||c=='-'||c=='.'){++at;continue;}
  break;
 }
 return at<head_end&&text[at]==':'?text+at:NULL;
}
/* Grammar understood: [scheme:][//authority]path[?query][#fragment].  A
 * relative "/path", a network-path "//authority/path" and a bare path are
 * accepted.  A scheme that is not followed by '/' is opaque: it has no path
 * (getPath() returns null), but its query and fragment are still split the
 * same way.  The fragment is removed before the query is located, so '?'
 * or '&' inside a fragment never leaks into getQuery()/getQueryParameter(). */
static uri_payload *uri_parse(struct dvm *v,const char *text){
 size_t length=strlen(text);
 const char *hash=memchr(text,'#',length);
 size_t main_end=hash?(size_t)(hash-text):length;
 const char *question=memchr(text,'?',main_end);
 size_t head_end=question?(size_t)(question-text):main_end;
 uri_payload *u=calloc(1,sizeof *u);
 if(!u){dvm__throw(v,"java/lang/OutOfMemoryError","Uri payload");return NULL;}
 u->magic=URI_MAGIC;
 u->hierarchical=true;
 u->text=copy_string(v,text);
 if(!u->text)goto fail;
 if(hash){
  u->fragment=copy_range(v,hash+1,length-(size_t)(hash+1-text));
  if(!u->fragment)goto fail;
 }
 if(question){
  u->query=copy_range(v,question+1,main_end-(size_t)(question+1-text));
  if(!u->query)goto fail;
 }
 const char *colon=scheme_colon(text,head_end);
 if(colon){
  u->scheme=copy_range(v,text,(size_t)(colon-text));
  if(!u->scheme)goto fail;
  const char *after=colon+1;
  size_t after_length=head_end-(size_t)(after-text);
  if(after_length>=2&&after[0]=='/'&&after[1]=='/'){
   const char *authority=after+2;
   const char *authority_end=authority;
   while(authority_end<text+head_end&&*authority_end!='/'&&*authority_end!='\\')++authority_end;
   u->authority=copy_range(v,authority,(size_t)(authority_end-authority));
   if(!u->authority)goto fail;
   u->host=parse_host(v,authority,authority_end);
   if(!u->host)goto fail;
   u->path=copy_range(v,authority_end,(size_t)(text+head_end-authority_end));
   if(!u->path)goto fail;
  }else if(after_length>=1&&after[0]=='/'){ /* scheme:/path without authority */
   u->path=copy_range(v,after,after_length);
   if(!u->path)goto fail;
  }else{
   /* Opaque URI (for example mailto:user@example.com or urn:isbn:...). */
   u->hierarchical=false;
   u->path=NULL;
  }
 }else if(head_end>=2&&text[0]=='/'&&text[1]=='/'){
  const char *authority=text+2;
  const char *authority_end=authority;
  while(authority_end<text+head_end&&*authority_end!='/'&&*authority_end!='\\')++authority_end;
  u->authority=copy_range(v,authority,(size_t)(authority_end-authority));
  if(!u->authority)goto fail;
  u->host=parse_host(v,authority,authority_end);
  if(!u->host)goto fail;
  u->path=copy_range(v,authority_end,(size_t)(text+head_end-authority_end));
  if(!u->path)goto fail;
 }else{
  u->path=copy_range(v,text,head_end);
  if(!u->path)goto fail;
 }
 return u;
fail:
 uri_payload_free(u);
 if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Uri");
 return NULL;
}
static int uri_hex(char c){
 if(c>='0'&&c<='9')return c-'0';
 if(c>='a'&&c<='f')return c-'a'+10;
 if(c>='A'&&c<='F')return c-'A'+10;
 return -1;
}
/* UTF-8 percent decoding.  '+' becomes a space when plus is true (query and
 * query-parameter names/values); it stays '+' in paths.  '%2B' always becomes
 * '+'.  A '%' that is not followed by two hex digits becomes U+FFFD, matching
 * UriCodec's lenient decoding instead of silently dropping the character. */
static char *uri_decode(struct dvm *v,const char *s,size_t n,bool plus,size_t *out_length){
 if(n>((size_t)-1-1)/3){dvm__throw(v,"java/lang/OutOfMemoryError","Uri decode");return NULL;}
 char *out=malloc(n*3+1);
 if(!out){dvm__throw(v,"java/lang/OutOfMemoryError","Uri decode");return NULL;}
 size_t at=0;
 for(size_t i=0;i<n;++i){
  unsigned char c=(unsigned char)s[i];
  if(c=='%'&&i+2<n){
   int high=uri_hex(s[i+1]),low=uri_hex(s[i+2]);
   if(high>=0&&low>=0){out[at++]=(char)((high<<4)|low);i+=2;continue;}
  }
  if(c=='%'){out[at++]=(char)0xef;out[at++]=(char)0xbf;out[at++]=(char)0xbd;continue;}
  out[at++]=(c=='+'&&plus)?' ':(char)c;
 }
 out[at]=0;
 if(out_length)*out_length=at;
 return out;
}
static dvm_ref uri_decoded_range(struct dvm *v,const char *s,size_t n,bool plus){
 size_t length=0;
 char *decoded=uri_decode(v,s,n,plus,&length);
 if(!decoded)return 0;
 dvm_ref result=dvm_new_string_n(v,decoded,length);
 free(decoded);
 if(!result)dvm__throw(v,"java/lang/OutOfMemoryError","Uri decoded string");
 return result;
}
static dvm_ref uri_decoded_string(struct dvm *v,const char *s,bool plus){
 return s?uri_decoded_range(v,s,strlen(s),plus):0;
}
/* First matching parameter wins.  Keys are compared after decoding (so
 * "%61=1" answers getQueryParameter("a")); the value is either returned in
 * its original encoded form or decoded with '+' as a space.  A parameter
 * without '=' has an empty value, and a missing name returns null. */
static dvm_ref query_parameter(struct dvm *v,const char *query,const char *name,bool encoded_value){
 if(!query||!name)return 0;
 size_t name_length=strlen(name);
 const char *p=query;
 while(*p){
  const char *end=strchr(p,'&');
  if(!end)end=p+strlen(p);
  const char *equals=memchr(p,'=',(size_t)(end-p));
  const char *key_end=equals?equals:end;
  size_t key_length=0;
  char *key=uri_decode(v,p,(size_t)(key_end-p),true,&key_length);
  if(!key)return 0;
  bool match=key_length==name_length&&!memcmp(key,name,name_length);
  free(key);
  if(match){
   if(!equals)return dvm_new_string(v,"");
   const char *value=equals+1;
   size_t value_length=(size_t)(end-value);
   dvm_ref result=encoded_value?dvm_new_string_n(v,value,value_length)
                               :uri_decoded_range(v,value,value_length,true);
   if(!result)dvm__throw(v,"java/lang/OutOfMemoryError","Uri query parameter");
   return result;
  }
  if(!*end)break;
  p=end+1;
 }
 return 0;
}
/* Android uses Uri.decode(name), not form decoding, so '+' is literal.
 * Decode before insertion so alternate encodings of one key deduplicate,
 * preserving the first occurrence through the real LinkedHashSet runtime. */
static dvm_ref query_parameter_names(struct dvm *v,const char *query){
 dvm_ref set=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/LinkedHashSet"));
 union dvm_value out={0};
 if(!set||!set_runtime_call(v,"java/util/LinkedHashSet","<init>","()V",set,NULL,0,&out)||dvm_exception(v))return 0;
 if(query){
  const char *start=query;
  do{
   const char *end=strchr(start,'&');if(!end)end=start+strlen(start);
   const char *equals=memchr(start,'=',(size_t)(end-start));
   union dvm_value name={.l=uri_decoded_range(v,start,(size_t)((equals?equals:end)-start),false)};
   if(!name.l||!set_runtime_call(v,"java/util/Set","add","(Ljava/lang/Object;)Z",set,&name,1,&out)||dvm_exception(v))return 0;
   if(!*end)break;start=end+1;
  }while(*start);
 }
 union dvm_value source={.l=set};
 if(!set_runtime_call(v,"java/util/Collections","unmodifiableSet","(Ljava/util/Set;)Ljava/util/Set;",0,&source,1,&out)||dvm_exception(v))return 0;
 return out.l;
}
static uri_payload *uri_get(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","Uri receiver");return NULL;}
 if(!o->data){dvm__throw(v,"java/lang/IllegalStateException","Uri not created by Uri.parse");return NULL;}
 if(o->data_destroy!=uri_payload_free)return NULL;
 uri_payload *u=o->data;
 return u&&u->magic==URI_MAGIC?u:NULL;
}
/* Builder owns a snapshot, so mutating it cannot change the source Uri or an
 * already-built result. Keep the original authority verbatim (userinfo,
 * IPv6 brackets and port included), rather than rebuilding it from getHost. */
typedef struct uri_builder_payload {char *text,*path;bool path_set,query_cleared;} uri_builder_payload;
static void uri_builder_free(void *ptr){uri_builder_payload *b=ptr;if(b){free(b->text);free(b->path);free(b);}}
static dvm_ref uri_new(struct dvm *v,const char *text){
 uri_payload *u=uri_parse(v,text);if(!u)return 0;
 struct dvm_class *cls=dvm_find_class_or_external(v,"android/net/Uri");
 dvm_ref ref=cls?dvm_new_object(v,cls):0;struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
 if(!o){uri_payload_free(u);dvm__throw(v,"java/lang/OutOfMemoryError","Uri object");return 0;}
 o->data=u;o->data_destroy=uri_payload_free;return ref;
}
/* Android Uri.encode(path, "/"): ASCII alphanumeric and _-!.~'()* remain
 * literal, slash is explicitly allowed, all other UTF-8 bytes are escaped. */
static char *uri_encode_path(struct dvm *v,const char *text){
 size_t n=strlen(text);if(n>((size_t)-1-1)/3){dvm__throw(v,"java/lang/OutOfMemoryError","Uri path");return NULL;}
 char *encoded=malloc(n*3+1);if(!encoded){dvm__throw(v,"java/lang/OutOfMemoryError","Uri path");return NULL;}
 const char hex[]="0123456789ABCDEF";size_t at=0;
 for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)text[i];
  if(ascii_alnum(c)||strchr("_-!.~'()*/",c))encoded[at++]=(char)c;
  else{encoded[at++]='%';encoded[at++]=hex[c>>4];encoded[at++]=hex[c&15];}
 }encoded[at]=0;return encoded;
}
static char *uri_builder_text(struct dvm *v,uri_builder_payload *b){
 if(!b->path_set&&!b->query_cleared)return copy_string(v,b->text);
 const char *text=b->text;size_t n=strlen(text);
 const char *hash=memchr(text,'#',n);size_t main_end=hash?(size_t)(hash-text):n;
 const char *question=memchr(text,'?',main_end);size_t head_end=question?(size_t)(question-text):main_end;
 if(!b->path_set){
  size_t suffix=n-main_end,total=head_end+suffix;
  char *result=malloc(total+1);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","Uri clearQuery");return NULL;}
  memcpy(result,text,head_end);memcpy(result+head_end,text+main_end,suffix);result[total]=0;return result;
 }
 const char *colon=scheme_colon(text,head_end);size_t prefix=colon?(size_t)(colon+1-text):0;
 bool authority=head_end>=prefix+2&&text[prefix]=='/'&&text[prefix+1]=='/';
 if(authority){prefix+=2;while(prefix<head_end&&text[prefix]!='/'&&text[prefix]!='\\')prefix++;}
 /* A path operation changes an opaque builder to hierarchical, so discard
  * the old scheme-specific part; query/fragment still belong to the Uri. */
 const char *path=b->path?b->path:"";size_t path_n=strlen(path);
 bool leading=(authority||colon)&&path_n&&path[0]!='/';
 size_t tail=question&&!b->query_cleared?(size_t)(question-text):main_end;
 if(prefix>SIZE_MAX-path_n-2||prefix+path_n+2>SIZE_MAX-(n-tail)){dvm__throw(v,"java/lang/OutOfMemoryError","Uri build");return NULL;}
 size_t total=prefix+path_n+(leading?1:0)+(n-tail);
 char *result=malloc(total+1);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","Uri build");return NULL;}
 size_t at=prefix;memcpy(result,text,prefix);if(leading)result[at++]='/';
 memcpy(result+at,path,path_n);at+=path_n;memcpy(result+at,text+tail,n-tail);result[total]=0;return result;
}
/* Read-only identity check for other runtimes (for example Intent.setData).
 * Never throws and never dereferences a foreign payload: a reference is a Uri
 * only when this module owns it and the magic matches. */
bool uri_runtime_is_uri(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
 if(!o||o->data_destroy!=uri_payload_free)return false;
 uri_payload *u=o->data;
 return u&&u->magic==URI_MAGIC;
}
/* Java component constructors receive decoded text: quote percent/illegal
 * delimiters, keep literal '+', and preserve Unicode for toString(). */
static char *java_uri_quote(struct dvm *v,const char *text,int component){
 if(!text)return NULL;
 size_t n=strlen(text);if(n>(SIZE_MAX-1)/3){dvm__throw(v,"java/lang/OutOfMemoryError","URI component");return NULL;}
 char *out=malloc(n*3+1);if(!out){dvm__throw(v,"java/lang/OutOfMemoryError","URI component");return NULL;}
 const char *hex="0123456789ABCDEF";size_t at=0;
 for(size_t i=0;i<n;i++){
  unsigned char ch=(unsigned char)text[i];
  bool allowed=ascii_alnum(ch)||ch>=128||strchr("-._~!$&'()*+,;=:@",ch)!=NULL;
  if(component==0&&(ch=='['||ch==']'))allowed=true;
  if(component!=0&&ch=='/')allowed=true;
  if(component==2&&(ch=='?'||ch=='['||ch==']'))allowed=true;
  if(allowed)out[at++]=(char)ch;
  else{out[at++]='%';out[at++]=hex[ch>>4];out[at++]=hex[ch&15];}
 }out[at]=0;return out;
}
static uri_payload *java_uri_from_components(struct dvm *v,const union dvm_value *a){
 const char *part[5];char *quoted[4]={0};uri_payload *result=NULL;char *text=NULL;
 for(int i=0;i<5;i++){
  part[i]=a[i].l?dvm_string_utf8(v,a[i].l):NULL;
  if(a[i].l&&!part[i]){dvm__throw(v,"java/lang/IllegalArgumentException","URI component must be String");goto done;}
 }
 if(part[0]){
  bool valid=ascii_alpha((unsigned char)part[0][0]);
  for(size_t i=1;valid&&part[0][i];i++)if(!ascii_alnum((unsigned char)part[0][i])&&!strchr("+-.",part[0][i]))valid=false;
  if(!valid){dvm__throw(v,"java/net/URISyntaxException","invalid URI scheme");goto done;}
 }
 if(part[0]&&part[2]&&*part[2]&&*part[2]!='/'){
  dvm__throw(v,"java/net/URISyntaxException","Relative path in absolute URI");goto done;
 }
 for(int i=1;i<5;i++)if(part[i]){quoted[i-1]=java_uri_quote(v,part[i],i==1?0:(i==2?1:2));if(!quoted[i-1])goto done;}
 size_t length=part[0]?strlen(part[0])+1:0;
 for(int i=1;i<5;i++)if(quoted[i-1]){
  size_t extra=strlen(quoted[i-1])+(i==1?2:(i==2?0:1));
  if(extra>SIZE_MAX-length-1){dvm__throw(v,"java/lang/OutOfMemoryError","URI components");goto done;}length+=extra;
 }
 text=malloc(length+1);if(!text){dvm__throw(v,"java/lang/OutOfMemoryError","URI components");goto done;}
 size_t at=0;
 if(part[0]){size_t n=strlen(part[0]);memcpy(text+at,part[0],n);at+=n;text[at++]=':';}
 for(int i=1;i<5;i++)if(quoted[i-1]){
  if(i==1){text[at++]='/';text[at++]='/';}else if(i==3)text[at++]='?';else if(i==4)text[at++]='#';
  size_t n=strlen(quoted[i-1]);memcpy(text+at,quoted[i-1],n);at+=n;
 }text[at]=0;result=uri_parse(v,text);
done:
 free(text);for(int i=0;i<4;i++)free(quoted[i]);return result;
}
#define IS(method,signature) (!strcmp(m,method)&&!strcmp(sig,signature))
static bool uri_runtime_call_gc9_operation(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,
 const union dvm_value *a,int n,union dvm_value *out){
 memset(out,0,sizeof *out);
 if(!strcmp(c,"android/net/Uri$Builder")){
  struct dvm_object *o=dvm__obj(v,self);
  if(!o){dvm__throw(v,"java/lang/NullPointerException","Uri.Builder receiver");return true;}
  if(IS("<init>","()V")){
   if(o->data){dvm__throw(v,"java/lang/IllegalStateException","Uri.Builder already initialized");return true;}
   uri_builder_payload *b=calloc(1,sizeof *b);if(!b){dvm__throw(v,"java/lang/OutOfMemoryError","Uri.Builder");return true;}
   b->text=copy_string(v,"");if(!b->text){uri_builder_free(b);return true;}o->data=b;o->data_destroy=uri_builder_free;return true;
  }
  if(o->data_destroy!=uri_builder_free||!o->data){dvm__throw(v,"java/lang/IllegalStateException","Uri.Builder not initialized");return true;}
  uri_builder_payload *b=o->data;
  if(IS("clearQuery","()Landroid/net/Uri$Builder;")&&n==0){b->query_cleared=true;out->l=self;return true;}
  if((IS("path","(Ljava/lang/String;)Landroid/net/Uri$Builder;")||IS("encodedPath","(Ljava/lang/String;)Landroid/net/Uri$Builder;"))&&n==1){
   const char *path=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
   if(a[0].l&&!path){dvm__throw(v,"java/lang/IllegalArgumentException","Uri.Builder path must be String");return true;}
   char *replacement=path?(!strcmp(m,"path")?uri_encode_path(v,path):copy_string(v,path)):NULL;
   if(path&&!replacement)return true;free(b->path);b->path=replacement;b->path_set=true;out->l=self;return true;
  }
  if(IS("build","()Landroid/net/Uri;")){char *text=uri_builder_text(v,b);if(text){out->l=uri_new(v,text);free(text);}return true;}
  return false;
 }
 if(strcmp(c,"android/net/Uri")&&strcmp(c,"java/net/URI"))return false;
 if(!strcmp(c,"android/net/Uri")&&IS("decode","(Ljava/lang/String;)Ljava/lang/String;")&&n==1){
  if(!a[0].l)return true;
  const char *text=dvm_string_utf8(v,a[0].l);
  if(!text){dvm__throw(v,"java/lang/IllegalArgumentException","Uri decode text must be a String");return true;}
  out->l=uri_decoded_range(v,text,dvm_string_utf8_length(v,a[0].l),false);
  return true;
 }
 if(IS("parse","(Ljava/lang/String;)Landroid/net/Uri;")&&n==1){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Uri text");return true;}
  const char *text=dvm_string_utf8(v,a[0].l);
  if(!text){dvm__throw(v,"java/lang/IllegalArgumentException","Uri text must be a String");return true;}
  uri_payload *u=uri_parse(v,text);
  if(!u)return true;
  struct dvm_class *cls=dvm_find_class_or_external(v,"android/net/Uri");
  dvm_ref ref=cls?dvm_new_object(v,cls):0;
  struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
  if(!o){uri_payload_free(u);dvm__throw(v,"java/lang/OutOfMemoryError","Uri object");return true;}
  o->data=u;o->data_destroy=uri_payload_free;out->l=ref;return true;
 }
 if(!strcmp(c,"java/net/URI")&&IS("<init>","(Ljava/lang/String;)V")&&n==1){
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","URI text");return true;}
  const char *text=dvm_string_utf8(v,a[0].l);
  if(!text){dvm__throw(v,"java/net/URISyntaxException","URI text must be a String");return true;}
  struct dvm_object *o=dvm__obj(v,self);
  if(!o){dvm__throw(v,"java/lang/NullPointerException","URI receiver");return true;}
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","URI already initialized");return true;}
  uri_payload *parsed=uri_parse(v,text);
  if(!parsed){if(!dvm_exception(v))dvm__throw(v,"java/net/URISyntaxException","invalid URI");return true;}
  o->data=parsed;o->data_destroy=uri_payload_free;return true;
 }
 if(!strcmp(c,"java/net/URI")&&IS("<init>","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")&&n==5){
  struct dvm_object *o=dvm__obj(v,self);
  if(!o){dvm__throw(v,"java/lang/NullPointerException","URI receiver");return true;}
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","URI already initialized");return true;}
  if(!a)return false;uri_payload *parsed=java_uri_from_components(v,a);
  if(parsed){o->data=parsed;o->data_destroy=uri_payload_free;}return true;
 }
 uri_payload *u=uri_get(v,self);
 if(!u)return dvm_exception(v)!=0;
 if(IS("buildUpon","()Landroid/net/Uri$Builder;")){
  uri_builder_payload *b=calloc(1,sizeof *b);if(!b){dvm__throw(v,"java/lang/OutOfMemoryError","Uri.Builder");return true;}
  b->text=copy_string(v,u->text);if(!b->text){uri_builder_free(b);return true;}
  struct dvm_class *cls=dvm_find_class_or_external(v,"android/net/Uri$Builder");
  dvm_ref ref=cls?dvm_new_object(v,cls):0;struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
  if(!o){uri_builder_free(b);dvm__throw(v,"java/lang/OutOfMemoryError","Uri.Builder object");return true;}
  o->data=b;o->data_destroy=uri_builder_free;out->l=ref;return true;
 }
 if(IS("toString","()Ljava/lang/String;")){out->l=dvm_new_string(v,u->text?u->text:"");return true;}
 if(IS("getScheme","()Ljava/lang/String;")){out->l=u->scheme?dvm_new_string(v,u->scheme):0;return true;}
 if(IS("getAuthority","()Ljava/lang/String;")||IS("getRawAuthority","()Ljava/lang/String;")||IS("getEncodedAuthority","()Ljava/lang/String;")){
  const char *authority=u->authority;
  if(!strcmp(c,"java/net/URI")&&authority&&!*authority)authority=NULL;
  if(authority)out->l=!strcmp(m,"getAuthority")?uri_decoded_string(v,authority,false):dvm_new_string(v,authority);
  return true;
 }
 if(IS("getHost","()Ljava/lang/String;")){out->l=u->host?dvm_new_string(v,u->host):0;return true;}
 if(IS("getRawPath","()Ljava/lang/String;")){out->l=u->path?dvm_new_string(v,u->path):0;return true;}
 if(IS("getRawQuery","()Ljava/lang/String;")){out->l=u->query?dvm_new_string(v,u->query):0;return true;}
 if(IS("getRawFragment","()Ljava/lang/String;")){out->l=u->fragment?dvm_new_string(v,u->fragment):0;return true;}
 if(IS("isAbsolute","()Z")){out->i=u->scheme!=NULL;return true;}
 if(IS("isOpaque","()Z")){out->i=!u->hierarchical;return true;}
 if(IS("getPort","()I")){out->i=-1;return true;} /* port is not stored; undefined is -1 */
 /* Decoded getters: getPath/getQuery/getFragment decode UTF-8 escapes.  The
  * query uses '+' as a space; paths keep a literal '+'. */
 if(IS("getPath","()Ljava/lang/String;")||IS("getEncodedPath","()Ljava/lang/String;")){
  bool encoded=!strcmp(m,"getEncodedPath");
  out->l=!u->path?0:(encoded?dvm_new_string(v,u->path):uri_decoded_string(v,u->path,false));
  return true;
 }
 if(IS("getQuery","()Ljava/lang/String;")||IS("getEncodedQuery","()Ljava/lang/String;")){
  bool encoded=!strcmp(m,"getEncodedQuery");
  out->l=!u->query?0:(encoded?dvm_new_string(v,u->query):uri_decoded_string(v,u->query,strcmp(c,"java/net/URI")!=0));
  return true;
 }
 if(IS("getFragment","()Ljava/lang/String;")||IS("getEncodedFragment","()Ljava/lang/String;")){
  bool encoded=!strcmp(m,"getEncodedFragment");
  out->l=!u->fragment?0:(encoded?dvm_new_string(v,u->fragment):uri_decoded_string(v,u->fragment,false));
  return true;
 }
 if(!strcmp(c,"android/net/Uri")&&IS("getQueryParameterNames","()Ljava/util/Set;")&&n==0){
  if(!u->hierarchical){dvm__throw(v,"java/lang/UnsupportedOperationException","This isn't a hierarchical URI.");return true;}
  out->l=query_parameter_names(v,u->query);
  if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Uri query parameter names");
  return true;
 }
 if((IS("getQueryParameter","(Ljava/lang/String;)Ljava/lang/String;")||
     IS("getEncodedQueryParameter","(Ljava/lang/String;)Ljava/lang/String;"))&&n==1){
  /* Android rejects query-parameter access on an opaque URI. */
  if(!u->hierarchical){dvm__throw(v,"java/lang/UnsupportedOperationException","This isn't a hierarchical URI.");return true;}
  const char *name=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
  if(name)out->l=query_parameter(v,u->query,name,!strcmp(m,"getEncodedQueryParameter"));
  return true;
 }
 return false;
}
static const char *uri_string_or_null(struct dvm *v,dvm_ref ref){
 return ref?dvm_string_utf8(v,ref):NULL;
}
/* Name of the pending exception, for diagnostics only. */
static const char *uri_exception_name(struct dvm *v){
 dvm_ref e=dvm_exception(v);
 struct dvm_object *o=e?dvm__obj(v,e):NULL;
 return o&&o->cls&&o->cls->name?o->cls->name:"(none)";
}
static void uri_check_fail(struct dvm *v,const char *api,const char *name,
 const char *want,const char *got){
 sdk_trace_fprintf(stderr,
  "URI_RUNTIME_SELFTEST CHECK_FAIL api=%s name=%s want=%s got=%s exception=%s\n",
  api,name?name:"-",want?want:"<null>",got?got:"<null>",uri_exception_name(v));
}
static bool get_text(struct dvm *v,dvm_ref uri,const char *method,const char *want){
 union dvm_value out={0};
 if(!uri_runtime_call(v,"android/net/Uri",method,"()Ljava/lang/String;",uri,NULL,0,&out)){
  uri_check_fail(v,method,NULL,want,"<unsupported>");
  return false;
 }
 if(dvm_exception(v)){uri_check_fail(v,method,NULL,want,"<exception>");return false;}
 const char *got=uri_string_or_null(v,out.l);
 if(got&&!strcmp(got,want))return true;
 uri_check_fail(v,method,NULL,want,got);
 return false;
}
static bool get_null(struct dvm *v,dvm_ref uri,const char *method){
 union dvm_value out={0};
 if(!uri_runtime_call(v,"android/net/Uri",method,"()Ljava/lang/String;",uri,NULL,0,&out)){
  uri_check_fail(v,method,NULL,"<null>","<unsupported>");
  return false;
 }
 if(dvm_exception(v)){uri_check_fail(v,method,NULL,"<null>","<exception>");return false;}
 if(out.l==0)return true;
 uri_check_fail(v,method,NULL,"<null>",uri_string_or_null(v,out.l));
 return false;
}
static bool get_parameter_common(struct dvm *v,dvm_ref uri,const char *name,
 const char *want,bool encoded){
 const char *api=encoded?"getEncodedQueryParameter":"getQueryParameter";
 union dvm_value arg={.l=dvm_new_string(v,name)},out={0};
 if(!arg.l){uri_check_fail(v,api,name,want,"<allocation>");return false;}
 if(!uri_runtime_call(v,"android/net/Uri",api,"(Ljava/lang/String;)Ljava/lang/String;",uri,&arg,1,&out)){
  uri_check_fail(v,api,name,want,"<unsupported>");
  return false;
 }
 if(dvm_exception(v)){uri_check_fail(v,api,name,want,"<exception>");return false;}
 const char *got=uri_string_or_null(v,out.l);
 if(got&&!strcmp(got,want))return true;
 uri_check_fail(v,api,name,want,got);
 return false;
}
static bool get_parameter(struct dvm *v,dvm_ref uri,const char *name,const char *want){
 return get_parameter_common(v,uri,name,want,false);
}
static bool get_encoded_parameter(struct dvm *v,dvm_ref uri,const char *name,const char *want){
 return get_parameter_common(v,uri,name,want,true);
}
static bool parameter_absent(struct dvm *v,dvm_ref uri,const char *name){
 union dvm_value arg={.l=dvm_new_string(v,name)},out={0};
 if(!arg.l){uri_check_fail(v,"getQueryParameter",name,"<null>","<allocation>");return false;}
 if(!uri_runtime_call(v,"android/net/Uri","getQueryParameter","(Ljava/lang/String;)Ljava/lang/String;",uri,&arg,1,&out)){
  uri_check_fail(v,"getQueryParameter",name,"<null>","<unsupported>");
  return false;
 }
 if(dvm_exception(v)){uri_check_fail(v,"getQueryParameter",name,"<null>","<exception>");return false;}
 if(out.l==0)return true;
 uri_check_fail(v,"getQueryParameter",name,"<null>",uri_string_or_null(v,out.l));
 return false;
}
/* Accept the internal slash name as well as a descriptor-shaped name, so a
 * runtime-defined exception class cannot make the assertion fail by spelling
 * alone. */
static bool uri_exception_name_is(const char *actual,const char *want){
 if(!actual||!want)return false;
 if(!strcmp(actual,want))return true;
 if(actual[0]=='L'&&!strncmp(actual+1,want,strlen(want))&&
    !strcmp(actual+1+strlen(want),";"))return true;
 return strstr(actual,want)!=NULL;
}
static bool parameter_throws(struct dvm *v,dvm_ref uri,const char *name,const char *type){
 union dvm_value arg={.l=dvm_new_string(v,name)},out={0};
 if(!arg.l){uri_check_fail(v,"getQueryParameter",name,type,"<allocation>");return false;}
 if(!uri_runtime_call(v,"android/net/Uri","getQueryParameter","(Ljava/lang/String;)Ljava/lang/String;",uri,&arg,1,&out)){
  uri_check_fail(v,"getQueryParameter",name,type,"<unsupported>");
  return false;
 }
 if(!dvm_exception(v)){
  uri_check_fail(v,"getQueryParameter",name,type,"<no exception>");
  return false;
 }
 const char *actual=uri_exception_name(v);
 bool ok=uri_exception_name_is(actual,type);
 dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,
  "URI_RUNTIME_SELFTEST CHECK api=getQueryParameter name=%s expected_exception=%s actual_exception=%s %s\n",
  name,type,actual,ok?"PASS":"FAIL");
 return ok;
}
static bool uri_selftest_fail(struct dvm *v,int line){
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST FAIL line=%d exception=%s\n",line,uri_exception_name(v));
 return false;
}
static bool java_uri_expect(struct dvm *v,dvm_ref uri,const char *method,const char *want){
 union dvm_value out={0};
 if(!uri_runtime_call(v,"java/net/URI",method,"()Ljava/lang/String;",uri,NULL,0,&out)||dvm_exception(v))return false;
 const char *got=out.l?dvm_string_utf8(v,out.l):NULL;
 bool okay=want?(got&&!strcmp(got,want)):!out.l;
 if(!okay)uri_check_fail(v,method,"java.net.URI",want,got);
 return okay;
}
static bool uri_query_names_selftest(struct dvm *v){
 static const char *ordered[]={"z","a","plus+name","plus name","","word","\xe4\xb8\xad"};
 static const struct {const char *url;size_t count;bool complex;} cases[]={
  {"https://host/path",0,false},{"https://host/path?",1,false},
  {"https://host/path?&",1,false},
  {"https://host/path?z=1&%61=2&a=3&plus+name=x&plus%20name=x&=empty&&word&a=4&%E4%B8%AD=x&z=2&#not_a_key=1",7,true}
 };
 for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
  union dvm_value arg={.l=dvm_new_string(v,cases[i].url)},out={0};
  if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
  dvm_ref uri=out.l;
  if(!uri_runtime_call(v,"android/net/Uri","getQueryParameterNames","()Ljava/util/Set;",uri,NULL,0,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
  dvm_ref set=out.l;
  if(!set_runtime_call(v,"java/util/Set","size","()I",set,NULL,0,&out)||dvm_exception(v)||out.i!=(int)cases[i].count)return uri_selftest_fail(v,__LINE__);
  if(!set_runtime_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",set,NULL,0,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
  dvm_ref iterator=out.l;
  for(size_t j=0;j<cases[i].count;j++){
   if(!set_runtime_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",iterator,NULL,0,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
   const char *got=dvm_string_utf8(v,out.l),*want=cases[i].complex?ordered[j]:"";
   if(!got||strcmp(got,want))return uri_selftest_fail(v,__LINE__);
  }
  if(!set_runtime_call(v,"java/util/Iterator","hasNext","()Z",iterator,NULL,0,&out)||dvm_exception(v)||out.i)return uri_selftest_fail(v,__LINE__);
  const char *mutators[]={"add","remove","clear"};
  for(int k=0;k<3;k++){
   arg.l=dvm_new_string(v,"new-key");
   if(!set_runtime_call(v,"java/util/Set",mutators[k],k==2?"()V":"(Ljava/lang/Object;)Z",set,&arg,k==2?0:1,&out)||!dvm_exception(v)||strcmp(uri_exception_name(v),"java/lang/UnsupportedOperationException"))return uri_selftest_fail(v,__LINE__);
   dvm_clear_exception(v);
  }
 }
 union dvm_value arg={.l=dvm_new_string(v,"mailto:user@example.org?a=1")},out={0};
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
 if(!uri_runtime_call(v,"android/net/Uri","getQueryParameterNames","()Ljava/util/Set;",out.l,NULL,0,&out)||!dvm_exception(v)||strcmp(uri_exception_name(v),"java/lang/UnsupportedOperationException"))return uri_selftest_fail(v,__LINE__);
 dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"URI_QUERY_NAMES_SELFTEST PASS cases=5 decoded_dedup=1 ordered=1 utf8=1 literal_plus=1 first_equals=1 absent_empty_query=1 trailing_ampersand=1 fragment_excluded=1 unmodifiable_add_remove_clear=1 opaque_rejected=1\n");
 return true;
}
static bool java_uri_authority_selftest(struct dvm *v){
 static const struct {const char *text,*raw,*decoded;} cases[]={
  {"https://user%40name:pw@[2001:db8::1]:8443/a?q=a+b#f","user%40name:pw@[2001:db8::1]:8443","user@name:pw@[2001:db8::1]:8443"},
  {"//user+name@host:9/path","user+name@host:9","user+name@host:9"},
  {"https://h%6fst/path","h%6fst","host"},
  {"file:///tmp/x",NULL,NULL},{"relative/path",NULL,NULL},
  {"mailto:user@example.org",NULL,NULL},{"https://host?x=1#fragment","host","host"}
 };
 for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
  dvm_ref uri=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/URI"));
  union dvm_value arg={.l=dvm_new_string(v,cases[i].text)},out={0};
  if(!uri||!arg.l||!uri_runtime_call(v,"java/net/URI","<init>","(Ljava/lang/String;)V",uri,&arg,1,&out)||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
  if(!java_uri_expect(v,uri,"getRawAuthority",cases[i].raw)||!java_uri_expect(v,uri,"getAuthority",cases[i].decoded))return uri_selftest_fail(v,__LINE__);
  if(i==0&&!java_uri_expect(v,uri,"getQuery","q=a+b"))return uri_selftest_fail(v,__LINE__);
 }
 const char *parts[]={"https","user:name@[2001:db8::1]:8443","/a b/%","q=a+b&x=%2F","frag #?"};
 union dvm_value args[5],out={0};
 for(int i=0;i<5;i++){args[i].l=dvm_new_string(v,parts[i]);if(!args[i].l)return uri_selftest_fail(v,__LINE__);}
 dvm_ref uri=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/URI"));
 const char *ctor="(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V";
 if(!uri||!uri_runtime_call(v,"java/net/URI","<init>",ctor,uri,args,5,&out)||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!java_uri_expect(v,uri,"toString","https://user:name@[2001:db8::1]:8443/a%20b/%25?q=a+b&x=%252F#frag%20%23?")||
    !java_uri_expect(v,uri,"getRawAuthority",parts[1])||!java_uri_expect(v,uri,"getPath",parts[2])||
    !java_uri_expect(v,uri,"getQuery",parts[3])||!java_uri_expect(v,uri,"getFragment",parts[4]))return uri_selftest_fail(v,__LINE__);
 memset(args,0,sizeof args);args[2].l=dvm_new_string(v,"relative path");
 uri=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/URI"));
 if(!uri||!uri_runtime_call(v,"java/net/URI","<init>",ctor,uri,args,5,&out)||dvm_exception(v)||!java_uri_expect(v,uri,"toString","relative%20path")||!java_uri_expect(v,uri,"getAuthority",NULL))return uri_selftest_fail(v,__LINE__);
 args[0].l=dvm_new_string(v,"https");
 uri=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/URI"));
 if(!uri||!uri_runtime_call(v,"java/net/URI","<init>",ctor,uri,args,5,&out)||!dvm_exception(v)||strcmp(uri_exception_name(v),"java/net/URISyntaxException"))return uri_selftest_fail(v,__LINE__);
 dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"JAVA_URI_AUTHORITY_SELFTEST PASS parse_cases=7 raw_decoded=1 userinfo_ipv6_port=1 absent_empty_authority=1 query_plus_preserved=1 ctor5=1 component_quoting=1 percent_quoted=1 relative=1 absolute_relative_path_rejected=1\n");
 return true;
}
bool uri_runtime_builder_selftest(struct dvm *v){
 static const struct {const char *source,*path,*want;bool encoded;} cases[]={
  {"biligame://portal","/gsc_wiki_game_tip_library/WikiGameTipActivity","biligame://portal/gsc_wiki_game_tip_library/WikiGameTipActivity",false},
  {"https://user:pw@[2001:db8::1]:8443/old?p=%2F+#f%20x","new path/%?+#","https://user:pw@[2001:db8::1]:8443/new%20path/%25%3F%2B%23?p=%2F+#f%20x",false},
  {"https://host:123/old?q=x#f","/a%2Fb/%E4%B8%AD","https://host:123/a%2Fb/%E4%B8%AD?q=x#f",true},
  {"//user@host:9/old?x#f","relative","//user@host:9/relative?x#f",false},
  {"old/path?x#f","next path","next%20path?x#f",false},
  {"scheme:/old?x#f","relative","scheme:/relative?x#f",false},
  {"https://host/old?x#f",NULL,"https://host?x#f",false},
  {"https://host/old?x#f","","https://host?x#f",true},
  {"https://host/old","/\xe4\xb8\xad\xe6\x96\x87/\xf0\x9f\x98\x80","https://host/%E4%B8%AD%E6%96%87/%F0%9F%98%80",false},
  {"scheme:opaque#fragment","new","scheme:/new#fragment",false}
 };
 for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
  union dvm_value arg={.l=dvm_new_string(v,cases[i].source)},out={0};
  if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
  dvm_ref original=out.l;
  if(!uri_runtime_call(v,"android/net/Uri","buildUpon","()Landroid/net/Uri$Builder;",original,NULL,0,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
  dvm_ref builder=out.l;
  /* An unchanged build must preserve every original encoded byte. */
  if(!uri_runtime_call(v,"android/net/Uri$Builder","build","()Landroid/net/Uri;",builder,NULL,0,&out)||dvm_exception(v)||!out.l||!get_text(v,out.l,"toString",cases[i].source))return uri_selftest_fail(v,__LINE__);
  dvm_ref snapshot=out.l;
  arg.l=cases[i].path?dvm_new_string(v,cases[i].path):0;
  if(!uri_runtime_call(v,"android/net/Uri$Builder",cases[i].encoded?"encodedPath":"path","(Ljava/lang/String;)Landroid/net/Uri$Builder;",builder,&arg,1,&out)||dvm_exception(v)||out.l!=builder)return uri_selftest_fail(v,__LINE__);
  if(!uri_runtime_call(v,"android/net/Uri$Builder","build","()Landroid/net/Uri;",builder,NULL,0,&out)||dvm_exception(v)||!out.l||!get_text(v,out.l,"toString",cases[i].want))return uri_selftest_fail(v,__LINE__);
  if(!get_text(v,original,"toString",cases[i].source)||!get_text(v,snapshot,"toString",cases[i].source))return uri_selftest_fail(v,__LINE__);
 }
 /* Direct public Builder construction is usable without Uri.parse. */
 struct dvm_class *cls=dvm_find_class_or_external(v,"android/net/Uri$Builder");dvm_ref b=cls?dvm_new_object(v,cls):0;
 union dvm_value arg={.l=dvm_new_string(v,"a b/c")},out={0};
 if(!b||!arg.l||!uri_runtime_call(v,"android/net/Uri$Builder","<init>","()V",b,NULL,0,&out)||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!uri_runtime_call(v,"android/net/Uri$Builder","path","(Ljava/lang/String;)Landroid/net/Uri$Builder;",b,&arg,1,&out)||dvm_exception(v)||out.l!=b)return uri_selftest_fail(v,__LINE__);
 if(!uri_runtime_call(v,"android/net/Uri$Builder","build","()Landroid/net/Uri;",b,NULL,0,&out)||dvm_exception(v)||!out.l||!get_text(v,out.l,"toString","a%20b/c"))return uri_selftest_fail(v,__LINE__);
 const char *query_cases[]={"https://user@host:99/old?a=b#f?keep","https://host/old?#f","relative?a=b#f","https://host/old#f?keep"};
 const char *cleared[]={"https://user@host:99/old#f?keep","https://host/old#f","relative#f","https://host/old#f?keep"};
 for(unsigned i=0;i<4;i++){
  arg.l=dvm_new_string(v,query_cases[i]);
  if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
  dvm_ref source=out.l;
  if(!uri_runtime_call(v,"android/net/Uri","buildUpon","()Landroid/net/Uri$Builder;",source,NULL,0,&out)||dvm_exception(v)||!out.l)return uri_selftest_fail(v,__LINE__);
  b=out.l;
  if(!uri_runtime_call(v,"android/net/Uri$Builder","clearQuery","()Landroid/net/Uri$Builder;",b,NULL,0,&out)||dvm_exception(v)||out.l!=b)return uri_selftest_fail(v,__LINE__);
  if(!uri_runtime_call(v,"android/net/Uri$Builder","build","()Landroid/net/Uri;",b,NULL,0,&out)||dvm_exception(v)||!out.l||!get_text(v,out.l,"toString",cleared[i])||!get_text(v,source,"toString",query_cases[i]))return uri_selftest_fail(v,__LINE__);
  arg.l=dvm_new_string(v,"/new");
  if(!arg.l||!uri_runtime_call(v,"android/net/Uri$Builder","path","(Ljava/lang/String;)Landroid/net/Uri$Builder;",b,&arg,1,&out)||dvm_exception(v)||out.l!=b)return uri_selftest_fail(v,__LINE__);
  if(!uri_runtime_call(v,"android/net/Uri$Builder","build","()Landroid/net/Uri;",b,NULL,0,&out)||dvm_exception(v)||!out.l||!get_null(v,out.l,"getQuery")||!get_text(v,out.l,"getPath","/new"))return uri_selftest_fail(v,__LINE__);
 }
 sdk_trace_fprintf(stderr,"URI_BUILDER_SELFTEST PASS cases=15 snapshot_identity=1 source_immutable=1 authority_preserved=1 query_fragment_preserved=1 path_utf8_percent=1 encoded_path=1 null_empty_path=1 constructor=1 clear_query=1\n");return true;
}
bool uri_runtime_selftest(struct dvm *v){
 union dvm_value arg={0},out={0};
 if(!uri_runtime_call(v,"android/net/Uri","decode","(Ljava/lang/String;)Ljava/lang/String;",0,&arg,1,&out)||out.l||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 arg.l=dvm_new_string(v,"a+b%20c%2B%25");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","decode","(Ljava/lang/String;)Ljava/lang/String;",0,&arg,1,&out)||dvm_exception(v)||!out.l||strcmp(dvm_string_utf8(v,out.l),"a+b c+%"))return uri_selftest_fail(v,__LINE__);
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE full\n");
 const char *full="https://user@example.com:8443/a/b?x=1&y=hello%20world&z";
 arg.l=dvm_new_string(v,full);
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 dvm_ref uri=out.l;
 if(!uri||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"toString",full))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getScheme","https"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getHost","example.com"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getPath","/a/b"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getEncodedPath","/a/b"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getQuery","x=1&y=hello world&z"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getEncodedQuery","x=1&y=hello%20world&z"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"x","1"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"y","hello world"))return uri_selftest_fail(v,__LINE__);
 if(!get_encoded_parameter(v,uri,"y","hello%20world"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"z",""))return uri_selftest_fail(v,__LINE__);
 if(!parameter_absent(v,uri,"missing"))return uri_selftest_fail(v,__LINE__);
 /* Duplicate keys resolve to the first value; only the first '=' splits. */
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE duplicate\n");
 arg.l=dvm_new_string(v,"https://host/p?dup=first&dup=second&eq=a=b&empty=");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"dup","first"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"eq","a=b"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"empty",""))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getQuery","dup=first&dup=second&eq=a=b&empty="))return uri_selftest_fail(v,__LINE__);
 /* '+' in the query is a space, while %2B is a literal plus.  Keys are
  * matched after percent decoding, and the fragment is not part of query. */
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE encoded\n");
 arg.l=dvm_new_string(v,"https://host/path%20x?state=a%2Bb&q=a+b&name=%E4%B8%AD%E6%96%87&%61=1&dup=first&dup=second&empty=&flag#frag%20ment");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getPath","/path x"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getEncodedPath","/path%20x"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getQuery","state=a+b&q=a b&name=" "\xe4\xb8\xad" "\xe6\x96\x87" "&a=1&dup=first&dup=second&empty=&flag"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getEncodedQuery","state=a%2Bb&q=a+b&name=%E4%B8%AD%E6%96%87&%61=1&dup=first&dup=second&empty=&flag"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"state","a+b"))return uri_selftest_fail(v,__LINE__);
 if(!get_encoded_parameter(v,uri,"state","a%2Bb"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"q","a b"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"name","\xe4\xb8\xad" "\xe6\x96\x87"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"a","1"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"dup","first"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"empty",""))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"flag",""))return uri_selftest_fail(v,__LINE__);
 if(!parameter_absent(v,uri,"missing"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getFragment","frag ment"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getEncodedFragment","frag%20ment"))return uri_selftest_fail(v,__LINE__);
 /* A fragment after the query must not leak into the value. */
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE fragment\n");
 arg.l=dvm_new_string(v,"https://host/p?x=1#frag");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getPath","/p")||!get_text(v,uri,"getQuery","x=1"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getEncodedQuery","x=1")||!get_parameter(v,uri,"x","1"))return uri_selftest_fail(v,__LINE__);
 if(!parameter_absent(v,uri,"1#frag")||!parameter_absent(v,uri,"x=1#frag"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getFragment","frag"))return uri_selftest_fail(v,__LINE__);
 /* A literal '+' stays '+' in a path, and %2B also decodes to '+'. */
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE plus_path\n");
 arg.l=dvm_new_string(v,"https://host/a+b%2Bc?k=v");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getPath","/a+b+c")||!get_text(v,uri,"getEncodedPath","/a+b%2Bc"))return uri_selftest_fail(v,__LINE__);
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE relative_path\n");
 arg.l=dvm_new_string(v,"/only/path?k=v");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||!get_null(v,uri,"getScheme")||!get_null(v,uri,"getHost"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getPath","/only/path")||!get_text(v,uri,"getQuery","k=v"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"k","v"))return uri_selftest_fail(v,__LINE__);
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE network_path\n");
 arg.l=dvm_new_string(v,"//host/relative?flag");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||!get_null(v,uri,"getScheme"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getHost","host")||!get_text(v,uri,"getPath","/relative"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"flag",""))return uri_selftest_fail(v,__LINE__);
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE bare_relative\n");
 arg.l=dvm_new_string(v,"relative/path?k=v");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||!get_null(v,uri,"getScheme")||!get_null(v,uri,"getHost"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getPath","relative/path")||!get_text(v,uri,"getQuery","k=v"))return uri_selftest_fail(v,__LINE__);
 if(!get_parameter(v,uri,"k","v"))return uri_selftest_fail(v,__LINE__);
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE plain\n");
 arg.l=dvm_new_string(v,"plain");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||!get_null(v,uri,"getScheme")||!get_null(v,uri,"getHost"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getPath","plain")||!get_null(v,uri,"getQuery"))return uri_selftest_fail(v,__LINE__);
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE no_path\n");
 arg.l=dvm_new_string(v,"https://host.no.path");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||!get_text(v,uri,"getPath","")||!get_null(v,uri,"getQuery"))return uri_selftest_fail(v,__LINE__);
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE colon_in_path\n");
 arg.l=dvm_new_string(v,"/a://b");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||!get_null(v,uri,"getScheme")||!get_text(v,uri,"getPath","/a://b"))return uri_selftest_fail(v,__LINE__);
 /* Non-hierarchical URI: scheme exists but there is no path; query and
  * fragment are still separated, and getQueryParameter rejects the access
  * the way Android's opaque Uri does. */
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST STAGE opaque\n");
 arg.l=dvm_new_string(v,"mailto:user@example.com?subject=hello%20world#top");
 if(!arg.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&arg,1,&out))return uri_selftest_fail(v,__LINE__);
 uri=out.l;
 if(!uri||dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getScheme","mailto")||!get_null(v,uri,"getHost"))return uri_selftest_fail(v,__LINE__);
 if(!get_null(v,uri,"getPath")||!get_null(v,uri,"getEncodedPath"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getQuery","subject=hello world"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getEncodedQuery","subject=hello%20world"))return uri_selftest_fail(v,__LINE__);
 if(!get_text(v,uri,"getFragment","top"))return uri_selftest_fail(v,__LINE__);
 if(!parameter_throws(v,uri,"subject","java/lang/UnsupportedOperationException"))return uri_selftest_fail(v,__LINE__);
 if(dvm_exception(v))return uri_selftest_fail(v,__LINE__);
 if(!uri_runtime_builder_selftest(v))return uri_selftest_fail(v,__LINE__);
 if(!java_uri_authority_selftest(v))return uri_selftest_fail(v,__LINE__);
 if(!uri_query_names_selftest(v))return uri_selftest_fail(v,__LINE__);
 sdk_trace_fprintf(stderr,"URI_RUNTIME_SELFTEST PASS scheme=1 host=1 path=1 path_decoded=1 query_decoded=1 query_encoded=1 query_parameter=1 encoded_key=1 plus_space=1 percent_utf8=1 duplicate_key_first=1 first_equals_only=1 missing_parameter=1 empty_value=1 fragment_split=1 encoded_fragment=1 non_hierarchical=1 relative_path=1 bare_path=1 builder=1\n");
 return true;
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int uri_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;(void)visit;(void)user;return o->data_destroy==uri_payload_free||o->data_destroy==uri_builder_free?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}

bool uri_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,
 const union dvm_value *a,int n,union dvm_value *out){

 struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
 if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,self))return true;
 bool handled=uri_runtime_call_gc9_operation(v,c,m,sig,self,a,n,out);
 if(handled&&!dvm_exception(v))gc_operation_result(&op,sig,out->l);
 return handled;
}
