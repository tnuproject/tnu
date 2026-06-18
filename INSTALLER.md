# Installer

`sysinstall` in the kernel shell delegates to `/sbin/sysinstall`.
The old live-image cloning path is intentionally hidden behind:

```sh
sysinstall --raw-image
```

Do not use raw-image mode for real hardware installs.

## Disk Layout

The real installer creates and formats the disk itself:

1. BIOS Boot Partition, LBAs 34-2047.
2. EFI System Partition, 256 MiB FAT32.
3. TFS root partition.

TFS is the default and only root filesystem in the normal installer flow.

## Native Formatting

`/sbin/sysinstall` no longer requires `mkfs.vfat`, `mount`, `umount`, or
`grub-install` to format the disk. It writes:

- protective MBR
- primary and backup GPT
- BIOS Boot partition entry
- FAT32 ESP
- `/EFI/BOOT/BOOTX64.EFI`
- `/boot/kernel.elf`
- `/boot/grub/grub.cfg`
- TFS root generated from the live root filesystem

The BIOS Boot partition is reserved for native GRUB core embedding. UEFI
removable boot files are installed directly into the ESP.
