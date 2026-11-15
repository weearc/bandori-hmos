import pathlib, re, subprocess, hashlib
root=pathlib.Path(__file__).resolve().parent
src=root/'vendor/mbedtls-3.6.7'
sdk=pathlib.Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native')
clang=sdk/'llvm/bin/clang.exe'; ar=sdk/'llvm/bin/llvm-ar.exe'
out=root/'direct-build'; out.mkdir(exist_ok=True)
text=(src/'library/CMakeLists.txt').read_text()
log=(out/'build.log').open('w',encoding='utf8')
def run(args):
 p=subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=120)
 log.write(' '.join(map(str,args))+'\n'+p.stdout+p.stderr); log.flush()
 if p.returncode: raise RuntimeError(f'command failed {p.returncode}: {args[0]} {p.stderr}')
for group,lib in [('crypto','mbedcrypto'),('x509','mbedx509'),('tls','mbedtls')]:
 match=re.search(r'set\(src_'+group+r'\s+(.*?)\)',text,re.S)
 files=match.group(1).split(); objs=[]
 for name in files:
  obj=out/(group+'_'+name+'.o'); objs.append(obj)
  run([clang,'--target=aarch64-linux-ohos','--sysroot='+str(sdk/'sysroot'),'-fPIC','-O2','-D__MUSL__','-I'+str(src/'include'),'-I'+str(src/'library'),'-c',src/'library'/name,'-o',obj])
 archive=out/('lib'+lib+'.a'); run([ar,'rcs',archive,*objs])
 print(archive.name, len(objs), hashlib.sha256(archive.read_bytes()).hexdigest(),flush=True)
