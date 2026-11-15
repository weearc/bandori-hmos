#!/usr/bin/env python3
"""E2 acceptance checker: parse harness artifacts and print PASS/FAIL with numbers."""
import csv, glob, io, json, os, re, sys

def rows(path):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            return list(csv.DictReader(f))
    except Exception:
        return []

def num(v, d=0.0):
    try: return float(v)
    except Exception: return d

def summary(d):
    out = []
    gc = rows(os.path.join(d, 'vm-memory.csv'))
    if gc:
        denied = [num(r.get('arena_denied')) for r in gc]
        used = [num(r.get('arena_used')) for r in gc]
        peak = [num(r.get('arena_peak')) for r in gc]
        lim = num(gc[-1].get('arena_limit'))
        out.append(('arena_denied_last', denied[-1]))
        out.append(('arena_denied_max', max(denied)))
        out.append(('arena_used_last_MB', round(used[-1]/1048576, 2)))
        out.append(('arena_peak_MB', round(max(peak)/1048576, 2)))
        out.append(('arena_limit_MB', round(lim/1048576, 2)))
        if gc[0].get('objects'):
            out.append(('objects_last', gc[-1].get('objects')))
    gcf = rows(os.path.join(d, 'gc.csv'))
    if gcf:
        col = gcf[0].keys()
        for k in ('real_vm_collection_enabled', 'real_vm_moving_enabled'):
            if k in col:
                vals = set(r.get(k) for r in gcf)
                out.append((k, ','.join(sorted(vals))))
        if 'pause_ms' in col:
            vals = [num(r.get('pause_ms'), -1) for r in gcf if r.get('pause_ms') not in (None, '', 'NA')]
            if vals:
                vals.sort()
                out.append(('pause_p50', vals[len(vals)//2]))
                out.append(('pause_p99', vals[min(len(vals)-1, int(len(vals)*0.99))]))
                out.append(('pause_max', vals[-1]))
        if 'committed_bytes' in col:
            cs = [num(r.get('committed_bytes')) for r in gcf]
            out.append(('committed_last_MB', round(cs[-1]/1048576, 2)))
    return out

def scan_logs(d):
    hits = {}
    for p in glob.glob(os.path.join(d, '*.txt')) + glob.glob(os.path.join(d, '*.log')):
        try:
            t = open(p, 'r', encoding='utf-8', errors='replace').read()
        except Exception:
            continue
        for k, pat in (('oom_vsync', r'OutOfMemoryError:\s*VSync'),
                       ('oom_any', r'OutOfMemoryError'),
                       ('touchq_full', r'TOUCHQ.*full=1'),
                       ('touchq_drop', r'dropped=(\d+)'),
                       ('frame_wait50', r'frame_wait_50s')):
            m = re.findall(pat, t)
            if m:
                if k == 'touchq_drop':
                    hits[k] = max(hits.get(k, 0), max(int(x) for x in m))
                else:
                    hits[k] = hits.get(k, 0) + len(m)
    return hits

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    print('== dir:', d)
    for k, v in summary(d):
        print(f'  {k} = {v}')
    h = scan_logs(d)
    print('  log_hits =', json.dumps(h, ensure_ascii=False) if h else '{}')
    verdict = []
    if 'oom_vsync' in h: verdict.append('FAIL: VSync OOM present')
    if 'frame_wait50' in h: verdict.append('WARN: frame_wait_50s present')
    if h.get('touchq_drop', 0) > 0: verdict.append(f"WARN: touch dropped={h['touchq_drop']}")
    print('  verdict:', '; '.join(verdict) if verdict else 'no red flags found')

if __name__ == '__main__':
    main()
