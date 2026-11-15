"""Build reviewed real-SQL/strict-executor/original-Activity diagnostic candidate.

Does not stage or install anything. Preserves all prior binary candidates.
"""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess

root = Path(__file__).resolve().parent
workspace = root.parent
out = root/'build/integrated-login-next'
src = out/'source'
shutil.copytree(root/'build/numeric-next/source', src, dirs_exist_ok=True)
for name in ['java_basics.c', 'java_basics.h', 'executor_runtime.c', 'collections.c', 'collections_test.c']:
    shutil.copy2(root/name, src/name)
for name in ['executor_runtime.c', 'executor_runtime.h', 'thread_runtime.c', 'thread_runtime.h']:
    shutil.copy2(workspace/'scheduled-runtime-next'/name, src/name)
tests = src/'java_basics_test.c'
tests.write_text('#include "thread_runtime.h"\n'+tests.read_text(encoding='utf-8'), encoding='utf-8')
shutil.copy2(workspace/'login-milestone-next/strict_runtime_login.c', src/'strict_runtime.c')
for name in ['sqlite_runtime.c', 'sqlite_runtime.h']:
    shutil.copy2(workspace/'sqlite-backend-next/src'/name, src/name)
for name in ['activity_runtime.c', 'activity_runtime.h', 'activity_resources.inc']:
    shutil.copy2(workspace/'login-milestone-next'/name, src/name)
for name in ['java_log_runtime.c', 'java_log_runtime.h']:
    shutil.copy2(workspace/'java-log-next'/name, src/name)
for name in ['enum_runtime.c', 'enum_runtime.h', 'foundation_test.c']:
    shutil.copy2(workspace/'enum-runtime-next'/name, src/name)
for name in ['throwable_reference.c', 'throwable_reference.h']:
    shutil.copy2(workspace/'throwable-reference-next'/name, src/name)
shutil.copy2(workspace/'package-metadata-next/reviewed_metadata.c', src/'package_metadata.c')
for name in ['package_metadata.h', 'metadata_values.inc']:
    shutil.copy2(workspace/'package-metadata-next'/name, src/name)
# assets.inc is derived from the user's APK and never committed
if not (workspace / 'asset-reader-next/assets.inc').is_file():
    import subprocess as _sp
    _apk = os.environ.get('BG_APK')
    if not _apk:
        raise SystemExit('assets.inc missing: run "python asset-reader-next/generate.py --apk <your.apk>"')
    _sp.run([__import__('sys').executable, str(workspace / 'asset-reader-next/generate.py'), '--apk', _apk], check=True)
for name in ['asset_reader.c', 'asset_reader.h', 'assets.inc']:
    shutil.copy2(workspace/'asset-reader-next'/name, src/name)
for directory, names in [
    ('string-runtime-next', ['string_runtime.c', 'string_runtime.h']),
    ('json-runtime-next', ['json_runtime.c', 'json_runtime.h']),
    ('file-directory-next', ['file_runtime.c', 'file_runtime.h']),
    ('preferences-runtime-next', ['prefs_runtime.c', 'prefs_runtime.h']),
    ('reflection-runtime-next', ['reflection_runtime.c', 'reflection_runtime.h']),
    ('array-runtime-next', ['array_runtime.c', 'array_runtime.h']),
    ('list-views-next', ['list_views.c', 'list_views.h']),
    ('platform-runtime-next', ['platform_runtime.c', 'platform_runtime.h']),
    ('core-object-next', ['object_runtime.c', 'object_runtime.h']),
    ('network-runtime-next', ['proxy_runtime.c', 'proxy_runtime.h', 'socket_runtime.c', 'socket_runtime.h']),
    ('set-runtime-next', ['set_runtime.c', 'set_runtime.h']),
    ('tls-trust-next', ['trust.c', 'trust.h', 'fixture_selftest.c', 'fixture_selftest.h', 'fixture_der.h']),
    ('cert-objects-next', ['cert_objects.c', 'cert_objects.h', 'cert_selftest.c', 'cert_selftest.h', 'expected_spki.h']),
]:
    for name in names:
        shutil.copy2(workspace/directory/name, src/name)
tests.write_text(tests.read_text(encoding='utf-8').replace(
    'if(!executor_runtime_selftest(v,&checks))okay=false;',
    'if(!executor_runtime_selftest(v,&checks))okay=false;if(!foundation_next_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('extern bool foundation_next_selftest(struct dvm*);\n'+tests.read_text(encoding='utf-8'), encoding='utf-8')
tests.write_text('#include "throwable_reference.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!foundation_next_selftest(v))okay=false;',
    'if(!foundation_next_selftest(v))okay=false;if(!throwable_reference_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('extern bool package_metadata_selftest(struct dvm*);\n'+tests.read_text(encoding='utf-8').replace(
    'if(!throwable_reference_selftest(v))okay=false;',
    'if(!throwable_reference_selftest(v))okay=false;if(!package_metadata_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('extern bool asset_reader_selftest(struct dvm*);\n'+tests.read_text(encoding='utf-8').replace(
    'if(!package_metadata_selftest(v))okay=false;',
    'if(!package_metadata_selftest(v))okay=false;if(!asset_reader_selftest(v))okay=false;'), encoding='utf-8')
shutil.copy2(workspace/'sqlite-backend-next/vendor/sqlite3.h', src/'sqlite3.h')
tests.write_text('#include "string_runtime.h"\n#include "json_runtime.h"\n#include "file_runtime.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!asset_reader_selftest(v))okay=false;',
    'if(!asset_reader_selftest(v))okay=false;if(!string_runtime_selftest(v))okay=false;if(!json_runtime_selftest(v))okay=false;if(!file_runtime_selftest(v))okay=false;'), encoding='utf-8')

android = src/'android_basics.c'
text = android.read_text(encoding='utf-8')
text = re.sub(r'^struct sqlite_(helper|database|cursor)\{[^\n]*\};\n', '', text, flags=re.M)
start = text.index('static bool sqlite_contains_ci(')
end = text.index('bool android_basics_call(', start)
text = text[:start] + text[end:]
start = text.index(' if(!strcmp(c,"android/database/sqlite/SQLiteOpenHelper"))')
end = text.rindex(' return false;')
text = text[:start] + text[end:]
text = '#include "sqlite_runtime.h"\n' + text
signature = 'bool android_basics_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){'
assert signature in text
text = text.replace(signature, signature+'\n if(sqlite_runtime_call(v,c,m,sig,self,a,out))return true;', 1)
assert all(name not in text for name in ['sqlite_contains_ci', 'logical_count', 'sqlite_cursor_call'])
android.write_text(text, encoding='utf-8')
strict = src/'strict_runtime.c'
strict_text = '#include "string_runtime.h"\n#include "json_runtime.h"\n#include "file_runtime.h"\n#include "asset_reader.h"\n#include "java_log_runtime.h"\n#include "enum_runtime.h"\n#include "throwable_reference.h"\n#include "package_metadata.h"\n'+strict.read_text(encoding='utf-8')
strict_text = strict_text.replace('if(activity_runtime_call(', 'if(asset_reader_call(vm,c,m,s,self,a,n,out)||package_metadata_call(vm,c,m,s,self,a,n,out)||throwable_reference_call(vm,c,m,s,self,a,n,out)||enum_runtime_call(vm,c,m,s,self,a,n,out)||java_log_runtime_call(vm,c,m,s,self,a,n,out)||activity_runtime_call(', 1)
strict_text = strict_text.replace('void dvm_runtime_install(struct dvm *v) {', 'void dvm_runtime_install(struct dvm *v) {asset_reader_vm_start(v);if(!package_metadata_vm_start(v)){dvm__throw(v,"java/lang/IllegalStateException","metadata bootstrap failed");return;}throwable_reference_vm_start(v);', 1)
strict_text = strict_text.replace('if(asset_reader_call(', 'if(string_runtime_call(vm,c,m,s,self,a,n,out)||json_runtime_call(vm,c,m,s,self,a,n,out)||file_runtime_call(vm,c,m,s,self,a,n,out)||asset_reader_call(', 1)
strict_text = strict_text.replace('throwable_reference_vm_start(v);', 'throwable_reference_vm_start(v);json_runtime_vm_start(v);file_runtime_vm_start(v);', 1)
strict_text = strict_text.replace('if(!guest_profile_load(dir)', 'if(!file_runtime_configure(dir)||!guest_profile_load(dir)')
strict_text = re.sub(r'SDK_TRACE marker=[^ ]+', 'SDK_TRACE marker=integrated-login-v21', strict_text)
strict_text = strict_text.replace('SDK_LOGIN_TRACE original-', 'SDK_LOGIN_TRACE candidate=integrated-login-v21 original-')
strict_text = strict_text.replace('done:\n memcpy(returned_trace',
    'done:\n sdk_trace_fprintf(stderr,"RESULT login diagnostic stopped; inspect BLOCKED/EXCEPTION; login unverified\\n");\n memcpy(returned_trace', 1)
strict_text = '#include "prefs_runtime.h"\n'+strict_text
strict_text = strict_text.replace('if(string_runtime_call(', 'if(prefs_runtime_call(vm,c,m,s,self,a,n,out)||string_runtime_call(', 1)
strict_text = strict_text.replace('file_runtime_vm_start(v);', 'file_runtime_vm_start(v);prefs_runtime_vm_start(v);', 1)
strict_text = strict_text.replace('if(!file_runtime_configure(dir)', 'if(!prefs_runtime_configure(dir)||!file_runtime_configure(dir)')
strict_text = strict_text.replace('integrated-login-v21', 'integrated-login-v23')
strict_text = '#include "reflection_runtime.h"\n'+strict_text
strict_text = strict_text.replace('if(prefs_runtime_call(', 'if(reflection_runtime_call(vm,c,m,s,self,a,n,out)||prefs_runtime_call(', 1)
strict_text = strict_text.replace('prefs_runtime_vm_start(v);', 'prefs_runtime_vm_start(v);reflection_runtime_vm_start(v);', 1)
strict_text = strict_text.replace('integrated-login-v23', 'integrated-login-v24')
strict_text = '#include "array_runtime.h"\n'+strict_text
strict_text = strict_text.replace('if(reflection_runtime_call(', 'if(array_runtime_call(vm,c,m,s,self,a,n,out)||reflection_runtime_call(', 1)
strict_text = strict_text.replace('integrated-login-v24', 'integrated-login-v25')
strict_text = '#include "list_views.h"\n#include "platform_runtime.h"\n'+strict_text
strict_text = strict_text.replace('if(array_runtime_call(', 'if(list_views_call(vm,c,m,s,self,a,n,out)||platform_runtime_call(vm,c,m,s,self,a,n,out)||array_runtime_call(', 1)
strict_text = strict_text.replace('collections_vm_start(v);', 'collections_vm_start(v);list_views_vm_start(v);platform_runtime_vm_start(v);', 1)
strict_text = strict_text.replace('bool dvm_platform_class_exists(const char *d) {', 'bool dvm_platform_class_exists(const char *d) {if(!strcmp(d,"Ljava/util/Arrays$ArrayList;")||!strcmp(d,"Ljava/util/Collections$UnmodifiableList;")||!strcmp(d,"Ljava/util/Arrays$ArrayList$Itr;")||!strcmp(d,"Ljava/util/Collections$UnmodifiableList$Itr;"))return true;', 1)
strict_text = strict_text.replace('integrated-login-v25', 'integrated-login-v26')
strict_text = '#include "object_runtime.h"\n'+strict_text
strict_text = strict_text.replace('if(list_views_call(', 'if(object_runtime_call(vm,c,m,s,self,a,n,out)||list_views_call(', 1)
strict_text = strict_text.replace('integrated-login-v26', 'integrated-login-v27')
strict_text = '#include "proxy_runtime.h"\n#include "socket_runtime.h"\n'+strict_text
strict_text = strict_text.replace('if(object_runtime_call(', 'if(proxy_runtime_call(vm,c,m,s,self,a,n,out)||socket_runtime_call(vm,c,m,s,self,a,n,out)||object_runtime_call(', 1)
strict_text = strict_text.replace('platform_runtime_vm_start(v);', 'platform_runtime_vm_start(v);socket_runtime_vm_start(v);', 1)
strict_text = strict_text.replace('integrated-login-v27', 'integrated-login-v29')
strict_text = '#include "set_runtime.h"\n'+strict_text
strict_text = strict_text.replace('if(proxy_runtime_call(', 'if(set_runtime_call(vm,c,m,s,self,a,n,out)||proxy_runtime_call(', 1)
strict_text = strict_text.replace('collections_vm_start(v);', 'collections_vm_start(v);set_runtime_vm_start(v);', 1)
strict_text = '#include <stdio.h>\n#include "fixture_selftest.h"\nint sdk_trace_fprintf(FILE*,const char*,...);\nstatic void certificate_batch_log(const char *line,void *ctx){(void)ctx;sdk_trace_fprintf(stderr,"CERTIFICATE_BATCH %s\\n",line);}\nstatic void certificate_batch_run(void){int status=tls_trust_selftest(certificate_batch_log,NULL);sdk_trace_fprintf(stderr,"CERTIFICATE_BATCH_RESULT status=%d SDK_TLS_integration=not_provided\\n",status);}\n'+strict_text
strict_text, count = re.subn(r'( sdk_trace_fprintf\(stderr,"SDK_LOGIN_TRACE[^\n]*\n)', r'\1 certificate_batch_run();\n', strict_text, count=1)
assert count == 1
strict_text = strict_text.replace('integrated-login-v29', 'integrated-login-v30')
strict_text = '#include "cert_selftest.h"\n'+strict_text
strict_text = strict_text.replace('int status=tls_trust_selftest(', 'int crypto_status=cert_objects_selftest();sdk_trace_fprintf(stderr,"CERT_OBJECTS_RESULT status=%d DER_SPKI_hash_fixture=1 Java_pin_integration=not_provided\\n",crypto_status);int status=tls_trust_selftest(', 1)
strict.write_text(strict_text, encoding='utf-8')
engine = src/'generated/dvm/dvm.c'
engine_text = engine.read_text(encoding='utf-8')
engine_text = '#include "reflection_runtime.h"\n' + engine_text
needle = '   dvm_alarm_finish(vm);'
assert needle in engine_text
engine_text = engine_text.replace(needle, needle+'\n   reflection_runtime_vm_end(vm);', 1)
engine.write_text(engine_text, encoding='utf-8')
tests.write_text('#include "prefs_runtime.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!file_runtime_selftest(v))okay=false;',
    'if(!file_runtime_selftest(v))okay=false;if(!prefs_runtime_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('#include "reflection_runtime.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!prefs_runtime_selftest(v))okay=false;',
    'if(!prefs_runtime_selftest(v))okay=false;if(!reflection_runtime_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('#include "array_runtime.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!reflection_runtime_selftest(v))okay=false;',
    'if(!reflection_runtime_selftest(v))okay=false;if(!array_runtime_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('#include "list_views.h"\n#include "platform_runtime.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!array_runtime_selftest(v))okay=false;',
    'if(!array_runtime_selftest(v))okay=false;if(!list_views_selftest(v))okay=false;if(!platform_runtime_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('#include "object_runtime.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!platform_runtime_selftest(v))okay=false;',
    'if(!platform_runtime_selftest(v))okay=false;if(!object_runtime_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('#include "socket_runtime.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!object_runtime_selftest(v))okay=false;',
    'if(!object_runtime_selftest(v))okay=false;if(!socket_runtime_selftest(v))okay=false;'), encoding='utf-8')
tests.write_text('#include "set_runtime.h"\n'+tests.read_text(encoding='utf-8').replace(
    'if(!socket_runtime_selftest(v))okay=false;',
    'if(!socket_runtime_selftest(v))okay=false;if(!set_runtime_selftest(v))okay=false;'), encoding='utf-8')

sdk = Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native')
compiler = sdk/'llvm/bin/clang.exe'
common = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot='+str(sdk/'sysroot'),
          '-std=gnu11', '-fPIC', '-O1', '-I'+str(src), '-I'+str(src/'generated'),
          '-I'+str(workspace/'tls-transport-next/vendor/mbedtls-3.6.7/include')]
names = ['generated/dvm/dvm.c', 'generated/dvm/dex.c', 'strict_runtime.c',
         'java_basics.c', 'java_basics_test.c', 'android_basics.c', 'android_basics_test.c',
         'guest_profile.c', 'collections.c', 'collections_test.c', 'next_arena.c',
         'thread_runtime.c', 'thread_runtime_test.c', 'executor_runtime.c',
         'numeric_runtime.c', 'numeric_runtime_test.c', 'sqlite_runtime.c', 'activity_runtime.c', 'java_log_runtime.c',
         'enum_runtime.c', 'foundation_test.c', 'throwable_reference.c', 'package_metadata.c', 'asset_reader.c',
         'string_runtime.c', 'json_runtime.c', 'file_runtime.c', 'prefs_runtime.c', 'reflection_runtime.c', 'array_runtime.c', 'list_views.c', 'platform_runtime.c', 'object_runtime.c', 'proxy_runtime.c', 'socket_runtime.c', 'set_runtime.c', 'trust.c', 'fixture_selftest.c', 'cert_objects.c', 'cert_selftest.c']
objects = []
for name in names:
    obj = out/(Path(name).stem+'.o')
    flags = common + ([] if name=='next_arena.c' else ['-include', str(src/'next_interpose.h')])
    subprocess.run(flags+['-c', str(src/name), '-o', str(obj)], check=True)
    objects.append(str(obj))

audit = []
for name, obj in zip(names, objects):
    if name=='next_arena.c': continue
    undefined = subprocess.check_output([str(sdk/'llvm/bin/llvm-nm.exe'), '--undefined-only', obj], text=True)
    bypass = {'malloc','calloc','realloc','free','strdup','strndup'}.intersection(
        line.split()[-1] for line in undefined.splitlines() if line.split())
    assert not bypass, (name, bypass)
    audit.append(name)
sqlite = workspace/'sqlite-backend-next/build/libgame_sqlite.so'
assert sqlite.is_file()
shutil.copy2(sqlite, out/sqlite.name)
library = out/'libsdktrace.so'
subprocess.run(common+['-shared', *objects, '-Wl,--no-undefined', '-L'+str(out),
                      '-L'+str(workspace/'tls-transport-next/direct-build'), '-lmbedx509', '-lmbedcrypto',
                      '-lgame_sqlite', '-lnet_connection', '-lhilog_ndk.z', '-ldl', '-lm', '-o', str(library)], check=True)
manifest = {'candidate':'integrated-login-v30', 'sha256':hashlib.sha256(library.read_bytes()).hexdigest(),
    'bytes':library.stat().st_size, 'sqlite_sha256':hashlib.sha256(sqlite.read_bytes()).hexdigest(),
    'allocation_audit':audit, 'status':'compiled only; not staged or device tested',
    'limits':['SQLite is real in-memory SQL, not persistent Android storage',
              'executor factories limited to NEW plain Thread with supplied worker target; original DEX factory unverified',
              'scheduled Runnable one-shot only; periodic and Callable scheduling unsupported',
              'SharedPreferences String subset is persistent; apply writes synchronously; typed values/listeners unsupported',
              'original Activity constructor/initialize only; attach/lifecycle/network unavailable',
              'native SQLite internal heap is outside the managed VM arena']}
(out/'candidate.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
print(json.dumps(manifest, indent=2))
