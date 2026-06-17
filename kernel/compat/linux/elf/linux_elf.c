#include <tnu/linux_compat.h>
#include <tnu/string.h>

#define EI_NIDENT 16
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_DYN 3
#define EM_386 3
#define EM_X86_64 62

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_TLS 7
#define PT_GNU_STACK 0x6474e551u
#define PT_GNU_RELRO 0x6474e552u

struct elf32_ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

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

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
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

static bool valid_ident(const unsigned char ident[EI_NIDENT], unsigned char klass)
{
    return ident[0] == ELFMAG0 && ident[1] == ELFMAG1 &&
           ident[2] == ELFMAG2 && ident[3] == ELFMAG3 &&
           ident[4] == klass && ident[5] == ELFDATA2LSB;
}

static int note_segment(struct linux_elf_info *info, uint32_t type, uint32_t flags,
                        uint64_t offset, uint64_t vaddr, uint64_t filesz,
                        uint64_t memsz, size_t image_size)
{
    if (offset + filesz > image_size || memsz < filesz) {
        return -1;
    }
    switch (type) {
    case PT_LOAD:
        if (vaddr < info->lowest_vaddr) {
            info->lowest_vaddr = vaddr;
        }
        if (vaddr + memsz > info->highest_vaddr) {
            info->highest_vaddr = vaddr + memsz;
        }
        break;
    case PT_INTERP:
        info->has_interp = true;
        info->interp_offset = offset;
        info->interp_size = filesz;
        break;
    case PT_TLS:
        info->has_tls = true;
        info->tls_vaddr = vaddr;
        info->tls_filesz = filesz;
        info->tls_memsz = memsz;
        break;
    case PT_GNU_STACK:
        info->has_gnu_stack = true;
        info->stack_flags = flags;
        break;
    case PT_GNU_RELRO:
        info->has_relro = true;
        info->relro_vaddr = vaddr;
        info->relro_memsz = memsz;
        break;
    default:
        break;
    }
    return 0;
}

static int probe32(const void *image, size_t size, struct linux_elf_info *info)
{
    const struct elf32_ehdr *eh = image;
    if (size < sizeof(*eh) || !valid_ident(eh->e_ident, ELFCLASS32) ||
        eh->e_machine != EM_386 || eh->e_phentsize < sizeof(struct elf32_phdr) ||
        (uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        return -1;
    }
    info->elf_class = LINUX_ELF32;
    info->machine = eh->e_machine;
    info->type = eh->e_type;
    info->entry = eh->e_entry;
    info->phoff = eh->e_phoff;
    info->phentsize = eh->e_phentsize;
    info->phnum = eh->e_phnum;
    info->is_pie = eh->e_type == ET_DYN;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf32_phdr *ph =
            (const struct elf32_phdr *)((const uint8_t *)image + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);
        if (note_segment(info, ph->p_type, ph->p_flags, ph->p_offset, ph->p_vaddr,
                         ph->p_filesz, ph->p_memsz, size) < 0) {
            return -1;
        }
    }
    return 0;
}

static int probe64(const void *image, size_t size, struct linux_elf_info *info)
{
    const struct elf64_ehdr *eh = image;
    if (size < sizeof(*eh) || !valid_ident(eh->e_ident, ELFCLASS64) ||
        eh->e_machine != EM_X86_64 || eh->e_phentsize < sizeof(struct elf64_phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        return -1;
    }
    info->elf_class = LINUX_ELF64;
    info->machine = eh->e_machine;
    info->type = eh->e_type;
    info->entry = eh->e_entry;
    info->phoff = eh->e_phoff;
    info->phentsize = eh->e_phentsize;
    info->phnum = eh->e_phnum;
    info->is_pie = eh->e_type == ET_DYN;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);
        if (note_segment(info, ph->p_type, ph->p_flags, ph->p_offset, ph->p_vaddr,
                         ph->p_filesz, ph->p_memsz, size) < 0) {
            return -1;
        }
    }
    return 0;
}

int linux_elf_probe(const void *image, size_t size, struct linux_elf_info *info)
{
    if (!image || !info || size < EI_NIDENT) {
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->lowest_vaddr = UINT64_MAX;

    const unsigned char *ident = image;
    int rc;
    if (ident[4] == ELFCLASS32) {
        rc = probe32(image, size, info);
    } else if (ident[4] == ELFCLASS64) {
        rc = probe64(image, size, info);
    } else {
        return -1;
    }

    if (rc == 0 && info->lowest_vaddr == UINT64_MAX) {
        info->lowest_vaddr = 0;
    }
    return rc;
}
