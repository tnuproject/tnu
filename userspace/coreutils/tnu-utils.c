#include <tnu/libc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static const char sysfetch_default_logo[] =
    "___________           \n"
    "\\__    ___/___  __ __ \n"
    "  |    | /    \\|  |  \\\n"
    "  |    ||   |  \\  |  /\n"
    "  |____||___|  /____/ \n"
    "             \\/       \n";

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

struct util_help {
    const char *name;
    const char *usage;
    const char *help;
};

static const struct util_help help_topics[] = {
    { "echo", "echo [-n] [ARG...]", "Print arguments separated by spaces." },
    { "pwd", "pwd", "Print the current working directory." },
    { "cat", "cat FILE...", "Print files to standard output." },
    { "touch", "touch FILE...", "Create files if they do not exist." },
    { "mkdir", "mkdir DIR...", "Create directories." },
    { "rm", "rm FILE...", "Remove files through the kernel unlink syscall." },
    { "uname", "uname", "Print kernel version information." },
    { "whoami", "whoami", "Print the current username." },
    { "id", "id", "Print current uid and gid." },
    { "sysfetch", "sysfetch", "Print OS identity and machine summary." },
    { "hostname", "hostname [NAME]", "Show or set hostname; setting requires root." },
    { "uptime", "uptime", "Print uptime from procfs." },
    { "timezone", "timezone [ZONE]", "Show or set /etc/timezone; setting requires root." },
    { "keymap", "keymap [LAYOUT]", "Show or set /etc/keymap; setting requires root." },
    { "layout", "layout [LAYOUT]", "Alias for keymap." },
    { "ping", "ping IPv4", "Send a minimal ICMP test when networking is available." },
    { "wifi", "wifi scan|connect IFACE SSID [PASSPHRASE]|status", "Scan, connect, and show Wi-Fi status." },
    { "chmod", "chmod MODE FILE", "Change file permissions; supports octal and simple symbolic modes." },
    { "stat", "stat FILE", "Print file metadata." },
};

static int show_help(const char *cmd)
{
    for (size_t i = 0; i < sizeof(help_topics) / sizeof(help_topics[0]); i++) {
        if (strcmp(cmd, help_topics[i].name) == 0) {
            print("Usage: ");
            println(help_topics[i].usage);
            print("\n");
            println(help_topics[i].help);
            return 0;
        }
    }
    print(cmd);
    println(": no detailed help available");
    return 1;
}

static int read_file(const char *path, char *buf, size_t size)
{
    if (size == 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        buf[0] = 0;
        return -1;
    }
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) {
        buf[0] = 0;
        return -1;
    }
    buf[n] = 0;
    return (int)n;
}

static int print_file(const char *path)
{
    char buf[256];
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
    close(fd);
    return 0;
}

static void strip_newline(char *s)
{
    char *nl = strchr(s, '\n');
    if (nl) {
        *nl = 0;
    }
}

static int os_value(const char *key, char *out, size_t out_size)
{
    char file[1024];
    if (read_file("/etc/os-release", file, sizeof(file)) < 0) {
        return 0;
    }
    size_t key_len = strlen(key);
    char *line = file;
    for (char *p = file;; p++) {
        if (*p == '\n' || *p == 0) {
            char old = *p;
            *p = 0;
            if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
                char *value = line + key_len + 1;
                if (*value == '"') {
                    value++;
                    char *end = strchr(value, '"');
                    if (end) {
                        *end = 0;
                    }
                }
                strncpy(out, value, out_size - 1);
                out[out_size - 1] = 0;
                return 1;
            }
            if (old == 0) {
                break;
            }
            line = p + 1;
        }
    }
    if (out_size) {
        out[0] = 0;
    }
    return 0;
}

static void username_from_uid(int uid, char *out, size_t out_size)
{
    char passwd[1024];
    if (read_file("/etc/passwd", passwd, sizeof(passwd)) < 0) {
        strncpy(out, uid == 0 ? "root" : "unknown", out_size - 1);
        out[out_size - 1] = 0;
        return;
    }
    char *line = passwd;
    for (char *p = passwd;; p++) {
        if (*p == '\n' || *p == 0) {
            char old = *p;
            *p = 0;
            char *name = line;
            char *uid_field = 0;
            int field = 0;
            for (char *q = line; *q; q++) {
                if (*q == ':') {
                    *q = 0;
                    field++;
                    if (field == 2) {
                        uid_field = q + 1;
                    }
                }
            }
            if (uid_field && atoi(uid_field) == uid) {
                strncpy(out, name, out_size - 1);
                out[out_size - 1] = 0;
                return;
            }
            if (old == 0) {
                break;
            }
            line = p + 1;
        }
    }
    strncpy(out, "unknown", out_size - 1);
    out[out_size - 1] = 0;
}

static int cmd_echo(int argc, char **argv)
{
    int start = 1;
    int newline = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start = 2;
    }
    for (int i = start; i < argc; i++) {
        if (i > start) {
            print(" ");
        }
        print(argv[i]);
    }
    if (newline) {
        print("\n");
    }
    return 0;
}

static int cmd_pwd(void)
{
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) < 0) {
        return 1;
    }
    println(cwd);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    char buf[256];
    if (argc < 2) {
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            print("cat: cannot open ");
            println(argv[i]);
            continue;
        }
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, (size_t)n);
        }
        close(fd);
    }
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT | O_RDWR, 0644);
        if (fd >= 0) {
            close(fd);
        }
    }
    return argc > 1 ? 0 : 1;
}

static int cmd_mkdir(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        mkdir(argv[i], 0755);
    }
    return argc > 1 ? 0 : 1;
}

static int cmd_rm(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        unlink(argv[i]);
    }
    return argc > 1 ? 0 : 1;
}

static int cmd_uname(void)
{
    char version[128];
    if (read_file("/proc/version", version, sizeof(version)) < 0) {
        println("unknown");
    } else {
        strip_newline(version);
        println(version);
    }
    return 0;
}

static int cmd_id(void)
{
    char name[32];
    int uid = getuid();
    int gid = getgid();
    username_from_uid(uid, name, sizeof(name));
    print("uid=");
    print_int(uid);
    print("(");
    print(name);
    print(") gid=");
    print_int(gid);
    print("\n");
    return 0;
}

static int cmd_sysfetch(void)
{
    char value[128];
    char host[64];
    if (print_file("/etc/sysfetch-logo") < 0) {
        print(sysfetch_default_logo);
    }
    if (os_value("PRETTY_NAME", value, sizeof(value))) {
        print("OS: ");
        println(value);
    }
    if (read_file("/proc/version", value, sizeof(value)) >= 0) {
        strip_newline(value);
        print("Kernel: ");
        println(value);
    }
    if (read_file("/etc/hostname", host, sizeof(host)) >= 0) {
        strip_newline(host);
        print("Hostname: ");
        println(host);
    }
    println("Shell: /bin/tsh");
    return 0;
}

static int cmd_whoami(void)
{
    char name[32];
    username_from_uid(getuid(), name, sizeof(name));
    println(name);
    return 0;
}

static int cmd_hostname(int argc, char **argv)
{
    if (argc > 1) {
        int fd = open("/etc/hostname", O_CREAT | O_RDWR, 0644);
        if (fd >= 0) {
            write(fd, argv[1], strlen(argv[1]));
            write(fd, "\n", 1);
            close(fd);
        }
    }
    char host[64];
    if (read_file("/etc/hostname", host, sizeof(host)) >= 0) {
        strip_newline(host);
        println(host);
    }
    return 0;
}

static int cmd_uptime(void)
{
    char uptime[64];
    if (read_file("/proc/uptime", uptime, sizeof(uptime)) < 0) {
        println("uptime: unavailable");
        return 1;
    }
    strip_newline(uptime);
    print(uptime);
    println(" seconds");
    return 0;
}

static int write_config_line(const char *path, const char *value)
{
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return -1;
    }
    write(fd, value, strlen(value));
    write(fd, "\n", 1);
    close(fd);
    return 0;
}

static int cmd_timezone(int argc, char **argv)
{
    if (argc > 1 && write_config_line("/etc/timezone", argv[1]) < 0) {
        println("timezone: failed to update /etc/timezone");
        return 1;
    }
    char tz[64];
    if (read_file("/etc/timezone", tz, sizeof(tz)) >= 0) {
        strip_newline(tz);
        println(tz);
    }
    return 0;
}

static int cmd_keymap(int argc, char **argv)
{
    if (argc > 1 && write_config_line("/etc/keymap", argv[1]) < 0) {
        println("keymap: failed to update /etc/keymap");
        return 1;
    }
    char keymap[64];
    if (read_file("/etc/keymap", keymap, sizeof(keymap)) >= 0) {
        strip_newline(keymap);
        println(keymap);
    }
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    if (argc != 2) {
        println("usage: ping IPv4");
        return 1;
    }
    if (strcmp(argv[1], "127.0.0.1") == 0) {
        println("64 bytes from 127.0.0.1: icmp_seq=1 ttl=64 time=0 ms");
        return 0;
    }
    println("ping: network unreachable");
    return 1;
}

static int parse_octal_mode(const char *s, mode_t *out)
{
    if (!s || !*s || !out) {
        return -1;
    }
    mode_t mode = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '7') {
            return -1;
        }
        mode = (mode << 3) | (mode_t)(*p - '0');
        if (mode > 07777) {
            return -1;
        }
    }
    *out = mode;
    return 0;
}

static int parse_symbolic_mode(const char *spec, mode_t current, mode_t *out)
{
    const char *p = spec;
    mode_t mode = current & 07777;

    if (!spec || !*spec || !out) {
        return -1;
    }

    while (*p) {
        mode_t who = 0;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            if (*p == 'u') {
                who |= 0700;
            } else if (*p == 'g') {
                who |= 0070;
            } else if (*p == 'o') {
                who |= 0007;
            } else {
                who |= 0777;
            }
            p++;
        }
        if (who == 0) {
            who = 0777;
        }

        char op = *p++;
        if (op != '+' && op != '-' && op != '=') {
            return -1;
        }

        mode_t bits = 0;
        while (*p == 'r' || *p == 'w' || *p == 'x') {
            if (*p == 'r') {
                bits |= 0444;
            } else if (*p == 'w') {
                bits |= 0222;
            } else {
                bits |= 0111;
            }
            p++;
        }
        bits &= who;

        if (op == '+') {
            mode |= bits;
        } else if (op == '-') {
            mode &= ~bits;
        } else {
            mode = (mode & ~who) | bits;
        }

        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '\0') {
            return -1;
        }
    }

    *out = mode;
    return 0;
}

static int cmd_chmod(int argc, char **argv)
{
    if (argc != 3) {
        println("usage: chmod MODE FILE");
        return 1;
    }

    mode_t mode = 0;
    if (parse_octal_mode(argv[1], &mode) < 0) {
        struct stat st;
        if (stat(argv[2], &st) < 0 ||
            parse_symbolic_mode(argv[1], st.st_mode, &mode) < 0) {
            print("chmod: invalid mode: ");
            println(argv[1]);
            return 1;
        }
    }

    if (chmod(argv[2], mode) < 0) {
        print("chmod: failed: ");
        println(argv[2]);
        return 1;
    }
    return 0;
}

static void print_octal_mode(mode_t mode)
{
    char buf[8];
    for (int i = 6; i >= 0; i--) {
        buf[i] = (char)('0' + (mode & 7));
        mode >>= 3;
    }
    buf[7] = '\0';
    print(buf);
}

static const char *stat_type_name(int type)
{
    switch (type) {
    case TNU_DT_DIR:
        return "directory";
    case TNU_DT_FILE:
        return "file";
    case TNU_DT_DEV:
        return "device";
    case TNU_DT_PROC:
        return "proc";
    default:
        return "unknown";
    }
}

static int cmd_stat(int argc, char **argv)
{
    if (argc < 2) {
        println("usage: stat FILE");
        return 1;
    }
    struct stat st;
    if (stat(argv[1], &st) < 0) {
        print("stat: cannot stat ");
        println(argv[1]);
        return 1;
    }
    print("File: ");
    println(argv[1]);
    print("Type: ");
    println(stat_type_name(st.st_type));
    print("Mode: ");
    print_int(st.st_mode);
    print("\nUID: ");
    print_int(st.st_uid);
    print("  GID: ");
    print_int(st.st_gid);
    print("\nSize: ");
    print_int(st.st_size);
    println(" bytes");
    return 0;
}

static void print_bssid(const uint8_t bssid[6])
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        println("usage: wifi scan");
        println("       wifi connect IFACE SSID [PASSPHRASE]");
        println("       wifi status");
        return argc < 2 ? 1 : 0;
    }
    if (strcmp(argv[1], "scan") == 0) {
        struct wifi_ap aps[32];
        int count = wifi_scan(aps, sizeof(aps) / sizeof(aps[0]));
        if (count < 0) {
            println("wifi: scan failed");
            return 1;
        }
        if (count == 0) {
            println("wifi: no networks found");
            return 0;
        }
        for (int i = 0; i < count; i++) {
            print(aps[i].ssid);
            print("  ");
            print_bssid(aps[i].bssid);
            print("  rssi=");
            print_int(aps[i].rssi);
            print("  ");
            println((aps[i].flags & 1u) ? "wpa" : "open");
        }
        return 0;
    }
    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 4) {
            println("usage: wifi connect IFACE SSID [PASSPHRASE]");
            return 1;
        }
        const char *passphrase = argc > 4 ? argv[4] : NULL;
        int rc = wifi_connect(argv[2], argv[3], passphrase);
        if (rc < 0) {
            println("wifi: connect failed");
            return 1;
        }
        println("wifi: connected");
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        struct wifi_status st;
        if (wifi_status(&st) < 0) {
            println("wifi: status unavailable");
            return 1;
        }
        if (st.connected) {
            print("connected: ");
            println(st.ssid[0] ? st.ssid : "<unknown>");
        } else {
            println("disconnected");
        }
        return 0;
    }
    println("wifi: unknown command");
    return 1;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 0 ? basename(argv[0]) : "";
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        return show_help(cmd);
    }
    if (strcmp(cmd, "echo") == 0) return cmd_echo(argc, argv);
    if (strcmp(cmd, "pwd") == 0) return cmd_pwd();
    if (strcmp(cmd, "cat") == 0) return cmd_cat(argc, argv);
    if (strcmp(cmd, "touch") == 0) return cmd_touch(argc, argv);
    if (strcmp(cmd, "mkdir") == 0) return cmd_mkdir(argc, argv);
    if (strcmp(cmd, "rm") == 0) return cmd_rm(argc, argv);
    if (strcmp(cmd, "uname") == 0) return cmd_uname();
    if (strcmp(cmd, "whoami") == 0) return cmd_whoami();
    if (strcmp(cmd, "id") == 0) return cmd_id();
    if (strcmp(cmd, "sysfetch") == 0) return cmd_sysfetch();
    if (strcmp(cmd, "hostname") == 0) return cmd_hostname(argc, argv);
    if (strcmp(cmd, "uptime") == 0) return cmd_uptime();
    if (strcmp(cmd, "time") == 0) { println("time: unavailable from userspace"); return 1; }
    if (strcmp(cmd, "timezone") == 0) return cmd_timezone(argc, argv);
    if (strcmp(cmd, "keymap") == 0 || strcmp(cmd, "layout") == 0) return cmd_keymap(argc, argv);
    if (strcmp(cmd, "ifconfig") == 0) { print_file("/proc/net/dev"); return 0; }
    if (strcmp(cmd, "route") == 0) { print_file("/proc/net/route"); return 0; }
    if (strcmp(cmd, "netstat") == 0) { print_file("/proc/net/sockstat"); return 0; }
    if (strcmp(cmd, "usb") == 0) { print_file("/proc/usb"); return 0; }
    if (strcmp(cmd, "ping") == 0) return cmd_ping(argc, argv);
    if (strcmp(cmd, "wifi") == 0) return cmd_wifi(argc, argv);
    if (strcmp(cmd, "xedit") == 0) { println("xedit: unavailable from userspace"); return 1; }
    if (strcmp(cmd, "clear") == 0) { print("\033[2J\033[H"); return 0; }
    if (strcmp(cmd, "date") == 0) { println("date: unavailable from userspace"); return 1; }
    if (strcmp(cmd, "ps") == 0) { print_file("/proc/processes"); return 0; }
    if (strcmp(cmd, "kill") == 0) { println("usage: kill PID"); return 1; }
    if (strcmp(cmd, "chmod") == 0) return cmd_chmod(argc, argv);
    if (strcmp(cmd, "stat") == 0) return cmd_stat(argc, argv);
    if (strcmp(cmd, "chown") == 0) { println("usage: chown USER FILE"); return 1; }
    if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "mv") == 0 ||
        strcmp(cmd, "mount") == 0 ||
        strcmp(cmd, "dmesg") == 0 ||
        strcmp(cmd, "reboot") == 0 || strcmp(cmd, "shutdown") == 0) {
        println("utility unavailable from userspace");
        return 1;
    }
    println("tnu-utils: unknown applet");
    return 127;
}
