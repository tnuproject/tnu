#include <tnu/libc.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

struct block_header {
    size_t size;
    int free;
    struct block_header *next;
};

#define ALIGNMENT 16
#define MIN_SPLIT 32

static struct block_header *heap_head;
static struct block_header *heap_tail;

static size_t align_up(size_t value, size_t align)
{
    return (value + align - 1) & ~(align - 1);
}

static struct block_header *find_free_block(size_t size)
{
    for (struct block_header *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            return b;
        }
    }
    return 0;
}

static void split_block(struct block_header *b, size_t size)
{
    if (b->size < size + sizeof(struct block_header) + MIN_SPLIT) {
        return;
    }

    struct block_header *next =
        (struct block_header *)((char *)(b + 1) + size);
    next->size = b->size - size - sizeof(struct block_header);
    next->free = 1;
    next->next = b->next;

    b->size = size;
    b->next = next;
    if (heap_tail == b) {
        heap_tail = next;
    }
}

static struct block_header *request_block(size_t size)
{
    size_t total = sizeof(struct block_header) + size;
    struct block_header *b = sbrk((intptr_t)total);
    if (b == (void *)-1) {
        return 0;
    }
    b->size = size;
    b->free = 0;
    b->next = 0;
    if (!heap_head) {
        heap_head = b;
    }
    if (heap_tail) {
        heap_tail->next = b;
    }
    heap_tail = b;
    return b;
}

void *malloc(size_t size)
{
    if (size == 0) {
        return 0;
    }

    size = align_up(size, ALIGNMENT);
    struct block_header *b = find_free_block(size);
    if (!b) {
        b = request_block(size);
        if (!b) {
            return 0;
        }
    } else {
        b->free = 0;
        split_block(b, size);
    }
    return b + 1;
}

void *calloc(size_t nmemb, size_t size)
{
    if (size && nmemb > ((size_t)-1) / size) {
        return 0;
    }
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static void coalesce(void)
{
    for (struct block_header *b = heap_head; b && b->next; b = b->next) {
        char *end = (char *)(b + 1) + b->size;
        if (b->free && b->next->free && end == (char *)b->next) {
            struct block_header *next = b->next;
            b->size += sizeof(struct block_header) + next->size;
            b->next = next->next;
            if (heap_tail == next) {
                heap_tail = b;
            }
        }
    }
}

void free(void *ptr)
{
    if (!ptr) {
        return;
    }
    struct block_header *b = ((struct block_header *)ptr) - 1;
    b->free = 1;
    coalesce();
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return 0;
    }

    size = align_up(size, ALIGNMENT);
    struct block_header *old = ((struct block_header *)ptr) - 1;
    if (size <= old->size) {
        split_block(old, size);
        return ptr;
    }
    if (old->next && old->next->free &&
        old->size + sizeof(struct block_header) + old->next->size >= size) {
        old->size += sizeof(struct block_header) + old->next->size;
        old->next = old->next->next;
        if (!old->next) {
            heap_tail = old;
        }
        split_block(old, size);
        return ptr;
    }

    void *next = malloc(size);
    if (!next) {
        return 0;
    }
    memcpy(next, ptr, old->size);
    free(ptr);
    return next;
}
