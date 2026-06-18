# Fastfetch Upstream Provenance

This port targets the real Fastfetch project, not a local clone.

- Upstream repository: https://github.com/fastfetch-cli/fastfetch
- License: MIT
- Upstream branch inspected: `dev`
- Upstream layout used: `src/fastfetch.c`, `src/logo/`, `src/modules/`, detector/output architecture

TNU does not yet provide the hosted platform expected by upstream Fastfetch
CMake builds, so this directory carries a native backend slice: Fastfetch-style
modules and CLI are preserved, while platform detection is implemented through
TNU `/proc`, `/etc`, VFS, and tty ioctl APIs.

