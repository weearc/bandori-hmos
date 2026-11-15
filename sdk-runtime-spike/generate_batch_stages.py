"""Verify every batch entry against original DEX before emitting C descriptors."""
from pathlib import Path
import hashlib
import json
import sys
import zipfile
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT.parent))
from inspect_game_startup import Dex
STAGES=[
 ('applicationAttach','com/gsc/pub/GSCPubCommon','applicationAttach','(Landroid/app/Application;)V',1),
 ('getInstance','com/gsc/pub/GSCPubCommon','getInstance','()Lcom/gsc/pub/GSCPubCommon;',0),
 ('Track.getInstance','com/base/trackingdata/Track','getInstance','(Landroid/app/Application;)Lcom/base/trackingdata/Track;',1),
 ('TrackId.buildTrackId','com/base/trackingdata/utils/TrackId','buildTrackId','()V',3),
 ('EventMessageLooper.getInstance','com/base/trackingdata/mq/EventMessageLooper','getInstance','()Lcom/base/trackingdata/mq/EventMessageLooper;',0),
 ('TrackConfig.constructor','com/base/trackingdata/config/TrackConfig','<init>','()V',2),
 ('CacheInstaller.getInstance','com/base/cachelib/CacheInstaller','getInstance','()Lcom/base/cachelib/CacheInstaller;',0),
 ('GlobeContext.setGlobeContext','com/base/trackingdata/constants/GlobeContext','setGlobeContext','(Landroid/content/Context;)V',1),
]
def generate():
 apk=next(ROOT.parent.glob('BanGDream*.apk'));found={}
 with zipfile.ZipFile(apk) as z:
  for name in z.namelist():
   if name.startswith('classes') and name.endswith('.dex') and '/' not in name:
    data=z.read(name);dex=Dex(data)
    for label,klass,method,sig,mode in STAGES:
     full='L'+klass+';->'+method+sig
     for m in dex.classes.get('L'+klass+';',{}).get('methods',[]):
      if m['name']==full:
       assert bool(m['access']&8)==(mode!=2),full
       assert m['code'],full
       found[label]={'label':label,'class':klass,'method':method,'signature':sig,'mode':mode,
                     'dex':name,'dex_sha256':hashlib.sha256(data).hexdigest(),'access':m['access'],
                     'code_offset':m['code'],'calls':dex.calls(m['code'])['calls']}
 assert len(found)==len(STAGES),'Missing original DEX stage: '+str(set(s[0] for s in STAGES)-found.keys())
 output='/* Generated from original DEX; see batch-stages.json. */\nstatic const struct sdk_stage sdk_stages[]={\n'
 for label,klass,method,sig,mode in STAGES:
  output+=' {'+','.join(json.dumps(s) for s in [label,klass,method,sig])+','+str(mode)+'},\n'
 output+='};\n'
 (ROOT/'generated/sdk_stages.h').write_text(output,encoding='utf8')
 (ROOT/'batch-stages.json').write_text(json.dumps({'marker':'batch-context-v6','scope':'Independent original methods, not a reconstructed SDK startup sequence.',
  'modes':{'0':'static no args','1':'static with VM Application','2':'original constructor on new object','3':'original GlobeContext.setGlobeContext then static no args'},'stages':[found[s[0]] for s in STAGES]},indent=2)+'\n')
 print('Verified original DEX batch stages:',len(found))
if __name__=='__main__':generate()
