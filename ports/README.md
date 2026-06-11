# TNU Porting Status

TNU is now closer to a POSIX-like target, but it is not ready for honest upstream
ports of Doom, nano, or X.Org yet.

Implemented foundations:

- POSIX-style userspace headers for common includes such as `unistd.h`,
  `fcntl.h`, `sys/stat.h`, `stdio.h`, `stdlib.h`, `string.h`, and `errno.h`.
- Basic libc routines used by small Unix programs: `printf`, `puts`,
  `malloc`, `calloc`, `realloc`, `free`, `strtol`, `memmove`, `memcmp`,
  `strcat`, and `strrchr`.
- Syscall wrappers for file I/O, users, directories, metadata, and `lseek`.
- Kernel `SYS_LSEEK` support.
- Ethernet/e1000 ARP, IPv4, ICMP echo, UDP DNS queries, and hostname
  resolution for commands such as `ping google.com`.
- `/bin/sh` and `/usr/bin/sh` aliases for TSH.
- Executable `.sh` scripts with shebang handling for `/bin/sh`, `/bin/tsh`,
  and `/usr/bin/env sh`.
- Script arguments, `$0`, `$1` through `$9`, `$@`, `$?`, variables,
  `NAME=value`, `if exists path { ... }`, and `for name in pattern { ... }`.

Current hard blockers for real upstream ports:

- `SYS_EXEC` still does not activate a userspace ELF image. The kernel shell can
  validate an ELF and create a process record, but it does not yet switch to a
  ring-3 address space and transfer control to the binary.
- `fork`, `wait`, signals, `pipe(2)`, `dup2`, `mmap`, `brk`, `ioctl`,
  `termios`, and directory iteration APIs are not complete.
- TCP, sockets, TLS, and a userspace networking API are not complete. `curl`,
  `wget`, package managers, and browsers need those before they can be real.
- NVMe and full AHCI block I/O are not implemented. Installation currently uses
  the block devices exposed by the existing legacy ATA path.
- ext2, ext4, FAT32, and devfs are detection/foundation layers, not complete
  production filesystem drivers.
- The C library is intentionally small and freestanding. It is enough for TNU's
  own userspace and small test programs, not for large configure-based projects.
- X.Org also requires a mature graphics device model, input stack, shared
  memory, dynamic loading, and a much larger POSIX surface.

Porting order:

1. Finish userspace ELF execution and process address spaces.
2. Add `fork`, `wait`, signals, pipes, `dup2`, `brk`/`mmap`, and directory APIs.
3. Replace the static libc heap with a syscall-backed heap.
4. Bring up a termios-compatible tty layer.
5. Add TCP sockets and enough POSIX networking for `wget`/`curl`.
6. Add AHCI and NVMe block read/write with a shared block-device layer.
7. Replace filesystem probes with real mounted ext2/FAT32 read paths, then ext4.
8. Port a tiny real upstream program first, then nano, then Doom.
9. Treat X.Org as a long-term goal after graphics, input, shared memory, and
   dynamic linking mature.
