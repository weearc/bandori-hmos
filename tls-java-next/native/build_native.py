#!/usr/bin/env python3
"""Directly cross-compile Mbed TLS 3.6.7 and the native TLS client."""

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys


NATIVE = pathlib.Path(__file__).resolve().parent
REPO = NATIVE.parent.parent
DEFAULT_SOURCE = REPO / "tls-transport-next" / "vendor" / "mbedtls-3.6.7"
DEFAULT_SDK = pathlib.Path(
    "C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native"
)


def run(command, log):
    command = [str(item) for item in command]
    log.write("$ " + subprocess.list2cmdline(command) + "\n")
    log.flush()
    result = subprocess.run(command, text=True, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"command failed with exit code {result.returncode}: {command[0]}")


def source_list(cmake_text, group):
    match = re.search(r"set\(src_" + group + r"\s+(.*?)\)", cmake_text, re.S)
    if not match:
        raise RuntimeError(f"could not find src_{group} in library/CMakeLists.txt")
    return match.group(1).split()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=DEFAULT_SOURCE)
    parser.add_argument("--sdk", type=pathlib.Path, default=DEFAULT_SDK)
    parser.add_argument("--archives-only", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    sdk = args.sdk.resolve()
    clang = sdk / "llvm" / "bin" / "clang.exe"
    llvm_ar = sdk / "llvm" / "bin" / "llvm-ar.exe"
    sysroot = sdk / "sysroot"
    output = NATIVE / "build"
    objects = output / "obj"
    output.mkdir(parents=True, exist_ok=True)
    objects.mkdir(parents=True, exist_ok=True)

    if not clang.is_file() or not llvm_ar.is_file() or not sysroot.is_dir():
        raise RuntimeError(f"HarmonyOS native SDK not found: {sdk}")
    if not (source / "include" / "mbedtls" / "build_info.h").is_file():
        raise RuntimeError(f"Mbed TLS source not found: {source}")

    log_path = output / "build.log"
    config_define = '-DMBEDTLS_USER_CONFIG_FILE="tls_mbedtls_user_config.h"'
    common = [
        clang,
        "--target=aarch64-linux-ohos",
        "--sysroot=" + str(sysroot),
        "-fPIC",
        "-O2",
        "-D__MUSL__",
        config_define,
        "-pthread",
        "-I" + str(NATIVE),
        "-I" + str(source / "include"),
        "-I" + str(source / "library"),
    ]

    with log_path.open("w", encoding="utf-8") as log:
        log.write(f"source={source}\nsdk={sdk}\n")
        log.write("config=tls_mbedtls_user_config.h (MBEDTLS_THREADING_C, MBEDTLS_THREADING_PTHREAD)\n")
        cmake_text = (source / "library" / "CMakeLists.txt").read_text(encoding="utf-8")
        for group, library in (
            ("crypto", "mbedcrypto"),
            ("x509", "mbedx509"),
            ("tls", "mbedtls"),
        ):
            members = []
            for source_name in source_list(cmake_text, group):
                source_file = source / "library" / source_name
                object_file = objects / f"{group}_{source_name}.o"
                run(common + ["-c", source_file, "-o", object_file], log)
                members.append(object_file)

            archive = output / f"lib{library}.a"
            temporary = output / f"lib{library}.a.new"
            if temporary.exists():
                temporary.unlink()
            run([llvm_ar, "rcs", temporary] + members, log)
            temporary.replace(archive)
            print(f"{archive.name} sha256={sha256(archive)}", flush=True)

        if not args.archives_only:
            client_object = objects / "tls_client.o"
            run(common + ["-c", NATIVE / "tls_client.c", "-o", client_object], log)
            shared = output / "libtls_native_check.so"
            run(
                common
                + [
                    "-shared",
                    "-Wl,--no-undefined",
                    "-o",
                    shared,
                    client_object,
                    "-Wl,--whole-archive",
                    output / "libmbedtls.a",
                    output / "libmbedx509.a",
                    output / "libmbedcrypto.a",
                    "-Wl,--no-whole-archive",
                    "-pthread",
                ],
                log,
            )
            print(f"{shared.name} sha256={sha256(shared)}", flush=True)

    print(f"build log: {log_path}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(f"build_native.py: {error}", file=sys.stderr)
        sys.exit(1)
