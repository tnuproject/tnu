#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int filter_colon_file(const char *path, const char *tmp, const char *name)
{
    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) return -1;
    int removed = 0;
    char line[256];
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char copy[256];
            strncpy(copy, line, sizeof(copy) - 1);
            copy[sizeof(copy) - 1] = '\0';
            char *colon = strchr(copy, ':');
            if (colon) *colon = '\0';
            if (strcmp(copy, name) == 0) {
                removed = 1;
                continue;
            }
            fputs(line, out);
        }
        fclose(in);
    }
    fclose(out);
    remove(path);
    rename(tmp, path);
    return removed;
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
    int removed = filter_colon_file("/etc/passwd", "/etc/passwd.tmp", argv[1]);
    filter_colon_file("/etc/shadow", "/etc/shadow.tmp", argv[1]);
    filter_colon_file("/etc/group", "/etc/group.tmp", argv[1]);
    if (removed <= 0) {
        printf("userdel: unknown user: %s\n", argv[1]);
        return 1;
    }
    printf("userdel: removed %s\n", argv[1]);
    return 0;
}
