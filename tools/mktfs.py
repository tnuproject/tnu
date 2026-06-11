#!/usr/bin/env python3
"""Build a TFS boot image.

TFS is intentionally simple for teaching: one fixed entry table followed by
file payloads. The kernel copies this image into its in-memory VFS at boot.
"""

from __future__ import annotations

import os
import stat
import struct
import sys
from pathlib import Path


MAGIC = b"TFS1TNU\0"
VERSION = 1
HEADER = struct.Struct("<8sIIQQ")
ENTRY = struct.Struct("<IIIIQQQ256s")
TYPE_DIR = 1
TYPE_FILE = 2


def align(value: int, amount: int = 8) -> int:
    return (value + amount - 1) & ~(amount - 1)


def usage() -> None:
    print("usage: mktfs.py ROOTDIR OUTFILE", file=sys.stderr)
    raise SystemExit(2)


def collect(root: Path):
    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        path = Path(dirpath)
        rel = path.relative_to(root)
        tfs_path = "/" if str(rel) == "." else "/" + rel.as_posix()
        if tfs_path != "/":
            st = path.stat()
            entries.append(
                {
                    "type": TYPE_DIR,
                    "path": tfs_path,
                    "mode": stat.S_IMODE(st.st_mode),
                    "uid": 0,
                    "gid": 0,
                    "mtime": int(st.st_mtime),
                    "data": b"",
                }
            )
        for name in filenames:
            if name == ".keep":
                continue
            fpath = path / name
            st = fpath.stat()
            rel_file = fpath.relative_to(root)
            entries.append(
                {
                    "type": TYPE_FILE,
                    "path": "/" + rel_file.as_posix(),
                    "mode": stat.S_IMODE(st.st_mode),
                    "uid": 0,
                    "gid": 0,
                    "mtime": int(st.st_mtime),
                    "data": fpath.read_bytes(),
                }
            )
    entries.sort(key=lambda e: (e["path"].count("/"), e["path"]))
    return entries


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        usage()

    root = Path(argv[1])
    out = Path(argv[2])
    if not root.is_dir():
        raise SystemExit(f"{root} is not a directory")

    entries = collect(root)
    entries_offset = HEADER.size
    data_offset = align(entries_offset + len(entries) * ENTRY.size)

    payload = bytearray()
    packed_entries = bytearray()
    current_data = data_offset

    for item in entries:
        data = item["data"]
        if item["type"] == TYPE_FILE:
            offset = current_data
            size = len(data)
            payload.extend(data)
            pad = align(len(payload)) - len(payload)
            payload.extend(b"\0" * pad)
            current_data = data_offset + len(payload)
        else:
            offset = 0
            size = 0

        encoded_path = item["path"].encode("utf-8")
        if len(encoded_path) >= 256:
            raise SystemExit(f"path too long for TFS: {item['path']}")
        packed_entries.extend(
            ENTRY.pack(
                item["type"],
                item["mode"],
                item["uid"],
                item["gid"],
                size,
                offset,
                item["mtime"],
                encoded_path + b"\0" * (256 - len(encoded_path)),
            )
        )

    image = bytearray()
    image.extend(HEADER.pack(MAGIC, VERSION, len(entries), entries_offset, data_offset))
    image.extend(packed_entries)
    image.extend(b"\0" * (data_offset - len(image)))
    image.extend(payload)

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(f"mktfs: wrote {out} with {len(entries)} entries ({len(image)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
