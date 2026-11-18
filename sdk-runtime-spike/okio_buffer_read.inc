/* C implementation of the shipped Okio Buffer readers.  Storage remains the
 * original circular Segment list; recycled segments go through SegmentPool. */
struct okio_read_segment {dvm_ref ref,data,next,prev;int32_t pos,limit;};
static bool okio_read_segment(struct dvm *v,dvm_ref ref,struct okio_read_segment *s){
 union dvm_value q={0};memset(s,0,sizeof *s);s->ref=ref;
 if(!ref){dvm__throw(v,"java/lang/IllegalStateException","missing Okio head");return false;}
#define OR_FIELD(key,type,dst,member) do{if(!dvm_get_field(v,ref,key,type,&q))return false;s->dst=q.member;}while(0)
 OR_FIELD("data","[B",data,l);OR_FIELD("next","Lokio/Segment;",next,l);OR_FIELD("prev","Lokio/Segment;",prev,l);OR_FIELD("pos","I",pos,i);OR_FIELD("limit","I",limit,i);
#undef OR_FIELD
 struct dvm_object *a=dvm__obj(v,s->data);
 if(!a||a->kind!=DVM_OBJ_ARRAY||a->elem_kind!='B'||s->pos<0||s->limit<s->pos||(uint32_t)s->limit>a->length){dvm__throw(v,"java/lang/IllegalStateException","invalid Okio segment bounds");return false;}
 return true;
}
static bool okio_read_state(struct dvm *v,dvm_ref self,int64_t *size,dvm_ref *head){union dvm_value a={0},b={0};if(!dvm_get_field(v,self,"size","J",&a)||!dvm_get_field(v,self,"head","Lokio/Segment;",&b))return false;*size=a.j;*head=b.l;return true;}
static bool okio_read_recycle(struct dvm *v,dvm_ref self,struct okio_read_segment *s){
 union dvm_value next={.l=s->next==s->ref?0:s->next},prev={.l=s->prev},zero={0};
 if(!dvm_set_field(v,s->prev,"next","Lokio/Segment;",(union dvm_value){.l=s->next})||!dvm_set_field(v,s->next,"prev","Lokio/Segment;",prev)||!dvm_set_field(v,self,"head","Lokio/Segment;",next)||!dvm_set_field(v,s->ref,"next","Lokio/Segment;",zero)||!dvm_set_field(v,s->ref,"prev","Lokio/Segment;",zero))return false;
 struct dvm_class *pool=dvm_find_class(v,"okio/SegmentPool");struct dvm_method *m=pool?dvm_find_method(v,pool,"recycle","(Lokio/Segment;)V"):NULL;union dvm_value a={.l=s->ref},q={0};
 if(!m){dvm__throw(v,"java/lang/NoSuchMethodError","SegmentPool.recycle");return false;}
 return dvm_call(v,m,0,&a,1,&q)&&!dvm_exception(v);
}
/* Copy or skip exactly count, updating the live list after each head segment. */
static bool okio_read_consume(struct dvm *v,dvm_ref self,unsigned char *dst,int64_t count){
 int64_t size;dvm_ref head;if(!okio_read_state(v,self,&size,&head))return false;
 while(count>0){struct okio_read_segment s;if(!okio_read_segment(v,head,&s))return false;int take=s.limit-s.pos;if((int64_t)take>count)take=(int)count;
  if(take<=0){dvm__throw(v,"java/lang/IllegalStateException","empty Okio head");return false;}
  if(dst){memmove(dst,(unsigned char*)dvm_array_data(v,s.data)+s.pos,(size_t)take);dst+=take;}
  s.pos+=take;size-=take;count-=take;
  if(!dvm_set_field(v,s.ref,"pos","I",(union dvm_value){.i=s.pos})||!dvm_set_field(v,self,"size","J",(union dvm_value){.j=size}))return false;
  if(s.pos==s.limit&&!okio_read_recycle(v,self,&s))return false;
  if(!okio_read_state(v,self,&size,&head))return false;
 }
 return true;
}
static bool okio_read_count(struct dvm *v,int64_t size,int64_t count){
 if(count<0||count>size){dvm__throw(v,"java/lang/ArrayIndexOutOfBoundsException","size=%lld offset=0 byteCount=%lld",(long long)size,(long long)count);return false;}
 if(count>INT32_MAX){dvm__throw(v,"java/lang/IllegalArgumentException","byteCount > Integer.MAX_VALUE: %lld",(long long)count);return false;}return true;
}
static dvm_ref okio_read_bytes(struct dvm *v,dvm_ref self,int64_t size,int64_t count){
 if(!okio_read_count(v,size,count))return 0;dvm_ref bytes=dvm_new_array(v,'B',"B",(uint32_t)count);if(!bytes){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Okio byte array");return 0;}
 dvm_pin(v,bytes);bool ok=okio_read_consume(v,self,dvm_array_data(v,bytes),count);dvm_unpin(v,bytes);return ok?bytes:0;
}
/* Standard Charset decoder semantics: malformed input is replaced, not lost.
 * The other charsets retain the runtime's ordinary String constructor path. */
static dvm_ref okio_read_decode(struct dvm *v,const unsigned char *bytes,size_t count,dvm_ref charset){
 const char *name;size_t length;if(!charset_object_name(v,charset,&name,&length))return 0;
 if(get_bytes_charset_name(name,length)==GET_BYTES_UTF8){
  if(count>(SIZE_MAX-1)/3){dvm__throw(v,"java/lang/OutOfMemoryError","Okio UTF8 decode");return 0;}
  char *decoded=malloc(count*3+1);if(!decoded){dvm__throw(v,"java/lang/OutOfMemoryError","Okio UTF8 decode");return 0;}
  size_t len=java_utf8_decode(bytes,count,decoded);decoded[len]=0;
  if(len>UINT32_MAX){free(decoded);dvm__throw(v,"java/lang/OutOfMemoryError","Okio UTF8 String size");return 0;}
  dvm_ref result=dvm_new_string_n(v,decoded,len);free(decoded);if(!result&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Okio UTF8 String");return result;
 }
 enum get_bytes_charset kind=get_bytes_charset_name(name,length);
 bool le=kind==GET_BYTES_UTF16LE,utf16=kind==GET_BYTES_UTF16||kind==GET_BYTES_UTF16LE||kind==GET_BYTES_UTF16BE;
 bool utf32le=charset_alias(name,length,"utf-32le"),utf32be=charset_alias(name,length,"utf-32be"),utf32=charset_alias(name,length,"utf-32")||utf32le||utf32be;
 bool latin=charset_alias(name,length,"iso-8859-1")||charset_alias(name,length,"iso8859-1")||charset_alias(name,length,"latin1"),ascii=charset_alias(name,length,"us-ascii")||charset_alias(name,length,"ascii");
 if(!utf16&&!utf32&&!latin&&!ascii){dvm__throw(v,"java/lang/UnsupportedOperationException","String.<init> charset %.*s",length>200?200:(int)length,name);return 0;}
 uint16_t *units=count?malloc(count*sizeof *units):NULL;if(count&&!units){dvm__throw(v,"java/lang/OutOfMemoryError","Okio charset decode");return 0;}size_t used=0,at=0;
 if(utf16&&kind==GET_BYTES_UTF16&&count>=2){if(bytes[0]==0xfe&&bytes[1]==0xff){at=2;le=false;}else if(bytes[0]==0xff&&bytes[1]==0xfe){at=2;le=true;}}
 if(utf32){le=utf32le;if(!utf32le&&count>=4&&bytes[0]==0&&bytes[1]==0&&bytes[2]==0xfe&&bytes[3]==0xff){at=4;le=false;}else if(!utf32be&&count>=4&&bytes[0]==0xff&&bytes[1]==0xfe&&!bytes[2]&&!bytes[3]){at=4;le=true;}}
 while(at<count){
  if(latin||ascii){unsigned b=bytes[at++];units[used++]=(uint16_t)(ascii&&b>=128?0xfffd:b);continue;}
  if(utf32){if(count-at<4){units[used++]=0xfffd;break;}uint32_t cp=le?((uint32_t)bytes[at]|((uint32_t)bytes[at+1]<<8)|((uint32_t)bytes[at+2]<<16)|((uint32_t)bytes[at+3]<<24)):(((uint32_t)bytes[at]<<24)|((uint32_t)bytes[at+1]<<16)|((uint32_t)bytes[at+2]<<8)|bytes[at+3]);at+=4;
   if(cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))units[used++]=0xfffd;else if(cp<0x10000)units[used++]=(uint16_t)cp;else{cp-=0x10000;units[used++]=(uint16_t)(0xd800+(cp>>10));units[used++]=(uint16_t)(0xdc00+(cp&1023));}continue;}
  if(count-at<2){units[used++]=0xfffd;break;}uint16_t u=le?(uint16_t)(bytes[at]|(bytes[at+1]<<8)):(uint16_t)((bytes[at]<<8)|bytes[at+1]);at+=2;
  if(u>=0xd800&&u<=0xdbff){if(count-at<2){units[used++]=0xfffd;at=count;continue;}uint16_t b=le?(uint16_t)(bytes[at]|(bytes[at+1]<<8)):(uint16_t)((bytes[at]<<8)|bytes[at+1]);if(b>=0xdc00&&b<=0xdfff){units[used++]=u;units[used++]=b;at+=2;}else{units[used++]=0xfffd;at+=2;}/* JDK consumes malformed high+nonlow as a four-byte unit. */}
  else units[used++]=u>=0xdc00&&u<=0xdfff?0xfffd:u;
 }
 dvm_ref result=from_units(v,units,(int)used);free(units);if(!result&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Okio decoded String");return result;
}
static bool okio_read_string(struct dvm *v,dvm_ref self,int64_t size,int64_t count,dvm_ref charset,union dvm_value *out){
 /* Original checkOffsetAndCount precedes charset, and charset precedes MAX. */
 if(count<0||count>size){dvm__throw(v,"java/lang/ArrayIndexOutOfBoundsException","size=%lld offset=0 byteCount=%lld",(long long)size,(long long)count);return true;}
 if(!charset){dvm__throw(v,"java/lang/IllegalArgumentException","charset == null");return true;}
 if(!okio_read_count(v,size,count))return true;if(!count){out->l=dvm__intern(v,"");if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Okio empty String");return true;}
 int64_t ignored;dvm_ref head;if(!okio_read_state(v,self,&ignored,&head))return true;struct okio_read_segment s;if(!okio_read_segment(v,head,&s))return true;
 if(count<=s.limit-s.pos){out->l=okio_read_decode(v,(unsigned char*)dvm_array_data(v,s.data)+s.pos,(size_t)count,charset);if(!dvm_exception(v)&&out->l)okio_read_consume(v,self,NULL,count);return true;}
 dvm_ref bytes=okio_read_bytes(v,self,size,count);if(!bytes)return true;dvm_pin(v,bytes);out->l=okio_read_decode(v,dvm_array_data(v,bytes),(size_t)count,charset);dvm_unpin(v,bytes);return true;
}
bool string_runtime_okio_read_buffer(struct dvm *v,dvm_ref self,const char *name,const char *sig,const uint32_t *a,int n,union dvm_value *out){
 bool strings=!strcmp(name,"readString")&&((n==1&&!strcmp(sig,"(Ljava/nio/charset/Charset;)Ljava/lang/String;"))||(n==3&&!strcmp(sig,"(JLjava/nio/charset/Charset;)Ljava/lang/String;")));
 bool arrays=!strcmp(name,"readByteArray")&&((!n&&!strcmp(sig,"()[B"))||(n==2&&!strcmp(sig,"(J)[B")));
 bool read=!strcmp(name,"read")&&n==3&&!strcmp(sig,"([BII)I");
 bool elements=!strcmp(name,"indexOfElement")&&((n==1&&!strcmp(sig,"(Lokio/ByteString;)J"))||(n==3&&!strcmp(sig,"(Lokio/ByteString;J)J")));
 bool complete=!strcmp(name,"completeSegmentByteCount")&&!n&&!strcmp(sig,"()J"),point=!strcmp(name,"readUtf8CodePoint")&&!n&&!strcmp(sig,"()I");
 if(!strings&&!arrays&&!read&&!elements&&!complete&&!point)return false;int64_t size;dvm_ref head;if(!okio_read_state(v,self,&size,&head))return true;
 if(strings){int64_t count=n==1?size:(int64_t)((uint64_t)a[0]|((uint64_t)a[1]<<32));return okio_read_string(v,self,size,count,a[n-1],out);}
 if(arrays){int64_t count=n?((int64_t)((uint64_t)a[0]|((uint64_t)a[1]<<32))):size;out->l=okio_read_bytes(v,self,size,count);return true;}
 if(read){
  if(!a[0]){dvm__throw(v,"java/lang/NullPointerException","sink == null");return true;}struct dvm_object *dst=dvm__obj(v,a[0]);int32_t off=(int32_t)a[1],len=(int32_t)a[2];
  if(!dst||dst->kind!=DVM_OBJ_ARRAY||dst->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalArgumentException","sink is not byte[]");return true;}
  if(off<0||len<0||(uint32_t)off>dst->length||(uint32_t)len>dst->length-(uint32_t)off){dvm__throw(v,"java/lang/ArrayIndexOutOfBoundsException","sink offset/count");return true;}
  if(!head){out->i=-1;return true;}struct okio_read_segment s;if(!okio_read_segment(v,head,&s))return true;int take=len<s.limit-s.pos?len:s.limit-s.pos;
  if(!take&&s.pos==s.limit){if(okio_read_recycle(v,self,&s))out->i=0;return true;}
  if(okio_read_consume(v,self,(unsigned char*)dvm_array_data(v,a[0])+off,take))out->i=take;return true;
 }
 if(complete){out->j=size;if(!size)return true;struct okio_read_segment h,t;if(!okio_read_segment(v,head,&h)||!okio_read_segment(v,h.prev,&t))return true;union dvm_value owner={0};if(!dvm_get_field(v,t.ref,"owner","Z",&owner))return true;if(t.limit<8192&&owner.i)out->j-=t.limit-t.pos;return true;}
 if(elements){
  int64_t from=n==1?0:(int64_t)((uint64_t)a[1]|((uint64_t)a[2]<<32));out->j=-1;if(from<0){dvm__throw(v,"java/lang/IllegalArgumentException","fromIndex < 0");return true;}if(!head)return true;
  if(!a[0]){dvm__throw(v,"java/lang/NullPointerException","targetBytes");return true;}
  struct dvm_class *cls=dvm_object_class(v,a[0]);struct dvm_method *m=dvm_find_method(v,cls,"size","()I");union dvm_value q={0};if(!m){dvm__throw(v,"java/lang/NoSuchMethodError","ByteString.size");return true;}if(!dvm_call(v,m,a[0],NULL,0,&q)||dvm_exception(v))return true;
  unsigned char match[256]={0};
  if(q.i==2){m=dvm_find_method(v,cls,"getByte","(I)B");if(!m){dvm__throw(v,"java/lang/NoSuchMethodError","ByteString.getByte");return true;}for(int i=0;i<2;i++){union dvm_value index={.i=i};if(!dvm_call(v,m,a[0],&index,1,&q)||dvm_exception(v))return true;match[(unsigned char)q.i]=1;}}
  else{m=dvm_find_method(v,cls,"internalArray","()[B");if(!m){dvm__throw(v,"java/lang/NoSuchMethodError","ByteString.internalArray");return true;}if(!dvm_call(v,m,a[0],NULL,0,&q)||dvm_exception(v))return true;struct dvm_object *bytes=dvm__obj(v,q.l);if(!bytes||bytes->kind!=DVM_OBJ_ARRAY||bytes->elem_kind!='B'){dvm__throw(v,"java/lang/IllegalStateException","ByteString.internalArray");return true;}/* Snapshot scalar bounds before resolving the payload; the following loop makes no VM calls. */uint32_t length=bytes->length;const unsigned char *data=length?dvm_array_data(v,q.l):NULL;for(uint32_t i=0;i<length;i++)match[data[i]]=1;}
  if(from>=size)return true;dvm_ref ref=head;int64_t base=0;do{struct okio_read_segment s;if(!okio_read_segment(v,ref,&s))return true;int64_t end=base+s.limit-s.pos;if(from<end){int begin=s.pos+(int)(from>base?from-base:0);unsigned char *data=dvm_array_data(v,s.data);for(int i=begin;i<s.limit;i++)if(match[data[i]]){out->j=base+i-s.pos;return true;}}base=end;ref=s.next;}while(ref&&ref!=head&&base<size);return true;
 }
 if(point){
  if(!size){dvm__throw(v,"java/io/EOFException","");return true;}unsigned char bytes[4];unsigned have=0;dvm_ref ref=head;
  do{struct okio_read_segment s;if(!okio_read_segment(v,ref,&s))return true;unsigned char *data=dvm_array_data(v,s.data);for(int i=s.pos;i<s.limit&&have<4;i++)bytes[have++]=data[i];ref=s.next;}while(have<4&&ref&&ref!=head);
  if(!have){dvm__throw(v,"java/lang/IllegalStateException","Okio size/segments mismatch");return true;}unsigned b=bytes[0],width;uint32_t cp,min;
  if(!(b&0x80)){width=1;cp=b;min=0;}else if((b&0xe0)==0xc0){width=2;cp=b&31;min=0x80;}else if((b&0xf0)==0xe0){width=3;cp=b&15;min=0x800;}else if((b&0xf8)==0xf0){width=4;cp=b&7;min=0x10000;}else{okio_read_consume(v,self,NULL,1);out->i=0xfffd;return true;}
  if(size<(int64_t)width){dvm__throw(v,"java/io/EOFException","size < %u: %lld (to read code point prefixed 0x%x)",width,(long long)size,b);return true;}
  if(have<width){dvm__throw(v,"java/lang/IllegalStateException","Okio size/segments mismatch");return true;}
  for(unsigned i=1;i<width;i++){if((bytes[i]&0xc0)!=0x80){okio_read_consume(v,self,NULL,i);out->i=0xfffd;return true;}cp=(cp<<6)|(bytes[i]&63);}
  okio_read_consume(v,self,NULL,width);out->i=cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||cp<min?0xfffd:(int32_t)cp;return true;
 }
 return false;
}
