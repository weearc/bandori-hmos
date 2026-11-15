#!/usr/bin/env python3
"""Resolve and extract the launcher icon from a user-supplied Android APK.

Pure Python (struct + zipfile only). It reads AndroidManifest.xml (binary XML)
and resources.arsc to find <application android:icon>, then extracts the
highest-density variant. No network, no full unpack, no execution.

Used by prepare_from_apk.py so the repository ships no bitmaps taken from the
game APK: the icon is regenerated locally from the user's own package.
"""
import struct
import zipfile
from pathlib import Path
from typing import Optional

ANDROID_ATTR_ICON = 0x01010002
TYPE_REFERENCE = 0x01
TYPE_STRING = 0x03
COMPLEX_FLAG = 0x0001


def _string_pool(data: bytes, start: int):
    kind, header, size = struct.unpack_from("<HHI", data, start)
    if kind != 0x0001:
        raise ValueError("not a string pool")
    count, _, flags, strings_off, _ = struct.unpack_from("<IIIII", data, start + 8)
    utf8 = bool(flags & 0x100)

    def read_len(pos):
        if utf8:
            value = data[pos]
            if value & 0x80:
                return ((value & 0x7F) << 8) | data[pos + 1], pos + 2
            return value, pos + 1
        value = struct.unpack_from("<H", data, pos)[0]
        if value & 0x8000:
            return ((value & 0x7FFF) << 16) | struct.unpack_from("<H", data, pos + 2)[0], pos + 4
        return value, pos + 2

    result = []
    for index in range(count):
        offset = struct.unpack_from("<I", data, start + header + index * 4)[0]
        chars, pos = read_len(start + strings_off + offset)
        nbytes, pos = read_len(pos) if utf8 else (chars * 2, pos)
        end = start + size
        result.append(data[pos:min(pos + nbytes, end)].decode("utf-8" if utf8 else "utf-16-le", "replace"))
    return result


def _chunks(data: bytes, start: int, end: int):
    pos = start
    while pos + 8 <= end:
        kind, header, size = struct.unpack_from("<HHI", data, pos)
        if size < 8 or pos + size > end:
            return
        yield kind, header, pos, size
        pos += size


def manifest_icon_ids(manifest: bytes):
    """Resource ids referenced by <application android:icon>."""
    if len(manifest) < 8:
        return []
    kind, header, size = struct.unpack_from("<HHI", manifest, 0)
    if kind != 0x0003:
        return []
    strings, resmap, found = [], [], []
    for kind, hdr, pos, _size in _chunks(manifest, header, len(manifest)):
        if kind == 0x0001:
            strings = _string_pool(manifest, pos)
        elif kind == 0x0180:
            count = (_size - hdr) // 4
            resmap = list(struct.unpack_from("<%dI" % count, manifest, pos + hdr))
        elif kind == 0x0102:
            body = pos + hdr
            _ns, name_idx = struct.unpack_from("<II", manifest, body)
            attr_start, attr_size, attr_count = struct.unpack_from("<HHH", manifest, body + 8)
            elem = strings[name_idx] if name_idx < len(strings) else ""
            if elem != "application":
                continue
            base = body + attr_start
            for i in range(attr_count):
                a = base + i * attr_size
                _a_ns, a_name, _a_raw = struct.unpack_from("<III", manifest, a)
                value_type = manifest[a + 15]
                value_data = struct.unpack_from("<i", manifest, a + 16)[0]
                attr_id = resmap[a_name] if a_name < len(resmap) else 0
                attr_name = strings[a_name] if a_name < len(strings) else ""
                if attr_id == ANDROID_ATTR_ICON or attr_name == "icon":
                    if value_type == TYPE_REFERENCE:
                        found.append(value_data & 0xFFFFFFFF)
    return found


def arsc_entries(res: bytes):
    """Yield (res_id, density, value_type, value_data) for simple values."""
    if len(res) < 8:
        return
    kind, header, total = struct.unpack_from("<HHI", res, 0)
    if kind != 0x0002:
        return
    global_strings, packages = [], []
    for kind, hdr, pos, size in _chunks(res, header, min(total, len(res))):
        if kind == 0x0001:
            global_strings = _string_pool(res, pos)
        elif kind == 0x0200:
            packages.append((pos, hdr, size))
    for pbase, phdr, psize in packages:
        package_id = struct.unpack_from("<I", res, pbase + 8)[0]
        type_off = struct.unpack_from("<I", res, pbase + 268)[0]
        for kind, hdr, pos, _size in _chunks(res, pbase + phdr, pbase + psize):
            if kind != 0x0201:
                continue
            type_id = res[pos + 8]
            entry_count = struct.unpack_from("<I", res, pos + 12)[0]
            entries_start = struct.unpack_from("<I", res, pos + 16)[0]
            cfg_size = struct.unpack_from("<I", res, pos + 20)[0]
            density = 0
            if cfg_size >= 16:
                density = struct.unpack_from("<H", res, pos + 20 + 14)[0]
            try:
                offsets = struct.unpack_from("<%dI" % entry_count, res, pos + hdr)
            except struct.error:
                continue
            ebase = pos + entries_start
            for index, entry_off in enumerate(offsets):
                if entry_off == 0xFFFFFFFF:
                    continue
                entry = ebase + entry_off
                if entry + 16 > len(res):
                    continue
                _esize, eflags, _ekey = struct.unpack_from("<HHI", res, entry)
                if eflags & COMPLEX_FLAG:
                    continue
                _vsize, _res0, vtype, vdata = struct.unpack_from("<HBBI", res, entry + 8)
                yield (package_id << 24) | (type_id << 16) | index, density, vtype, vdata, pos


def _png_size(data: bytes):
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    width, height = struct.unpack_from(">II", data, 16)
    return width, height


def resolve_icon(apk: Path) -> Optional[tuple]:
    """Return (zip_entry_name, width, height) for the best icon, or None."""
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        if "AndroidManifest.xml" not in names or "resources.arsc" not in names:
            return None
        manifest = archive.read("AndroidManifest.xml")
        res = archive.read("resources.arsc")

        wanted = set(manifest_icon_ids(manifest))
        candidates = {}
        strings_pool = None
        for kind, hdr, pos, size in _chunks(res, struct.unpack_from("<H", res, 2)[0], len(res)):
            if kind == 0x0001:
                strings_pool = _string_pool(res, pos)
                break
        for res_id, density, vtype, vdata, _pos in arsc_entries(res):
            if wanted and res_id not in wanted:
                continue
            if vtype != TYPE_STRING or strings_pool is None:
                continue
            if vdata >= len(strings_pool):
                continue
            path = strings_pool[vdata]
            if not path.lower().endswith(".png") or path not in names:
                continue
            candidates[path] = max(candidates.get(path, 0), density)

        if candidates:
            best = max(candidates.items(), key=lambda kv: (kv[1], archive.getinfo(kv[0]).file_size))
            data = archive.read(best[0])
            size = _png_size(data)
            if size:
                return best[0], size[0], size[1]

        # Fallback: largest square PNG under res/ (works for obfuscated tables).
        best = None
        for name in names:
            if not name.startswith("res/") or not name.lower().endswith(".png"):
                continue
            info = archive.getinfo(name)
            if info.file_size < 2048:
                continue
            data = archive.read(name)
            size = _png_size(data)
            if not size or size[0] != size[1] or size[0] < 96:
                continue
            if best is None or size[0] > best[1]:
                best = (name, size[0], size[1])
        if best:
            return best
    return None


def extract_icon(apk: Path, output: Path) -> Optional[tuple]:
    """Write the icon into output (PNG). Returns (entry, width, height)."""
    found = resolve_icon(apk)
    if not found:
        return None
    entry, width, height = found
    with zipfile.ZipFile(apk) as archive:
        data = archive.read(entry)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    return entry, width, height


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Resolve the launcher icon inside an APK")
    parser.add_argument("apk", type=Path)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()
    result = resolve_icon(args.apk)
    if not result:
        raise SystemExit("icon not found")
    print("entry=%s size=%dx%d" % result)
    if args.out:
        print(extract_icon(args.apk, args.out))
