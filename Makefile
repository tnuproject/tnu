ARCH := x86_64
PROJECT := tnu

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

BUILD := build
KERNEL := $(BUILD)/kernel.elf
ROOTFS := $(BUILD)/root.tfs
ISO := $(BUILD)/tnu.iso
EFI_BOOT := $(BUILD)/BOOTX64.EFI
INSTALL_IMAGE := $(BUILD)/install.img

KERNEL_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding \
	-fno-stack-protector -fno-stack-check -fno-pic -fno-omit-frame-pointer \
	-fno-builtin -m64 -mno-red-zone -mgeneral-regs-only \
	-Ikernel/include -Ikernel/arch/x86_64/include
KERNEL_ASFLAGS := $(KERNEL_CFLAGS)
KERNEL_LDFLAGS := -T kernel/linker.ld -nostdlib -static -no-pie \
	-Wl,-z,max-page-size=0x1000 -Wl,--build-id=none

USER_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding \
	-fno-stack-protector -fno-builtin -fno-pic -m64 -mno-red-zone \
	-Iuserspace/libc/include
USER_LDFLAGS := -T userspace/linker.ld -nostdlib -static -no-pie \
	-Wl,-z,max-page-size=0x1000

KERNEL_C := $(shell find kernel -name '*.c' | sort)
KERNEL_S := $(shell find kernel -name '*.S' | sort)
KERNEL_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(KERNEL_C)) \
	$(patsubst %.S,$(BUILD)/obj/%.o,$(KERNEL_S))

LIBC_C := $(shell find userspace/libc/src -name '*.c' | sort)
LIBC_S := $(shell find userspace/libc/src -name '*.S' | sort)
LIBC_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(LIBC_C)) \
	$(patsubst %.S,$(BUILD)/obj/%.o,$(LIBC_S))
USER_CRT := $(BUILD)/obj/userspace/libc/src/crt0.o
USER_LIB := $(BUILD)/user/libtnu.a

COREUTIL_NAMES := cat chmod chown clear cp date dmesg echo hostname \
	id ifconfig keymap kill ls mkdir mount mv xedit netstat ping ps pwd reboot rm route \
	shutdown stat sysfetch time timezone touch uname uptime usb whoami wifi

.PHONY: all kernel userspace iso run clean rootfs verify verify-kernel verify-iso ports-preflight

all: iso

kernel: $(KERNEL)

userspace: $(USER_LIB) $(BUILD)/user/init $(BUILD)/user/tsh \
	$(BUILD)/user/tnu-utils $(BUILD)/user/login $(BUILD)/user/passwd \
	$(BUILD)/user/useradd $(BUILD)/user/userdel $(BUILD)/user/sysinstall

iso: $(ISO)

run: $(ISO)
	$(QEMU) -m 256M -cdrom $(ISO) -vga std -serial stdio -no-reboot -no-shutdown

clean:
	rm -rf $(BUILD)

$(KERNEL): $(KERNEL_OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS) -lgcc
	$(GRUB_FILE) --is-x86-multiboot2 $@
	$(READELF) -h $@ | grep -q 'Class:.*ELF64'
	$(READELF) -h $@ | grep -q 'Machine:.*Advanced Micro Devices X86-64'

$(BUILD)/obj/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/obj/kernel/%.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_ASFLAGS) -c $< -o $@

$(BUILD)/obj/userspace/%.o: userspace/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/obj/userspace/%.o: userspace/%.S
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

rootfs: userspace
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs
	cp -a rootfs/. $(BUILD)/rootfs/
	cp ascii.txt $(BUILD)/rootfs/etc/sysfetch-logo
	mkdir -p $(BUILD)/rootfs/bin $(BUILD)/rootfs/sbin $(BUILD)/rootfs/usr/bin
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

$(ROOTFS): rootfs tools/mktfs.py
	$(HOSTPY) tools/mktfs.py $(BUILD)/rootfs $@

$(EFI_BOOT): boot/grub/grub.cfg
	@mkdir -p $(dir $@)
	$(GRUB_MKSTANDALONE) -O x86_64-efi -o $@ \
		--modules="part_gpt part_msdos iso9660 fat search search_fs_file test normal boot multiboot2 font gfxterm all_video efi_gop efi_uga video video_fb video_bochs video_cirrus" \
		"boot/grub/grub.cfg=boot/grub/grub.cfg"

$(INSTALL_IMAGE): $(KERNEL) $(ROOTFS) boot/grub/grub.cfg $(EFI_BOOT)
	rm -rf $(BUILD)/install-iso
	mkdir -p $(BUILD)/install-iso/boot/grub/fonts $(BUILD)/install-iso/EFI/BOOT
	cp $(KERNEL) $(BUILD)/install-iso/boot/kernel.elf
	cp $(ROOTFS) $(BUILD)/install-iso/boot/root.tfs
	cp boot/grub/grub.cfg $(BUILD)/install-iso/boot/grub/grub.cfg
	cp $(GRUB_FONT) $(BUILD)/install-iso/boot/grub/fonts/unicode.pf2
	cp $(EFI_BOOT) $(BUILD)/install-iso/EFI/BOOT/BOOTX64.EFI
	$(GRUB_MKRESCUE) -V TNU_BOOT -o $@ $(BUILD)/install-iso
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/kernel.elf'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/root.tfs'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/grub/grub.cfg'
	$(ISOINFO) -i $@ -R -f | grep -qx '/boot/grub/fonts/unicode.pf2'
	$(ISOINFO) -i $@ -R -f | grep -qx '/EFI/BOOT/BOOTX64.EFI'

$(ISO): $(KERNEL) $(ROOTFS) boot/grub/grub.cfg $(EFI_BOOT) $(INSTALL_IMAGE)
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/grub/fonts $(BUILD)/iso/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso/boot/kernel.elf
	cp $(ROOTFS) $(BUILD)/iso/boot/root.tfs
	cp $(INSTALL_IMAGE) $(BUILD)/iso/boot/install.img
	cp boot/grub/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
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

verify: verify-kernel verify-iso

ports-preflight:
	$(HOSTPY) tools/ports_preflight.py

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
