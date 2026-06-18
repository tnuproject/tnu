# Linux Compatibility Layer

Tiramisu (TNU) includes a Linux ABI compatibility layer that allows running
Linux x86_64 ELF binaries directly on TNU without a full Linux kernel.

## Overview

The Linux compat layer implements:

- **System calls**: Core Linux x86_64 syscalls (read, write, open, close, mmap, brk, execve, etc.)
- **Filesystem**: Path translation from Linux paths to TNU's `/usr/linux` chroot
- **Networking**: Socket syscalls (stub - returns ENOSYS)
- **IPC**: Pipe and epoll syscalls (stub - returns ENOSYS)
- **Signals**: rt_sigaction, rt_sigprocmask, sigaltstack (stub)
- **Process**: fork, clone, vfork (stub - returns ENOSYS)

## Using `linux-run`

The kernel shell (`tsh`) provides a `linux-run` builtin command:

```
# linux-run /usr/linux/bin/busybox ls -la
# linux-run /usr/linux/usr/bin/fastfetch
```

Linux commands can also be launched directly from `tsh`. Command resolution is
controlled by `/etc/priority`:

```text
linux: 99999
tnu: 1
```

Higher weight wins. With the default above, typing `ls` prefers
`/usr/linux/bin/ls` or `/usr/linux/usr/bin/ls` over Tiramisu's applet when that
Linux command exists. Explicit paths are respected, so `/bin/ls` still means the
Tiramisu path and `/usr/linux/bin/ls` still means the Linux path.

Tiramisu branding and system-management commands always keep native priority:
`sysfetch`, `hostname`, `login`, `useradd`, `userdel`, `passwd`, `init`, `sh`,
`tsh`, `uname`, `tirux`, `shutdown`, `reboot`, `sync`, `keymap`, `timezone`,
`layout`, and `nano`.

## Linux Chroot

TNU fetches an Alpine Linux minirootfs during `make all`:

```bash
make linux-chroot-fetch    # Downloads Alpine minirootfs
make linux-chroot-packages # Installs fastfetch and freedoom via apk
```

`make all` runs both targets before building the ISO. The build stops if
`/usr/bin/fastfetch` is missing from the Alpine chroot, so the final
`/usr/linux` environment is not silently shipped half-prepared.

The chroot is installed at `/usr/linux` in the TNU rootfs.

### Running Nano

Nano is a native Tiramisu applet and does not depend on the Linux compat layer:

```
# nano /path/to/file.txt
```

### Running Fastfetch

Fastfetch is installed from Alpine Linux packages inside the Linux chroot:

```
# linux-run /usr/linux/usr/bin/fastfetch
```

### Running Doom (Freedoom)

Freedoom is installed from Alpine Linux packages. The `freedoom` package includes
the Freedoom WAD files. To play Doom:

```
# linux-run /usr/linux/usr/games/doom /usr/linux/usr/share/games/doom/freedoom1.wad
```

You can also use your own WAD files from any directory:

```
# linux-run /usr/linux/usr/games/doom /home/user/Doom1.WAD
# linux-run /usr/linux/usr/games/doom /path/to/doom2.wad
```

### Installing Additional Packages

To install additional Linux packages in the chroot:

```bash
# On the host system:
chroot build/linux-chroot /bin/sh -c 'apk update && apk add <package>'
```

Then rebuild the ISO to include the updated chroot:

```bash
make iso
```

## Current Limitations

1. **No networking**: Socket syscalls return ENOSYS. Linux binaries requiring
   network access will fail.

2. **No fork/clone**: Process creation is not supported. Multi-process Linux
   applications will not work.

3. **No pipes**: IPC mechanisms are stubs. Shell pipelines with Linux binaries
   won't work.

4. **Limited signal handling**: Signals are accepted but not delivered.

## Supported Applications

Applications that work well with the current compat layer:

- Simple command-line tools (busybox utilities from Alpine)
- Native Tiramisu utilities alongside Linux busybox applets
- Games using framebuffer directly (freedoom from Alpine)

## Architecture

```
User Linux ELF Binary
         |
         v
+------------------+
| linux-run /      |
| linux_execve()   |
+------------------+
         |
         v
+------------------+
| Linux Syscall    |
| Dispatch         |
| (linux_syscall.c)|
+------------------+
         |
         +---> Native TNU syscall (read, write, open, etc.)
         |
         +---> Linux-specific implementation (mmap, brk, arch_prctl)
         |
         +---> Stub returning ENOSYS (socket, fork, pipe)
```

## Files

- `kernel/compat/linux/runtime/linux_runtime.c` - Chroot initialization
- `kernel/compat/linux/userspace/linux_exec.c` - ELF loading and execution
- `kernel/compat/linux/syscall/linux_syscall.c` - Syscall dispatch
- `kernel/compat/linux/elf/linux_elf.c` - ELF probing
- `kernel/compat/linux/net/linux_net.c` - Socket syscall stubs
- `kernel/compat/linux/ipc/linux_ipc.c` - Pipe/epoll syscall stubs
- `kernel/compat/linux/signal/linux_signal.c` - Signal syscall stubs
- `kernel/compat/linux/proc/linux_proc.c` - fork/clone syscall stubs

## Future Work

- Implement true socket support bridging to TNU's net stack
- Add pipe/epoll for IPC
- Implement fork/clone for multi-process support
- Add more complete signal delivery
