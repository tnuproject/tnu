#include <tnu/libc.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#define PKG_ARCH "x86_64"
#define PKG_ETC_DIR "/etc/pkg"
#define PKG_DB_DIR "/var/db/pkg/installed"
#define PKG_REPOS_FILE PKG_ETC_DIR "/repos.json"
#define PKG_DEFAULT_MAIN_NAME "universe-main"
#define PKG_DEFAULT_ALT_NAME  "universe-alt"
#define PKG_DEFAULT_MAIN_URL  "file:/universe-main"
#define PKG_DEFAULT_ALT_URL   "file:/universe-alt"
#define MAX_REPOS 16
#define MAX_PACKAGES 128

struct repo_entry {
    char name[64];
    char url[256];
};

struct pkg_info {
    char name[64];
    char version[32];
    char arch[32];
    char description[160];
    char archive[256];
};

static long read_all_text(const char *path, char *out, size_t out_size)
{
    int fd = open(path, O_RDONLY, 0);
    size_t pos = 0;
    if (fd < 0) return -1;
    while (pos + 1 < out_size) {
        ssize_t n = read(fd, out + pos, out_size - 1 - pos);
        if (n <= 0) break;
        pos += (size_t)n;
    }
    out[pos] = '\0';
    close(fd);
    return (long)pos;
}

static int write_all_text(const char *path, const char *data)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    size_t len = strlen(data);
    size_t off = 0;
    if (fd < 0) return -1;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static int mkdir_p(const char *path, uint32_t mode)
{
    char tmp[512];
    char *p;
    size_t len;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_dir_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode);
}

static void ensure_dirs(void)
{
    mkdir_p(PKG_ETC_DIR, 0755);
    mkdir_p(PKG_DB_DIR, 0755);
}

static int save_repos(const struct repo_entry *repos, int repo_count)
{
    char out[4096];
    size_t pos = 0;
    int i;
    pos += snprintf(out + pos, sizeof(out) - pos, "{\n  \"repos\": [\n");
    for (i = 0; i < repo_count; i++) {
        pos += snprintf(out + pos, sizeof(out) - pos,
            "    {\"name\":\"%s\",\"url\":\"%s\"}%s\n",
            repos[i].name, repos[i].url, (i + 1 < repo_count) ? "," : "");
    }
    pos += snprintf(out + pos, sizeof(out) - pos, "  ]\n}\n");
    return write_all_text(PKG_REPOS_FILE, out);
}

static int load_repos(struct repo_entry *repos, int *repo_count)
{
    char buf[4096];
    char *p;
    *repo_count = 0;
    if (read_all_text(PKG_REPOS_FILE, buf, sizeof(buf)) < 0) return -1;
    p = strstr(buf, "\"repos\"");
    if (!p) return -1;
    p = strchr(p, '[');
    if (!p) return -1;
    p++;
    while (*p && *p != ']' && *repo_count < MAX_REPOS) {
        char *obj = strchr(p, '{');
        char *name_key;
        char *url_key;
        char *q;
        char *e;
        size_t n;
        if (!obj) break;
        p = obj + 1;
        name_key = strstr(p, "\"name\":\"");
        url_key = strstr(p, "\"url\":\"");
        if (!name_key || !url_key) break;
        q = name_key + 8;
        e = strchr(q, '"');
        if (!e) break;
        n = (size_t)(e - q);
        if (n >= sizeof(repos[*repo_count].name)) n = sizeof(repos[*repo_count].name) - 1;
        memcpy(repos[*repo_count].name, q, n);
        repos[*repo_count].name[n] = '\0';
        q = url_key + 7;
        e = strchr(q, '"');
        if (!e) break;
        n = (size_t)(e - q);
        if (n >= sizeof(repos[*repo_count].url)) n = sizeof(repos[*repo_count].url) - 1;
        memcpy(repos[*repo_count].url, q, n);
        repos[*repo_count].url[n] = '\0';
        (*repo_count)++;
        p = e;
    }
    return *repo_count > 0 ? 0 : -1;
}

static void ensure_default_repos(void)
{
    struct repo_entry repos[2];
    int n = 0;
    ensure_dirs();
    if (load_repos(repos, &n) == 0) return;
    memset(repos, 0, sizeof(repos));
    strcpy(repos[0].name, PKG_DEFAULT_MAIN_NAME);
    strcpy(repos[0].url, PKG_DEFAULT_MAIN_URL);
    strcpy(repos[1].name, PKG_DEFAULT_ALT_NAME);
    strcpy(repos[1].url, PKG_DEFAULT_ALT_URL);
    save_repos(repos, 2);
}

static int repo_url_to_path(const char *url, char *out, size_t out_size)
{
    if (strncmp(url, "file:", 5) != 0) return -1;
    snprintf(out, out_size, "%s", url + 5);
    return 0;
}

static int load_repo_packages(const char *repo_url, struct pkg_info *pkgs, int *pkg_count)
{
    char repo_path[512];
    char repo_txt[512];
    char buf[16384];
    char *line;
    *pkg_count = 0;
    if (repo_url_to_path(repo_url, repo_path, sizeof(repo_path)) < 0) return -1;
    snprintf(repo_txt, sizeof(repo_txt), "%s/repo.txt", repo_path);
    if (read_all_text(repo_txt, buf, sizeof(buf)) < 0) return -1;
    line = buf;
    while (line && *line && *pkg_count < MAX_PACKAGES) {
        char *next = strchr(line, '\n');
        char *a;
        char *b;
        char *c;
        char *d;
        char *e;
        if (next) *next = '\0';
        if (*line) {
            a = strtok(line, "|");
            b = strtok(NULL, "|");
            c = strtok(NULL, "|");
            d = strtok(NULL, "|");
            e = strtok(NULL, "|");
            if (a && b && c && d && e) {
                strncpy(pkgs[*pkg_count].name, a, sizeof(pkgs[*pkg_count].name) - 1);
                strncpy(pkgs[*pkg_count].version, b, sizeof(pkgs[*pkg_count].version) - 1);
                strncpy(pkgs[*pkg_count].arch, c, sizeof(pkgs[*pkg_count].arch) - 1);
                strncpy(pkgs[*pkg_count].description, d, sizeof(pkgs[*pkg_count].description) - 1);
                strncpy(pkgs[*pkg_count].archive, e, sizeof(pkgs[*pkg_count].archive) - 1);
                (*pkg_count)++;
            }
        }
        line = next ? next + 1 : NULL;
    }
    return 0;
}

static int find_package_in_repo(const char *repo_url, const char *name, struct pkg_info *pkg)
{
    struct pkg_info pkgs[MAX_PACKAGES];
    int count = 0;
    int i;
    if (load_repo_packages(repo_url, pkgs, &count) < 0) return -1;
    for (i = 0; i < count; i++) {
        if (strcmp(pkgs[i].name, name) == 0 && strcmp(pkgs[i].arch, PKG_ARCH) == 0) {
            *pkg = pkgs[i];
            return 0;
        }
    }
    return -1;
}

static int copy_file(const char *src, const char *dst)
{
    int in_fd = open(src, O_RDONLY, 0);
    int out_fd;
    char buf[4096];
    if (in_fd < 0) return -1;
    out_fd = open(dst, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out_fd < 0) {
        close(in_fd);
        return -1;
    }
    for (;;) {
        ssize_t n = read(in_fd, buf, sizeof(buf));
        size_t off = 0;
        if (n < 0) {
            close(in_fd);
            close(out_fd);
            return -1;
        }
        if (n == 0) break;
        while (off < (size_t)n) {
            ssize_t w = write(out_fd, buf + off, (size_t)n - off);
            if (w <= 0) {
                close(in_fd);
                close(out_fd);
                return -1;
            }
            off += (size_t)w;
        }
    }
    close(in_fd);
    close(out_fd);
    return 0;
}

static int copy_tree(const char *src, const char *dst)
{
    DIR *dir;
    struct dirent *de;
    mkdir_p(dst, 0755);
    dir = opendir(src);
    if (!dir) return -1;
    while ((de = readdir(dir)) != NULL) {
        char src_path[512];
        char dst_path[512];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        snprintf(src_path, sizeof(src_path), "%s/%s", src, de->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, de->d_name);
        if (stat(src_path, &st) < 0) {
            closedir(dir);
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (copy_tree(src_path, dst_path) < 0) {
                closedir(dir);
                return -1;
            }
        } else {
            if (copy_file(src_path, dst_path) < 0) {
                closedir(dir);
                return -1;
            }
        }
    }
    closedir(dir);
    return 0;
}

static int cmd_repo_list(void)
{
    struct repo_entry repos[MAX_REPOS];
    int count = 0;
    int i;
    ensure_default_repos();
    if (load_repos(repos, &count) < 0) return 1;
    for (i = 0; i < count; i++) printf("%s %s\n", repos[i].name, repos[i].url);
    return 0;
}

static int cmd_repo_add(const char *name, const char *url)
{
    struct repo_entry repos[MAX_REPOS];
    int count = 0;
    int i;
    ensure_default_repos();
    load_repos(repos, &count);
    for (i = 0; i < count; i++) {
        if (strcmp(repos[i].name, name) == 0) {
            snprintf(repos[i].url, sizeof(repos[i].url), "%s", url);
            return save_repos(repos, count);
        }
    }
    if (count >= MAX_REPOS) {
        fprintf(stderr, "pkg: too many repos\n");
        return 1;
    }
    snprintf(repos[count].name, sizeof(repos[count].name), "%s", name);
    snprintf(repos[count].url, sizeof(repos[count].url), "%s", url);
    return save_repos(repos, count + 1);
}

static int cmd_repo_remove(const char *name)
{
    struct repo_entry repos[MAX_REPOS];
    int count = 0;
    int i;
    int found = -1;
    ensure_default_repos();
    if (load_repos(repos, &count) < 0) return 1;
    for (i = 0; i < count; i++) {
        if (strcmp(repos[i].name, name) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        fprintf(stderr, "pkg: repo not found: %s\n", name);
        return 1;
    }
    for (i = found; i + 1 < count; i++) repos[i] = repos[i + 1];
    return save_repos(repos, count - 1);
}

static int cmd_update(void)
{
    struct repo_entry repos[MAX_REPOS];
    int count = 0;
    int i;
    int ok = 0;
    ensure_default_repos();
    if (load_repos(repos, &count) < 0) return 1;
    for (i = 0; i < count; i++) {
        char repo_path[512];
        char repo_txt[512];
        if (repo_url_to_path(repos[i].url, repo_path, sizeof(repo_path)) < 0) {
            fprintf(stderr, "update failed %s %s\n", repos[i].name, repos[i].url);
            continue;
        }
        snprintf(repo_txt, sizeof(repo_txt), "%s/repo.txt", repo_path);
        if (path_exists(repo_txt)) {
            printf("updated %s %s\n", repos[i].name, repos[i].url);
            ok++;
        } else {
            fprintf(stderr, "update failed %s %s\n", repos[i].name, repos[i].url);
        }
    }
    return ok > 0 ? 0 : 1;
}

static int cmd_list(void)
{
    struct repo_entry repos[MAX_REPOS];
    int repo_count = 0;
    int i;
    ensure_default_repos();
    if (load_repos(repos, &repo_count) < 0) return 1;
    for (i = 0; i < repo_count; i++) {
        struct pkg_info pkgs[MAX_PACKAGES];
        int pkg_count = 0;
        int j;
        if (load_repo_packages(repos[i].url, pkgs, &pkg_count) < 0) continue;
        for (j = 0; j < pkg_count; j++) {
            if (strcmp(pkgs[j].arch, PKG_ARCH) == 0)
                printf("%s %s %s\n", pkgs[j].name, pkgs[j].version, repos[i].name);
        }
    }
    return 0;
}

static int cmd_search(const char *needle)
{
    struct repo_entry repos[MAX_REPOS];
    int repo_count = 0;
    int i;
    ensure_default_repos();
    if (load_repos(repos, &repo_count) < 0) return 1;
    for (i = 0; i < repo_count; i++) {
        struct pkg_info pkgs[MAX_PACKAGES];
        int pkg_count = 0;
        int j;
        if (load_repo_packages(repos[i].url, pkgs, &pkg_count) < 0) continue;
        for (j = 0; j < pkg_count; j++) {
            if (strstr(pkgs[j].name, needle) || strstr(pkgs[j].description, needle))
                printf("%s %s - %s\n", pkgs[j].name, pkgs[j].version, pkgs[j].description);
        }
    }
    return 0;
}

static int cmd_info(const char *name)
{
    struct repo_entry repos[MAX_REPOS];
    int repo_count = 0;
    int i;
    ensure_default_repos();
    if (load_repos(repos, &repo_count) < 0) return 1;
    for (i = 0; i < repo_count; i++) {
        struct pkg_info pkg;
        if (find_package_in_repo(repos[i].url, name, &pkg) == 0) {
            printf("name: %s\nversion: %s\narch: %s\nrepo: %s\ndescription: %s\narchive: %s\n",
                pkg.name, pkg.version, pkg.arch, repos[i].name, pkg.description, pkg.archive);
            return 0;
        }
    }
    fprintf(stderr, "pkg: package not found: %s\n", name);
    return 1;
}

static int install_package(const char *name)
{
    struct repo_entry repos[MAX_REPOS];
    int repo_count = 0;
    int i;
    ensure_default_repos();
    if (getuid() != 0) {
        fprintf(stderr, "pkg: install: permission denied (requires root)\n");
        return 1;
    }
    if (load_repos(repos, &repo_count) < 0) return 1;
    for (i = 0; i < repo_count; i++) {
        struct pkg_info pkg;
        char repo_path[512];
        char src_path[512];
        char db_path[512];
        if (repo_url_to_path(repos[i].url, repo_path, sizeof(repo_path)) < 0) continue;
        if (find_package_in_repo(repos[i].url, name, &pkg) < 0) continue;
        snprintf(src_path, sizeof(src_path), "%s/%s", repo_path, pkg.archive);
        if (!path_exists(src_path) || !is_dir_path(src_path)) continue;
        if (copy_tree(src_path, "/") < 0) {
            fprintf(stderr, "pkg: install failed from %s\n", repos[i].name);
            return 1;
        }
        mkdir_p(PKG_DB_DIR, 0755);
        snprintf(db_path, sizeof(db_path), "%s/%s", PKG_DB_DIR, name);
        if (write_all_text(db_path, pkg.version) < 0) {
            fprintf(stderr, "pkg: warning: installed but db update failed\n");
        }
        printf("installed %s %s from %s\n", pkg.name, pkg.version, repos[i].name);
        return 0;
    }
    fprintf(stderr, "pkg: package not found in any mirror: %s\n", name);
    return 1;
}

static void usage(const char *argv0)
{
    printf("usage: %s <command> [args]\n", argv0);
    printf("commands:\n");
    printf("  repo-list\n");
    printf("  repo-add <name> <url>\n");
    printf("  repo-remove <name>\n");
    printf("  update\n");
    printf("  list\n");
    printf("  search <term>\n");
    printf("  info <package>\n");
    printf("  install <package>\n");
}

int main(int argc, char **argv)
{
    ensure_default_repos();
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "repo-list") == 0) return cmd_repo_list();
    if (strcmp(argv[1], "repo-add") == 0) {
        if (argc < 4) {
            fprintf(stderr, "pkg: repo-add requires name and url\n");
            return 1;
        }
        return cmd_repo_add(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "repo-remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "pkg: repo-remove requires a repo name\n");
            return 1;
        }
        return cmd_repo_remove(argv[2]);
    }
    if (strcmp(argv[1], "update") == 0) return cmd_update();
    if (strcmp(argv[1], "list") == 0) return cmd_list();
    if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) {
            fprintf(stderr, "pkg: search requires a term\n");
            return 1;
        }
        return cmd_search(argv[2]);
    }
    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "pkg: info requires a package name\n");
            return 1;
        }
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "install") == 0) {
        if (argc < 3) {
            fprintf(stderr, "pkg: install requires a package name\n");
            return 1;
        }
        return install_package(argv[2]);
    }
    usage(argv[0]);
    return 1;
}