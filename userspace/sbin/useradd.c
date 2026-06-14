#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"
#define GROUP_PATH  "/etc/group"

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

static int next_uid(void)
{
    int max = 999;
    FILE *f = fopen(PASSWD_PATH, "r");
    if (!f) return 1000;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ':');
        if (!p) continue;
        p = strchr(p + 1, ':');
        if (!p) continue;
        int uid = atoi(p + 1);
        if (uid > max) max = uid;
    }
    fclose(f);
    return max + 1;
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
    if (user_exists(name)) {
        printf("useradd: user already exists: %s\n", name);
        return 1;
    }

    int uid = next_uid();
    char home[128];
    snprintf(home, sizeof(home), "/home/%s", name);

    FILE *pw = fopen(PASSWD_PATH, "a");
    if (!pw) {
        printf("useradd: cannot open %s\n", PASSWD_PATH);
        return 1;
    }
    fprintf(pw, "%s:x:%d:%d::%s:/bin/tsh\n", name, uid, uid, home);
    fclose(pw);

    FILE *sh = fopen(SHADOW_PATH, "a");
    if (sh) {
        fprintf(sh, "%s:\n", name); /* empty password by default */
        fclose(sh);
    }

    FILE *gr = fopen(GROUP_PATH, "a");
    if (gr) {
        fprintf(gr, "%s:x:%d:\n", name, uid);
        fclose(gr);
    }

    mkdir(home, 0755);
    printf("useradd: created %s uid=%d home=%s\n", name, uid, home);
    printf("useradd: password is empty; press Enter at login or run passwd %s\n", name);
    return 0;
}
