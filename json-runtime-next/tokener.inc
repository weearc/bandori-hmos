/* Basic Android JSONTokener cursor: UTF-16 source, comments, strings, scalars,
 * and strict JSON object/array values. No DEX parser forwarding. */
extern bool string_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
struct tokener_state {uint16_t *units;uint32_t size,pos;};
static void tokener_free(void *ptr){struct tokener_state *t=ptr;free(t->units);free(t);}
static char *tokener_utf8(const uint16_t *u,uint32_t n){
 char *s=malloc((size_t)n*3+1);if(!s)return NULL;size_t j=0;
 for(uint32_t i=0;i<n;i++){uint32_t c=u[i];
  if(c>=0xd800&&c<=0xdbff&&i+1<n&&u[i+1]>=0xdc00&&u[i+1]<=0xdfff){c=0x10000+((c-0xd800)<<10)+u[++i]-0xdc00;s[j++]=0xf0|(c>>18);s[j++]=0x80|((c>>12)&63);s[j++]=0x80|((c>>6)&63);s[j++]=0x80|(c&63);}
  else if(c&&c<128)s[j++]=(char)c;else if(c<2048){s[j++]=0xc0|(c>>6);s[j++]=0x80|(c&63);}else{s[j++]=0xe0|(c>>12);s[j++]=0x80|((c>>6)&63);s[j++]=0x80|(c&63);}}
 s[j]=0;return s;
}
static int tokener_clean(struct dvm *v,struct tokener_state *t){
 while(t->pos<t->size){uint16_t c=t->units[t->pos++];
  if(c==' '||c=='\t'||c=='\n'||c=='\r')continue;
  if(c=='#'){while(t->pos<t->size&&t->units[t->pos]!='\n'&&t->units[t->pos]!='\r')t->pos++;continue;}
  if(c=='/'&&t->pos<t->size){uint16_t next=t->units[t->pos];
   if(next=='/'){t->pos++;while(t->pos<t->size&&t->units[t->pos]!='\n'&&t->units[t->pos]!='\r')t->pos++;continue;}
   if(next=='*'){t->pos++;bool closed=false;while(t->pos+1<t->size){if(t->units[t->pos]=='*'&&t->units[t->pos+1]=='/'){t->pos+=2;closed=true;break;}t->pos++;}if(!closed){fail(v);return 0;}continue;}}
  return c;
 }return 0;
}
static dvm_ref tokener_null(struct dvm *v){
 struct dvm_class *c=dvm_find_class_or_external(v,"org/json/JSONObject");
 if(!c->sfields){c->sfields=calloc(1,sizeof *c->sfields);c->sslots=calloc(1,sizeof *c->sslots);if(!c->sfields||!c->sslots)return 0;
  c->sfields[0]=(struct dvm_field){.cls=c,.name="NULL",.type="Ljava/lang/Object;",.kind='L',.width=1,.access=0x19,.slot=0};c->nsfields=c->nsslots=1;}
 for(int i=0;i<c->nsfields;i++)if(!strcmp(c->sfields[i].name,"NULL")){unsigned slot=c->sfields[i].slot;if(!c->sslots[slot].l)c->sslots[slot].l=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Object"));return c->sslots[slot].l;}
 return 0;
}
static dvm_ref tokener_value(struct dvm *v,struct tokener_state *t){
 int first=tokener_clean(v,t);if(!first||dvm_exception(v)){if(!dvm_exception(v))fail(v);return 0;}
 uint32_t start=t->pos-1,end=0;
 if(first=='"'||first=='\''){
  uint16_t *decoded=malloc((size_t)(t->size-t->pos+1)*sizeof *decoded);if(!decoded){dvm__throw(v,"java/lang/OutOfMemoryError","JSONTokener string");return 0;}
  uint32_t count=0;bool closed=false;
  while(t->pos<t->size){uint16_t c=t->units[t->pos++];if(c==first){closed=true;break;}
   if(c=='\\'){
    if(t->pos==t->size)break;c=t->units[t->pos++];
    if(c=='u'){if(t->size-t->pos<4)break;unsigned x=0;bool valid=true;for(int i=0;i<4;i++){uint16_t h=t->units[t->pos++];int n=h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h>='A'&&h<='F'?h-'A'+10:-1;if(n<0){valid=false;break;}x=x*16+(unsigned)n;}if(!valid)break;c=(uint16_t)x;}
    else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';
   }decoded[count++]=c;
  }
  if(!closed){free(decoded);fail(v);return 0;}char *text=tokener_utf8(decoded,count);free(decoded);if(!text)return 0;dvm_ref result=dvm_new_string(v,text);free(text);return result;
 }
 if(first=='{'||first=='['){
  uint16_t stack[128];unsigned depth=1;stack[0]=first;int quote=0;bool escape=false;
  while(t->pos<t->size&&depth){uint16_t c=t->units[t->pos++];
   if(quote){if(escape)escape=false;else if(c=='\\')escape=true;else if(c==quote)quote=0;continue;}
   if(c=='"'){quote=c;continue;}
   if(c=='{'||c=='['){if(depth==128){fail(v);return 0;}stack[depth++]=c;}
   else if(c=='}'||c==']'){if((c=='}'&&stack[depth-1]!='{')||(c==']'&&stack[depth-1]!='[')){fail(v);return 0;}depth--;}
  }
  if(depth){fail(v);return 0;}end=t->pos;char *doc=tokener_utf8(t->units+start,end-start);if(!doc)return 0;
  sqlite3 *db=NULL;sqlite3_stmt *q=NULL;bool valid=sqlite3_open(":memory:",&db)==SQLITE_OK;
  if(valid){q=query(db,"SELECT json_valid(?1)",doc,NULL);valid=q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_int(q,0);}
  sqlite3_finalize(q);sqlite3_close(db);if(!valid){free(doc);fail(v);return 0;}
  dvm_ref result=dvm_new_object(v,dvm_find_class_or_external(v,first=='{'?"org/json/JSONObject":"org/json/JSONArray"));
  bool ok=result&&attach(v,result,doc);free(doc);return ok?result:0;
 }
 while(t->pos<t->size){uint16_t c=t->units[t->pos];if(c<=32||c==','||c==':'||c==';'||c==']'||c=='}'||c=='/'||c=='#'||c=='='||c=='\\')break;t->pos++;}
 end=t->pos;char *literal=tokener_utf8(t->units+start,end-start);if(!literal)return 0;
 dvm_ref result=0;union dvm_value arg={0},out={0};
 if(!strcasecmp(literal,"null"))result=tokener_null(v);
 else if(!strcasecmp(literal,"true")||!strcasecmp(literal,"false")){arg.i=!strcasecmp(literal,"true");java_basics_call(v,"java/lang/Boolean","valueOf","(Z)Ljava/lang/Boolean;",0,&arg,1,&out);result=out.l;}
 else {char *tail=NULL;errno=0;long long integer=strtoll(literal,&tail,10);
  if(*literal&&!*tail&&errno!=ERANGE){if(integer>=INT32_MIN&&integer<=INT32_MAX){arg.i=(int32_t)integer;java_basics_call(v,"java/lang/Integer","valueOf","(I)Ljava/lang/Integer;",0,&arg,1,&out);}else{arg.j=integer;java_basics_call(v,"java/lang/Long","valueOf","(J)Ljava/lang/Long;",0,&arg,1,&out);}result=out.l;}
  else {errno=0;double real=strtod(literal,&tail);if(*literal&&!*tail&&isfinite(real)){arg.d=real;java_basics_call(v,"java/lang/Double","valueOf","(D)Ljava/lang/Double;",0,&arg,1,&out);result=out.l;}else result=dvm_new_string(v,literal);}}
 free(literal);return result;
}
static bool tokener_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","JSONTokener receiver");return true;}
 if(!strcmp(m,"<init>")&&!strcmp(sig,"(Ljava/lang/String;)V")&&n==1){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","JSONTokener input");return true;}if(o->data){dvm__throw(v,"java/lang/IllegalStateException","JSONTokener already constructed");return true;}
  union dvm_value size={0},unit={0},index={0};if(!string_runtime_call(v,"java/lang/String","length","()I",a[0].l,NULL,0,&size)||dvm_exception(v))return true;
  struct tokener_state *t=calloc(1,sizeof *t);if(!t){dvm__throw(v,"java/lang/OutOfMemoryError","JSONTokener");return true;}t->size=(uint32_t)size.i;t->units=malloc(((size_t)t->size+1)*sizeof *t->units);if(!t->units){free(t);dvm__throw(v,"java/lang/OutOfMemoryError","JSONTokener input");return true;}
  for(uint32_t i=0;i<t->size;i++){index.i=(int32_t)i;if(!string_runtime_call(v,"java/lang/String","charAt","(I)C",a[0].l,&index,1,&unit)||dvm_exception(v)){tokener_free(t);return true;}t->units[i]=(uint16_t)unit.i;}
  if(t->size&&t->units[0]==0xfeff)t->pos=1;o->data=t;o->data_destroy=tokener_free;return true;
 }
 struct tokener_state *t=o->data_destroy==tokener_free?o->data:NULL;if(!t)return false;
 if(!strcmp(m,"nextClean")&&!strcmp(sig,"()C")&&n==0){out->i=tokener_clean(v,t);return true;}
 if(!strcmp(m,"nextValue")&&!strcmp(sig,"()Ljava/lang/Object;")&&n==0){out->l=tokener_value(v,t);if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","JSONTokener value");return true;}
 return false;
}
