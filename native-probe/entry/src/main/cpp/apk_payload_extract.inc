/* ZIP primitives shared by cold bootstrap and on-demand asset cache. */
#include <zlib.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <algorithm>
struct ApkPayloadEntry {const char *name,*dest;uint64_t size;uint32_t crc;};
#if __has_include("apk_payload_manifest.h") && !defined(APK_MANIFEST_ABSENT_TEST)
#include "apk_payload_manifest.h"
#define PAYLOAD_HAS_MANIFEST 1
#else
#define PAYLOAD_HAS_MANIFEST 0
#endif
static std::mutex payloadStateLock,payloadLogLock;
static std::string payloadRoot,payloadStep="waiting",payloadError;
static std::atomic<uint64_t> payloadDone{0},payloadTotal{0};
// Bootstrap progress counts verified/published entries, including cache hits.
static std::atomic<uint64_t> payloadEntriesDone{0},payloadEntriesTotal{0};
static std::atomic<bool> payloadBusy{false};
static uint64_t PayloadMs(){return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static void PayloadLog(const std::string& text){
 std::lock_guard<std::mutex> g(payloadLogLock);if(payloadRoot.empty())return;
 std::string p=payloadRoot+"/vm-boot.txt";struct stat st{};
 if(!stat(p.c_str(),&st)&&st.st_size>1024*1024-8192){unlink((p+".1").c_str());rename(p.c_str(),(p+".1").c_str());}
 FILE*f=fopen(p.c_str(),"a");if(f){fchmod(fileno(f),0644);fprintf(f,"pid=%d mono_ms=%llu %s\n",getpid(),(unsigned long long)PayloadMs(),text.substr(0,4096).c_str());fclose(f);}
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
 uint32_t sum=crc32(0,nullptr,0);std::vector<unsigned char>b(65536);ssize_t n;
 while((n=read(fd,b.data(),b.size()))>0)sum=crc32(sum,b.data(),(uInt)n);close(fd);return n==0&&sum==expected.crc;
}
struct PayloadZipEntry{uint32_t compressed,size,crc,offset;uint16_t method,flags;};
static uint16_t Payload16(const unsigned char*p){return p[0]|(uint16_t)p[1]<<8;}
static uint32_t Payload32(const unsigned char*p){return Payload16(p)|(uint32_t)Payload16(p+2)<<16;}
static void PayloadRead(FILE*f,uint64_t offset,void*b,size_t n){if(fseeko(f,(off_t)offset,SEEK_SET)||fread(b,1,n,f)!=n)PayloadFail("E-EXTRACT-OPEN","short APK read offset="+std::to_string(offset));}
static bool PayloadSafePath(const std::string& n){
 if(n.empty()||n.front()=='/'||n.find('\\')!=std::string::npos||n.find('\0')!=std::string::npos)return false;
 size_t at=0;while(at<n.size()){size_t end=n.find('/',at);auto part=n.substr(at,end-at);if(part.empty()||part=="."||part=="..")return false;if(end==std::string::npos)break;at=end+1;}return true;
}
static bool PayloadDex(const std::string& n){
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
  if(!name.empty()&&name.back()!='/'){
   if(!PayloadSafePath(name))PayloadFail("E-EXTRACT-OPEN","unsafe ZIP path");
   PayloadZipEntry z{Payload32(h+20),Payload32(h+24),Payload32(h+16),Payload32(h+42),Payload16(h+10),Payload16(h+8)};
   if(z.flags&1||z.offset==UINT32_MAX||z.size==UINT32_MAX||(z.method!=0&&z.method!=8)||!index.emplace(name,z).second)PayloadFail("E-EXTRACT-OPEN","unsupported/duplicate entry "+name);
  }p+=46+names+extra+comment;
 }
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
 z_stream stream{};bool inflated=false;uint64_t produced=0;uint32_t crc=crc32(0,nullptr,0);int status=Z_OK;PayloadSha256 sha;
 try{
  if(z.method==8){if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK)PayloadFail("E-EXTRACT-OPEN","inflate init");inflated=true;}
  std::vector<unsigned char>in(65536),out(65536);uint64_t remain=z.compressed;
  while(remain){size_t n=(size_t)std::min<uint64_t>(remain,in.size());PayloadRead(apk,offset,in.data(),n);offset+=n;remain-=n;
   if(z.method==0){if(produced+n>s.size)PayloadFail("E-EXTRACT-BADSIZE","size overflow");PayloadWrite(fd,in.data(),n);sha.add(in.data(),n);crc=crc32(crc,in.data(),n);produced+=n;payloadDone.fetch_add(n);}
   else{stream.next_in=in.data();stream.avail_in=n;do{stream.next_out=out.data();stream.avail_out=out.size();status=inflate(&stream,Z_NO_FLUSH);if(status!=Z_OK&&status!=Z_STREAM_END)PayloadFail("E-EXTRACT-OPEN","inflate "+std::string(s.name));size_t got=out.size()-stream.avail_out;
     if(produced+got>s.size)PayloadFail("E-EXTRACT-BADSIZE","inflate size overflow");PayloadWrite(fd,out.data(),got);sha.add(out.data(),got);crc=crc32(crc,out.data(),got);produced+=got;payloadDone.fetch_add(got);
    }while((stream.avail_in||!stream.avail_out)&&status!=Z_STREAM_END);if(status==Z_STREAM_END&&(remain||stream.avail_in))PayloadFail("E-EXTRACT-OPEN","trailing compressed bytes");}
  }
  if(inflated){inflateEnd(&stream);inflated=false;}
  if(produced!=s.size||crc!=s.crc||(z.method==8&&status!=Z_STREAM_END))PayloadFail("E-EXTRACT-BADSIZE","CRC/size "+std::string(s.name));
  if(fsync(fd))PayloadFail(errno==ENOSPC?"E-EXTRACT-NOSPACE":"E-EXTRACT-WRITE","fsync");close(fd);fd=-1;
  if(rename(part.c_str(),dest.c_str()))PayloadFail("E-EXTRACT-WRITE","rename "+dest);
  FILE* digest=fopen((dest+".sha256").c_str(),"w");if(!digest)PayloadFail("E-EXTRACT-WRITE","digest open");auto sum=sha.finish();bool ok=fwrite(sum.data(),1,sum.size(),digest)==sum.size();if(fclose(digest))ok=false;if(!ok)PayloadFail("E-EXTRACT-WRITE","digest write");
 }catch(...){if(inflated)inflateEnd(&stream);if(fd>=0)close(fd);unlink(part.c_str());throw;}
}
#include <dirent.h>
#include "apk_cache_runtime.inc"
#ifndef PAYLOAD_OFFLINE_TEST
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
 return Str(env,"vm_last_step="+payloadStep+"\nprocessed="+std::to_string(payloadEntriesDone.load())+" total="+std::to_string(payloadEntriesTotal.load())+"\n"+(payloadError.empty()?"正在解压游戏资源 "+std::to_string(percent)+"%":payloadError));}

static napi_value ApkVersionInfo(napi_env env,napi_callback_info){return Str(env,PayloadVersionInfo());}
#endif
