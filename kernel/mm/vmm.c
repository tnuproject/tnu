#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/string.h>

extern uint64_t pml4_table[];

#define PTE_PRESENT 0x001ULL
#define PTE_WRITABLE 0x002ULL
#define PTE_USER 0x004ULL
#define PTE_HUGE 0x080ULL

#define PAGE_TABLE_ENTRIES 512
#define HUGE_PAGE_SIZE (2ULL * 1024ULL * 1024ULL)
#define HUGE_PAGE_MASK (~(HUGE_PAGE_SIZE - 1ULL))
#define TABLE_ADDR_MASK 0x000ffffffffff000ULL
#define LOWER_HALF_LIMIT 0x0000800000000000ULL

static uint64_t extra_pdpt_tables[8][PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t extra_pd_tables[64][PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static size_t next_pdpt_table;
static size_t next_pd_table;

static uint64_t *alloc_pdpt_table(void)
{
    if (next_pdpt_table >= sizeof(extra_pdpt_tables) / sizeof(extra_pdpt_tables[0])) {
        return NULL;
    }
    uint64_t *table = extra_pdpt_tables[next_pdpt_table++];
    memset(table, 0, PAGE_SIZE);
    return table;
}

static uint64_t *alloc_pd_table(void)
{
    if (next_pd_table >= sizeof(extra_pd_tables) / sizeof(extra_pd_tables[0])) {
        return NULL;
    }
    uint64_t *table = extra_pd_tables[next_pd_table++];
    memset(table, 0, PAGE_SIZE);
    return table;
}

static uint64_t *table_from_entry(uint64_t entry)
{
    return (uint64_t *)(uintptr_t)(entry & TABLE_ADDR_MASK);
}

static void reload_cr3(void)
{
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3) :: "memory");
}

static int map_huge_identity(uintptr_t addr, uint64_t flags)
{
    uint64_t aligned = (uint64_t)addr & HUGE_PAGE_MASK;
    if (aligned >= LOWER_HALF_LIMIT) {
        return -1;
    }

    size_t pml4_index = (size_t)((aligned >> 39) & 0x1ff);
    size_t pdpt_index = (size_t)((aligned >> 30) & 0x1ff);
    size_t pd_index = (size_t)((aligned >> 21) & 0x1ff);

    if (!(pml4_table[pml4_index] & PTE_PRESENT)) {
        uint64_t *new_pdpt = alloc_pdpt_table();
        if (!new_pdpt) {
            return -1;
        }
        pml4_table[pml4_index] = ((uint64_t)(uintptr_t)new_pdpt) |
                                  PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else {
        pml4_table[pml4_index] |= flags & PTE_USER;
    }

    uint64_t *pdpt = table_from_entry(pml4_table[pml4_index]);
    if (!(pdpt[pdpt_index] & PTE_PRESENT)) {
        uint64_t *new_pd = alloc_pd_table();
        if (!new_pd) {
            return -1;
        }
        pdpt[pdpt_index] = ((uint64_t)(uintptr_t)new_pd) |
                            PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else {
        pdpt[pdpt_index] |= flags & PTE_USER;
    }

    uint64_t *pd = table_from_entry(pdpt[pdpt_index]);
    if (!(pd[pd_index] & PTE_PRESENT)) {
        pd[pd_index] = aligned | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE | flags;
    } else {
        pd[pd_index] |= flags;
    }
    return 0;
}

void vmm_init(void)
{
    log_info("vmm", "bootstrap page table at %p, identity mapped first 1 GiB", pml4_table);
}

int vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags)
{
    if (virt != phys) {
        return -1;
    }
    return map_huge_identity(phys, flags);
}

int vmm_map_range_identity(uintptr_t phys, size_t length, uint64_t flags)
{
    if (!length) {
        return 0;
    }

    uint64_t start = (uint64_t)phys & HUGE_PAGE_MASK;
    uint64_t end = ((uint64_t)phys + length + HUGE_PAGE_SIZE - 1ULL) & HUGE_PAGE_MASK;
    if (end < start || end > LOWER_HALF_LIMIT) {
        return -1;
    }

    for (uint64_t addr = start; addr < end; addr += HUGE_PAGE_SIZE) {
        if (map_huge_identity((uintptr_t)addr, flags) < 0) {
            return -1;
        }
    }
    reload_cr3();
    return 0;
}
