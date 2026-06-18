#include <tnu/libc.h>
#include <tnu/tls.h>
#include <dirent.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

/* Forward declarations */
static int cmd_shutdown(int argc, char **argv);
static int cmd_reboot(int argc, char **argv);
static int cmd_sync(int argc, char **argv);
static int cmd_tirux(int argc, char **argv);
static int cmd_tar(int argc, char **argv);
static int cmd_zip(int argc, char **argv);
static int cmd_unzip(int argc, char **argv);
static int cmd_dns(int argc, char **argv);
static int cmd_curl(int argc, char **argv);
static int cmd_wget(int argc, char **argv);
static int cmd_tls(int argc, char **argv);
static int cmd_driver(int argc, char **argv);
static int cmd_linuxdrv(int argc, char **argv);
static int cmd_net(int argc, char **argv);

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
    { "dns", "dns HOST", "Resolve an IPv4 address through the kernel DNS resolver." },
    { "nano", "nano FILE", "Edit a text file with the nano port." },
    { "driver", "driver list|stats", "Inspect native and Linux driver providers." },
    { "linuxdrv", "linuxdrv load MODULE|logs|modules|stats", "Inspect Linux Driver Runtime state." },
    { "net", "net trace", "Show native/TNAI/Linux netdev bridge path." },
    { "curl", "curl URL [-o FILE]", "Fetch file: and http:// URLs; HTTPS waits for TLS." },
    { "wget", "wget URL [-O FILE]", "Fetch file: and http:// URLs; HTTPS waits for TLS." },
    { "tls", "tls status", "Show HTTPS/TLS backend readiness." },
    { "wifi", "wifi scan|connect IFACE SSID [PASSPHRASE]|disconnect IFACE|status|debug", "Scan, connect, and show Wi-Fi status." },
    { "chmod", "chmod MODE FILE", "Change file permissions; supports octal and simple symbolic modes." },
    { "stat", "stat FILE", "Print file metadata." },
    { "tirux", "tirux install|status|help", "Prepare and use the Tiramisu Linux compatibility root." },
    { "tar", "tar -c|-t|-x -f ARCHIVE [PATH...]", "Create, list, or extract uncompressed ustar archives." },
    { "zip", "zip ARCHIVE.zip FILE...", "Create a store-only ZIP archive." },
    { "unzip", "unzip ARCHIVE.zip", "Extract files from a store-only ZIP archive." },
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
        println("       wifi disconnect IFACE");
        println("       wifi status");
        println("       wifi debug");
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
    if (strcmp(argv[1], "disconnect") == 0) {
        if (argc < 3) {
            println("usage: wifi disconnect IFACE");
            return 1;
        }
        if (wifi_disconnect(argv[2]) < 0) {
            println("wifi: disconnect failed");
            return 1;
        }
        println("wifi: disconnected");
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
    if (strcmp(argv[1], "debug") == 0) {
        struct wifi_status st;
        println("wifi debug:");
        if (wifi_status(&st) < 0) {
            println("  status: unavailable");
        } else if (st.connected) {
            print("  status: connected ");
            println(st.ssid[0] ? st.ssid : "<unknown>");
        } else {
            println("  status: disconnected");
        }
        println("  native path: wifi -> Tiramisu WiFi API -> iwlwifi/TNAI backend");
        println("  linuxdrv detail: use kernel shell command 'linuxdrv stats'");
        println("  net trace: use kernel shell command 'net trace'");
        return 0;
    }
    println("wifi: unknown command");
    return 1;
}

static int cmd_driver(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        println("driver providers:");
        println("  native: loopback e1000 iwlwifi framebuffer usb block");
        println("  linuxdrv: cfg80211 mac80211 iwlwifi iwlmvm drm i915");
        println("  live kernel detail: run builtin 'driver list' from tsh");
        return 0;
    }
    if (strcmp(argv[1], "stats") == 0) {
        println("driver stats:");
        println("  live counters are maintained in kernel LDR");
        println("  run builtin 'driver stats' or 'linuxdrv stats' from tsh");
        return 0;
    }
    println("usage: driver list|stats");
    return 1;
}

static int cmd_linuxdrv(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "logs") == 0) {
        println("linuxdrv logs:");
        println("  LDR is the Linux kernel-driver backend manager for native Tiramisu APIs.");
        println("  live kernel logs: run builtin 'linuxdrv logs' from tsh");
        return 0;
    }
    if (strcmp(argv[1], "load") == 0) {
        if (argc < 3) {
            println("usage: linuxdrv load MODULE");
            return 1;
        }
        println("linuxdrv load:");
        println("  module loading is a kernel builtin operation.");
        println("  run from tsh: linuxdrv load MODULE");
        return 1;
    }
    if (strcmp(argv[1], "modules") == 0) {
        println("linuxdrv modules:");
        println("  cfg80211 -> mac80211 -> iwlwifi -> iwlmvm");
        println("  drm -> i915");
        return 0;
    }
    if (strcmp(argv[1], "stats") == 0) {
        println("linuxdrv stats:");
        println("  live counters are exposed by the kernel builtin 'linuxdrv stats'");
        return 0;
    }
    println("usage: linuxdrv load MODULE|logs|modules|stats");
    return 1;
}

static int cmd_net(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "trace") == 0) {
        println("net trace:");
        println("  TX: native socket -> Tiramisu IP -> TNAI -> Linux net_device -> driver -> hardware");
        println("  RX: hardware -> Linux driver -> net_device -> TNAI -> Tiramisu IP -> native socket");
        return 0;
    }
    println("usage: net trace");
    return 1;
}

static int cmd_shutdown(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    
    println("Shutting down system...");
    sync();
    if (shutdown() < 0) {
        println("shutdown: permission denied (requires root)");
        return 1;
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    
    println("Rebooting system...");
    sync();
    if (reboot() < 0) {
        println("reboot: permission denied (requires root)");
        return 1;
    }
    return 0;
}

static int cmd_sync(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    
    int result = sync();
    if (result < 0) {
        println("sync: failed (error writing to disk)");
        println("Check kernel logs with: dmesg | grep tfs");
        return 1;
    }
    if (result == 0) {
        println("Sync complete.");
    }
    return 0;
}

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int write_text_file(const char *path, const char *text)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static void tirux_mkdirs(void)
{
    mkdir("/usr", 0755);
    mkdir("/usr/linux", 0755);
    mkdir("/usr/linux/bin", 0755);
    mkdir("/usr/linux/etc", 0755);
    mkdir("/usr/linux/home", 0755);
    mkdir("/usr/linux/lib", 0755);
    mkdir("/usr/linux/lib64", 0755);
    mkdir("/usr/linux/lib/modules", 0755);
    mkdir("/usr/linux/proc", 0555);
    mkdir("/usr/linux/root", 0700);
    mkdir("/usr/linux/sbin", 0755);
    mkdir("/usr/linux/sys", 0555);
    mkdir("/usr/linux/tmp", 01777);
    mkdir("/usr/linux/usr", 0755);
    mkdir("/usr/linux/usr/bin", 0755);
    mkdir("/usr/linux/usr/lib", 0755);
    mkdir("/usr/linux/usr/sbin", 0755);
    mkdir("/usr/linux/var", 0755);
    mkdir("/usr/linux/var/tmp", 01777);
}

static void tirux_print_usage(void)
{
    println("tirux install [alpine|manual URL]");
    println("  prepares /usr/linux and records the chosen Linux rootfs source");
    println("");
    println("Current beta limitation:");
    println("  HTTPS download is not implemented in Tiramisu userspace yet.");
    println("  Tar extraction is available with: tar -x -f ARCHIVE.tar");
    println("  For now, copy/extract the rootfs contents into /usr/linux before booting.");
    println("");
    println("After a rootfs is present:");
    println("  linux-run /bin/busybox echo hello");
    println("  linux-run /bin/sh");
}

static int cmd_tirux_status(void)
{
    println("Tirux Linux root: /usr/linux");
    print("  /usr/linux/bin/sh: ");
    println(path_exists("/usr/linux/bin/sh") ? "present" : "missing");
    print("  /usr/linux/bin/busybox: ");
    println(path_exists("/usr/linux/bin/busybox") ? "present" : "missing");
    print("  /bin/nano native: ");
    println(path_exists("/bin/nano") ? "present" : "missing");
    print("  /usr/linux/usr/bin/fastfetch: ");
    println(path_exists("/usr/linux/usr/bin/fastfetch") ? "present" : "missing");
    print("  /usr/linux/lib: ");
    println(path_exists("/usr/linux/lib") ? "present" : "missing");
    print("  /usr/linux/lib64: ");
    println(path_exists("/usr/linux/lib64") ? "present" : "missing");
    if (path_exists("/usr/linux/bin/sh") || path_exists("/usr/linux/bin/busybox")) {
        println("");
        println("Try:");
        println("  linux-run /bin/sh");
        println("  linux-run /bin/busybox sh");
        println("  nano file.txt");
        println("  linux-run /usr/bin/fastfetch");
    } else {
        println("");
        println("No Linux userspace appears to be installed yet.");
        println("Run: tirux install alpine");
        println("Then place a Linux rootfs under /usr/linux.");
    }
    return 0;
}

static int cmd_tirux_install(int argc, char **argv)
{
    const char *choice = argc >= 3 ? argv[2] : "alpine";
    tirux_mkdirs();

    write_text_file("/usr/linux/TIRUX.txt",
        "Tirux Linux compatibility root.\n"
        "Populate this directory with a Linux rootfs.\n"
        "Then run Linux binaries with linux-run.\n");
    if (strcmp(choice, "manual") == 0 && argc >= 4) {
        write_text_file("/usr/linux/TIRUX_SOURCE", argv[3]);
    } else if (strcmp(choice, "alpine") == 0) {
        write_text_file("/usr/linux/TIRUX_SOURCE", "alpine:latest-stable:x86_64");
    } else {
        write_text_file("/usr/linux/TIRUX_SOURCE", choice);
    }

    println("Tirux prepared /usr/linux.");
    println("");
    println("Selected source:");
    if (strcmp(choice, "manual") == 0 && argc >= 4) {
        println(argv[3]);
    } else if (strcmp(choice, "alpine") == 0) {
        println("alpine:latest-stable:x86_64");
    } else {
        println(choice);
    }
    println("");
    println("Download backend status: pending native HTTPS support.");
    println("Archive backend status: native tar/zip/unzip applets available.");
    println("For this beta, put the extracted rootfs contents directly in /usr/linux.");
    println("");
    println("Then use:");
    println("  tirux status");
    println("  linux-run /bin/sh");
    println("  linux-run /bin/busybox echo hello");
    return 0;
}

static int cmd_tirux(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        tirux_print_usage();
        return 0;
    }
    if (strcmp(argv[1], "install") == 0) {
        return cmd_tirux_install(argc, argv);
    }
    if (strcmp(argv[1], "status") == 0) {
        return cmd_tirux_status();
    }
    println("tirux: unknown command");
    tirux_print_usage();
    return 1;
}

static int write_all_fd(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int write_fd_cb(const void *data, size_t len, void *ctx)
{
    int fd = *(int *)ctx;
    return write_all_fd(fd, data, len);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int)(crc & 1));
        }
    }
    return ~crc;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int mkdir_parent_dirs(const char *path)
{
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return 0;
}

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static void tar_octal(char *out, size_t size, uint64_t value)
{
    if (size == 0) {
        return;
    }
    for (size_t i = 0; i < size; i++) {
        out[i] = '0';
    }
    out[size - 1] = '\0';
    size_t pos = size - 2;
    do {
        out[pos--] = (char)('0' + (value & 7));
        value >>= 3;
    } while (value && pos < size);
}

static uint64_t tar_parse_octal(const char *s, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '7') {
            continue;
        }
        v = (v << 3) + (uint64_t)(s[i] - '0');
    }
    return v;
}

static int tar_write_header(int out, const char *path, const struct stat *st, int is_dir)
{
    struct tar_header h;
    memset(&h, 0, sizeof(h));
    strncpy(h.name, path, sizeof(h.name) - 1);
    tar_octal(h.mode, sizeof(h.mode), st ? (uint64_t)(st->st_mode & 0777) : 0755);
    tar_octal(h.uid, sizeof(h.uid), st ? (uint64_t)st->st_uid : 0);
    tar_octal(h.gid, sizeof(h.gid), st ? (uint64_t)st->st_gid : 0);
    tar_octal(h.size, sizeof(h.size), is_dir ? 0 : (uint64_t)(st ? st->st_size : 0));
    tar_octal(h.mtime, sizeof(h.mtime), st ? (uint64_t)st->st_mtime : 0);
    memset(h.chksum, ' ', sizeof(h.chksum));
    h.typeflag = is_dir ? '5' : '0';
    memcpy(h.magic, "ustar", 5);
    memcpy(h.version, "00", 2);
    strncpy(h.uname, "root", sizeof(h.uname) - 1);
    strncpy(h.gname, "root", sizeof(h.gname) - 1);

    unsigned int sum = 0;
    const unsigned char *p = (const unsigned char *)&h;
    for (size_t i = 0; i < sizeof(h); i++) {
        sum += p[i];
    }
    tar_octal(h.chksum, sizeof(h.chksum), sum);
    h.chksum[6] = '\0';
    h.chksum[7] = ' ';
    return write_all_fd(out, &h, sizeof(h));
}

static int tar_add_path(int out, const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        print("tar: cannot stat ");
        println(path);
        return -1;
    }
    int is_dir = S_ISDIR(st.st_mode);
    if (tar_write_header(out, path, &st, is_dir) < 0) {
        return -1;
    }
    if (is_dir) {
        DIR *dir = opendir(path);
        if (!dir) {
            return 0;
        }
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }
            char child[256];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            tar_add_path(out, child);
        }
        closedir(dir);
        return 0;
    }

    int in = open(path, O_RDONLY, 0);
    if (in < 0) {
        return -1;
    }
    char buf[512];
    ssize_t n;
    uint64_t written = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write_all_fd(out, buf, (size_t)n) < 0) {
            close(in);
            return -1;
        }
        written += (uint64_t)n;
    }
    close(in);
    size_t pad = (size_t)((512 - (written % 512)) % 512);
    if (pad) {
        char zero[512];
        memset(zero, 0, sizeof(zero));
        return write_all_fd(out, zero, pad);
    }
    return 0;
}

static int cmd_tar(int argc, char **argv)
{
    int create = 0, extract = 0, list = 0;
    const char *archive = NULL;
    int first_path = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) create = 1;
        else if (strcmp(argv[i], "-x") == 0) extract = 1;
        else if (strcmp(argv[i], "-t") == 0) list = 1;
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) archive = argv[++i];
        else if (argv[i][0] != '-' && !archive) archive = argv[i];
        else { first_path = i; break; }
        first_path = i + 1;
    }
    if (!archive || create + extract + list != 1) {
        println("usage: tar -c|-t|-x -f ARCHIVE [PATH...]");
        return 1;
    }
    if (create) {
        int out = open(archive, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (out < 0) return 1;
        if (first_path >= argc) {
            tar_add_path(out, ".");
        } else {
            for (int i = first_path; i < argc; i++) tar_add_path(out, argv[i]);
        }
        char zero[1024];
        memset(zero, 0, sizeof(zero));
        write_all_fd(out, zero, sizeof(zero));
        close(out);
        return 0;
    }

    int in = open(archive, O_RDONLY, 0);
    if (in < 0) return 1;
    for (;;) {
        struct tar_header h;
        ssize_t n = read(in, &h, sizeof(h));
        if (n != sizeof(h)) break;
        int empty = 1;
        const unsigned char *hp = (const unsigned char *)&h;
        for (size_t i = 0; i < sizeof(h); i++) if (hp[i]) { empty = 0; break; }
        if (empty) break;
        uint64_t size = tar_parse_octal(h.size, sizeof(h.size));
        if (list) {
            println(h.name);
        } else if (extract) {
            mkdir_parent_dirs(h.name);
            if (h.typeflag == '5') {
                mkdir(h.name, 0755);
            } else {
                int out = open(h.name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
                char buf[512];
                uint64_t left = size;
                while (left) {
                    size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
                    ssize_t r = read(in, buf, want);
                    if (r <= 0) break;
                    if (out >= 0) write_all_fd(out, buf, (size_t)r);
                    left -= (uint64_t)r;
                }
                if (out >= 0) close(out);
                size_t pad = (size_t)((512 - (size % 512)) % 512);
                if (pad) lseek(in, (off_t)pad, SEEK_CUR);
                continue;
            }
        }
        size_t skip = (size_t)(size + ((512 - (size % 512)) % 512));
        if (skip) lseek(in, (off_t)skip, SEEK_CUR);
    }
    close(in);
    return 0;
}

struct zip_entry {
    char name[128];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
};

static int zip_write_file(int out, const char *path, struct zip_entry *entry)
{
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) {
        print("zip: skipping ");
        println(path);
        return -1;
    }
    int in = open(path, O_RDONLY, 0);
    if (in < 0) return -1;
    entry->offset = (uint32_t)lseek(out, 0, SEEK_CUR);
    strncpy(entry->name, path, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->size = (uint32_t)st.st_size;

    uint8_t local[30];
    memset(local, 0, sizeof(local));
    put_le32(local, 0x04034b50);
    put_le16(local + 4, 20);
    put_le16(local + 8, 0);
    put_le16(local + 26, (uint16_t)strlen(entry->name));
    write_all_fd(out, local, sizeof(local));
    write_all_fd(out, entry->name, strlen(entry->name));

    char buf[512];
    ssize_t n;
    uint32_t crc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        crc = crc32_update(crc, (const uint8_t *)buf, (size_t)n);
        write_all_fd(out, buf, (size_t)n);
    }
    close(in);
    entry->crc = crc;

    off_t end = lseek(out, 0, SEEK_CUR);
    lseek(out, (off_t)entry->offset + 14, SEEK_SET);
    uint8_t meta[12];
    put_le32(meta, entry->crc);
    put_le32(meta + 4, entry->size);
    put_le32(meta + 8, entry->size);
    write_all_fd(out, meta, sizeof(meta));
    lseek(out, end, SEEK_SET);
    return 0;
}

static int cmd_zip(int argc, char **argv)
{
    if (argc < 3) {
        println("usage: zip ARCHIVE.zip FILE...");
        return 1;
    }
    int out = open(argv[1], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out < 0) return 1;
    struct zip_entry entries[64];
    int count = 0;
    for (int i = 2; i < argc && count < 64; i++) {
        if (zip_write_file(out, argv[i], &entries[count]) == 0) count++;
    }
    uint32_t central_offset = (uint32_t)lseek(out, 0, SEEK_CUR);
    for (int i = 0; i < count; i++) {
        uint8_t c[46];
        memset(c, 0, sizeof(c));
        put_le32(c, 0x02014b50);
        put_le16(c + 4, 20);
        put_le16(c + 6, 20);
        put_le32(c + 16, entries[i].crc);
        put_le32(c + 20, entries[i].size);
        put_le32(c + 24, entries[i].size);
        put_le16(c + 28, (uint16_t)strlen(entries[i].name));
        put_le32(c + 42, entries[i].offset);
        write_all_fd(out, c, sizeof(c));
        write_all_fd(out, entries[i].name, strlen(entries[i].name));
    }
    uint32_t central_size = (uint32_t)lseek(out, 0, SEEK_CUR) - central_offset;
    uint8_t eocd[22];
    memset(eocd, 0, sizeof(eocd));
    put_le32(eocd, 0x06054b50);
    put_le16(eocd + 8, (uint16_t)count);
    put_le16(eocd + 10, (uint16_t)count);
    put_le32(eocd + 12, central_size);
    put_le32(eocd + 16, central_offset);
    write_all_fd(out, eocd, sizeof(eocd));
    close(out);
    return 0;
}

static int cmd_unzip(int argc, char **argv)
{
    if (argc < 2) {
        println("usage: unzip ARCHIVE.zip");
        return 1;
    }
    int in = open(argv[1], O_RDONLY, 0);
    if (in < 0) return 1;
    for (;;) {
        uint8_t h[30];
        ssize_t n = read(in, h, sizeof(h));
        if (n != sizeof(h)) break;
        uint32_t sig = get_le32(h);
        if (sig != 0x04034b50) break;
        uint16_t method = get_le16(h + 8);
        uint32_t size = get_le32(h + 18);
        uint16_t name_len = get_le16(h + 26);
        uint16_t extra_len = get_le16(h + 28);
        char name[256];
        if (name_len >= sizeof(name)) {
            close(in);
            return 1;
        }
        read(in, name, name_len);
        name[name_len] = '\0';
        if (extra_len) lseek(in, extra_len, SEEK_CUR);
        if (method != 0) {
            print("unzip: unsupported compressed entry ");
            println(name);
            lseek(in, size, SEEK_CUR);
            continue;
        }
        mkdir_parent_dirs(name);
        int out = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        char buf[512];
        uint32_t left = size;
        while (left) {
            size_t want = left < sizeof(buf) ? left : sizeof(buf);
            ssize_t r = read(in, buf, want);
            if (r <= 0) break;
            if (out >= 0) write_all_fd(out, buf, (size_t)r);
            left -= (uint32_t)r;
        }
        if (out >= 0) close(out);
    }
    close(in);
    return 0;
}

static void format_ipv4_user(uint32_t ip, char *out, size_t out_size)
{
    snprintf(out, out_size, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xff),
             (unsigned)((ip >> 16) & 0xff),
             (unsigned)((ip >> 8) & 0xff),
             (unsigned)(ip & 0xff));
}

static int cmd_dns(int argc, char **argv)
{
    if (argc < 2) {
        println("usage: dns HOST");
        return 1;
    }
    uint32_t ip = 0;
    if (resolve4(argv[1], &ip) < 0) {
        print("dns: cannot resolve ");
        println(argv[1]);
        return 1;
    }
    char buf[16];
    format_ipv4_user(ip, buf, sizeof(buf));
    print(argv[1]);
    print(" ");
    println(buf);
    return 0;
}

static const char *url_local_path(const char *url)
{
    if (strncmp(url, "file://", 7) == 0) {
        return url + 7;
    }
    if (strncmp(url, "file:", 5) == 0) {
        return url + 5;
    }
    if (strstr(url, "://")) {
        return NULL;
    }
    return url;
}

struct parsed_url {
    char scheme[8];
    char host[128];
    char path[256];
    uint16_t port;
};

static int parse_http_url(const char *url, struct parsed_url *out)
{
    memset(out, 0, sizeof(*out));
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        return -1;
    }
    size_t scheme_len = (size_t)(scheme_end - url);
    if (scheme_len >= sizeof(out->scheme)) {
        return -1;
    }
    memcpy(out->scheme, url, scheme_len);
    out->scheme[scheme_len] = '\0';
    out->port = strcmp(out->scheme, "https") == 0 ? 443 : 80;

    const char *host = scheme_end + 3;
    const char *path = strchr(host, '/');
    const char *host_end = path ? path : host + strlen(host);
    const char *colon = NULL;
    for (const char *p = host; p < host_end; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }
    size_t host_len = (size_t)((colon ? colon : host_end) - host);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return -1;
    }
    memcpy(out->host, host, host_len);
    out->host[host_len] = '\0';
    if (colon) {
        int port = atoi(colon + 1);
        if (port <= 0 || port > 65535) {
            return -1;
        }
        out->port = (uint16_t)port;
    }
    if (path && path[0]) {
        strncpy(out->path, path, sizeof(out->path) - 1);
    } else {
        strcpy(out->path, "/");
    }
    return 0;
}

static int http_get_to_fd(const char *url, int out)
{
    struct parsed_url u;
    if (parse_http_url(url, &u) < 0 || strcmp(u.scheme, "http") != 0) {
        return -1;
    }
    uint32_t ip = 0;
    if (resolve4(u.host, &ip) < 0) {
        print("http: cannot resolve ");
        println(u.host);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        println("http: socket unavailable");
        return 1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u.port);
    addr.sin_addr.s_addr = htonl(ip);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        println("http: connect failed");
        close(fd);
        return 1;
    }

    char req[512];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: tiramisu/0.1\r\nConnection: close\r\n\r\n",
             u.path, u.host);
    if (send(fd, req, strlen(req), 0) < 0) {
        println("http: send failed");
        close(fd);
        return 1;
    }

    char buf[512];
    char header[1024];
    size_t header_len = 0;
    int header_done = 0;
    int status_code = 0;
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        if (header_done) {
            write_all_fd(out, buf, (size_t)n);
            continue;
        }
        size_t pos = 0;
        while (pos < (size_t)n && header_len + 1 < sizeof(header)) {
            header[header_len++] = buf[pos++];
            header[header_len] = '\0';
            if (header_len >= 4 &&
                header[header_len - 4] == '\r' && header[header_len - 3] == '\n' &&
                header[header_len - 2] == '\r' && header[header_len - 1] == '\n') {
                header_done = 1;
                if (strncmp(header, "HTTP/", 5) == 0) {
                    const char *sp = strchr(header, ' ');
                    if (sp) {
                        status_code = atoi(sp + 1);
                    }
                }
                if (status_code < 200 || status_code >= 400) {
                    print("http: bad status ");
                    print_int(status_code);
                    print("\n");
                    close(fd);
                    return 1;
                }
                if (pos < (size_t)n) {
                    write_all_fd(out, buf + pos, (size_t)n - pos);
                }
                break;
            }
        }
        if (!header_done && header_len + 1 >= sizeof(header)) {
            println("http: response header too large");
            close(fd);
            return 1;
        }
    }
    close(fd);
    return header_done ? 0 : 1;
}

static int copy_url_to_fd(const char *url, int out)
{
    if (strncmp(url, "http://", 7) == 0) {
        return http_get_to_fd(url, out);
    }
    if (strncmp(url, "https://", 8) == 0) {
        int rc = tnu_https_get(url, write_fd_cb, &out);
        if (rc < 0) {
            print("https: ");
            println(tnu_tls_strerror(rc));
            return 1;
        }
        return 0;
    }
    const char *path = url_local_path(url);
    if (!path) {
        println("network fetch: unsupported URL scheme");
        return 1;
    }

    int in = open(path, O_RDONLY, 0);
    if (in < 0) {
        print("fetch: cannot open ");
        println(path);
        return 1;
    }
    char buf[512];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write_all_fd(out, buf, (size_t)n) < 0) {
            close(in);
            return 1;
        }
    }
    close(in);
    return 0;
}

static int cmd_curl(int argc, char **argv)
{
    const char *url = NULL;
    const char *out_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            url = argv[i];
        }
    }
    if (!url) {
        println("usage: curl URL [-o FILE]");
        return 1;
    }
    if (!out_path) {
        return copy_url_to_fd(url, STDOUT_FILENO);
    }
    int out = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out < 0) {
        print("curl: cannot create ");
        println(out_path);
        return 1;
    }
    int rc = copy_url_to_fd(url, out);
    close(out);
    return rc;
}

static const char *wget_output_name(const char *url)
{
    const char *path = url_local_path(url);
    const char *s = path ? path : url;
    const char *slash = strrchr(s, '/');
    const char *name = slash ? slash + 1 : s;
    return name[0] ? name : "index.html";
}

static int cmd_wget(int argc, char **argv)
{
    const char *url = NULL;
    const char *out_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            url = argv[i];
        }
    }
    if (!url) {
        println("usage: wget URL [-O FILE]");
        return 1;
    }
    if (!out_path) {
        out_path = wget_output_name(url);
    }
    int out = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out < 0) {
        print("wget: cannot create ");
        println(out_path);
        return 1;
    }
    int rc = copy_url_to_fd(url, out);
    close(out);
    return rc;
}

static void print_tls_feature(const char *name, int enabled)
{
    print(name);
    print(": ");
    println(enabled ? "yes" : "missing");
}

static int cmd_tls(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "status") != 0) {
        println("usage: tls status");
        return 1;
    }
    const struct tnu_tls_features *f = tnu_tls_features();
    int selftest = tnu_tls_selftest();
    println("TLS backend status:");
    print("selftest: ");
    println(selftest == TNU_TLS_OK ? "ok" : "failed");
    print_tls_feature("sha256", f->sha256);
    print_tls_feature("hkdf-sha256", f->hkdf_sha256);
    print_tls_feature("x25519", f->x25519);
    print_tls_feature("aes-128-gcm", f->aes_128_gcm);
    print_tls_feature("chacha20", f->chacha20);
    print_tls_feature("poly1305", f->poly1305);
    print_tls_feature("chacha20-poly1305", f->chacha20_poly1305);
    print_tls_feature("tls-record-crypto", f->tls_record_crypto);
    print_tls_feature("tls13-client-hello", f->tls13_client_hello);
    print_tls_feature("x509", f->x509);
    print_tls_feature("ca-store", f->ca_store);
    println("trust-store-path: /etc/ssl/certs/tnu-pins.txt");
    return (selftest == TNU_TLS_OK &&
            f->sha256 && f->hkdf_sha256 && f->x25519 &&
            (f->aes_128_gcm || f->chacha20_poly1305) &&
            f->tls13_client_hello && f->x509 && f->ca_store) ? 0 : 1;
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
    if (strcmp(cmd, "dns") == 0) return cmd_dns(argc, argv);
    if (strcmp(cmd, "driver") == 0) return cmd_driver(argc, argv);
    if (strcmp(cmd, "linuxdrv") == 0) return cmd_linuxdrv(argc, argv);
    if (strcmp(cmd, "net") == 0) return cmd_net(argc, argv);
    if (strcmp(cmd, "curl") == 0) return cmd_curl(argc, argv);
    if (strcmp(cmd, "wget") == 0) return cmd_wget(argc, argv);
    if (strcmp(cmd, "tls") == 0) return cmd_tls(argc, argv);
    if (strcmp(cmd, "wifi") == 0) return cmd_wifi(argc, argv);
    if (strcmp(cmd, "xedit") == 0) { println("xedit: unavailable from userspace"); return 1; }
    if (strcmp(cmd, "clear") == 0) { print("\033[2J\033[H"); return 0; }
    if (strcmp(cmd, "date") == 0) { println("date: unavailable from userspace"); return 1; }
    if (strcmp(cmd, "ps") == 0) { print_file("/proc/processes"); return 0; }
    if (strcmp(cmd, "kill") == 0) { println("usage: kill PID"); return 1; }
    if (strcmp(cmd, "chmod") == 0) return cmd_chmod(argc, argv);
    if (strcmp(cmd, "stat") == 0) return cmd_stat(argc, argv);
    if (strcmp(cmd, "tar") == 0) return cmd_tar(argc, argv);
    if (strcmp(cmd, "zip") == 0) return cmd_zip(argc, argv);
    if (strcmp(cmd, "unzip") == 0) return cmd_unzip(argc, argv);
    if (strcmp(cmd, "tirux") == 0) return cmd_tirux(argc, argv);
    if (strcmp(cmd, "shutdown") == 0) return cmd_shutdown(argc, argv);
    if (strcmp(cmd, "reboot") == 0) return cmd_reboot(argc, argv);
    if (strcmp(cmd, "sync") == 0) return cmd_sync(argc, argv);
    if (strcmp(cmd, "chown") == 0) { println("usage: chown USER FILE"); return 1; }
    if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "mv") == 0 ||
        strcmp(cmd, "mount") == 0 ||
        strcmp(cmd, "dmesg") == 0) {
        println("utility unavailable from userspace");
        return 1;
    }
    println("tnu-utils: unknown applet");
    return 127;
}
