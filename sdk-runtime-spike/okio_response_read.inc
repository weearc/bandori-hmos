/* Included by string_runtime.c. Generic original OkHttp/Okio semantics;
 * all source/body/media calls resolve against the actual guest receiver. */
static bool response_read_virtual(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 if(!self){dvm__throw(v,"java/lang/NullPointerException","response source receiver");return false;}
 struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,self),name,sig);
 if(!m){dvm__throw(v,"java/lang/NoSuchMethodError","response source %s%s",name,sig);return false;}
 return dvm_call(v,m,self,a,n,out)&&!dvm_exception(v);
}
static bool response_read_close_quietly(struct dvm *v,dvm_ref source){
 if(!source)return true;
 union dvm_value q={0};
 if(response_read_virtual(v,source,"close","()V",NULL,0,&q))return true;
 struct dvm_class *error=dvm_object_class(v,dvm_exception(v));
 struct dvm_class *runtime=dvm_find_class_or_external(v,"java/lang/RuntimeException");
 struct dvm_class *exception=dvm_find_class_or_external(v,"java/lang/Exception");
 /* closeQuietly rethrows RuntimeException and Errors, swallowing checked
  * Exception. A close failure therefore overrides a prior read failure. */
 if(error&&dvm__class_assignable(v,error,exception)&&!dvm__class_assignable(v,error,runtime)){
  dvm_clear_exception(v);return true;
 }
 return false;
}
bool string_runtime_response_read(struct dvm *v,const char *owner,dvm_ref self,const char *name,const char *sig,const uint32_t *slots,int n,union dvm_value *out){
 if(!strcmp(owner,"okio/RealBufferedSource")&&!strcmp(name,"readString")&&!strcmp(sig,"(Ljava/nio/charset/Charset;)Ljava/lang/String;")&&n==1){
  if(!slots[0]){dvm__throw(v,"java/lang/IllegalArgumentException","charset == null");return true;}
  union dvm_value buffer={0},source={0},q={0},arg={0};
  if(!dvm_get_field(v,self,"buffer","Lokio/Buffer;",&buffer)||!dvm_get_field(v,self,"source","Lokio/Source;",&source))return true;
  arg.l=source.l;
  if(!response_read_virtual(v,buffer.l,"writeAll","(Lokio/Source;)J",&arg,1,&q))return true;
  arg.l=slots[0];response_read_virtual(v,buffer.l,"readString","(Ljava/nio/charset/Charset;)Ljava/lang/String;",&arg,1,out);return true;
 }
 if(strcmp(owner,"okhttp3/ResponseBody")||strcmp(name,"string")||strcmp(sig,"()Ljava/lang/String;")||n)return false;
 union dvm_value source={0},media={0},charset={0},q={0},a[2]={{0}};
 if(!response_read_virtual(v,self,"source","()Lokio/BufferedSource;",NULL,0,&source))return true;
 /* The original try/finally begins after source(), before charset(). */
 if(!response_read_virtual(v,self,"contentType","()Lokhttp3/MediaType;",NULL,0,&media))goto close;
 struct dvm_class *util=dvm_find_class(v,"okhttp3/internal/Util");
 if(!util||!dvm_init_class(v,util))goto close;
 if(!dvm_get_static(v,util,"UTF_8","Ljava/nio/charset/Charset;",&charset))goto close;
 if(media.l){a[0]=charset;if(!response_read_virtual(v,media.l,"charset","(Ljava/nio/charset/Charset;)Ljava/nio/charset/Charset;",a,1,&charset))goto close;}
 /* Match shipped Util.bomAwareCharset order exactly, including UTF16-LE
  * before UTF32-LE. rangeEquals may request bytes from a real stream. */
 static const char *boms[]={"UTF_8_BOM","UTF_16_BE_BOM","UTF_16_LE_BOM","UTF_32_BE_BOM","UTF_32_LE_BOM"};
 static const char *encs[]={"UTF_8","UTF_16_BE","UTF_16_LE","UTF_32_BE","UTF_32_LE"};
 static const int sizes[]={3,2,2,4,4};
 for(int i=0;i<5;i++){
  union dvm_value bom={0};if(!dvm_get_static(v,util,boms[i],"Lokio/ByteString;",&bom))goto close;
  a[0].j=0;a[1]=bom;
  if(!response_read_virtual(v,source.l,"rangeEquals","(JLokio/ByteString;)Z",a,2,&q))goto close;
  if(q.i){a[0].j=sizes[i];if(!response_read_virtual(v,source.l,"skip","(J)V",a,1,&q))goto close;
   if(!dvm_get_static(v,util,encs[i],"Ljava/nio/charset/Charset;",&charset))goto close;break;}
 }
 a[0]=charset;response_read_virtual(v,source.l,"readString","(Ljava/nio/charset/Charset;)Ljava/lang/String;",a,1,out);
 close: {
  dvm_ref prior=dvm_exception(v);
  struct dvm_method *prior_trace[sizeof v->exc_trace/sizeof v->exc_trace[0]];
  int prior_count=v->nexc_trace;uint32_t prior_pc=v->exc_pc;
  if(prior){memcpy(prior_trace,v->exc_trace,sizeof prior_trace);dvm_pin(v,prior);}dvm_clear_exception(v);
  bool closed=response_read_close_quietly(v,source.l);
  if(closed&&prior){v->exception=prior;v->nexc_trace=prior_count;v->exc_pc=prior_pc;memcpy(v->exc_trace,prior_trace,sizeof prior_trace);}
  if(prior)dvm_unpin(v,prior);
 }
 return true;
}
