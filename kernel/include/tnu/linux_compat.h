#ifndef TNU_LINUX_COMPAT_H
#define TNU_LINUX_COMPAT_H

#include <tnu/types.h>

#define LINUX_PERSONALITY 0x4c4e5801u

#define LINUX_AT_NULL    0
#define LINUX_AT_PHDR    3
#define LINUX_AT_PHENT   4
#define LINUX_AT_PHNUM   5
#define LINUX_AT_PAGESZ  6
#define LINUX_AT_BASE    7
#define LINUX_AT_ENTRY   9
#define LINUX_AT_UID    11
#define LINUX_AT_EUID   12
#define LINUX_AT_GID    13
#define LINUX_AT_EGID   14
#define LINUX_AT_HWCAP  16
#define LINUX_AT_CLKTCK 17
#define LINUX_AT_SECURE 23
#define LINUX_AT_RANDOM 25
#define LINUX_AT_EXECFN 31

enum linux_elf_class {
    LINUX_ELF_NONE = 0,
    LINUX_ELF32 = 1,
    LINUX_ELF64 = 2,
};

struct linux_elf_info {
    enum linux_elf_class elf_class;
    uint16_t machine;
    uint16_t type;
    uint64_t entry;
    uint64_t phoff;
    uint16_t phentsize;
    uint16_t phnum;
    uint64_t lowest_vaddr;
    uint64_t highest_vaddr;
    uint64_t interp_offset;
    uint64_t interp_size;
    uint64_t tls_vaddr;
    uint64_t tls_memsz;
    uint64_t tls_filesz;
    uint64_t relro_vaddr;
    uint64_t relro_memsz;
    uint64_t stack_flags;
    bool has_interp;
    bool has_tls;
    bool has_relro;
    bool has_gnu_stack;
    bool is_pie;
};

struct linux_syscall_args {
    uint64_t nr;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
};

void linux_compat_init(void);
int linux_elf_probe(const void *image, size_t size, struct linux_elf_info *info);
long linux_run_binary(const char *path, int argc, char **argv);
void linux_mm_reset(uintptr_t brk_floor, uintptr_t mmap_base, uintptr_t mmap_limit);
long linux_execve(const char *path, char *const argv[], char *const envp[]);
long linux_execveat(int dirfd, const char *path, char *const argv[],
                    char *const envp[], int flags);
long linux_syscall_entry(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5);
long linux_syscall_dispatch(const struct linux_syscall_args *args);
const char *linux_syscall_name(uint64_t nr);

#endif
