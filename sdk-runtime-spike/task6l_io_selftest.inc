/* TASK6l: actual VM lookup + dvm_call only; fixture data, no SDK callbacks. */
typedef struct {struct dvm *v;FILE *f;unsigned checks,failed;dvm_ref pins[96];unsigned npins;} task6l_test_ctx;
static void task6l_check(task6l_test_ctx *t,const char *name,bool ok){
 t->checks++;if(!ok)t->failed++;
 if(t->f)dprintf(fileno(t->f),"TASK6L_IO pid=%d vm=%llu check=%s result=%s exception=%u\n",getpid(),(unsigned long long)t->v->serial,name,ok?"PASS":"FAIL",t->v->exception);
 if(t->v->exception)dvm_clear_exception(t->v);
}
static dvm_ref task6l_hold(task6l_test_ctx *t,dvm_ref r){if(r&&t->npins<96){dvm_pin(t->v,r);t->pins[t->npins++]=r;}return r;}
static dvm_ref task6l_object(task6l_test_ctx *t,const char *name){struct dvm_class *c=dvm_find_class_or_external(t->v,name);return c?task6l_hold(t,dvm_new_object(t->v,c)):0;}
static bool task6l_call(task6l_test_ctx *t,dvm_ref r,const char *name,const char *sig,const union dvm_value *a,int n,union dvm_value *q){
 memset(q,0,sizeof *q);struct dvm_object *o=dvm__obj(t->v,r);struct dvm_method *m=o?dvm_find_method(t->v,o->cls,name,sig):NULL;
 bool ok=m&&dvm_call(t->v,m,r,a,n,q)&&!dvm_exception(t->v);
 if(t->f)dprintf(fileno(t->f),"TASK6L_CALL receiver=%s name=%s sig=%s resolved_owner=%s ok=%d exception=%u dvm_call=1\n",o&&o->cls?o->cls->name:"<null>",name,sig,m&&m->cls?m->cls->name:"<missing>",ok,t->v->exception);
 return ok;
}
static bool task6l_text(task6l_test_ctx *t,dvm_ref ref,const char *expected){const char *s=dvm_string_utf8(t->v,ref);return s&&dvm_string_utf8_length(t->v,ref)==strlen(expected)&&!memcmp(s,expected,strlen(expected));}
static void task6l_io_selftest(struct dvm *v){
 dvm_ref saved=v->exception;bool parked=v->parked;uint64_t steps=v->call_steps,limit=v->step_limit;
 v->exception=0;v->parked=false;v->call_steps=0;v->step_limit=100000;
 task6l_test_ctx t={.v=v,.f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task6l-io-selftest.txt","a")};
 union dvm_value q={0},a[3]={{0}};bool ok;
 const char *json="{\"a\":1}";
 dvm_ref text=task6l_hold(&t,dvm_new_string(v,json));
 dvm_ref sw=task6l_object(&t,"java/io/StringWriter");
 task6l_check(&t,"StringWriter.ctor",task6l_call(&t,sw,"<init>","()V",NULL,0,&q));
 a[0].l=text;task6l_check(&t,"StringWriter.write_string",task6l_call(&t,sw,"write","(Ljava/lang/String;)V",a,1,&q));
 ok=task6l_call(&t,sw,"toString","()Ljava/lang/String;",NULL,0,&q);
 const char *s=ok?dvm_string_utf8(v,q.l):NULL;
 task6l_check(&t,"StringWriter.json_exact_no_identity",ok&&task6l_text(&t,q.l,json)&&s&&!strchr(s,'@')&&!strstr(s,"StringWriter"));
 struct dvm_method *method=dvm_find_method(v,dvm_object_class(v,sw),"toString","()Ljava/lang/String;");
 task6l_check(&t,"StringWriter.toString_owner",method&&method->cls&&!strcmp(method->cls->name,"java/io/StringWriter"));
 dvm_ref sw_pair=task6l_object(&t,"java/io/StringWriter");
 task6l_check(&t,"StringWriter.surrogate_ctor",task6l_call(&t,sw_pair,"<init>","()V",NULL,0,&q));
 a[0].i=0xd83d;task6l_check(&t,"StringWriter.write_high_surrogate",task6l_call(&t,sw_pair,"write","(I)V",a,1,&q));
 a[0].i=0xde00;task6l_check(&t,"StringWriter.write_low_surrogate",task6l_call(&t,sw_pair,"write","(I)V",a,1,&q));
 ok=task6l_call(&t,sw_pair,"toString","()Ljava/lang/String;",NULL,0,&q);
 task6l_check(&t,"StringWriter.split_surrogate_exact",ok&&task6l_text(&t,q.l,"\xf0\x9f\x98\x80"));
 dvm_ref sw_range=task6l_object(&t,"java/io/StringWriter");
 task6l_check(&t,"StringWriter.range_ctor",task6l_call(&t,sw_range,"<init>","()V",NULL,0,&q));
 a[0].l=task6l_hold(&t,dvm_new_string(v,"\xe4\xb8\xad\xf0\x9f\x98\x80Z"));a[1].i=1;a[2].i=2;
 task6l_check(&t,"StringWriter.write_String_UTF16_range",task6l_call(&t,sw_range,"write","(Ljava/lang/String;II)V",a,3,&q));
 ok=task6l_call(&t,sw_range,"toString","()Ljava/lang/String;",NULL,0,&q);
 task6l_check(&t,"StringWriter.String_range_is_UTF16_not_bytes",ok&&task6l_text(&t,q.l,"\xf0\x9f\x98\x80"));
 dvm_ref sr=task6l_object(&t,"java/io/StringReader");a[0].l=task6l_hold(&t,dvm_new_string(v,"A\xf0\x9f\x98\x80Z"));
 task6l_check(&t,"StringReader.ctor",task6l_call(&t,sr,"<init>","(Ljava/lang/String;)V",a,1,&q));
 ok=task6l_call(&t,sr,"read","()I",NULL,0,&q);task6l_check(&t,"StringReader.read_A",ok&&q.i=='A');
 dvm_ref chars=task6l_hold(&t,dvm_new_array(v,'C',"C",6));
 a[0].l=chars;a[1].i=1;a[2].i=1;ok=task6l_call(&t,sr,"read","([CII)I",a,3,&q);
 uint16_t *cu=dvm_array_data(v,chars);task6l_check(&t,"StringReader.range_high_surrogate",ok&&q.i==1&&cu&&cu[0]==0&&cu[1]==0xd83d);
 ok=task6l_call(&t,sr,"read","()I",NULL,0,&q);task6l_check(&t,"StringReader.read_low_surrogate",ok&&q.i==0xde00);
 ok=task6l_call(&t,sr,"read","()I",NULL,0,&q);task6l_check(&t,"StringReader.read_Z",ok&&q.i=='Z');
 ok=task6l_call(&t,sr,"read","()I",NULL,0,&q);task6l_check(&t,"StringReader.EOF",ok&&q.i==-1);
 a[0].l=chars;a[1].i=2;a[2].i=0;ok=task6l_call(&t,sr,"read","([CII)I",a,3,&q);task6l_check(&t,"StringReader.zero_at_EOF",ok&&q.i==0);
 task6l_check(&t,"StringReader.reset",task6l_call(&t,sr,"reset","()V",NULL,0,&q));
 ok=task6l_call(&t,sr,"read","()I",NULL,0,&q);task6l_check(&t,"StringReader.reset_position",ok&&q.i=='A');
 /* A char array contains true UTF-16 units, with a supplementary character. */
 const uint16_t utf16[]={0x4e2d,0xd83d,0xde00,'Z'};const char *utf8="\xe4\xb8\xad\xf0\x9f\x98\x80Z";
 dvm_ref source=task6l_hold(&t,dvm_new_array(v,'C',"C",6));cu=dvm_array_data(v,source);if(cu)memcpy(cu+1,utf16,sizeof utf16);
 dvm_ref cw=task6l_object(&t,"java/io/CharArrayWriter");
 task6l_check(&t,"CharArrayWriter.ctor",task6l_call(&t,cw,"<init>","()V",NULL,0,&q));
 a[0].l=source;a[1].i=1;a[2].i=4;task6l_check(&t,"CharArrayWriter.write_range",task6l_call(&t,cw,"write","([CII)V",a,3,&q));
 ok=task6l_call(&t,cw,"toString","()Ljava/lang/String;",NULL,0,&q);task6l_check(&t,"CharArrayWriter.UTF16_toString",ok&&task6l_text(&t,q.l,utf8));
 ok=task6l_call(&t,cw,"toCharArray","()[C",NULL,0,&q);dvm_ref copy=ok?task6l_hold(&t,q.l):0;struct dvm_object *arr=dvm__obj(v,copy);
 task6l_check(&t,"CharArrayWriter.toCharArray_exact",ok&&arr&&arr->kind==DVM_OBJ_ARRAY&&arr->elem_kind=='C'&&arr->length==4&&!memcmp(dvm_array_data(v,copy),utf16,sizeof utf16));
 if(copy&&dvm_array_data(v,copy))((uint16_t*)dvm_array_data(v,copy))[0]='X';
 ok=task6l_call(&t,cw,"toString","()Ljava/lang/String;",NULL,0,&q);task6l_check(&t,"CharArrayWriter.toCharArray_defensive_copy",ok&&task6l_text(&t,q.l,utf8));
 dvm_ref cr=task6l_object(&t,"java/io/CharArrayReader");a[0].l=source;
 task6l_check(&t,"CharArrayReader.ctor",task6l_call(&t,cr,"<init>","([C)V",a,1,&q));
 ok=task6l_call(&t,cr,"read","()I",NULL,0,&q);task6l_check(&t,"CharArrayReader.read_leading_NUL",ok&&q.i==0);
 a[0].l=chars;a[1].i=1;a[2].i=4;ok=task6l_call(&t,cr,"read","([CII)I",a,3,&q);cu=dvm_array_data(v,chars);
 task6l_check(&t,"CharArrayReader.range_UTF16",ok&&q.i==4&&cu&&!memcmp(cu+1,utf16,sizeof utf16));
 dvm_ref cr_range=task6l_object(&t,"java/io/CharArrayReader");a[0].l=source;a[1].i=1;a[2].i=4;
 task6l_check(&t,"CharArrayReader.range_ctor",task6l_call(&t,cr_range,"<init>","([CII)V",a,3,&q));
 ok=task6l_call(&t,cr_range,"read","()I",NULL,0,&q);task6l_check(&t,"CharArrayReader.range_starts_at_offset",ok&&q.i==0x4e2d);
 a[0].i=20;task6l_check(&t,"CharArrayReader.mark",task6l_call(&t,cr_range,"mark","(I)V",a,1,&q));
 a[0].j=2;ok=task6l_call(&t,cr_range,"skip","(J)J",a,1,&q);task6l_check(&t,"CharArrayReader.skip_two_units",ok&&q.j==2);
 ok=task6l_call(&t,cr_range,"read","()I",NULL,0,&q);task6l_check(&t,"CharArrayReader.skip_position",ok&&q.i=='Z');
 ok=task6l_call(&t,cr_range,"read","()I",NULL,0,&q);task6l_check(&t,"CharArrayReader.range_EOF",ok&&q.i==-1);
 task6l_check(&t,"CharArrayReader.mark_reset",task6l_call(&t,cr_range,"reset","()V",NULL,0,&q));
 ok=task6l_call(&t,cr_range,"read","()I",NULL,0,&q);task6l_check(&t,"CharArrayReader.reset_returns_marked_surrogate",ok&&q.i==0xd83d);
 a[0].j=-1;ok=task6l_call(&t,cr_range,"skip","(J)J",a,1,&q);task6l_check(&t,"CharArrayReader.negative_skip_zero",ok&&q.j==0);
 task6l_check(&t,"CharArrayReader.range_close",task6l_call(&t,cr_range,"close","()V",NULL,0,&q));
 ok=task6l_call(&t,cr_range,"ready","()Z",NULL,0,&q);struct dvm_object *closed_exc=dvm__obj(v,v->exception);
 task6l_check(&t,"CharArrayReader.closed_ready_IOException",!ok&&closed_exc&&closed_exc->cls&&!strcmp(closed_exc->cls->name,"java/io/IOException"));
 task6l_check(&t,"CharArrayReader.close",task6l_call(&t,cr,"close","()V",NULL,0,&q));
 ok=task6l_call(&t,cr,"read","()I",NULL,0,&q);struct dvm_object *exc=dvm__obj(v,v->exception);
 task6l_check(&t,"CharArrayReader.closed_IOException",!ok&&exc&&exc->cls&&!strcmp(exc->cls->name,"java/io/IOException"));
 dvm_ref bo=task6l_object(&t,"java/io/ByteArrayOutputStream");
 task6l_check(&t,"ByteArrayOutputStream.ctor",task6l_call(&t,bo,"<init>","()V",NULL,0,&q));
 size_t blen=strlen(utf8);dvm_ref bytes=task6l_hold(&t,dvm_new_array(v,'B',"B",(uint32_t)blen));if(bytes&&dvm_array_data(v,bytes))memcpy(dvm_array_data(v,bytes),utf8,blen);
 a[0].l=bytes;a[1].i=0;a[2].i=(int32_t)blen;task6l_check(&t,"ByteArrayOutputStream.write_range",task6l_call(&t,bo,"write","([BII)V",a,3,&q));
 ok=task6l_call(&t,bo,"toString","()Ljava/lang/String;",NULL,0,&q);task6l_check(&t,"ByteArrayOutputStream.default_UTF8",ok&&task6l_text(&t,q.l,utf8));
 a[0].l=task6l_hold(&t,dvm_new_string(v,"UTF-8"));ok=task6l_call(&t,bo,"toString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&q);task6l_check(&t,"ByteArrayOutputStream.named_UTF8",ok&&task6l_text(&t,q.l,utf8));
 ok=task6l_call(&t,bo,"toByteArray","()[B",NULL,0,&q);arr=dvm__obj(v,q.l);copy=ok?task6l_hold(&t,q.l):0;
 task6l_check(&t,"ByteArrayOutputStream.toByteArray_exact",ok&&arr&&arr->kind==DVM_OBJ_ARRAY&&arr->elem_kind=='B'&&arr->length==blen&&!memcmp(dvm_array_data(v,copy),utf8,blen));
 if(copy&&dvm_array_data(v,copy))((uint8_t*)dvm_array_data(v,copy))[0]='X';
 ok=task6l_call(&t,bo,"toString","()Ljava/lang/String;",NULL,0,&q);task6l_check(&t,"ByteArrayOutputStream.toByteArray_defensive_copy",ok&&task6l_text(&t,q.l,utf8));
 dvm_ref latin_bo=task6l_object(&t,"java/io/ByteArrayOutputStream");
 task6l_check(&t,"ByteArrayOutputStream.Latin1_ctor",task6l_call(&t,latin_bo,"<init>","()V",NULL,0,&q));
 dvm_ref latin_bytes=task6l_hold(&t,dvm_new_array(v,'B',"B",2));if(latin_bytes&&dvm_array_data(v,latin_bytes)){((uint8_t*)dvm_array_data(v,latin_bytes))[0]=0xe9;((uint8_t*)dvm_array_data(v,latin_bytes))[1]=0xff;}
 a[0].l=latin_bytes;a[1].i=0;a[2].i=2;task6l_check(&t,"ByteArrayOutputStream.Latin1_write",task6l_call(&t,latin_bo,"write","([BII)V",a,3,&q));
 a[0].l=task6l_hold(&t,dvm_new_string(v,"ISO-8859-1"));ok=task6l_call(&t,latin_bo,"toString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&q);
 task6l_check(&t,"ByteArrayOutputStream.Latin1_decodes",ok&&task6l_text(&t,q.l,"\xc3\xa9\xc3\xbf"));
 dvm_ref bo_target=task6l_object(&t,"java/io/ByteArrayOutputStream");
 task6l_check(&t,"ByteArrayOutputStream.writeTo_target_ctor",task6l_call(&t,bo_target,"<init>","()V",NULL,0,&q));
 a[0].l=bo_target;task6l_check(&t,"ByteArrayOutputStream.writeTo_dispatch",task6l_call(&t,bo,"writeTo","(Ljava/io/OutputStream;)V",a,1,&q));
 ok=task6l_call(&t,bo_target,"toByteArray","()[B",NULL,0,&q);arr=dvm__obj(v,q.l);
 task6l_check(&t,"ByteArrayOutputStream.writeTo_exact",ok&&arr&&arr->kind==DVM_OBJ_ARRAY&&arr->elem_kind=='B'&&arr->length==blen&&!memcmp(dvm_array_data(v,q.l),utf8,blen));
 ok=task6l_call(&t,bo,"size","()I",NULL,0,&q);task6l_check(&t,"ByteArrayOutputStream.writeTo_source_not_consumed",ok&&q.i==(int32_t)blen);
 a[0].i='!';task6l_check(&t,"ByteArrayOutputStream.writeTo_target_not_closed",task6l_call(&t,bo_target,"write","(I)V",a,1,&q));
 ok=task6l_call(&t,bo_target,"size","()I",NULL,0,&q);task6l_check(&t,"ByteArrayOutputStream.writeTo_target_still_writable",ok&&q.i==(int32_t)blen+1);
 a[0].i='!';task6l_check(&t,"ByteArrayOutputStream.writeTo_source_not_closed",task6l_call(&t,bo,"write","(I)V",a,1,&q));
 ok=task6l_call(&t,bo,"size","()I",NULL,0,&q);task6l_check(&t,"ByteArrayOutputStream.writeTo_source_still_writable",ok&&q.i==(int32_t)blen+1);
 /* Reader layering through the normal declared methods, no direct runtime call. */
 dvm_ref ba=task6l_object(&t,"java/io/ByteArrayInputStream");a[0].l=bytes;
 task6l_check(&t,"ByteArrayInputStream.ctor",task6l_call(&t,ba,"<init>","([B)V",a,1,&q));
 dvm_ref isr=task6l_object(&t,"java/io/InputStreamReader");a[0].l=ba;
 task6l_check(&t,"InputStreamReader.ctor",task6l_call(&t,isr,"<init>","(Ljava/io/InputStream;)V",a,1,&q));
 dvm_ref br=task6l_object(&t,"java/io/BufferedReader");a[0].l=isr;
 task6l_check(&t,"BufferedReader.ctor",task6l_call(&t,br,"<init>","(Ljava/io/Reader;)V",a,1,&q));
 a[0].l=chars;a[1].i=1;a[2].i=4;ok=task6l_call(&t,br,"read","([CII)I",a,3,&q);cu=dvm_array_data(v,chars);
 task6l_check(&t,"BufferedReader.layered_UTF16",ok&&q.i==4&&cu&&!memcmp(cu+1,utf16,sizeof utf16));
 ok=task6l_call(&t,br,"read","()I",NULL,0,&q);task6l_check(&t,"BufferedReader.layered_EOF",ok&&q.i==-1);
 task6l_check(&t,"BufferedReader.close",task6l_call(&t,br,"close","()V",NULL,0,&q));
 dvm_ref object=task6l_object(&t,"java/lang/Object");
 task6l_check(&t,"Object.ctor",task6l_call(&t,object,"<init>","()V",NULL,0,&q));
 ok=task6l_call(&t,object,"toString","()Ljava/lang/String;",NULL,0,&q);s=ok?dvm_string_utf8(v,q.l):NULL;
 task6l_check(&t,"Object.identity_unchanged",ok&&s&&!strncmp(s,"java.lang.Object@",17));
 method=dvm_find_method(v,dvm_object_class(v,object),"toString","()Ljava/lang/String;");
 task6l_check(&t,"Object.toString_owner_still_Object",method&&method->cls&&!strcmp(method->cls->name,"java/lang/Object"));
 dvm_ref pw=task6l_object(&t,"java/io/PrintWriter");a[0].l=sw;
 task6l_check(&t,"PrintWriter.identity_ctor",task6l_call(&t,pw,"<init>","(Ljava/io/Writer;)V",a,1,&q));
 method=dvm_find_method(v,dvm_object_class(v,pw),"toString","()Ljava/lang/String;");
 task6l_check(&t,"PrintWriter.no_invented_toString_override",method&&method->cls&&!strcmp(method->cls->name,"java/lang/Object"));
 ok=task6l_call(&t,pw,"toString","()Ljava/lang/String;",NULL,0,&q);s=ok?dvm_string_utf8(v,q.l):NULL;
 task6l_check(&t,"PrintWriter.identity_unchanged",ok&&s&&!strncmp(s,"java.io.PrintWriter@",20));
 method=dvm_find_method(v,dvm_object_class(v,br),"toString","()Ljava/lang/String;");
 task6l_check(&t,"BufferedReader.no_invented_toString_override",method&&method->cls&&!strcmp(method->cls->name,"java/lang/Object"));
 ok=task6l_call(&t,br,"toString","()Ljava/lang/String;",NULL,0,&q);s=ok?dvm_string_utf8(v,q.l):NULL;
 task6l_check(&t,"BufferedReader.identity_unchanged_after_close",ok&&s&&!strncmp(s,"java.io.BufferedReader@",23));
 if(t.f){dprintf(fileno(t.f),"TASK6L_IO_SUMMARY pid=%d vm=%llu checks=%u failed=%u pass=%d local_fixture=1 no_network=1 no_callback=1 steps=%llu fixture_limit=100000 production_limit_unchanged=1\n",getpid(),(unsigned long long)v->serial,t.checks,t.failed,t.failed==0,(unsigned long long)v->call_steps);fclose(t.f);}
 for(unsigned i=0;i<t.npins;i++)dvm_unpin(v,t.pins[i]);
 v->exception=saved;v->parked=parked;v->call_steps=steps;v->step_limit=limit;
}
