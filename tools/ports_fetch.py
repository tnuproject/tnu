#!/usr/bin/env python3
"""Fetch upstream source archives/repositories for TNU ports.

The script intentionally only downloads/extracts sources. Building real ports
is gated by `tools/ports_preflight.py`, because TNU must not pretend that large
POSIX applications are runnable before exec, signals, termios, graphics, and
socket foundations exist.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request


ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "ports" / "ports.json"
SOURCES = ROOT / "build" / "ports" / "sources"
DOWNLOADS = ROOT / "build" / "ports" / "downloads"


def load_ports() -> list[dict]:
    return json.loads(MANIFEST.read_text())["ports"]


def selected_ports(names: set[str]) -> list[dict]:
    ports = load_ports()
    if not names:
        return ports
    selected = [port for port in ports if port["name"] in names]
    missing = names - {port["name"] for port in selected}
    if missing:
        raise SystemExit(f"unknown port(s): {', '.join(sorted(missing))}")
    return selected


def fetch_git(port: dict, force: bool) -> None:
    target = SOURCES / port["name"]
    if target.exists():
        if not force:
            print(f"{port['name']}: already present at {target}")
            return
        shutil.rmtree(target)
    print(f"{port['name']}: cloning {port['source']}")
    subprocess.check_call(["git", "clone", "--depth", "1", port["source"], str(target)])


def fetch_archive(port: dict, force: bool) -> None:
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    SOURCES.mkdir(parents=True, exist_ok=True)
    url = port["source"]
    suffix = "".join(pathlib.PurePosixPath(url).suffixes) or ".tar"
    archive = DOWNLOADS / f"{port['name']}{suffix}"
    target = SOURCES / port["name"]

    if force or not archive.exists():
        print(f"{port['name']}: downloading {url}")
        with urllib.request.urlopen(url) as response, archive.open("wb") as out:
            shutil.copyfileobj(response, out)
    else:
        print(f"{port['name']}: using cached {archive}")

    if target.exists():
        if not force:
            print(f"{port['name']}: already extracted at {target}")
            return
        shutil.rmtree(target)

    tmp = SOURCES / f".{port['name']}.extract"
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    print(f"{port['name']}: extracting")
    with tarfile.open(archive) as tar:
        tar.extractall(tmp)

    children = [p for p in tmp.iterdir() if p.name not in {".", ".."}]
    if len(children) == 1 and children[0].is_dir():
        children[0].rename(target)
        tmp.rmdir()
    else:
        tmp.rename(target)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="fetch TNU upstream port sources")
    parser.add_argument("ports", nargs="*", help="port names; defaults to all")
    parser.add_argument("--force", action="store_true", help="redownload/reclone sources")
    args = parser.parse_args(argv)

    SOURCES.mkdir(parents=True, exist_ok=True)
    for port in selected_ports(set(args.ports)):
        source = port["source"]
        if source.startswith("ports/"):
            print(f"{port['name']}: local source {source}")
        elif source.endswith(".git"):
            fetch_git(port, args.force)
        else:
            fetch_archive(port, args.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
