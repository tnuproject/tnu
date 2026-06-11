#ifndef TNU_MULTIBOOT2_H
#define TNU_MULTIBOOT2_H

#include <tnu/types.h>

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289

enum {
    MULTIBOOT_TAG_TYPE_END = 0,
    MULTIBOOT_TAG_TYPE_CMDLINE = 1,
    MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME = 2,
    MULTIBOOT_TAG_TYPE_MODULE = 3,
    MULTIBOOT_TAG_TYPE_BASIC_MEMINFO = 4,
    MULTIBOOT_TAG_TYPE_BOOTDEV = 5,
    MULTIBOOT_TAG_TYPE_MMAP = 6,
    MULTIBOOT_TAG_TYPE_FRAMEBUFFER = 8,
};

struct multiboot_info {
    uint32_t total_size;
    uint32_t reserved;
};

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];
};

struct multiboot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry entries[];
};

struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
};

struct boot_module {
    uintptr_t start;
    uintptr_t end;
    const char *cmdline;
};

struct boot_info {
    const struct multiboot_info *mbi;
    const struct multiboot_tag_mmap *mmap;
    struct boot_module rootfs;
    struct boot_module install_image;
    uint64_t mem_lower_kib;
    uint64_t mem_upper_kib;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint8_t framebuffer_bpp;
};

void boot_info_parse(uint32_t magic, uintptr_t mbi_addr);
const struct boot_info *boot_info_get(void);

#endif
