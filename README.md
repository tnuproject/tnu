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
- GNU nano (real upstream, compiled from source during `make all`).
- DOOM via doomgeneric (compiled from source during `make all`), selectable WAD via `doom --version=N`.
- Intel Wi-Fi (iwlwifi MVM/DVM) driver with WPA2-PSK association and ping.
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
boot/                    GRUB configuration
kernel/                  monolithic kernel source
kernel/arch/             x86_64-specific code
kernel/fs/               TFS, VFS, procfs, devfs, and filesystem probes
ports/                   first-party ports (compiled from source at build time)
ports/nano/              GNU nano port (downloads madnight/nano via git clone)
ports/doom/              DOOM port via doomgeneric (downloads ozkl/doomgeneric via git clone)
rootfs/                  files packed into the boot TFS image
rootfs/usr/share/games/doom/  place your DOOM WAD files here before building
tools/                   host-side image builders
userspace/               freestanding libc, shell, init, sbin tools, and applets
```

## DOOM WAD files

DOOM WAD data files are **not distributed with this project** — they are
copyrighted by id Software / Bethesda. You must supply your own legally
obtained copies.

Place the following files in `rootfs/usr/share/games/doom/` before running
`make all`:

| File        | Game                   |
|-------------|------------------------|
| `Doom1.WAD` | DOOM (1993)            |
| `Doom2.wad` | DOOM II (1994)         |
| `Doom3.WAD` | The Ultimate DOOM      |

You can also use [FreeDOOM](https://freedoom.github.io/) as a free, open-source
replacement. See
[`rootfs/usr/share/games/doom/README.md`](rootfs/usr/share/games/doom/README.md)
for full details.

Once the WADs are in place, use `doom --version=1`, `doom --version=2`, or
`doom --version=3` from the Tiramisù shell to select which game to launch.

## License

See [LICENSE](LICENSE).

## FreeBSD Acknowledgement

The local `freebsd-src/` tree may be used as a reference for BSD-licensed
interfaces and driver design. Any FreeBSD-derived source imported into
Tiramisù must retain its original copyright notices and license conditions.
FreeBSD is copyright The FreeBSD Project and its contributors; see
`freebsd-src/COPYRIGHT`.
