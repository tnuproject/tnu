#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/printf.h>

extern char kernel_end;

static struct memory_stats stats;
static uintptr_t frame_cursor;
static uintptr_t frame_limit;
#define PMM_FREE_STACK_MAX 65536
static uintptr_t free_stack[PMM_FREE_STACK_MAX];
static size_t free_stack_count;

static uintptr_t align_up(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void pmm_init(const struct boot_info *boot)
{
    stats.total_bytes = 0;
    stats.usable_bytes = 0;
    stats.allocated_frames = 0;
    stats.free_frames_estimate = 0;

    uintptr_t best_start = 0;
    uint64_t best_len = 0;
    uint64_t highest_usable_limit = 0;

    if (boot->mmap) {
        const uint8_t *entry = (const uint8_t *)boot->mmap->entries;
        const uint8_t *end = (const uint8_t *)boot->mmap + boot->mmap->size;
        while (entry < end) {
            const struct multiboot_mmap_entry *m = (const struct multiboot_mmap_entry *)entry;
            if (m->type == 1) {
                uintptr_t start = (uintptr_t)m->addr;
                uintptr_t limit = (uintptr_t)(m->addr + m->len);
                if (limit > highest_usable_limit) {
                    highest_usable_limit = limit;
                }
                if (limit > 0x100000) {
                    if (start < 0x100000) {
                        start = 0x100000;
                    }
                    stats.usable_bytes += limit - start;
                    if (limit - start > best_len) {
                        best_start = start;
                        best_len = limit - start;
                    }
                }
            }
            entry += boot->mmap->entry_size;
        }
        stats.total_bytes = highest_usable_limit;
    } else {
        stats.total_bytes = (boot->mem_lower_kib + boot->mem_upper_kib) * 1024;
        stats.usable_bytes = boot->mem_upper_kib * 1024;
        best_start = 0x200000;
        best_len = stats.usable_bytes > 0x200000 ? stats.usable_bytes - 0x200000 : 0;
    }

    uintptr_t reserved_end = align_up((uintptr_t)&kernel_end, PAGE_SIZE);
    if (boot->rootfs.end && boot->rootfs.end > reserved_end) {
        reserved_end = align_up(boot->rootfs.end, PAGE_SIZE);
    }
    if (boot->install_image.end && boot->install_image.end > reserved_end) {
        reserved_end = align_up(boot->install_image.end, PAGE_SIZE);
    }

    frame_cursor = align_up(best_start, PAGE_SIZE);
    if (frame_cursor < reserved_end && reserved_end < best_start + best_len) {
        frame_cursor = reserved_end;
    }
    frame_limit = align_up(best_start + best_len, PAGE_SIZE);
    if (frame_limit < frame_cursor) {
        frame_limit = frame_cursor;
    }
    free_stack_count = 0;
    stats.free_frames_estimate = (frame_limit - frame_cursor) / PAGE_SIZE;

    log_info("pmm", "memory total=%llu KiB usable=%llu KiB frame pool=%p-%p",
             stats.total_bytes / 1024, stats.usable_bytes / 1024,
             (void *)frame_cursor, (void *)frame_limit);
}

uintptr_t pmm_alloc_frame(void)
{
    if (free_stack_count) {
        uintptr_t frame = free_stack[--free_stack_count];
        stats.allocated_frames++;
        if (stats.free_frames_estimate) {
            stats.free_frames_estimate--;
        }
        return frame;
    }
    if (frame_cursor + PAGE_SIZE > frame_limit) {
        return 0;
    }
    uintptr_t frame = frame_cursor;
    frame_cursor += PAGE_SIZE;
    stats.allocated_frames++;
    if (stats.free_frames_estimate) {
        stats.free_frames_estimate--;
    }
    return frame;
}

void pmm_free_frame(uintptr_t frame)
{
    if (!frame || (frame & (PAGE_SIZE - 1)) != 0) {
        return;
    }
    if (free_stack_count >= PMM_FREE_STACK_MAX) {
        return;
    }
    free_stack[free_stack_count++] = frame;
    if (stats.allocated_frames) {
        stats.allocated_frames--;
    }
    stats.free_frames_estimate++;
}

const struct memory_stats *memory_stats_get(void)
{
    return &stats;
}

void pmm_record_heap(size_t used, size_t size);
void pmm_record_heap(size_t used, size_t size)
{
    stats.heap_used = used;
    stats.heap_size = size;
}
