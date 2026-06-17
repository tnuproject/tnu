#include <arch/cpu.h>

#include <tnu/linux_compat.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/process.h>
#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/vfs.h>

#include "../syscall/linux_errno.h"

#define AT_FDCWD -100
#define EI_NIDENT 16
#define ELFCLASS64 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PAGE_MASK (~(uintptr_t)(PAGE_SIZE - 1))
#define LINUX_USER_BASE 0x400000ULL
#define LINUX_PIE_BASE 0x4000000ULL
#define LINUX_INTERP_BASE 0x8000000ULL
#define LINUX_HEAP_BASE 0x10000000ULL
#define LINUX_MMAP_BASE 0x20000000ULL
#define LINUX_MMAP_LIMIT 0x40000000ULL
#define LINUX_STACK_BOTTOM 0x50000000ULL
#define LINUX_STACK_TOP 0x50400000ULL
#define LINUX_MAX_ARGS 32
#define LINUX_MAX_ENV 16
#define LINUX_MAX_ARG_LEN 255

struct elf64_ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct linux_loaded_image {
    struct linux_elf_info info;
    uint64_t load_bias;
    uint64_t entry;
    uint64_t phdr;
};

static uintptr_t page_down(uintptr_t value)
{
    return value & PAGE_MASK;
}

static uintptr_t page_up(uintptr_t value)
{
    return (value + PAGE_SIZE - 1) & PAGE_MASK;
}

static struct vfs_node *linux_lookup_path(const char *path, char *resolved, size_t resolved_size)
{
    struct process *proc = process_current();
    if (!path || !path[0] || !resolved || resolved_size == 0) {
        return NULL;
    }

    if (path[0] == '/') {
        if (strncmp(path, "/proc", 5) == 0 || strncmp(path, "/dev", 4) == 0 ||
            strncmp(path, "/sys", 4) == 0) {
            struct vfs_node *pseudo = vfs_lookup(path, "/");
            if (pseudo) {
                vfs_normalize(path, "/", resolved, resolved_size);
                return pseudo;
            }
        }
        ksnprintf(resolved, resolved_size, "/usr/linux%s", path);
        struct vfs_node *linux_node = vfs_lookup(resolved, "/");
        if (linux_node) {
            return linux_node;
        }
    }
    struct vfs_node *node = vfs_lookup(path, proc ? proc->cwd : "/");
    if (node) {
        vfs_normalize(path, proc ? proc->cwd : "/", resolved, resolved_size);
        return node;
    }
    ksnprintf(resolved, resolved_size, "/usr/linux/%s", path);
    return vfs_lookup(resolved, "/");
}

static int copy_interp_path(const void *image, const struct linux_elf_info *info,
                            char *out, size_t out_size)
{
    if (!info->has_interp) {
        out[0] = '\0';
        return 0;
    }
    if (info->interp_size == 0 || info->interp_size >= out_size) {
        return -1;
    }
    memcpy(out, (const uint8_t *)image + info->interp_offset, (size_t)info->interp_size);
    out[info->interp_size] = '\0';
    return 0;
}

static int load_elf64_segments(const void *image, size_t size, uint64_t load_bias,
                               struct linux_loaded_image *loaded)
{
    const struct elf64_ehdr *eh = image;
    if (!image || size < sizeof(*eh) || eh->e_ident[4] != ELFCLASS64 ||
        eh->e_machine != EM_X86_64 ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        return -1;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > size || ph->p_memsz < ph->p_filesz) {
            return -1;
        }

        uintptr_t dest = (uintptr_t)(load_bias + ph->p_vaddr);
        memcpy((void *)dest, (const uint8_t *)image + ph->p_offset, (size_t)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset((void *)(dest + ph->p_filesz), 0, (size_t)(ph->p_memsz - ph->p_filesz));
        }
    }

    loaded->load_bias = load_bias;
    loaded->entry = load_bias + eh->e_entry;
    loaded->phdr = load_bias + eh->e_phoff;
    return 0;
}

static int load_linux_image(struct vfs_node *node, uint64_t preferred_bias,
                            struct linux_loaded_image *loaded)
{
    if (!node || !node->data || !loaded ||
        linux_elf_probe(node->data, (size_t)node->size, &loaded->info) < 0) {
        return -1;
    }
    if (loaded->info.elf_class != LINUX_ELF64) {
        return -1;
    }

    uint64_t bias = loaded->info.is_pie ? preferred_bias : 0;
    uintptr_t start = page_down((uintptr_t)(bias + loaded->info.lowest_vaddr));
    uintptr_t end = page_up((uintptr_t)(bias + loaded->info.highest_vaddr));
    if (start < LINUX_USER_BASE || end >= LINUX_STACK_BOTTOM || end <= start) {
        return -1;
    }
    if (vmm_map_range_identity(start, end - start, VMM_FLAG_WRITABLE | VMM_FLAG_USER) < 0) {
        return -1;
    }
    return load_elf64_segments(node->data, (size_t)node->size, bias, loaded);
}

static int push_string(uintptr_t *sp, const char *s, uint64_t *out)
{
    size_t len = strlen(s) + 1;
    *sp -= len;
    memcpy((void *)*sp, s, len);
    *out = *sp;
    return 0;
}

static uintptr_t build_linux_stack(const struct linux_loaded_image *main_img,
                                   const struct linux_loaded_image *interp_img,
                                   const char *exec_path, int argc,
                                   const char *argv_values[])
{
    static const char *env_values[LINUX_MAX_ENV] = {
        "PATH=/bin:/usr/bin:/usr/local/bin",
        "HOME=/root",
        "TERM=linux",
        "SHELL=/bin/sh",
        "USER=root",
    };
    int envc = 5;
    uintptr_t sp = LINUX_STACK_TOP;
    uint64_t argv_ptrs[LINUX_MAX_ARGS];
    uint64_t env_ptrs[LINUX_MAX_ENV];
    uint64_t execfn_ptr = 0;
    uint64_t random_ptr = 0;
    uint8_t random_bytes[16];
    struct process *proc = process_current();

    uint64_t seed = (uint64_t)(uintptr_t)main_img ^ (uint64_t)(uintptr_t)exec_path;
    for (size_t i = 0; i < sizeof(random_bytes); i++) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        random_bytes[i] = (uint8_t)seed;
    }

    push_string(&sp, exec_path, &execfn_ptr);
    sp -= sizeof(random_bytes);
    memcpy((void *)sp, random_bytes, sizeof(random_bytes));
    random_ptr = sp;
    for (int i = argc - 1; i >= 0; i--) {
        push_string(&sp, argv_values[i], &argv_ptrs[i]);
    }
    for (int i = envc - 1; i >= 0; i--) {
        push_string(&sp, env_values[i], &env_ptrs[i]);
    }

    sp &= ~0xfULL;
    uint64_t *stack = (uint64_t *)sp;
    *--stack = 0;
    *--stack = execfn_ptr;
    *--stack = LINUX_AT_EXECFN;
    *--stack = random_ptr;
    *--stack = LINUX_AT_RANDOM;
    *--stack = 0;
    *--stack = LINUX_AT_SECURE;
    *--stack = 100;
    *--stack = LINUX_AT_CLKTCK;
    *--stack = 0;
    *--stack = LINUX_AT_HWCAP;
    *--stack = proc ? proc->gid : 0;
    *--stack = LINUX_AT_EGID;
    *--stack = proc ? proc->gid : 0;
    *--stack = LINUX_AT_GID;
    *--stack = proc ? proc->uid : 0;
    *--stack = LINUX_AT_EUID;
    *--stack = proc ? proc->uid : 0;
    *--stack = LINUX_AT_UID;
    *--stack = main_img->entry;
    *--stack = LINUX_AT_ENTRY;
    *--stack = interp_img ? interp_img->load_bias : 0;
    *--stack = LINUX_AT_BASE;
    *--stack = PAGE_SIZE;
    *--stack = LINUX_AT_PAGESZ;
    *--stack = main_img->info.phnum;
    *--stack = LINUX_AT_PHNUM;
    *--stack = main_img->info.phentsize;
    *--stack = LINUX_AT_PHENT;
    *--stack = main_img->phdr;
    *--stack = LINUX_AT_PHDR;
    *--stack = 0;
    for (int i = envc - 1; i >= 0; i--) {
        *--stack = env_ptrs[i];
    }
    *--stack = 0;
    for (int i = argc - 1; i >= 0; i--) {
        *--stack = argv_ptrs[i];
    }
    *--stack = (uint64_t)argc;
    return (uintptr_t)stack;
}

long linux_run_binary(const char *path, int argc, char **argv)
{
    char resolved[VFS_PATH_MAX];
    char interp_path[VFS_PATH_MAX];
    char interp_resolved[VFS_PATH_MAX];
    char arg_storage[LINUX_MAX_ARGS][LINUX_MAX_ARG_LEN + 1];
    const char *arg_values[LINUX_MAX_ARGS];

    if (!path) {
        return -LINUX_EFAULT;
    }
    if (argc <= 0) {
        argc = 1;
    }
    if (argc > LINUX_MAX_ARGS) {
        argc = LINUX_MAX_ARGS;
    }

    struct vfs_node *node = linux_lookup_path(path, resolved, sizeof(resolved));
    if (!node || node->type != VFS_NODE_FILE) {
        return -LINUX_ENOENT;
    }

    for (int i = 0; i < argc; i++) {
        const char *src = argv && argv[i] ? argv[i] : (i == 0 ? resolved : "");
        strncpy(arg_storage[i], src, LINUX_MAX_ARG_LEN);
        arg_storage[i][LINUX_MAX_ARG_LEN] = '\0';
        arg_values[i] = arg_storage[i];
    }

    struct linux_loaded_image main_img;
    if (load_linux_image(node, LINUX_PIE_BASE, &main_img) < 0) {
        return -LINUX_ENOEXEC;
    }

    struct linux_loaded_image interp_img;
    struct linux_loaded_image *interp = NULL;
    if (copy_interp_path(node->data, &main_img.info, interp_path, sizeof(interp_path)) < 0) {
        return -LINUX_ENOEXEC;
    }
    if (interp_path[0]) {
        struct vfs_node *interp_node = linux_lookup_path(interp_path, interp_resolved,
                                                        sizeof(interp_resolved));
        if (!interp_node || load_linux_image(interp_node, LINUX_INTERP_BASE, &interp_img) < 0) {
            log_warn("linux", "interpreter not available: %s", interp_path);
            return -LINUX_ENOENT;
        }
        interp = &interp_img;
    }

    if (vmm_map_range_identity(LINUX_HEAP_BASE, LINUX_STACK_TOP - LINUX_HEAP_BASE,
                               VMM_FLAG_WRITABLE | VMM_FLAG_USER) < 0) {
        return -LINUX_ENOMEM;
    }
    memset((void *)LINUX_STACK_BOTTOM, 0, LINUX_STACK_TOP - LINUX_STACK_BOTTOM);
    linux_mm_reset(LINUX_HEAP_BASE, LINUX_MMAP_BASE, LINUX_MMAP_LIMIT);

    uintptr_t stack = build_linux_stack(&main_img, interp, resolved, argc, arg_values);
    struct process *proc = process_current();
    uint32_t saved_personality = proc ? proc->personality : 0;
    char saved_name[PROCESS_NAME_MAX + 1];
    if (proc) {
        strncpy(saved_name, proc->name, sizeof(saved_name) - 1);
        saved_name[sizeof(saved_name) - 1] = '\0';
        proc->personality = LINUX_PERSONALITY;
        proc->linux_tls_base = main_img.info.tls_vaddr;
        proc->linux_vdso_base = 0;
        strncpy(proc->linux_exec_path, resolved, sizeof(proc->linux_exec_path) - 1);
        proc->linux_exec_path[sizeof(proc->linux_exec_path) - 1] = '\0';
        strncpy(proc->name, resolved, PROCESS_NAME_MAX);
        proc->name[PROCESS_NAME_MAX] = '\0';
    }

    uint64_t entry = interp ? interp->entry : main_img.entry;
    uint64_t saved_fs_base = cpu_get_fs_base();
    int rc = arch_enter_user(entry, (uint64_t)stack);
    cpu_set_fs_base(saved_fs_base);
    if (proc) {
        proc->personality = saved_personality;
        strncpy(proc->name, saved_name, PROCESS_NAME_MAX);
        proc->name[PROCESS_NAME_MAX] = '\0';
        proc->exit_code = rc;
        if (proc->state == PROCESS_ZOMBIE) {
            proc->state = PROCESS_RUNNING;
        }
    }
    return rc;
}

static long linux_exec_path(const char *path, char *const argv[])
{
    char resolved[VFS_PATH_MAX];

    if (!path) {
        return -LINUX_EFAULT;
    }

    struct vfs_node *node = linux_lookup_path(path, resolved, sizeof(resolved));
    if (!node || node->type != VFS_NODE_FILE) {
        return -LINUX_ENOENT;
    }

    struct linux_elf_info info;
    if (linux_elf_probe(node->data, (size_t)node->size, &info) < 0) {
        return -LINUX_ENOEXEC;
    }

    int argc = 0;
    while (argv && argv[argc] && argc < LINUX_MAX_ARGS) {
        argc++;
    }
    return linux_run_binary(resolved, argc > 0 ? argc : 1, (char **)argv);
}

long linux_execve(const char *path, char *const argv[], char *const envp[])
{
    (void)envp;
    return linux_exec_path(path, argv);
}

long linux_execveat(int dirfd, const char *path, char *const argv[],
                    char *const envp[], int flags)
{
    (void)flags;
    if (dirfd != AT_FDCWD) {
        return -LINUX_ENOTSUP;
    }
    return linux_execve(path, argv, envp);
}
