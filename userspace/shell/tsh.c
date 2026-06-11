#include <tnu/libc.h>

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
    char *nl = strchr(buf, '\n');
    if (nl) {
        *nl = 0;
    }
    return (int)n;
}

static void username_from_uid(int uid, char *out, size_t out_size)
{
    char passwd[512];
    if (read_file("/etc/passwd", passwd, sizeof(passwd)) < 0) {
        strncpy(out, uid == 0 ? "root" : "user", out_size - 1);
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
    strncpy(out, "user", out_size - 1);
    out[out_size - 1] = 0;
}

static void read_line(char *buf, size_t size)
{
    size_t len = 0;
    while (len + 1 < size) {
        char c;
        if (read(0, &c, 1) <= 0) {
            break;
        }
        if (c == '\n') {
            break;
        }
        buf[len++] = c;
    }
    buf[len] = 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[128];
    for (;;) {
        char cwd[128];
        char host[64];
        char user[32];
        getcwd(cwd, sizeof(cwd));
        if (read_file("/etc/hostname", host, sizeof(host)) < 0) {
            strcpy(host, "tnu");
        }
        username_from_uid(getuid(), user, sizeof(user));
        print(user);
        print("@");
        print(host);
        print(":");
        print(cwd);
        print(getuid() == 0 ? "# " : "$ ");
        read_line(line, sizeof(line));
        if (strcmp(line, "exit") == 0) {
            return 0;
        }
        if (line[0]) {
            exec(line);
        }
    }
}
