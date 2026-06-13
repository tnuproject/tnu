#!/usr/bin/env python3
import argparse
import base64
import binascii
from pathlib import Path


INTEL_LICENSE_SUMMARY = """\
Intel wireless firmware imported from FreeBSD sys/contrib/dev/iwn,
sys/contrib/dev/wpi, and sys/contrib/dev/iwm.

The firmware blobs are redistributed in binary form, without modification,
under Intel's firmware license included at the top of the FreeBSD .fw.uu
source files. The license permits binary redistribution with the copyright
notice/disclaimer, prohibits endorsement by Intel/suppliers, and prohibits
reverse engineering, decompilation, or disassembly.

TNU decodes or copies these files at build time and installs them in:
  /lib/firmware/iwlwifi

Source paths:
"""


IWM_ALIASES = {
    "iwm-3160-17.fw": ["iwlwifi-3160-17.ucode"],
    "iwm-3168-22.fw": ["iwlwifi-3168-22.ucode"],
    "iwm-7260-17.fw": ["iwlwifi-7260-17.ucode"],
    "iwm-7265-17.fw": ["iwlwifi-7265-17.ucode"],
    "iwm-7265D-22.fw": ["iwlwifi-7265D-22.ucode"],
    "iwm-8000C-22.fw": ["iwlwifi-8000C-22.ucode", "iwlwifi-8000C-36.ucode"],
    "iwm-8265-22.fw": ["iwlwifi-8265-22.ucode", "iwlwifi-8265-36.ucode"],
    "iwm-9000-34.fw": ["iwlwifi-9000-pu-b0-jf-b0-34.ucode"],
    "iwm-9260-34.fw": ["iwlwifi-9260-th-b0-jf-b0-34.ucode", "iwlwifi-9260-th-b0-jf-b0-46.ucode"],
}


def output_name(path: Path) -> str:
    name = path.name
    if name.endswith(".uu"):
        name = name[:-3]
    if name.startswith("iwnwifi-2030"):
        name = name.replace("iwnwifi-", "iwlwifi-", 1)
    return name


def output_names(path: Path) -> list[str]:
    aliases = IWM_ALIASES.get(path.name)
    if aliases:
        return aliases
    return [output_name(path)]


def decode_begin_base64(lines, start):
    payload = []
    for line in lines[start + 1:]:
        if line.strip() == "====":
            break
        payload.append(line.strip())
    if not payload:
        raise ValueError("empty begin-base64 payload")
    return base64.b64decode("".join(payload), validate=False)


def decode_begin_uu(lines, start):
    out = bytearray()
    for line in lines[start + 1:]:
        if line == "end":
            break
        if line == "`":
            continue
        try:
            out.extend(binascii.a2b_uu(line.encode("ascii")))
        except binascii.Error as exc:
            raise ValueError(f"bad uuencode line near {line[:16]!r}") from exc
    if not out:
        raise ValueError("empty uuencode payload")
    return bytes(out)


def decode_file(path: Path) -> bytes:
    if path.suffix == ".fw":
        return path.read_bytes()
    lines = path.read_text(errors="replace").splitlines()
    for index, line in enumerate(lines):
        if line.startswith("begin-base64 "):
            return decode_begin_base64(lines, index)
        if line.startswith("begin "):
            return decode_begin_uu(lines, index)
    raise ValueError("no begin marker found")


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode FreeBSD Intel wireless firmware")
    parser.add_argument("--out", required=True, help="output firmware directory")
    parser.add_argument("inputs", nargs="+", help=".fw.uu files from FreeBSD")
    args = parser.parse_args()

    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    decoded = []
    for raw in args.inputs:
        src = Path(raw)
        names = output_names(src)
        blob = decode_file(src)
        for name in names:
            (outdir / name).write_bytes(blob)
            decoded.append((src, name, len(blob)))

    readme = [INTEL_LICENSE_SUMMARY]
    for src, name, size in decoded:
        readme.append(f"  {src} -> {name} ({size} bytes)\n")
    (outdir / "README.TNU").write_text("".join(readme))

    print(f"decoded {len(decoded)} Intel wireless firmware blobs into {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
