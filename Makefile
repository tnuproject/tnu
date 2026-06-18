# Tiramisu OS Makefile
# Build for x86_64 (default) or i386:
#   make                    # Build x86_64 (default)
#   make ARCH=i386          # Build 32-bit i386
#   make ARCH=x86_64        # Build 64-bit x86_64 (explicit)

include version.mk

# Allow architecture override: make ARCH=i386 or make ARCH=x86_64
ARCH ?= $(TNU_ARCH)
PROJECT := $(TNU_PROJECT)

# Set architecture-specific compiler if cross-compiling
ifeq ($(ARCH),i386)
    ifeq ($(origin CC),default)
        CC := $(shell command -v i686-elf-gcc >/dev/null 2>&1 && echo i686-elf-gcc || echo gcc)
    endif
    ifeq ($(origin AR),default)
        AR := $(shell command -v i686-elf-ar >/dev/null 2>&1 && echo i686-elf-ar || echo ar)
    endif
    ARCH_BITS := 32
    ARCH_M_FLAG := -m32
    ARCH_DIR := x86
    ELF_CLASS_RE := Class:.*ELF32
    ELF_MACHINE_RE := Machine:.*Intel 80386
else
    ifeq ($(origin CC),default)
        CC := $(shell command -v x86_64-elf-gcc >/dev/null 2>&1 && echo x86_64-elf-gcc || echo gcc)
    endif
    ifeq ($(origin AR),default)
        AR := $(shell command -v x86_64-elf-ar >/dev/null 2>&1 && echo x86_64-elf-ar || echo ar)
    endif
    ARCH_BITS := 64
    ARCH_M_FLAG := -m64
    ARCH_DIR := x86_64
    ELF_CLASS_RE := Class:.*ELF64
    ELF_MACHINE_RE := Machine:.*Advanced Micro Devices X86-64
endif
HOSTPY ?= python3
QEMU ?= qemu-system-x86_64
GRUB_FILE ?= grub-file
GRUB_MKRESCUE ?= grub-mkrescue
GRUB_MKSTANDALONE ?= grub-mkstandalone
GRUB_FONT ?= /usr/share/grub/unicode.pf2
ISOINFO ?= isoinfo
READELF ?= readelf
XORRISO ?= xorriso
MFORMAT ?= mformat
GIT ?= git
PATCH ?= patch

BUILD := build
KERNEL := $(BUILD)/kernel.elf
ROOTFS := $(BUILD)/root.tfs
# ISO filename reflects the actual architecture being built
ISO := $(BUILD)/$(PROJECT)-$(TNU_VERSION)-$(ARCH).iso
EFI_BOOT := $(BUILD)/BOOTX64.EFI
INSTALL_IMAGE := $(BUILD)/install.img
GENERATED := $(BUILD)/generated
GENERATED_VERSION := $(GENERATED)/include/tnu/version.h
GENERATED_GRUB := $(GENERATED)/boot/grub/grub.cfg

KERNEL_CFLAGS := -std=gnu11 -Os -g -Wall -Wextra -ffreestanding \
	-fno-stack-protector -fno-stack-check -fno-pic -fomit-frame-pointer \
	-fno-builtin $(ARCH_M_FLAG) -mno-red-zone -mgeneral-regs-only \
	-ffunction-sections -fdata-sections \
	-I$(GENERATED)/include -Ikernel/include -Ikernel/arch/$(ARCH_DIR)/include
KERNEL_ASFLAGS := $(KERNEL_CFLAGS)
KERNEL_LDFLAGS := -T kernel/linker.ld -nostdlib -static -no-pie \
	-Wl,-z,max-page-size=0x1000 -Wl,--build-id=none \
	-Wl,--gc-sections

USER_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding \
	-fno-stack-protector -fno-builtin -fno-pic $(ARCH_M_FLAG) -mno-red-zone \
	-Iuserspace/libc/include -Ikernel/include
USER_LDFLAGS := -T userspace/linker.ld -nostdlib -static -no-pie \
	-Wl,-z,max-page-size=0x1000

KERNEL_C := $(shell find kernel -name '*.c' | sort)
# tss.c is automatically included by the recursive kernel source scan
KERNEL_S := $(shell find kernel -name '*.S' | sort)
KERNEL_HEADERS := $(shell find kernel/include kernel/arch/$(ARCH_DIR)/include -name '*.h' 2>/dev/null | sort) \
	$(GENERATED_VERSION)
KERNEL_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(KERNEL_C)) \
	$(patsubst %.S,$(BUILD)/obj/%.o,$(KERNEL_S))

LIBC_C := $(shell find userspace/libc/src -name '*.c' | sort)
LIBC_S := $(shell find userspace/libc/src -name '*.S' | sort)
USER_HEADERS := $(shell find userspace/libc/include -name '*.h' | sort)
LIBC_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(LIBC_C)) \
	$(patsubst %.S,$(BUILD)/obj/%.o,$(LIBC_S))
USER_CRT := $(BUILD)/obj/userspace/libc/src/crt0.o
USER_LIB := $(BUILD)/user/libtnu.a

COREUTIL_NAMES := cat chmod chown clear cp curl date dmesg dns driver echo hostname \
	id ifconfig keymap kill linuxdrv ls mkdir mount mv net xedit netstat ping ps pwd reboot rm route dhcp \
	shutdown stat sysfetch sync tar time timezone tirux tls touch uname unzip uptime usb wget whoami wifi zip

IWN_FW_SRC := $(shell find freebsd-src/sys/contrib/dev/iwn freebsd-src/sys/contrib/dev/wpi -maxdepth 1 -name '*.fw.uu' 2>/dev/null | sort)
IWM_FW_SRC := $(shell find freebsd-src/sys/contrib/dev/iwm -maxdepth 1 -name '*.fw' 2>/dev/null | sort)
LINUX_IWL_FW_SRC := $(shell find rootfs/lib/firmware rootfs/lib/firmware/iwlwifi /lib/firmware /lib/firmware/iwlwifi -maxdepth 1 \( -name 'iwlwifi-*.ucode' -o -name 'iwlwifi-*.fw' \) 2>/dev/null | sort -u)

.PHONY: all kernel userspace iso run clean rootfs version-files permission-tests verify verify-kernel verify-iso \
	firmware-iwlwifi ports-preflight ports-fetch ports-fetch-core \
	fastfetch ports-fetch-freedoom \
	linux-chroot-fetch linux-chroot linux-chroot-packages

all: ports-fetch linux-chroot-fetch linux-chroot-packages iso

kernel: $(KERNEL)

userspace: $(USER_LIB) $(BUILD)/user/init $(BUILD)/user/tsh \
	$(BUILD)/user/tnu-utils $(BUILD)/user/login $(BUILD)/user/passwd \
	$(BUILD)/user/useradd $(BUILD)/user/userdel $(BUILD)/user/sysinstall \
	$(BUILD)/user/bootd $(BUILD)/user/fastfetch

fastfetch: $(BUILD)/user/fastfetch $(BUILD)/user/fastfetch

iso: $(ISO)

run: $(ISO)
	$(QEMU) -m 1G -cdrom $(ISO) -vga std -serial file:/tmp/qemu_serial.log

clean:
	rm -rf $(BUILD)

version-files: $(GENERATED_VERSION) $(GENERATED_GRUB)

$(GENERATED_VERSION) $(GENERATED_GRUB): version.mk boot/grub/grub.cfg.in tools/gen_version.py
	$(HOSTPY) tools/gen_version.py --version version.mk --out $(GENERATED)

firmware-iwlwifi: tools/decode_iwn_firmware.py
	rm -rf $(BUILD)/firmware/iwlwifi
	mkdir -p $(BUILD)/firmware/iwlwifi
	@if [ -n "$(strip $(IWN_FW_SRC) $(IWM_FW_SRC))" ]; then \
		$(HOSTPY) tools/decode_iwn_firmware.py --out $(BUILD)/firmware/iwlwifi $(IWN_FW_SRC) $(IWM_FW_SRC); \
	else \
		echo "No FreeBSD iwn/iwm firmware sources found, skipping decode"; \
		printf "TNU iwlwifi firmware bundle\n\nNo FreeBSD .fw/.fw.uu sources were found during this build.\n" > $(BUILD)/firmware/iwlwifi/README.TNU; \
	fi
	@if [ -n "$(strip $(LINUX_IWL_FW_SRC))" ]; then \
		for fw in $(LINUX_IWL_FW_SRC); do cp -f "$$fw" $(BUILD)/firmware/iwlwifi/; done; \
		echo "Copied Linux iwlwifi firmware into $(BUILD)/firmware/iwlwifi"; \
	else \
		echo "No Linux iwlwifi-*.ucode/.fw files found in rootfs/lib/firmware or /lib/firmware"; \
	fi

$(KERNEL): $(KERNEL_OBJS) kernel/linker.ld $(GENERATED_VERSION)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS) -lgcc
	$(GRUB_FILE) --is-x86-multiboot2 $@
	@LC_ALL=C $(READELF) -h $@ | grep -q '$(ELF_CLASS_RE)' || { LC_ALL=C $(READELF) -h $@; echo "error: $@ is not the expected $(ARCH)-class kernel"; false; }
	@LC_ALL=C $(READELF) -h $@ | grep -q '$(ELF_MACHINE_RE)' || { LC_ALL=C $(READELF) -h $@; echo "error: $@ is not the expected $(ARCH) machine type"; false; }

$(BUILD)/obj/kernel/%.o: kernel/%.c $(KERNEL_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/obj/kernel/compat/linux/syscall/linux_syscall_table.o: \
	kernel/compat/linux/syscall/linux_syscall_table.c \
	kernel/compat/linux/syscall/linux_syscall.tbl \
	tools/gen_linux_syscalls.py \
	$(KERNEL_HEADERS)
	$(HOSTPY) tools/gen_linux_syscalls.py kernel/compat/linux/syscall/linux_syscall.tbl kernel/compat/linux/syscall/linux_syscall_table.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c kernel/compat/linux/syscall/linux_syscall_table.c -o $@

$(BUILD)/obj/kernel/%.o: kernel/%.S $(KERNEL_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_ASFLAGS) -c $< -o $@

$(BUILD)/obj/userspace/%.o: userspace/%.c $(USER_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/obj/userspace/%.o: userspace/%.S $(USER_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_LIB): $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(filter-out $(USER_CRT),$(LIBC_OBJS))

$(BUILD)/user/init: $(BUILD)/obj/userspace/init/init.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/tsh: $(BUILD)/obj/userspace/shell/tsh.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/tnu-utils: $(BUILD)/obj/userspace/coreutils/tnu-utils.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/httpget: $(BUILD)/obj/userspace/bin/httpget.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/login: $(BUILD)/obj/userspace/sbin/login.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/passwd: $(BUILD)/obj/userspace/sbin/passwd.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/useradd: $(BUILD)/obj/userspace/sbin/useradd.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/userdel: $(BUILD)/obj/userspace/sbin/userdel.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/sysinstall: $(BUILD)/obj/userspace/sbin/sysinstall.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/bootd: $(BUILD)/obj/userspace/sbin/bootd.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/dhclient: $(BUILD)/obj/userspace/sbin/dhclient.o $(USER_LIB) $(USER_CRT) userspace/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_CRT) $< $(USER_LIB) -lgcc

$(BUILD)/user/fastfetch: $(USER_LIB) $(USER_CRT) userspace/linker.ld \
	$(shell find ports/fastfetch/src -type f 2>/dev/null)
	$(MAKE) -C ports/fastfetch CC="$(CC)" \
		USER_CRT="$(abspath $(USER_CRT))" \
		USER_LIB="$(abspath $(USER_LIB))"

rootfs: userspace version-files firmware-iwlwifi
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs
	cp -a rootfs/. $(BUILD)/rootfs/
	cp -a $(GENERATED)/rootfs/. $(BUILD)/rootfs/
	cp ascii.txt $(BUILD)/rootfs/etc/sysfetch-logo
	mkdir -p $(BUILD)/rootfs/bin $(BUILD)/rootfs/sbin $(BUILD)/rootfs/usr/bin \
		$(BUILD)/rootfs/usr/games $(BUILD)/rootfs/usr/share/games/doom \
		$(BUILD)/rootfs/lib/modules \
		$(BUILD)/rootfs/lib/firmware/iwlwifi \
		$(BUILD)/rootfs/usr/linux
	cp $(BUILD)/user/init $(BUILD)/rootfs/sbin/init
	cp $(BUILD)/user/tsh $(BUILD)/rootfs/bin/tsh
	cp $(BUILD)/user/tsh $(BUILD)/rootfs/bin/sh
	cp $(BUILD)/user/tsh $(BUILD)/rootfs/usr/bin/sh
	cp $(BUILD)/user/login $(BUILD)/rootfs/bin/login
	cp $(BUILD)/user/passwd $(BUILD)/rootfs/bin/passwd
	cp $(BUILD)/user/useradd $(BUILD)/rootfs/sbin/useradd
	cp $(BUILD)/user/userdel $(BUILD)/rootfs/sbin/userdel
	cp $(BUILD)/user/sysinstall $(BUILD)/rootfs/sbin/sysinstall
	for name in $(COREUTIL_NAMES); do cp $(BUILD)/user/tnu-utils $(BUILD)/rootfs/bin/$$name; done
	cp $(BUILD)/user/fastfetch $(BUILD)/rootfs/usr/bin/fastfetch
	cp -a $(BUILD)/firmware/iwlwifi/. $(BUILD)/rootfs/lib/firmware/iwlwifi/
	@if [ -d "$(LINUX_CHROOT_DIR)" ]; then \
		echo "rootfs: copying Linux chroot into rootfs/usr/linux"; \
		rm -rf $(BUILD)/rootfs/usr/linux; \
		mkdir -p $(BUILD)/rootfs/usr/linux; \
		tar -C $(LINUX_CHROOT_DIR) -cf - bin lib lib64 sbin usr var etc 2>/dev/null | \
			tar -C $(BUILD)/rootfs/usr/linux -xf -; \
		if [ -d "$(LINUX_CHROOT_DIR)/lib/modules" ]; then \
			mkdir -p $(BUILD)/rootfs/lib/modules; \
			cp -a $(LINUX_CHROOT_DIR)/lib/modules/. $(BUILD)/rootfs/lib/modules/; \
		fi; \
		# Fix broken symlinks that point to absolute paths \
		find $(BUILD)/rootfs/usr/linux -xtype l -delete 2>/dev/null || true; \
	else \
		echo "rootfs: no Linux chroot found (run 'make linux-chroot-fetch')"; \
	fi

$(ROOTFS): rootfs tools/mktfs.py
	$(HOSTPY) tools/mktfs.py $(BUILD)/rootfs $@

$(EFI_BOOT): $(GENERATED_GRUB)
	@mkdir -p $(dir $@)
	$(GRUB_MKSTANDALONE) -O x86_64-efi -o $@ \
		--modules="part_gpt part_msdos iso9660 fat search search_fs_file test normal boot multiboot2 font gfxterm all_video efi_gop efi_uga video video_fb video_bochs video_cirrus" \
		"boot/grub/grub.cfg=$(GENERATED_GRUB)"

$(INSTALL_IMAGE): $(KERNEL) $(ROOTFS) $(GENERATED_GRUB) $(EFI_BOOT)
	rm -rf $(BUILD)/install-iso
	mkdir -p $(BUILD)/install-iso/boot/grub/fonts $(BUILD)/install-iso/EFI/BOOT
	cp $(KERNEL) $(BUILD)/install-iso/boot/kernel.elf
	$(GRUB_FILE) --is-x86-multiboot2 $(BUILD)/install-iso/boot/kernel.elf
	@LC_ALL=C $(READELF) -h $(BUILD)/install-iso/boot/kernel.elf | grep -q '$(ELF_CLASS_RE)' || { LC_ALL=C $(READELF) -h $(BUILD)/install-iso/boot/kernel.elf; echo "error: install kernel is not the expected $(ARCH)-class kernel"; false; }
	$(READELF) -l $(BUILD)/install-iso/boot/kernel.elf | grep -q 'LOAD'
	cp $(ROOTFS) $(BUILD)/install-iso/boot/root.tfs
	cp $(GENERATED_GRUB) $(BUILD)/install-iso/boot/grub/grub.cfg
	cp $(GRUB_FONT) $(BUILD)/install-iso/boot/grub/fonts/unicode.pf2
	cp $(EFI_BOOT) $(BUILD)/install-iso/EFI/BOOT/BOOTX64.EFI
	@command -v $(MFORMAT) >/dev/null 2>&1 || { echo "error: $(GRUB_MKRESCUE) needs $(MFORMAT) from mtools; install the mtools package"; false; }
	$(GRUB_MKRESCUE) -V TNU_BOOT -o $@ $(BUILD)/install-iso
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/kernel.elf'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/root.tfs'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/grub/grub.cfg'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/grub/fonts/unicode.pf2'
	$(ISOINFO) -i $@ -R -f | grep -qx '/EFI/BOOT/BOOTX64.EFI'

$(ISO): $(KERNEL) $(ROOTFS) $(GENERATED_GRUB) $(EFI_BOOT) $(INSTALL_IMAGE)
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/grub/fonts $(BUILD)/iso/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso/boot/kernel.elf
	$(GRUB_FILE) --is-x86-multiboot2 $(BUILD)/iso/boot/kernel.elf
	@LC_ALL=C $(READELF) -h $(BUILD)/iso/boot/kernel.elf | grep -q '$(ELF_CLASS_RE)' || { LC_ALL=C $(READELF) -h $(BUILD)/iso/boot/kernel.elf; echo "error: ISO kernel is not the expected $(ARCH)-class kernel"; false; }
	$(READELF) -l $(BUILD)/iso/boot/kernel.elf | grep -q 'LOAD'
	cp $(ROOTFS) $(BUILD)/iso/boot/root.tfs
	cp $(INSTALL_IMAGE) $(BUILD)/iso/boot/install.img
	cp $(GENERATED_GRUB) $(BUILD)/iso/boot/grub/grub.cfg
	cp $(GRUB_FONT) $(BUILD)/iso/boot/grub/fonts/unicode.pf2
	cp $(EFI_BOOT) $(BUILD)/iso/EFI/BOOT/BOOTX64.EFI
	@command -v $(MFORMAT) >/dev/null 2>&1 || { echo "error: $(GRUB_MKRESCUE) needs $(MFORMAT) from mtools; install the mtools package"; false; }
	$(GRUB_MKRESCUE) -V TNU_BOOT -o $@ $(BUILD)/iso
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/kernel.elf'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/root.tfs'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/install.img'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/grub/grub.cfg'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/grub/fonts/unicode.pf2'
	$(ISOINFO) -i $@ -R -f | grep -qx '/EFI/BOOT/BOOTX64.EFI'
	$(XORRISO) -indev $@ -report_el_torito plain 2>/dev/null | grep -q 'BIOS'
	$(XORRISO) -indev $@ -report_el_torito plain 2>/dev/null | grep -q 'UEFI'

verify: permission-tests verify-kernel verify-iso

permission-tests:
	$(HOSTPY) tools/permission_policy_test.py

ports-preflight:
	$(HOSTPY) tools/ports_preflight.py

ports-fetch: ports-fetch-core

# ports-fetch-core: fetch core dependencies.
# Xorg is not required for the current TNU console-only workflow.
ports-fetch-core:
	@echo "ports-fetch-core: skipping xorg"

verify-kernel: $(KERNEL)
	$(GRUB_FILE) --is-x86-multiboot2 $(KERNEL)
	@LC_ALL=C $(READELF) -h $(KERNEL) | grep -q '$(ELF_CLASS_RE)' || { LC_ALL=C $(READELF) -h $(KERNEL); echo "error: $(KERNEL) is not the expected $(ARCH)-class kernel"; false; }
	@LC_ALL=C $(READELF) -h $(KERNEL) | grep -q '$(ELF_MACHINE_RE)' || { LC_ALL=C $(READELF) -h $(KERNEL); echo "error: $(KERNEL) is not the expected $(ARCH) machine type"; false; }

verify-iso: $(ISO)
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/kernel.elf'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/root.tfs'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/install.img'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/grub/grub.cfg'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/grub/fonts/unicode.pf2'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/EFI/BOOT/BOOTX64.EFI'
	$(XORRISO) -indev $(ISO) -report_el_torito plain 2>/dev/null | grep -q 'BIOS'
	$(XORRISO) -indev $(ISO) -report_el_torito plain 2>/dev/null | grep -q 'UEFI'

# Linux chroot fetch - downloads Alpine Linux minirootfs for Linux ABI compatibility
ALPINE_VERSION := 3.20
ALPINE_RELEASE := 3.20.0
ALPINE_ARCH := x86_64
ALPINE_MIRROR := https://dl-cdn.alpinelinux.org/alpine/v$(ALPINE_VERSION)/releases/$(ALPINE_ARCH)
ALPINE_REPO_BASE := https://dl-cdn.alpinelinux.org/alpine/v$(ALPINE_VERSION)
ALPINE_TARBALL := alpine-minirootfs-$(ALPINE_RELEASE)-$(ALPINE_ARCH).tar.gz
ALPINE_URL := $(ALPINE_MIRROR)/$(ALPINE_TARBALL)
LINUX_CHROOT_DIR := $(BUILD)/linux-chroot
LINUX_CHROOT_TARBALL := $(BUILD)/downloads/$(ALPINE_TARBALL)
LINUX_CHROOT_REQUIRED_PACKAGES := nano fastfetch
LINUX_CHROOT_OPTIONAL_PACKAGES := freedoom
LINUX_CHROOT_PACKAGES := $(LINUX_CHROOT_REQUIRED_PACKAGES) $(LINUX_CHROOT_OPTIONAL_PACKAGES)
APK_RETRIES ?= 3

linux-chroot-fetch: $(LINUX_CHROOT_DIR)/bin/busybox

$(LINUX_CHROOT_TARBALL):
	@mkdir -p $(dir $@)
	@echo "linux-chroot: downloading Alpine Linux minirootfs..."
	@curl -fsSL -o $@ "$(ALPINE_URL)" || wget -q -O $@ "$(ALPINE_URL)" || \
		{ echo "linux-chroot: failed to download from $(ALPINE_URL)"; \
		  echo "linux-chroot: you can manually download $(ALPINE_TARBALL) and place it at $@"; \
		  false; }

$(LINUX_CHROOT_DIR)/bin/busybox: $(LINUX_CHROOT_TARBALL)
	@rm -rf $(LINUX_CHROOT_DIR)
	@mkdir -p $(LINUX_CHROOT_DIR)
	@echo "linux-chroot: extracting Alpine minirootfs..."
	@tar -xzf $(LINUX_CHROOT_TARBALL) -C $(LINUX_CHROOT_DIR)
	@touch $@
	@echo "linux-chroot: Alpine Linux chroot ready at $(LINUX_CHROOT_DIR)"

# Install packages into the Alpine chroot. nano/fastfetch are required for the
# beta Linux userspace; freedoom is optional because it lives in community and
# should not make the OS build fail when Alpine mirrors are temporarily flaky.
linux-chroot-packages: $(LINUX_CHROOT_DIR)/bin/busybox
	@echo "linux-chroot: Installing required Linux packages: $(LINUX_CHROOT_REQUIRED_PACKAGES)"
	@mkdir -p $(LINUX_CHROOT_DIR)/etc/apk
	@printf '%s\n%s\n' '$(ALPINE_REPO_BASE)/main' '$(ALPINE_REPO_BASE)/community' > $(LINUX_CHROOT_DIR)/etc/apk/repositories
	@if [ -f /etc/resolv.conf ]; then \
		cp /etc/resolv.conf $(LINUX_CHROOT_DIR)/etc/resolv.conf; \
	else \
		printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > $(LINUX_CHROOT_DIR)/etc/resolv.conf; \
	fi
	@if command -v apk >/dev/null 2>&1; then \
		echo "linux-chroot: Running host apk --root add $(LINUX_CHROOT_REQUIRED_PACKAGES)..."; \
		ok=0; n=1; while [ $$n -le $(APK_RETRIES) ]; do \
			apk --root $(LINUX_CHROOT_DIR) --initdb --no-cache \
				--repository '$(ALPINE_REPO_BASE)/main' \
				--repository '$(ALPINE_REPO_BASE)/community' \
				add $(LINUX_CHROOT_REQUIRED_PACKAGES) && { ok=1; break; }; \
			echo "linux-chroot: host apk attempt $$n failed, retrying..."; \
			n=$$((n + 1)); sleep 2; \
		done; \
		if [ $$ok -ne 1 ]; then \
			echo "linux-chroot: host apk failed, trying chroot apk..."; \
			ok=0; n=1; while [ $$n -le $(APK_RETRIES) ]; do \
				chroot $(LINUX_CHROOT_DIR) /bin/sh -c \
					'apk add --no-cache --repository "$(ALPINE_REPO_BASE)/main" --repository "$(ALPINE_REPO_BASE)/community" $(LINUX_CHROOT_REQUIRED_PACKAGES)' && { ok=1; break; }; \
				echo "linux-chroot: chroot apk attempt $$n failed, retrying..."; \
				n=$$((n + 1)); sleep 2; \
			done; \
			[ $$ok -eq 1 ]; \
		fi; \
	elif command -v chroot >/dev/null 2>&1; then \
		echo "linux-chroot: Running chroot apk add $(LINUX_CHROOT_REQUIRED_PACKAGES)..."; \
		ok=0; n=1; while [ $$n -le $(APK_RETRIES) ]; do \
			chroot $(LINUX_CHROOT_DIR) /bin/sh -c \
				'apk add --no-cache --repository "$(ALPINE_REPO_BASE)/main" --repository "$(ALPINE_REPO_BASE)/community" $(LINUX_CHROOT_REQUIRED_PACKAGES)' && { ok=1; break; }; \
			echo "linux-chroot: chroot apk attempt $$n failed, retrying..."; \
			n=$$((n + 1)); sleep 2; \
		done; \
		[ $$ok -eq 1 ]; \
	else \
		echo "linux-chroot: neither apk nor chroot is available; cannot preinstall Linux packages"; \
		false; \
	fi
	@if [ -n "$(strip $(LINUX_CHROOT_OPTIONAL_PACKAGES))" ]; then \
		echo "linux-chroot: Installing optional Linux packages: $(LINUX_CHROOT_OPTIONAL_PACKAGES)"; \
		if command -v apk >/dev/null 2>&1; then \
			apk --root $(LINUX_CHROOT_DIR) --initdb --no-cache \
				--repository '$(ALPINE_REPO_BASE)/main' \
				--repository '$(ALPINE_REPO_BASE)/community' \
				add $(LINUX_CHROOT_OPTIONAL_PACKAGES) || \
				echo "linux-chroot: optional packages unavailable; continuing without $(LINUX_CHROOT_OPTIONAL_PACKAGES)."; \
		elif command -v chroot >/dev/null 2>&1; then \
			chroot $(LINUX_CHROOT_DIR) /bin/sh -c \
				'apk add --no-cache --repository "$(ALPINE_REPO_BASE)/main" --repository "$(ALPINE_REPO_BASE)/community" $(LINUX_CHROOT_OPTIONAL_PACKAGES)' || \
				echo "linux-chroot: optional packages unavailable; continuing without $(LINUX_CHROOT_OPTIONAL_PACKAGES)."; \
		fi; \
	fi
	@if [ -x "$(LINUX_CHROOT_DIR)/usr/bin/nano" ] && [ -x "$(LINUX_CHROOT_DIR)/usr/bin/fastfetch" ]; then \
		echo "linux-chroot: Packages installed."; \
	else \
		echo "linux-chroot: package install incomplete; nano/fastfetch missing from Linux chroot."; \
		echo "linux-chroot: check DNS/mirror access from the build host, or override ALPINE_REPO_BASE / APK_RETRIES."; \
		false; \
	fi
	@echo "linux-chroot: Nano is at /usr/linux/usr/bin/nano"
	@echo "linux-chroot: Fastfetch is at /usr/linux/usr/bin/fastfetch"
	@echo "linux-chroot: Freedoom WAD files are at /usr/linux/usr/share/games/doom/"

linux-chroot: $(LINUX_CHROOT_DIR)/bin/busybox
	@echo "linux-chroot: Alpine Linux chroot ready at $(LINUX_CHROOT_DIR)"

