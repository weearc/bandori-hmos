"""Group actual Unity Java/JNI requirements already decoded from original DEX."""
import collections
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent
source = json.loads((root/'game-startup-analysis.json').read_text(encoding='utf-8'))
calls = collections.defaultdict(list)
natives = []
errors = []
for name, klass in source['classes'].items():
    if not name.startswith('Lcom/unity3d/player/'):
        continue
    for method in klass['methods']:
        if method['access'] & 0x100:
            natives.append({'method': method['name'], 'dex': klass['dex']})
        if 'decode_error' in method:
            errors.append({'method': method['name'], 'error': method['decode_error']})
        for call in method.get('calls', []):
            target = call['target']
            if target.startswith(('Landroid/', 'Ljava/', 'Ljavax/')):
                calls[target].append(method['name'])
groups = collections.Counter(target.split(';')[0] for target in calls)
result = {'source': 'game-startup-analysis.json: original APK DEX call sites',
          'scope': 'Static Unity Java classes only; no JNI invocation, Android service execution, or game startup',
          'native_methods': natives, 'external_methods': [
              {'target': target, 'callers': sorted(set(callers))} for target, callers in sorted(calls.items())],
          'external_classes': dict(groups.most_common()), 'decode_errors': errors}
path = root/'batch-validation/unity-java-requirements.json'
path.write_text(json.dumps(result, indent=2), encoding='utf-8')
print('Unity Java:',len(natives),'native methods;',len(calls),'external method signatures;',len(errors),'decode errors')
for klass, count in groups.most_common(12):
    print(klass,count)
print('Input JNI:',*[n['method'] for n in natives if any(word in n['method'].lower() for word in ['input','touch','inject','render','initjni'])],sep='\n')
