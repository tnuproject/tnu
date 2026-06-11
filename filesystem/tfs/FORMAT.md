# TFS On-Disk Format

TFS, the TNU File System, is the native educational filesystem for TNU.
The current boot image format is deliberately simple:

- 32-byte header
- fixed 296-byte metadata records
- 8-byte-aligned file payload area

All integers are little-endian. Paths are absolute, UTF-8 byte strings capped at
255 bytes plus a trailing NUL. The kernel imports this image into a mutable
in-memory VFS during TNU 0.1 boot. A persistent block allocator, free-space
bitmap, and directory indexing are planned for TNU 0.2.

See `tools/mktfs.py` and `kernel/fs/tfs.c` for the authoritative format code.
