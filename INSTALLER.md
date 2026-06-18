# Installer

`sysinstall` in the kernel shell is now a safety wrapper for `/sbin/sysinstall`.
The old live-image cloning path is intentionally hidden behind:

```sh
sysinstall --raw-image
```

Do not use raw-image mode for real hardware installs. It copies the live ISO
image to the disk and can leave legacy BIOS firmware stuck at a plain `GRUB`
screen before the menu appears.

## Disk Layout

The real installer creates a GPT layout suitable for both legacy BIOS and UEFI:

1. BIOS Boot Partition, LBAs 34-2047, for GRUB `i386-pc` core image.
2. EFI System Partition, 256 MiB FAT32, mounted by firmware as removable UEFI.
3. Root partition, default TFS.

TFS mount discovery uses the Tiramisu root partition GUID, with a fallback for
older two-partition installs.

## Bootloader Requirements

The installer refuses to modify disks unless it can complete the bootloader
installation. A valid install requires helper tools in the live environment:

- `mkfs.vfat`
- `mount`
- `umount`
- `grub-install`

When those helpers are present, `/sbin/sysinstall` installs:

- BIOS GRUB with `grub-install --target=i386-pc`
- UEFI removable boot with `grub-install --target=x86_64-efi --removable`
- `/EFI/BOOT/BOOTX64.EFI`
- `/boot/kernel.elf`
- `/boot/root.tfs`
- `/boot/grub/grub.cfg`

If helper execution is unavailable, the installer exits before writing the disk.
