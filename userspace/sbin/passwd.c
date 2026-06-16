#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tnu/syscall.h>

#define PASSWD_PATH "/etc/passwd"

long tnu_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);

static void strip_newline(char *s)
{
    if (!s) return;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            s[i] = '\0';
            return;
        }
    }
}

static int username_for_uid(int uid, char *out, size_t out_size)
{
    FILE *f = fopen(PASSWD_PATH, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char copy[256];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *name = strtok(copy, ":");
        strtok(NULL, ":");
        char *uid_s = strtok(NULL, ":");
        if (name && uid_s && atoi(uid_s) == uid) {
            strncpy(out, name, out_size - 1);
            out[out_size - 1] = '\0';
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int user_exists(const char *name)
{
    FILE *f = fopen(PASSWD_PATH, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        if (strcmp(line, name) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int set_password(const char *user, const char *password)
{
    return (int)tnu_syscall(SYS_SET_PASSWORD, (long)user, (long)password, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    char current_user[64];
    char user[64];
    if (argc > 2) {
        printf("usage: passwd [USER]\n");
        return 1;
    }
    if (username_for_uid(getuid(), current_user, sizeof(current_user)) < 0) {
        printf("passwd: cannot determine current user\n");
        return 1;
    }

    if (argc >= 2) {
        strncpy(user, argv[1], sizeof(user) - 1);
        user[sizeof(user) - 1] = '\0';
    } else {
        strncpy(user, current_user, sizeof(user) - 1);
        user[sizeof(user) - 1] = '\0';
    }

    if (!user_exists(user)) {
        printf("passwd: unknown user: %s\n", user);
        return 1;
    }
    if (getuid() != 0 && strcmp(user, current_user) != 0) {
        printf("passwd: permission denied for %s\n", user);
        return 1;
    }

    char p1[128], p2[128];
    printf("New password for %s (empty allowed): ", user);
    fflush(stdout);
    if (!fgets(p1, sizeof(p1), stdin)) return 1;
    strip_newline(p1);
    printf("Retype password: ");
    fflush(stdout);
    if (!fgets(p2, sizeof(p2), stdin)) return 1;
    strip_newline(p2);
    if (strcmp(p1, p2) != 0) {
        printf("passwd: passwords do not match\n");
        return 1;
    }
    if (set_password(user, p1) < 0) {
        printf("passwd: failed to update %s\n", user);
        return 1;
    }
    if (p1[0] == '\0') {
        printf("passwd: cleared password for %s\n", user);
    } else {
        printf("passwd: updated %s\n", user);
    }
    return 0;
}
