/* Actual VM allocation, method resolution and dvm_call; no mock streams. */
static bool bais_st_call(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *args,int n,union dvm_value *q){
 struct dvm_method *method=dvm_find_method(v,dvm_object_class(v,self),name,sig);
 memset(q,0,sizeof *q);return method&&dvm_call(v,method,self,args,n,q)&&!dvm_exception(v);
}
bool io_runtime_byte_array_input_selftest(struct dvm *v){
 dvm_ref saved=dvm_exception(v);bool parked=v->parked;uint64_t steps=v->call_steps,limit=v->step_limit;
 dvm_clear_exception(v);v->parked=false;v->call_steps=0;v->step_limit=2000000;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/byte-array-input-selftest.txt","w");
 bool ok=true;unsigned checks=0;union dvm_value q={0},a[3]={{0}};
 dvm_ref stream=0,slice=0,bad=0,bytes=0,dst=0;struct dvm_class *k=NULL;
#define BAIS_ST(label,expr) do{if(!(expr)){if(f)fprintf(f,"FAIL %s line=%d exception=%u\n",label,__LINE__,dvm_exception(v));ok=false;goto done;}checks++;if(f)fprintf(f,"PASS %s\n",label);}while(0)
 BAIS_ST("class existence",dvm_class_exists(v,"Ljava/io/ByteArrayInputStream;"));
 k=dvm_find_class_or_external(v,"java/io/ByteArrayInputStream");
 stream=dvm_new_object(v,k);slice=dvm_new_object(v,k);bad=dvm_new_object(v,k);
 bytes=dvm_new_array(v,'B',"B",5);dst=dvm_new_array(v,'B',"B",8);
 BAIS_ST("actual VM objects",stream&&slice&&bad&&bytes&&dst&&!dvm_exception(v));
 {const uint8_t data[]={0,127,128,255,42};memcpy(dvm_array_data(v,bytes),data,sizeof data);}
 a[0].l=bytes;
 BAIS_ST("constructor byte array",bais_st_call(v,stream,"<init>","([B)V",a,1,&q));
 BAIS_ST("protected buf retains original array",dvm_get_field(v,stream,"buf","[B",&q)&&q.l==bytes);
 BAIS_ST("initial available",bais_st_call(v,stream,"available","()I",NULL,0,&q)&&q.i==5);
 BAIS_ST("read unsigned zero",bais_st_call(v,stream,"read","()I",NULL,0,&q)&&q.i==0);
 a[0].i=-1;
 BAIS_ST("mark ignores negative limit",bais_st_call(v,stream,"mark","(I)V",a,1,&q));
 ((uint8_t*)dvm_array_data(v,bytes))[1]=129;
 BAIS_ST("buffer alias and unsigned read",bais_st_call(v,stream,"read","()I",NULL,0,&q)&&q.i==129);
 BAIS_ST("read unsigned 128",bais_st_call(v,stream,"read","()I",NULL,0,&q)&&q.i==128);
 BAIS_ST("reset mark",bais_st_call(v,stream,"reset","()V",NULL,0,&q));
 a[0].l=dst;a[1].i=2;a[2].i=3;
 BAIS_ST("range read",bais_st_call(v,stream,"read","([BII)I",a,3,&q)&&q.i==3);
 BAIS_ST("range content",((uint8_t*)dvm_array_data(v,dst))[2]==129&&((uint8_t*)dvm_array_data(v,dst))[3]==128&&((uint8_t*)dvm_array_data(v,dst))[4]==255);
 a[0].j=-4;
 BAIS_ST("negative skip",bais_st_call(v,stream,"skip","(J)J",a,1,&q)&&q.j==0);
 a[0].j=INT64_MAX;
 BAIS_ST("skip capped at remaining",bais_st_call(v,stream,"skip","(J)J",a,1,&q)&&q.j==1);
 BAIS_ST("EOF single byte",bais_st_call(v,stream,"read","()I",NULL,0,&q)&&q.i==-1);
 a[0].l=dst;a[1].i=0;a[2].i=0;
 BAIS_ST("zero range at EOF is minus one",bais_st_call(v,stream,"read","([BII)I",a,3,&q)&&q.i==-1);
 BAIS_ST("mark supported",bais_st_call(v,stream,"markSupported","()Z",NULL,0,&q)&&q.i==1);
 BAIS_ST("close no-op",bais_st_call(v,stream,"close","()V",NULL,0,&q));
 BAIS_ST("reset after close",bais_st_call(v,stream,"reset","()V",NULL,0,&q));
 BAIS_ST("read after close",bais_st_call(v,stream,"read","()I",NULL,0,&q)&&q.i==129);
 a[0].l=bytes;a[1].i=2;a[2].i=99;
 BAIS_ST("slice constructor clamps count",bais_st_call(v,slice,"<init>","([BII)V",a,3,&q));
 BAIS_ST("slice available",bais_st_call(v,slice,"available","()I",NULL,0,&q)&&q.i==3);
 a[0].l=dst;
 BAIS_ST("whole buffer read partial",bais_st_call(v,slice,"read","([B)I",a,1,&q)&&q.i==3&&((uint8_t*)dvm_array_data(v,dst))[0]==128&&((uint8_t*)dvm_array_data(v,dst))[1]==255&&((uint8_t*)dvm_array_data(v,dst))[2]==42);
 a[0].l=dst;a[1].i=INT32_MAX;a[2].i=2;
 (void)bais_st_call(v,slice,"read","([BII)I",a,3,&q);
 BAIS_ST("range validation before EOF",io_selftest_exception_named(v,"java/lang/IndexOutOfBoundsException"));
 a[0].l=0;
 (void)bais_st_call(v,bad,"<init>","([B)V",a,1,&q);
 BAIS_ST("null constructor",io_selftest_exception_named(v,"java/lang/NullPointerException"));
 (void)bais_st_call(v,stream,"read","([B)I",a,1,&q);
 BAIS_ST("null destination",io_selftest_exception_named(v,"java/lang/NullPointerException"));
 BAIS_ST("superclass dispatch",io_runtime_call(v,"java/io/InputStream","read","()I",stream,NULL,0,&q)&&q.i==128&&!dvm_exception(v));
done:
 if(f){fprintf(f,"BYTE_ARRAY_INPUT_SELFTEST %s checks=%u\n",ok?"PASS":"FAIL",checks);fclose(f);}
 sdk_trace_fprintf(stderr,"BYTE_ARRAY_INPUT_SELFTEST %s checks=%u\n",ok?"PASS":"FAIL",checks);
 dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;v->step_limit=limit;
#undef BAIS_ST
 return ok;
}
