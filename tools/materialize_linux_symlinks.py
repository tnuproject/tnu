#!/usr/bin/env python3
"""Replace symlinks in a Linux rootfs with regular files/directories for TFS.

TFS currently stores only regular files and directories. Alpine uses symlinks
heavily for BusyBox applets and dynamic loader/library aliases, so deleting
those symlinks makes commands such as ls, clear and fastfetch disappear
or fail at runtime. This helper resolves symlinks that point inside the Linux
root and replaces them with copies of their targets.
"""

from __future__ import annotations

import os
import shutil
import stat
import sys
from pathlib import Path


def resolve_inside(root: Path, link: Path) -> Path | None:
    target_text = os.readlink(link)
    if target_text.startswith("/"):
        target = root / target_text.lstrip("/")
    else:
        target = (link.parent / target_text)
    try:
        resolved = target.resolve(strict=True)
        resolved.relative_to(root.resolve(strict=True))
    except (FileNotFoundError, ValueError):
        return None
    return resolved


def materialize_link(root: Path, link: Path) -> bool:
    target = resolve_inside(root, link)
    if target is None:
        link.unlink()
        return False

    tmp = link.with_name(link.name + ".tnu-materialized")
    if tmp.exists() or tmp.is_symlink():
        if tmp.is_dir() and not tmp.is_symlink():
            shutil.rmtree(tmp)
        else:
            tmp.unlink()

    if target.is_dir():
        shutil.copytree(target, tmp, symlinks=False)
    else:
        shutil.copy2(target, tmp)
        mode = target.stat().st_mode
        if mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
            tmp.chmod(mode | stat.S_IXUSR)

    link.unlink()
    tmp.rename(link)
    return True


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: materialize_linux_symlinks.py ROOT", file=sys.stderr)
        return 2

    root = Path(argv[1])
    if not root.is_dir():
        print(f"{root}: not a directory", file=sys.stderr)
        return 2

    links = [p for p in root.rglob("*") if p.is_symlink()]
    converted = 0
    removed = 0
    # Resolve shallow links first; repeated passes handle symlink chains.
    for _ in range(8):
        pending = [p for p in links if p.exists() or p.is_symlink()]
        if not pending:
            break
        progressed = False
        for link in pending:
            if not link.is_symlink():
                continue
            if materialize_link(root, link):
                converted += 1
            else:
                removed += 1
            progressed = True
        if not progressed:
            break

    print(f"materialize-linux-symlinks: converted={converted} removed={removed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
