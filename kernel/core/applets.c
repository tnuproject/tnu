#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/keyboard.h>
#include <arch/pit.h>
#include <tnu/applets.h>
#include <tnu/console.h>
#include <tnu/log.h>
#include <tnu/memory.h>
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

static const char *applet_stdin;

struct applet_command {
    const char *name;
    int (*fn)(int argc, char **argv);
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

static void mode_string(uint32_t mode, enum vfs_node_type type, char out[11])
{
    out[0] = type == VFS_NODE_DIR ? 'd' : type == VFS_NODE_DEV ? 'c' : type == VFS_NODE_PROC ? 'p' : '-';
    const char bits[] = { 'r', 'w', 'x' };
    for (int i = 0; i < 9; i++) {
        out[i + 1] = (mode & (1u << (8 - i))) ? bits[i % 3] : '-';
    }
    out[10] = '\0';
}

static void ls_emit(struct vfs_node *node, void *ctx)
{
    (void)ctx;
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
    proc_first_line("/proc/version", version, sizeof(version), "TNU unknown x86_64");
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
    read_file_text("/etc/hostname", host, sizeof(host), "tnu");
    strip_first_newline(host);
    read_file_text("/etc/sysfetch-logo", logo, sizeof(logo), "TNU\n");
    os_release_value("PRETTY_NAME", pretty, sizeof(pretty));
    os_release_value("NAME", name, sizeof(name));
    os_release_value("VERSION_ID", version_id, sizeof(version_id));
    os_release_value("VERSION_CODENAME", codename, sizeof(codename));
    proc_first_line("/proc/version", kernel, sizeof(kernel), "TNU unknown x86_64");
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
    kprintf("Memory: %llu KiB usable / %llu KiB total\n",
            mem->usable_bytes / 1024, mem->total_bytes / 1024);
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
    const char *path = argc > 1 ? argv[1] : ".";
    struct process *proc = process_current();
    struct vfs_node *node = vfs_lookup(path, proc->cwd);
    if (!node) {
        kprintf("ls: not found: %s\n", path);
        return 1;
    }
    if (node->type == VFS_NODE_DIR) {
        vfs_list(node, ls_emit, NULL);
    } else {
        ls_emit(node, NULL);
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
    for (int i = 1; i < argc; i++) {
        kprintf("%s%s", i == 1 ? "" : " ", argv[i]);
    }
    kprintf("\n");
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: mkdir DIR...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (syscall_dispatch(SYS_MKDIR, (uint64_t)argv[i], 0755, 0, 0, 0, 0) < 0) {
            kprintf("mkdir: failed: %s\n", argv[i]);
        }
    }
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: rm FILE...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (syscall_dispatch(SYS_UNLINK, (uint64_t)argv[i], 0, 0, 0, 0, 0) < 0) {
            kprintf("rm: failed: %s\n", argv[i]);
        }
    }
    return 0;
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
    return vfs_write_file(dst, process_current()->cwd, node->data, (size_t)node->size);
}

static int cmd_cp(int argc, char **argv)
{
    if (argc != 3 || copy_file(argv[1], argv[2]) < 0) {
        kprintf("usage: cp SRC DST\n");
        return 1;
    }
    return 0;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc != 3 || copy_file(argv[1], argv[2]) < 0 ||
        vfs_unlink(argv[1], process_current()->cwd) < 0) {
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
        kprintf("chmod: failed\n");
    }
    return 0;
}

static int cmd_chown(int argc, char **argv)
{
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
        kprintf("chown: failed\n");
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
    uint64_t seconds = pit_uptime_seconds();
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
    kprintf("rebooting...\n");
    while (inb(0x64) & 0x02) {
    }
    outb(0x64, 0xfe);
    for (;;) {
        cpu_halt();
    }
    return 0;
}

static int cmd_shutdown(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("requesting ACPI/QEMU poweroff...\n");
    outw(0x604, 0x2000);
    outw(0xb004, 0x2000);
    for (;;) {
        cpu_halt();
    }
    return 0;
}

static int cmd_mount(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("rootfs on / type tfs (memory)\n");
    kprintf("devfs on /dev type devfs\n");
    kprintf("procfs on /proc type procfs\n");
    kprintf("available read-only probes: ext2 ext4 fat32\n");
    return 0;
}

static int cmd_ifconfig(int argc, char **argv)
{
    if (argc >= 3) {
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

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        procfs_refresh();
        bool found = false;
        for (size_t i = 0; i < net_iface_count(); i++) {
            const struct net_iface *iface = net_iface_get(i);
            if (iface->type == NET_IFACE_WIFI) {
                found = true;
                kprintf("%s: Intel Wi-Fi device detected; association is not online yet\n",
                        iface->name);
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
        } else {
            kprintf("wifi: Wi-Fi hardware found, but scan requires the 802.11 firmware/MAC layer\n");
        }
        return rc == 0 ? 0 : 1;
    }
    if (argc >= 4 && strcmp(argv[1], "connect") == 0) {
        int rc = net_wifi_connect(argv[2], argv[3], argc > 4 ? argv[4] : "");
        if (rc == -1) {
            kprintf("wifi: no such Wi-Fi interface: %s\n", argv[2]);
        } else {
            kprintf("wifi: connect needs the Intel Wi-Fi firmware/MAC layer before association can work\n");
        }
        return rc == 0 ? 0 : 1;
    }
    kprintf("usage: wifi [status|scan|connect IFACE SSID [PASSPHRASE]]\n");
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
    if (vfs_write_file(argv[1], process_current()->cwd, content, strlen(content)) < 0) {
        kprintf("xedit: failed to write %s\n", argv[1]);
        return 1;
    }
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
    if (argc != 2) {
        kprintf("usage: useradd NAME\n");
        return 1;
    }
    uint32_t uid = 1000 + (uint32_t)user_count();
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
    if (argc != 2 || user_del(argv[1]) < 0) {
        kprintf("usage: userdel NAME\n");
        return 1;
    }
    return 0;
}

static const struct applet_command applets[] = {
    { "clear", cmd_clear },     { "uname", cmd_uname },     { "sysfetch", cmd_sysfetch },
    { "dmesg", cmd_dmesg },     { "ls", cmd_ls },           { "pwd", cmd_pwd },
    { "cat", cmd_cat },         { "echo", cmd_echo },       { "mkdir", cmd_mkdir },
    { "rm", cmd_rm },           { "touch", cmd_touch },     { "cp", cmd_cp },
    { "mv", cmd_mv },           { "chmod", cmd_chmod },     { "chown", cmd_chown },
    { "stat", cmd_stat },       { "ps", cmd_ps },           { "kill", cmd_kill },
    { "whoami", cmd_whoami },   { "id", cmd_id },           { "hostname", cmd_hostname },
    { "date", cmd_date },       { "time", cmd_time },       { "uptime", cmd_uptime },
    { "timezone", cmd_timezone }, { "keymap", cmd_keymap }, { "layout", cmd_keymap },
    { "reboot", cmd_reboot },   { "shutdown", cmd_shutdown }, { "mount", cmd_mount },
    { "ifconfig", cmd_ifconfig }, { "route", cmd_route },
    { "netstat", cmd_netstat }, { "usb", cmd_usb },         { "ping", cmd_ping },
    { "wifi", cmd_wifi },
    { "xedit", cmd_xedit },
    { "passwd", cmd_passwd },   { "useradd", cmd_useradd },
    { "userdel", cmd_userdel },
};

const char *tnu_applet_list(void)
{
    return "clear uname sysfetch dmesg ls pwd cat echo mkdir rm touch cp mv chmod chown stat "
           "ps kill whoami id hostname date time uptime timezone keymap layout reboot shutdown "
           "mount ifconfig route netstat usb ping wifi xedit passwd useradd userdel";
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

int tnu_applet_run(int argc, char **argv, const char *stdin_data)
{
    if (argc == 0) {
        return 0;
    }
    const char *name = basename(argv[0]);
    applet_stdin = stdin_data;
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        if (strcmp(name, applets[i].name) == 0) {
            int rc = applets[i].fn(argc, argv);
            applet_stdin = NULL;
            return rc;
        }
    }
    applet_stdin = NULL;
    return 127;
}
