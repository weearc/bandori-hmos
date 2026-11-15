static void reader_fixture_free(void *p){free(p);}
/* Real VM objects and dvm_call, backed by a one-byte-per-read InputStream. */
typedef struct {const uint8_t *bytes;int length,pos,reads,closes;bool fail;} isr_st_input;
typedef struct {int writes,flushes,closes;bool io_error,runtime_error;} pw_st_sink;
static bool pw_st_write(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *q){
 (void)a;(void)n;(void)q;pw_st_sink *s=dvm__obj(v,self)->data;const char *name=dvm__builtin_method()->name;
 if(!strcmp(name,"flush"))s->flushes++;else if(!strcmp(name,"close"))s->closes++;else s->writes++;
 if(s->runtime_error){dvm__throw(v,"java/lang/IllegalArgumentException","selftest writer runtime failure");return true;}
 if(s->io_error){dvm__throw(v,"java/io/IOException","selftest writer IO failure");return true;}return true;
}
static dvm_ref pw_st_sink_new(struct dvm *v){
 struct dvm_class *k=dvm_find_class_or_external(v,"host/selftest/PrintWriterSink");if(!k)return 0;
 if(!k->nmethods){
  static const char *names[]={"write","flush","close"},*sigs[]={"([BII)V","()V","()V"};
  k->super=dvm_find_class_or_external(v,"java/io/OutputStream");k->methods=calloc(3,sizeof *k->methods);if(!k->methods)return 0;k->nmethods=3;
  for(int i=0;i<3;i++){k->methods[i].cls=k;k->methods[i].name=names[i];k->methods[i].sig=strdup(sigs[i]);k->methods[i].builtin=pw_st_write;k->methods[i].ret_kind='V';k->methods[i].arg_count=k->methods[i].arg_slots=i==0?3:0;}
 }
 dvm_ref ref=dvm_new_object(v,k);if(!ref)return 0;struct dvm_object *o=dvm__obj(v,ref);o->data=calloc(1,sizeof(pw_st_sink));o->data_destroy=reader_fixture_free;return o->data?ref:0;
}
static bool isr_st_stream(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *q){
 (void)a;(void)n;isr_st_input *s=dvm__obj(v,self)->data;const char *name=dvm__builtin_method()->name;
 if(!strcmp(name,"close")){s->closes++;return true;}
 if(s->fail){dvm__throw(v,"java/io/IOException","selftest input failure");return true;}
 if(!strcmp(name,"available")){q->i=s->length-s->pos;return true;}
 s->reads++;q->i=s->pos<s->length?s->bytes[s->pos++]:-1;return true;
}
static bool isr_st_call(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *a,int n,union dvm_value *q){
 struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,self),name,sig);memset(q,0,sizeof *q);
 return m&&dvm_call(v,m,self,a,n,q)&&!dvm_exception(v);
}
static dvm_ref isr_st_input_new(struct dvm *v,const uint8_t *bytes,int length){
 struct dvm_class *k=dvm_find_class_or_external(v,"host/selftest/OneByteInputStream");if(!k)return 0;
 if(!k->nmethods){
  static const char *names[]={"read","available","close"};static const char *sigs[]={"()I","()I","()V"};
  k->super=dvm_find_class_or_external(v,"java/io/InputStream");k->methods=calloc(3,sizeof *k->methods);if(!k->methods)return 0;k->nmethods=3;
  for(int i=0;i<3;i++){k->methods[i].cls=k;k->methods[i].name=names[i];k->methods[i].sig=strdup(sigs[i]);k->methods[i].builtin=isr_st_stream;k->methods[i].ret_kind=i==2?'V':'I';}
 }
 dvm_ref ref=dvm_new_object(v,k);if(!ref)return 0;isr_st_input *s=calloc(1,sizeof *s);if(!s)return 0;
 s->bytes=bytes;s->length=length;struct dvm_object *o=dvm__obj(v,ref);o->data=s;o->data_destroy=reader_fixture_free;return ref;
}
/* A real builtin caller establishes a nearly exhausted outer budget. Guest
 * JSON constructor/parser calls remain real DEX, including overload nesting. */
typedef struct {bool ctor,parse,ctor_restore,parse_restore,error_restore;uint64_t ctor_used,parse_used;} json_slice_st;
static bool json_slice_st_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *q){
 (void)a;(void)n;(void)q;json_slice_st *s=dvm__obj(v,self)->data;
 uint64_t saved_steps=v->call_steps,saved_limit=v->step_limit;
 struct dvm_class *jc=dvm_find_class(v,"copy/google/json/JSON");
 dvm_ref parser=jc?dvm_new_object(v,jc):0;
 union dvm_value result={0},args[2]={{0}};
 v->call_steps=99999;v->step_limit=100000;
 uint64_t before=dvm_instructions(v);
 s->ctor=parser&&isr_st_call(v,parser,"<init>","()V",NULL,0,&result);
 s->ctor_used=dvm_instructions(v)-before;
 s->ctor_restore=v->call_steps==99999&&v->step_limit==100000;
 if(!s->ctor||!s->ctor_restore)goto done;
 args[0].l=dvm_new_string(v,"\"slice-probe\"");args[1].l=dvm_class_object(v,dvm_find_class_or_external(v,"java/lang/String"));
 before=dvm_instructions(v);
 s->parse=isr_st_call(v,parser,"fromJson","(Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/Object;",args,2,&result)&&io_selftest_string(v,result.l,"slice-probe",11);
 s->parse_used=dvm_instructions(v)-before;
 s->parse_restore=v->call_steps==99999&&v->step_limit==100000;
 if(!s->parse||!s->parse_restore)goto done;
 args[0].l=dvm_new_string(v,"[1");
 bool invalid=!isr_st_call(v,parser,"fromJson","(Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/Object;",args,2,&result)&&dvm_exception(v);
 s->error_restore=invalid&&v->call_steps==99999&&v->step_limit==100000;
 done:
 dvm_clear_exception(v);v->call_steps=saved_steps;v->step_limit=saved_limit;return true;
}
static dvm_ref json_slice_st_new(struct dvm *v){
 struct dvm_class *c=dvm_find_class_or_external(v,"host/selftest/JsonSliceCaller");if(!c)return 0;
 if(!c->nmethods){
  c->methods=calloc(1,sizeof *c->methods);if(!c->methods)return 0;c->nmethods=1;
  c->methods[0].cls=c;c->methods[0].name="run";c->methods[0].sig=strdup("()V");c->methods[0].builtin=json_slice_st_run;c->methods[0].ret_kind='V';
 }
 dvm_ref ref=dvm_new_object(v,c);struct dvm_object *o=dvm__obj(v,ref);if(!o)return 0;
 o->data=calloc(1,sizeof(json_slice_st));o->data_destroy=reader_fixture_free;return o->data?ref:0;
}
bool io_runtime_input_stream_reader_selftest(struct dvm *v){
 dvm_ref saved=dvm_exception(v);bool parked=v->parked;uint64_t steps=v->call_steps,limit=v->step_limit;
 dvm_clear_exception(v);v->parked=false;v->call_steps=0;v->step_limit=2000000;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/input-stream-reader-selftest.txt","w");
 bool ok=true;unsigned checks=0;union dvm_value q={0},a[3]={{0}};
 static const uint8_t text[]={'A',0xE4,0xB8,0xAD,0xF0,0x9F,0x98,0x80,'\r','\n','Z'};
 static const int units[]={'A',0x4E2D,0xD83D,0xDE00,'\r','\n','Z'};
 static const uint8_t malformed[]={0xE2,'Q',0xC0,0xAF,0xED,0xA0,0x80,0xF0,0x9F};
 static const int replacements[]={0xFFFD,'Q',0xFFFD,0xFFFD,0xFFFD,0xFFFD};
 struct dvm_class *k=dvm_find_class_or_external(v,"java/io/InputStreamReader");
 dvm_ref input=0,reader=0,chars=0,other=0,br=0;
#define ISR_ST(label,expr) do{if(!(expr)){if(f)fprintf(f,"FAIL %s line=%d exception=%u\n",label,__LINE__,dvm_exception(v));ok=false;goto done;}checks++;if(f)fprintf(f,"PASS %s\n",label);}while(0)
 ISR_ST("class registered",k&&dvm_class_exists(v,"Ljava/io/InputStreamReader;"));
 {
  struct dvm_class *baos_class=dvm_find_class_or_external(v,"java/io/ByteArrayOutputStream");
  struct dvm_class *os_class=dvm_find_class_or_external(v,"java/io/OutputStream");
  dvm_ref baos=dvm_new_object(v,baos_class),bytes=dvm_new_array(v,'B',"B",2);
  ISR_ST("ByteArrayOutputStream extends OutputStream",baos_class&&os_class&&dvm__class_assignable(v,baos_class,os_class));
  ISR_ST("ByteArrayOutputStream constructor",baos&&io_runtime_call(v,"java/io/ByteArrayOutputStream","<init>","()V",baos,NULL,0,&q)&&!dvm_exception(v));
  a[0].i=0xff;ISR_ST("OutputStream typed BAOS write int",io_runtime_call(v,"java/io/OutputStream","write","(I)V",baos,a,1,&q)&&!dvm_exception(v));
  ISR_ST("BAOS test byte array",bytes!=0);((uint8_t*)dvm_array_data(v,bytes))[0]='A';((uint8_t*)dvm_array_data(v,bytes))[1]='B';
  a[0].l=bytes;ISR_ST("OutputStream typed BAOS write array",io_runtime_call(v,"java/io/OutputStream","write","([B)V",baos,a,1,&q)&&!dvm_exception(v));
  ISR_ST("OutputStream typed BAOS close",io_runtime_call(v,"java/io/OutputStream","close","()V",baos,NULL,0,&q)&&!dvm_exception(v));
  a[0].l=bytes;a[1].i=1;a[2].i=1;
  ISR_ST("BAOS range write after close",io_runtime_call(v,"java/io/OutputStream","write","([BII)V",baos,a,3,&q)&&!dvm_exception(v));
  ISR_ST("BAOS preserved exact bytes",io_runtime_call(v,"java/io/ByteArrayOutputStream","toByteArray","()[B",baos,NULL,0,&q)&&q.l&&dvm_array_length(v,q.l)==4&&!memcmp(dvm_array_data(v,q.l),"\xff" "ABB",4));
 }
 {
  struct dvm_class *pk=dvm_find_class_or_external(v,"java/io/PrintWriter");
  dvm_ref baos=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/ByteArrayOutputStream")),pw=dvm_new_object(v,pk),chars=dvm_new_array(v,'C',"C",4);
  ISR_ST("PrintWriter class registered",pk&&dvm_class_exists(v,"Ljava/io/PrintWriter;"));
  ISR_ST("PrintWriter BAOS setup",baos&&pw&&chars&&io_runtime_call(v,"java/io/ByteArrayOutputStream","<init>","()V",baos,NULL,0,&q));
  a[0].l=baos;a[1].i=1;ISR_ST("PrintWriter OutputStream autoFlush constructor via VM",isr_st_call(v,pw,"<init>","(Ljava/io/OutputStream;Z)V",a,2,&q));
  a[0].l=dvm_new_string(v,"\xe4\xb8\xad\xf0\x9f\x98\x80");ISR_ST("PrintWriter UTF8 String",a[0].l&&isr_st_call(v,pw,"print","(Ljava/lang/String;)V",a,1,&q));
  a[0].i=7;ISR_ST("PrintWriter println int",isr_st_call(v,pw,"println","(I)V",a,1,&q));
  uint16_t *units=dvm_array_data(v,chars);units[0]='A';units[1]=0xD83D;units[2]=0xDE00;units[3]='B';
  a[0].l=chars;a[1].i=1;a[2].i=2;ISR_ST("PrintWriter char array UTF16 pair range",isr_st_call(v,pw,"write","([CII)V",a,3,&q));
  a[0].l=dvm_new_string(v,"x\xe4\xb8\xadz");a[1].i=1;a[2].i=1;ISR_ST("PrintWriter String range uses UTF16 indexes",isr_st_call(v,pw,"write","(Ljava/lang/String;II)V",a,3,&q));
  a[0].i=0xD83D;ISR_ST("PrintWriter pending high surrogate",isr_st_call(v,pw,"write","(I)V",a,1,&q));
  a[0].i=0xDE00;ISR_ST("PrintWriter low surrogate across writes",isr_st_call(v,pw,"write","(I)V",a,1,&q));
  a[0].i=0;ISR_ST("PrintWriter embedded NUL",isr_st_call(v,pw,"write","(I)V",a,1,&q));
  static const char expected[]="\xe4\xb8\xad\xf0\x9f\x98\x80" "7\n" "\xf0\x9f\x98\x80\xe4\xb8\xad\xf0\x9f\x98\x80\0";
  ISR_ST("PrintWriter exact UTF8 bytes",io_runtime_call(v,"java/io/ByteArrayOutputStream","toByteArray","()[B",baos,NULL,0,&q)&&q.l&&dvm_array_length(v,q.l)==sizeof expected-1&&!memcmp(dvm_array_data(v,q.l),expected,sizeof expected-1));
  ISR_ST("PrintWriter checkError success",isr_st_call(v,pw,"checkError","()Z",NULL,0,&q)&&!q.i);
  a[0].l=chars;a[1].i=-1;a[2].i=1;(void)isr_st_call(v,pw,"write","([CII)V",a,3,&q);ISR_ST("PrintWriter invalid range propagates",io_selftest_exception_named(v,"java/lang/IndexOutOfBoundsException"));
  ISR_ST("PrintWriter close",isr_st_call(v,pw,"close","()V",NULL,0,&q));
  a[0].i='Q';ISR_ST("PrintWriter closed write suppressed",isr_st_call(v,pw,"write","(I)V",a,1,&q));
  ISR_ST("PrintWriter closed write trouble",isr_st_call(v,pw,"checkError","()Z",NULL,0,&q)&&q.i);
  dvm_ref sink=pw_st_sink_new(v),pw2=dvm_new_object(v,pk);a[0].l=sink;a[1].i=1;
  ISR_ST("PrintWriter generic stream constructor",sink&&pw2&&isr_st_call(v,pw2,"<init>","(Ljava/io/OutputStream;Z)V",a,2,&q));
  pw_st_sink *state=dvm__obj(v,sink)->data;a[0].l=dvm_new_string(v,"a\n");
  ISR_ST("PrintWriter print newline does not autoFlush",isr_st_call(v,pw2,"print","(Ljava/lang/String;)V",a,1,&q)&&state->writes==1&&state->flushes==0);
  ISR_ST("PrintWriter println autoFlush",isr_st_call(v,pw2,"println","()V",NULL,0,&q)&&state->flushes==1);
  state->io_error=true;a[0].l=dvm_new_string(v,"io");ISR_ST("PrintWriter IOException swallowed",isr_st_call(v,pw2,"print","(Ljava/lang/String;)V",a,1,&q));
  ISR_ST("PrintWriter IOException sets trouble",isr_st_call(v,pw2,"checkError","()Z",NULL,0,&q)&&q.i);
  state->io_error=false;state->runtime_error=true;(void)isr_st_call(v,pw2,"print","(Ljava/lang/String;)V",a,1,&q);
  ISR_ST("PrintWriter RuntimeException not swallowed",io_selftest_exception_named(v,"java/lang/IllegalArgumentException"));
  state->runtime_error=false;ISR_ST("PrintWriter closes underlying stream once",isr_st_call(v,pw2,"close","()V",NULL,0,&q)&&isr_st_call(v,pw2,"close","()V",NULL,0,&q)&&state->closes==1);
  dvm_ref sw=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/StringWriter")),pw3=dvm_new_object(v,pk);
  ISR_ST("PrintWriter Writer setup",sw&&pw3&&io_runtime_call(v,"java/io/StringWriter","<init>","()V",sw,NULL,0,&q));
  a[0].l=sw;ISR_ST("PrintWriter Writer constructor",isr_st_call(v,pw3,"<init>","(Ljava/io/Writer;)V",a,1,&q));
  a[0].l=dvm_new_string(v,"writer");ISR_ST("PrintWriter Writer delegates actual text",isr_st_call(v,pw3,"println","(Ljava/lang/String;)V",a,1,&q)&&io_runtime_call(v,"java/io/StringWriter","toString","()Ljava/lang/String;",sw,NULL,0,&q)&&io_selftest_string(v,q.l,"writer\n",7));
 }
 input=isr_st_input_new(v,text,sizeof text);reader=dvm_new_object(v,k);a[0].l=input;
 ISR_ST("default constructor",input&&reader&&isr_st_call(v,reader,"<init>","(Ljava/io/InputStream;)V",a,1,&q));
 ISR_ST("encoding UTF8",isr_st_call(v,reader,"getEncoding","()Ljava/lang/String;",NULL,0,&q)&&io_selftest_string(v,q.l,"UTF8",4));
 ISR_ST("ready without consuming",isr_st_call(v,reader,"ready","()Z",NULL,0,&q)&&q.i&&((isr_st_input*)dvm__obj(v,input)->data)->reads==0);
 for(unsigned i=0;i<sizeof units/sizeof units[0];i++)ISR_ST("one-byte UTF8 including surrogate pair",isr_st_call(v,reader,"read","()I",NULL,0,&q)&&q.i==units[i]);
 ISR_ST("EOF",isr_st_call(v,reader,"read","()I",NULL,0,&q)&&q.i==-1);
 chars=dvm_new_array(v,'C',"C",10);a[0].l=chars;a[1].i=2;a[2].i=0;
 ISR_ST("zero read at EOF",isr_st_call(v,reader,"read","([CII)I",a,3,&q)&&q.i==0);
 ISR_ST("ready EOF",isr_st_call(v,reader,"ready","()Z",NULL,0,&q)&&q.i==0);
 ISR_ST("close",isr_st_call(v,reader,"close","()V",NULL,0,&q));
 ISR_ST("double close once underlying",isr_st_call(v,reader,"close","()V",NULL,0,&q)&&((isr_st_input*)dvm__obj(v,input)->data)->closes==1);
 ISR_ST("encoding null after close",isr_st_call(v,reader,"getEncoding","()Ljava/lang/String;",NULL,0,&q)&&!q.l);
 (void)isr_st_call(v,reader,"read","()I",NULL,0,&q);ISR_ST("closed read throws",io_selftest_exception_named(v,"java/io/IOException"));
 input=isr_st_input_new(v,text,sizeof text);reader=dvm_new_object(v,k);a[0].l=input;a[1].l=dvm_new_string(v,"uTf-8");
 ISR_ST("String charset constructor",isr_st_call(v,reader,"<init>","(Ljava/io/InputStream;Ljava/lang/String;)V",a,2,&q));
 a[0].l=chars;a[1].i=1;a[2].i=7;
 ISR_ST("range read count",isr_st_call(v,reader,"read","([CII)I",a,3,&q)&&q.i==7);
 {uint16_t *p=dvm_array_data(v,chars);for(int i=0;i<7;i++)ISR_ST("range UTF16 content",p[i+1]==units[i]);}
 a[0].l=chars;a[1].i=9;a[2].i=2;(void)isr_st_call(v,reader,"read","([CII)I",a,3,&q);
 ISR_ST("bad range",io_selftest_exception_named(v,"java/lang/IndexOutOfBoundsException"));
 input=isr_st_input_new(v,malformed,sizeof malformed);reader=dvm_new_object(v,k);a[0].l=input;
 ISR_ST("malformed input constructor",isr_st_call(v,reader,"<init>","(Ljava/io/InputStream;)V",a,1,&q));
 for(unsigned i=0;i<sizeof replacements/sizeof replacements[0];i++)ISR_ST("invalid/truncated UTF8 replacement",isr_st_call(v,reader,"read","()I",NULL,0,&q)&&q.i==replacements[i]);
 ISR_ST("malformed EOF",isr_st_call(v,reader,"read","()I",NULL,0,&q)&&q.i==-1);
 other=dvm_new_object(v,k);a[0].l=input;a[1].l=dvm_new_string(v,"unknown-charset");(void)isr_st_call(v,other,"<init>","(Ljava/io/InputStream;Ljava/lang/String;)V",a,2,&q);
 ISR_ST("unsupported encoding throws",io_selftest_exception_named(v,"java/io/UnsupportedEncodingException"));
 a[0].l=0;(void)isr_st_call(v,other,"<init>","(Ljava/io/InputStream;)V",a,1,&q);ISR_ST("null input throws",io_selftest_exception_named(v,"java/lang/NullPointerException"));
 /* Obtain the same Charset implementation the guest uses, not a fake name. */
 {struct dvm_class *cs=dvm_find_class_or_external(v,"java/nio/charset/Charset");struct dvm_method *fm=dvm_find_method(v,cs,"forName","(Ljava/lang/String;)Ljava/nio/charset/Charset;");a[0].l=dvm_new_string(v,"UTF-8");
  ISR_ST("real Charset forName",fm&&dvm_call(v,fm,0,a,1,&q)&&q.l&&!dvm_exception(v));a[1].l=q.l;}
 input=isr_st_input_new(v,text,sizeof text);reader=dvm_new_object(v,k);a[0].l=input;
 ISR_ST("Charset constructor",isr_st_call(v,reader,"<init>","(Ljava/io/InputStream;Ljava/nio/charset/Charset;)V",a,2,&q));
 br=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/BufferedReader"));a[0].l=reader;
 ISR_ST("BufferedReader wraps InputStreamReader",br&&isr_st_call(v,br,"<init>","(Ljava/io/Reader;)V",a,1,&q));
 ISR_ST("BufferedReader CRLF and nonBMP",isr_st_call(v,br,"readLine","()Ljava/lang/String;",NULL,0,&q)&&io_selftest_string(v,q.l,"A\xE4\xB8\xAD\xF0\x9F\x98\x80",8));
 ISR_ST("BufferedReader final line",isr_st_call(v,br,"readLine","()Ljava/lang/String;",NULL,0,&q)&&io_selftest_string(v,q.l,"Z",1));
 ISR_ST("BufferedReader EOF",isr_st_call(v,br,"readLine","()Ljava/lang/String;",NULL,0,&q)&&!q.l);
 ISR_ST("BufferedReader close underlying",isr_st_call(v,br,"close","()V",NULL,0,&q)&&((isr_st_input*)dvm__obj(v,input)->data)->closes==1);
 input=isr_st_input_new(v,text,sizeof text);reader=dvm_new_object(v,k);a[0].l=input;ISR_ST("error input constructor",isr_st_call(v,reader,"<init>","(Ljava/io/InputStream;)V",a,1,&q));
 ((isr_st_input*)dvm__obj(v,input)->data)->fail=true;(void)isr_st_call(v,reader,"read","()I",NULL,0,&q);ISR_ST("underlying IOException propagated",io_selftest_exception_named(v,"java/io/IOException"));
 /* Static-method resolution and dvm_call exercise the production dispatcher,
  * including the two-slot long ABI and unsigned negative hex semantics. */
 {
  static const struct {bool wide;int64_t value;const char *expected;} cases[]={
   {false,0,"0"},{false,1,"1"},{false,15,"f"},{false,16,"10"},
   {false,-1,"ffffffff"},{false,INT32_MIN,"80000000"},{false,INT32_MAX,"7fffffff"},
   {true,0,"0"},{true,1,"1"},{true,15,"f"},{true,16,"10"},
   {true,-1,"ffffffffffffffff"},{true,INT64_MIN,"8000000000000000"},{true,INT64_MAX,"7fffffffffffffff"},
   {true,INT64_C(0x100000000),"100000000"},{true,-INT64_C(0x100000000),"ffffffff00000000"}
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
   struct dvm_class *owner=dvm_find_class_or_external(v,cases[i].wide?"java/lang/Long":"java/lang/Integer");
   const char *sig=cases[i].wide?"(J)Ljava/lang/String;":"(I)Ljava/lang/String;";
   struct dvm_method *method=dvm_find_method(v,owner,"toHexString",sig);
   union dvm_value value={0};if(cases[i].wide)value.j=cases[i].value;else value.i=(int32_t)cases[i].value;
   ISR_ST("toHexString actual VM static dispatch",method&&(method->access&DEX_ACC_STATIC)&&dvm_call(v,method,0,&value,1,&q)&&!dvm_exception(v)&&io_selftest_string(v,q.l,cases[i].expected,strlen(cases[i].expected)));
  }
 }
 /* StringBuffer shares the production mutable-string native backing. */
 {
  struct dvm_class *owner=dvm_find_class_or_external(v,"java/lang/StringBuffer");
  dvm_ref buffer=owner?dvm_new_object(v,owner):0,empty=owner?dvm_new_object(v,owner):0;
  ISR_ST("StringBuffer actual VM object",buffer&&empty&&dvm_class_exists(v,"Ljava/lang/StringBuffer;"));
  a[0].l=dvm_new_string(v,"ab\xF0\x9F\x98\x80");
  ISR_ST("StringBuffer String constructor",isr_st_call(v,buffer,"<init>","(Ljava/lang/String;)V",a,1,&q));
  ISR_ST("StringBuffer UTF16 length",isr_st_call(v,buffer,"length","()I",NULL,0,&q)&&q.i==4);
  a[0].i=2;ISR_ST("StringBuffer charAt surrogate",isr_st_call(v,buffer,"charAt","(I)C",a,1,&q)&&q.i==0xD83D);
  ISR_ST("StringBuffer reverse returns self",isr_st_call(v,buffer,"reverse","()Ljava/lang/StringBuffer;",NULL,0,&q)&&q.l==buffer);
  ISR_ST("StringBuffer reverse preserves pair",isr_st_call(v,buffer,"toString","()Ljava/lang/String;",NULL,0,&q)&&io_selftest_string(v,q.l,"\xF0\x9F\x98\x80" "ba",6));
  ISR_ST("StringBuffer empty constructor",isr_st_call(v,empty,"<init>","()V",NULL,0,&q));
  a[0].l=dvm_new_string(v,"X");ISR_ST("StringBuffer append String",isr_st_call(v,empty,"append","(Ljava/lang/String;)Ljava/lang/StringBuffer;",a,1,&q)&&q.l==empty);
  a[0].l=0;ISR_ST("StringBuffer append null Object",isr_st_call(v,empty,"append","(Ljava/lang/Object;)Ljava/lang/StringBuffer;",a,1,&q)&&q.l==empty);
  a[0].i=-7;ISR_ST("StringBuffer append int",isr_st_call(v,empty,"append","(I)Ljava/lang/StringBuffer;",a,1,&q)&&q.l==empty);
  a[0].j=INT64_C(4294967296);ISR_ST("StringBuffer append long",isr_st_call(v,empty,"append","(J)Ljava/lang/StringBuffer;",a,1,&q)&&q.l==empty);
  a[0].i=1;ISR_ST("StringBuffer append boolean",isr_st_call(v,empty,"append","(Z)Ljava/lang/StringBuffer;",a,1,&q)&&q.l==empty);
  a[0].i='!';ISR_ST("StringBuffer append char",isr_st_call(v,empty,"append","(C)Ljava/lang/StringBuffer;",a,1,&q)&&q.l==empty);
  ISR_ST("StringBuffer appended content",isr_st_call(v,empty,"toString","()Ljava/lang/String;",NULL,0,&q)&&io_selftest_string(v,q.l,"Xnull-74294967296true!",sizeof "Xnull-74294967296true!"-1));
  a[0].i=1;ISR_ST("StringBuffer setLength",isr_st_call(v,empty,"setLength","(I)V",a,1,&q));
  ISR_ST("StringBuffer truncation content",isr_st_call(v,empty,"toString","()Ljava/lang/String;",NULL,0,&q)&&io_selftest_string(v,q.l,"X",1));
  a[0].i=32;ISR_ST("StringBuffer capacity constructor",isr_st_call(v,empty,"<init>","(I)V",a,1,&q));
  ISR_ST("StringBuffer capacity constructor empty",isr_st_call(v,empty,"length","()I",NULL,0,&q)&&q.i==0);
  a[0].l=0;(void)isr_st_call(v,empty,"<init>","(Ljava/lang/String;)V",a,1,&q);ISR_ST("StringBuffer null String rejected",io_selftest_exception_named(v,"java/lang/NullPointerException"));
  a[0].i=-1;(void)isr_st_call(v,empty,"<init>","(I)V",a,1,&q);ISR_ST("StringBuffer negative capacity rejected",io_selftest_exception_named(v,"java/lang/NegativeArraySizeException"));
 }
 {
  dvm_ref caller=json_slice_st_new(v);
  ISR_ST("JSON slice real VM caller",caller&&isr_st_call(v,caller,"run","()V",NULL,0,&q));
  json_slice_st *probe=dvm__obj(v,caller)->data;
  if(f)fprintf(f,"JSON_SLICE_SELFTEST ctor_used=%llu parse_used=%llu outer_steps=99999 outer_limit=100000 ctor_restored=%d parse_restored=%d error_restored=%d\n",(unsigned long long)probe->ctor_used,(unsigned long long)probe->parse_used,probe->ctor_restore,probe->parse_restore,probe->error_restore);
  ISR_ST("JSON constructor independent slice actual DEX",probe->ctor&&probe->ctor_used>1&&probe->ctor_used<=200000);
  ISR_ST("JSON constructor restores outer budget",probe->ctor_restore);
  ISR_ST("JSON String Class nested parser actual DEX",probe->parse&&probe->parse_used>1&&probe->parse_used<=200000);
  ISR_ST("JSON parser restores outer budget",probe->parse_restore);
  ISR_ST("JSON parser error restores outer budget",probe->error_restore);
 }
 /* Exercise the Reader overload itself, so dvm.c's parser slice and its
  * diagnostic used-step logging run on real guest JSON code. */
 {
  static const uint8_t json_bytes[]="\"reader-probe\"";
  struct dvm_class *json_class=dvm_find_class(v,"copy/google/json/JSON");
  dvm_ref parser=json_class?dvm_new_object(v,json_class):0;
  dvm_ref string_class=dvm_class_object(v,dvm_find_class_or_external(v,"java/lang/String"));
  ISR_ST("JSON reader fixture constructor",parser&&string_class&&isr_st_call(v,parser,"<init>","()V",NULL,0,&q));
  dvm_ref text_reader=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/StringReader"));
  a[0].l=dvm_new_string(v,(const char*)json_bytes);
  /* StringReader's native constructor is exposed by io_runtime_call; its
   * optional boot method metadata is absent. Only fixture setup uses this
   * dispatcher directly; the JSON overload below still uses dvm_call. */
  ISR_ST("JSON StringReader fixture",text_reader&&io_runtime_call(v,"java/io/StringReader","<init>","(Ljava/lang/String;)V",text_reader,a,1,&q)&&!dvm_exception(v));
  a[0].l=text_reader;a[1].l=string_class;
  ISR_ST("JSON fromJson StringReader Class actual VM",isr_st_call(v,parser,"fromJson","(Ljava/io/Reader;Ljava/lang/Class;)Ljava/lang/Object;",a,2,&q)&&io_selftest_string(v,q.l,"reader-probe",12));
  dvm_ref byte_input=isr_st_input_new(v,json_bytes,sizeof json_bytes-1);
  dvm_ref byte_reader=dvm_new_object(v,dvm_find_class_or_external(v,"java/io/InputStreamReader"));
  a[0].l=byte_input;
  ISR_ST("JSON ISR fixture",byte_input&&byte_reader&&isr_st_call(v,byte_reader,"<init>","(Ljava/io/InputStream;)V",a,1,&q));
  a[0].l=byte_reader;a[1].l=string_class;
  ISR_ST("JSON fromJson ISR Class actual VM",isr_st_call(v,parser,"fromJson","(Ljava/io/Reader;Ljava/lang/Class;)Ljava/lang/Object;",a,2,&q)&&io_selftest_string(v,q.l,"reader-probe",12));
 }
done:
 if(f){fprintf(f,"INPUT_STREAM_READER_SELFTEST %s checks=%u\n",ok?"PASS":"FAIL",checks);fclose(f);}
 sdk_trace_fprintf(stderr,"INPUT_STREAM_READER_SELFTEST %s checks=%u\n",ok?"PASS":"FAIL",checks);
 dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;v->step_limit=limit;
#undef ISR_ST
 return ok;
}
