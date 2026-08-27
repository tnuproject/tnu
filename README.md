# Tiramisu OS ☕

[![GitHub stars](https://img.shields.io/github/stars/tnuproject/tnu?style=for-the-badge)](https://github.com/tnuproject/tnu/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/tnuproject/tnu?style=for-the-badge)](https://github.com/tnuproject/tnu/network)
[![GitHub issues](https://img.shields.io/github/issues/tnuproject/tnu?style=for-the-badge)](https://github.com/tnuproject/tnu/issues)
[![GitHub license](https://img.shields.io/github/license/tnuproject/tnu?style=for-the-badge)](LICENSE)
[![](https://dcbadge.limes.pink/api/server/TpKK29ACyr)](https://discord.gg/TpKK29ACyr)

![Tiramisu OS](https://raw.githubusercontent.com/tnuproject/tnu/refs/heads/main/assets/screenshot.png)

## What is Tiramisu OS?

**Tiramisu** is a lightweight, clean, and modern 64-bit (`x86_64`) Unix-like operating system and independent kernel built from scratch.

Our goal is to deliver a fast, responsive, and reliable operating system for real hardware and virtual machines with rich Unix semantics, high-performance hardware drivers, and package management.

### Key Highlights & Features
- **Lightweight x86_64 Kernel**: Custom monolithic modular architecture with VFS, TFS zero-copy root filesystem, Ring 3 preemptive multitasking, POSIX-compatible libc and syscalls.
- **Fluid Screen Rendering on Real Hardware**:
  - Linear Framebuffer (VESA/GOP) with CPU Write-Combining (WC) acceleration.
  - Double-buffering architecture with granular **dirty-rectangle tracking** to minimize PCIe/VRAM bus traffic and eliminate screen tearing.
  - High-performance 64-bit/SIMD-optimized row blits and scrolling.
- **Input & Device Drivers**:
  - Complete **USB HID Keyboard** driver with boot protocol report decoding and Host Controller support (UHCI, OHCI, EHCI, xHCI).
  - Legacy i8042 PS/2 controller driver with automatic fallbacks.
  - Storage drivers: AHCI (SATA), NVMe, and legacy ATA.
- **Intel Wi-Fi (`iwlwifi`) Driver for Real Hardware**:
  - Dual support for **DVM** (legacy Centrino 6000/5000) and **MVM** (3160, 7260, 7265, 8260, 8265, 9000, 9260, 9560, AX200, AX201, AX210, AX211, AX411, and Wi-Fi 7) wireless chipsets.
  - Firmware loading, LMAC scan offloading, and WPA2-PSK (CCMP) authentication.
- **Pacman-Inspired Package Manager (`pkg`)**:
  - Fast, lightweight command-line package manager supporting standard pacman operations:
    - `pkg -S <pkg...>`: Install packages.
    - `pkg -Syu` / `pkg -Sy`: Synchronize databases and upgrade system.
    - `pkg -Ss <query>`: Search remote repositories.
    - `pkg -Si <pkg>`: View remote package information.
    - `pkg -R <pkg...>`: Uninstall packages and clean up installed files.
    - `pkg -Q` / `pkg -Ql <pkg>`: Query installed packages and list installed files.
    - `pkg -Scc`: Clean package cache.
- **Ports & Package Hosting (Vanilla Upstream + Patches)**:
  - Transparent port recipes (`ports/<name>/recipe.json`).
  - Vanilla Linux/POSIX upstream sources with modular `.patch` files (e.g. `doom`, `nano`, `fastfetch`).
  - Automated package repository builder (`tools/pkg-repo-builder.py`) and GitHub Actions deployment to host packages on GitHub Pages or Raw repositories.

---

## Building Tiramisu OS

### Requirements (Linux or WSL)
- `gcc` or `x86_64-elf-gcc`
- `nasm`
- `grub-mkrescue`, `grub-mkstandalone`, `grub-file`
- `xorriso`, `isoinfo`
- `mtools` / `mformat`
- `python3`
- `qemu-system-x86_64` (for emulation)

### Build & Run
```sh
# Build the complete kernel, userspace, and bootable ISO
make all

# Test the system in QEMU
make run

# Build without Linux binary compatibility bundle
make wolinux

# Package repository building
python3 tools/pkg-repo-builder.py --output universe-main
```

The bootable ISO will be generated at `build/tiramisu-1.1.0-x86_64.iso`.

---

## Package Management & Ports

Install packages directly inside Tiramisu OS:
```sh
# Search for packages
pkg -Ss doom

# Install DOOM and nano
pkg -S doom nano

# List installed packages and files
pkg -Q
pkg -Ql doom

# Run installed applications
/usr/games/doom
/bin/nano
```

---

## Community & Contributing
- Discord: https://dsc.gg/tnutiramisu
- GitHub Issues & PRs: https://github.com/tnuproject/tnu/issues

## License
Tiramisu OS is licensed under the BSD/GPL compatible terms. See [LICENSE](LICENSE) for details.
