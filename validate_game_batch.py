"""Collect independent SDK and Unity boundary reports in one device pass."""
import argparse
import json
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--sdk-marker', required=True)
    parser.add_argument('--unity-marker', required=True)
    args = parser.parse_args()
    folder = Path('batch-validation')
    folder.mkdir(exist_ok=True)
    cases = [
        ('sdk', 'Trace original SDK', 'sdk-trace.txt', args.sdk_marker),
        ('unity', 'Check Unity batch', 'unity-batch.txt', args.unity_marker),
    ]
    results = []
    for index, (name, button, remote, marker) in enumerate(cases):
        report = folder / (name + '-report.txt')
        command = [sys.executable, 'validate_probe_device.py', '--button', button,
                   '--report-file', remote, '--expect-report-marker', marker,
                   '--output', str(report)]
        if args.install and index == 0:
            command.append('--install')
        run = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180)
        (folder / (name + '-automation.txt')).write_text(run.stdout + run.stderr, encoding='utf-8')
        # A failed automation run must never reuse a previous report.
        lines = report.read_text(encoding='utf-8').splitlines() if run.returncode == 0 else []
        selected = [line for line in lines if line.startswith((
            'BUILD', 'BEGIN_STAGE', 'END_STAGE', 'BOUNDARY', 'RESULT',
            'MISSING_API', 'MISSING_FIELD', 'MISSING_STATIC', 'SUMMARY', 'LIBRARY'))]
        results.append({'case': name, 'automation_passed': run.returncode == 0,
                        'report': str(report) if run.returncode == 0 else None,
                        'boundaries': selected})
        print(name, 'report collected' if run.returncode == 0 else 'automation failed; see automation log')
    (folder / 'summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    return 0 if all(result['automation_passed'] for result in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
