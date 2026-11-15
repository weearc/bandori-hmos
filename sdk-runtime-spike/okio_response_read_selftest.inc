/* Actual VM virtual dispatch with original RealBufferedSource and Buffer.
 * Only the lowest Source is a deterministic test fixture, not network data. */
static bool response_st_text(struct dvm *v,dvm_ref ref,const char *expected,size_t length){const char *text=dvm_string_utf8(v,ref);return text&&dvm_string_utf8_length(v,ref)==length&&!memcmp(text,expected,length);}
typedef struct {dvm_ref bytes;int length,pos,chunk,reads,closes;bool read_error,close_io,close_runtime;} response_st_source;
typedef struct {dvm_ref source,media;int source_calls,media_calls;} response_st_body;
static void response_st_source_free(void *p){free(p);}
static void response_st_body_free(void *p){free(p);}
int response_fixture_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;
 if(o->data_destroy==response_st_source_free){if(visit)visit(&((response_st_source*)o->data)->bytes,user);return DVM_GC_PAYLOAD_REFS;}
 if(o->data_destroy==response_st_body_free){response_st_body *p=o->data;if(visit){visit(&p->source,user);visit(&p->media,user);}return DVM_GC_PAYLOAD_REFS;}
 return DVM_GC_PAYLOAD_UNKNOWN;
}
static bool response_st_source_call(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;response_st_source *s=dvm__obj(v,self)->data;const char *method=dvm__builtin_method()->name;
 if(!strcmp(method,"close")){
  s->closes++;if(s->close_runtime)dvm__throw(v,"java/lang/IllegalStateException","fixture close runtime");
  else if(s->close_io)dvm__throw(v,"java/io/IOException","fixture close checked");return true;
 }
 s->reads++;
 if(s->read_error){dvm__throw(v,"java/io/IOException","fixture read failure");return true;}
 if(!a[0].l){dvm__throw(v,"java/lang/IllegalArgumentException","sink == null");return true;}
 if(a[1].j<0){dvm__throw(v,"java/lang/IllegalArgumentException","byteCount < 0");return true;}
 if(!a[1].j){out->j=0;return true;}
 if(s->pos==s->length){out->j=-1;return true;}
 int count=s->length-s->pos;if(count>s->chunk)count=s->chunk;if(count>a[1].j)count=(int)a[1].j;
 union dvm_value args[3]={{.l=s->bytes},{.i=s->pos},{.i=count}},q={0};
 if(!response_read_virtual(v,a[0].l,"write","([BII)Lokio/Buffer;",args,3,&q))return true;
 s->pos+=count;out->j=count;return true;
}
static bool response_st_body_call(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;response_st_body *s=dvm__obj(v,self)->data;
 if(!strcmp(dvm__builtin_method()->name,"source")){s->source_calls++;out->l=s->source;}
 else{s->media_calls++;out->l=s->media;}return true;
}
static struct dvm_class *response_st_class(struct dvm *v,bool body){
 struct dvm_class *c=dvm_find_class_or_external(v,body?"host/selftest/ResponseBody":"host/selftest/ChunkSource");if(!c)return NULL;
 if(c->nmethods)return c;
 c->super=dvm_find_class_or_external(v,body?"okhttp3/ResponseBody":"java/lang/Object");
 /* A native fixture has no own instance fields but inherits every base slot.
  * Keeping zero here made the strict GC layout reject the entire root walk. */
 c->islots=c->super?c->super->islots:0;
 if(!dvm_gc_layout_ensure(v,c))return NULL;
 if(!body){c->ifaces=calloc(1,sizeof *c->ifaces);if(!c->ifaces)return NULL;c->ifaces[0]=dvm_find_class(v,"okio/Source");c->nifaces=1;}
 c->methods=calloc(2,sizeof *c->methods);if(!c->methods)return NULL;c->nmethods=2;
 for(int i=0;i<2;i++){
  struct dvm_method *m=&c->methods[i];m->cls=c;m->access=DEX_ACC_PUBLIC;
  m->name=body?(i?"contentType":"source"):(i?"close":"read");
  m->sig=strdup(body?(i?"()Lokhttp3/MediaType;":"()Lokio/BufferedSource;"):(i?"()V":"(Lokio/Buffer;J)J"));
  m->builtin=body?response_st_body_call:response_st_source_call;m->ret_kind=body?'L':i?'V':'J';m->arg_count=!body&&!i?2:0;m->arg_slots=!body&&!i?3:0;
 }return c;
}
static dvm_ref response_st_fixture(struct dvm *v,const uint8_t *bytes,int length,int chunk,const char *media,response_st_source **source_state,response_st_body **body_state){
 struct dvm_class *sc=response_st_class(v,false),*bc=response_st_class(v,true);if(!sc||!bc)return 0;
 dvm_ref sr=dvm_new_object(v,sc);struct dvm_object *so=dvm__obj(v,sr);if(!so)return 0;
 response_st_source *s=calloc(1,sizeof *s);if(!s)return 0;so->data=s;so->data_destroy=response_st_source_free;
 s->bytes=dvm_new_array(v,'B',"B",length);if(!s->bytes)return 0;
 memcpy(dvm_array_data(v,s->bytes),bytes,(size_t)length);s->length=length;s->chunk=chunk;
 dvm_ref buffered=dvm_new_object(v,dvm_find_class(v,"okio/RealBufferedSource"));union dvm_value a={.l=sr},q={0};
 if(!buffered||!response_read_virtual(v,buffered,"<init>","(Lokio/Source;)V",&a,1,&q))return 0;
 dvm_ref body=dvm_new_object(v,bc);struct dvm_object *bo=dvm__obj(v,body);if(!bo)return 0;
 response_st_body *b=calloc(1,sizeof *b);if(!b)return 0;bo->data=b;bo->data_destroy=response_st_body_free;b->source=buffered;
 if(media){struct dvm_class *mt=dvm_find_class(v,"okhttp3/MediaType");struct dvm_method *parse=mt?dvm_find_method(v,mt,"parse","(Ljava/lang/String;)Lokhttp3/MediaType;"):NULL;
  a.l=dvm_new_string(v,media);if(!parse||!dvm_call(v,parse,0,&a,1,&q)||!q.l)return 0;b->media=q.l;}
 *source_state=s;*body_state=b;return body;
}
bool string_runtime_response_read_selftest(struct dvm *v){
 dvm_ref saved=dvm_exception(v);bool parked=v->parked;uint64_t steps=v->call_steps,limit=v->step_limit;
 dvm_clear_exception(v);v->parked=false;v->call_steps=0;if(!v->step_limit||v->step_limit>100000)v->step_limit=100000;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/okio-response-read-selftest.txt","w");
 bool ok=true;unsigned checks=0;union dvm_value q={0};response_st_source *s=NULL;response_st_body *b=NULL;dvm_ref body=0;
 #define RESPONSE_ST(label,expr) do{if(!(expr)){if(f)fprintf(f,"FAIL %s exception=%u\n",label,dvm_exception(v));ok=false;goto done;}checks++;if(f)fprintf(f,"PASS %s\n",label);}while(0)
 static const uint8_t utf8[]={0xef,0xbb,0xbf,'A',0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
 body=response_st_fixture(v,utf8,sizeof utf8,1,"text/plain; charset=ISO-8859-1",&s,&b);
 RESPONSE_ST("BOM overrides media charset actual VM",body&&response_read_virtual(v,body,"string","()Ljava/lang/String;",NULL,0,&q)&&response_st_text(v,q.l,"A\xE4\xB8\xAD\xF0\x9F\x98\x80",8));
 RESPONSE_ST("virtual source contentType and finally close",b->source_calls==1&&b->media_calls==1&&s->closes==1&&s->pos==s->length&&s->reads>s->length);
 union dvm_value closed={0},actual_buffer={0},remaining={0};
 RESPONSE_ST("close marks actual source closed and buffer consumed",dvm_get_field(v,b->source,"closed","Z",&closed)&&closed.i&&dvm_get_field(v,b->source,"buffer","Lokio/Buffer;",&actual_buffer)&&dvm_get_field(v,actual_buffer.l,"size","J",&remaining)&&remaining.j==0);
 bool after_close=!response_read_virtual(v,b->source,"readByte","()B",NULL,0,&q);struct dvm_class *closed_error=dvm_object_class(v,dvm_exception(v));
 RESPONSE_ST("subsequent source read throws original closed exception",after_close&&closed_error&&!strcmp(closed_error->name,"java/lang/IllegalStateException"));dvm_clear_exception(v);
 static const uint8_t utf16[]={0xfe,0xff,0x4e,0x2d};
 body=response_st_fixture(v,utf16,sizeof utf16,1,NULL,&s,&b);
 RESPONSE_ST("UTF16 BE BOM",body&&response_read_virtual(v,body,"string","()Ljava/lang/String;",NULL,0,&q)&&response_st_text(v,q.l,"\xE4\xB8\xAD",3));
 static const uint8_t latin[]={0xe9};
 body=response_st_fixture(v,latin,sizeof latin,1,"text/plain; charset=ISO-8859-1",&s,&b);
 RESPONSE_ST("media charset without BOM",body&&response_read_virtual(v,body,"string","()Ljava/lang/String;",NULL,0,&q)&&response_st_text(v,q.l,"\xC3\xA9",2));
 body=response_st_fixture(v,latin,0,1,NULL,&s,&b);
 RESPONSE_ST("empty source real EOF gives empty string",body&&response_read_virtual(v,body,"string","()Ljava/lang/String;",NULL,0,&q)&&response_st_text(v,q.l,"",0)&&s->reads>0&&s->closes==1);
 body=response_st_fixture(v,latin,1,1,NULL,&s,&b);RESPONSE_ST("null charset fixture",body!=0);union dvm_value null_charset={0};
 bool null_failed=!response_read_virtual(v,b->source,"readString","(Ljava/nio/charset/Charset;)Ljava/lang/String;",&null_charset,1,&q);struct dvm_class *null_error=dvm_object_class(v,dvm_exception(v));
 RESPONSE_ST("RealBufferedSource null charset rejects before read",null_failed&&null_error&&!strcmp(null_error->name,"java/lang/IllegalArgumentException")&&s->reads==0);dvm_clear_exception(v);
 uint8_t *long_text=malloc(8195);RESPONSE_ST("cross segment fixture memory",long_text!=NULL);memset(long_text,'x',8191);memcpy(long_text+8191,"\xF0\x9F\x98\x80",4);
 body=response_st_fixture(v,long_text,8195,8192,NULL,&s,&b);free(long_text);
 RESPONSE_ST("UTF8 across segment and source boundary",body&&response_read_virtual(v,body,"string","()Ljava/lang/String;",NULL,0,&q)&&dvm_string_utf8_length(v,q.l)==8195&&!memcmp(dvm_string_utf8(v,q.l)+8191,"\xF0\x9F\x98\x80",4)&&s->closes==1);
 body=response_st_fixture(v,latin,sizeof latin,1,NULL,&s,&b);RESPONSE_ST("read failure fixture",body!=0);s->read_error=true;s->close_io=true;
 bool read_failed=!response_read_virtual(v,body,"string","()Ljava/lang/String;",NULL,0,&q);struct dvm_class *error=dvm_object_class(v,dvm_exception(v));
 RESPONSE_ST("read IOException retained and checked close swallowed",read_failed&&error&&!strcmp(error->name,"java/io/IOException")&&s->closes==1);dvm_clear_exception(v);
 body=response_st_fixture(v,latin,sizeof latin,1,NULL,&s,&b);RESPONSE_ST("runtime close fixture",body!=0);s->read_error=true;s->close_runtime=true;
 bool close_failed=!response_read_virtual(v,body,"string","()Ljava/lang/String;",NULL,0,&q);error=dvm_object_class(v,dvm_exception(v));
 RESPONSE_ST("RuntimeException close overrides read failure",close_failed&&error&&!strcmp(error->name,"java/lang/IllegalStateException")&&s->closes==1);dvm_clear_exception(v);
 done:
 if(f){fprintf(f,"OKIO_RESPONSE_READ_SELFTEST %s checks=%u actual_VM=1\n",ok?"PASS":"FAIL",checks);fclose(f);}
 sdk_trace_fprintf(stderr,"OKIO_RESPONSE_READ_SELFTEST %s checks=%u actual_VM=1\n",ok?"PASS":"FAIL",checks);
 dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;v->step_limit=limit;return ok;
 #undef RESPONSE_ST
}
