"""Build an isolated managed-arena candidate; never overwrite waiting v7 SO."""
from pathlib import Path
import hashlib,json,re,shutil,subprocess
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'build/next'
SRC=OUT/'source'
SRC.mkdir(parents=True,exist_ok=True)
original_library=hashlib.sha256((ROOT/'build/libsdktrace.so').read_bytes()).hexdigest()
for pattern in ['*.c','*.h','*.inc']:
    for path in ROOT.glob(pattern):
        shutil.copy2(path,SRC/path.name)
shutil.copytree(ROOT/'generated',SRC/'generated',dirs_exist_ok=True)
def edit(path,old,new,count=1):
    p=SRC/path;s=p.read_text(encoding='utf8')
    assert s.count(old)==count,(path,old,s.count(old))
    p.write_text(s.replace(old,new),encoding='utf8')

core=SRC/'generated/dvm/dvm.c'
source=core.read_text(encoding='utf8')
match=re.search(r'struct dvm \*dvm_create\([^;]*?\)\s*\{.*?^}',source,re.M|re.S)
assert match
body=match.group()
body=body.replace('{\n','{\n   if (!vm_arena_begin(VM_ARENA_LIMIT)) return NULL;\n',1)
body=body.replace('if (!vm) return NULL;','if (!vm) { vm_arena_end(); return NULL; }')
body=body.replace('      free(vm);\n      return NULL;','      free(vm);\n      vm_arena_end();\n      return NULL;')
body=body.replace('   prof_start();','   /* Candidate disables optional profiler workers; VM execution stays serialized. */')
source=source[:match.start()]+body+source[match.end():]
match=re.search(r'void dvm_destroy\([^;]*?\)\s*\{.*?^}',source,re.M|re.S)
assert match
body=match.group();assert '   free(vm);\n}' in body
body=body.replace('   free(vm);\n}','   free(vm);\n   vm_arena_end();\n}')
source=source[:match.start()]+body+source[match.end():]
match=re.search(r'void dvm__throw\([^;]*?\)\s*\{.*?^}',source,re.M|re.S)
assert match
body=match.group();assert not re.search(r'\breturn\s*;',body)
body=body.replace('{\n','{\n   vm_arena_emergency_enter();\n',1)
body=body[:-1]+'   vm_arena_emergency_leave();\n}'
source=source[:match.start()]+body+source[match.end():]
# Metadata failures must not dereference null field arrays.
for field,count in [('ifields','nifields'),('sfields','nsfields')]:
    needle=f'   int slot = base;' if field=='ifields' else '   for (int i = 0; i < c->nsfields; ++i) {'
    assert source.count(needle)==1
    source=source.replace(needle,f'   if (c->{count} && !c->{field}) {{ dvm__throw(vm,"java/lang/OutOfMemoryError","class fields"); return false; }}\n'+needle)
# Reject partial dex cache initialization before publishing it to the VM.
needle=re.compile(r'(   dd->string_cache\s*= calloc\([^\n]+\);)')
source,n=needle.subn(r'\1\n   if (!dd->type_cache || !dd->method_cache || !dd->proto_cache || !dd->field_cache || !dd->string_cache) { free(dd->type_cache); free(dd->method_cache); free(dd->proto_cache); free(dd->field_cache); free(dd->string_cache); dex_close(&dd->file); free(dd); return false; }',source)
assert n==2
core.write_text(source,encoding='utf8')
edit(Path('runtime_extra.inc'),'if(IS("maxMemory","()J"))sdk_trace_fprintf(stderr,"RUNTIME_BOUNDARY maxMemory requires an enforced managed-heap budget; host RAM and arbitrary constants are not Java heap limits\\n");',
     'if(IS("maxMemory","()J")){out->j=(int64_t)vm_arena_limit();return true;}')
edit(Path('runtime_extra_test.inc'),'EXTRA_CHECK(!runtime_extra_call(v,runtime,"maxMemory","()J",r,NULL,0,&out));',
     'EXTRA_CHECK(runtime_extra_call(v,runtime,"maxMemory","()J",r,NULL,0,&out)&&out.j==VM_ARENA_LIMIT);')
for name in ['strict_runtime.c','java_basics_test.c','runtime_extra_test.inc']:
    p=SRC/name;s=p.read_text(encoding='utf8').replace('standard-batch-v7','managed-arena-v8');p.write_text(s,encoding='utf8')
edit(Path('strict_runtime.c'),' if(!java_basics_selftest()){',' if(!vm_arena_selftest()||!java_basics_selftest()){')
edit(Path('java_basics_test.c'),'if(!runtime_extra_selftest(v,&checks))okay=false;dvm_destroy(v);',
     'if(!runtime_extra_selftest(v,&checks))okay=false;if(!vm_arena_java_selftest(v))okay=false;dvm_destroy(v);')
edit(Path('strict_runtime.c'),' sdk_trace_fprintf(stderr,"SCOPE eight isolated original SDK paths; independent methods do not constitute complete startup or login\\n");',
     ' sdk_trace_fprintf(stderr,"SCOPE eight isolated original SDK paths; independent methods do not constitute complete startup or login\\n");\n sdk_trace_fprintf(stderr,"MANAGED_ARENA limit=134217728 emergency_reserve=2097152 scope=VM_objects_strings_collections_DEX_metadata_headers enforced=1 host_libc_internal_buffers_excluded=1 Java_threads_not_started=1\\n");')
compiler=Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/llvm/bin/clang.exe')
sdk=compiler.parents[2]
common=[str(compiler),'--target=aarch64-linux-ohos','--sysroot='+str(sdk/'sysroot'),'-std=gnu11','-fPIC','-O1','-I'+str(SRC),'-I'+str(SRC/'generated')]
sources=['generated/dvm/dvm.c','generated/dvm/dex.c','strict_runtime.c','java_basics.c','java_basics_test.c','android_basics.c','android_basics_test.c','guest_profile.c','collections.c','collections_test.c','next_arena.c']
objects=[]
for name in sources:
    obj=OUT/(Path(name).stem+'.o');args=common.copy()
    if name!='next_arena.c':args+=['-include',str(SRC/'next_interpose.h')]
    subprocess.run(args+['-c',str(SRC/name),'-o',str(obj)],check=True);objects.append(str(obj))
nm=compiler.parent/'llvm-nm.exe'
allocation_symbols={'malloc','calloc','realloc','free','strdup','strndup','aligned_alloc','posix_memalign','mmap'}
audit=[]
for name,obj in zip(sources,objects):
    if name=='next_arena.c':continue
    undefined=subprocess.check_output([str(nm),'--undefined-only',obj],text=True)
    bypass=allocation_symbols.intersection(line.split()[-1] for line in undefined.splitlines() if line.split())
    assert not bypass,(name,'untracked allocation symbols',bypass)
    audit.append({'translation_unit':name,'untracked_allocation_symbols':sorted(bypass)})
subprocess.run(common+['-shared',*objects,'-Wl,--no-undefined','-lm','-o',str(OUT/'libsdktrace.so')],check=True)
assert hashlib.sha256((ROOT/'build/libsdktrace.so').read_bytes()).hexdigest()==original_library
manifest={'candidate':'managed-arena-v8','waiting_v7_sha256_unchanged':original_library,
          'candidate_sha256':hashlib.sha256((OUT/'libsdktrace.so').read_bytes()).hexdigest(),
          'limit_bytes':128*1024*1024,'emergency_reserve_bytes':2*1024*1024,
          'object_allocation_coverage_audit':audit,
          'coverage':'all candidate VM translation-unit malloc/calloc/realloc/free/strdup/strndup calls; source snapshot retained',
          'limitations':['VM arena includes DEX/metadata/headers; not host process RSS or full ART heap partitions.',
                         'Host libc internal buffers are outside the arena.',
                         'Thread.start remains unsupported; allocator lock does not make DVM concurrency safe.',
                         'Compilation and embedded tests are not device verification.']}
(OUT/'candidate.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
