#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def load_mk(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or ":=" not in line:
            continue
        key, value = line.split(":=", 1)
        values[key.strip()] = value.strip()
    for _ in range(8):
        changed = False
        for key, value in list(values.items()):
            expanded = value
            for ref, ref_value in values.items():
                expanded = expanded.replace(f"$({ref})", ref_value)
            if expanded != value:
                values[key] = expanded
                changed = True
        if not changed:
            break
    return values


def subst(text: str, values: dict[str, str]) -> str:
    for key, value in values.items():
        text = text.replace(f"@{key}@", value)
    return text


def write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default="version.mk")
    parser.add_argument("--out", default="build/generated")
    args = parser.parse_args()

    values = load_mk(Path(args.version))
    out = Path(args.out)

    write(
        out / "include/tnu/version.h",
        f"""#ifndef TNU_VERSION_H
#define TNU_VERSION_H

#define TNU_NAME "{values['TNU_NAME']}"
#define TNU_FULL_NAME "{values['TNU_FULL_NAME']}"
#define TNU_VERSION "{values['TNU_VERSION']}"
#define TNU_CODENAME "{values['TNU_CODENAME']}"
#define TNU_ARCH "{values['TNU_ARCH']}"

#endif
""",
    )

    write(
        out / "rootfs/etc/os-release",
        f"""NAME={values['TNU_NAME']}
PRETTY_NAME="{values['TNU_NAME']} {values['TNU_VERSION']}"
ID={values['TNU_ID']}
VERSION_ID={values['TNU_VERSION']}
VERSION_CODENAME={values['TNU_CODENAME']}
HOME_URL={values['TNU_HOME_URL']}
BUG_REPORT_URL={values['TNU_BUG_REPORT_URL']}
""",
    )

    write(
        out / "rootfs/etc/motd",
        f"""Welcome to {values['TNU_NAME']} {values['TNU_VERSION']}
Made with <3 from Italy.

Type "help" in tsh for available commands.
Run "passwd" to set the root password.
""",
    )

    template = Path("boot/grub/grub.cfg.in").read_text()
    write(out / "boot/grub/grub.cfg", subst(template, values))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
