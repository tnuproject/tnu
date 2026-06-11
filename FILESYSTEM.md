# Filesystem

TNU includes **TFS**, the TNU File System. TFS is intentionally simple and
ext2-like in spirit: case-sensitive paths, fixed metadata records, permissions,
owners, and byte-addressed file data.

## Image Format

The build tool `tools/mktfs.py` creates a read-mostly boot image:

- header with magic, version, entry count, and data offset
- fixed-size entry table
- file payload region

At boot the kernel copies the TFS image into an in-memory VFS tree. Runtime
commands such as `mkdir`, `touch`, `chmod`, and `chown` mutate that in-memory
tree. Persistent disk writes are planned for TNU 0.2/0.5.

## Standard Tree

The generated root filesystem contains:

- `/bin`
- `/sbin`
- `/etc`
- `/dev`
- `/proc`
- `/home`
- `/root`
- `/tmp`
- `/usr`
- `/var`

## Virtual Filesystems

`/dev` is populated by `devfs`, and `/proc` exposes kernel state such as memory,
version, uptime, and process information.

## Other Filesystems

The kernel includes read-only metadata probes for ext2, ext4, and FAT32. These
drivers identify superblocks/BPBs and report basic volume information, but they
do not mount directory trees or write data yet.
