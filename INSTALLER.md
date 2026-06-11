# Installer

`sysinstall` exists both as a userspace program source and as a kernel-shell
command. In TNU 0.1 it is intentionally safe: it can configure the live system
identity, root password, and a normal user, but it does not write partition
tables or install a bootloader because persistent block-device writes are not
implemented yet.

Planned flow:

1. Select a disk.
2. Partition the disk.
3. Format a TFS root filesystem.
4. Install GRUB or Limine.
5. Copy the base system.
6. Set hostname.
7. Set root password.
8. Create a normal user.

The command is structured so disk operations can be connected once ATA/AHCI
write support, a persistent TFS allocator, and a bootloader writer are ready.
Until then, `/etc/passwd`, `/etc/shadow`, created users, files, and directories
are live-session state only.
