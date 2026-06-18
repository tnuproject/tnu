#include <tnu/elf.h>
#include <tnu/string.h>

#define EI_NIDENT 16
#define PT_LOAD 1
#define EM_X86_64 62

struct elf64_ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

int elf64_validate(const void *image, size_t size, struct elf_image_info *info)
{
    if (!image || size < sizeof(struct elf64_ehdr)) {
        return -1;
    }
    const struct elf64_ehdr *eh = image;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != 2 || eh->e_ident[5] != 1 ||
        eh->e_machine != EM_X86_64) {
        return -1;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size ||
        eh->e_phentsize < sizeof(struct elf64_phdr)) {
        return -1;
    }

    if (info) {
        memset(info, 0, sizeof(*info));
        info->entry = eh->e_entry;
        info->phnum = eh->e_phnum;
        info->type = eh->e_type;
        info->lowest_vaddr = UINT64_MAX;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > size || ph->p_memsz < ph->p_filesz) {
            return -1;
        }
        if (info) {
            if (ph->p_vaddr < info->lowest_vaddr) {
                info->lowest_vaddr = ph->p_vaddr;
            }
            if (ph->p_vaddr + ph->p_memsz > info->highest_vaddr) {
                info->highest_vaddr = ph->p_vaddr + ph->p_memsz;
            }
        }
    }
    if (info && info->lowest_vaddr == UINT64_MAX) {
        info->lowest_vaddr = 0;
    }
    return 0;
}

/*
 * Userspace load region: [USER_LOAD_MIN, USER_LOAD_MAX).
 * Must cover the code+heap+stack windows mapped by sys_exec_image.
 * Large binaries (nano, doom) extend well past 0x8000000 into heap space.
 */
#define USER_LOAD_MIN 0x400000ULL
#define USER_LOAD_MAX 0x38000000ULL

int elf64_load(const void *image, size_t size)
{
    struct elf_image_info info;
    if (elf64_validate(image, size, &info) < 0) {
        return -1;
    }
    if (info.lowest_vaddr < USER_LOAD_MIN || info.highest_vaddr > USER_LOAD_MAX) {
        return -1;
    }

    const struct elf64_ehdr *eh = image;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        void *dest = (void *)(uintptr_t)ph->p_vaddr;
        memcpy(dest, (const uint8_t *)image + ph->p_offset, (size_t)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset((uint8_t *)dest + ph->p_filesz, 0, (size_t)(ph->p_memsz - ph->p_filesz));
        }
    }
    return 0;
}
