/* Included by string_runtime.c: real guest Buffer/Segment objects, no mock IO. */
static bool okio_st_dispatch(struct dvm *v,dvm_ref ref,const char *name,const char *sig,const uint32_t *a,int n,union dvm_value *q){
 memset(q,0,sizeof *q);return string_runtime_okio_buffer(v,ref,name,sig,a,n,q)&&!dvm_exception(v);
}
static bool okio_st_guest(struct dvm *v,dvm_ref ref,const char *name,const char *sig,union dvm_value *q){
 struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,ref),name,sig);
 return m&&dvm_call(v,m,ref,NULL,0,q)&&!dvm_exception(v);
}
static dvm_ref okio_st_buffer(struct dvm *v){
 struct dvm_class *k=dvm_find_class(v,"okio/Buffer");if(!k)return 0;
 dvm_ref ref=dvm_new_object(v,k);union dvm_value q={0};return ref&&okio_st_guest(v,ref,"<init>","()V",&q)?ref:0;
}
static dvm_ref okio_st_ref(struct dvm *v,dvm_ref ref,const char *field,const char *type){union dvm_value q={0};return dvm_get_field(v,ref,field,type,&q)?q.l:0;}
static int64_t okio_st_number(struct dvm *v,dvm_ref ref,const char *field,const char *type){union dvm_value q={0};if(!dvm_get_field(v,ref,field,type,&q))return INT64_MIN;return *type=='J'?q.j:q.i;}
static bool okio_st_exception(struct dvm *v,const char *name){struct dvm_class *k=dvm_object_class(v,dvm_exception(v));bool ok=k&&!strcmp(k->name,name);dvm_clear_exception(v);return ok;}
bool string_runtime_okio_selftest(struct dvm *v){
 dvm_ref saved=dvm_exception(v);bool parked=v->parked;uint64_t steps=v->call_steps,limit=v->step_limit;
 dvm_clear_exception(v);v->parked=false;v->call_steps=0;v->step_limit=2000000;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/okio-selftest.txt","w");
 bool ok=true;unsigned checks=0;union dvm_value q={0};uint32_t a[3]={0};
#define OKIO_ST(label,expr) do{if(!(expr)){if(f)fprintf(f,"FAIL %s line=%d exception=%u\n",label,__LINE__,dvm_exception(v));ok=false;goto done;}checks++;if(f)fprintf(f,"PASS %s\n",label);}while(0)
 dvm_ref b=okio_st_buffer(v),source=dvm_new_array(v,'B',"B",8208);
 OKIO_ST("real Buffer constructor and byte array",b&&source&&!dvm_exception(v));
 unsigned char expected[8208];for(unsigned i=0;i<sizeof expected;i++)expected[i]=(unsigned char)(i*37+11);
 memcpy(dvm_array_data(v,source),expected,sizeof expected);
 a[0]=source;a[1]=0;a[2]=8191;
 OKIO_ST("write 8191 returns this",okio_st_dispatch(v,b,"write","([BII)Lokio/Buffer;",a,3,&q)&&q.l==b);
 dvm_ref head=okio_st_ref(v,b,"head","Lokio/Segment;");
 OKIO_ST("first segment circular and limit 8191",head&&okio_st_ref(v,head,"next","Lokio/Segment;")==head&&okio_st_ref(v,head,"prev","Lokio/Segment;")==head&&okio_st_number(v,head,"limit","I")==8191);
 a[1]=8191;a[2]=17;
 OKIO_ST("cross 8192 returns this",okio_st_dispatch(v,b,"write","([BII)Lokio/Buffer;",a,3,&q)&&q.l==b);
 dvm_ref tail=okio_st_ref(v,head,"prev","Lokio/Segment;");
 OKIO_ST("two segment links limits and size",tail&&tail!=head&&okio_st_ref(v,head,"next","Lokio/Segment;")==tail&&okio_st_ref(v,tail,"next","Lokio/Segment;")==head&&okio_st_ref(v,tail,"prev","Lokio/Segment;")==head&&okio_st_number(v,head,"limit","I")==8192&&okio_st_number(v,tail,"limit","I")==16&&okio_st_number(v,b,"size","J")==8208);
 uint32_t search[5]={expected[8192],8190,0,8193,0};
 OKIO_ST("indexOf scans across segment boundary",okio_st_dispatch(v,b,"indexOf","(BJJ)J",search,5,&q)&&q.j==8192);
 search[3]=8192;
 OKIO_ST("indexOf excludes toIndex boundary",okio_st_dispatch(v,b,"indexOf","(BJJ)J",search,5,&q)&&q.j==-1);
 search[1]=8193;search[3]=8208;
 OKIO_ST("indexOf excludes earlier matching fromIndex",okio_st_dispatch(v,b,"indexOf","(BJJ)J",search,5,&q)&&q.j==-1);
 search[1]=UINT32_MAX;search[2]=UINT32_MAX;search[3]=8208;
 string_runtime_okio_buffer(v,b,"indexOf","(BJJ)J",search,5,&q);
 OKIO_ST("indexOf rejects negative fromIndex",okio_st_exception(v,"java/lang/IllegalArgumentException"));
 search[1]=5;search[2]=0;search[3]=4;
 string_runtime_okio_buffer(v,b,"indexOf","(BJJ)J",search,5,&q);
 OKIO_ST("indexOf rejects reversed range",okio_st_exception(v,"java/lang/IllegalArgumentException"));
 search[1]=8208;search[3]=8208;
 OKIO_ST("indexOf empty range returns minus one",okio_st_dispatch(v,b,"indexOf","(BJJ)J",search,5,&q)&&q.j==-1);
 OKIO_ST("original DEX readByteArray exact cross segment bytes",okio_st_guest(v,b,"readByteArray","()[B",&q)&&dvm_array_length(v,q.l)==sizeof expected&&!memcmp(dvm_array_data(v,q.l),expected,sizeof expected));
 OKIO_ST("read consumes size and head",okio_st_number(v,b,"size","J")==0&&!okio_st_ref(v,b,"head","Lokio/Segment;"));
 b=okio_st_buffer(v);a[0]=source;a[1]=0;a[2]=32;
 OKIO_ST("prepare real nonzero pos read",b&&okio_st_dispatch(v,b,"write","([BII)Lokio/Buffer;",a,3,&q)&&okio_st_guest(v,b,"readByte","()B",&q)&&q.i==(int8_t)expected[0]);
 head=okio_st_ref(v,b,"head","Lokio/Segment;");search[0]=expected[7];
 OKIO_ST("indexOf single argument uses logical offset after pos",okio_st_number(v,head,"pos","I")==1&&okio_st_dispatch(v,b,"indexOf","(B)J",search,1,&q)&&q.j==6);
 search[1]=7;search[2]=0;
 OKIO_ST("indexOf byte long from skips earlier match",okio_st_dispatch(v,b,"indexOf","(BJ)J",search,3,&q)&&q.j==-1);
 b=okio_st_buffer(v);a[0]=source;a[1]=8208;a[2]=0;
 OKIO_ST("zero count at array end leaves head null",b&&okio_st_dispatch(v,b,"write","([BII)Lokio/Buffer;",a,3,&q)&&q.l==b&&!okio_st_ref(v,b,"head","Lokio/Segment;")&&okio_st_number(v,b,"size","J")==0);
 const int32_t ranges[4][2]={{-1,1},{0,-1},{8209,0},{8200,9}};
 for(unsigned i=0;i<4;i++){a[1]=(uint32_t)ranges[i][0];a[2]=(uint32_t)ranges[i][1];string_runtime_okio_buffer(v,b,"write","([BII)Lokio/Buffer;",a,3,&q);OKIO_ST("invalid offset/count throws and leaves Buffer unchanged",okio_st_exception(v,"java/lang/ArrayIndexOutOfBoundsException")&&!okio_st_ref(v,b,"head","Lokio/Segment;")&&okio_st_number(v,b,"size","J")==0);}
 a[0]=0;a[1]=0;a[2]=0;string_runtime_okio_buffer(v,b,"write","([BII)Lokio/Buffer;",a,3,&q);
 OKIO_ST("null source throws IllegalArgumentException",okio_st_exception(v,"java/lang/IllegalArgumentException"));
 a[0]=0;string_runtime_okio_buffer(v,b,"writableSegment","(I)Lokio/Segment;",a,1,&q);
 OKIO_ST("minimum 0 rejected without allocation",okio_st_exception(v,"java/lang/IllegalArgumentException")&&!okio_st_ref(v,b,"head","Lokio/Segment;"));
 a[0]=8193;string_runtime_okio_buffer(v,b,"writableSegment","(I)Lokio/Segment;",a,1,&q);
 OKIO_ST("minimum 8193 rejected without allocation",okio_st_exception(v,"java/lang/IllegalArgumentException")&&!okio_st_ref(v,b,"head","Lokio/Segment;"));
 a[0]=1;OKIO_ST("minimum 1 creates real segment",okio_st_dispatch(v,b,"writableSegment","(I)Lokio/Segment;",a,1,&q)&&q.l);
 head=q.l;dvm_ref data=okio_st_ref(v,head,"data","[B");
 OKIO_ST("new or pooled segment has 8192 bytes owner and clean range",data&&dvm_array_length(v,data)==8192&&okio_st_number(v,head,"owner","Z")==1&&okio_st_number(v,head,"shared","Z")==0&&okio_st_number(v,head,"pos","I")==0&&okio_st_number(v,head,"limit","I")==0);
 union dvm_value one={.i=1},zero={.i=0};
 OKIO_ST("configure owned shared tail",dvm_set_field(v,head,"shared","Z",one));
 OKIO_ST("owned shared tail remains appendable",okio_st_dispatch(v,b,"writableSegment","(I)Lokio/Segment;",a,1,&q)&&q.l==head);
 OKIO_ST("configure nonowner tail",dvm_set_field(v,head,"owner","Z",zero));
 OKIO_ST("nonowner tail allocates different owner",okio_st_dispatch(v,b,"writableSegment","(I)Lokio/Segment;",a,1,&q)&&q.l!=head&&okio_st_number(v,q.l,"owner","Z")==1&&okio_st_ref(v,head,"prev","Lokio/Segment;")==q.l);
 b=okio_st_buffer(v);const uint16_t units[]={'A',0,0xd83d,0xde00,0xd800,'B',0xdc00};
 dvm_ref str=from_units(v,units,7);a[0]=str;
 const unsigned char utf8[]={'A',0,0xf0,0x9f,0x98,0x80,'?','B','?'};
 OKIO_ST("single String UTF8 returns this",b&&str&&okio_st_dispatch(v,b,"writeUtf8","(Ljava/lang/String;)Lokio/Buffer;",a,1,&q)&&q.l==b);
 OKIO_ST("original DEX reads NUL nonBMP and lone surrogate replacement",okio_st_guest(v,b,"readByteArray","()[B",&q)&&dvm_array_length(v,q.l)==sizeof utf8&&!memcmp(dvm_array_data(v,q.l),utf8,sizeof utf8));
 a[0]=str;a[1]=2;a[2]=4;
 OKIO_ST("UTF16 subrange selects surrogate pair",okio_st_dispatch(v,b,"writeUtf8","(Ljava/lang/String;II)Lokio/Buffer;",a,3,&q)&&q.l==b&&okio_st_guest(v,b,"readByteArray","()[B",&q)&&dvm_array_length(v,q.l)==4&&!memcmp(dvm_array_data(v,q.l),utf8+2,4));
 a[1]=2;a[2]=3;
 OKIO_ST("subrange splitting pair writes question mark",okio_st_dispatch(v,b,"writeUtf8","(Ljava/lang/String;II)Lokio/Buffer;",a,3,&q)&&okio_st_guest(v,b,"readByteArray","()[B",&q)&&dvm_array_length(v,q.l)==1&&((unsigned char*)dvm_array_data(v,q.l))[0]=='?');
 a[0]=0;string_runtime_okio_buffer(v,b,"writeUtf8","(Ljava/lang/String;)Lokio/Buffer;",a,1,&q);
 OKIO_ST("single String null follows length NullPointerException",okio_st_exception(v,"java/lang/NullPointerException"));
 a[1]=0;a[2]=0;string_runtime_okio_buffer(v,b,"writeUtf8","(Ljava/lang/String;II)Lokio/Buffer;",a,3,&q);
 OKIO_ST("three argument null follows IllegalArgumentException",okio_st_exception(v,"java/lang/IllegalArgumentException"));
 b=okio_st_buffer(v);a[0]=0;
 OKIO_ST("codepoint NUL returns this",b&&okio_st_dispatch(v,b,"writeUtf8CodePoint","(I)Lokio/Buffer;",a,1,&q)&&q.l==b);
 a[0]=0x1f600;
 OKIO_ST("codepoint nonBMP returns this",okio_st_dispatch(v,b,"writeUtf8CodePoint","(I)Lokio/Buffer;",a,1,&q)&&q.l==b);
 a[0]=0xd800;
 OKIO_ST("codepoint surrogate returns this",okio_st_dispatch(v,b,"writeUtf8CodePoint","(I)Lokio/Buffer;",a,1,&q)&&q.l==b);
 /* Original code=0x42028 branches cp<128 directly to writeByte(cp), so
  * negative int values are truncated to one byte rather than rejected. */
 a[0]=UINT32_MAX;
 OKIO_ST("negative codepoint preserves original writeByte truncation",okio_st_dispatch(v,b,"writeUtf8CodePoint","(I)Lokio/Buffer;",a,1,&q)&&q.l==b);
 const unsigned char points[]={0,0xf0,0x9f,0x98,0x80,'?',0xff};
 OKIO_ST("original DEX codepoint byte readback",okio_st_guest(v,b,"readByteArray","()[B",&q)&&dvm_array_length(v,q.l)==sizeof points&&!memcmp(dvm_array_data(v,q.l),points,sizeof points));
 a[0]=0x110000;string_runtime_okio_buffer(v,b,"writeUtf8CodePoint","(I)Lokio/Buffer;",a,1,&q);
 OKIO_ST("codepoint above Unicode maximum rejected without writing",okio_st_exception(v,"java/lang/IllegalArgumentException")&&okio_st_number(v,b,"size","J")==0);
 dvm_ref source_buffer=okio_st_buffer(v);b=okio_st_buffer(v);a[0]=source;a[1]=0;a[2]=8208;
 OKIO_ST("prepare real Buffer Source for writeAll",source_buffer&&b&&okio_st_dispatch(v,source_buffer,"write","([BII)Lokio/Buffer;",a,3,&q));
 a[0]=source_buffer;
 OKIO_ST("writeAll reads real Source returns total bytes",okio_st_dispatch(v,b,"writeAll","(Lokio/Source;)J",a,1,&q)&&q.j==8208&&okio_st_number(v,source_buffer,"size","J")==0&&okio_st_number(v,b,"size","J")==8208);
 OKIO_ST("writeAll original DEX exact byte readback",okio_st_guest(v,b,"readByteArray","()[B",&q)&&dvm_array_length(v,q.l)==sizeof expected&&!memcmp(dvm_array_data(v,q.l),expected,sizeof expected));
 OKIO_ST("writeAll empty Source returns zero",okio_st_dispatch(v,b,"writeAll","(Lokio/Source;)J",a,1,&q)&&q.j==0&&okio_st_number(v,b,"size","J")==0);
 a[0]=0;string_runtime_okio_buffer(v,b,"writeAll","(Lokio/Source;)J",a,1,&q);
 OKIO_ST("writeAll null Source IllegalArgumentException",okio_st_exception(v,"java/lang/IllegalArgumentException")&&okio_st_number(v,b,"size","J")==0);
done:
 if(f){fprintf(f,"RESULT ok=%d checks=%u\n",ok,checks);fclose(f);}
 dvm_clear_exception(v);v->exception=saved;v->parked=parked;v->call_steps=steps;v->step_limit=limit;
#undef OKIO_ST
 return ok;
}
