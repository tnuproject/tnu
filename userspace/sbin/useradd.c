#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <tnu/syscall.h>

long tnu_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);

static int valid_name(const char *s)
{
    if (!s || !s[0]) return 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int add_user(const char *name)
{
    return (int)tnu_syscall(SYS_ADD_USER, (long)name, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: useradd NAME\n");
        return 1;
    }
    const char *name = argv[1];
    if (!valid_name(name)) {
        printf("useradd: invalid username\n");
        return 1;
    }
    if (add_user(name) < 0) {
        printf("useradd: failed to create %s\n", name);
        return 1;
    }
    printf("useradd: created %s with home /home/%s\n", name, name);
    printf("useradd: run passwd %s to set a password\n", name);
    return 0;
}
