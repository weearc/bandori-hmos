"""Record pinned upstream synchronization boundaries and actual SDK callbacks."""
from pathlib import Path
import hashlib,json,re,sys,zipfile,shutil
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT.parent))
from inspect_game_startup import Dex
records=[]
for file,names in {'dvm.c':['dvm_gil_acquire','dvm_gil_release','dvm__tstate_reset','dvm__tstate_save','dvm__tstate_load'],
                   'dvm_runtime.c':['bytecode_thread_main','spawn_bytecode_thread','ht_run','ht_getLooper','looper_loop']}.items():
    raw=(ROOT.parent/'research/lunaria-source'/file).read_bytes();text=raw.decode('utf8')
    for name in names:
        matches=list(re.finditer(r'^(?:static )?[^\n;]*?\b'+name+r'\([^;]*?\)\s*\{',text,re.M|re.S))
        if not matches:raise ValueError(name)
        m=matches[-1]
        records.append({'file':file,'function':name,'source_line':text.count('\n',0,m.start())+1,'source_file_sha256':hashlib.sha256(raw).hexdigest()})
sdk=[]
with zipfile.ZipFile(next(ROOT.parent.glob('BanGDream*.apk'))) as z:
    for file in z.namelist():
        if not(file.startswith('classes') and file.endswith('.dex') and '/' not in file):continue
        dex=Dex(z.read(file))
        for klass in ['Lcom/base/trackingdata/mq/EventMessageLooper;','Lcom/base/trackingdata/mq/EventMessageLooper'+chr(36)+'1;']:
            for m in dex.classes.get(klass,{}).get('methods',[]):
                if any('->'+name+'(' in m['name'] for name in ['<init>','startLooper','handleMessage']):
                    sdk.append({'dex':file,'method':m['name'],'access':m['access'],'code_offset':m['code'],'calls':dex.calls(m['code'])['calls']})
doc={'commit':'8ed4b75368153f2b9d1174c986e50c8fe87aee59','license':'MPL-2.0','upstream_boundaries':records,'actual_original_sdk_paths':sdk,
     'adaptation':'Existing interpreter GIL/TLS primitives retained; standalone actual worker queue and lifecycle adapted without desktop ARM/JNI dependencies.'}
path=ROOT/'thread-runtime-provenance.json';path.write_text(json.dumps(doc,indent=2)+'\n');shutil.copy2(path,ROOT/'build/thread-next/source'/path.name)
print('THREAD_PROVENANCE verified_upstream_boundaries=',len(records),' original_SDK_methods=',len(sdk))
