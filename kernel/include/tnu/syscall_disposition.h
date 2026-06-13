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
uint64_t syscall_encode_result(long scalar_return, enum syscall_return_disposition disp);

/* Decoded disposition extracted by syscall_entry. */
enum syscall_return_disposition syscall_decode_disposition(uint64_t encoded);

/* Decoded scalar return value extracted by syscall_entry. */
long syscall_decode_scalar(uint64_t encoded);

#endif

