#ifndef TNU_LINUX_SYSCALL_TABLE_H
#define TNU_LINUX_SYSCALL_TABLE_H

#include <tnu/types.h>

struct linux_syscall_desc {
    const char *name;
    const char *target;
};

extern const struct linux_syscall_desc linux_syscall_table[];
extern const uint64_t linux_syscall_table_size;

#endif
