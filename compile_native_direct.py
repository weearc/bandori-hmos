"""Compile the actual CMake-generated native commands without Ninja's pipes.

No compiler options are replaced, and no build/dependency cache is fabricated.
"""
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent
    folder = root / 'native-probe/entry/.cxx/default/default/debug/arm64-v8a'
    ninja = Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/build-tools/cmake/bin/ninja.exe')
    if not (folder / 'build.ninja').is_file():
        raise RuntimeError('Configure this project with DevEco/CMake before direct native compilation')
    result = subprocess.run([str(ninja), '-C', str(folder), '-t', 'commands'], capture_output=True,
                            text=True, check=True, timeout=20)
    commands = []
    for command in result.stdout.splitlines():
        if '--target=aarch64-linux-ohos' not in command:
            continue
        if 'cmd.exe /C' in command:
            command = command.split('&&', 1)[1].rsplit('&&', 1)[0].strip()
        commands.append(command)
    if len(commands) != 4:
        raise RuntimeError('Expected the two known probe targets, with four compile/link commands')
    for index, command in enumerate(commands, 1):
        result = subprocess.run(command, cwd=folder, capture_output=True, text=True, timeout=45)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print('Actual CMake native compile/link command', index, 'passed')


if __name__ == '__main__':
    main()
