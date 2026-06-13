#include <tnu/memory.h>
#include <tnu/string.h>

#define KERNEL_HEAP_SIZE (64 * 1024 * 1024)
#define HEAP_MAGIC 0x544e5548u

struct heap_block {
    size_t size;
    bool free;
    uint32_t magic;
    struct heap_block *next;
    struct heap_block *prev;
};

static uint8_t heap[KERNEL_HEAP_SIZE] __attribute__((aligned(16)));
static struct heap_block *heap_head;
static size_t heap_used_highwater;

void pmm_record_heap(size_t used, size_t size);

static size_t align16(size_t value)
{
    return (value + 15u) & ~((size_t)15u);
}

static void record_heap_usage(void)
{
    pmm_record_heap(heap_used_highwater, KERNEL_HEAP_SIZE);
}

void heap_init(void)
{
    heap_head = (struct heap_block *)heap;
    heap_head->size = KERNEL_HEAP_SIZE - sizeof(struct heap_block);
    heap_head->free = true;
    heap_head->magic = HEAP_MAGIC;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    heap_used_highwater = sizeof(struct heap_block);
    record_heap_usage();
}

static void split_block(struct heap_block *block, size_t size)
{
    if (!block || block->size < size + sizeof(struct heap_block) + 32) {
        return;
    }

    struct heap_block *next = (struct heap_block *)((uint8_t *)(block + 1) + size);
    next->size = block->size - size - sizeof(struct heap_block);
    next->free = true;
    next->magic = HEAP_MAGIC;
    next->next = block->next;
    next->prev = block;
    if (next->next) {
        next->next->prev = next;
    }
    block->next = next;
    block->size = size;
}

static void coalesce(struct heap_block *block)
{
    if (!block || block->magic != HEAP_MAGIC) {
        return;
    }
    if (block->next && block->next->magic == HEAP_MAGIC && block->next->free) {
        struct heap_block *next = block->next;
        block->size += sizeof(struct heap_block) + next->size;
        block->next = next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }
    if (block->prev && block->prev->magic == HEAP_MAGIC && block->prev->free) {
        coalesce(block->prev);
    }
}

void *kmalloc(size_t size)
{
    size = align16(size);
    if (size == 0) {
        return NULL;
    }

    for (struct heap_block *b = heap_head; b; b = b->next) {
        if (b->magic != HEAP_MAGIC) {
            return NULL;
        }
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = false;
            uintptr_t end = (uintptr_t)(b + 1) + b->size - (uintptr_t)heap;
            if (end > heap_used_highwater) {
                heap_used_highwater = end;
                record_heap_usage();
            }
            return b + 1;
        }
    }
    return NULL;
}

void kfree(void *ptr)
{
    if (!ptr) {
        return;
    }
    if ((uintptr_t)ptr < (uintptr_t)heap ||
        (uintptr_t)ptr >= (uintptr_t)heap + KERNEL_HEAP_SIZE) {
        return;
    }
    struct heap_block *b = ((struct heap_block *)ptr) - 1;
    if (b->magic != HEAP_MAGIC) {
        return;
    }
    b->free = true;
    coalesce(b);
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
    if (!ptr) {
        return kmalloc(new_size);
    }
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct heap_block *b = ((struct heap_block *)ptr) - 1;
    if (b->magic == HEAP_MAGIC && b->size >= new_size) {
        split_block(b, align16(new_size));
        return ptr;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return NULL;
    }
    if (old_size) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    } else if (b->magic == HEAP_MAGIC) {
        memcpy(new_ptr, ptr, b->size < new_size ? b->size : new_size);
    }
    kfree(ptr);
    return new_ptr;
}
