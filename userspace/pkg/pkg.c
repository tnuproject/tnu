#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PKG_CONFIG "/etc/pkg/repos.conf"
#define PKG_DB_DIR "/var/db/pkg/installed"
#define PKG_CACHE_DIR "/var/cache/pkg/repos"
#define MAX_REPOS 8
#define MAX_LINE 256
#define MAX_PATH_LEN 256

struct repo {
    char name[32];
    char url[MAX_PATH_LEN];
    char cache[MAX_PATH_LEN];
};

static int require_root_for_mutation(const char *action);

static void trim_newline(char *s)
{
    char *nl = strchr(s, '\n');
    if (nl) {
        *nl = '\0';
    }
}

static const char *skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static int mkdir_p(const char *path)
{
    char tmp[MAX_PATH_LEN];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    strcpy(tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static void parent_dir(const char *path, char *out, size_t out_size)
{
    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        strcpy(out, "/");
    } else {
        *slash = '\0';
    }
}

static void join_path(char *out, size_t out_size, const char *base, const char *name)
{
    if (strcmp(base, "/") == 0) {
        snprintf(out, out_size, "/%s", name);
    } else {
        snprintf(out, out_size, "%s/%s", base, name);
    }
}

static int read_line(int fd, char *out, size_t out_size)
{
    size_t n = 0;
    while (n + 1 < out_size) {
        char c;
        ssize_t got = read(fd, &c, 1);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            return -1;
        }
        out[n++] = c;
        if (c == '\n') {
            break;
        }
    }
    out[n] = '\0';
    return n == 0 ? 0 : 1;
}

static int read_text_file(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        out[0] = '\0';
        return -1;
    }
    ssize_t n = read(fd, out, out_size - 1);
    close(fd);
    if (n < 0) {
        out[0] = '\0';
        return -1;
    }
    out[n] = '\0';
    return (int)n;
}

static void parse_repo_line(char *line, struct repo *repos, size_t *count)
{
    const char *p = skip_space(line);
    if (*p == '\0' || *p == '#') {
        return;
    }
    char *eq = strchr((char *)p, '=');
    if (!eq) {
        return;
    }
    *eq = '\0';
    char *name = (char *)p;
    char *url = eq + 1;
    while (*url == ' ' || *url == '\t') {
        url++;
    }
    char *dot = strstr(name, ".cache");
    if (dot && strcmp(dot, ".cache") == 0) {
        *dot = '\0';
        for (size_t i = 0; i < *count; i++) {
            if (strcmp(repos[i].name, name) == 0) {
                strncpy(repos[i].cache, url, sizeof(repos[i].cache) - 1);
                repos[i].cache[sizeof(repos[i].cache) - 1] = '\0';
                return;
            }
        }
        return;
    }
    if (*count >= MAX_REPOS) {
        return;
    }
    strncpy(repos[*count].name, name, sizeof(repos[*count].name) - 1);
    repos[*count].name[sizeof(repos[*count].name) - 1] = '\0';
    strncpy(repos[*count].url, url, sizeof(repos[*count].url) - 1);
    repos[*count].url[sizeof(repos[*count].url) - 1] = '\0';
    repos[*count].cache[0] = '\0';
    (*count)++;
}

static int load_repos(struct repo *repos, size_t *count)
{
    *count = 0;
    char file[1024];
    if (read_text_file(PKG_CONFIG, file, sizeof(file)) < 0) {
        printf("pkg: cannot open %s\n", PKG_CONFIG);
        return -1;
    }

    char *line = file;
    for (char *p = file;; p++) {
        if (*p == '\n' || *p == '\0') {
            char old = *p;
            *p = '\0';
            parse_repo_line(line, repos, count);
            if (old == '\0') {
                break;
            }
            line = p + 1;
        }
    }
    return *count ? 0 : -1;
}

static const char *repo_local_path(const struct repo *repo)
{
    static char cache_path[MAX_PATH_LEN];
    if (strncmp(repo->url, "file://", 7) == 0) {
        return repo->url + 7;
    }
    if (repo->url[0] == '/') {
        return repo->url;
    }
    snprintf(cache_path, sizeof(cache_path), "%s/%s", PKG_CACHE_DIR, repo->name);
    struct stat st;
    if (stat(cache_path, &st) == 0 && st.st_type == TNU_DT_DIR) {
        return cache_path;
    }
    return NULL;
}

static bool repo_url_is_local(const struct repo *repo)
{
    return strncmp(repo->url, "file://", 7) == 0 || repo->url[0] == '/';
}

static const char *repo_bootstrap_path(const struct repo *repo)
{
    if (strncmp(repo->cache, "file://", 7) == 0) {
        return repo->cache + 7;
    }
    if (repo->cache[0] == '/') {
        return repo->cache;
    }
    return NULL;
}

static int find_repo_for_pkg(const char *pkg, char *out, size_t out_size)
{
    struct repo repos[MAX_REPOS];
    size_t count = 0;
    if (load_repos(repos, &count) < 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        const char *root = repo_local_path(&repos[i]);
        if (!root) {
            continue;
        }
        snprintf(out, out_size, "%s/packages/%s", root, pkg);
        struct stat st;
        if (stat(out, &st) == 0 && st.st_type == TNU_DT_DIR) {
            return 0;
        }
    }
    return -1;
}

static void progress(const char *label, size_t done, size_t total)
{
    int pct = total ? (int)((done * 100) / total) : 100;
    int bars = pct / 5;
    printf("\r%-18s [", label);
    for (int i = 0; i < 20; i++) {
        putchar(i < bars ? '#' : '-');
    }
    printf("] %3d%%", pct);
    if (pct >= 100) {
        printf("\n");
    }
}

static int copy_file(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) < 0) {
        return -1;
    }
    char parent[MAX_PATH_LEN];
    parent_dir(dst, parent, sizeof(parent));
    mkdir_p(parent);

    int in = open(src, O_RDONLY);
    if (in < 0) {
        return -1;
    }
    int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 0777);
    if (out < 0) {
        close(in);
        return -1;
    }

    size_t size = (size_t)st.st_size;
    char *buf = size ? malloc(size) : NULL;
    if (size && !buf) {
        close(in);
        close(out);
        return -1;
    }

    size_t copied = 0;
    while (copied < size) {
        ssize_t n = read(in, buf + copied, size - copied);
        if (n <= 0) {
            free(buf);
            close(in);
            close(out);
            return -1;
        }
        copied += (size_t)n;
    }

    if (size && write(out, buf, size) != (ssize_t)size) {
        free(buf);
        close(in);
        close(out);
        return -1;
    }
    progress(dst, 1, 1);
    free(buf);
    close(in);
    close(out);
    chmod(dst, st.st_mode & 0777);
    return 0;
}

static int copy_tree(const char *src_root, const char *dst_root)
{
    struct stat st;
    if (stat(src_root, &st) < 0) {
        return -1;
    }
    if (st.st_type == TNU_DT_FILE) {
        return copy_file(src_root, dst_root);
    }
    mkdir_p(dst_root);
    DIR *dir = opendir(src_root);
    if (!dir) {
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char src[MAX_PATH_LEN];
        char dst[MAX_PATH_LEN];
        join_path(src, sizeof(src), src_root, de->d_name);
        join_path(dst, sizeof(dst), dst_root, de->d_name);
        if (copy_tree(src, dst) < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

static int manifest_value(const char *manifest, const char *key, char *out, size_t out_size)
{
    int fd = open(manifest, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t key_len = strlen(key);
    char line[MAX_LINE];
    while (read_line(fd, line, sizeof(line)) > 0) {
        trim_newline(line);
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            strncpy(out, line + key_len + 1, out_size - 1);
            out[out_size - 1] = '\0';
            close(fd);
            return 0;
        }
    }
    close(fd);
    return -1;
}

static int list_repos(void)
{
    struct repo repos[MAX_REPOS];
    size_t count = 0;
    if (load_repos(repos, &count) < 0) {
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        printf("%s %s", repos[i].name, repos[i].url);
        const char *local = repo_local_path(&repos[i]);
        if (local) {
            printf(" cached=%s", local);
        } else if (repos[i].cache[0]) {
            printf(" bootstrap=%s", repos[i].cache);
        }
        printf("\n");
    }
    return 0;
}

static int sync_repos(void)
{
    if (require_root_for_mutation("sync") < 0) {
        return 1;
    }
    struct repo repos[MAX_REPOS];
    size_t count = 0;
    if (load_repos(repos, &count) < 0) {
        return 1;
    }
    mkdir_p(PKG_CACHE_DIR);
    int failures = 0;
    for (size_t i = 0; i < count; i++) {
        const char *src = repo_url_is_local(&repos[i]) ? repo_local_path(&repos[i]) : NULL;
        if (!src) {
            src = repo_bootstrap_path(&repos[i]);
        }
        if (!src) {
            printf("%s: HTTPS sync waits for TCP/HTTP transport: %s\n",
                   repos[i].name, repos[i].url);
            failures++;
            continue;
        }
        char dst[MAX_PATH_LEN];
        snprintf(dst, sizeof(dst), "%s/%s", PKG_CACHE_DIR, repos[i].name);
        printf("Syncing %s\n", repos[i].name);
        if (copy_tree(src, dst) < 0) {
            printf("pkg: sync failed: %s\n", repos[i].name);
            failures++;
        }
    }
    return failures ? 1 : 0;
}

static int list_packages(void)
{
    struct repo repos[MAX_REPOS];
    size_t count = 0;
    if (load_repos(repos, &count) < 0) {
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        const char *root = repo_local_path(&repos[i]);
        if (!root) {
            printf("%s: run pkg sync first; HTTPS transport pending for %s\n",
                   repos[i].name, repos[i].url);
            continue;
        }
        char packages[MAX_PATH_LEN];
        snprintf(packages, sizeof(packages), "%s/packages", root);
        DIR *dir = opendir(packages);
        if (!dir) {
            continue;
        }
        struct dirent *de;
        while ((de = readdir(dir))) {
            if (de->d_name[0] == '.') {
                continue;
            }
            char manifest[MAX_PATH_LEN];
            char version[64] = "unknown";
            char summary[96] = "";
            snprintf(manifest, sizeof(manifest), "%s/%s/manifest", packages, de->d_name);
            manifest_value(manifest, "version", version, sizeof(version));
            manifest_value(manifest, "summary", summary, sizeof(summary));
            printf("%-12s %-14s %s\n", de->d_name, version, summary);
        }
        closedir(dir);
    }
    return 0;
}

static int require_root_for_mutation(const char *action)
{
    if (getuid() == 0) {
        return 0;
    }
    printf("pkg: permission denied: %s requires root; try sudo pkg %s\n",
           action, action);
    return -1;
}

static int install_pkg(const char *name)
{
    if (require_root_for_mutation("install") < 0) {
        return 1;
    }
    char pkg_root[MAX_PATH_LEN];
    if (find_repo_for_pkg(name, pkg_root, sizeof(pkg_root)) < 0) {
        printf("pkg: package not found: %s\n", name);
        return 1;
    }
    char files[MAX_PATH_LEN];
    snprintf(files, sizeof(files), "%s/files", pkg_root);
    printf("Installing %s\n", name);
    if (copy_tree(files, "/") < 0) {
        printf("pkg: install failed while copying files\n");
        return 1;
    }
    mkdir_p(PKG_DB_DIR);
    char src_manifest[MAX_PATH_LEN];
    char dst_manifest[MAX_PATH_LEN];
    snprintf(src_manifest, sizeof(src_manifest), "%s/manifest", pkg_root);
    snprintf(dst_manifest, sizeof(dst_manifest), "%s/%s.manifest", PKG_DB_DIR, name);
    if (copy_file(src_manifest, dst_manifest) < 0) {
        printf("pkg: warning: installed files, but could not write package database\n");
        return 1;
    }
    printf("%s installed\n", name);
    return 0;
}

static int status_packages(void)
{
    DIR *dir = opendir(PKG_DB_DIR);
    if (!dir) {
        printf("No packages installed.\n");
        return 0;
    }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') {
            continue;
        }
        printf("%s\n", de->d_name);
    }
    closedir(dir);
    return 0;
}

static int remove_pkg(const char *name)
{
    if (require_root_for_mutation("remove") < 0) {
        return 1;
    }
    char manifest[MAX_PATH_LEN];
    snprintf(manifest, sizeof(manifest), "%s/%s.manifest", PKG_DB_DIR, name);
    int fd = open(manifest, O_RDONLY);
    if (fd < 0) {
        printf("pkg: not installed: %s\n", name);
        return 1;
    }
    char line[MAX_LINE];
    while (read_line(fd, line, sizeof(line)) > 0) {
        trim_newline(line);
        if (strncmp(line, "file=", 5) == 0) {
            unlink(line + 5);
            printf("removed %s\n", line + 5);
        }
    }
    close(fd);
    unlink(manifest);
    printf("%s removed\n", name);
    return 0;
}

static void usage(void)
{
    printf("pkg commands:\n");
    printf("  pkg list\n");
    printf("  pkg repos\n");
    printf("  pkg sync\n");
    printf("  pkg install NAME\n");
    printf("  pkg remove NAME\n");
    printf("  pkg status\n");
    printf("  pkg shell\n");
}

static int run_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "search") == 0) {
        return list_packages();
    }
    if (strcmp(argv[1], "repos") == 0) {
        return list_repos();
    }
    if (strcmp(argv[1], "sync") == 0 || strcmp(argv[1], "update") == 0) {
        return sync_repos();
    }
    if (strcmp(argv[1], "status") == 0) {
        return status_packages();
    }
    if (strcmp(argv[1], "install") == 0 && argc >= 3) {
        return install_pkg(argv[2]);
    }
    if ((strcmp(argv[1], "remove") == 0 || strcmp(argv[1], "delete") == 0) && argc >= 3) {
        return remove_pkg(argv[2]);
    }
    usage();
    return 1;
}

static int interactive(void)
{
    char line[128];
    char *argv[8];
    printf("TNU pkg interactive shell. Type help or quit.\n");
    for (;;) {
        printf("pkg> ");
        size_t n = 0;
        while (n + 1 < sizeof(line)) {
            int c = getchar();
            if (c == '\n' || c == '\r') {
                break;
            }
            if (c == EOF) {
                return 0;
            }
            line[n++] = (char)c;
        }
        line[n] = '\0';
        int argc = 1;
        argv[0] = "pkg";
        char *p = line;
        while (*p && argc < 8) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            argv[argc++] = p;
            while (*p && *p != ' ') {
                p++;
            }
            if (*p) {
                *p++ = '\0';
            }
        }
        argv[argc] = NULL;
        if (argc == 1) {
            continue;
        }
        if (strcmp(argv[1], "quit") == 0 || strcmp(argv[1], "exit") == 0) {
            return 0;
        }
        run_command(argc, argv);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "shell") == 0) {
        return interactive();
    }
    if (argc == 1) {
        usage();
        return 0;
    }
    return run_command(argc, argv);
}
