# Userspace

`userspace/` contains freestanding programs for TNU:

- `libc`: tiny C library and syscall wrappers
- `init`: first process design target
- `shell/tsh`: TNU Shell
- `coreutils/tnu-utils`: multicall implementation for basic Unix commands,
  including `xedit`, date/time tools, keymap management, and network-inspection
  foundations
- `sbin`: login, passwd, useradd, userdel, and sysinstall

These programs do not link glibc or musl. They are built with `x86_64-elf-gcc`
and a minimal linker script.

TNU boots into a kernel-hosted shell while ring-3 execution is still being
completed. Normal command behavior lives in `kernel/core/applets.c` and is
dispatched only for command files that exist under `/bin` or `/sbin`; true ELF
activation remains the next userspace milestone.

The shell itself owns session state and syntax: `cd`, `login`, history,
environment variables, TAB completion, `~`, `*`, pipes, `&&`, comparisons, and
redirection. Identity is read from `/etc/hostname`, `/etc/passwd`, `/etc/group`,
`/etc/shadow`, and `/etc/os-release`.
