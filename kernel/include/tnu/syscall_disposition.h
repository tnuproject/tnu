#ifndef TNU_SYSCALL_DISPOSITION_H
#define TNU_SYSCALL_DISPOSITION_H

#include <tnu/types.h>

/*
 * Return-to-userspace vs return-to-kernel/shell.
 *
 * syscall_dispatch encodes both:
 *   - scalar return value (what user code sees as rax)
 *   - disposition (what syscall_entry/assembly should do)
 *
 * The encoding format is private to the kernel/arch syscall boundary.
 */

enum syscall_return_disposition {
    SYSCALL_RET_TO_USERSPACE = 0,
    SYSCALL_RET_TO_KERNEL    = 1,
};

/* Encoded return value placed in rax by syscall_entry on the final return to usermode. */
static inline uint64_t syscall_encode_result(long scalar_return,
                                             enum syscall_return_disposition disp)
{
    uint64_t scalar = (uint64_t)scalar_return & ((1ull << 62) - 1ull);
    return ((uint64_t)disp << 62) | scalar;
}

/* Decoded disposition extracted by syscall_entry. */
static inline enum syscall_return_disposition syscall_decode_disposition(uint64_t encoded)
{
    return (enum syscall_return_disposition)(encoded >> 62);
}

/* Decoded scalar return value extracted by syscall_entry. */
static inline long syscall_decode_scalar(uint64_t encoded)
{
    return (long)((int64_t)(encoded << 2) >> 2);
}

#endif

