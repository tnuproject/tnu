# ISO Generation

The bootable ISO is assembled by the top-level `Makefile`.

`make iso` creates:

- `build/iso/boot/kernel.elf`
- `build/iso/boot/root.tfs`
- `build/iso/boot/grub/grub.cfg`
- `build/tnu.iso`

GRUB loads the kernel with Multiboot2 and passes `root.tfs` as a boot module.
The project keeps the ISO staging area generated under `build/` so source and
build artifacts stay separate.
