"""Summarize observed original SDK boundaries, excluding negative selftests."""
import hashlib
import json
import re
from pathlib import Path

root=Path(__file__).resolve().parent
report=root.parent/'batch-validation/sdk-report.txt'
raw=report.read_bytes()
source=raw.decode('utf8')
marker=re.search(r'marker=([^ ]+)',source.splitlines()[0]).group(1)
stage=None
rows=[]
results=[]
for line in source.splitlines():
    if line.startswith('BEGIN_STAGE '):
        stage=line.split()[1]
    for match in re.finditer(r'MISSING_API ([^\s]+)',line):
        if stage:
            rows.append({'stage':stage,'api':match.group(1)})
    if 'END_STAGE ' in line:
        results.append(line[line.index('END_STAGE '):])
output={'marker':marker,'report':str(report),'report_sha256':hashlib.sha256(raw).hexdigest(),
        'unique_missing_apis':sorted({row['api'] for row in rows}),
        'occurrences':rows,'stage_results':results}
(root/('batch-missing-'+marker+'.json')).write_text(json.dumps(output,indent=2)+'\n')
print('OBSERVED_BATCH_MISSING unique=',len(output['unique_missing_apis']),'occurrences=',len(rows))
