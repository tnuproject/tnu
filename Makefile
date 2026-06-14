include version.mk

ARCH := $(TNU_ARCH)
PROJECT := $(TNU_PROJECT)

ifeq ($(origin CC),default)
CC := $(shell command -v x86_64-elf-gcc >/dev/null 2>&1 && echo x86_64-elf-gcc || echo gcc)
endif
ifeq ($(origin AR),default)
AR := $(shell command -v x86_64-elf-ar >/dev/null 2>&1 && echo x86_64-elf-ar || echo ar)
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
GIT ?= git
PATCH ?= patch

# Upstream source mirrors used only when the local clones are missing.
# GNU nano official source repo: https://www.nano-editor.org/git.php
NANO_UPSTREAM_URL ?= https://git.savannah.gnu.org/git/nano.git
NANO_UPSTREAM_DIR ?= ports/nano/upstream

# Freedoom official GitHub repo: https://github.com/freedoom/freedoom
FREEDOOM_UPSTREAM_URL ?= https://github.com/freedoom/freedoom.git
FREEDOOM_UPSTREAM_DIR ?= ports/doom/freedoom

DOOM_TNU_PATCH ?= patches/doom/i_video_tnu_fast.patch
DOOM_UPSTREAM_VIDEO ?= ports/doom/upstream/i_video.c

BUILD := build
KERNEL := $(BUILD)/kernel.elf
ROOTFS := $(BUILD)/root.tfs
ISO := $(BUILD)/$(TNU_ISO_NAME)
EFI_BOOT := $(BUILD)/BOOTX64.EFI
INSTALL_IMAGE := $(BUILD)/install.img
GENERATED := $(BUILD)/generated
GENERATED_VERSION := $(GENERATED)/include/tnu/version.h
GENERATED_GRUB := $(GENERATED)/boot/grub/grub.cfg

KERNEL_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding \
	-fno-stack-protector -fno-stack-check -fno-pic -fno-omit-frame-pointer \
	-fno-builtin -m64 -mno-red-zone -mgeneral-regs-only \
	-I$(GENERATED)/include -Ikernel/include -Ikernel/arch/x86_64/include
KERNEL_ASFLAGS := $(KERNEL_CFLAGS)
KERNEL_LDFLAGS := -T kernel/linker.ld -nostdlib -static -no-pie \
	-Wl,-z,max-page-size=0x1000 -Wl,--build-id=none

USER_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding \
	-fno-stack-protector -fno-builtin -fno-pic -m64 -mno-red-zone \
	-Iuserspace/libc/include -Ikernel/include
USER_LDFLAGS := -T userspace/linker.ld -nostdlib -static -no-pie \
	-Wl,-z,max-page-size=0x1000

KERNEL_C := $(shell find kernel -name '*.c' | sort)
# tss.c is automatically included by the recursive kernel source scan
KERNEL_S := $(shell find kernel -name '*.S' | sort)
KERNEL_HEADERS := $(shell find kernel/include kernel/arch/x86_64/include -name '*.h' | sort) \
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

COREUTIL_NAMES := cat chmod chown clear cp date dmesg echo hostname \
	id ifconfig keymap kill ls mkdir mount mv xedit netstat ping ps pwd reboot rm route dhcp \
	shutdown stat sysfetch time timezone touch uname uptime usb whoami wifi

IWN_FW_SRC := $(shell find freebsd-src/sys/contrib/dev/iwn freebsd-src/sys/contrib/dev/wpi -maxdepth 1 -name '*.fw.uu' 2>/dev/null | sort)
IWM_FW_SRC := $(shell find freebsd-src/sys/contrib/dev/iwm -maxdepth 1 -name '*.fw' 2>/dev/null | sort)
LINUX_IWL_FW_SRC := $(shell find rootfs/lib/firmware rootfs/lib/firmware/iwlwifi /lib/firmware /lib/firmware/iwlwifi -maxdepth 1 \( -name 'iwlwifi-*.ucode' -o -name 'iwlwifi-*.fw' \) 2>/dev/null | sort -u)

.PHONY: all kernel userspace iso run clean rootfs version-files permission-tests verify verify-kernel verify-iso \
	firmware-iwlwifi ports-preflight ports-fetch ports-fetch-core \
	nano doom fastfetch ports-fetch-nano ports-fetch-freedoom ports-fetch-doom-upstream ports-patch-doom

all: ports-fetch iso

kernel: $(KERNEL)

userspace: $(USER_LIB) $(BUILD)/user/init $(BUILD)/user/tsh \
	$(BUILD)/user/tnu-utils $(BUILD)/user/login $(BUILD)/user/passwd \
	$(BUILD)/user/useradd $(BUILD)/user/userdel $(BUILD)/user/sysinstall \
	$(BUILD)/user/bootd $(BUILD)/user/dhclient $(BUILD)/user/httpget \
	$(BUILD)/user/nano $(BUILD)/user/doom $(BUILD)/user/fastfetch

nano: $(BUILD)/user/nano
doom: $(BUILD)/user/doom
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
	$(READELF) -h $@ | grep -q 'Class:.*ELF64'
	$(READELF) -h $@ | grep -q 'Machine:.*Advanced Micro Devices X86-64'

$(BUILD)/obj/kernel/%.o: kernel/%.c $(KERNEL_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

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

$(BUILD)/user/nano: $(USER_LIB) $(USER_CRT) userspace/linker.ld \
	$(shell find ports/nano/src -type f 2>/dev/null) | ports-fetch-nano
	$(MAKE) -C ports/nano CC="$(CC)" \
		USER_CRT="$(abspath $(USER_CRT))" \
		USER_LIB="$(abspath $(USER_LIB))"

$(BUILD)/user/doom: $(USER_LIB) $(USER_CRT) userspace/linker.ld \
	$(shell find ports/doom/src -type f 2>/dev/null) | ports-fetch-freedoom ports-fetch-doom-upstream ports-patch-doom
	$(MAKE) -C ports/doom CC="$(CC)" \
		USER_CRT="$(abspath $(USER_CRT))" \
		USER_LIB="$(abspath $(USER_LIB))"

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
		$(BUILD)/rootfs/lib/firmware/iwlwifi
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
	cp $(BUILD)/user/nano $(BUILD)/rootfs/bin/nano
	cp $(BUILD)/user/doom $(BUILD)/rootfs/usr/games/doom
	cp $(BUILD)/user/fastfetch $(BUILD)/rootfs/usr/bin/fastfetch
	cp -a $(BUILD)/firmware/iwlwifi/. $(BUILD)/rootfs/lib/firmware/iwlwifi/

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
	$(READELF) -h $(BUILD)/install-iso/boot/kernel.elf | grep -q 'Class:.*ELF64'
	$(READELF) -l $(BUILD)/install-iso/boot/kernel.elf | grep -q 'LOAD'
	cp $(ROOTFS) $(BUILD)/install-iso/boot/root.tfs
	cp $(GENERATED_GRUB) $(BUILD)/install-iso/boot/grub/grub.cfg
	cp $(GRUB_FONT) $(BUILD)/install-iso/boot/grub/fonts/unicode.pf2
	cp $(EFI_BOOT) $(BUILD)/install-iso/EFI/BOOT/BOOTX64.EFI
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
	$(READELF) -h $(BUILD)/iso/boot/kernel.elf | grep -q 'Class:.*ELF64'
	$(READELF) -l $(BUILD)/iso/boot/kernel.elf | grep -q 'LOAD'
	cp $(ROOTFS) $(BUILD)/iso/boot/root.tfs
	cp $(INSTALL_IMAGE) $(BUILD)/iso/boot/install.img
	cp $(GENERATED_GRUB) $(BUILD)/iso/boot/grub/grub.cfg
	cp $(GRUB_FONT) $(BUILD)/iso/boot/grub/fonts/unicode.pf2
	cp $(EFI_BOOT) $(BUILD)/iso/EFI/BOOT/BOOTX64.EFI
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

ports-fetch: ports-fetch-core ports-fetch-nano ports-fetch-freedoom ports-fetch-doom-upstream ports-patch-doom

ports-fetch-core:
	$(HOSTPY) tools/ports_fetch.py xorg

ports-fetch-nano:
	@mkdir -p $(dir $(NANO_UPSTREAM_DIR))
	@if [ -d "$(NANO_UPSTREAM_DIR)/.git" ]; then \
		echo "nano upstream already cloned at $(NANO_UPSTREAM_DIR)"; \
	else \
		echo "cloning GNU nano upstream into $(NANO_UPSTREAM_DIR)"; \
		rm -rf "$(NANO_UPSTREAM_DIR)"; \
		$(GIT) clone --depth 1 "$(NANO_UPSTREAM_URL)" "$(NANO_UPSTREAM_DIR)"; \
	fi

ports-fetch-freedoom:
	@mkdir -p $(dir $(FREEDOOM_UPSTREAM_DIR))
	@if [ -d "$(FREEDOOM_UPSTREAM_DIR)/.git" ]; then \
		echo "Freedoom upstream already cloned at $(FREEDOOM_UPSTREAM_DIR)"; \
	else \
		echo "cloning Freedoom upstream into $(FREEDOOM_UPSTREAM_DIR)"; \
		rm -rf "$(FREEDOOM_UPSTREAM_DIR)"; \
		$(GIT) clone --depth 1 "$(FREEDOOM_UPSTREAM_URL)" "$(FREEDOOM_UPSTREAM_DIR)"; \
	fi

ports-fetch-doom-upstream:
	@if [ -f "$(DOOM_UPSTREAM_VIDEO)" ]; then \
		echo "Doom upstream already available at ports/doom/upstream"; \
	else \
		echo "Doom upstream missing; trying tools/ports_fetch.py doom"; \
		$(HOSTPY) tools/ports_fetch.py doom || true; \
	fi

ports-patch-doom: ports-fetch-doom-upstream
	@if [ ! -f "$(DOOM_TNU_PATCH)" ]; then \
		echo "warning: missing $(DOOM_TNU_PATCH); Doom TNU video patch was not applied"; \
	elif [ ! -f "$(DOOM_UPSTREAM_VIDEO)" ]; then \
		echo "warning: missing $(DOOM_UPSTREAM_VIDEO); run make ports-fetch or check ports/doom upstream fetch"; \
	elif grep -q "TNU_DOOM_FAST_NATIVE" "$(DOOM_UPSTREAM_VIDEO)"; then \
		echo "Doom TNU fast video patch already applied"; \
	else \
		echo "applying Doom TNU fast video patch"; \
		$(PATCH) -p0 < "$(DOOM_TNU_PATCH)" || $(PATCH) -p1 < "$(DOOM_TNU_PATCH)"; \
	fi

verify-kernel: $(KERNEL)
	$(GRUB_FILE) --is-x86-multiboot2 $(KERNEL)
	$(READELF) -h $(KERNEL) | grep -q 'Class:.*ELF64'
	$(READELF) -h $(KERNEL) | grep -q 'Machine:.*Advanced Micro Devices X86-64'

verify-iso: $(ISO)
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/kernel.elf'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/root.tfs'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/install.img'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/grub/grub.cfg'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/boot/grub/fonts/unicode.pf2'
	$(ISOINFO) -i $(ISO) -R -f | grep -qx '/EFI/BOOT/BOOTX64.EFI'
	$(XORRISO) -indev $(ISO) -report_el_torito plain 2>/dev/null | grep -q 'BIOS'
	$(XORRISO) -indev $(ISO) -report_el_torito plain 2>/dev/null | grep -q 'UEFI'
