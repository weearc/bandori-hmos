/* Included after ZIP primitives. All cache mutation is serialized; bootstrap
 * prepares DEX, assets are inflated only when AssetManager requests them. */
static std::mutex payloadCacheLock;
static std::string payloadCacheRoot,payloadKey,payloadVersion="unknown";
static uint32_t payloadVersionCode=0;
static std::unique_ptr<FILE,decltype(&fclose)> payloadApk(nullptr,fclose);
static std::map<std::string,PayloadZipEntry> payloadIndex;
static std::map<std::string,std::pair<time_t,long>> payloadVerified;
static std::map<std::string,bool> payloadRetryLogged;
static std::atomic<bool> payloadOpenErrorLogged{false};
static std::string PayloadFingerprint(int fd,uint64_t offset,uint64_t size){
 PayloadSha256 h;std::vector<unsigned char>b(65536);
 auto add=[&](uint64_t at,uint64_t count){while(count){size_t n=(size_t)std::min<uint64_t>(count,b.size());ssize_t k=pread(fd,b.data(),n,offset+at);if(k<0&&errno==EINTR)continue;if(k<=0)PayloadFail("E-EXTRACT-OPEN","fingerprint short read");h.add(b.data(),k);at+=k;count-=k;}};
 add(0,std::min<uint64_t>(size,1048576));add(size>65536?size-65536:0,std::min<uint64_t>(size,65536));
 unsigned char length[8];for(int i=0;i<8;i++)length[i]=(unsigned char)(size>>(8*i));h.add(length,8);return h.finish();
}
static std::string PayloadFileSha(const std::string& path){
 int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return "";
 PayloadSha256 h;std::vector<unsigned char>b(65536);ssize_t n;while((n=read(fd,b.data(),b.size()))>0)h.add(b.data(),n);close(fd);return n==0?h.finish():"";
}
static std::string PayloadEnsure(const std::string& name){
 if(!PayloadSafePath(name))PayloadFail("E-EXTRACT-OPEN","unsafe resource path");
 auto it=payloadIndex.find(name);if(it==payloadIndex.end())return "";
 const auto& z=it->second;std::string path=payloadCacheRoot+"/"+name;
 auto valid=[&](){struct stat st{};if(lstat(path.c_str(),&st)||!S_ISREG(st.st_mode)||(uint64_t)st.st_size!=z.size)return false;
  auto stamp=std::make_pair(st.st_mtim.tv_sec,st.st_mtim.tv_nsec);auto seen=payloadVerified.find(name);
  if(seen!=payloadVerified.end()&&seen->second==stamp)return true;
  FILE*f=fopen((path+".sha256").c_str(),"r");char sum[66]={};size_t n=f?fread(sum,1,65,f):0;if(f)fclose(f);
  if(n!=64||PayloadFileSha(path)!=sum)return false;
  // The central directory CRC also binds cached bytes to this ZIP entry.
  ApkPayloadEntry e{name.c_str(),name.c_str(),z.size,z.crc};if(!PayloadValid(path,e,true))return false;
  payloadVerified[name]=stamp;return true;};
 if(valid())return path;
 if(access(path.c_str(),F_OK)==0&&!payloadRetryLogged[name]){payloadRetryLogged[name]=true;PayloadLog("resource_sha256_failed reextract_once=1 resource="+name);}
 ApkPayloadEntry e{name.c_str(),name.c_str(),z.size,z.crc};
 for(unsigned attempt=0;attempt<2;attempt++){
  try{PayloadExtract(payloadApk.get(),z,e,payloadCacheRoot);if(valid())return path;}
  catch(const std::exception&){if(attempt)throw;}
  if(!payloadRetryLogged[name]){payloadRetryLogged[name]=true;PayloadLog("resource_sha256_failed reextract_once=1 resource="+name);}
 }
 PayloadFail("E-EXTRACT-BADSIZE","resource SHA256 retry exhausted "+name);
}
static int PayloadOpenAsset(const char* name){
 if(!name)return -1;int saved=errno;
 try{std::lock_guard<std::mutex>g(payloadCacheLock);std::string path=PayloadEnsure(std::string("assets/")+name);if(path.empty()){errno=ENOENT;return -1;}int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd>=0)errno=saved;return fd;}
 catch(const std::exception&e){if(!payloadOpenErrorLogged.exchange(true))PayloadLog(std::string("asset_open_failed ")+e.what());errno=EIO;return -1;}
}
static std::vector<unsigned char> PayloadBytes(const std::string& name){
 auto p=PayloadEnsure(name);FILE*f=fopen(p.c_str(),"rb");if(!f)PayloadFail("E-EXTRACT-OPEN","manifest read");
 auto size=payloadIndex.at(name).size;if(size>16*1024*1024){fclose(f);PayloadFail("E-EXTRACT-OPEN","manifest too large");}
 std::vector<unsigned char>b(size);bool ok=fread(b.data(),1,b.size(),f)==b.size();fclose(f);if(!ok)PayloadFail("E-EXTRACT-OPEN","manifest short read");return b;
}
static void PayloadReadVersion(){
 auto b=PayloadBytes("AndroidManifest.xml");
 auto u16=[&](size_t p){if(p+2>b.size())throw std::runtime_error("AXML bounds");return Payload16(b.data()+p);};
 auto u32=[&](size_t p){if(p+4>b.size())throw std::runtime_error("AXML bounds");return Payload32(b.data()+p);};
 std::vector<std::string>strings;
 for(size_t p=u16(2);p+8<=b.size();){uint16_t kind=u16(p),header=u16(p+2);uint32_t size=u32(p+4);if(size<8||header<8||size<header||size>b.size()-p)throw std::runtime_error("AXML chunk");
  if(kind==1){unsigned count=u32(p+8),flags=u32(p+16),start=u32(p+20);if(count>(size-header)/4)throw std::runtime_error("AXML string count");
   auto length=[&](size_t& q,bool wide){unsigned n=wide?u16(q):b.at(q);q+=wide?2:1;unsigned mask=wide?0x8000:0x80;if(n&mask){n=((n&(mask-1))<<(wide?16:8))|(wide?u16(q):b.at(q));q+=wide?2:1;}return n;};
   for(unsigned i=0;i<count;i++){size_t q=p+start+u32(p+header+4*i);std::string s;if(flags&256){length(q,false);unsigned n=length(q,false);if(q+n>p+size)throw std::runtime_error("AXML UTF8 bounds");s.assign((char*)b.data()+q,n);}
    else{unsigned n=length(q,true);if(q+2*n>p+size)throw std::runtime_error("AXML UTF16 bounds");for(unsigned j=0;j<n;j++){unsigned c=u16(q+2*j);if(c<128)s+=(char)c;else if(c<2048){s+=(char)(0xc0|(c>>6));s+=(char)(0x80|(c&63));}else{s+=(char)(0xe0|(c>>12));s+=(char)(0x80|((c>>6)&63));s+=(char)(0x80|(c&63));}}}strings.push_back(s);}
  }
  if(kind==0x102&&strings.at(u32(p+20))=="manifest"){
   size_t a=p+16+u16(p+24);unsigned count=u16(p+28),stride=u16(p+26);if(stride<20||a>p+size||count>(p+size-a)/stride)throw std::runtime_error("AXML attributes");
   for(unsigned i=0;i<count;i++){size_t q=a+i*stride;auto name=strings.at(u32(q+4));unsigned raw=u32(q+8),value=u32(q+16),type=b.at(q+15);
    if(name=="versionName"&&(raw!=UINT32_MAX||type==3))payloadVersion=strings.at(raw!=UINT32_MAX?raw:value);
    if(name=="versionCode")payloadVersionCode=raw!=UINT32_MAX?(uint32_t)std::stoul(strings.at(raw)):value;
   }return;
  }p+=size;
 }
}
static std::string PayloadVersionInfo(){std::lock_guard<std::mutex>g(payloadCacheLock);return "Native Probe 0.1.0 (10001)\napk_version="+payloadVersion+"\napk_version_code="+std::to_string(payloadVersionCode);}
static void PayloadPublishBootOriginal(const std::string& source,const std::string& dest){
 // App sandbox policy can forbid hard links across directories. Keep only
 // boot-critical legacy paths as atomic byte copies of the verified cache.
 std::string part=dest+".part";int in=open(source.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(in<0)PayloadFail("E-EXTRACT-OPEN","boot cache");
 int out=open(part.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(out<0){close(in);PayloadFail("E-EXTRACT-WRITE","boot copy");}
 try{std::vector<unsigned char>b(65536);ssize_t n;while((n=read(in,b.data(),b.size()))>0)PayloadWrite(out,b.data(),n);if(n<0)PayloadFail("E-EXTRACT-OPEN","boot copy read");close(in);in=-1;if(fsync(out))PayloadFail("E-EXTRACT-WRITE","boot copy fsync");close(out);out=-1;if(rename(part.c_str(),dest.c_str()))PayloadFail("E-EXTRACT-WRITE","boot copy rename");}
 catch(...){if(in>=0)close(in);if(out>=0)close(out);unlink(part.c_str());throw;}
}
#ifndef PAYLOAD_BOOT_REUSE
#define PAYLOAD_BOOT_REUSE 1
#endif
#include "payload_boot_reuse.inc"
static std::string PayloadPrepare(const std::string& directory,int sourceFd,uint64_t sourceOffset,uint64_t sourceLength,bool reset){
 (void)reset;std::lock_guard<std::mutex>cacheGuard(payloadCacheLock);
 char resolved[PATH_MAX];if(!realpath(directory.c_str(),resolved))PayloadFail("E-EXTRACT-OPEN","directory");
 {std::lock_guard<std::mutex>g(payloadStateLock);payloadRoot=resolved;payloadError.clear();}
 payloadVerified.clear();payloadRetryLogged.clear();payloadOpenErrorLogged=false;payloadDone=0;payloadTotal=0;payloadEntriesDone=0;payloadEntriesTotal=0;
 PayloadStage("native_asset_dir/fingerprint");payloadKey=PayloadFingerprint(sourceFd,sourceOffset,sourceLength);
 bool manifest=false;
#if PAYLOAD_HAS_MANIFEST
#ifndef APK_MANIFEST_MISMATCH_TEST
 manifest=sourceLength==APK_SOURCE_SIZE&&payloadKey==APK_SOURCE_ID;
#endif
#endif
 PayloadLog(manifest?"manifest_match_fast_path":"manifest_mismatch_fallback_runtime");
 PayloadLog("apk_key="+payloadKey+" source_bytes="+std::to_string(sourceLength)+" whole_apk_crc_hard_gate=0");
 std::string apkPath=payloadRoot+"/base.apk";
 int old=open(apkPath.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st{};bool ready=old>=0&&!fstat(old,&st)&&(uint64_t)st.st_size==sourceLength&&PayloadFingerprint(old,0,sourceLength)==payloadKey;if(old>=0)close(old);
 if(!ready){if(PayloadFree(payloadRoot)<sourceLength+64*1024*1024ULL)PayloadFail("E-EXTRACT-NOSPACE","copy APK");std::string part=apkPath+".part";int fd=open(part.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)PayloadFail("E-EXTRACT-WRITE","copy APK open");
  try{std::vector<unsigned char>b(65536);for(uint64_t at=0;at<sourceLength;){size_t n=(size_t)std::min<uint64_t>(b.size(),sourceLength-at);ssize_t k=pread(sourceFd,b.data(),n,sourceOffset+at);if(k<0&&errno==EINTR)continue;if(k<=0)PayloadFail("E-EXTRACT-OPEN","copy short read");PayloadWrite(fd,b.data(),k);at+=k;}if(fsync(fd))PayloadFail("E-EXTRACT-WRITE","APK fsync");close(fd);fd=-1;if(rename(part.c_str(),apkPath.c_str()))PayloadFail("E-EXTRACT-WRITE","APK rename");}catch(...){if(fd>=0)close(fd);unlink(part.c_str());throw;}
 }
 payloadApk.reset(fopen(apkPath.c_str(),"rb"));if(!payloadApk)PayloadFail("E-EXTRACT-OPEN","base.apk");payloadIndex=PayloadIndex(payloadApk.get());
 auto parent=payloadRoot.substr(0,payloadRoot.find_last_of('/'));PayloadDirs(parent,"apk-cache/"+payloadKey+"/entry");payloadCacheRoot=parent+"/apk-cache/"+payloadKey;
 PayloadStage("native_asset_dir/dex_bootstrap");
 std::vector<std::string>dex;
#if PAYLOAD_HAS_MANIFEST
 if(manifest){for(const auto&e:APK_PAYLOAD)if(PayloadDex(e.name))dex.push_back(e.name);}
#endif
 if(!manifest){for(const auto&e:payloadIndex)if(PayloadDex(e.first))dex.push_back(e.first);}
 if(dex.empty())PayloadFail("E-EXTRACT-NOENTRY","no DEX in APK");
 payloadEntriesTotal=dex.size()+1+(payloadIndex.count("assets/bin/Data/Managed/Metadata/global-metadata.dat")?1:0);
 // Remove only stale root DEX links, so a replacement APK cannot load old DEX.
 DIR*dir=opendir(payloadRoot.c_str());if(!dir)PayloadFail("E-EXTRACT-OPEN","DEX directory");while(auto*e=readdir(dir)){if(PayloadDex(e->d_name)&&std::find(dex.begin(),dex.end(),e->d_name)==dex.end()){std::string stale=payloadRoot+"/"+e->d_name;unlink(stale.c_str());unlink((stale+".sha256").c_str());}}closedir(dir);
 for(const auto&name:dex){auto p=PayloadEnsure(name);if(p.empty())PayloadFail("E-EXTRACT-NOENTRY",name);std::string digest;
#if PAYLOAD_HAS_MANIFEST
  if(manifest)for(const auto& d:APK_BOOT_DIGESTS)if(name==d.name){digest=d.sha256;break;}
#endif
  PayloadPublishBootVerified(p,payloadRoot+"/"+name,manifest,payloadIndex.at(name).size,digest);++payloadEntriesDone;}
 PayloadDirs(payloadRoot,"native-assets/.placeholder");
 // Diagnostic-only legacy metadata reader needs this single bootstrap file.
 const std::string metadata="assets/bin/Data/Managed/Metadata/global-metadata.dat";
 if(payloadIndex.count(metadata)){auto p=PayloadEnsure(metadata);auto rel=std::string("native-assets/")+metadata.substr(7);PayloadDirs(payloadRoot,rel);PayloadPublishBootOriginal(p,payloadRoot+"/"+rel);++payloadEntriesDone;}
 payloadVersion="unknown";payloadVersionCode=0;
 try{PayloadReadVersion();++payloadEntriesDone;}catch(const std::exception&e){PayloadLog(std::string("apk_version_read_failed reason=")+e.what());}
 PayloadLog("project_version=0.1.0 project_version_code=10001 apk_version="+payloadVersion+" apk_version_code="+std::to_string(payloadVersionCode));
 PayloadStage("native_asset_dir/ready_lazy");PayloadLog("runtime_index_entries="+std::to_string(payloadIndex.size())+" boot_dex="+std::to_string(dex.size())+" extracted_bytes="+std::to_string(payloadDone.load()));
 return "APK indexed; assets extracted on demand";
}
