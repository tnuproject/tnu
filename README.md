# Tiramisù

Tiramisù is a small x86_64 monolithic operating system built for systems
programming study. It boots with GRUB, mounts a TFS root image, starts `tsh`,
and provides a freestanding userspace tree with native ELF binaries.

Current release metadata is generated from [version.mk](version.mk).

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
- `pkg`, with a local `universe` mirror for installable Doom and Nano packages.
- Read-only probe foundations for ext2, ext4, and FAT32 metadata.
- Ring-3 ELF execution for the current static userspace model.

Not finished yet:

- Per-process address spaces and a full `fork`/`exec`/`wait` process model.
- Persistent writable TFS on real disks.
- Real disk installation from `sysinstall`.
- DHCP, HTTP, Git transport, remote package downloads, and WiFi association.
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
build/<project>-<version>-<arch>.iso
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
universe/      default package repository metadata for pkg
```

## License

See [LICENSE](LICENSE).

## FreeBSD Acknowledgement

The local `freebsd-src/` tree may be used as a reference for BSD-licensed
interfaces and driver design. Any FreeBSD-derived source imported into
Tiramisù must retain its original copyright notices and license conditions.
FreeBSD is copyright The FreeBSD Project and its contributors; see
`freebsd-src/COPYRIGHT`.
