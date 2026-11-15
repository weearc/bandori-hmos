/* Cold bootstrap only. Streaming ZIP extraction; never runs on render/vsync. */
#include <zlib.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <algorithm>
#include "apk_payload_manifest.h"
static std::mutex payloadStateLock,payloadLogLock;
static std::string payloadRoot,payloadStep="waiting",payloadError;
static std::atomic<uint64_t> payloadDone{0},payloadTotal{0};
static std::atomic<bool> payloadBusy{false};
static uint64_t PayloadMs(){return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static void PayloadLog(const std::string& text){
 std::lock_guard<std::mutex> g(payloadLogLock);if(payloadRoot.empty())return;
 std::string p=payloadRoot+"/vm-boot.txt";struct stat st{};
 if(!stat(p.c_str(),&st)&&st.st_size>1024*1024-8192){unlink((p+".1").c_str());rename(p.c_str(),(p+".1").c_str());}
 FILE*f=fopen(p.c_str(),"a");if(f){fprintf(f,"pid=%d mono_ms=%llu %s\n",getpid(),(unsigned long long)PayloadMs(),text.substr(0,4096).c_str());fclose(f);}
}
static void PayloadStage(const std::string& step){
 {std::lock_guard<std::mutex> g(payloadStateLock);payloadStep=step;}
 PayloadLog("step="+step);
}
[[noreturn]] static void PayloadFail(const char* code,const std::string& detail){
 int e=errno;throw std::runtime_error(std::string(code)+" step="+payloadStep+" errno="+std::to_string(e)+" "+detail);
}
#include "payload_sha256.h"
static uint64_t PayloadFree(const std::string& root){struct statvfs st{};if(statvfs(root.c_str(),&st))PayloadFail("E-EXTRACT-OPEN","statvfs");return (uint64_t)st.f_bavail*st.f_frsize;}
static void PayloadDirs(const std::string& root,const std::string& relative){
 size_t pos=0;while((pos=relative.find('/',pos))!=std::string::npos){std::string p=root+"/"+relative.substr(0,pos++);struct stat st{};
  if(lstat(p.c_str(),&st)){if(errno!=ENOENT||mkdir(p.c_str(),0700))PayloadFail("E-EXTRACT-WRITE","mkdir "+p);}
  else if(!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))PayloadFail("E-EXTRACT-OPEN","non-directory "+p);
 }
}
static bool PayloadValid(const std::string& path,const ApkPayloadEntry& expected,bool crc){
 struct stat st{};if(lstat(path.c_str(),&st)||!S_ISREG(st.st_mode)||(uint64_t)st.st_size!=expected.size)return false;
 if(!crc)return true;int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
 uint32_t sum=crc32(0,nullptr,0);unsigned char b[65536];ssize_t n;
 while((n=read(fd,b,sizeof b))>0)sum=crc32(sum,b,(uInt)n);close(fd);return n==0&&sum==expected.crc;
}
struct PayloadZipEntry{uint32_t compressed,size,crc,offset;uint16_t method,flags;};
static uint16_t Payload16(const unsigned char*p){return p[0]|(uint16_t)p[1]<<8;}
static uint32_t Payload32(const unsigned char*p){return Payload16(p)|(uint32_t)Payload16(p+2)<<16;}
static void PayloadRead(FILE*f,uint64_t offset,void*b,size_t n){if(fseeko(f,(off_t)offset,SEEK_SET)||fread(b,1,n,f)!=n)PayloadFail("E-EXTRACT-OPEN","short APK read offset="+std::to_string(offset));}
static bool PayloadSelected(const std::string& n){
 if(n.compare(0,7,"assets/")==0)return !n.empty()&&n.back()!='/';
 if(n.compare(0,7,"classes")||n.size()<11||n.substr(n.size()-4)!=".dex")return false;
 for(size_t i=7;i<n.size()-4;i++)if(n[i]<'0'||n[i]>'9')return false;return true;
}
static std::map<std::string,PayloadZipEntry> PayloadIndex(FILE*f){
 if(fseeko(f,0,SEEK_END))PayloadFail("E-EXTRACT-OPEN","APK seek");off_t size=ftello(f);
 if(size<22)PayloadFail("E-EXTRACT-OPEN","invalid APK size");size_t count=(size_t)std::min<off_t>(size,65557);
 std::vector<unsigned char> tail(count);PayloadRead(f,size-count,tail.data(),count);int found=-1;
 for(int i=(int)count-22;i>=0;i--)if(Payload32(&tail[i])==0x06054b50&&i+22+Payload16(&tail[i+20])==count){found=i;break;}
 if(found<0)PayloadFail("E-EXTRACT-OPEN","ZIP end missing");auto*e=&tail[found];
 uint32_t offset=Payload32(e+16),bytes=Payload32(e+12);unsigned entries=Payload16(e+10);
 if(Payload16(e+4)||Payload16(e+6)||entries!=Payload16(e+8)||entries==65535||offset==UINT32_MAX||(uint64_t)offset+bytes>(uint64_t)size)PayloadFail("E-EXTRACT-OPEN","unsupported ZIP layout");
 std::map<std::string,PayloadZipEntry> index;uint64_t p=offset,end=(uint64_t)offset+bytes;
 for(unsigned i=0;i<entries;i++){
  unsigned char h[46];if(p+46>end)PayloadFail("E-EXTRACT-OPEN","central directory bounds");PayloadRead(f,p,h,sizeof h);
  if(Payload32(h)!=0x02014b50)PayloadFail("E-EXTRACT-OPEN","central signature");unsigned names=Payload16(h+28),extra=Payload16(h+30),comment=Payload16(h+32);
  if(p+46+names+extra+comment>end)PayloadFail("E-EXTRACT-OPEN","central entry bounds");std::string name(names,'\0');PayloadRead(f,p+46,name.data(),names);
  if(PayloadSelected(name)){
   PayloadZipEntry z{Payload32(h+20),Payload32(h+24),Payload32(h+16),Payload32(h+42),Payload16(h+10),Payload16(h+8)};
   if(z.flags&1||z.offset==UINT32_MAX||z.size==UINT32_MAX||(z.method!=0&&z.method!=8)||!index.emplace(name,z).second)PayloadFail("E-EXTRACT-OPEN","unsupported/duplicate entry "+name);
  }p+=46+names+extra+comment;
 }
 if(index.size()!=sizeof APK_PAYLOAD/sizeof APK_PAYLOAD[0])PayloadFail("E-EXTRACT-NOENTRY","payload entry count mismatch");
 for(const auto& s:APK_PAYLOAD){auto it=index.find(s.name);if(it==index.end()||it->second.size!=s.size||it->second.crc!=s.crc)PayloadFail("E-EXTRACT-NOENTRY",s.name);}
 return index;
}
static void PayloadWrite(int fd,const void*buffer,size_t n){
 const unsigned char*p=(const unsigned char*)buffer;
 while(n){ssize_t k=write(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)PayloadFail(errno==ENOSPC?"E-EXTRACT-NOSPACE":"E-EXTRACT-WRITE","write payload");p+=k;n-=k;}
}
static void PayloadExtract(FILE*apk,const PayloadZipEntry& z,const ApkPayloadEntry& s,const std::string& root){
 PayloadDirs(root,s.dest);std::string dest=root+"/"+s.dest,part=dest+".part";
 unsigned char h[30];PayloadRead(apk,z.offset,h,sizeof h);
 if(Payload32(h)!=0x04034b50)PayloadFail("E-EXTRACT-OPEN","local signature "+std::string(s.name));
 unsigned names=Payload16(h+26);std::string name(names,'\0');PayloadRead(apk,z.offset+30,name.data(),names);
 if(name!=s.name)PayloadFail("E-EXTRACT-OPEN","local name mismatch");uint64_t offset=(uint64_t)z.offset+30+names+Payload16(h+28);
 int fd=open(part.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)PayloadFail(errno==ENOSPC?"E-EXTRACT-NOSPACE":"E-EXTRACT-WRITE",part);
 z_stream stream{};bool inflated=false;uint64_t produced=0;uint32_t crc=crc32(0,nullptr,0);int status=Z_OK;
 try{
  if(z.method==8){if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK)PayloadFail("E-EXTRACT-OPEN","inflate init");inflated=true;}
  unsigned char in[65536],out[65536];uint64_t remain=z.compressed;
  while(remain){size_t n=(size_t)std::min<uint64_t>(remain,sizeof in);PayloadRead(apk,offset,in,n);offset+=n;remain-=n;
   if(z.method==0){if(produced+n>s.size)PayloadFail("E-EXTRACT-BADSIZE","size overflow");PayloadWrite(fd,in,n);crc=crc32(crc,in,n);produced+=n;payloadDone.fetch_add(n);}
   else{stream.next_in=in;stream.avail_in=n;do{stream.next_out=out;stream.avail_out=sizeof out;status=inflate(&stream,Z_NO_FLUSH);if(status!=Z_OK&&status!=Z_STREAM_END)PayloadFail("E-EXTRACT-OPEN","inflate "+std::string(s.name));size_t got=sizeof out-stream.avail_out;
     if(produced+got>s.size)PayloadFail("E-EXTRACT-BADSIZE","inflate size overflow");PayloadWrite(fd,out,got);crc=crc32(crc,out,got);produced+=got;payloadDone.fetch_add(got);
    }while((stream.avail_in||!stream.avail_out)&&status!=Z_STREAM_END);if(status==Z_STREAM_END&&(remain||stream.avail_in))PayloadFail("E-EXTRACT-OPEN","trailing compressed bytes");}
  }
  if(inflated){inflateEnd(&stream);inflated=false;}
  if(produced!=s.size||crc!=s.crc||(z.method==8&&status!=Z_STREAM_END))PayloadFail("E-EXTRACT-BADSIZE","CRC/size "+std::string(s.name));
  if(fsync(fd))PayloadFail(errno==ENOSPC?"E-EXTRACT-NOSPACE":"E-EXTRACT-WRITE","fsync");close(fd);fd=-1;
  if(rename(part.c_str(),dest.c_str()))PayloadFail("E-EXTRACT-WRITE","rename "+dest);
 }catch(...){if(inflated)inflateEnd(&stream);if(fd>=0)close(fd);unlink(part.c_str());throw;}
}
static std::string PayloadPrepare(const std::string& directory,int sourceFd,uint64_t sourceOffset,uint64_t sourceLength,bool reset){
 char resolved[PATH_MAX];if(!realpath(directory.c_str(),resolved))PayloadFail("E-EXTRACT-OPEN","directory");
 {std::lock_guard<std::mutex>g(payloadStateLock);payloadRoot=resolved;payloadError.clear();}
 uint64_t total=0;for(const auto&s:APK_PAYLOAD)total+=s.size;payloadTotal=total;payloadDone=0;

#ifndef NDEBUG
 if(reset){
  PayloadStage("native_asset_dir/selftest_reset");
  for(const auto&e:APK_PAYLOAD){std::string path=payloadRoot+"/"+e.dest;if(unlink(path.c_str())&&errno!=ENOENT)PayloadFail("E-EXTRACT-WRITE","selftest unlink "+path);unlink((path+".part").c_str());}
  unlink((payloadRoot+"/native-assets/.complete").c_str());
  std::vector<std::string> dirs;
  for(const auto&e:APK_PAYLOAD){std::string path=e.dest;size_t slash;while((slash=path.find_last_of('/'))!=std::string::npos){path.resize(slash);dirs.push_back(payloadRoot+"/"+path);}}
  std::sort(dirs.begin(),dirs.end(),[](const std::string&a,const std::string&b){return a.size()>b.size()||(a.size()==b.size()&&a>b);});
  dirs.erase(std::unique(dirs.begin(),dirs.end()),dirs.end());for(const auto&dir:dirs)if(rmdir(dir.c_str())&&errno!=ENOENT)PayloadFail("E-EXTRACT-WRITE","selftest rmdir "+dir);
  PayloadLog("selftest native_assets_absent="+std::to_string(access((payloadRoot+"/native-assets").c_str(),F_OK)!=0));
  PayloadLog("selftest_reset=complete dex_assets_libraries_removed=1 prefs_untouched=1");
 }
#else
 (void)reset;
#endif
 auto begin=PayloadMs();PayloadStage("native_asset_dir/copy_apk");
 if(sourceLength!=APK_SOURCE_SIZE)PayloadFail("E-EXTRACT-OPEN","packaged APK length mismatch");
 std::string apkPath=payloadRoot+"/base.apk",stampPath=payloadRoot+"/.apk-source",apkPart=apkPath+".part";
 FILE*stamp=fopen(stampPath.c_str(),"r");char hash[128]={0};if(stamp){fread(hash,1,sizeof hash-1,stamp);fclose(stamp);}
 ApkPayloadEntry source{"base.apk","base.apk",APK_SOURCE_SIZE,APK_SOURCE_CRC};
 bool sourceReady=PayloadValid(apkPath,source,std::string(hash)!=APK_SOURCE_ID);
 if(!sourceReady){
  unlink(apkPart.c_str());uint64_t free=PayloadFree(payloadRoot),needed=APK_SOURCE_SIZE+64*1024*1024ULL;
  if(free<needed){errno=ENOSPC;PayloadFail("E-EXTRACT-NOSPACE","需要 "+std::to_string((needed+1048575)/1048576)+" MB / 可用 "+std::to_string(free/1048576)+" MB");}
  int out=open(apkPart.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(out<0)PayloadFail("E-EXTRACT-WRITE","copy APK open");
  try{unsigned char b[65536];uint64_t at=0;uint32_t crc=crc32(0,nullptr,0);
   while(at<sourceLength){size_t n=(size_t)std::min<uint64_t>(sizeof b,sourceLength-at);ssize_t got=pread(sourceFd,b,n,sourceOffset+at);
    if(got<0&&errno==EINTR)continue;if(got<=0)PayloadFail("E-EXTRACT-OPEN","packaged APK short read");PayloadWrite(out,b,got);crc=crc32(crc,b,got);at+=got;}
   if(crc!=APK_SOURCE_CRC)PayloadFail("E-EXTRACT-OPEN","packaged APK CRC");if(fsync(out))PayloadFail("E-EXTRACT-WRITE","APK fsync");close(out);out=-1;
   if(rename(apkPart.c_str(),apkPath.c_str()))PayloadFail("E-EXTRACT-WRITE","APK rename");
  }catch(...){if(out>=0)close(out);unlink(apkPart.c_str());throw;}
 }
 if(std::string(hash)!=APK_SOURCE_ID){FILE*f=fopen(stampPath.c_str(),"w");if(!f)PayloadFail("E-EXTRACT-WRITE","APK stamp");fputs(APK_SOURCE_ID,f);fclose(f);}
 PayloadStage("native_asset_dir/open_apk");
 std::unique_ptr<FILE,decltype(&fclose)> apk(fopen((payloadRoot+"/base.apk").c_str(),"rb"),fclose);
 if(!apk)PayloadFail("E-EXTRACT-OPEN","base.apk");auto index=PayloadIndex(apk.get());
 std::string marker=payloadRoot+"/native-assets/.complete";
 PayloadSha256 test;test.add("abc",3);if(test.finish()!="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")PayloadFail("E-EXTRACT-OPEN","SHA256 selftest");
 std::string expected=std::string("{\"versionCode\":")+std::to_string(APK_APP_VERSION_CODE)+",\"versionName\":\""+APK_APP_VERSION_NAME+"\",\"files\":"+std::to_string(index.size())+",\"bytes\":"+std::to_string(total)+",\"our_so\":{\"libprobe.so\":\""+PayloadOurHash("libprobe.so")+"\",\"libsdktrace.so\":\""+PayloadOurHash("libsdktrace.so")+"\"},\"manifest_sha256\":\""+APK_PAYLOAD_HASH+"\",\"ts\":";

 FILE* mf=fopen(marker.c_str(),"r");std::string saved;if(mf){char b[2048];size_t n=fread(b,1,sizeof b,mf);saved.assign(b,n);fclose(mf);}
 bool complete=false;
 if(saved.compare(0,expected.size(),expected)==0){std::string tail=saved.substr(expected.size());size_t digits=tail.find_first_not_of("0123456789");complete=digits!=std::string::npos&&digits>0&&tail.substr(digits)=="}\n";}for(const auto&s:APK_PAYLOAD)if(complete&&!PayloadValid(payloadRoot+"/"+s.dest,s,false))complete=false;
 if(complete){payloadDone=total;PayloadStage("native_asset_dir/ready_cached");PayloadLog("result=skip_complete written_bytes=0 elapsed_ms="+std::to_string(PayloadMs()-begin));return "native-assets ready (cached)";}
 PayloadStage("native_asset_dir/check_existing");std::vector<bool> valid;uint64_t remain=0;unsigned skipped=0;
 for(const auto&s:APK_PAYLOAD){PayloadDirs(payloadRoot,s.dest);unlink((payloadRoot+"/"+s.dest+".part").c_str());bool ok=PayloadValid(payloadRoot+"/"+s.dest,s,true);valid.push_back(ok);if(ok){payloadDone.fetch_add(s.size);skipped++;}else remain+=s.size;}
 uint64_t freeBefore=PayloadFree(payloadRoot),minFree=freeBefore,required=remain+64*1024*1024ULL;
 PayloadLog("space required_bytes="+std::to_string(required)+" available_bytes="+std::to_string(freeBefore)+" remaining_bytes="+std::to_string(remain));
 if(freeBefore<required){errno=ENOSPC;PayloadFail("E-EXTRACT-NOSPACE","需要 "+std::to_string((required+1048575)/1048576)+" MB / 可用 "+std::to_string(freeBefore/1048576)+" MB");}
 PayloadStage("native_asset_dir/extract");unsigned i=0,written=0;uint64_t last=0;
 for(const auto&s:APK_PAYLOAD){if(!valid[i]){PayloadExtract(apk.get(),index.at(s.name),s,payloadRoot);written++;minFree=std::min(minFree,PayloadFree(payloadRoot));}i++;
  uint64_t now=PayloadMs();if(now-last>=1000){last=now;PayloadLog("progress done_bytes="+std::to_string(payloadDone.load())+" total_bytes="+std::to_string(total)+" files="+std::to_string(i));}
 }
 PayloadStage("native_asset_dir/commit");PayloadDirs(payloadRoot,"native-assets/.complete");std::string part=marker+".part";
 int fd=open(part.c_str(),O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)PayloadFail("E-EXTRACT-WRITE","marker open");
 try{std::string record=expected+std::to_string((long long)time(nullptr))+"}\n";PayloadWrite(fd,record.data(),record.size());if(fsync(fd))PayloadFail("E-EXTRACT-WRITE","marker fsync");close(fd);fd=-1;if(rename(part.c_str(),marker.c_str()))PayloadFail("E-EXTRACT-WRITE","marker rename");}catch(...){if(fd>=0)close(fd);unlink(part.c_str());throw;}
 PayloadStage("native_asset_dir/ready");PayloadLog("result=complete manifest_sha256="+std::string(APK_PAYLOAD_HASH)+" written_files="+std::to_string(written)+" skipped_files="+std::to_string(skipped)+" elapsed_ms="+std::to_string(PayloadMs()-begin)+" free_before="+std::to_string(freeBefore)+" min_free="+std::to_string(minFree)+" free_after="+std::to_string(PayloadFree(payloadRoot)));
 return "native-assets ready";
}
struct PayloadJob{napi_async_work work=nullptr;napi_deferred deferred=nullptr;std::string directory,result,error;int fd=-1;uint64_t offset=0,length=0;bool reset=false;};
static void PayloadExecute(napi_env,void* data){auto*j=(PayloadJob*)data;try{j->result=PayloadPrepare(j->directory,j->fd,j->offset,j->length,j->reset);}catch(const std::exception&e){j->error=e.what();{std::lock_guard<std::mutex>g(payloadStateLock);payloadError=j->error;}PayloadLog("result=failed "+j->error);}}
static void PayloadComplete(napi_env env,napi_status status,void*data){auto*j=(PayloadJob*)data;if(status!=napi_ok&&j->error.empty())j->error="E-EXTRACT-OPEN step=worker_cancelled";
 if(j->error.empty())napi_resolve_deferred(env,j->deferred,Str(env,j->result));else{napi_value e;napi_create_error(env,nullptr,Str(env,j->error),&e);napi_reject_deferred(env,j->deferred,e);}napi_delete_async_work(env,j->work);if(j->fd>=0)close(j->fd);payloadBusy=false;delete j;}
static napi_value PrepareNativeAssets(napi_env env,napi_callback_info info){
 size_t argc=5,n=0;napi_value args[5];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);int32_t fd=-1;int64_t offset=-1,length=-1;
 if((argc!=4&&argc!=5)||napi_get_value_string_utf8(env,args[0],nullptr,0,&n)!=napi_ok||!n||n>=PATH_MAX||napi_get_value_int32(env,args[1],&fd)!=napi_ok||fd<0||napi_get_value_int64(env,args[2],&offset)!=napi_ok||offset<0||napi_get_value_int64(env,args[3],&length)!=napi_ok||length<=0){napi_throw_type_error(env,nullptr,"E-EXTRACT-OPEN step=argument");return nullptr;}
 if(payloadBusy.exchange(true)){napi_throw_error(env,nullptr,"E-EXTRACT-OPEN step=already_running");return nullptr;}
 auto*j=new PayloadJob;std::vector<char>b(n+1);napi_get_value_string_utf8(env,args[0],b.data(),b.size(),&n);j->directory.assign(b.data(),n);j->fd=dup(fd);j->offset=offset;j->length=length;if(argc==5)napi_get_value_bool(env,args[4],&j->reset);
 if(j->fd<0){delete j;payloadBusy=false;napi_throw_error(env,nullptr,"E-EXTRACT-OPEN step=dup_apk");return nullptr;}
 napi_value promise,name;napi_status s=napi_create_promise(env,&j->deferred,&promise);if(s==napi_ok)s=napi_create_string_utf8(env,"Extract APK payload",NAPI_AUTO_LENGTH,&name);
 if(s==napi_ok)s=napi_create_async_work(env,nullptr,name,PayloadExecute,PayloadComplete,j,&j->work);if(s==napi_ok)s=napi_queue_async_work(env,j->work);
 if(s!=napi_ok){if(j->work)napi_delete_async_work(env,j->work);close(j->fd);delete j;payloadBusy=false;napi_throw_error(env,nullptr,"E-EXTRACT-OPEN step=queue_worker");return nullptr;}return promise;
}
static napi_value NativeAssetsStatus(napi_env env,napi_callback_info){std::lock_guard<std::mutex>g(payloadStateLock);uint64_t total=payloadTotal.load(),done=payloadDone.load();unsigned percent=total?(unsigned)std::min<uint64_t>(100,done*100/total):0;
 return Str(env,"vm_last_step="+payloadStep+"\n"+(payloadError.empty()?"正在解压游戏资源 "+std::to_string(percent)+"%":payloadError));}
