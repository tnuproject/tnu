#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"
#define TMP_PATH    "/etc/shadow.tmp"

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

int main(int argc, char **argv)
{
    char user[64];
    if (argc >= 2) {
        strncpy(user, argv[1], sizeof(user) - 1);
        user[sizeof(user) - 1] = '\0';
    } else if (username_for_uid(getuid(), user, sizeof(user)) < 0) {
        printf("passwd: cannot determine current user\n");
        return 1;
    }

    if (!user_exists(user)) {
        printf("passwd: unknown user: %s\n", user);
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

    FILE *in = fopen(SHADOW_PATH, "r");
    FILE *out = fopen(TMP_PATH, "w");
    if (!out) {
        printf("passwd: cannot write %s\n", TMP_PATH);
        return 1;
    }

    int updated = 0;
    char line[256];
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char copy[256];
            strncpy(copy, line, sizeof(copy) - 1);
            copy[sizeof(copy) - 1] = '\0';
            char *colon = strchr(copy, ':');
            if (colon) *colon = '\0';
            if (strcmp(copy, user) == 0) {
                fprintf(out, "%s:%s\n", user, p1);
                updated = 1;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }
    if (!updated) {
        fprintf(out, "%s:%s\n", user, p1);
    }
    fclose(out);

    remove(SHADOW_PATH);
    if (rename(TMP_PATH, SHADOW_PATH) < 0) {
        printf("passwd: cannot replace %s\n", SHADOW_PATH);
        return 1;
    }
    printf("passwd: updated %s\n", user);
    return 0;
}
