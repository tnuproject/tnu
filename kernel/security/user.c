#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/user.h>
#include <tnu/vfs.h>

#define MAX_USERS 32

static struct user_record users[MAX_USERS];
static size_t users_len;
static size_t current_user;

static uint64_t fnv1a_update(uint64_t hash, const char *s)
{
    while (s && *s) {
        hash ^= (uint8_t)*s++;
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint32_t make_salt(const char *name, uint32_t uid)
{
    uint64_t h = 1469598103934665603ull;
    h = fnv1a_update(h, name);
    h ^= uid;
    h *= 1099511628211ull;
    h ^= 0x544e5531u;
    return (uint32_t)(h ^ (h >> 32));
}

static uint64_t hash_password(uint32_t salt, const char *password)
{
    char salt_buf[16];
    ksnprintf(salt_buf, sizeof(salt_buf), "%x", salt);
    uint64_t h = 1469598103934665603ull;
    h = fnv1a_update(h, USER_HASH_SCHEME);
    h = fnv1a_update(h, "$");
    h = fnv1a_update(h, salt_buf);
    h = fnv1a_update(h, "$");
    h = fnv1a_update(h, password);
    return h;
}

static uint64_t parse_hex64(const char *s)
{
    uint64_t value = 0;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') {
            digit = *s - '0';
        } else if (*s >= 'a' && *s <= 'f') {
            digit = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'F') {
            digit = *s - 'A' + 10;
        } else {
            break;
        }
        value = (value << 4) | (uint64_t)digit;
        s++;
    }
    return value;
}

static void add_record(const char *name, uint32_t uid, uint32_t gid,
                       const char *home, const char *shell)
{
    if (users_len >= MAX_USERS) {
        return;
    }
    struct user_record *u = &users[users_len++];
    memset(u, 0, sizeof(*u));
    strncpy(u->name, name, USER_NAME_MAX);
    u->uid = uid;
    u->gid = gid;
    strncpy(u->home, home, USER_HOME_MAX);
    strncpy(u->shell, shell, USER_SHELL_MAX);
    u->salt = make_salt(name, uid);
    u->password_set = false;
}

static void parse_passwd_line(char *line)
{
    char *fields[7] = { 0 };
    int field = 0;
    fields[field++] = line;
    for (char *p = line; *p && field < 7; p++) {
        if (*p == ':') {
            *p = '\0';
            fields[field++] = p + 1;
        }
    }
    if (field >= 7) {
        add_record(fields[0], (uint32_t)atoi(fields[2]), (uint32_t)atoi(fields[3]),
                   fields[5], fields[6]);
    }
}

static void rebuild_passwd(void)
{
    char buf[2048];
    buf[0] = '\0';
    for (size_t i = 0; i < users_len; i++) {
        char line[256];
        ksnprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:%s\n",
                  users[i].name, users[i].uid, users[i].gid, users[i].name,
                  users[i].home, users[i].shell);
        if (strlen(buf) + strlen(line) < sizeof(buf)) {
            strcat(buf, line);
        }
    }
    vfs_write_file("/etc/passwd", "/", buf, strlen(buf));
}

static void rebuild_group(void)
{
    char buf[2048];
    buf[0] = '\0';
    strcat(buf, "root:x:0:\nwheel:x:10:root\n");
    for (size_t i = 0; i < users_len; i++) {
        if (users[i].uid == 0) {
            continue;
        }
        char line[128];
        ksnprintf(line, sizeof(line), "%s:x:%u:%s\n",
                  users[i].name, users[i].gid, users[i].name);
        if (strlen(buf) + strlen(line) < sizeof(buf)) {
            strcat(buf, line);
        }
    }
    vfs_write_file("/etc/group", "/", buf, strlen(buf));
}

static void rebuild_shadow(void)
{
    char buf[2048];
    buf[0] = '\0';
    for (size_t i = 0; i < users_len; i++) {
        char line[192];
        if (users[i].password_set) {
            ksnprintf(line, sizeof(line), "%s:%s$%08x$%016llx:0:0:99999:7:::\n",
                      users[i].name, USER_HASH_SCHEME, users[i].salt,
                      users[i].password_hash);
        } else {
            ksnprintf(line, sizeof(line), "%s::0:0:99999:7:::\n", users[i].name);
        }
        if (strlen(buf) + strlen(line) < sizeof(buf)) {
            strcat(buf, line);
        }
    }
    vfs_write_file("/etc/shadow", "/", buf, strlen(buf));
    vfs_chmod("/etc/shadow", "/", 0600);
    vfs_chown("/etc/shadow", "/", 0, 0);
}

static void parse_shadow_line(char *line)
{
    char *fields[2] = { 0 };
    fields[0] = line;
    for (char *p = line; *p; p++) {
        if (*p == ':') {
            *p = '\0';
            fields[1] = p + 1;
            break;
        }
    }
    if (!fields[0] || !fields[1] || fields[1][0] == '\0') {
        return;
    }
    struct user_record *u = NULL;
    for (size_t i = 0; i < users_len; i++) {
        if (strcmp(users[i].name, fields[0]) == 0) {
            u = &users[i];
            break;
        }
    }
    if (!u || strncmp(fields[1], USER_HASH_SCHEME "$", 5) != 0) {
        return;
    }

    char *salt = fields[1] + 5;
    char *hash = strchr(salt, '$');
    if (!hash) {
        return;
    }
    *hash++ = '\0';
    u->salt = (uint32_t)parse_hex64(salt);
    u->password_hash = parse_hex64(hash);
    u->password_set = true;
}

static void parse_shadow(void)
{
    struct vfs_node *shadow = vfs_lookup("/etc/shadow", "/");
    if (!shadow || !shadow->data) {
        return;
    }
    char copy[2048];
    size_t n = shadow->size < sizeof(copy) - 1 ? (size_t)shadow->size : sizeof(copy) - 1;
    memcpy(copy, shadow->data, n);
    copy[n] = '\0';
    char *line = copy;
    for (char *p = copy; ; p++) {
        if (*p == '\n' || *p == '\0') {
            char old = *p;
            *p = '\0';
            if (line[0]) {
                parse_shadow_line(line);
            }
            if (old == '\0') {
                break;
            }
            line = p + 1;
        }
    }
}

void users_init(void)
{
    users_len = 0;
    current_user = 0;

    struct vfs_node *passwd = vfs_lookup("/etc/passwd", "/");
    if (passwd && passwd->data) {
        char copy[2048];
        size_t n = passwd->size < sizeof(copy) - 1 ? (size_t)passwd->size : sizeof(copy) - 1;
        memcpy(copy, passwd->data, n);
        copy[n] = '\0';
        char *line = copy;
        for (char *p = copy; ; p++) {
            if (*p == '\n' || *p == '\0') {
                char old = *p;
                *p = '\0';
                if (line[0]) {
                    parse_passwd_line(line);
                }
                if (old == '\0') {
                    break;
                }
                line = p + 1;
            }
        }
    }

    if (users_len == 0) {
        add_record("root", 0, 0, "/root", "/bin/tsh");
        rebuild_passwd();
    }

    if (!user_find_name("root")) {
        users_len = 0;
        add_record("root", 0, 0, "/root", "/bin/tsh");
        rebuild_passwd();
    }
    parse_shadow();
    rebuild_group();
    rebuild_shadow();
    vfs_mkdir("/root", "/", VFS_S_IFDIR | 0700, 0, 0);
    vfs_chmod("/root", "/", 0700);
    vfs_chown("/root", "/", 0, 0);
}

const struct user_record *user_current(void)
{
    return &users[current_user];
}

int user_login(const char *name)
{
    for (size_t i = 0; i < users_len; i++) {
        if (strcmp(users[i].name, name) == 0) {
            current_user = i;
            return 0;
        }
    }
    return -1;
}

int user_login_password(const char *name, const char *password)
{
    if (!user_check_password(name, password)) {
        return -1;
    }
    return user_login(name);
}

const struct user_record *user_find_name(const char *name)
{
    for (size_t i = 0; i < users_len; i++) {
        if (strcmp(users[i].name, name) == 0) {
            return &users[i];
        }
    }
    return NULL;
}

const struct user_record *user_find_uid(uint32_t uid)
{
    for (size_t i = 0; i < users_len; i++) {
        if (users[i].uid == uid) {
            return &users[i];
        }
    }
    return NULL;
}

int user_add(const char *name, uint32_t uid, uint32_t gid)
{
    if (!name || user_find_name(name) || users_len >= MAX_USERS) {
        return -1;
    }
    char home[USER_HOME_MAX + 1];
    ksnprintf(home, sizeof(home), "/home/%s", name);
    add_record(name, uid, gid, home, "/bin/tsh");
    vfs_mkdir(home, "/", VFS_S_IFDIR | 0700, uid, gid);
    vfs_chmod(home, "/", 0700);
    vfs_chown(home, "/", uid, gid);
    rebuild_passwd();
    rebuild_group();
    rebuild_shadow();
    return 0;
}

int user_del(const char *name)
{
    for (size_t i = 0; i < users_len; i++) {
        if (strcmp(users[i].name, name) == 0 && users[i].uid != 0) {
            memmove(&users[i], &users[i + 1], (users_len - i - 1) * sizeof(users[0]));
            users_len--;
            if (current_user >= users_len) {
                current_user = 0;
            }
            rebuild_passwd();
            rebuild_group();
            rebuild_shadow();
            return 0;
        }
    }
    return -1;
}

int user_set_password(const char *name, const char *password)
{
    if (!name || !password) {
        return -1;
    }
    for (size_t i = 0; i < users_len; i++) {
        if (strcmp(users[i].name, name) == 0) {
            users[i].salt = make_salt(name, users[i].uid);
            /* An empty passwd input intentionally clears the password. */
            if (password[0] == '\0') {
                users[i].password_hash = 0;
                users[i].password_set = false;
            } else {
                users[i].salt ^= (uint32_t)strlen(password);
                users[i].password_hash = hash_password(users[i].salt, password);
                users[i].password_set = true;
            }
            rebuild_shadow();
            return 0;
        }
    }
    return -1;
}

bool user_check_password(const char *name, const char *password)
{
    const struct user_record *u = user_find_name(name);
    if (!u) {
        return false;
    }
    if (!u->password_set) {
        return true;
    }
    return hash_password(u->salt, password) == u->password_hash;
}

bool user_has_password(const char *name)
{
    const struct user_record *u = user_find_name(name);
    return u && u->password_set;
}

size_t user_count(void)
{
    return users_len;
}

const struct user_record *user_get(size_t index)
{
    return index < users_len ? &users[index] : NULL;
}
