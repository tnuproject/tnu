#!/usr/bin/env python3
"""Preinstall TNU universe packages into a rootfs staging tree."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


def read_required(path: Path) -> list[str]:
    packages: list[str] = []
    if not path.exists():
        return packages
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            packages.append(line)
    return packages


def install_package(rootfs: Path, universe: Path, name: str) -> None:
    pkg = universe / "packages" / name
    manifest = pkg / "manifest"
    files = pkg / "files"
    if not manifest.is_file() or not files.is_dir():
        raise SystemExit(f"required-packages: package not found in universe: {name}")

    for src in files.rglob("*"):
        rel = src.relative_to(files)
        dst = rootfs / rel
        if src.is_dir():
            dst.mkdir(parents=True, exist_ok=True)
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    db = rootfs / "var" / "db" / "pkg" / "installed"
    db.mkdir(parents=True, exist_ok=True)
    shutil.copy2(manifest, db / f"{name}.manifest")
    print(f"required-packages: installed {name}")


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: install_required_packages.py ROOTFS UNIVERSE REQUIRED", file=sys.stderr)
        return 2
    rootfs = Path(argv[1])
    universe = Path(argv[2])
    required = Path(argv[3])
    if not rootfs.is_dir():
        raise SystemExit(f"rootfs staging directory does not exist: {rootfs}")
    if not universe.is_dir():
        raise SystemExit(f"universe directory does not exist: {universe}")
    for package in read_required(required):
        install_package(rootfs, universe, package)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
