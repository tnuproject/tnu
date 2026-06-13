# X.Org Port

Upstream: X.Org modular tree / xserver  
Target path: `/usr/bin/Xorg`

Current TNU status:

- Source fetch is handled by `make ports-fetch-core`.
- Minimal framebuffer and keyboard device APIs exist, but TNU does not yet have
  the POSIX, dynamic loading, or shared-memory surface required by X.Org.

Hard blockers:

- Userspace ELF execution.
- `mmap`, shared memory, advanced `ioctl`, `poll/select`, signals, pipes, sockets.
- Dynamic loader or a fully static Xorg profile with patched module loading.
- evdev-compatible input events.
- Userspace graphics ABI, ideally a small fbdev-compatible subset first.
- Dependencies: xproto, xtrans, pixman, libXfont or modern replacement path.

Near-term strategy:

1. Land a small fbdev-style userspace graphics ABI.
2. Port `pixman` before attempting xserver.
3. Build a tiny X11 test server/client pair before full X.Org.
4. Keep X.Org as the long-range target rather than disguising a custom demo as Xorg.
