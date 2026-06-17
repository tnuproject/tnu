#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/keyboard.h>
#include <arch/pit.h>
#include <tnu/applets.h>
#include <tnu/console.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/iwlwifi.h>
#include <tnu/net.h>
#include <tnu/printf.h>
#include <tnu/process.h>
#include <tnu/procfs.h>
#include <tnu/string.h>
#include <tnu/syscall.h>
#include <tnu/time.h>
#include <tnu/user.h>
#include <tnu/version.h>
#include <tnu/vfs.h>

#define APPLET_LINE_MAX 256

static const char sysfetch_default_logo[] =
    "___________           \n"
    "\\__    ___/___  __ __ \n"
    "  |    | /    \\|  |  \\\n"
    "  |    ||   |  \\  |  /\n"
    "  |____||___|  /____/ \n"
    "             \\/       \n";

static const char *applet_stdin;

struct applet_command {
    const char *name;
    int (*fn)(int argc, char **argv);
    const char *usage;
    const char *help;
};

static const char *basename(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            last = p + 1;
        }
    }
    return last;
}

static void read_file_text(const char *path, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }
    struct vfs_node *node = vfs_lookup(path, "/");
    if (!node || !node->data) {
        strncpy(out, fallback, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    size_t n = node->size < out_size - 1 ? (size_t)node->size : out_size - 1;
    memcpy(out, node->data, n);
    out[n] = '\0';
}

static void strip_first_newline(char *out)
{
    char *nl = strchr(out, '\n');
    if (nl) {
        *nl = '\0';
    }
}

static bool os_release_value(const char *key, char *out, size_t out_size)
{
    char file[1024];
    read_file_text("/etc/os-release", file, sizeof(file), "");
    size_t key_len = strlen(key);
    char *line = file;
    for (char *p = file; ; p++) {
        if (*p == '\n' || *p == '\0') {
            char old = *p;
            *p = '\0';
            if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
                char *value = line + key_len + 1;
                if (*value == '"') {
                    value++;
                    char *end = strrchr(value, '"');
                    if (end) {
                        *end = '\0';
                    }
                }
                strncpy(out, value, out_size - 1);
                out[out_size - 1] = '\0';
                return true;
            }
            if (old == '\0') {
                break;
            }
            line = p + 1;
        }
    }
    if (out_size) {
        out[0] = '\0';
    }
    return false;
}

static void proc_first_line(const char *path, char *out, size_t out_size, const char *fallback)
{
    procfs_refresh();
    read_file_text(path, out, out_size, fallback);
    strip_first_newline(out);
}

static void write_hostname(const char *name)
{
    char buf[96];
    ksnprintf(buf, sizeof(buf), "%s\n", name);
    vfs_write_file("/etc/hostname", "/", buf, strlen(buf));
}

static bool require_root(const char *cmd)
{
    if (process_current() && process_current()->uid == 0) {
        return true;
    }
    kprintf("%s: permission denied: requires root; try sudo %s\n", cmd, cmd);
    return false;
}

static bool path_is_or_under(const char *path, const char *prefix)
{
    size_t len = strlen(prefix);
    if (strcmp(path, prefix) == 0) {
        return true;
    }
    return len > 1 && strncmp(path, prefix, len) == 0 && path[len] == '/';
}

static bool applet_path_requires_root_for_mutation(const char *path)
{
    char normal[VFS_PATH_MAX];
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

    if (vfs_normalize(path, process_current() ? process_current()->cwd : "/",
                      normal, sizeof(normal)) < 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(protected_prefixes) / sizeof(protected_prefixes[0]); i++) {
        if (path_is_or_under(normal, protected_prefixes[i])) {
            return true;
        }
    }
    return false;
}

static bool report_permission_denied_if_protected(const char *cmd, const char *path)
{
    if (process_current() && process_current()->uid == 0) {
        return false;
    }
    if (!applet_path_requires_root_for_mutation(path)) {
        return false;
    }
    kprintf("%s: permission denied: requires root: %s\n", cmd, path);
    return true;
}

static bool applet_interrupted(void)
{
    int ch = keyboard_try_getchar();
    if (ch == KEY_CTRL_C) {
        kprintf("^C\n");
        return true;
    }
    return false;
}

static void mode_string(uint32_t mode, enum vfs_node_type type, char out[11])
{
    out[0] = type == VFS_NODE_DIR ? 'd' : type == VFS_NODE_DEV ? 'c' : type == VFS_NODE_PROC ? 'p' : '-';
    const char bits[] = { 'r', 'w', 'x' };
    for (int i = 0; i < 9; i++) {
        out[i + 1] = (mode & (1u << (8 - i))) ? bits[i % 3] : '-';
    }
    out[10] = '\0';
}

static int parse_mode_octal(const char *text, uint32_t *out)
{
    if (!text || !*text || !out) {
        return -1;
    }
    uint32_t mode = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '7') {
            return -1;
        }
        mode = (mode << 3) | (uint32_t)(*p - '0');
        if (mode > 07777) {
            return -1;
        }
    }
    *out = mode;
    return 0;
}

static int mkdir_p_path(const char *path, uint32_t mode)
{
    char normal[VFS_PATH_MAX];
    char partial[VFS_PATH_MAX];
    if (vfs_normalize(path, process_current()->cwd, normal, sizeof(normal)) < 0) {
        return -1;
    }
    if (strcmp(normal, "/") == 0) {
        return 0;
    }

    partial[0] = '/';
    partial[1] = '\0';
    const char *p = normal + 1;
    while (*p) {
        char comp[VFS_NAME_MAX + 1];
        size_t n = 0;
        while (*p && *p != '/' && n < VFS_NAME_MAX) {
            comp[n++] = *p++;
        }
        comp[n] = '\0';
        while (*p == '/') {
            p++;
        }
        if (n == 0) {
            continue;
        }
        if (strcmp(partial, "/") != 0) {
            strcat(partial, "/");
        }
        if (strlen(partial) + strlen(comp) + 1 >= sizeof(partial)) {
            return -1;
        }
        strcat(partial, comp);
        struct vfs_node *node = vfs_lookup(partial, "/");
        if (node) {
            if (node->type != VFS_NODE_DIR) {
                return -1;
            }
            continue;
        }
        if (syscall_dispatch(SYS_MKDIR, (uint64_t)partial, mode, 0, 0, 0, 0) < 0) {
            return -1;
        }
    }
    return 0;
}

struct ls_context {
    bool long_format;
    bool all;
};

static void ls_emit(struct vfs_node *node, void *ctx)
{
    struct ls_context *ls = ctx;
    if (!node || (!ls->all && node->name[0] == '.')) {
        return;
    }
    if (!ls->long_format) {
        kprintf("%s%s  ", node->name, node->type == VFS_NODE_DIR ? "/" : "");
        return;
    }
    char mode[11];
    mode_string(node->mode, node->type, mode);
    kprintf("%s %4u:%-4u %8llu %s%s\n", mode, node->uid, node->gid,
            node->size, node->name, node->type == VFS_NODE_DIR ? "/" : "");
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    console_clear();
    return 0;
}

static int cmd_uname(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char version[128];
    proc_first_line("/proc/version", version, sizeof(version), "Tiramisù unknown x86_64");
    kprintf("%s\n", version);
    return 0;
}

static int cmd_sysfetch(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char host[64];
    char cpu[128];
    char logo[512];
    char pretty[128];
    char name[64];
    char version_id[32];
    char codename[32];
    char kernel[128];
    char uptime[64];
    const struct memory_stats *mem = memory_stats_get();
    read_file_text("/etc/hostname", host, sizeof(host), "tiramisu");
    strip_first_newline(host);
    read_file_text("/etc/sysfetch-logo", logo, sizeof(logo), sysfetch_default_logo);
    os_release_value("PRETTY_NAME", pretty, sizeof(pretty));
    os_release_value("NAME", name, sizeof(name));
    os_release_value("VERSION_ID", version_id, sizeof(version_id));
    os_release_value("VERSION_CODENAME", codename, sizeof(codename));
    proc_first_line("/proc/version", kernel, sizeof(kernel), "Tiramisù unknown x86_64");
    proc_first_line("/proc/uptime", uptime, sizeof(uptime), "0");
    cpu_get_brand(cpu, sizeof(cpu));
    kprintf("%s", logo);
    if (logo[0] && logo[strlen(logo) - 1] != '\n') {
        kprintf("\n");
    }
    kprintf("OS: %s\n", pretty[0] ? pretty : (name[0] ? name : TNU_NAME));
    kprintf("Kernel: %s\n", kernel);
    kprintf("Version: %s%s%s%s\n", version_id[0] ? version_id : "?",
            codename[0] ? " (" : "", codename[0] ? codename : "",
            codename[0] ? ")" : "");
    kprintf("Architecture: %s\n", TNU_ARCH);
    kprintf("Hostname: %s\n", host);
    kprintf("Uptime: %s seconds\n", uptime);
    const uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    uint64_t usable_tenths = (mem->usable_bytes * 10 + gib / 2) / gib;
    uint64_t total_tenths = (mem->total_bytes * 10 + gib / 2) / gib;
    kprintf("Memory: ");
    if (usable_tenths % 10 == 0) {
        kprintf("%lluGB", usable_tenths / 10);
    } else {
        kprintf("%llu.%lluGB", usable_tenths / 10, usable_tenths % 10);
    }
    kprintf(" usable out of ");
    if (total_tenths % 10 == 0) {
        kprintf("%lluGB", total_tenths / 10);
    } else {
        kprintf("%llu.%lluGB", total_tenths / 10, total_tenths % 10);
    }
    kprintf("\n");
    kprintf("Shell: /bin/tsh\n");
    kprintf("CPU: %s\n", cpu);
    return 0;
}

static int cmd_dmesg(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("%s", log_buffer());
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    procfs_refresh();
    struct ls_context ctx = { .long_format = false, .all = false };
    int first_path = 1;
    for (; first_path < argc; first_path++) {
        if (strcmp(argv[first_path], "--") == 0) {
            first_path++;
            break;
        }
        if (argv[first_path][0] != '-' || argv[first_path][1] == '\0') {
            break;
        }
        for (const char *p = argv[first_path] + 1; *p; p++) {
            if (*p == 'l') {
                ctx.long_format = true;
            } else if (*p == 'a') {
                ctx.all = true;
            } else {
                kprintf("ls: invalid option -- %c\n", *p);
                kprintf("usage: ls [-la] [FILE...]\n");
                return 1;
            }
        }
    }
    const char *path = first_path < argc ? argv[first_path] : ".";
    struct process *proc = process_current();
    struct vfs_node *node = vfs_lookup(path, proc->cwd);
    if (!node) {
        kprintf("ls: not found: %s\n", path);
        return 1;
    }
    if (node->type == VFS_NODE_DIR) {
        vfs_list(node, ls_emit, &ctx);
        if (!ctx.long_format) {
            kprintf("\n");
        }
    } else {
        ls_emit(node, &ctx);
        if (!ctx.long_format) {
            kprintf("\n");
        }
    }
    for (int i = first_path + 1; i < argc; i++) {
        node = vfs_lookup(argv[i], proc->cwd);
        if (!node) {
            kprintf("ls: not found: %s\n", argv[i]);
            continue;
        }
        if (argc - first_path > 1) {
            kprintf("%s:\n", argv[i]);
        }
        if (node->type == VFS_NODE_DIR) {
            vfs_list(node, ls_emit, &ctx);
            if (!ctx.long_format) {
                kprintf("\n");
            }
        } else {
            ls_emit(node, &ctx);
            if (!ctx.long_format) {
                kprintf("\n");
            }
        }
    }
    return 0;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("%s\n", process_current()->cwd);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        if (applet_stdin) {
            kprintf("%s", applet_stdin);
            return 0;
        }
        kprintf("usage: cat FILE...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        procfs_refresh();
        struct vfs_node *node = vfs_lookup(argv[i], process_current()->cwd);
        if (!node || node->type == VFS_NODE_DIR) {
            kprintf("cat: cannot read %s\n", argv[i]);
            continue;
        }
        if (node->data && node->size) {
            console_write_n((const char *)node->data, (size_t)node->size);
            if (node->data[node->size - 1] != '\n') {
                console_putc('\n');
            }
        }
    }
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    bool newline = true;
    int start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        start = 2;
    }
    for (int i = start; i < argc; i++) {
        kprintf("%s%s", i == start ? "" : " ", argv[i]);
    }
    if (newline) {
        kprintf("\n");
    }
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    bool parents = false;
    uint32_t mode = 0755;
    int first_path = 1;
    for (; first_path < argc; first_path++) {
        if (strcmp(argv[first_path], "--") == 0) {
            first_path++;
            break;
        }
        if (strcmp(argv[first_path], "-p") == 0) {
            parents = true;
            continue;
        }
        if (strcmp(argv[first_path], "-m") == 0) {
            if (first_path + 1 >= argc ||
                parse_mode_octal(argv[first_path + 1], &mode) < 0) {
                kprintf("mkdir: invalid mode\n");
                return 1;
            }
            first_path++;
            continue;
        }
        if (argv[first_path][0] == '-' && argv[first_path][1] != '\0') {
            kprintf("mkdir: invalid option: %s\n", argv[first_path]);
            return 1;
        }
        break;
    }
    if (first_path >= argc) {
        kprintf("usage: mkdir [-p] [-m MODE] DIR...\n");
        return 1;
    }
    int status = 0;
    for (int i = first_path; i < argc; i++) {
        int rc = parents
                     ? mkdir_p_path(argv[i], mode)
                     : (int)syscall_dispatch(SYS_MKDIR, (uint64_t)argv[i], mode, 0, 0, 0, 0);
        if (rc < 0) {
            if (!report_permission_denied_if_protected("mkdir", argv[i])) {
                kprintf("mkdir: failed: %s\n", argv[i]);
            }
            status = 1;
        }
    }
    return status;
}

static int rm_recursive_path(const char *path, bool recursive, bool force)
{
    char normal[VFS_PATH_MAX];
    if (vfs_normalize(path, process_current()->cwd, normal, sizeof(normal)) < 0) {
        if (!force) {
            kprintf("rm: invalid path: %s\n", path);
        }
        return force ? 0 : 1;
    }
    if (report_permission_denied_if_protected("rm", normal)) {
        return 1;
    }

    struct vfs_node *node = vfs_lookup(normal, "/");
    if (!node) {
        if (!force) {
            kprintf("rm: not found: %s\n", path);
        }
        return force ? 0 : 1;
    }

    if (node->type == VFS_NODE_DIR) {
        if (!recursive) {
            kprintf("rm: %s: is a directory\n", path);
            return 1;
        }
        while (node->first_child) {
            char child[VFS_PATH_MAX];
            if (strcmp(normal, "/") == 0) {
                ksnprintf(child, sizeof(child), "/%s", node->first_child->name);
            } else {
                ksnprintf(child, sizeof(child), "%s/%s", normal, node->first_child->name);
            }
            if (rm_recursive_path(child, true, force) != 0 && !force) {
                return 1;
            }
            node = vfs_lookup(normal, "/");
            if (!node) {
                return 0;
            }
        }
        if (strcmp(normal, "/") == 0) {
            return 0;
        }
    }

    if (syscall_dispatch(SYS_UNLINK, (uint64_t)normal, 0, 0, 0, 0, 0) < 0) {
        if (!force) {
            if (!report_permission_denied_if_protected("rm", normal)) {
                kprintf("rm: failed: %s\n", path);
            }
        }
        return force ? 0 : 1;
    }
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: rm [-r] [-f] FILE...\n");
        return 1;
    }
    bool recursive = false;
    bool force = false;
    int first_path = 1;
    for (; first_path < argc; first_path++) {
        if (argv[first_path][0] != '-' || argv[first_path][1] == '\0') {
            break;
        }
        for (const char *p = argv[first_path] + 1; *p; p++) {
            if (*p == 'r' || *p == 'R') {
                recursive = true;
            } else if (*p == 'f') {
                force = true;
            } else {
                kprintf("rm: unknown option -- %c\n", *p);
                return 1;
            }
        }
    }
    if (first_path >= argc) {
        if (!force) {
            kprintf("rm: missing operand\n");
            return 1;
        }
        return 0;
    }
    int status = 0;
    for (int i = first_path; i < argc; i++) {
        if (rm_recursive_path(argv[i], recursive, force) != 0) {
            status = 1;
        }
    }
    return status;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: touch FILE...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (!vfs_lookup(argv[i], process_current()->cwd)) {
            int fd = (int)syscall_dispatch(SYS_OPEN, (uint64_t)argv[i], VFS_O_CREAT | VFS_O_RDWR, 0644, 0, 0, 0);
            if (fd >= 0) {
                syscall_dispatch(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
            } else if (!report_permission_denied_if_protected("touch", argv[i])) {
                kprintf("touch: failed: %s\n", argv[i]);
            }
        }
    }
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    struct vfs_node *node = vfs_lookup(src, process_current()->cwd);
    if (!node || node->type != VFS_NODE_FILE) {
        return -1;
    }
    int src_fd = (int)syscall_dispatch(SYS_OPEN, (uint64_t)src, VFS_O_RDONLY, 0, 0, 0, 0);
    if (src_fd < 0) {
        return -1;
    }
    syscall_dispatch(SYS_CLOSE, (uint64_t)src_fd, 0, 0, 0, 0, 0);

    int dst_fd = (int)syscall_dispatch(SYS_OPEN, (uint64_t)dst,
                                       VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC,
                                       node->mode & 0777, 0, 0, 0);
    if (dst_fd < 0) {
        return -1;
    }
    if (node->size == 0) {
        syscall_dispatch(SYS_CLOSE, (uint64_t)dst_fd, 0, 0, 0, 0, 0);
        return 0;
    }
    long written = syscall_dispatch(SYS_WRITE, (uint64_t)dst_fd, (uint64_t)node->data,
                                    (size_t)node->size, 0, 0, 0);
    syscall_dispatch(SYS_CLOSE, (uint64_t)dst_fd, 0, 0, 0, 0, 0);
    return written == (long)node->size ? 0 : -1;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc != 3 || copy_file(argv[1], argv[2]) < 0) {
        if (argc == 3 && report_permission_denied_if_protected("cp", argv[2])) {
            return 1;
        }
        kprintf("usage: cp SRC DST\n");
        return 1;
    }
    return 0;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc != 3 || copy_file(argv[1], argv[2]) < 0 ||
        syscall_dispatch(SYS_UNLINK, (uint64_t)argv[1], 0, 0, 0, 0, 0) < 0) {
        if (argc == 3 &&
            (report_permission_denied_if_protected("mv", argv[1]) ||
             report_permission_denied_if_protected("mv", argv[2]))) {
            return 1;
        }
        kprintf("usage: mv SRC DST\n");
        return 1;
    }
    return 0;
}

static int cmd_chmod(int argc, char **argv)
{
    if (argc != 3) {
        kprintf("usage: chmod MODE FILE\n");
        return 1;
    }
    uint32_t mode = (uint32_t)strtol(argv[1], NULL, 8);
    if (syscall_dispatch(SYS_CHMOD, (uint64_t)argv[2], mode, 0, 0, 0, 0) < 0) {
        if (!report_permission_denied_if_protected("chmod", argv[2])) {
            kprintf("chmod: failed: %s\n", argv[2]);
        }
        return 1;
    }
    return 0;
}

static int cmd_chown(int argc, char **argv)
{
    if (!require_root("chown")) {
        return 1;
    }
    if (argc != 3) {
        kprintf("usage: chown USER FILE\n");
        return 1;
    }
    const struct user_record *u = user_find_name(argv[1]);
    if (!u) {
        kprintf("chown: unknown user: %s\n", argv[1]);
        return 1;
    }
    if (syscall_dispatch(SYS_CHOWN, (uint64_t)argv[2], u->uid, u->gid, 0, 0, 0) < 0) {
        kprintf("chown: failed: %s\n", argv[2]);
        return 1;
    }
    return 0;
}

static int cmd_stat(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("usage: stat FILE\n");
        return 1;
    }
    struct vfs_stat st;
    if (syscall_dispatch(SYS_STAT, (uint64_t)argv[1], (uint64_t)&st, 0, 0, 0, 0) < 0) {
        kprintf("stat: not found\n");
        return 1;
    }
    char mode[11];
    mode_string(st.mode, st.type, mode);
    kprintf("File: %s\nType: %d\nMode: %s (%o)\nOwner: %u:%u\nSize: %llu\n",
            argv[1], st.type, mode, st.mode & 07777, st.uid, st.gid, st.size);
    return 0;
}

static void ps_emit(const struct process *proc, void *ctx)
{
    (void)ctx;
    kprintf("%4d %4d %-8s %-16s %s\n", proc->pid, proc->ppid,
            process_state_name(proc->state), proc->name, proc->cwd);
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf(" PID PPID STATE    NAME             CWD\n");
    process_each(ps_emit, NULL);
    return 0;
}

static int cmd_kill(int argc, char **argv)
{
    if (argc != 2 || process_kill(atoi(argv[1])) < 0) {
        kprintf("usage: kill PID\n");
        return 1;
    }
    return 0;
}

static int cmd_whoami(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("%s\n", user_current()->name);
    return 0;
}

static int cmd_id(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const struct user_record *u = user_current();
    kprintf("uid=%u(%s) gid=%u\n", u->uid, u->name, u->gid);
    return 0;
}

static int cmd_hostname(int argc, char **argv)
{
    if (argc > 1) {
        if (!require_root("hostname")) {
            return 1;
        }
        write_hostname(argv[1]);
    }
    char host[64];
    read_file_text("/etc/hostname", host, sizeof(host), "tnu");
    strip_first_newline(host);
    kprintf("%s\n", host);
    return 0;
}

static int timezone_offset_minutes(const char *tz)
{
    if (!tz || !tz[0] || strcmp(tz, "UTC") == 0 || strcmp(tz, "Z") == 0) {
        return 0;
    }
    if (strncmp(tz, "UTC", 3) != 0 && strncmp(tz, "GMT", 3) != 0) {
        return 0;
    }
    const char *p = tz + 3;
    int sign = 1;
    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        sign = -1;
        p++;
    } else {
        return 0;
    }
    int hours = 0;
    int minutes = 0;
    while (*p >= '0' && *p <= '9') {
        hours = hours * 10 + (*p - '0');
        p++;
    }
    if (*p == ':') {
        p++;
        while (*p >= '0' && *p <= '9') {
            minutes = minutes * 10 + (*p - '0');
            p++;
        }
    }
    if (hours > 14 || minutes > 59) {
        return 0;
    }
    return sign * (hours * 60 + minutes);
}

static bool timezone_valid(const char *tz)
{
    if (!tz || !tz[0] || strcmp(tz, "UTC") == 0 || strcmp(tz, "Z") == 0) {
        return true;
    }
    if (strncmp(tz, "UTC", 3) != 0 && strncmp(tz, "GMT", 3) != 0) {
        return false;
    }
    const char *p = tz + 3;
    if (*p != '+' && *p != '-') {
        return false;
    }
    p++;
    int digits = 0;
    int hours = 0;
    while (*p >= '0' && *p <= '9') {
        hours = hours * 10 + (*p - '0');
        digits++;
        p++;
    }
    if (digits == 0 || hours > 14) {
        return false;
    }
    if (*p == ':') {
        p++;
        int minutes = 0;
        int mdigits = 0;
        while (*p >= '0' && *p <= '9') {
            minutes = minutes * 10 + (*p - '0');
            mdigits++;
            p++;
        }
        if (mdigits != 2 || minutes > 59) {
            return false;
        }
    }
    return *p == '\0';
}

static bool leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int month_days(int year, int month)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

static void apply_timezone(struct rtc_time *t, int offset_minutes)
{
    int total = t->hour * 60 + t->minute + offset_minutes;
    while (total < 0) {
        total += 24 * 60;
        t->day--;
        if (t->day < 1) {
            t->month--;
            if (t->month < 1) {
                t->month = 12;
                t->year--;
            }
            t->day = month_days(t->year, t->month);
        }
    }
    while (total >= 24 * 60) {
        total -= 24 * 60;
        t->day++;
        if (t->day > month_days(t->year, t->month)) {
            t->day = 1;
            t->month++;
            if (t->month > 12) {
                t->month = 1;
                t->year++;
            }
        }
    }
    t->hour = total / 60;
    t->minute = total % 60;
}

static void read_timezone(char *tz, size_t size)
{
    read_file_text("/etc/timezone", tz, size, "UTC");
    strip_first_newline(tz);
    if (!tz[0]) {
        strcpy(tz, "UTC");
    }
}

static int cmd_date(int argc, char **argv)
{
    bool utc = argc > 1 && strcmp(argv[1], "-u") == 0;
    struct rtc_time t;
    if (rtc_read_time(&t) < 0) {
        kprintf("date: RTC unavailable\n");
        return 1;
    }
    char tz[32];
    read_timezone(tz, sizeof(tz));
    if (!utc) {
        apply_timezone(&t, timezone_offset_minutes(tz));
    }
    kprintf("%04d-%02d-%02d %02d:%02d:%02d %s\n",
            t.year, t.month, t.day, t.hour, t.minute, t.second, utc ? "UTC" : tz);
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    bool utc = argc > 1 && strcmp(argv[1], "-u") == 0;
    struct rtc_time t;
    if (rtc_read_time(&t) < 0) {
        kprintf("time: RTC unavailable\n");
        return 1;
    }
    char tz[32];
    read_timezone(tz, sizeof(tz));
    if (!utc) {
        apply_timezone(&t, timezone_offset_minutes(tz));
    }
    kprintf("%02d:%02d:%02d %s\n", t.hour, t.minute, t.second, utc ? "UTC" : tz);
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint64_t seconds = time_uptime_seconds();
    kprintf("%llu days, %02llu:%02llu:%02llu\n",
            seconds / 86400,
            (seconds / 3600) % 24,
            (seconds / 60) % 60,
            seconds % 60);
    return 0;
}

static int cmd_timezone(int argc, char **argv)
{
    if (argc == 1) {
        char tz[32];
        read_timezone(tz, sizeof(tz));
        kprintf("%s\n", tz);
        return 0;
    }
    if (argc != 2 || !timezone_valid(argv[1])) {
        kprintf("usage: timezone UTC|UTC+H|UTC-H|UTC+HH:MM\n");
        return 1;
    }
    if (!require_root("timezone")) {
        return 1;
    }
    char buf[40];
    ksnprintf(buf, sizeof(buf), "%s\n", argv[1]);
    vfs_write_file("/etc/timezone", "/", buf, strlen(buf));
    kprintf("timezone set to %s\n", argv[1]);
    return 0;
}

static int cmd_keymap(int argc, char **argv)
{
    if (argc == 1) {
        kprintf("%s\n", keyboard_current_layout());
        kprintf("available: %s\n", keyboard_available_layouts());
        return 0;
    }
    if (argc != 2 || keyboard_set_layout(argv[1]) < 0) {
        kprintf("usage: keymap {%s}\n", keyboard_available_layouts());
        return 1;
    }
    if (!require_root("keymap")) {
        return 1;
    }
    char buf[32];
    ksnprintf(buf, sizeof(buf), "%s\n", argv[1]);
    vfs_write_file("/etc/keymap", "/", buf, strlen(buf));
    kprintf("keyboard layout set to %s\n", argv[1]);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!require_root("reboot")) {
        return 1;
    }
    kprintf("rebooting...\n");
    cpu_cli();
    while (inb(0x64) & 0x02) {
    }
    outb(0x64, 0xfe);
    for (volatile size_t i = 0; i < 1000000; i++) {
        cpu_pause();
    }
    outb(0xcf9, 0x02);
    io_wait();
    outb(0xcf9, 0x06);
    for (;;) {
        cpu_halt();
    }
    return 0;
}

static int cmd_shutdown(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!require_root("shutdown")) {
        return 1;
    }
    kprintf("requesting poweroff...\n");
    cpu_cli();
    outw(0x604, 0x2000);
    outw(0xb004, 0x2000);
    outw(0x4004, 0x3400);
    outw(0x600, 0x34);
    kprintf("shutdown: firmware did not power off; halting CPU\n");
    for (;;) {
        cpu_halt();
    }
    return 0;
}

static int cmd_mount(int argc, char **argv)
{
    (void)argv;
    if (argc > 1 && !require_root("mount")) {
        return 1;
    }
    if (argc > 1) {
        kprintf("mount: mounting additional filesystems is not available in this build\n");
        return 1;
    }
    kprintf("rootfs on / type tfs (memory)\n");
    kprintf("devfs on /dev type devfs\n");
    kprintf("procfs on /proc type procfs\n");
    kprintf("available read-only probes: ext2 ext4 fat32\n");
    return 0;
}

static int cmd_ifconfig(int argc, char **argv)
{
    if (argc >= 3) {
        if (!require_root("ifconfig")) {
            return 1;
        }
        if (strcmp(argv[2], "up") == 0 || strcmp(argv[2], "down") == 0) {
            if (net_iface_set_up(argv[1], strcmp(argv[2], "up") == 0) < 0) {
                kprintf("ifconfig: cannot change %s\n", argv[1]);
                return 1;
            }
            procfs_refresh();
            return 0;
        }
        if ((strcmp(argv[2], "inet") == 0 && argc >= 5) || argc >= 4) {
            int base = strcmp(argv[2], "inet") == 0 ? 3 : 2;
            uint32_t ip = net_parse_ipv4(argv[base]);
            uint32_t mask = net_parse_ipv4(argv[base + 1]);
            uint32_t gateway = argc > base + 2 ? net_parse_ipv4(argv[base + 2]) : 0;
            if (!ip || !mask || (argc > base + 2 && !gateway)) {
                kprintf("usage: ifconfig IFACE inet IPv4 NETMASK [GATEWAY]\n");
                return 1;
            }
            if (net_iface_configure_ipv4(argv[1], ip, mask, gateway) < 0) {
                kprintf("ifconfig: cannot configure %s\n", argv[1]);
                return 1;
            }
            procfs_refresh();
            return 0;
        }
        kprintf("usage: ifconfig [IFACE up|down|inet IPv4 NETMASK [GATEWAY]]\n");
        return 1;
    }
    procfs_refresh();
    struct vfs_node *node = vfs_lookup("/proc/net/dev", "/");
    if (node && node->data) {
        console_write_n((const char *)node->data, (size_t)node->size);
    }
    return 0;
}

static int cmd_route(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "add") == 0 && strcmp(argv[2], "default") == 0) {
        if (!require_root("route")) {
            return 1;
        }
        uint32_t gateway = net_parse_ipv4(argv[3]);
        const struct net_iface *iface = net_iface_find(argv[4]);
        if (!gateway || !iface || !iface->ipv4 || !iface->netmask ||
            net_iface_configure_ipv4(argv[4], iface->ipv4, iface->netmask, gateway) < 0) {
            kprintf("usage: route add default GATEWAY IFACE\n");
            return 1;
        }
        procfs_refresh();
        return 0;
    }
    if (argc != 1) {
        kprintf("usage: route [add default GATEWAY IFACE]\n");
        return 1;
    }
    procfs_refresh();
    struct vfs_node *node = vfs_lookup("/proc/net/route", "/");
    if (node && node->data) {
        console_write_n((const char *)node->data, (size_t)node->size);
    }
    return 0;
}

static int cmd_dhcp(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("usage: dhcp IFACE\n");
        return 1;
    }
    if (!require_root("dhcp")) {
        return 1;
    }
    int rc = net_iface_dhcp(argv[1]);
    if (rc < 0) {
        kprintf("dhcp: failed on %s (%d)\n", argv[1], rc);
        return 1;
    }
    procfs_refresh();
    return 0;
}

static int cmd_netstat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    procfs_refresh();
    struct vfs_node *node = vfs_lookup("/proc/net/sockstat", "/");
    if (node && node->data) {
        console_write_n((const char *)node->data, (size_t)node->size);
    }
    return 0;
}

static int cmd_usb(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    procfs_refresh();
    struct vfs_node *node = vfs_lookup("/proc/usb", "/");
    if (node && node->data) {
        console_write_n((const char *)node->data, (size_t)node->size);
    }
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("usage: ping HOST|IPv4\n");
        return 1;
    }
    uint32_t ip = net_parse_ipv4(argv[1]);
    if (!ip) {
        if (net_resolve4(argv[1], &ip) < 0) {
            kprintf("ping: cannot resolve %s\n", argv[1]);
            return 1;
        }
    }
    char ip_text[32];
    net_format_ipv4(ip, ip_text, sizeof(ip_text));
    if (strcmp(argv[1], ip_text) != 0) {
        kprintf("PING %s (%s)\n", argv[1], ip_text);
    }
    for (uint16_t seq = 1; seq <= 4; seq++) {
        if (applet_interrupted()) {
            return 130;
        }
        uint32_t latency = 0;
        if (net_ping4(ip, seq, &latency) == 0) {
            kprintf("64 bytes from %s: icmp_seq=%u ttl=64 time=%u ms\n",
                    ip_text, seq, latency);
        } else {
            kprintf("ping: %s: external IPv4 transport is not online\n", ip_text);
            return 1;
        }
    }
    return 0;
}

static const struct net_iface *first_wifi_iface(void)
{
    for (size_t i = 0; i < net_iface_count(); i++) {
        const struct net_iface *iface = net_iface_get(i);
        if (iface && iface->type == NET_IFACE_WIFI) {
            return iface;
        }
    }
    return NULL;
}

static bool wifi_arg_is_iface(const char *arg)
{
    if (!arg || strncmp(arg, "wlan", 4) != 0) {
        return false;
    }
    return arg[4] >= '0' && arg[4] <= '9';
}

static const char *wifi_arg_value(int argc, char **argv, const char *name)
{
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static bool wifi_arg_present(int argc, char **argv, const char *name)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static const char *wifi_security_name(uint16_t flags)
{
    if (flags & WIFI_AP_WPA3) return "WPA3";
    if (flags & WIFI_AP_WPA2) return "WPA2";
    if (flags & WIFI_AP_WPA) return "WPA";
    if (flags & WIFI_AP_WEP) return "WEP";
    if (flags & WIFI_AP_PRIVACY) return "Protected";
    return "Open";
}

static int wifi_save_credentials(const char *iface, const char *ssid,
                                 const char *passphrase)
{
    if (!iface || !ssid || !ssid[0]) {
        return -1;
    }
    if (!vfs_lookup("/etc/wifi", "/")) {
        int mk = vfs_mkdir("/etc/wifi", "/", VFS_S_IFDIR | 0700, 0, 0);
        if (mk < 0) {
            return mk;
        }
    }

    char path[VFS_PATH_MAX];
    char body[192];
    ksnprintf(path, sizeof(path), "/etc/wifi/%s.conf", iface);
    ksnprintf(body, sizeof(body),
              "iface=%s\nssid=%s\npassphrase=%s\nautoconnect=yes\n",
              iface, ssid, passphrase ? passphrase : "");
    return vfs_write_file(path, "/", body, strlen(body));
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 &&
        (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "debug") == 0))) {
        procfs_refresh();
        bool found = false;
        for (size_t i = 0; i < net_iface_count(); i++) {
            const struct net_iface *iface = net_iface_get(i);
            if (iface->type == NET_IFACE_WIFI) {
                found = true;
                kprintf("%s: Wi-Fi hardware detected (%04x:%04x), driver=%s\n",
                        iface->name, iface->pci_vendor, iface->pci_device, iface->driver);
                if (strcmp(iface->driver, "iwlwifi") == 0) {
                    const struct iwlwifi_state *st = iwlwifi_state_for(iface);
                    if (st && st->firmware_loaded) {
                        kprintf("%s: firmware %s loaded (%llu KiB, %s, version=%u)\n",
                                iface->name, st->firmware_name,
                                (unsigned long long)(st->firmware_size / 1024),
                                st->firmware_tlv ? "TLV" : "legacy",
                                st->firmware_version);
                        if (st->runtime_section_count && !st->init_section_count) {
                            kprintf("%s: parsed=%s staged=%s runtime=%llu bytes init=runtime-only\n",
                                    iface->name,
                                    st->firmware_parsed ? "yes" : "no",
                                    st->firmware_staged ? "yes" : "no",
                                    (unsigned long long)st->runtime_section_bytes);
                        } else {
                            kprintf("%s: parsed=%s staged=%s runtime=%u+%u init=%u+%u\n",
                                    iface->name,
                                    st->firmware_parsed ? "yes" : "no",
                                    st->firmware_staged ? "yes" : "no",
                                    st->runtime_ucode.text_size,
                                    st->runtime_ucode.data_size,
                                    st->init_ucode.text_size,
                                    st->init_ucode.data_size);
                        }
                        if (st->runtime_section_count || st->init_section_count) {
                            kprintf("%s: sections runtime=%u/%llu bytes init=%u/%llu bytes api=%08x capa=%08x\n",
                                    iface->name,
                                    (uint32_t)st->runtime_section_count,
                                    (unsigned long long)st->runtime_section_bytes,
                                    (uint32_t)st->init_section_count,
                                    (unsigned long long)st->init_section_bytes,
                                    st->api_flags[0],
                                    st->capa_flags[0]);
                        }
                        kprintf("%s: running=%s alive=%s associated=%s link=%s\n",
                                iface->name,
                                st->firmware_running ? "yes" : "no",
                                st->firmware_alive ? "yes" : "no",
                                st->associated ? "yes" : "no",
                                st->link_ready ? "yes" : "no");
                        if (st->modern_transport) {
                            kprintf("%s: transport=modern Intel iwm/iwlwifi\n",
                                    iface->name);
                        } else {
                            kprintf("%s: transport=legacy Intel iwn\n", iface->name);
                        }
                        if (st->ap_count) {
                            kprintf("%s: last scan found %u AP(s)\n",
                                    iface->name, (uint32_t)st->ap_count);
                            for (size_t ap_i = 0; ap_i < IWLWIFI_AP_CACHE_MAX; ap_i++) {
                                const struct iwlwifi_ap *ap = &st->aps[ap_i];
                                if (!ap->valid) {
                                    continue;
                                }
                                kprintf("  %-32s %02x:%02x:%02x:%02x:%02x:%02x ch=%u %s\n",
                                        ap->ssid,
                                        ap->bssid[0], ap->bssid[1], ap->bssid[2],
                                        ap->bssid[3], ap->bssid[4], ap->bssid[5],
                                        ap->channel,
                                        wifi_security_name(ap->security_flags));
                            }
                        }
                    } else {
                        kprintf("%s: firmware %s missing; install it under /lib/firmware/iwlwifi\n",
                                iface->name, st && st->firmware_name ? st->firmware_name : "unknown");
                    }
                } else if (strcmp(iface->driver, "iwlwifi-pending") == 0) {
                    kprintf("%s: iwlwifi attach failed; check dmesg for firmware/MMIO/device details\n",
                            iface->name);
                } else if (strcmp(iface->driver, "iwlwifi-unsupported") == 0) {
                    kprintf("%s: Intel Wi-Fi device %04x is not mapped to an iwlwifi firmware family\n",
                            iface->name, iface->pci_device);
                } else {
                    kprintf("%s: association offline; Wi-Fi needs a supported driver\n",
                            iface->name);
                }
            }
        }
        if (!found) {
            kprintf("wifi: no Wi-Fi device detected\n");
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        int rc = net_wifi_scan();
        if (rc == -1) {
            kprintf("wifi: no Wi-Fi device detected\n");
        } else if (rc == 0 || rc == -4) {
            struct wifi_ap aps[32];
            int count = net_wifi_scan_results(aps, sizeof(aps) / sizeof(aps[0]));
            if (count <= 0) {
                kprintf("wifi: scan completed, no APs decoded\n");
            } else {
                kprintf("%-32s %-6s %-9s %-3s %s\n",
                        "SSID", "SIGNAL", "SECURITY", "CH", "BSSID");
                for (int i = 0; i < count; i++) {
                    const char *ssid = aps[i].ssid[0] ? aps[i].ssid : "<hidden>";
                    kprintf("%-32s %4d   %-9s %3u %02x:%02x:%02x:%02x:%02x:%02x\n",
                            ssid,
                            aps[i].rssi,
                            wifi_security_name(aps[i].flags),
                            aps[i].channel,
                            aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                            aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
                }
            }
        } else if (rc == -3) {
            kprintf("wifi: iwlwifi attached, but command transport is not ready\n");
        } else if (rc == -6) {
            kprintf("wifi: modern Intel scan command failed\n");
        } else if (rc == -7) {
            kprintf("wifi: legacy Intel scan command/RX parser failed\n");
        } else if (rc == -9) {
            kprintf("wifi: scan command failed on this device\n");
        } else if (rc == -10) {
            kprintf("wifi: scan completed but no APs were decoded\n");
        } else {
            kprintf("wifi: Wi-Fi hardware found, but scan requires the 802.11 firmware/MAC layer\n");
        }
        return rc == 0 ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "start") == 0) {
        if (!require_root("wifi")) {
            return 1;
        }
        const char *name = argc >= 3 ? argv[2] : "wlan0";
        for (size_t i = 0; i < net_iface_count(); i++) {
            const struct net_iface *iface = net_iface_get(i);
            if (iface->type == NET_IFACE_WIFI && strcmp(iface->name, name) == 0 &&
                strcmp(iface->driver, "iwlwifi") == 0) {
                struct net_iface *mutable_iface = (struct net_iface *)iface;
                int rc = iwlwifi_start(mutable_iface);
                if (rc == 0) {
                    kprintf("wifi: %s firmware is alive\n", name);
                    return 0;
                }
                kprintf("wifi: %s firmware start failed (%d)\n", name, rc);
                return 1;
            }
        }
        kprintf("wifi: no iwlwifi interface named %s\n", name);
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "connect") == 0) {
        if (!require_root("wifi")) {
            return 1;
        }
        const char *iface_name = NULL;
        const char *ssid = NULL;
        const char *passphrase = "";
        bool save = !wifi_arg_present(argc, argv, "--no-save");

        if (wifi_arg_is_iface(argv[2])) {
            iface_name = argv[2];
            ssid = argc >= 4 ? argv[3] : NULL;
            if (argc >= 5 && argv[4][0] != '-') {
                passphrase = argv[4];
            }
        } else {
            const struct net_iface *iface = first_wifi_iface();
            iface_name = iface ? iface->name : "wlan0";
            ssid = argv[2];
        }

        const char *pw_opt = wifi_arg_value(argc, argv, "--password");
        if (!pw_opt) {
            pw_opt = wifi_arg_value(argc, argv, "-p");
        }
        if (pw_opt) {
            passphrase = pw_opt;
        }
        if (!ssid || !ssid[0]) {
            kprintf("usage: wifi connect [IFACE] SSID [PASSPHRASE] [--password PASSPHRASE] [--no-save]\n");
            return 1;
        }

        kprintf("wifi: scanning/authenticating/associating with %s...\n", ssid);
        int rc = net_wifi_connect(iface_name, ssid, passphrase);
        if (rc == -1) {
            kprintf("wifi: no such Wi-Fi interface: %s\n", iface_name);
        } else if (rc == -3) {
            kprintf("wifi: iwlwifi attach is online, but firmware startup failed\n");
        } else if (rc == -4) {
            kprintf("wifi: associated, but DHCP failed\n");
        } else if (rc == -6) {
            kprintf("wifi: modern Intel command/RX/TX transport failed\n");
        } else if (rc == -7) {
            kprintf("wifi: legacy Intel scan/association commands failed\n");
        } else if (rc == -8) {
            kprintf("wifi: this network requires a WPA passphrase\n");
        } else if (rc == -10) {
            kprintf("wifi: SSID not found or AP out of range\n");
        } else if (rc == -11) {
            kprintf("wifi: 802.11 authentication failed or timed out\n");
        } else if (rc == -12) {
            kprintf("wifi: 802.11 association failed or timed out\n");
        } else if (rc == -13) {
            kprintf("wifi: associated, but Ethernet-over-802.11 data path failed\n");
        } else if (rc == -14) {
            kprintf("wifi: WPA timed out waiting for message 1\n");
        } else if (rc == -15) {
            kprintf("wifi: WPA timed out waiting for message 3\n");
        } else if (rc == -16) {
            kprintf("wifi: WPA pairwise CCMP setup failed\n");
        } else if (rc == -17) {
            kprintf("wifi: WPA PTK installed; CCMP data encryption is not online yet\n");
        } else {
            kprintf("wifi: connect needs the Intel Wi-Fi firmware/MAC layer before association can work\n");
        }
        if (rc == 0) {
            const struct net_iface *iface = net_iface_find(iface_name);
            char ip[32], gw[32], dns[32];
            net_format_ipv4(iface ? iface->ipv4 : 0, ip, sizeof(ip));
            net_format_ipv4(iface ? iface->gateway : 0, gw, sizeof(gw));
            net_format_ipv4(iface ? iface->dns_server : 0, dns, sizeof(dns));
            kprintf("wifi: connected\n");
            kprintf("IP: %s\n", ip);
            kprintf("Gateway: %s\n", gw);
            kprintf("DNS: %s\n", dns);
            if (save) {
                int save_rc = wifi_save_credentials(iface_name, ssid, passphrase);
                if (save_rc < 0) {
                    kprintf("wifi: connected, but could not save /etc/wifi/%s.conf (%d)\n",
                            iface_name, save_rc);
                }
            }
        }
        return rc == 0 ? 0 : 1;
    }
    kprintf("usage: wifi [status|debug|scan|start IFACE|connect [IFACE] SSID [PASSPHRASE] [--password PASSPHRASE] [--no-save]]\n");
    return 1;
}

static void applet_read_line(char *line, size_t size)
{
    size_t len = 0;
    line[0] = '\0';
    for (;;) {
        int ch = console_getchar();
        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            line[len] = '\0';
            return;
        }
        if (ch == '\b' || ch == 127) {
            if (len) {
                len--;
                console_putc('\b');
            }
            continue;
        }
        if (ch >= 32 && ch <= 255 && len + 1 < size) {
            line[len++] = (char)ch;
            console_putc((char)ch);
        }
    }
}

static void applet_read_password(char *line, size_t size)
{
    size_t len = 0;
    line[0] = '\0';
    for (;;) {
        int ch = console_getchar();
        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            line[len] = '\0';
            return;
        }
        if (ch == '\b' || ch == 127) {
            if (len) {
                len--;
            }
            continue;
        }
        if (ch >= 32 && ch <= 255 && len + 1 < size) {
            line[len++] = (char)ch;
        }
    }
}

static int cmd_xedit(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("usage: xedit FILE\n");
        return 1;
    }
    struct vfs_node *node = vfs_lookup(argv[1], process_current()->cwd);
    if (node && node->data && node->size) {
        kprintf("--- current %s ---\n", argv[1]);
        console_write_n((const char *)node->data, (size_t)node->size);
        if (node->data[node->size - 1] != '\n') {
            console_putc('\n');
        }
        kprintf("--- replacement text; single '.' line saves ---\n");
    } else {
        kprintf("--- new file; single '.' line saves ---\n");
    }

    char content[2048];
    char line[APPLET_LINE_MAX];
    content[0] = '\0';
    for (;;) {
        kprintf("> ");
        applet_read_line(line, sizeof(line));
        if (strcmp(line, ".") == 0) {
            break;
        }
        if (strlen(content) + strlen(line) + 2 >= sizeof(content)) {
            kprintf("xedit: buffer full\n");
            return 1;
        }
        strcat(content, line);
        strcat(content, "\n");
    }
    int fd = (int)syscall_dispatch(SYS_OPEN, (uint64_t)argv[1],
                                   VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC,
                                   0644, 0, 0, 0);
    if (fd < 0 ||
        syscall_dispatch(SYS_WRITE, (uint64_t)fd, (uint64_t)content,
                         strlen(content), 0, 0, 0) < 0) {
        if (fd >= 0) {
            syscall_dispatch(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
        }
        kprintf("xedit: failed to write %s\n", argv[1]);
        return 1;
    }
    syscall_dispatch(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
    kprintf("wrote %s (%llu bytes)\n", argv[1], (uint64_t)strlen(content));
    return 0;
}

static int cmd_passwd(int argc, char **argv)
{
    const char *target = argc > 1 ? argv[1] : user_current()->name;
    const struct user_record *target_user = user_find_name(target);
    if (!target_user) {
        kprintf("passwd: unknown user: %s\n", target);
        return 1;
    }
    if (strcmp(target, user_current()->name) != 0 && user_current()->uid != 0) {
        kprintf("passwd: only root can change another user's password\n");
        return 1;
    }

    char old_password[APPLET_LINE_MAX];
    char new_password[APPLET_LINE_MAX];
    char confirm[APPLET_LINE_MAX];

    if (user_has_password(target) && user_current()->uid != 0) {
        kprintf("Current password: ");
        applet_read_password(old_password, sizeof(old_password));
        if (!user_check_password(target, old_password)) {
            kprintf("passwd: authentication failed\n");
            return 1;
        }
    }

    kprintf("New password: ");
    applet_read_password(new_password, sizeof(new_password));
    if (strlen(new_password) < 1) {
        kprintf("passwd: empty passwords are not allowed\n");
        return 1;
    }
    kprintf("Retype new password: ");
    applet_read_password(confirm, sizeof(confirm));
    if (strcmp(new_password, confirm) != 0) {
        kprintf("passwd: passwords do not match\n");
        return 1;
    }
    if (user_set_password(target, new_password) < 0) {
        kprintf("passwd: failed to update password\n");
        return 1;
    }
    kprintf("passwd: password updated for %s\n", target);
    return 0;
}

static int cmd_useradd(int argc, char **argv)
{
    if (!require_root("useradd")) {
        return 1;
    }
    if (argc != 2) {
        kprintf("usage: useradd NAME\n");
        return 1;
    }
    if (!user_name_valid(argv[1])) {
        kprintf("useradd: invalid username\n");
        return 1;
    }
    uint32_t uid = user_next_uid();
    if (user_add(argv[1], uid, uid) < 0) {
        kprintf("useradd: failed\n");
        return 1;
    }
    kprintf("useradd: created %s with home /home/%s\n", argv[1], argv[1]);
    kprintf("useradd: run passwd %s to set a password\n", argv[1]);
    return 0;
}

static int cmd_userdel(int argc, char **argv)
{
    if (!require_root("userdel")) {
        return 1;
    }
    if (argc != 2 || user_del(argv[1]) < 0) {
        kprintf("usage: userdel NAME\n");
        return 1;
    }
    return 0;
}

static const struct applet_command applets[] = {
    { "clear", cmd_clear, "clear",
      "Clear the active console." },
    { "uname", cmd_uname, "uname",
      "Print kernel and machine information from /proc/version." },
    { "sysfetch", cmd_sysfetch, "sysfetch",
      "Show OS identity, kernel, uptime, memory and CPU information." },
    { "dmesg", cmd_dmesg, "dmesg",
      "Print the kernel log buffer." },
    { "ls", cmd_ls, "ls [-la] [FILE...]",
      "List files. -l prints long metadata; -a includes dot files." },
    { "pwd", cmd_pwd, "pwd",
      "Print the current working directory." },
    { "cat", cmd_cat, "cat FILE...",
      "Concatenate files to standard output." },
    { "echo", cmd_echo, "echo [-n] [ARG...]",
      "Print arguments separated by spaces; -n suppresses the trailing newline." },
    { "mkdir", cmd_mkdir, "mkdir [-p] [-m MODE] DIR...",
      "Create directories. -p creates parents; -m sets an octal mode." },
    { "rm", cmd_rm, "rm [-r] [-f] FILE...",
      "Remove files or, with -r, directory trees. Protected paths require root." },
    { "touch", cmd_touch, "touch FILE...",
      "Create files if they do not exist." },
    { "cp", cmd_cp, "cp SRC DST",
      "Copy one regular file to another path." },
    { "mv", cmd_mv, "mv SRC DST",
      "Move or rename a file using copy plus checked unlink." },
    { "chmod", cmd_chmod, "chmod MODE FILE",
      "Change file mode using an octal mode; protected paths require root." },
    { "chown", cmd_chown, "chown USER FILE",
      "Change file owner and group to USER; requires root." },
    { "stat", cmd_stat, "stat FILE",
      "Print file type, mode, owner and size." },
    { "ps", cmd_ps, "ps",
      "List kernel process table entries." },
    { "kill", cmd_kill, "kill PID",
      "Terminate a process. Non-root can only kill owned processes." },
    { "whoami", cmd_whoami, "whoami",
      "Print the current user name." },
    { "id", cmd_id, "id",
      "Print current uid and gid." },
    { "hostname", cmd_hostname, "hostname [NAME]",
      "Show or set hostname. Setting the hostname requires root." },
    { "date", cmd_date, "date [-u]",
      "Print RTC date, adjusted by /etc/timezone unless -u is used." },
    { "time", cmd_time, "time [-u]",
      "Print RTC time, adjusted by /etc/timezone unless -u is used." },
    { "uptime", cmd_uptime, "uptime",
      "Print kernel uptime." },
    { "timezone", cmd_timezone, "timezone [UTC|UTC+H|UTC-H|UTC+HH:MM]",
      "Show or set the system timezone; setting requires root." },
    { "keymap", cmd_keymap, "keymap [LAYOUT]",
      "Show or set keyboard layout; setting requires root." },
    { "layout", cmd_keymap, "layout [LAYOUT]",
      "Alias for keymap." },
    { "reboot", cmd_reboot, "reboot",
      "Reboot the machine; requires root." },
    { "shutdown", cmd_shutdown, "shutdown",
      "Power off or halt the machine; requires root." },
    { "mount", cmd_mount, "mount [DEVICE TARGET]",
      "Show mounted filesystems. Additional mounts require root and driver support." },
    { "ifconfig", cmd_ifconfig, "ifconfig [IFACE up|down|inet IPv4 NETMASK [GATEWAY]]",
      "Show or configure network interfaces; mutations require root." },
    { "route", cmd_route, "route [add default GATEWAY IFACE]",
      "Show or update the default IPv4 route; updates require root." },
    { "dhcp", cmd_dhcp, "dhcp IFACE",
      "Request DHCP configuration for an interface; requires root." },
    { "netstat", cmd_netstat, "netstat",
      "Print network stack summary from procfs." },
    { "usb", cmd_usb, "usb",
      "Print detected USB controller/device information." },
    { "ping", cmd_ping, "ping HOST|IPv4",
      "Send ICMP echo requests through the kernel network stack." },
    { "wifi", cmd_wifi, "wifi [status|debug|scan|start IFACE|connect [IFACE] SSID [--password PASS]]",
      "Inspect and manage Wi-Fi. Starting, connecting, and saving credentials require root." },
    { "xedit", cmd_xedit, "xedit FILE",
      "Open the simple line editor and save replacement text." },
    { "passwd", cmd_passwd, "passwd [USER]",
      "Change a password. Root may change any user." },
    { "useradd", cmd_useradd, "useradd NAME",
      "Create a user and home directory; requires root." },
    { "userdel", cmd_userdel, "userdel NAME",
      "Delete a non-root user; requires root." },
};

const char *tnu_applet_list(void)
{
    return "clear uname sysfetch dmesg ls pwd cat echo mkdir rm touch cp mv chmod chown stat "
           "ps kill whoami id hostname date time uptime timezone keymap layout reboot shutdown "
           "mount ifconfig route dhcp netstat usb ping wifi xedit passwd useradd userdel";
}

bool tnu_applet_is_command(const char *name)
{
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        if (strcmp(name, applets[i].name) == 0) {
            return true;
        }
    }
    return false;
}

int tnu_applet_help(const char *name)
{
    const char *base = basename(name ? name : "");
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        if (strcmp(base, applets[i].name) == 0) {
            kprintf("Usage: %s\n\n%s\n", applets[i].usage, applets[i].help);
            return 0;
        }
    }
    return -1;
}

int tnu_applet_run(int argc, char **argv, const char *stdin_data)
{
    if (argc == 0) {
        return 0;
    }
    const char *name = basename(argv[0]);
    applet_stdin = stdin_data;
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        if (strcmp(name, applets[i].name) == 0) {
            if (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
                              strcmp(argv[1], "-h") == 0)) {
                int rc = tnu_applet_help(name);
                applet_stdin = NULL;
                return rc;
            }
            int rc = applets[i].fn(argc, argv);
            applet_stdin = NULL;
            return rc;
        }
    }
    applet_stdin = NULL;
    return 127;
}
