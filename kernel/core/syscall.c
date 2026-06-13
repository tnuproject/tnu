#include <tnu/console.h>
#include <tnu/elf.h>
#include <tnu/framebuffer.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/multiboot2.h>
#include <tnu/printf.h>
#include <tnu/process.h>
#include <tnu/string.h>
#include <tnu/syscall.h>
#include <tnu/syscall_disposition.h>
#include <tnu/user.h>
#include <tnu/vfs.h>

#include <arch/cpu.h>
#include <arch/keyboard.h>
#include <arch/pit.h>

/* Validate that a user-supplied pointer and length are within user address space */
static bool uptr_ok(const void *ptr, size_t len)
{
    if (!ptr) {
        return false;
    }
    uintptr_t start = (uintptr_t)ptr;
    /* Reject kernel addresses (upper half) and null-page */
    if (start < 0x1000) {
        return false;
    }
    if (start >= 0x0000800000000000ULL) {
        return false;
    }
    /* Check for overflow */
    if (len && start + len < start) {
        return false;
    }
    return true;
}

static bool ustr_ok(const char *s)
{
    if (!s) {
        return false;
    }
    uintptr_t v = (uintptr_t)s;
    return v >= 0x1000 && v < 0x0000800000000000ULL;
}

static bool has_perm(const struct process *proc, const struct vfs_node *node, uint32_t perm)
{
    if (!proc || !node) {
        return false;
    }
    if (proc->uid == 0) {
        return true;
    }
    uint32_t shift = 0;
    if (proc->uid == node->uid) {
        shift = 6;
    } else if (proc->gid == node->gid) {
        shift = 3;
    }
    return ((node->mode >> shift) & perm) == perm;
}

static bool is_root(const struct process *proc)
{
    return proc && proc->uid == 0;
}

static bool path_is_or_under(const char *path, const char *prefix)
{
    size_t len = strlen(prefix);
    if (strcmp(path, prefix) == 0) {
        return true;
    }
    return len > 1 && strncmp(path, prefix, len) == 0 && path[len] == '/';
}

static bool path_requires_root_for_mutation(const char *normal)
{
    static const char *const protected_prefixes[] = {
        "/",
        "/bin",
        "/boot",
        "/dev",
        "/etc",
        "/lib",
        "/proc",
        "/root",
        "/sbin",
        "/usr",
        "/var/cache",
        "/var/db",
        "/var/log",
        "/var/run",
    };

    for (size_t i = 0; i < sizeof(protected_prefixes) / sizeof(protected_prefixes[0]); i++) {
        if (path_is_or_under(normal, protected_prefixes[i])) {
            return true;
        }
    }
    return false;
}

static bool path_is_public_device(const char *normal)
{
    return strcmp(normal, "/dev/null") == 0 ||
           strcmp(normal, "/dev/zero") == 0 ||
           strcmp(normal, "/dev/tty") == 0 ||
           strcmp(normal, "/dev/console") == 0;
}

static int normalize_for_process(const struct process *proc, const char *path,
                                 char *normal, size_t normal_size)
{
    if (!proc || !path || !normal || normal_size == 0) {
        return -1;
    }
    return vfs_normalize(path, proc->cwd, normal, normal_size);
}

static struct vfs_node *lookup_parent_from_normal(const char *normal)
{
    char parent[VFS_PATH_MAX];
    if (!normal || strcmp(normal, "/") == 0) {
        return NULL;
    }
    strncpy(parent, normal, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash) {
        return NULL;
    }
    if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    return vfs_lookup(parent, "/");
}

static bool can_create_in_parent(const struct process *proc, const char *normal)
{
    struct vfs_node *parent = lookup_parent_from_normal(normal);
    return parent && parent->type == VFS_NODE_DIR &&
           has_perm(proc, parent, 2) && has_perm(proc, parent, 1);
}

static uintptr_t user_brk_floor_current = 0x5000000;
static uintptr_t user_brk_current = 0x5000000;
static uintptr_t user_heap_limit_current = 0x7000000;

static uintptr_t page_align_up(uintptr_t value)
{
    return (value + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
}

static bool module_overlaps_range(const struct boot_module *module,
                                  uintptr_t start, uintptr_t end,
                                  uintptr_t *overlap_start,
                                  uintptr_t *overlap_end)
{
    if (!module || !module->start || module->end <= module->start ||
        module->end <= start || module->start >= end) {
        return false;
    }
    uintptr_t s = module->start > start ? module->start : start;
    uintptr_t e = module->end < end ? module->end : end;
    if (s >= e) {
        return false;
    }
    if (overlap_start) {
        *overlap_start = s;
    }
    if (overlap_end) {
        *overlap_end = e;
    }
    return true;
}

static bool boot_module_range_overlap(uintptr_t start, uintptr_t end)
{
    const struct boot_info *boot = boot_info_get();
    uintptr_t s, e;
    return boot &&
           (module_overlaps_range(&boot->rootfs, start, end, &s, &e) ||
            module_overlaps_range(&boot->install_image, start, end, &s, &e));
}

static uintptr_t boot_modules_end_in_range(uintptr_t start, uintptr_t end)
{
    const struct boot_info *boot = boot_info_get();
    uintptr_t max_end = 0;
    uintptr_t s, e;
    if (!boot) {
        return 0;
    }
    if (module_overlaps_range(&boot->rootfs, start, end, &s, &e) && e > max_end) {
        max_end = e;
    }
    if (module_overlaps_range(&boot->install_image, start, end, &s, &e) && e > max_end) {
        max_end = e;
    }
    return max_end;
}

static void clear_user_range_preserving_boot_modules(uintptr_t start, uintptr_t end)
{
    const struct boot_info *boot = boot_info_get();
    uintptr_t cur = start;
    while (cur < end) {
        uintptr_t next_skip_start = end;
        uintptr_t next_skip_end = end;
        uintptr_t s, e;

        if (boot && module_overlaps_range(&boot->rootfs, cur, end, &s, &e) &&
            s < next_skip_start) {
            next_skip_start = s;
            next_skip_end = e;
        }
        if (boot && module_overlaps_range(&boot->install_image, cur, end, &s, &e) &&
            s < next_skip_start) {
            next_skip_start = s;
            next_skip_end = e;
        }

        if (next_skip_start == end) {
            memset((void *)cur, 0, end - cur);
            break;
        }
        if (cur < next_skip_start) {
            memset((void *)cur, 0, next_skip_start - cur);
        }
        cur = next_skip_end > cur ? next_skip_end : cur + 1;
    }
}
static bool user_exec_active;
static char tty_pending[8];
static size_t tty_pending_len;
static size_t tty_pending_pos;

enum {
    SIGINT_NUMBER = 2,
    SIGNAL_DISPOSITION_DEFAULT = 0,
    SIGNAL_DISPOSITION_IGNORED = 1,
    SIGNAL_DISPOSITION_HANDLED = 2,
};

struct user_sigaction_abi {
    uintptr_t handler;
    uint64_t mask;
    int flags;
};

static bool tty_pending_pop(char *out)
{
    if (tty_pending_pos >= tty_pending_len) {
        tty_pending_pos = 0;
        tty_pending_len = 0;
        return false;
    }
    *out = tty_pending[tty_pending_pos++];
    if (tty_pending_pos >= tty_pending_len) {
        tty_pending_pos = 0;
        tty_pending_len = 0;
    }
    return true;
}

static void tty_pending_set(const char *seq)
{
    tty_pending_len = 0;
    tty_pending_pos = 0;
    while (seq[tty_pending_len] && tty_pending_len < sizeof(tty_pending)) {
        tty_pending[tty_pending_len] = seq[tty_pending_len];
        tty_pending_len++;
    }
}

static int tty_encode_key(int ch, struct process *proc)
{
    if (ch == KEY_CTRL_C &&
        proc && proc->signal_disposition[SIGINT_NUMBER] != SIGNAL_DISPOSITION_DEFAULT) {
        return 3;
    }

    switch (ch) {
    case KEY_UP:
        tty_pending_set("\x1b[A");
        break;
    case KEY_DOWN:
        tty_pending_set("\x1b[B");
        break;
    case KEY_RIGHT:
        tty_pending_set("\x1b[C");
        break;
    case KEY_LEFT:
        tty_pending_set("\x1b[D");
        break;
    case KEY_HOME:
        tty_pending_set("\x1b[H");
        break;
    case KEY_END:
        tty_pending_set("\x1b[F");
        break;
    case KEY_DELETE:
        tty_pending_set("\x1b[3~");
        break;
    default:
        return ch;
    }

    char first = 0;
    tty_pending_pop(&first);
    return (unsigned char)first;
}

static int tty_read_byte(struct process *proc)
{
    for (;;) {
        char c;
        if (tty_pending_pop(&c)) {
            return (unsigned char)c;
        }
        int ch = console_getchar();
        /* CTRL+C while blocking in read: deliver as interrupt */
        if (ch == KEY_CTRL_C) {
            keyboard_ack_interrupt();
            if (proc && proc->signal_disposition[SIGINT_NUMBER] == SIGNAL_DISPOSITION_DEFAULT) {
                process_exit(proc, 130);
                /* Return-to-kernel via syscall_encode */
                return -130;
            }
            return 3; /* pass raw ^C byte if handler is registered */
        }
        if (ch >= KEY_TTY1 && ch <= KEY_TTY1 + 5) {
            console_switch_tty((size_t)(ch - KEY_TTY1));
            continue;
        }
        return tty_encode_key(ch, proc);
    }
}

static int tty_try_read_byte(struct process *proc)
{
    char c;
    if (tty_pending_pop(&c)) {
        return (unsigned char)c;
    }
    int ch = keyboard_try_getchar();
    if (ch < 0) {
        return -1;
    }
    if (ch >= KEY_TTY1 && ch <= KEY_TTY6) {
        console_switch_tty((size_t)(ch - KEY_TTY1));
        return -1;
    }
    return tty_encode_key(ch, proc);
}

static int input_try_read_raw_key(void)
{
    for (;;) {
        int ch = keyboard_try_getchar();
        if (ch < 0) {
            return -1;
        }
        if (ch >= KEY_TTY1 && ch <= KEY_TTY6) {
            console_switch_tty((size_t)(ch - KEY_TTY1));
            continue;
        }
        return ch;
    }
}

static struct vfs_node *resolve_exec_node(const char *path, char *resolved, size_t resolved_size)
{
    struct process *proc = process_current();
    if (!path || !path[0] || !resolved || resolved_size == 0) {
        return NULL;
    }
    if (strchr(path, '/')) {
        strncpy(resolved, path, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
        return vfs_lookup(resolved, proc ? proc->cwd : "/");
    }

    ksnprintf(resolved, resolved_size, "/bin/%s", path);
    struct vfs_node *node = vfs_lookup(resolved, "/");
    if (node) {
        return node;
    }
    ksnprintf(resolved, resolved_size, "/sbin/%s", path);
    return vfs_lookup(resolved, "/");
}

static long sys_open(const char *path, int flags, int mode)
{
    if (!ustr_ok(path)) {
        return -1;
    }
    struct process *proc = process_current();
    char normal[VFS_PATH_MAX];
    if (normalize_for_process(proc, path, normal, sizeof(normal)) < 0) {
        return -1;
    }

    struct vfs_node *node = vfs_lookup(normal, "/");
    bool wants_write = (flags & VFS_O_WRONLY) || (flags & VFS_O_RDWR) ||
                       (flags & VFS_O_TRUNC) || (flags & VFS_O_APPEND);
    bool wants_read = !wants_write || (flags & VFS_O_RDWR);

    if (node && (flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) {
        return -1;
    }
    if (!node && (flags & VFS_O_CREAT)) {
        if (!is_root(proc) && path_requires_root_for_mutation(normal)) {
            return -1;
        }
        if (!can_create_in_parent(proc, normal)) {
            return -1;
        }
        if (vfs_create_file(normal, "/", VFS_S_IFREG | (mode & 0777), proc->uid, proc->gid) < 0) {
            return -1;
        }
        node = vfs_lookup(normal, "/");
    }
    if (!node) {
        return -1;
    }
    if (node->type == VFS_NODE_DEV && !is_root(proc) && !path_is_public_device(normal)) {
        return -1;
    }
    if (wants_write && !is_root(proc) && path_requires_root_for_mutation(normal)) {
        return -1;
    }
    if ((wants_read && !has_perm(proc, node, 4)) ||
        (wants_write && !has_perm(proc, node, 2))) {
        return -1;
    }
    if (wants_write && node->type == VFS_NODE_DIR) {
        return -1;
    }
    if ((flags & VFS_O_TRUNC) && wants_write && node->type == VFS_NODE_FILE) {
        node->data = NULL;
        node->size = 0;
        node->data_borrowed = false;
        node->modified++;
    }
    return process_open_fd(proc, node, flags);
}

static long sys_read(int fd, void *buf, size_t count)
{
    if (count > 0 && !uptr_ok(buf, count)) {
        return -1;
    }
    struct process *proc = process_current();
    if (fd == 0) {
        char *cbuf = buf;
        for (size_t i = 0; i < count; i++) {
            int ch = tty_read_byte(proc);
            if (ch < 0) {
                /* CTRL+C killed the process — return to kernel */
                return (long)syscall_encode_result(130, SYSCALL_RET_TO_KERNEL);
            }
            cbuf[i] = (char)ch;
            if (cbuf[i] == '\n') {
                return (long)i + 1;
            }
        }
        return (long)count;
    }
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -1;
    }
    if (file->flags & VFS_O_WRONLY) {
        return -1;
    }
    if (file->node->type == VFS_NODE_DEV) {
        if (strcmp(file->node->name, "null") == 0) {
            return 0;
        }
        if (strcmp(file->node->name, "zero") == 0) {
            memset(buf, 0, count);
            return (long)count;
        }
        if (strcmp(file->node->name, "tty") == 0 || strcmp(file->node->name, "console") == 0) {
            char *cbuf = buf;
            for (size_t i = 0; i < count; i++) {
                int ch = tty_read_byte(proc);
                if (ch < 0) {
                    return (long)syscall_encode_result(130, SYSCALL_RET_TO_KERNEL);
                }
                cbuf[i] = (char)ch;
                if (cbuf[i] == '\n') {
                    return (long)i + 1;
                }
            }
            return (long)count;
        }
        if (strcmp(file->node->name, "kbd") == 0) {
            unsigned char *cbuf = buf;
            size_t n = 0;
            if (count >= sizeof(uint16_t)) {
                uint16_t *keys = buf;
                size_t max_keys = count / sizeof(uint16_t);
                while (n < max_keys) {
                    int ch = input_try_read_raw_key();
                    if (ch < 0) {
                        break;
                    }
                    keys[n++] = (uint16_t)ch;
                }
                return (long)(n * sizeof(uint16_t));
            }
            while (n < count) {
                int ch = tty_try_read_byte(proc);
                if (ch < 0) {
                    break;
                }
                cbuf[n++] = (unsigned char)ch;
            }
            return (long)n;
        }
        return -1;
    }
    ssize_t ret = vfs_read_node(file->node, file->offset, buf, count);
    if (ret > 0) {
        file->offset += (uint64_t)ret;
    }
    return ret;
}

static long sys_write(int fd, const void *buf, size_t count)
{
    if (count > 0 && !uptr_ok(buf, count)) {
        return -1;
    }
    struct process *proc = process_current();
    if (fd == 1 || fd == 2) {
        console_write_n(buf, count);
        return (long)count;
    }
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -1;
    }
    if (!(file->flags & VFS_O_WRONLY) && !(file->flags & VFS_O_RDWR)) {
        return -1;
    }
    if (file->node->type == VFS_NODE_DEV) {
        if (strcmp(file->node->name, "null") == 0) {
            return (long)count;
        }
        if (strcmp(file->node->name, "tty") == 0 || strcmp(file->node->name, "console") == 0) {
            console_write_n(buf, count);
            return (long)count;
        }
        if (strcmp(file->node->name, "fb0") == 0) {
            const struct framebuffer_info *fb = framebuffer_info();
            if (!framebuffer_is_graphics() || !buf) {
                return -1;
            }
            size_t pixels = count / sizeof(uint32_t);
            size_t max_pixels = (size_t)fb->width * fb->height;
            if (pixels > max_pixels) {
                pixels = max_pixels;
            }
            framebuffer_blit(0, 0, fb->width, (uint32_t)(pixels / fb->width),
                             (const uint32_t *)buf, fb->width);
            return (long)(pixels * sizeof(uint32_t));
        }
        return -1;
    }
    if (file->flags & VFS_O_APPEND) {
        file->offset = file->node->size;
    }
    ssize_t ret = vfs_write_node(file->node, file->offset, buf, count);
    if (ret > 0) {
        file->offset += (uint64_t)ret;
    }
    return ret;
}

static long sys_lseek(int fd, int64_t offset, int whence)
{
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -1;
    }

    int64_t base = 0;
    switch (whence) {
    case 0:
        base = 0;
        break;
    case 1:
        base = (int64_t)file->offset;
        break;
    case 2:
        base = (int64_t)file->node->size;
        break;
    default:
        return -1;
    }

    int64_t next = base + offset;
    if (next < 0) {
        return -1;
    }
    file->offset = (uint64_t)next;
    return next;
}

static long sys_access(const char *path, int mode)
{
    struct process *proc = process_current();
    if (!proc || !path || (mode & ~(1 | 2 | 4)) != 0) {
        return -1;
    }
    char normal[VFS_PATH_MAX];
    if (normalize_for_process(proc, path, normal, sizeof(normal)) < 0) {
        return -1;
    }
    struct vfs_node *node = vfs_lookup(normal, "/");
    if (!node) {
        return -1;
    }
    if (mode == 0) {
        return 0;
    }
    if ((mode & 2) && !is_root(proc) && path_requires_root_for_mutation(normal)) {
        return -1;
    }
    if (node->type == VFS_NODE_DEV && !is_root(proc) && !path_is_public_device(normal)) {
        return -1;
    }
    return has_perm(proc, node, (uint32_t)mode) ? 0 : -1;
}

static long sys_readdir(int fd, struct syscall_dirent *out)
{
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file || !out || !file->node || file->node->type != VFS_NODE_DIR) {
        return -1;
    }

    uint64_t index = 0;
    for (struct vfs_node *node = file->node->first_child; node; node = node->next_sibling) {
        if (index++ != file->offset) {
            continue;
        }
        out->d_ino = file->offset + 1;
        switch (node->type) {
        case VFS_NODE_DIR:
            out->d_type = 4;
            break;
        case VFS_NODE_FILE:
            out->d_type = 8;
            break;
        default:
            out->d_type = 0;
            break;
        }
        strncpy(out->d_name, node->name, sizeof(out->d_name) - 1);
        out->d_name[sizeof(out->d_name) - 1] = '\0';
        file->offset++;
        return 1;
    }
    return 0;
}

static long sys_fstat(int fd, struct vfs_stat *out)
{
    if (!uptr_ok(out, sizeof(*out))) {
        return -1;
    }
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file || !file->node) {
        return -1;
    }

    out->mode = file->node->mode;
    out->uid = file->node->uid;
    out->gid = file->node->gid;
    out->size = file->node->size;
    out->modified = file->node->modified;
    out->type = file->node->type;
    return 0;
}

static long sys_ioctl(int fd, unsigned long request, void *arg)
{
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file || !file->node || file->node->type != VFS_NODE_DEV) {
        return -1;
    }
    if (strcmp(file->node->name, "fb0") == 0 && request == TNU_IOCTL_FB_GETINFO) {
        if (!arg || !framebuffer_is_graphics()) {
            return -1;
        }
        const struct framebuffer_info *fb = framebuffer_info();
        struct syscall_fb_info *out = arg;
        out->width = fb->width;
        out->height = fb->height;
        out->pitch = fb->pitch;
        out->bpp = fb->bpp;
        return 0;
    }
    if ((strcmp(file->node->name, "tty") == 0 || strcmp(file->node->name, "console") == 0) &&
        (request == TNU_IOCTL_TTY_GETSIZE || request == TNU_IOCTL_TIOCGWINSZ)) {
        if (!arg) {
            return -1;
        }
        struct syscall_winsize *ws = arg;
        ws->ws_row = (uint16_t)console_rows();
        ws->ws_col = (uint16_t)console_columns();
        ws->ws_xpixel = (uint16_t)console_pixel_width();
        ws->ws_ypixel = (uint16_t)console_pixel_height();
        return 0;
    }
    return -1;
}

static long sys_exec_image(const char *path, int argc, char **argv)
{
    enum {
        USER_BASE = 0x4000000,
        USER_HEAP_BASE = 0x8000000,
        USER_STACK_BOTTOM_SMALL = 0x17c00000,
        USER_STACK_TOP_SMALL = 0x18000000,
        USER_STACK_BOTTOM_LARGE = 0x2fc00000,
        USER_STACK_TOP_LARGE = 0x30000000,
        MAX_ARGS = 16,
        MAX_ARG_LEN = 127,
    };

    char path_copy[VFS_PATH_MAX];
    char resolved[VFS_PATH_MAX];
    char arg_storage[MAX_ARGS][MAX_ARG_LEN + 1];
    const char *arg_values[MAX_ARGS];
    int arg_count = 0;

    if (!path) {
        return -1;
    }
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    if (argc > MAX_ARGS) {
        argc = MAX_ARGS;
    }
    if (argc <= 0 || !argv) {
        strncpy(arg_storage[0], path_copy, MAX_ARG_LEN);
        arg_storage[0][MAX_ARG_LEN] = '\0';
        arg_values[arg_count++] = arg_storage[0];
    } else {
        for (int i = 0; i < argc; i++) {
            const char *src = argv[i] ? argv[i] : "";
            strncpy(arg_storage[i], src, MAX_ARG_LEN);
            arg_storage[i][MAX_ARG_LEN] = '\0';
            arg_values[arg_count++] = arg_storage[i];
        }
    }
    struct vfs_node *node = resolve_exec_node(path_copy, resolved, sizeof(resolved));
    if (!node || node->type != VFS_NODE_FILE || !node->data) {
        return -1;
    }
    log_info("exec", "path=%s argc=%d argv0=%s argv1=%s",
             resolved, arg_count,
             arg_count > 0 ? arg_values[0] : "",
             arg_count > 1 ? arg_values[1] : "");
    if (!has_perm(process_current(), node, 1)) {
        return -1;
    }

    struct elf_image_info info;
    if (elf64_validate(node->data, (size_t)node->size, &info) < 0) {
        return -1;
    }
    uintptr_t user_stack_bottom = USER_STACK_BOTTOM_SMALL;
    uintptr_t user_stack_top = USER_STACK_TOP_SMALL;
    const struct memory_stats *mem = memory_stats_get();
    if (mem && mem->usable_bytes >= USER_STACK_TOP_LARGE + 0x1000000) {
        user_stack_bottom = USER_STACK_BOTTOM_LARGE;
        user_stack_top = USER_STACK_TOP_LARGE;
    }
    if (mem && user_stack_top > mem->usable_bytes) {
        user_stack_bottom = USER_STACK_BOTTOM_SMALL;
        user_stack_top = USER_STACK_TOP_SMALL;
    }
    uintptr_t user_heap_base = USER_HEAP_BASE;
    uintptr_t user_heap_limit = user_stack_bottom;
    uintptr_t protected_end = boot_modules_end_in_range(USER_BASE, user_stack_bottom);
    if (protected_end > user_heap_base) {
        user_heap_base = page_align_up(protected_end);
    }
    if (user_heap_base >= user_heap_limit || user_heap_limit - user_heap_base < PAGE_SIZE) {
        log_warn("exec", "no userspace heap window path=%s heap_base=%p heap_limit=%p",
                 resolved, (void *)user_heap_base, (void *)user_heap_limit);
        return -1;
    }

    /*
     * Ensure ELF segments fit within our mapped regions.
     * highest_vaddr may reach into the heap area for large binaries (nano, doom).
     * We allow it as long as it stays below user_heap_limit.
     */
    if (info.lowest_vaddr < USER_BASE ||
        info.highest_vaddr > user_heap_base ||
        boot_module_range_overlap((uintptr_t)info.lowest_vaddr, (uintptr_t)info.highest_vaddr)) {
        log_warn("exec", "ELF image overlaps reserved boot area path=%s low=%p high=%p heap_base=%p",
                 resolved, (void *)(uintptr_t)info.lowest_vaddr,
                 (void *)(uintptr_t)info.highest_vaddr, (void *)user_heap_base);
        return -1;
    }

    /* Map code region, heap region, and stack region */
    if (vmm_map_range_identity(USER_BASE, user_heap_limit - USER_BASE,
                               VMM_FLAG_WRITABLE | VMM_FLAG_USER) < 0 ||
        vmm_map_range_identity(user_stack_bottom, user_stack_top - user_stack_bottom,
                               VMM_FLAG_WRITABLE | VMM_FLAG_USER) < 0) {
        return -1;
    }

    clear_user_range_preserving_boot_modules(USER_BASE, user_heap_limit);
    memset((void *)user_stack_bottom, 0, user_stack_top - user_stack_bottom);
    /* brk starts just after the highest ELF segment, page-aligned */
    uintptr_t elf_end = page_align_up((uintptr_t)info.highest_vaddr);
    if (elf_end < user_heap_base) {
        elf_end = user_heap_base;
    }
    user_brk_floor_current = elf_end;
    user_brk_current = elf_end;
    user_heap_limit_current = user_heap_limit;
    if (elf64_load(node->data, (size_t)node->size) < 0) {
        return -1;
    }

    uintptr_t sp = user_stack_top;
    uint64_t user_argv[MAX_ARGS];
    for (int i = arg_count - 1; i >= 0; i--) {
        size_t len = strlen(arg_values[i]) + 1;
        sp -= len;
        memcpy((void *)sp, arg_values[i], len);
        user_argv[i] = sp;
    }
    sp &= ~0xfULL;
    uint64_t *stack = (uint64_t *)sp;
    *--stack = 0;
    for (int i = arg_count - 1; i >= 0; i--) {
        *--stack = user_argv[i];
    }
    *--stack = (uint64_t)arg_count;

    struct process *proc = process_current();
    char saved_name[PROCESS_NAME_MAX + 1];
    enum process_state saved_state = PROCESS_RUNNING;
    uint8_t saved_signal_disposition[PROCESS_SIGNAL_MAX];
    uintptr_t saved_signal_handler[PROCESS_SIGNAL_MAX];
    if (proc) {
        strncpy(saved_name, proc->name, sizeof(saved_name) - 1);
        saved_name[sizeof(saved_name) - 1] = '\0';
        saved_state = proc->state;
        memcpy(saved_signal_disposition, proc->signal_disposition, sizeof(saved_signal_disposition));
        memcpy(saved_signal_handler, proc->signal_handler, sizeof(saved_signal_handler));
        memset(proc->signal_disposition, 0, sizeof(proc->signal_disposition));
        memset(proc->signal_handler, 0, sizeof(proc->signal_handler));
        strncpy(proc->name, resolved, PROCESS_NAME_MAX);
        proc->name[PROCESS_NAME_MAX] = '\0';
        proc->state = PROCESS_RUNNING;
    }

    user_exec_active = true;
    int rc = arch_enter_user(info.entry, (uint64_t)(uintptr_t)stack);
    user_exec_active = false;
    if (proc) {
        strncpy(proc->name, saved_name, PROCESS_NAME_MAX);
        proc->name[PROCESS_NAME_MAX] = '\0';
        proc->state = saved_state == PROCESS_UNUSED ? PROCESS_RUNNING : saved_state;
        if (proc->state == PROCESS_ZOMBIE) {
            proc->state = PROCESS_RUNNING;
        }
        proc->exit_code = rc;
        memcpy(proc->signal_disposition, saved_signal_disposition, sizeof(saved_signal_disposition));
        memcpy(proc->signal_handler, saved_signal_handler, sizeof(saved_signal_handler));
    }
    cpu_sti();
    return rc;
}

static long sys_sigaction(int sig, const struct user_sigaction_abi *act,
                          struct user_sigaction_abi *oldact)
{
    struct process *proc = process_current();
    if (!proc || sig <= 0 || sig >= PROCESS_SIGNAL_MAX) {
        return -1;
    }
    if (oldact) {
        oldact->handler = proc->signal_handler[sig];
        oldact->mask = 0;
        oldact->flags = 0;
    }
    if (act) {
        proc->signal_handler[sig] = act->handler;
        if (act->handler == 0) {
            proc->signal_disposition[sig] = SIGNAL_DISPOSITION_DEFAULT;
        } else if (act->handler == 1) {
            proc->signal_disposition[sig] = SIGNAL_DISPOSITION_IGNORED;
        } else {
            proc->signal_disposition[sig] = SIGNAL_DISPOSITION_HANDLED;
        }
    }
    return 0;
}

static long sys_brk(uintptr_t next)
{
    if (next == 0) {
        return (long)user_brk_current;
    }
    if (next < user_brk_floor_current) {
        return (long)user_brk_current;
    }
    /* Allow any value in [initial_brk, heap_limit]; shrink is always ok */
    if (next > user_heap_limit_current) {
        log_warn("syscall", "brk out of range next=%p limit=%p",
                 (void *)next, (void *)user_heap_limit_current);
        return (long)user_brk_current;
    }
    if (next > user_brk_current) {
        /* vmm_map_range_identity is idempotent for already-mapped pages */
        if (vmm_map_range_identity(user_brk_current, next - user_brk_current,
                                   VMM_FLAG_WRITABLE | VMM_FLAG_USER) < 0) {
            log_warn("syscall", "brk map failed start=%p len=%llu",
                     (void *)user_brk_current,
                     (uint64_t)(next - user_brk_current));
            return (long)user_brk_current;
        }
    }
    user_brk_current = next;
    return (long)user_brk_current;
}

long syscall_dispatch(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3;
    (void)a4;
    (void)a5;

    struct process *proc = process_current();

    if (user_exec_active && number != SYS_EXIT &&
        (!proc || proc->signal_disposition[SIGINT_NUMBER] == SIGNAL_DISPOSITION_DEFAULT) &&
        keyboard_consume_interrupt()) {
        process_exit(proc, 130);
        return (long)syscall_encode_result(130, SYSCALL_RET_TO_KERNEL);
    }

    switch (number) {

    case SYS_READ:
        return sys_read((int)a0, (void *)a1, (size_t)a2);
    case SYS_WRITE:
        return sys_write((int)a0, (const void *)a1, (size_t)a2);
    case SYS_OPEN:
        return sys_open((const char *)a0, (int)a1, (int)a2);
    case SYS_CLOSE:
        process_close_fd(proc, (int)a0);
        return 0;
    case SYS_GETPID:
        return proc ? proc->pid : -1;
    case SYS_GETPPID:
        return proc ? proc->ppid : -1;
    case SYS_GETUID:
        return proc ? (long)proc->uid : -1;
    case SYS_GETGID:
        return proc ? (long)proc->gid : -1;
    case SYS_LSEEK:
        return sys_lseek((int)a0, (int64_t)a1, (int)a2);
    case SYS_ACCESS:
        return sys_access((const char *)a0, (int)a1);
    case SYS_DUP:
        return process_dup_fd(proc, (int)a0);
    case SYS_DUP2:
        return process_dup2_fd(proc, (int)a0, (int)a1);
    case SYS_READDIR:
        return sys_readdir((int)a0, (struct syscall_dirent *)a1);
    case SYS_IOCTL:
        return sys_ioctl((int)a0, (unsigned long)a1, (void *)a2);
    case SYS_UPTIME_MS:
        return (long)(pit_ticks() * 10);
    case SYS_BRK:
        return sys_brk((uintptr_t)a0);
    case SYS_SIGACTION:
        return sys_sigaction((int)a0, (const struct user_sigaction_abi *)a1,
                             (struct user_sigaction_abi *)a2);
    case SYS_CHDIR: {
        struct vfs_node *node = vfs_lookup((const char *)a0, proc->cwd);
        if (!node || node->type != VFS_NODE_DIR || !has_perm(proc, node, 1)) {
            return -1;
        }
        return vfs_normalize((const char *)a0, proc->cwd, proc->cwd, sizeof(proc->cwd));
    }
    case SYS_GETCWD:
        strncpy((char *)a0, proc->cwd, (size_t)a1);
        return 0;
    case SYS_MKDIR:
    {
        char normal[VFS_PATH_MAX];
        if (normalize_for_process(proc, (const char *)a0, normal, sizeof(normal)) < 0) {
            return -1;
        }
        if (!is_root(proc) && path_requires_root_for_mutation(normal)) {
            return -1;
        }
        if (!can_create_in_parent(proc, normal)) {
            return -1;
        }
        return vfs_mkdir(normal, "/", VFS_S_IFDIR | ((uint32_t)a1 & 0777),
                         proc->uid, proc->gid);
    }
    case SYS_UNLINK:
    {
        char normal[VFS_PATH_MAX];
        if (normalize_for_process(proc, (const char *)a0, normal, sizeof(normal)) < 0 ||
            strcmp(normal, "/") == 0) {
            return -1;
        }
        struct vfs_node *node = vfs_lookup(normal, "/");
        struct vfs_node *parent = lookup_parent_from_normal(normal);
        if (!node || !parent || parent->type != VFS_NODE_DIR) {
            return -1;
        }
        if (!is_root(proc) && path_requires_root_for_mutation(normal)) {
            return -1;
        }
        if (!has_perm(proc, parent, 2) || !has_perm(proc, parent, 1)) {
            return -1;
        }
        return vfs_unlink(normal, "/");
    }
    case SYS_STAT:
        return vfs_stat((const char *)a0, proc->cwd, (struct vfs_stat *)a1);
    case SYS_FSTAT:
        return sys_fstat((int)a0, (struct vfs_stat *)a1);
    case SYS_CHMOD:
    {
        char normal[VFS_PATH_MAX];
        if (normalize_for_process(proc, (const char *)a0, normal, sizeof(normal)) < 0) {
            return -1;
        }
        struct vfs_node *node = vfs_lookup(normal, "/");
        if (!node) {
            return -1;
        }
        if (!is_root(proc) && path_requires_root_for_mutation(normal)) {
            return -1;
        }
        if (!is_root(proc) && proc->uid != node->uid) {
            return -1;
        }
        return vfs_chmod(normal, "/", (uint32_t)a1);
    }
    case SYS_CHOWN:
    {
        char normal[VFS_PATH_MAX];
        if (!is_root(proc) ||
            normalize_for_process(proc, (const char *)a0, normal, sizeof(normal)) < 0) {
            return -1;
        }
        return vfs_chown(normal, "/", (uint32_t)a1, (uint32_t)a2);
    }
    case SYS_SPAWN: {
        const struct user_record *u = user_current();
        struct process *child = process_create((const char *)a0, proc ? proc->pid : 0, u->uid, u->gid);
        return child ? child->pid : -1;
    }
    case SYS_EXEC:
        return sys_exec_image((const char *)a0, (int)a1, (char **)a2);
    case SYS_WAIT:
        return -1;
    case SYS_EXIT:
        process_exit(proc, (int)a0);
        return (long)syscall_encode_result((long)a0, SYSCALL_RET_TO_KERNEL);

    default:
        return -1;
    }
}
