#!/usr/bin/env python3
"""从你自己的游戏 APK 中提取构建所需的 guest 素材（不修改 APK，不上传任何内容）。

用法:
    python tools/prepare_from_apk.py --apk D:\\path\\to\\your.apk

产物:
    native-probe/entry/src/main/resources/rawfile/sdkdex/base.apk   （APK 原样复制）
    native-probe/entry/src/main/resources/rawfile/sdkdex/classes*.dex
    native-probe/entry/libs/arm64-v8a/<游戏自带的 so>
    native-probe/entry/src/main/resources/base/media/app_icon.png   （从 APK 解析出的图标）
    host/AppScope/resources/base/media/app_icon.png

图标只在本机从你自己的 APK 解出，用于替换仓库里的占位图；不要把结果提交回仓库。
"""
import argparse
import pathlib
import shutil
import sys
import zipfile

import apk_icon

GAME_LIBS = [
    "libil2cpp.so",
    "libunity.so",
    "libmain.so",
    "libcri_ware_unity.so",
    "libcrypto_1_1.so",
    "libssl_1_1.so",
    "libblkv.so",
    "libcustom_rules.so",
]

ICON_TARGETS = [
    "native-probe/entry/src/main/resources/base/media/app_icon.png",
    "host/AppScope/resources/base/media/app_icon.png",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--apk", type=pathlib.Path, required=True, help="你自己的游戏 APK 路径"
    )
    parser.add_argument(
        "--repo", type=pathlib.Path, default=None, help="仓库根目录（默认自动推断）"
    )
    parser.add_argument("--abi", default="arm64-v8a", help="目标 ABI，默认 arm64-v8a")
    parser.add_argument(
        "--no-icon", action="store_true", help="不替换图标（保留仓库占位图）"
    )
    args = parser.parse_args()

    repo = args.repo or pathlib.Path(__file__).resolve().parent.parent
    apk = args.apk.resolve()
    if not apk.is_file():
        print("APK 不存在: %s" % apk, file=sys.stderr)
        return 2

    sdkdex = repo / "native-probe/entry/src/main/resources/rawfile/sdkdex"
    libs = repo / "native-probe/entry/libs" / args.abi
    sdkdex.mkdir(parents=True, exist_ok=True)
    libs.mkdir(parents=True, exist_ok=True)

    copied = []
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        dexes = [n for n in names if n.startswith("classes") and n.endswith(".dex")]
        if not dexes:
            print("APK 内没有 classes*.dex，可能不是标准 APK", file=sys.stderr)
            return 3
        for name in sorted(dexes):
            target = sdkdex / pathlib.Path(name).name
            with archive.open(name) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
            copied.append((name, target))

        for lib in GAME_LIBS:
            member = "lib/%s/%s" % (args.abi, lib)
            if member not in names:
                print("警告: APK 内缺少 %s（若游戏不使用可忽略）" % member, file=sys.stderr)
                continue
            target = libs / lib
            with archive.open(member) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
            copied.append((member, target))

    shutil.copy2(apk, sdkdex / "base.apk")
    copied.append((str(apk), sdkdex / "base.apk"))

    print("已准备 %d 个文件（这些文件已被 .gitignore 排除，不要提交）:" % len(copied))
    for source, target in copied:
        print("  %s -> %s" % (source, target.relative_to(repo)))

    if args.no_icon:
        print("跳过图标替换（--no-icon）")
    else:
        result = apk_icon.resolve_icon(apk)
        if not result:
            print("警告: 未能从 APK 解析出图标，保留仓库占位图", file=sys.stderr)
        else:
            entry, width, height = result
            for rel in ICON_TARGETS:
                out = repo / rel
                apk_icon.extract_icon(apk, out)
                print("  图标 %s (%dx%d) -> %s" % (entry, width, height, rel))
            print("提示: 图标来自你自己的 APK，仅用于本地构建；不要提交回仓库。")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
