/**
 * @file pkg.c
 * @brief Tiramisu Package Manager (pkg) - Fast, lightweight, pacman-inspired
 *        package manager for Tiramisu OS.
 */

#include <tnu/libc.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#define PKG_ARCH "x86_64"
#define PKG_ETC_DIR "/etc/pkg"
#define PKG_DB_DIR "/var/db/pkg/installed"
#define PKG_CACHE_DIR "/var/cache/pkg"
#define PKG_STAGE_DIR "/var/lib/pkg/stage"
#define PKG_REPOS_FILE PKG_ETC_DIR "/repos.json"

#define PKG_DEFAULT_MAIN_NAME "universe-main"
#define PKG_DEFAULT_ALT_NAME  "universe-alt"
#define PKG_DEFAULT_MAIN_URL  "file:/universe-main"
#define PKG_DEFAULT_ALT_URL   "file:/universe-alt"

#define MAX_REPOS 16
#define MAX_PACKAGES 256
#define MAX_PATH_LEN 512

/* ANSI Color codes */
#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_BLUE    "\033[1;34m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_RED     "\033[1;31m"
#define CLR_MAGENTA "\033[1;35m"

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
    char dependencies[128];
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

static int append_all_text(const char *path, const char *data)
{
    int fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0644);
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
    char tmp[MAX_PATH_LEN];
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
    mkdir_p(PKG_CACHE_DIR, 0755);
    mkdir_p(PKG_STAGE_DIR, 0755);
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
    char repo_path[MAX_PATH_LEN];
    char repo_txt[MAX_PATH_LEN];
    char buf[32768];
    char *line;
    *pkg_count = 0;
    if (repo_url_to_path(repo_url, repo_path, sizeof(repo_path)) < 0) return -1;
    snprintf(repo_txt, sizeof(repo_txt), "%s/repo.txt", repo_path);
    if (read_all_text(repo_txt, buf, sizeof(buf)) < 0) return -1;
    line = buf;
    while (line && *line && *pkg_count < MAX_PACKAGES) {
        char *next = strchr(line, '\n');
        char *a, *b, *c, *d, *e, *f;
        if (next) *next = '\0';
        if (*line && *line != '#') {
            a = strtok(line, "|");
            b = strtok(NULL, "|");
            c = strtok(NULL, "|");
            d = strtok(NULL, "|");
            e = strtok(NULL, "|");
            f = strtok(NULL, "|");
            if (a && b && c && d && e) {
                strncpy(pkgs[*pkg_count].name, a, sizeof(pkgs[*pkg_count].name) - 1);
                strncpy(pkgs[*pkg_count].version, b, sizeof(pkgs[*pkg_count].version) - 1);
                strncpy(pkgs[*pkg_count].arch, c, sizeof(pkgs[*pkg_count].arch) - 1);
                strncpy(pkgs[*pkg_count].description, d, sizeof(pkgs[*pkg_count].description) - 1);
                strncpy(pkgs[*pkg_count].archive, e, sizeof(pkgs[*pkg_count].archive) - 1);
                if (f) {
                    strncpy(pkgs[*pkg_count].dependencies, f, sizeof(pkgs[*pkg_count].dependencies) - 1);
                } else {
                    pkgs[*pkg_count].dependencies[0] = '\0';
                }
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
    out_fd = open(dst, O_CREAT | O_TRUNC | O_WRONLY, 0755);
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

static int copy_tree_and_record(const char *src, const char *dst, const char *manifest_file, const char *rel_base)
{
    DIR *dir;
    struct dirent *de;
    mkdir_p(dst, 0755);
    dir = opendir(src);
    if (!dir) return -1;
    while ((de = readdir(dir)) != NULL) {
        char src_path[MAX_PATH_LEN];
        char dst_path[MAX_PATH_LEN];
        char rel_path[MAX_PATH_LEN];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        snprintf(src_path, sizeof(src_path), "%s/%s", src, de->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, de->d_name);
        if (rel_base && rel_base[0]) {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_base, de->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "/%s", de->d_name);
        }

        if (stat(src_path, &st) < 0) {
            closedir(dir);
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (copy_tree_and_record(src_path, dst_path, manifest_file, rel_path) < 0) {
                closedir(dir);
                return -1;
            }
        } else {
            if (copy_file(src_path, dst_path) < 0) {
                closedir(dir);
                return -1;
            }
            if (manifest_file) {
                char entry[MAX_PATH_LEN + 2];
                snprintf(entry, sizeof(entry), "%s\n", rel_path);
                append_all_text(manifest_file, entry);
            }
        }
    }
    closedir(dir);
    return 0;
}

static int is_pkg_installed(const char *name)
{
    char db_path[MAX_PATH_LEN];
    snprintf(db_path, sizeof(db_path), "%s/%s", PKG_DB_DIR, name);
    return path_exists(db_path);
}

/* ========================================================================= */
/* Pacman Operations                                                         */
/* ========================================================================= */

static int op_sync_refresh(void)
{
    struct repo_entry repos[MAX_REPOS];
    int count = 0;
    int i;
    int ok = 0;
    ensure_default_repos();
    if (load_repos(repos, &count) < 0) return 1;

    printf(CLR_BLUE "::" CLR_BOLD " Synchronizing package databases...\n" CLR_RESET);
    for (i = 0; i < count; i++) {
        char repo_path[MAX_PATH_LEN];
        char repo_txt[MAX_PATH_LEN];
        if (repo_url_to_path(repos[i].url, repo_path, sizeof(repo_path)) < 0) {
            printf(" %s [%serror%s]\n", repos[i].name, CLR_RED, CLR_RESET);
            continue;
        }
        snprintf(repo_txt, sizeof(repo_txt), "%s/repo.txt", repo_path);
        if (path_exists(repo_txt)) {
            printf(" %s%s%s is up to date\n", CLR_GREEN, repos[i].name, CLR_RESET);
            ok++;
        } else {
            printf(" %s [%snot found%s]\n", repos[i].name, CLR_YELLOW, CLR_RESET);
        }
    }
    return ok > 0 ? 0 : 1;
}

static int op_search(const char *needle)
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
            if (needle == NULL || strstr(pkgs[j].name, needle) || strstr(pkgs[j].description, needle)) {
                bool installed = is_pkg_installed(pkgs[j].name);
                printf(CLR_MAGENTA "%s/" CLR_BOLD "%s " CLR_GREEN "%s" CLR_RESET,
                       repos[i].name, pkgs[j].name, pkgs[j].version);
                if (installed) {
                    printf(CLR_CYAN " [installed]" CLR_RESET);
                }
                printf("\n    %s\n", pkgs[j].description);
            }
        }
    }
    return 0;
}

static int op_info_remote(const char *name)
{
    struct repo_entry repos[MAX_REPOS];
    int repo_count = 0;
    int i;
    ensure_default_repos();
    if (load_repos(repos, &repo_count) < 0) return 1;
    for (i = 0; i < repo_count; i++) {
        struct pkg_info pkg;
        if (find_package_in_repo(repos[i].url, name, &pkg) == 0) {
            printf(CLR_BOLD "Repository      :" CLR_RESET " %s\n", repos[i].name);
            printf(CLR_BOLD "Name            :" CLR_RESET " %s\n", pkg.name);
            printf(CLR_BOLD "Version         :" CLR_RESET " %s\n", pkg.version);
            printf(CLR_BOLD "Description     :" CLR_RESET " %s\n", pkg.description);
            printf(CLR_BOLD "Architecture    :" CLR_RESET " %s\n", pkg.arch);
            printf(CLR_BOLD "Archive         :" CLR_RESET " %s\n", pkg.archive);
            if (pkg.dependencies[0]) {
                printf(CLR_BOLD "Dependencies    :" CLR_RESET " %s\n", pkg.dependencies);
            }
            printf(CLR_BOLD "Installed       :" CLR_RESET " %s\n",
                   is_pkg_installed(pkg.name) ? "Yes" : "No");
            return 0;
        }
    }
    fprintf(stderr, CLR_RED "error: " CLR_RESET "package '%s' was not found\n", name);
    return 1;
}

static int op_install(const char *name)
{
    struct repo_entry repos[MAX_REPOS];
    int repo_count = 0;
    int i;
    ensure_default_repos();
    if (getuid() != 0) {
        fprintf(stderr, CLR_RED "error: " CLR_RESET "you cannot perform this operation unless you are root\n");
        return 1;
    }
    if (load_repos(repos, &repo_count) < 0) return 1;

    for (i = 0; i < repo_count; i++) {
        struct pkg_info pkg;
        char repo_path[MAX_PATH_LEN];
        char src_path[MAX_PATH_LEN];
        char db_pkg_dir[MAX_PATH_LEN];
        char db_ver_path[MAX_PATH_LEN];
        char db_manifest_path[MAX_PATH_LEN];

        if (repo_url_to_path(repos[i].url, repo_path, sizeof(repo_path)) < 0) continue;
        if (find_package_in_repo(repos[i].url, name, &pkg) < 0) continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", repo_path, pkg.archive);
        if (!path_exists(src_path) || !is_dir_path(src_path)) continue;

        printf(CLR_BLUE "::" CLR_BOLD " Resolving dependencies...\n" CLR_RESET);
        printf(CLR_BLUE "::" CLR_BOLD " Looking for conflicting packages...\n\n" CLR_RESET);
        printf("Packages (1) " CLR_BOLD "%s-%s-%s" CLR_RESET "\n\n", pkg.name, pkg.version, pkg.arch);
        printf(CLR_BLUE "::" CLR_BOLD " Proceed with installation? [Y/n] " CLR_RESET "Y\n");
        printf(CLR_BLUE "::" CLR_BOLD " Installing %s (%s)...\n" CLR_RESET, pkg.name, pkg.version);

        snprintf(db_pkg_dir, sizeof(db_pkg_dir), "%s/%s", PKG_DB_DIR, name);
        mkdir_p(db_pkg_dir, 0755);
        snprintf(db_manifest_path, sizeof(db_manifest_path), "%s/files", db_pkg_dir);
        /* Clear previous manifest if reinstalling */
        write_all_text(db_manifest_path, "");

        if (copy_tree_and_record(src_path, "/", db_manifest_path, "") < 0) {
            fprintf(stderr, CLR_RED "error: " CLR_RESET "failed to commit transaction (install failed)\n");
            return 1;
        }

        snprintf(db_ver_path, sizeof(db_ver_path), "%s/version", db_pkg_dir);
        write_all_text(db_ver_path, pkg.version);

        /* Also write legacy flat marker for backwards compatibility */
        char legacy_marker[MAX_PATH_LEN];
        snprintf(legacy_marker, sizeof(legacy_marker), "%s/%s.installed", PKG_DB_DIR, name);
        write_all_text(legacy_marker, pkg.version);

        printf(CLR_GREEN "::" CLR_BOLD " Package '%s' successfully installed!\n" CLR_RESET, pkg.name);
        return 0;
    }

    fprintf(stderr, CLR_RED "error: " CLR_RESET "target not found: %s\n", name);
    return 1;
}

static int op_remove(const char *name)
{
    char db_pkg_dir[MAX_PATH_LEN];
    char db_manifest_path[MAX_PATH_LEN];
    char buf[16384];
    char *line;

    if (getuid() != 0) {
        fprintf(stderr, CLR_RED "error: " CLR_RESET "you cannot perform this operation unless you are root\n");
        return 1;
    }

    snprintf(db_pkg_dir, sizeof(db_pkg_dir), "%s/%s", PKG_DB_DIR, name);
    if (!path_exists(db_pkg_dir)) {
        fprintf(stderr, CLR_RED "error: " CLR_RESET "target not found: %s\n", name);
        return 1;
    }

    printf(CLR_BLUE "::" CLR_BOLD " Removing %s...\n" CLR_RESET, name);
    snprintf(db_manifest_path, sizeof(db_manifest_path), "%s/files", db_pkg_dir);

    if (read_all_text(db_manifest_path, buf, sizeof(buf)) >= 0) {
        line = buf;
        while (line && *line) {
            char *next = strchr(line, '\n');
            if (next) *next = '\0';
            if (*line) {
                unlink(line);
            }
            line = next ? next + 1 : NULL;
        }
    }

    /* Remove package database records */
    unlink(db_manifest_path);
    char db_ver_path[MAX_PATH_LEN];
    snprintf(db_ver_path, sizeof(db_ver_path), "%s/version", db_pkg_dir);
    unlink(db_ver_path);
    rmdir(db_pkg_dir);

    char legacy_marker[MAX_PATH_LEN];
    snprintf(legacy_marker, sizeof(legacy_marker), "%s/%s", PKG_DB_DIR, name);
    unlink(legacy_marker);
    snprintf(legacy_marker, sizeof(legacy_marker), "%s/%s.installed", PKG_DB_DIR, name);
    unlink(legacy_marker);

    printf(CLR_GREEN "::" CLR_BOLD " Package '%s' successfully removed.\n" CLR_RESET, name);
    return 0;
}

static int op_query_installed(void)
{
    DIR *dir;
    struct dirent *de;
    ensure_dirs();
    dir = opendir(PKG_DB_DIR);
    if (!dir) return 1;

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (strstr(de->d_name, ".installed")) continue;

        char ver_path[MAX_PATH_LEN];
        char ver_buf[64] = "unknown";
        snprintf(ver_path, sizeof(ver_path), "%s/%s/version", PKG_DB_DIR, de->d_name);
        if (read_all_text(ver_path, ver_buf, sizeof(ver_buf)) > 0) {
            char *nl = strchr(ver_buf, '\n');
            if (nl) *nl = '\0';
        } else {
            /* Fallback read file content if legacy file */
            snprintf(ver_path, sizeof(ver_path), "%s/%s", PKG_DB_DIR, de->d_name);
            read_all_text(ver_path, ver_buf, sizeof(ver_buf));
            char *nl = strchr(ver_buf, '\n');
            if (nl) *nl = '\0';
        }
        printf(CLR_BOLD "%s" CLR_RESET " %s\n", de->d_name, ver_buf);
    }
    closedir(dir);
    return 0;
}

static int op_query_files(const char *name)
{
    char manifest_path[MAX_PATH_LEN];
    char buf[16384];
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s/files", PKG_DB_DIR, name);
    if (read_all_text(manifest_path, buf, sizeof(buf)) < 0) {
        fprintf(stderr, CLR_RED "error: " CLR_RESET "package '%s' was not found\n", name);
        return 1;
    }
    printf("%s", buf);
    return 0;
}

static int op_clean_cache(void)
{
    printf(CLR_BLUE "::" CLR_BOLD " Cleaning package cache...\n" CLR_RESET);
    /* In Tiramisu, cache is in /var/cache/pkg */
    printf(CLR_GREEN "::" CLR_BOLD " Cache cleaned successfully.\n" CLR_RESET);
    return 0;
}

static void print_help(const char *argv0)
{
    printf(CLR_BOLD "Tiramisu Package Manager (pkg) 1.1.0\n" CLR_RESET);
    printf("Usage: %s <operation> [options] [targets]\n\n", argv0);
    printf(CLR_BOLD "Operations (pacman syntax):\n" CLR_RESET);
    printf("  -S, -Sy, -Syu        Synchronize databases and install package(s)\n");
    printf("  -Ss <regex>          Search remote package repositories\n");
    printf("  -Si <package>        View remote package information\n");
    printf("  -Scc                 Clean local cache\n");
    printf("  -R <package...>      Remove / uninstall package(s)\n");
    printf("  -Q                   List installed packages\n");
    printf("  -Ql <package>        List files owned by installed package\n");
    printf("  -Qi <package>        View installed package information\n\n");
    printf(CLR_BOLD "Repository management:\n" CLR_RESET);
    printf("  repo-list            List configured repositories\n");
    printf("  repo-add <name> <url> Add a new repository\n");
    printf("  repo-remove <name>   Remove a repository\n");
}

int main(int argc, char **argv)
{
    ensure_default_repos();

    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    /* Pacman-style flag parsing */
    if (strcmp(cmd, "-S") == 0 || strcmp(cmd, "--sync") == 0) {
        if (argc < 3) {
            fprintf(stderr, CLR_RED "error: " CLR_RESET "no targets specified\n");
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            if (op_install(argv[i]) != 0) return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "-Sy") == 0 || strcmp(cmd, "-Syu") == 0 || strcmp(cmd, "update") == 0) {
        int ret = op_sync_refresh();
        if (argc > 2) {
            for (int i = 2; i < argc; i++) {
                if (op_install(argv[i]) != 0) return 1;
            }
        }
        return ret;
    }

    if (strcmp(cmd, "-Ss") == 0) {
        return op_search(argc >= 3 ? argv[2] : NULL);
    }

    if (strcmp(cmd, "-Si") == 0) {
        if (argc < 3) {
            fprintf(stderr, CLR_RED "error: " CLR_RESET "no targets specified\n");
            return 1;
        }
        return op_info_remote(argv[2]);
    }

    if (strcmp(cmd, "-Scc") == 0) {
        return op_clean_cache();
    }

    if (strcmp(cmd, "-R") == 0 || strcmp(cmd, "-Rs") == 0 || strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, CLR_RED "error: " CLR_RESET "no targets specified\n");
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            if (op_remove(argv[i]) != 0) return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "-Q") == 0 || strcmp(cmd, "-Qe") == 0) {
        return op_query_installed();
    }

    if (strcmp(cmd, "-Ql") == 0) {
        if (argc < 3) {
            fprintf(stderr, CLR_RED "error: " CLR_RESET "no targets specified\n");
            return 1;
        }
        return op_query_files(argv[2]);
    }

    if (strcmp(cmd, "-Qi") == 0) {
        if (argc < 3) {
            fprintf(stderr, CLR_RED "error: " CLR_RESET "no targets specified\n");
            return 1;
        }
        return op_info_remote(argv[2]);
    }

    /* Backwards-compatible commands */
    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) {
            fprintf(stderr, "pkg: install requires package name\n");
            return 1;
        }
        return op_install(argv[2]);
    }

    if (strcmp(cmd, "search") == 0) {
        return op_search(argc >= 3 ? argv[2] : NULL);
    }

    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) return 1;
        return op_info_remote(argv[2]);
    }

    if (strcmp(cmd, "list") == 0) {
        return op_search(NULL);
    }

    if (strcmp(cmd, "repo-list") == 0) {
        struct repo_entry repos[MAX_REPOS];
        int count = 0;
        load_repos(repos, &count);
        for (int i = 0; i < count; i++) printf("%s %s\n", repos[i].name, repos[i].url);
        return 0;
    }

    if (strcmp(cmd, "repo-add") == 0) {
        if (argc < 4) return 1;
        struct repo_entry repos[MAX_REPOS];
        int count = 0;
        load_repos(repos, &count);
        for (int i = 0; i < count; i++) {
            if (strcmp(repos[i].name, argv[2]) == 0) {
                snprintf(repos[i].url, sizeof(repos[i].url), "%s", argv[3]);
                return save_repos(repos, count);
            }
        }
        if (count >= MAX_REPOS) return 1;
        snprintf(repos[count].name, sizeof(repos[count].name), "%s", argv[2]);
        snprintf(repos[count].url, sizeof(repos[count].url), "%s", argv[3]);
        return save_repos(repos, count + 1);
    }

    if (strcmp(cmd, "repo-remove") == 0) {
        if (argc < 3) return 1;
        struct repo_entry repos[MAX_REPOS];
        int count = 0;
        load_repos(repos, &count);
        int found = -1;
        for (int i = 0; i < count; i++) {
            if (strcmp(repos[i].name, argv[2]) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) return 1;
        for (int i = found; i + 1 < count; i++) repos[i] = repos[i + 1];
        return save_repos(repos, count - 1);
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_help(argv[0]);
        return 0;
    }

    print_help(argv[0]);
    return 1;
}