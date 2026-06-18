# Building TNU

TNU is intended to build on Linux or WSL with standard OS-development tools.

## Dependencies

- `make`
- `x86_64-elf-gcc`
- `x86_64-elf-ar`
- `grub-file`
- `grub-mkrescue`
- `grub-mkstandalone`
- `grub-install` for real disk installs from `sysinstall`
- `xorriso`
- `isoinfo`
- `mkfs.vfat` from `dosfstools` for `sysinstall` ESP formatting
- `python3`
- `qemu-system-x86_64` for `make run`

## Commands

```sh
make all        # Build kernel, userspace, TFS image, and ISO
make kernel     # Build only the kernel ELF
make userspace  # Build freestanding userspace ELF binaries
make iso        # Build bootable ISO
make run        # Boot the ISO in QEMU
make verify     # Re-check kernel and ISO bootability metadata
make clean      # Remove build artifacts
```

The ISO name is generated from `version.mk`.

## Boot Path

GRUB loads `kernel.elf` through Multiboot2 and passes `root.tfs` as a module.
The assembly bootstrap builds an identity-mapped 1 GiB paging environment,
enters long mode, and calls the C kernel entry point. The VM layer then adds
identity mappings for Multiboot-provided framebuffers that live above 1 GiB,
which is common on UEFI/GOP hardware.

The generated ISO is hybrid bootable. It contains the BIOS El Torito image,
the UEFI El Torito image, and a visible removable-media fallback at
`/EFI/BOOT/BOOTX64.EFI`. The build fails if the kernel stops passing
`grub-file --is-x86-multiboot2` or if the ISO misses the required boot files.
