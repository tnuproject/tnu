# TNU Porting Status

TNU can now build and run selected real upstream applications in userspace.
The current priority is stabilizing this POSIX layer before attempting larger
desktop-class software.

## Working Ports

- `doom`: upstream doomgeneric can be built as a standalone `/usr/games/doom`
  binary. It uses `/dev/fb0`, `/dev/input/kbd`, monotonic time, TFS-backed
  file I/O, and the bundled `/usr/share/games/doom/freedoom1.wad`.
- `nano`: GNU nano 9.0 can be built as a standalone `/bin/nano` binary.
  It uses the TNU libc, a small curses/termcap shim, file descriptors,
  termios compatibility, signals compatibility, and the syscall-backed heap.

## Recently Fixed Foundations

- Ring-3 ELF execution through `SYS_EXEC`.
- Correct negative syscall returns to userspace.
- `brk`/`sbrk`-backed userspace heap.
- Reusable libc allocator with `free`, `realloc`, block splitting, and coalescing.
- TFS zero-copy mounting for large boot image files, with copy-on-write when a
  mounted file is modified.
- `O_EXCL` and `O_APPEND` semantics in the kernel VFS.
- More POSIX-like `stdio` formatting, including `%zu`, `%llu`, dynamic
  precision, and safe string precision.
- Minimal `mmap` compatibility for anonymous/private mappings.

## Xorg Status

Xorg is not complete yet. A real Xorg server needs kernel facilities that are
larger than a userspace recipe:

- TSS/RSP0 or equivalent safe interrupt entry from ring 3.
- `fork`, `waitpid`, process groups, and robust signal delivery.
- Real shared `mmap`, shared memory, and page ownership tracking.
- Unix domain sockets or local stream sockets.
- `select`/`poll` with device readiness.
- A tty/virtual-terminal layer with real `termios` and ioctls.
- A fuller input stack and a stable userspace framebuffer/DRM-style interface.
- Dynamic loading or a build profile that can statically link the required Xorg
  module set.

Until those pieces exist, claiming a complete Xorg port would be misleading.
The source fetch metadata is kept so the port can continue once the kernel
surface is ready.

## Fetching Sources

```sh
make ports-fetch-core
```

This fetches GNU nano, doomgeneric, and X.Org xserver into
`build/ports/sources/`. The versioned repo keeps recipes and compatibility
code; fetched upstream source trees remain build artifacts. The default TNU
image no longer preinstalls `nano` or `doom`; build those ports explicitly if
you want them in a custom image.
