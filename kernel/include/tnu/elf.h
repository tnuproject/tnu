#ifndef TNU_ELF_H
#define TNU_ELF_H

#include <tnu/types.h>

struct elf_image_info {
    uint64_t entry;
    uint16_t phnum;
    uint16_t type;
    uint64_t lowest_vaddr;
    uint64_t highest_vaddr;
};

int elf64_validate(const void *image, size_t size, struct elf_image_info *info);

#endif
