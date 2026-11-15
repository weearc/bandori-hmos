import json
from pathlib import Path
p=Path('login-milestone-next')
r=json.loads(Path('compat-spec/sdk-initializer-resources.json').read_text(encoding='utf-8'))
assert r['source_apk_sha256']=='2ff2baf1b8fe78692b67507f88d3849422b672a0a929b374a878e1087d1519ec'
lines=['/* Generated from original APK resource extraction; do not log values. */']
for item in r['resources'].values(): lines.append('case %d: value=%s; break;' % (item['id'],json.dumps(item['value'])))
(p/'activity_resources.inc').write_text('\n'.join(lines)+'\n',encoding='utf-8')
