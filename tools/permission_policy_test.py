#!/usr/bin/env python3
"""Static guard rails for TNU's root/sudo permission policy."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(path: str, needle: str) -> None:
    text = (ROOT / path).read_text()
    if needle not in text:
        raise SystemExit(f"permission policy test failed: {path} is missing {needle!r}")


def main() -> int:
    require("kernel/core/syscall.c", "path_requires_root_for_mutation")
    require("kernel/core/syscall.c", "case SYS_MKDIR")
    require("kernel/core/syscall.c", "case SYS_UNLINK")
    require("kernel/core/syscall.c", "case SYS_CHMOD")
    require("kernel/core/syscall.c", "case SYS_CHOWN")
    require("kernel/core/syscall.c", "wants_write && !is_root(proc)")
    require("kernel/core/syscall.c", "(mode & 2) && !is_root(proc)")
    require("kernel/core/syscall.c", "node->type == VFS_NODE_DEV && !is_root(proc)")
    require("kernel/core/process.c", "caller->uid != 0 && caller->uid != p->uid")
    require("kernel/shell/tsh.c", "run_command(argc - 1, argv + 1, NULL)")
    require("kernel/core/applets.c", "syscall_dispatch(SYS_UNLINK, (uint64_t)argv[1]")
    require("kernel/core/applets.c", "rm_recursive_path")
    require("userspace/pkg/pkg.c", "require_root_for_mutation(\"install\")")
    require("userspace/pkg/pkg.c", "require_root_for_mutation(\"remove\")")
    require("userspace/pkg/pkg.c", "require_root_for_mutation(\"sync\")")
    print("permission policy static checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
