#include <tnu/libc.h>

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

static void print_file(const char *path)
{
    char buf[256];
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
    close(fd);
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
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            print(" ");
        }
        print(argv[i]);
    }
    print("\n");
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
    print_file("/etc/sysfetch-logo");
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

int main(int argc, char **argv)
{
    const char *cmd = argc > 0 ? basename(argv[0]) : "";
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
    if (strcmp(cmd, "wifi") == 0) {
        println("wifi: use the kernel applet from tsh for scan/connect support");
        return 1;
    }
    if (strcmp(cmd, "xedit") == 0) { println("xedit: unavailable from userspace"); return 1; }
    if (strcmp(cmd, "clear") == 0) { print("\033[2J\033[H"); return 0; }
    if (strcmp(cmd, "date") == 0) { println("date: unavailable from userspace"); return 1; }
    if (strcmp(cmd, "ps") == 0) { print_file("/proc/processes"); return 0; }
    if (strcmp(cmd, "kill") == 0) { println("usage: kill PID"); return 1; }
    if (strcmp(cmd, "chmod") == 0 && argc == 3) return chmod(argv[2], atoi(argv[1]));
    if (strcmp(cmd, "chown") == 0) { println("usage: chown USER FILE"); return 1; }
    if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "mv") == 0 ||
        strcmp(cmd, "mount") == 0 ||
        strcmp(cmd, "dmesg") == 0 ||
        strcmp(cmd, "reboot") == 0 || strcmp(cmd, "shutdown") == 0 ||
        strcmp(cmd, "stat") == 0) {
        println("utility unavailable from userspace");
        return 1;
    }
    println("tnu-utils: unknown applet");
    return 127;
}
