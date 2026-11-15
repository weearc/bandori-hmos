import importlib.util, zipfile, re, json
from pathlib import Path
spec=importlib.util.spec_from_file_location('startup', Path(__file__).resolve().parent.parent/'inspect_game_startup.py')
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
apk=next(Path(__file__).resolve().parent.parent.glob('BanGDream*.apk'))
methods={}; classes={}; decode_errors=[]
with zipfile.ZipFile(apk) as z:
 for name in z.namelist():
  if re.fullmatch(r'classes\d*\.dex',name):
   d=mod.Dex(z.read(name))
   for cls, rec in d.classes.items():
    classes[cls]=rec
    for m in rec['methods']:
     try: calls=d.calls(m['code'])['calls']
     except Exception as e: calls=[]; decode_errors.append([m['name'],type(e).__name__])
     methods[m['name']]={'class':cls,'calls':[c['target'] for c in calls], 'native':bool(m['access']&256), 'dex':name}
root='Lcom/gsc/pub/GSCPubCommon;->login(Lcom/bsgamesdk/android/callbacklistener/CallbackListener;)V'
assert root in methods,'Actual original SDK login declaration not found'
from collections import deque
work=deque([(root,0)]);seen={};edges=[];external=set();ui=set();native=set()
while work and len(seen)<1500:
 current,depth=work.popleft()
 if current in seen:continue
 seen[current]=depth;item=methods.get(current)
 if not item:external.add(current);continue
 if item['native']:native.add(current)
 if depth>=6:continue
 for target in item['calls']:
  edges.append({'from':current,'to':target,'evidence':'direct DEX invoke; branch conditions not resolved'})
  if target.startswith(('Landroid/webkit/','Landroid/widget/','Landroid/app/','Landroid/view/')):ui.add(target)
  work.append((target,depth+1))
out=Path(__file__).resolve().parent
result={'root':root,'max_depth':6,'visited':len(seen),'queued_remaining':len(work),
 'nodes':seen,'edges':edges,'platform_ui_boundaries':sorted(ui),
 'native_boundaries':sorted(native),'unresolved':sorted(external),'decode_errors':decode_errors,
 'limits':['Static invoke graph only, not executed control flow.',
           'Interface/virtual dispatch, reflection, registered callbacks and queued runnables may hide additional UI paths.',
           'A missing UI method in this bounded graph does not prove it is unused.']}
(out/'route.json').write_text(json.dumps(result,indent=2),encoding='utf8')
text='# Original SDK login static route\n\nRoot: `'+root+'`\n\n'
text+='Visited '+str(len(seen))+' method signatures, '+str(len(edges))+' direct invoke edges. No game code executed.\n\n'
text+='## Observed UI call boundaries\n\n'+'\n'.join('- `'+x+'`' for x in sorted(ui))+'\n\n'
text+='## Native boundaries\n\n'+'\n'.join('- `'+x+'`' for x in sorted(native))+'\n\n'
text+='## Limits\n\n'+'\n'.join('- '+x for x in result['limits'])+'\n'
(out/'route.md').write_text(text,encoding='utf8')
print('Saved bounded original login call graph:',len(seen),'nodes;',len(ui),'UI boundaries;',len(native),'native boundaries')
for x in sorted(ui):print(x)
