/* Exact declarations for implemented File APIs; never add File methods to Object. */
static bool file_install_declarations(struct dvm *v){
 static const char *rows[][2]={
 {"<init>","(Ljava/lang/String;)V"},{"<init>","(Ljava/io/File;Ljava/lang/String;)V"},{"<init>","(Ljava/lang/String;Ljava/lang/String;)V"},
 {"getPath","()Ljava/lang/String;"},{"getAbsolutePath","()Ljava/lang/String;"},{"getCanonicalPath","()Ljava/lang/String;"},
 {"getAbsoluteFile","()Ljava/io/File;"},{"getCanonicalFile","()Ljava/io/File;"},
 {"getName","()Ljava/lang/String;"},{"getParent","()Ljava/lang/String;"},{"getParentFile","()Ljava/io/File;"},
 {"exists","()Z"},{"isFile","()Z"},{"isDirectory","()Z"},{"length","()J"},{"lastModified","()J"},
 {"canRead","()Z"},{"canWrite","()Z"},{"canExecute","()Z"},
 {"list","()[Ljava/lang/String;"},{"listFiles","()[Ljava/io/File;"},
 {"mkdir","()Z"},{"mkdirs","()Z"},{"createNewFile","()Z"},
 {"createTempFile","(Ljava/lang/String;Ljava/lang/String;Ljava/io/File;)Ljava/io/File;"},
 {"delete","()Z"},{"deleteOnExit","()V"},{"renameTo","(Ljava/io/File;)Z"},
 {"setLastModified","(J)Z"},{"setReadOnly","()Z"},{"toURI","()Ljava/net/URI;"},
 {"listFiles","(Ljava/io/FileFilter;)[Ljava/io/File;"},{"listFiles","(Ljava/io/FilenameFilter;)[Ljava/io/File;"},
 {"toString","()Ljava/lang/String;"},{"equals","(Ljava/lang/Object;)Z"},{"hashCode","()I"}
 };
 struct dvm_class *c=dvm_find_class_or_external(v,"java/io/File");if(!c)return false;
 if(c->nmethods)return true;
 size_t n=sizeof rows/sizeof rows[0];struct dvm_method *ms=calloc(n,sizeof *ms);
 if(!ms){dvm__throw(v,"java/lang/OutOfMemoryError","File declarations");return false;}
 for(size_t i=0;i<n;i++){ms[i].cls=c;ms[i].name=rows[i][0];ms[i].sig=strdup(rows[i][1]);
  if(!ms[i].sig){for(size_t j=0;j<i;j++)free(ms[j].sig);free(ms);return false;}
  ms[i].access=!strcmp(rows[i][0],"<init>")?0x10001:!strcmp(rows[i][0],"createTempFile")?9:1;ms[i].arg_count=dvm_sig_arg_count(ms[i].sig);ms[i].arg_slots=dvm_sig_arg_slots(ms[i].sig);ms[i].ret_kind=dvm_sig_return_kind(ms[i].sig);}
 c->methods=ms;c->nmethods=(int)n;return true;
}
static bool file_absolute(const char *p,char out[PATH_MAX]){
 if(*p=='/')return snprintf(out,PATH_MAX,"%s",p)<PATH_MAX;
 char cwd[PATH_MAX];if(!getcwd(cwd,sizeof cwd))return false;
 if(!*p)return snprintf(out,PATH_MAX,"%s",cwd)<PATH_MAX;
 return snprintf(out,PATH_MAX,"%s%s%s",cwd,!strcmp(cwd,"/")?"":"/",p)<PATH_MAX;
}
/* Resolve symlinks before processing a following '..'; missing suffixes are
 * allowed, just as File.getCanonicalPath is allowed for nonexistent files. */
static bool file_canonical(const char *p,char out[PATH_MAX]){
 char absolute[PATH_MAX],resolved[PATH_MAX];if(!file_absolute(p,absolute))return false;
 strcpy(out,"/");char *save=NULL;
 for(char *part=strtok_r(absolute,"/",&save);part;part=strtok_r(NULL,"/",&save)){
  if(!strcmp(part,"."))continue;
  if(!strcmp(part,"..")){char *last=strrchr(out,'/');if(last&&last!=out)*last=0;else strcpy(out,"/");continue;}
  size_t n=strlen(out),k=strlen(part);if(n+k+2>PATH_MAX)return false;
  if(n>1)out[n++]='/';memcpy(out+n,part,k+1);
  if(realpath(out,resolved))strcpy(out,resolved);
  else if(errno!=ENOENT)return false;
 }
 return true;
}
static bool file_read_extras(struct dvm *v,const char *m,const char *sig,dvm_ref self,struct dvm_object *o,const union dvm_value *a,union dvm_value *out){
 const char *p=o->data;char path[PATH_MAX];
 if(IS("getCanonicalFile","()Ljava/io/File;")||IS("getAbsoluteFile","()Ljava/io/File;")||IS("getAbsolutePath","()Ljava/lang/String;")){
  if(!(IS("getCanonicalFile","()Ljava/io/File;")?file_canonical(p,path):file_absolute(p,path)))return err(v,"java/io/IOException");
  out->l=IS("getAbsolutePath","()Ljava/lang/String;")?dvm_new_string(v,path):make(v,path);return out->l?true:err(v,"java/lang/OutOfMemoryError");
 }
 if(IS("getName","()Ljava/lang/String;")){const char *q=strrchr(p,'/');out->l=dvm_new_string(v,q?q+1:p);return true;}
 if(IS("toString","()Ljava/lang/String;")){out->l=dvm_new_string(v,p);return true;}
 if(IS("canWrite","()Z")||IS("canExecute","()Z")){out->i=access(p,IS("canWrite","()Z")?W_OK:X_OK)==0;return true;}
 if(IS("lastModified","()J")){struct stat st;out->j=stat(p,&st)?0:(int64_t)st.st_mtim.tv_sec*1000+st.st_mtim.tv_nsec/1000000;return true;}
 if(IS("equals","(Ljava/lang/Object;)Z")){struct dvm_object *other=dvm__obj(v,a[0].l);bool file=false;for(struct dvm_class *k=other?other->cls:NULL;k;k=k->super)if(!strcmp(k->name,"java/io/File"))file=true;out->i=file&&other->data_destroy==release&&!strcmp(p,other->data);return true;}
 if(IS("hashCode","()I")){uint32_t hash=0;/* String hash must use Java UTF-16 units. */
  dvm_ref text=dvm_new_string(v,p);if(!text)return err(v,"java/lang/OutOfMemoryError");
  extern bool string_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
  union dvm_value h={0};if(!string_runtime_call(v,"java/lang/String","hashCode","()I",text,NULL,0,&h))return false;hash=(uint32_t)h.i;out->i=(int32_t)(hash^1234321u);return true;}
 if(IS("list","()[Ljava/lang/String;")||IS("listFiles","()[Ljava/io/File;")){
  struct dirent **names=NULL;int count=scandir(p,&names,NULL,alphasort);if(count<0){out->l=0;return true;}
  int total=0;for(int i=0;i<count;i++)if(strcmp(names[i]->d_name,".")&&strcmp(names[i]->d_name,".."))total++;
  bool files=IS("listFiles","()[Ljava/io/File;");dvm_ref result=dvm_new_array(v,'L',files?"Ljava/io/File;":"Ljava/lang/String;",total);
  if(result)dvm_pin(v,result);int j=0;
  for(int i=0;i<count;i++){const char *name=names[i]->d_name;
   if(result&&strcmp(name,".")&&strcmp(name,"..")&&!dvm_exception(v)){
    dvm_ref child=0;if(files){if(snprintf(path,sizeof path,"%s%s%s",p,*p&&p[strlen(p)-1]!='/'?"/":"",name)<PATH_MAX)child=make(v,path);else err(v,"java/io/IOException");}else child=dvm_new_string(v,name);
    if(child){((dvm_ref*)dvm_array_data(v,result))[j++]=child;}
   }free(names[i]);
  }free(names);if(result)dvm_unpin(v,result);out->l=result;return true;
 }
 (void)self;return false;
}
static char *file_exit_paths[256];static unsigned file_exit_count;static bool file_exit_registered;
static void file_exit_cleanup(void){while(file_exit_count){char *p=file_exit_paths[--file_exit_count];remove(p);free(p);}}
static bool file_mutable(const char *p){char resolved[PATH_MAX];return file_canonical(p,resolved)&&allowed(resolved)&&safe(p)&&allowed(p);}
static bool file_other_extras(struct dvm *v,const char *m,const char *sig,dvm_ref self,struct dvm_object *o,const union dvm_value *a,union dvm_value *out){
 const char *p=o&&o->data_destroy==release?o->data:NULL;
 if(IS("createTempFile","(Ljava/lang/String;Ljava/lang/String;Ljava/io/File;)Ljava/io/File;")){
  const char *prefix=dvm_string_utf8(v,a[0].l),*suffix=a[1].l?dvm_string_utf8(v,a[1].l):".tmp";
  if(!prefix||!suffix)return err(v,"java/lang/NullPointerException");
  extern bool string_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
  union dvm_value prefix_length={0};if(!string_runtime_call(v,"java/lang/String","length","()I",a[0].l,NULL,0,&prefix_length))return false;
  if(prefix_length.i<3||strchr(prefix,'/')||strchr(suffix,'/'))return err(v,"java/lang/IllegalArgumentException");
  struct dvm_object *dir=dvm__obj(v,a[2].l);const char *base=a[2].l?(dir&&dir->data_destroy==release?dir->data:NULL):cache_root;
  if(!base)return err(v,"java/lang/IllegalArgumentException");char path[PATH_MAX];
  if(snprintf(path,sizeof path,"%s/%sXXXXXX%s",base,prefix,suffix)>=PATH_MAX||!file_mutable(path))return err(v,"java/io/IOException");
  int fd=mkstemps(path,(int)strlen(suffix));if(fd<0)return err(v,"java/io/IOException");close(fd);out->l=make(v,path);if(!out->l){unlink(path);return err(v,"java/lang/OutOfMemoryError");}return true;
 }
 if(!p)return false;
 if(IS("listFiles","(Ljava/io/FileFilter;)[Ljava/io/File;")||IS("listFiles","(Ljava/io/FilenameFilter;)[Ljava/io/File;")){
  union dvm_value all={0};if(!file_read_extras(v,"listFiles","()[Ljava/io/File;",self,o,NULL,&all)||!all.l){out->l=0;return true;}
  if(!a[0].l){out->l=all.l;return true;}dvm_pin(v,all.l);
  int count=dvm_array_length(v,all.l),accepted=0;dvm_ref *kept=calloc(count?count:1,sizeof *kept);
  if(!kept){dvm_unpin(v,all.l);return err(v,"java/lang/OutOfMemoryError");}
  bool names=IS("listFiles","(Ljava/io/FilenameFilter;)[Ljava/io/File;");
  const char *filter_sig=names?"(Ljava/io/File;Ljava/lang/String;)Z":"(Ljava/io/File;)Z";
  struct dvm_method *method=dvm_find_method(v,dvm_object_class(v,a[0].l),"accept",filter_sig);
  if(!method)dvm__throw(v,"java/lang/AbstractMethodError","File filter accept%s",filter_sig);
  for(int i=0;i<count&&!dvm_exception(v);i++){
   dvm_ref child=((dvm_ref*)dvm_array_data(v,all.l))[i];union dvm_value args[2]={{.l=names?self:child},{0}},result={0};
   if(names){struct dvm_object *co=dvm__obj(v,child);const char *name=strrchr(co->data,'/');args[1].l=dvm_new_string(v,name?name+1:co->data);}
   if(!dvm_call(v,method,a[0].l,args,names?2:1,&result))break;if(result.i)kept[accepted++]=child;
  }
  out->l=0;if(!dvm_exception(v)){out->l=dvm_new_array(v,'L',"Ljava/io/File;",accepted);if(out->l)memcpy(dvm_array_data(v,out->l),kept,(size_t)accepted*sizeof *kept);}
  free(kept);dvm_unpin(v,all.l);return true;
 }
 if(IS("toURI","()Ljava/net/URI;")){
  char path[PATH_MAX],text[PATH_MAX*3+16];if(!file_absolute(p,path))return err(v,"java/io/IOException");
  size_t used=5;memcpy(text,"file:",5);const char *hex="0123456789ABCDEF";
  for(const unsigned char *q=(const unsigned char*)path;*q;q++){unsigned char ch=*q;if((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||strchr("/-._~",ch))text[used++]=(char)ch;else{text[used++]='%';text[used++]=hex[ch>>4];text[used++]=hex[ch&15];}}
  struct stat st;if(!stat(path,&st)&&S_ISDIR(st.st_mode)&&text[used-1]!='/')text[used++]='/';text[used]=0;
  dvm_ref uri=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/URI"));if(!uri)return err(v,"java/lang/OutOfMemoryError");dvm_pin(v,uri);
  union dvm_value arg={.l=dvm_new_string(v,text)},ignored={0};extern bool uri_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
  bool handled=arg.l&&uri_runtime_call(v,"java/net/URI","<init>","(Ljava/lang/String;)V",uri,&arg,1,&ignored);dvm_unpin(v,uri);out->l=handled&&!dvm_exception(v)?uri:0;return true;
 }
 if(IS("delete","()Z")){out->i=file_mutable(p)&&remove(p)==0;return true;}
 if(IS("deleteOnExit","()V")){
  if(!file_mutable(p)){dvm__throw(v,"java/lang/SecurityException","deleteOnExit outside application storage");return true;}
  for(unsigned i=0;i<file_exit_count;i++)if(!strcmp(file_exit_paths[i],p))return true;
  if(file_exit_count==256)return err(v,"java/lang/OutOfMemoryError");
  if(!file_exit_registered){if(atexit(file_exit_cleanup))return err(v,"java/lang/OutOfMemoryError");file_exit_registered=true;}
  char *copy=strdup(p);if(!copy)return err(v,"java/lang/OutOfMemoryError");file_exit_paths[file_exit_count++]=copy;return true;
 }
 if(IS("renameTo","(Ljava/io/File;)Z")){struct dvm_object *dest=dvm__obj(v,a[0].l);if(!a[0].l)return err(v,"java/lang/NullPointerException");out->i=dest&&dest->data_destroy==release&&file_mutable(p)&&file_mutable(dest->data)&&rename(p,dest->data)==0;return true;}
 if(IS("setLastModified","(J)Z")){if(a[0].j<0)return err(v,"java/lang/IllegalArgumentException");struct timespec ts[2]={{.tv_nsec=UTIME_OMIT},{.tv_sec=a[0].j/1000,.tv_nsec=(a[0].j%1000)*1000000}};out->i=file_mutable(p)&&utimensat(AT_FDCWD,p,ts,0)==0;return true;}
 if(IS("setReadOnly","()Z")){struct stat st;out->i=file_mutable(p)&&stat(p,&st)==0&&chmod(p,st.st_mode&~(S_IWUSR|S_IWGRP|S_IWOTH))==0;return true;}
 return false;
}
