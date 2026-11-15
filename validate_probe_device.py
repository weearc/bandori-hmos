"""Run the SDK diagnostic in the built probe HAP and retrieve its report.

Only operates this HAP's diagnostic buttons and sandbox; never presses audio.
"""
import argparse
import hashlib
import json
import re
import subprocess
import time
import zipfile
from pathlib import Path


HDC = Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe')
HAP = Path('native-probe/entry/build/default/outputs/default/entry-default-signed.hap')

# R2 原库必须是未 strip 的原始字节。默认期望值来自原 APK 条目解压后 SHA256；
# 可用 --manifest 的 native_libraries[*].source_sha256 或 --native-lib-hash 覆盖。
REQUIRED_NATIVE_LIBS = [
    'libcustom_rules.so',
    'libcrypto_1_1.so',
    'libssl_1_1.so',
]
OPTIONAL_NATIVE_LIBS = ['libblkv.so','libmain.so','libunity.so','libil2cpp.so']
DEFAULT_NATIVE_HASHES = {
    'libmain.so': '98660436f78418667bf865646428b61666d10cfc72642d42ea11454ffddeae76',
    'libunity.so': '25c511a3f769b875de870a6baaf789fa894f90079fc48446e0bfc3ce488c6e8a',
    'libil2cpp.so': '3f9886b904fd7cba01c38880229d2f301a108edf9ca68ad9753daa6283378a33',
    'libcustom_rules.so': '21107551854bf3c9fa42011e480cc9f7d4a6d90a8cbb40f57d1559e8f689e793',
    'libcrypto_1_1.so': '21230851fabb183eb2a28300701cabd8bef3739292a85ed07ef5539814949f12',
    'libssl_1_1.so': 'c4e3624d75df152a8a55ff0697e8b018f77a3c447c8dd6072f01f6220e1d5d33',
    'libblkv.so': '359270485cdd713125b103f0516eacd3d20099e4f7e0d9c58ff79e753034c2b2',
}


def native_entry(name: str) -> str:
    return 'libs/arm64-v8a/' + name


def load_native_hashes(manifest: Path | None, overrides: list[str]) -> dict[str, str]:
    """Expected HAP bytes per native library, keyed by HAP entry name.

    Precedence: built-in defaults, then manifest source_sha256, then CLI.
    """
    expected = {native_entry(name): digest for name, digest in DEFAULT_NATIVE_HASHES.items()}
    if manifest is not None:
        data = json.loads(manifest.read_text(encoding='utf-8'))
        for item in data.get('native_libraries', []):
            entry = item.get('hap_entry')
            digest = item.get('source_sha256')
            if entry and digest:
                expected[entry] = digest
    for item in overrides:
        name, separator, digest = item.partition('=')
        if not separator or not re.fullmatch(r'[a-fA-F0-9]{64}', digest):
            raise RuntimeError(f'invalid --native-lib-hash value: {item}')
        entry = name if '/' in name else native_entry(name)
        expected[entry] = digest.lower()
    return expected


def validate_native_libraries(hap: zipfile.ZipFile, expected: dict[str, str]) -> None:
    """Reject a HAP that lacks a required original library, carries an empty
    substitute, or stores bytes whose SHA256 differs from the original APK
    extraction (for example because hvigor stripped it)."""
    present = set(hap.namelist())
    checked = []
    for name in REQUIRED_NATIVE_LIBS:
        checked.append(native_entry(name))
    for name in OPTIONAL_NATIVE_LIBS:
        entry = native_entry(name)
        if entry in present:
            checked.append(entry)
    for entry in checked:
        if entry not in present:
            raise RuntimeError(f'HAP is missing required native library: {entry}')
        info = hap.getinfo(entry)
        if info.file_size == 0:
            raise RuntimeError(f'HAP native library is empty: {entry}')
        data = hap.read(entry)
        actual = hashlib.sha256(data).hexdigest()
        expected_hash = expected.get(entry)
        if not expected_hash:
            raise RuntimeError(f'no expected SHA256 configured for {entry}')
        if actual != expected_hash:
            raise RuntimeError(
                f'HAP native library bytes differ from the original APK: {entry} '
                f'({len(data)} bytes, sha256={actual}, expected={expected_hash}); '
                'hvigor likely stripped it, so disable nativeLib strip or restore raw bytes')
        print(f'NATIVE_LIB entry={entry} bytes={len(data)} sha256={actual} status=ok')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', help='Defaults to the single connected USB device')
    parser.add_argument('--install', action='store_true', help='Install the existing signed HAP first')
    parser.add_argument('--fresh-process', action='store_true', help='Force-stop before the initial launch')
    parser.add_argument('--no-retry', action='store_true', help='Do not restart or retrigger a pending diagnostic')
    parser.add_argument('--auto-start', action='store_true', help='Observe automatic main startup; never click a hidden diagnostic button')
    parser.add_argument('--expect-report-marker', default='SDK_TRACE')
    parser.add_argument('--button', default='Trace original SDK', help='Exact diagnostic button text in this probe')
    parser.add_argument('--report-file', default='sdk-trace.txt', help='Diagnostic report basename in filesDir')
    parser.add_argument('--output', type=Path, default=Path('sdk-runtime-spike/device-report-latest.txt'))
    parser.add_argument('--manifest', type=Path,
                        help='Device manifest whose native_libraries[*].source_sha256 are expected hashes')
    parser.add_argument('--native-lib-hash', action='append', default=[], metavar='NAME=SHA256',
                        help='Override expected SHA256 for one native library (repeatable)')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+\.(txt|json)', args.report_file):
        parser.error('--report-file must be a plain diagnostic .txt/.json basename')

    def command(parts, device=None, allow_failure=False):
        cmd = [str(HDC)] + (['-t', device] if device else []) + parts
        run = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=25)
        output = run.stdout + run.stderr
        if not allow_failure and (run.returncode or '[Fail]' in output or 'error: failed' in output.lower()):
            raise RuntimeError(output.strip())
        return output

    if not HDC.is_file():
        raise RuntimeError('Installed HDC not found')
    with zipfile.ZipFile(HAP) as hap:
        bundle = json.loads(hap.read('module.json'))['app']['bundleName']
        required = ['libs/arm64-v8a/libsdktrace.so'] + [
            'resources/rawfile/sdkdex/' + name for name in ('classes.dex', 'classes2.dex', 'classes3.dex')]
        if any(name not in hap.namelist() for name in required):
            raise RuntimeError('This HAP does not contain the SDK diagnostic and all original DEX files')
        # R2：原库必须以原字节存在；缺失、空文件或 hash 不符（如被 strip）一律失败。
        validate_native_libraries(hap, load_native_hashes(args.manifest, args.native_lib_hash))
    device = args.device
    if not device:
        rows = command(['list', 'targets', '-v']).splitlines()
        connected = [row.split()[0] for row in rows if 'USB' in row and 'Connected' in row]
        if len(connected) != 1:
            raise RuntimeError('Expected one connected USB device; use --device to select it')
        device = connected[0]
    if args.install:
        output = command(['install', '-r', str(HAP.resolve())], device)
        if 'success' not in output.lower():
            raise RuntimeError('Installation did not report success: ' + output)
    if args.install or args.fresh_process:
        command(['shell', 'aa', 'force-stop', bundle], device, allow_failure=True)
    if args.auto_start:
        # Cold-start auto mode must clear old reports before the app can write.
        remote = '/data/storage/el2/base/haps/entry/files/' + args.report_file
        remote_dir, remote_name = remote.rsplit('/', 1)
        for stale in [remote,remote[:-4]+'-roll.txt',remote_dir+'/sdkdex/'+remote_name[:-4]+'-roll.txt']:
            command(['shell','rm','-f',stale],device,allow_failure=True)
    command(['shell', 'aa', 'start', '-a', 'EntryAbility', '-b', bundle], device)
    launch_time = time.monotonic()
    time.sleep(0.5)
    workspace = Path('native-probe')
    layout = workspace / 'device-layout.json'

    def dump_layout():
        command(['shell', 'uitest', 'dumpLayout', '-p', '/data/local/tmp/native-probe-layout.json'], device)
        command(['file', 'recv', '/data/local/tmp/native-probe-layout.json', str(layout.resolve())], device)
        return json.loads(layout.read_text(encoding='utf-8'))

    def find_button(node):
        attributes = node.get('attributes', {})
        if attributes.get('text') == args.button:
            return attributes
        for child in node.get('children', []):
            result = find_button(child)
            if result:
                return result
        return None

    if not args.auto_start:
        attributes = None
        previous_bounds = None
        for _ in range(12):
            attributes = find_button(dump_layout())
            bounds = attributes.get('bounds') if attributes else None
            # The host's startup orientation test restores portrait after 1.5 s.
            # Require stable coordinates after it finishes before injecting a tap.
            if bounds and bounds == previous_bounds and time.monotonic() - launch_time >= 2.5:
                break
            previous_bounds = bounds
            time.sleep(0.5)
        else:
            attributes = None
        if not attributes:
            raise RuntimeError('SDK diagnostic button is not visible in the foreground probe; no click injected')
        coords = [int(n) for n in re.findall(r'\d+', attributes.get('bounds', ''))]
        if len(coords) != 4 or coords[2] <= coords[0] or coords[3] <= coords[1]:
            raise RuntimeError('Invalid button bounds; no click injected')
        # Reinstalling preserves filesDir. Replace only this diagnostic report so
        # an old successful download cannot be mistaken for a new execution.
        remote = '/data/storage/el2/base/haps/entry/files/' + args.report_file
        # A previous run's roll would otherwise satisfy the header-marker check
        # before this run has written anything.  Remove both known locations.
        remote_dir, remote_name = remote.rsplit('/', 1)
        stale_rolls = [
            remote[:-4] + '-roll.txt',
            remote_dir + '/sdkdex/' + remote_name[:-4] + '-roll.txt',
        ]
        for stale_roll in stale_rolls:
            command(['shell', 'rm', '-f', stale_roll], device, allow_failure=True)
        pending = workspace / 'sdk-trace-pending.txt'
        pending.write_text('PENDING SDK diagnostic ' + str(time.time_ns()) + '\n', encoding='utf-8')
        command(['file', 'send', '-b', bundle, str(pending.resolve()), remote], device)
        command(['shell', 'uitest', 'uiInput', 'click', str((coords[0]+coords[2])//2), str((coords[1]+coords[3])//2)], device)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = ''
    login_entry = args.button == 'Trace login entry'
    native_probe = args.button == 'Native library probe'
    elf_loader_probe = args.button == 'ELF loader probe'
    accepted_markers = {args.expect_report_marker}
    accepted_completion = {'RESULT '}
    if elf_loader_probe:
        accepted_markers.add('ELF_LOADER_PROBE')
        accepted_completion.add('ELF_LOADER_PROBE_RC=')
    if login_entry:
        # The login diagnostic has a dedicated header and emits batch/selftest
        # summaries instead of the generic RESULT line.  The final line is
        # written only after the selftests AND the original lifecycle finish;
        # early markers such as SELFTEST PASS or the header RESULT lines would
        # stop the capture before Application/Activity output exists.
        accepted_markers.add('SDK_LOGIN_TRACE')
        accepted_completion = {'RESULT login diagnostic stopped'}
    if native_probe:
        # The R2 report is one JSON document with probe="sdk-native-probe";
        # "libraries" proves the load/ELF section was written, not that any
        # dlopen succeeded.
        accepted_markers.add('sdk-native-probe')
        accepted_completion.add('"libraries"')
    # The runtime rolls the in-memory trace into a side file when the buffer
    # nears its cap, so the final report keeps its real ending instead of
    # being truncated.  The roll lives either next to the report or in the
    # sdkdex working directory the runtime was given; the header marker is in
    # the rolled prefix, so the capture must merge before deciding that the
    # expected marker is missing.
    remote_dir, remote_name = remote.rsplit('/', 1)
    roll_candidates = [
        remote[:-4] + '-roll.txt',
        remote_dir + '/sdkdex/' + remote_name[:-4] + '-roll.txt',
    ]
    roll_local = args.output.with_name(args.output.stem + '-roll.txt')
    timed_out_rolls = set()
    # The diagnostic writes `PENDING` before it starts; a stalled run leaves
    # exactly that text behind while the process keeps burning CPU.  After
    # half of the capture window, restart the app and click once more before
    # treating the run as failed.
    pending_retried = False
    watched_pids = set()
    missing_process_checks = 0
    if args.fresh_process and args.no_retry:
        pid_text = command(['shell', 'pidof', bundle], device, allow_failure=True).strip()
        if pid_text and all(part.isdigit() for part in pid_text.split()):
            watched_pids = set(pid_text.split())
    for step in range(600):
        time.sleep(0.25)
        output = command(['file', 'recv', '-b', bundle, remote, str(args.output.resolve())], device, allow_failure=True)
        report = ''
        if 'FileTransfer finish' in output and args.output.exists():
            report = args.output.read_text(encoding='utf-8', errors='replace')
            if report.startswith(('SDK trace entry unavailable:', 'SDK trace runtime unavailable:', 'SDK trace failed:')):
                # This is a fresh terminal host error, not a completed probe.
                # Preserve it and let the controller capture diagnostics now.
                raise RuntimeError('Host could not start diagnostic; saved terminal error in ' + str(args.output))
        roll_text = ''
        for roll_remote in roll_candidates:
            if roll_remote in timed_out_rolls:
                continue
            try:
                roll_output = command(['file', 'recv', '-b', bundle, roll_remote,
                                       str(roll_local.resolve())], device, allow_failure=True)
            except subprocess.TimeoutExpired:
                timed_out_rolls.add(roll_remote)
                print('DIAGNOSTIC_ROLL_TIMEOUT path=' + roll_remote + ' continuing_with_primary_report=1', flush=True)
                continue
            if 'FileTransfer finish' in roll_output and roll_local.is_file():
                text = roll_local.read_text(encoding='utf-8', errors='replace')
                if text.strip():
                    roll_text = text
                    break
        merged = (roll_text + ('' if roll_text.endswith('\n') else '\n') + report) if roll_text else report
        has_marker = bool(merged) and any(marker in merged for marker in accepted_markers)
        has_completion = bool(merged) and any(marker in merged for marker in accepted_completion)
        if (not args.no_retry and not pending_retried and step >= 300 and not has_completion
                and 'PENDING SDK diagnostic' in merged):
            pending_retried = True
            command(['shell', 'aa', 'force-stop', bundle], device, allow_failure=True)
            time.sleep(1.0)
            command(['shell', 'aa', 'start', '-a', 'EntryAbility', '-b', bundle], device, allow_failure=True)
            time.sleep(3.0)
            try:
                again = find_button(dump_layout())
                if again:
                    c = [int(n) for n in re.findall(r'\d+', again.get('bounds', ''))]
                    if len(c) == 4 and c[2] > c[0] and c[3] > c[1]:
                        command(['shell', 'uitest', 'uiInput', 'click',
                                 str((c[0] + c[2]) // 2), str((c[1] + c[3]) // 2)], device)
            except Exception:
                pass
            continue
        if has_marker and has_completion:
            report = merged
            if roll_text:
                args.output.write_text(report, encoding='utf-8')
            break
        if watched_pids and step % 20 == 0:
            pid_text = command(['shell', 'pidof', bundle], device, allow_failure=True).strip()
            if not pid_text:
                missing_process_checks += 1
            elif all(part.isdigit() for part in pid_text.split()):
                missing_process_checks = 0 if watched_pids.intersection(pid_text.split()) else missing_process_checks + 1
            else:
                # A transport error does not establish that the app exited.
                missing_process_checks = 0
            if missing_process_checks >= 2:
                raise RuntimeError(
                    'Diagnostic process exited before report completion; original pids='
                    + ','.join(sorted(watched_pids))
                    + '; preserve progress and native fault artifacts'
                )
    else:
        dump_layout()
        failure_layout = args.output.with_suffix('.failure-layout.json')
        failure_layout.write_bytes(layout.read_bytes())
        raise RuntimeError('SDK diagnostic report was not retrieved with the expected marker')
    dump_layout()
    command(['shell', 'snapshot_display', '-f', '/data/local/tmp/native-probe.jpeg'], device)
    command(['file', 'recv', '/data/local/tmp/native-probe.jpeg', str((workspace/'device-screen.jpeg').resolve())], device)
    print('Retrieved', args.output, 'from', bundle)
    if native_probe or elf_loader_probe:
        print(report)
    else:
        for line in report.splitlines():
            if line.startswith(('SELFTEST', 'BEGIN_STAGE', 'END_STAGE', 'MISSING_', 'EXCEPTION', 'RESULT', 'BUILD')):
                print(line)


if __name__ == '__main__':
    main()
