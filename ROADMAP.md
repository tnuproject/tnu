# Roadmap

## TNU 0.1

- Bootable x86_64 kernel.
- VGA and serial console.
- Interrupts, timer, keyboard input.
- Memory manager foundations.
- TFS boot image and in-memory VFS.

## TNU 0.2

- Persistent TFS block-device backend.
- Init handoff from kernel shell to userspace.
- Better `/proc` and `/dev` models.
- Read-only ext2 exploration.
- Real-time clock timezone persistence and expanded keyboard layout tables.

## TNU 0.3

- Ring-3 execution for ELF userspace.
- `tsh` as a true userspace shell.
- Pipes, redirection, and environment handling.
- Writable editor workflow through `xedit`.

## TNU 0.4

- Basic users and groups enforcement.
- `/etc/shadow`-style password setup in `tsh`.
- File ownership and permission checks across core syscalls.
- User-visible administration commands for hostname, timezone, keymap, date,
  time, and uptime.

## TNU 0.6

- Ethernet driver transmit/receive path.
- ARP, IPv4, ICMP, UDP, TCP, DNS, and DHCP.
- Network-backed package index downloads.
- Porting layer for larger third-party programs such as Doom, Git, and editors.

## TNU 0.5

- Text installer with real disk selection, formatting, and bootloader install.
- Safer storage probing.
- Base-system copy from installation media.

## TNU 1.0

- Stable university demonstration release.
- Preemptive scheduler.
- Usable persistent filesystem.
- Documented labs for boot, memory, filesystem, syscalls, and userspace.
