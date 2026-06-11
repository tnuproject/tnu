#include <tnu/libc.h>

#define USER_HEAP_SIZE (256 * 1024)

struct block_header {
    size_t size;
};

static unsigned char heap[USER_HEAP_SIZE];
static size_t heap_used;

static size_t align_up(size_t value, size_t align)
{
    return (value + align - 1) & ~(align - 1);
}

void *malloc(size_t size)
{
    if (size == 0) {
        return 0;
    }
    size_t total = align_up(sizeof(struct block_header) + size, 16);
    if (heap_used + total > sizeof(heap)) {
        return 0;
    }
    struct block_header *hdr = (struct block_header *)(heap + heap_used);
    hdr->size = size;
    heap_used += total;
    return hdr + 1;
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

void *realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        return 0;
    }
    struct block_header *old = ((struct block_header *)ptr) - 1;
    if (size <= old->size) {
        old->size = size;
        return ptr;
    }
    void *next = malloc(size);
    if (!next) {
        return 0;
    }
    memcpy(next, ptr, old->size);
    return next;
}

void free(void *ptr)
{
    (void)ptr;
}
