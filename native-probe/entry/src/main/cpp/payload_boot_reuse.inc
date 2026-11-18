/* Called under payloadCacheLock. Manifest digest is generated from APK bytes.
 * Always hash the destination; never trust a timestamp-only cache hit. */
static std::string PayloadBootSidecar(const std::string& path){
 int fd=open((path+".sha256").c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 if(fd<0)return "";char bytes[65];ssize_t n=read(fd,bytes,sizeof bytes);close(fd);
 return n==64?std::string(bytes,64):std::string();
}
static void PayloadBootWriteSidecar(const std::string& dest,const std::string& digest){
 std::string side=dest+".sha256",part=side+".part";
 int fd=open(part.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0644);
 if(fd<0)PayloadFail("E-EXTRACT-WRITE","boot sidecar open");
 try{if(fchmod(fd,0644))PayloadFail("E-EXTRACT-WRITE","boot sidecar mode");PayloadWrite(fd,(const unsigned char*)digest.data(),digest.size());if(fsync(fd))PayloadFail("E-EXTRACT-WRITE","boot sidecar fsync");close(fd);fd=-1;if(rename(part.c_str(),side.c_str()))PayloadFail("E-EXTRACT-WRITE","boot sidecar rename");}
 catch(...){if(fd>=0)close(fd);unlink(part.c_str());throw;}
}
static void PayloadPublishBootVerified(const std::string& source,const std::string& dest,bool manifest,uint64_t size,const std::string& digest){
#if PAYLOAD_BOOT_REUSE
 struct stat st{};
 if(manifest&&digest.size()==64&&!lstat(dest.c_str(),&st)&&S_ISREG(st.st_mode)&&(uint64_t)st.st_size==size&&PayloadBootSidecar(dest)==digest&&PayloadFileSha(dest)==digest){
  PayloadLog("boot_copy_skipped verified_sha256=1 file="+dest);return;
 }
#else
 (void)manifest;(void)size;
#endif
 /* A miss never aborts bootstrap: use the unchanged atomic copy. Only its
  * original IO failures propagate; no corrupt partial destination is exposed. */
 PayloadPublishBootOriginal(source,dest);
 if(!digest.empty()){
  if(PayloadFileSha(dest)!=digest)PayloadFail("E-EXTRACT-BADSIZE","boot copy digest");
  /* Sidecar is an optimization, not a new availability requirement. */
  try{PayloadBootWriteSidecar(dest,digest);}catch(const std::exception& e){PayloadLog(std::string("boot_sidecar_unavailable copy_ok=1 reason=")+e.what());}
 }
 PayloadLog("boot_copy_completed file="+dest);
}
