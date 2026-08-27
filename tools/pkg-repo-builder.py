#!/usr/bin/env python3
"""
Tiramisu Package Repository Builder (pkg-repo-builder)

Automates fetching vanilla Linux/POSIX upstream sources, applying Tiramisu
patches, building package trees, and generating `repo.txt` / `packages.json`
metadata for hosting on GitHub (Raw GitHub / GitHub Pages).
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
PORTS_DIR = ROOT / "ports"
BUILD_DIR = ROOT / "build" / "universe"
REPO_OUTPUT = ROOT / "universe-main"


def load_recipes() -> list[dict]:
    recipes = []
    for recipe_file in sorted(PORTS_DIR.glob("*/recipe.json")):
        try:
            data = json.loads(recipe_file.read_text(encoding="utf-8"))
            data["_recipe_path"] = recipe_file
            data["_port_dir"] = recipe_file.parent
            recipes.append(data)
        except Exception as e:
            print(f"[WARN] Failed to load {recipe_file}: {e}", file=sys.stderr)
    return recipes


def fetch_upstream(recipe: dict, cache_dir: pathlib.Path) -> pathlib.Path:
    upstream = recipe.get("upstream", {})
    name = recipe["name"]
    source_type = upstream.get("type", "local")
    url = upstream.get("url", "")
    target_src = cache_dir / name

    if target_src.exists():
        return target_src

    target_src.mkdir(parents=True, exist_ok=True)

    if source_type == "git" and url:
        print(f"[FETCH] Cloning upstream git for {name}: {url}")
        subprocess.check_call(["git", "clone", "--depth", "1", url, str(target_src)])
    elif source_type == "tarball" and url:
        archive_path = cache_dir / f"{name}.tar.xz"
        print(f"[FETCH] Downloading upstream tarball for {name}: {url}")
        try:
            with urllib.request.urlopen(url) as resp, open(archive_path, "wb") as f:
                shutil.copyfileobj(resp, f)
            print(f"[EXTRACT] Extracting {archive_path}")
            with tarfile.open(archive_path) as tar:
                tar.extractall(target_src)
        except Exception as e:
            print(f"[WARN] Download failed: {e}. Using local port files if present.")
    else:
        # Local source in port dir
        print(f"[INFO] Using local source tree for {name}")

    return target_src


def apply_patches(recipe: dict, src_dir: pathlib.Path):
    port_dir = recipe["_port_dir"]
    patches = recipe.get("patches", [])
    for patch_rel in patches:
        patch_file = port_dir / patch_rel
        if not patch_file.exists():
            print(f"[WARN] Patch {patch_file} not found, skipping")
            continue
        print(f"[PATCH] Applying {patch_file.name} to {recipe['name']}")
        try:
            # Try git apply or patch command
            res = subprocess.run(
                ["git", "apply", "--check", str(patch_file)],
                cwd=str(src_dir),
                capture_output=True,
                text=True
            )
            if res.returncode == 0:
                subprocess.run(["git", "apply", str(patch_file)], cwd=str(src_dir), check=True)
            else:
                subprocess.run(
                    ["patch", "-p1", "-N", "-i", str(patch_file)],
                    cwd=str(src_dir),
                    capture_output=True
                )
        except Exception as e:
            print(f"[INFO] Patch note: {e}")


def build_package(recipe: dict, output_repo: pathlib.Path):
    name = recipe["name"]
    version = recipe["version"]
    arch = recipe.get("arch", "x86_64")
    port_dir = recipe["_port_dir"]

    pkg_archive_name = f"{name}-{version}-{arch}"
    pkg_dir = output_repo / pkg_archive_name
    pkg_dir.mkdir(parents=True, exist_ok=True)

    print(f"[BUILD] Packaging {name} v{version} ({arch}) into {pkg_archive_name}")

    # If port has pre-built or source binaries/data, copy them
    if name == "doom":
        games_dir = pkg_dir / "usr" / "games"
        share_dir = pkg_dir / "usr" / "share" / "games" / "doom"
        games_dir.mkdir(parents=True, exist_ok=True)
        share_dir.mkdir(parents=True, exist_ok=True)
        # Copy doom binary if compiled
        bin_src = ROOT / "build" / "ports" / "doom"
        if (bin_src / "doom").exists():
            shutil.copy2(bin_src / "doom", games_dir / "doom")
        elif (port_dir / "doom").exists():
            shutil.copy2(port_dir / "doom", games_dir / "doom")
        # Copy WAD
        wad_src = port_dir / "freedoom" / "freedoom1.wad"
        if wad_src.exists():
            shutil.copy2(wad_src, share_dir / "freedoom1.wad")

    elif name == "nano":
        bin_dir = pkg_dir / "bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        bin_src = ROOT / "build" / "ports" / "nano"
        if (bin_src / "nano").exists():
            shutil.copy2(bin_src / "nano", bin_dir / "nano")
        elif (port_dir / "nano").exists():
            shutil.copy2(port_dir / "nano", bin_dir / "nano")

    elif name == "fastfetch":
        bin_dir = pkg_dir / "usr" / "bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        bin_src = ROOT / "build" / "ports" / "fastfetch"
        if (bin_src / "fastfetch").exists():
            shutil.copy2(bin_src / "fastfetch", bin_dir / "fastfetch")
        elif (port_dir / "fastfetch").exists():
            shutil.copy2(port_dir / "fastfetch", bin_dir / "fastfetch")

    # Record package metadata file inside archive
    meta = {
        "name": name,
        "version": version,
        "arch": arch,
        "description": recipe.get("description", ""),
        "dependencies": recipe.get("dependencies", []),
        "license": recipe.get("license", "Unknown")
    }
    (pkg_dir / ".PKGINFO").write_text(json.dumps(meta, indent=2), encoding="utf-8")


def generate_repo_index(recipes: list[dict], output_repo: pathlib.Path):
    repo_txt_lines = [
        "# Tiramisu OS Official Package Repository Index",
        "# Format: name|version|arch|description|archive_name|dependencies",
    ]
    repo_json = {
        "repository": "universe-main",
        "arch": "x86_64",
        "packages": []
    }

    for r in recipes:
        name = r["name"]
        ver = r["version"]
        arch = r.get("arch", "x86_64")
        desc = r.get("description", "")
        archive = f"{name}-{ver}-{arch}"
        deps = ",".join(r.get("dependencies", []))

        repo_txt_lines.append(f"{name}|{ver}|{arch}|{desc}|{archive}|{deps}")
        repo_json["packages"].append({
            "name": name,
            "version": ver,
            "arch": arch,
            "description": desc,
            "archive": archive,
            "dependencies": r.get("dependencies", []),
            "upstream": r.get("upstream", {}),
            "patches": r.get("patches", [])
        })

    (output_repo / "repo.txt").write_text("\n".join(repo_txt_lines) + "\n", encoding="utf-8")
    (output_repo / "packages.json").write_text(json.dumps(repo_json, indent=2), encoding="utf-8")
    print(f"[SUCCESS] Generated repo.txt and packages.json in {output_repo}")


def main():
    parser = argparse.ArgumentParser(description="Build Tiramisu package repository")
    parser.add_argument("--output", default=str(REPO_OUTPUT), help="Output repository directory")
    parser.add_argument("--fetch", action="store_true", help="Fetch upstream source archives")
    args = parser.parse_args()

    out_path = pathlib.Path(args.output).resolve()
    out_path.mkdir(parents=True, exist_ok=True)
    cache_path = BUILD_DIR / "sources"

    recipes = load_recipes()
    print(f"[INFO] Loaded {len(recipes)} package recipes from {PORTS_DIR}")

    for recipe in recipes:
        if args.fetch:
            src = fetch_upstream(recipe, cache_path)
            apply_patches(recipe, src)
        build_package(recipe, out_path)

    generate_repo_index(recipes, out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
