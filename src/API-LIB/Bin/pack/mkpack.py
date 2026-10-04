#!/usr/bin/env python3
"""
mkpack.py - 把目录打成 PoserOS 安装包（.pack）

    usage: mkpack.py <srcdir> <out.pack>

srcdir 里必须有 PackageMain.ini：
    [header]
    sys=PoserOS
    arch=x86
    [info]
    name=...
    version=...
    author=...
    [starting]
    ready=/ReadyLoader.dwn
    type=DBC
    main=/pack/main.dwn

归档内路径用 '/' 分隔、不带前导 '/'；目录也写进 TOC（type=1），
方便运行时按序建目录再落文件。格式见 API-LIB/Bin/pack/pack.h。
"""

import os
import struct
import sys

MAGIC = b"PACK"
VERSION = 1
HDR_SIZE = 32
ENT_SIZE = 64
NAME_MAX = 52
CHK_OFF = 28


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def collect(src: str):
    """返回 [(relpath, is_dir)]，目录在前、按路径排序。"""
    dirs, files = [], []
    for root, dnames, fnames in os.walk(src):
        rel_root = os.path.relpath(root, src)
        rel_root = "" if rel_root == "." else rel_root.replace(os.sep, "/")
        if rel_root:
            dirs.append(rel_root)
        for fn in fnames:
            p = os.path.join(root, fn)
            if os.path.islink(p):
                continue
            rel = (rel_root + "/" + fn) if rel_root else fn
            files.append(rel.replace(os.sep, "/"))
    dirs.sort()
    files.sort()
    return [(d, True) for d in dirs] + [(f, False) for f in files]


def build(src: str, out: str):
    ini = os.path.join(src, "PackageMain.ini")
    if not os.path.isfile(ini):
        raise SystemExit("mkpack: %s not found" % ini)

    entries = collect(src)
    for rel, _ in entries:
        if len(rel) >= NAME_MAX:
            raise SystemExit("mkpack: name too long (%d>%d): %s"
                             % (len(rel), NAME_MAX - 1, rel))

    count = len(entries)
    toc_off = HDR_SIZE
    data_off = toc_off + count * ENT_SIZE

    toc = bytearray()
    data = bytearray()
    pos = data_off
    for rel, is_dir in entries:
        name = rel.encode()[: NAME_MAX - 1]
        ent = bytearray(ENT_SIZE)
        ent[0:len(name)] = name
        if is_dir:
            struct.pack_into("<I", ent, 52, pos)
            struct.pack_into("<I", ent, 56, 0)
            struct.pack_into("<I", ent, 60, 1)
        else:
            with open(os.path.join(src, rel), "rb") as f:
                blob = f.read()
            struct.pack_into("<I", ent, 52, pos)
            struct.pack_into("<I", ent, 56, len(blob))
            struct.pack_into("<I", ent, 60, 0)
            data += blob
            pos += len(blob)
        toc += ent

    total = data_off + len(data)

    hdr = bytearray(HDR_SIZE)
    hdr[0:4] = MAGIC
    struct.pack_into("<H", hdr, 4, VERSION)
    struct.pack_into("<H", hdr, 6, HDR_SIZE)
    struct.pack_into("<I", hdr, 8, count)
    struct.pack_into("<I", hdr, 12, toc_off)
    struct.pack_into("<I", hdr, 16, data_off)
    struct.pack_into("<I", hdr, 20, 0)
    struct.pack_into("<I", hdr, 24, total)

    blob = bytes(hdr) + bytes(toc) + bytes(data)
    chk = fnv1a(blob)
    blob = blob[:CHK_OFF] + struct.pack("<I", chk) + blob[CHK_OFF + 4:]

    with open(out, "wb") as f:
        f.write(blob)
    print("mkpack: %s -> %s (%d files, %d bytes)"
          % (src, out, count, len(blob)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: mkpack.py <srcdir> <out.pack>")
    build(sys.argv[1], sys.argv[2])
