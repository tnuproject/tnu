#include <tnu/memory.h>
#include <tnu/string.h>

#define KERNEL_HEAP_SIZE (64 * 1024 * 1024)

static uint8_t heap[KERNEL_HEAP_SIZE] __attribute__((aligned(16)));
static size_t heap_offset;

void pmm_record_heap(size_t used, size_t size);

static size_t align16(size_t value)
{
    return (value + 15u) & ~((size_t)15u);
}

void heap_init(void)
{
    heap_offset = 0;
    pmm_record_heap(heap_offset, KERNEL_HEAP_SIZE);
}

void *kmalloc(size_t size)
{
    size = align16(size);
    if (size == 0 || heap_offset + size > KERNEL_HEAP_SIZE) {
        return NULL;
    }
    void *ptr = heap + heap_offset;
    heap_offset += size;
    pmm_record_heap(heap_offset, KERNEL_HEAP_SIZE);
    return ptr;
}

void *kcalloc(size_t count, size_t size)
{
    if (count && size > ((size_t)-1) / count) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *krealloc(void *ptr, size_t old_size, size_t new_size)
{
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return NULL;
    }
    if (ptr) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}
