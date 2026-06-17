#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <tnu/syscall.h>

long tnu_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);

static int del_user(const char *name)
{
    return (int)tnu_syscall(SYS_DEL_USER, (long)name, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: userdel NAME\n");
        return 1;
    }
    if (strcmp(argv[1], "root") == 0) {
        printf("userdel: refusing to remove root\n");
        return 1;
    }
    if (del_user(argv[1]) < 0) {
        printf("userdel: unknown user: %s\n", argv[1]);
        return 1;
    }
    printf("userdel: removed %s\n", argv[1]);
    return 0;
}
