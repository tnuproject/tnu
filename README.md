# TNU

TNU is a small x86_64 monolithic operating system built for systems
programming study. It boots with GRUB, mounts a TFS root image, starts a
kernel-hosted `tsh` shell, and provides a freestanding userspace tree that is
compiled into ELF binaries for the next ring-3 loader milestone.

Current release: **0.1.0 "espresso"**.

## Status

Working today:

- GRUB Multiboot2 boot on x86_64 with BIOS and UEFI ISO paths.
- VGA text console, GOP/linear framebuffer console, serial logging, serial console input,
  and framebuffer state reporting.
- IDT, CPU exceptions, PIC, PIT timer, PS/2 keyboard input, and PCI discovery.
- Physical memory accounting, early virtual memory setup, and a kernel heap.
- TFS boot image mounted into an in-memory VFS.
- `/dev`, `/proc`, process table, file descriptors, and syscall dispatcher foundations.
- Root account with `/root`, `/etc/passwd`, `/etc/group`, and `/etc/shadow`.
- `tsh` with history, arrows, TAB completion, `~`, `*`, pipes, `&&`, comparisons,
  and basic redirection.
- Executable `#!/bin/tsh` scripts with arguments and simple variable expansion.
- `/bin` applets for common Unix-like commands.
- Read-only probe foundations for ext2, ext4, and FAT32 metadata.

Not finished yet:

- Ring-3 ELF execution.
- Persistent writable TFS on real disks.
- Real disk installation from `sysinstall`.
- Full TCP/IP, DNS, DHCP, HTTP, Git transport, or package downloads.
- Full ext2/ext4/FAT32 mounting and writing.

## Build

Required tools on Linux or WSL:

- `x86_64-elf-gcc` and `x86_64-elf-ar`, or host `gcc` for the current freestanding build.
- `grub-mkrescue`
- `grub-mkstandalone`
- `grub-file`
- `xorriso`
- `isoinfo`
- `qemu-system-x86_64`
- `python3`

```sh
make all
make run
make verify
```

The ISO is written to:

```text
build/tnu.iso
```

The ISO contains `/boot/kernel.elf`, `/boot/root.tfs`,
`/boot/grub/grub.cfg`, and the removable-media UEFI fallback
`/EFI/BOOT/BOOTX64.EFI`. Disable Secure Boot when testing on physical
hardware.

## Useful Commands

Inside TNU:

```sh
help
sysfetch
passwd
useradd alice
login alice
keymap it
timezone UTC+02:00
date
uptime
echo hello > /tmp/hello.txt
cat < /tmp/hello.txt
echo hello | cat
root == root && echo ok
cat /proc/framebuffer
```

Root’s home directory is `/root` and is intentionally mode `0700`. Normal users
should not be able to `cd /root`; that is standard Unix-style permission
behavior.

## Layout

```text
boot/          GRUB configuration
kernel/        monolithic kernel source
kernel/arch/   x86_64-specific code
kernel/fs/     TFS, VFS, procfs, devfs, and filesystem probes
rootfs/        files packed into the boot TFS image
tools/         host-side image builders
userspace/     freestanding libc, shell, init, sbin tools, and applets
```

## License

See [LICENSE](LICENSE).
