# Tiramisù/tnu

[![GitHub stars](https://img.shields.io/github/stars/tnuproject/tnu?style=for-the-badge)](https://github.com/tnuproject/tnu/stargazers)

[![GitHub forks](https://img.shields.io/github/forks/tnuproject/tnu?style=for-the-badge)](https://github.com/tnuproject/tnu/network)

[![GitHub issues](https://img.shields.io/github/issues/tnuproject/tnu?style=for-the-badge)](https://github.com/tnuproject/tnu/issues)

[![GitHub license](https://img.shields.io/github/license/tnuproject/tnu?style=for-the-badge)](LICENSE)

![Image](https://raw.githubusercontent.com/tnuproject/tnu/refs/heads/main/assets/screenshot.png)

## What is TNU/Tiramisu?

TNU (Tiramisu) is a free, open-source, community-driven kernel project aiming to become a complete and reliable *nix-like operating system.

Our goal is to provide a modern daily-use environment with strong compatibility, familiar Unix principles, and the ability to run Linux software while remaining an independent system.

> **Note**
> 
> Some parts of the current codebase were initially developed with the help of AI-generated code. This is one of the reasons why the project is fully open-source: we believe in transparency and community collaboration.
> 
> Our long-term goal is to review, improve, and replace experimental implementations with clean, stable, production-quality code written and maintained by real contributors.»
>
> Discord: https://dsc.gg/tnutiramisu

Also, we are NOT affiliated with Linux. Our kernel is developed independently and does not depend on Linux binaries or Linux source code.

---

**Build Requirements**

Install the required tools on Linux or WSL:

- "gcc" or "x86_64-elf-gcc"
- "grub-mkrescue"
- "grub-mkstandalone"
- "grub-file"
- "xorriso"
- "isoinfo"
- "mtools" / "mformat"
- "qemu-system-x86_64"
- "python3"

**Build**

Build the system and generate the bootable ISO:

`make all`

**Run it using QEMU:**

`make run`

**Verify the generated image:**

`make verify`

**Output**

The generated ISO will be available at:

`build/<project>-<version>-<arch>.iso`

The ISO includes the kernel, boot configuration, root filesystem, and UEFI boot files.
Disable Secure Boot when testing on physical hardware.

**License**

See [LICENSE](LICENSE).
