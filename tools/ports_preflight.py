#!/usr/bin/env python3
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

FEATURE_CHECKS = {
    "dns": lambda: "int net_resolve4" in (ROOT / "kernel/drivers/net/net.c").read_text(),
    "filesystem_read": lambda: "vfs_read_node" in (ROOT / "kernel/fs/vfs.c").read_text(),
    "filesystem_write": lambda: "vfs_write_node" in (ROOT / "kernel/fs/vfs.c").read_text(),
    "monotonic_time": lambda: "pit_ticks" in (ROOT / "kernel/arch/x86_64/pit.c").read_text(),
    "keyboard_events": lambda: "keyboard_getchar" in (ROOT / "kernel/arch/x86_64/keyboard.c").read_text(),
    "framebuffer_userspace": lambda: "/dev/fb0" in (ROOT / "kernel/fs/devfs.c").read_text()
    and "TNU_IOCTL_FB_GETINFO" in (ROOT / "kernel/core/syscall.c").read_text(),
    "libc": lambda: (ROOT / "userspace/libc/include/stdio.h").exists(),
    "exec": lambda: not re.search(r"case SYS_EXEC:\s*return -1;", (ROOT / "kernel/core/syscall.c").read_text()),
    "termios": lambda: (ROOT / "userspace/libc/include/termios.h").exists(),
    "signals": lambda: (ROOT / "userspace/libc/include/signal.h").exists(),
    "directory_iteration": lambda: "SYS_READDIR" in (ROOT / "kernel/include/tnu/syscall.h").read_text()
    and "readdir_fd" in (ROOT / "userspace/libc/src/syscall.c").read_text()
    and "errno = ENOSYS" not in re.search(
        r"DIR \*opendir.*?int closedir", (ROOT / "userspace/libc/src/posix_stubs.c").read_text(), re.S
    ).group(0),
    "locale_minimal": lambda: (ROOT / "userspace/libc/include/locale.h").exists(),
    "heap_syscall": lambda: "SYS_BRK" in (ROOT / "userspace/libc/include/tnu/syscall.h").read_text(),
    "tcp": lambda: "IP_PROTO_TCP" in (ROOT / "kernel/drivers/net/net.c").read_text(),
    "sockets": lambda: (ROOT / "userspace/libc/include/sys/socket.h").exists(),
    "select_poll": lambda: (ROOT / "userspace/libc/include/sys/select.h").exists() or (ROOT / "userspace/libc/include/poll.h").exists(),
    "tls_or_no_tls_profile": lambda: False,
    "mmap": lambda: (ROOT / "userspace/libc/include/sys/mman.h").exists(),
    "shared_memory": lambda: "SYS_SHM" in (ROOT / "userspace/libc/include/tnu/syscall.h").read_text(),
    "ioctl": lambda: "SYS_IOCTL" in (ROOT / "kernel/include/tnu/syscall.h").read_text()
    and "tnu_syscall(SYS_IOCTL" in (ROOT / "userspace/libc/src/posix_stubs.c").read_text(),
    "userspace_graphics": lambda: "/dev/fb0" in (ROOT / "kernel/fs/devfs.c").read_text()
    and "TNU_IOCTL_FB_GETINFO" in (ROOT / "kernel/core/syscall.c").read_text(),
    "input_events": lambda: "/dev/input/kbd" in (ROOT / "kernel/fs/devfs.c").read_text()
    and "keyboard_try_getchar" in (ROOT / "kernel/core/syscall.c").read_text(),
    "dynamic_loader": lambda: (ROOT / "userspace/libc/ld.so").exists(),
    "xorg": lambda: False,
    "http_client": lambda: False,
    "font_rendering": lambda: False,
}


def feature_ok(name: str) -> bool:
    check = FEATURE_CHECKS.get(name)
    if not check:
        return False
    try:
        return bool(check())
    except OSError:
        return False


def main() -> int:
    manifest = json.loads((ROOT / "ports/ports.json").read_text())
    failed = False
    for port in manifest["ports"]:
        missing = [f for f in port["required_features"] if not feature_ok(f)]
        status = "ready" if not missing else "blocked"
        print(f"{port['name']}: {status}")
        if missing:
            failed = True
            print("  missing: " + ", ".join(missing))
        print(f"  source: {port['source']}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
