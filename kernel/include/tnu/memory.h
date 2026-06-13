#ifndef TNU_MEMORY_H
#define TNU_MEMORY_H

#include <tnu/multiboot2.h>
#include <tnu/types.h>

#define PAGE_SIZE 4096UL
#define VMM_FLAG_WRITABLE 0x002ULL
#define VMM_FLAG_USER     0x004ULL

struct memory_stats {
    uint64_t total_bytes;
    uint64_t usable_bytes;
    uint64_t allocated_frames;
    uint64_t free_frames_estimate;
    uint64_t heap_used;
    uint64_t heap_size;
};

void pmm_init(const struct boot_info *boot);
uintptr_t pmm_alloc_frame(void);
const struct memory_stats *memory_stats_get(void);

void vmm_init(void);
int vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
int vmm_map_range_identity(uintptr_t phys, size_t length, uint64_t flags);

void heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t old_size, size_t new_size);

#endif
