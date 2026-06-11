#include <tnu/log.h>
#include <tnu/multiboot2.h>
#include <tnu/panic.h>
#include <tnu/string.h>

static struct boot_info boot;

static uintptr_t align8(uintptr_t value)
{
    return (value + 7u) & ~((uintptr_t)7u);
}

static bool bounded_contains(const char *haystack, size_t haystack_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || haystack_len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack_len && haystack[i]; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

void boot_info_parse(uint32_t magic, uintptr_t mbi_addr)
{
    memset(&boot, 0, sizeof(boot));

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        panic("bootloader did not provide Multiboot2 magic");
    }

    const struct multiboot_info *mbi = (const struct multiboot_info *)mbi_addr;
    if (mbi->total_size < sizeof(struct multiboot_info) || mbi->total_size > 16 * 1024 * 1024) {
        panic("Multiboot2 information block has an invalid size");
    }
    boot.mbi = mbi;
    uintptr_t tag_addr = mbi_addr + sizeof(struct multiboot_info);
    uintptr_t end = mbi_addr + mbi->total_size;

    while (tag_addr < end) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)tag_addr;
        if (tag->size < sizeof(struct multiboot_tag)) {
            panic("Multiboot2 tag has an invalid size");
        }
        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            break;
        }

        switch (tag->type) {
        case MULTIBOOT_TAG_TYPE_MODULE: {
            const struct multiboot_tag_module *module = (const struct multiboot_tag_module *)tag;
            size_t cmdline_len = tag->size > 16 ? tag->size - 16 : 0;
            if (bounded_contains(module->cmdline, cmdline_len, "install.img") ||
                bounded_contains(module->cmdline, cmdline_len, "tnu-install-image")) {
                boot.install_image.start = module->mod_start;
                boot.install_image.end = module->mod_end;
                boot.install_image.cmdline = module->cmdline;
            } else if (!boot.rootfs.start || bounded_contains(module->cmdline, cmdline_len, "root.tfs")) {
                boot.rootfs.start = module->mod_start;
                boot.rootfs.end = module->mod_end;
                boot.rootfs.cmdline = module->cmdline;
            }
            break;
        }
        case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO: {
            const uint32_t *mem = (const uint32_t *)((const uint8_t *)tag + 8);
            boot.mem_lower_kib = mem[0];
            boot.mem_upper_kib = mem[1];
            break;
        }
        case MULTIBOOT_TAG_TYPE_MMAP:
            boot.mmap = (const struct multiboot_tag_mmap *)tag;
            break;
        case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
            const struct multiboot_tag_framebuffer *fb = (const struct multiboot_tag_framebuffer *)tag;
            boot.framebuffer_addr = fb->framebuffer_addr;
            boot.framebuffer_width = fb->framebuffer_width;
            boot.framebuffer_height = fb->framebuffer_height;
            boot.framebuffer_pitch = fb->framebuffer_pitch;
            boot.framebuffer_bpp = fb->framebuffer_bpp;
            break;
        }
        default:
            break;
        }

        uintptr_t next_tag = align8(tag_addr + tag->size);
        if (next_tag <= tag_addr || next_tag > end) {
            panic("Multiboot2 tag walk advanced outside the information block");
        }
        tag_addr = next_tag;
    }

    log_info("boot", "Multiboot2 info at %p, rootfs %p-%p install %p-%p",
             (void *)mbi_addr, (void *)boot.rootfs.start, (void *)boot.rootfs.end,
             (void *)boot.install_image.start, (void *)boot.install_image.end);
}

const struct boot_info *boot_info_get(void)
{
    return &boot;
}
