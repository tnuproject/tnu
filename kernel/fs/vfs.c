#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/tfs.h>
#include <tnu/vfs.h>

#define VFS_MAX_NODES 1024
#define VFS_MAX_COMPONENTS 32

static struct vfs_node nodes[VFS_MAX_NODES];
static size_t node_count;
static struct vfs_node *root_node;

static uint64_t vfs_clock;

static struct vfs_node *alloc_node(const char *name, enum vfs_node_type type,
                                   uint32_t mode, uint32_t uid, uint32_t gid)
{
    if (node_count >= VFS_MAX_NODES) {
        return NULL;
    }
    struct vfs_node *node = &nodes[node_count++];
    memset(node, 0, sizeof(*node));
    strncpy(node->name, name, VFS_NAME_MAX);
    node->type = type;
    node->mode = mode;
    node->uid = uid;
    node->gid = gid;
    node->created = ++vfs_clock;
    node->modified = node->created;
    return node;
}

static void attach_child(struct vfs_node *parent, struct vfs_node *child)
{
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

static void release_node_data(struct vfs_node *node)
{
    if (node && node->data && !node->data_borrowed) {
        kfree(node->data);
    }
}

static bool node_is_runtime_fs(struct vfs_node *node)
{
    for (struct vfs_node *n = node; n; n = n->parent) {
        if (n->type == VFS_NODE_PROC || n->type == VFS_NODE_DEV) {
            return true;
        }
        bool volatile_name =
            strcmp(n->name, "proc") == 0 ||
            strcmp(n->name, "dev") == 0 ||
            strcmp(n->name, "sys") == 0 ||
            strcmp(n->name, "run") == 0 ||
            strcmp(n->name, "tmp") == 0;
        if (n->parent == root_node &&
            volatile_name) {
            return true;
        }
        if (volatile_name &&
            n->parent && strcmp(n->parent->name, "linux") == 0 &&
            n->parent->parent && strcmp(n->parent->parent->name, "usr") == 0 &&
            n->parent->parent->parent == root_node) {
            return true;
        }
    }
    return false;
}

static void sync_if_persistent_node(struct vfs_node *node)
{
    if (!node_is_runtime_fs(node)) {
        tfs_sync_if_mounted();
    }
}

static struct vfs_node *find_child(struct vfs_node *parent, const char *name)
{
    for (struct vfs_node *n = parent->first_child; n; n = n->next_sibling) {
        if (strcmp(n->name, name) == 0) {
            return n;
        }
    }
    return NULL;
}

int vfs_normalize(const char *path, const char *cwd, char *out, size_t out_size)
{
    char source[VFS_PATH_MAX];
    char comps[VFS_MAX_COMPONENTS][VFS_NAME_MAX + 1];
    size_t comp_count = 0;

    if (!path || !out || out_size == 0) {
        return -1;
    }
    if (path[0] == '/') {
        strncpy(source, path, sizeof(source) - 1);
    } else {
        ksnprintf(source, sizeof(source), "%s/%s", cwd && cwd[0] ? cwd : "/", path);
    }
    source[sizeof(source) - 1] = '\0';

    const char *p = source;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        char comp[VFS_NAME_MAX + 1];
        size_t len = 0;
        while (*p && *p != '/' && len < VFS_NAME_MAX) {
            comp[len++] = *p++;
        }
        while (*p && *p != '/') {
            p++;
        }
        comp[len] = '\0';
        if (strcmp(comp, ".") == 0 || comp[0] == '\0') {
            continue;
        }
        if (strcmp(comp, "..") == 0) {
            if (comp_count) {
                comp_count--;
            }
            continue;
        }
        if (comp_count >= VFS_MAX_COMPONENTS) {
            return -1;
        }
        strcpy(comps[comp_count++], comp);
    }

    if (out_size < 2) {
        return -1;
    }
    out[0] = '/';
    out[1] = '\0';
    for (size_t i = 0; i < comp_count; i++) {
        if (strlen(out) + strlen(comps[i]) + 2 > out_size) {
            return -1;
        }
        if (strcmp(out, "/") != 0) {
            strcat(out, "/");
        }
        strcat(out, comps[i]);
    }
    return 0;
}

void vfs_init(void)
{
    node_count = 0;
    vfs_clock = 0;
    root_node = alloc_node("", VFS_NODE_DIR, VFS_S_IFDIR | 0755, 0, 0);
    static const struct {
        const char *path;
        uint32_t mode;
    } dirs[] = {
        { "/bin", 0755 },  { "/sbin", 0755 }, { "/etc", 0755 },
        { "/dev", 0755 },  { "/proc", 0755 }, { "/home", 0755 },
        { "/root", 0700 }, { "/tmp", 01777 }, { "/usr", 0755 },
        { "/var", 0755 },
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        vfs_mkdir(dirs[i].path, "/", VFS_S_IFDIR | dirs[i].mode, 0, 0);
    }
    log_info("vfs", "created in-memory root filesystem");
}

struct vfs_node *vfs_root(void)
{
    return root_node;
}

struct vfs_node *vfs_lookup(const char *path, const char *cwd)
{
    char normal[VFS_PATH_MAX];
    if (vfs_normalize(path, cwd, normal, sizeof(normal)) < 0) {
        return NULL;
    }
    if (strcmp(normal, "/") == 0) {
        return root_node;
    }

    struct vfs_node *node = root_node;
    const char *p = normal + 1;
    while (*p && node) {
        char comp[VFS_NAME_MAX + 1];
        size_t len = 0;
        while (*p && *p != '/' && len < VFS_NAME_MAX) {
            comp[len++] = *p++;
        }
        comp[len] = '\0';
        while (*p == '/') {
            p++;
        }
        node = find_child(node, comp);
    }
    return node;
}

static int parent_and_name(const char *path, const char *cwd,
                           struct vfs_node **parent, char *name)
{
    char normal[VFS_PATH_MAX];
    if (vfs_normalize(path, cwd, normal, sizeof(normal)) < 0 || strcmp(normal, "/") == 0) {
        return -1;
    }
    char *slash = strrchr(normal, '/');
    if (!slash) {
        return -1;
    }
    strncpy(name, slash + 1, VFS_NAME_MAX);
    name[VFS_NAME_MAX] = '\0';
    if (slash == normal) {
        *parent = root_node;
    } else {
        *slash = '\0';
        *parent = vfs_lookup(normal, "/");
    }
    if (!*parent || (*parent)->type != VFS_NODE_DIR || name[0] == '\0') {
        return -1;
    }
    return 0;
}

int vfs_mkdir(const char *path, const char *cwd, uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct vfs_node *parent;
    char name[VFS_NAME_MAX + 1];
    if (parent_and_name(path, cwd, &parent, name) < 0) {
        return -1;
    }
    if (find_child(parent, name)) {
        return -2;
    }
    struct vfs_node *node = alloc_node(name, VFS_NODE_DIR, mode, uid, gid);
    if (!node) {
        return -3;
    }
    attach_child(parent, node);
    sync_if_persistent_node(node);
    return 0;
}

int vfs_create_file(const char *path, const char *cwd, uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct vfs_node *parent;
    char name[VFS_NAME_MAX + 1];
    if (parent_and_name(path, cwd, &parent, name) < 0) {
        return -1;
    }
    if (find_child(parent, name)) {
        return -2;
    }
    struct vfs_node *node = alloc_node(name, VFS_NODE_FILE, mode, uid, gid);
    if (!node) {
        return -3;
    }
    attach_child(parent, node);
    sync_if_persistent_node(node);
    return 0;
}

int vfs_write_file(const char *path, const char *cwd, const void *data, size_t size)
{
    struct vfs_node *node = vfs_lookup(path, cwd);
    if (!node) {
        int rc = vfs_create_file(path, cwd, VFS_S_IFREG | 0644, 0, 0);
        if (rc < 0) {
            return rc;
        }
        node = vfs_lookup(path, cwd);
    }
    if (!node || (node->type != VFS_NODE_FILE && node->type != VFS_NODE_PROC)) {
        return -1;
    }
    uint8_t *copy = NULL;
    if (size) {
        copy = kmalloc(size + 1);
        if (!copy) {
            return -2;
        }
        memcpy(copy, data, size);
        copy[size] = 0;
    }
    release_node_data(node);
    node->data = copy;
    node->data_borrowed = false;
    node->size = size;
    node->modified = ++vfs_clock;
    sync_if_persistent_node(node);
    return 0;
}

int vfs_unlink(const char *path, const char *cwd)
{
    struct vfs_node *parent;
    char name[VFS_NAME_MAX + 1];
    if (parent_and_name(path, cwd, &parent, name) < 0) {
        return -1;
    }
    struct vfs_node **link = &parent->first_child;
    while (*link) {
        if (strcmp((*link)->name, name) == 0) {
            if ((*link)->first_child) {
                return -2;
            }
            release_node_data(*link);
            *link = (*link)->next_sibling;
            parent->modified = ++vfs_clock;
            sync_if_persistent_node(parent);
            return 0;
        }
        link = &(*link)->next_sibling;
    }
    return -1;
}

int vfs_chmod(const char *path, const char *cwd, uint32_t mode)
{
    struct vfs_node *node = vfs_lookup(path, cwd);
    if (!node) {
        return -1;
    }
    node->mode = (node->mode & 0170000) | (mode & 07777);
    node->modified = ++vfs_clock;
    sync_if_persistent_node(node);
    return 0;
}

int vfs_chown(const char *path, const char *cwd, uint32_t uid, uint32_t gid)
{
    struct vfs_node *node = vfs_lookup(path, cwd);
    if (!node) {
        return -1;
    }
    node->uid = uid;
    node->gid = gid;
    node->modified = ++vfs_clock;
    sync_if_persistent_node(node);
    return 0;
}

int vfs_stat(const char *path, const char *cwd, struct vfs_stat *st)
{
    struct vfs_node *node = vfs_lookup(path, cwd);
    if (!node || !st) {
        return -1;
    }
    st->mode = node->mode;
    st->uid = node->uid;
    st->gid = node->gid;
    st->size = node->size;
    st->modified = node->modified;
    st->type = node->type;
    return 0;
}

ssize_t vfs_read_node(struct vfs_node *node, uint64_t offset, void *buf, size_t count)
{
    if (!node || !buf || node->type == VFS_NODE_DIR) {
        return -1;
    }
    if (offset >= node->size) {
        return 0;
    }
    size_t available = (size_t)(node->size - offset);
    if (count > available) {
        count = available;
    }
    memcpy(buf, node->data + offset, count);
    return (ssize_t)count;
}

ssize_t vfs_write_node(struct vfs_node *node, uint64_t offset, const void *buf, size_t count)
{
    if (!node || !buf || node->type != VFS_NODE_FILE) {
        return -1;
    }
    uint64_t end = offset + count;
    if (end > node->size || node->data_borrowed) {
        uint8_t *old_data = node->data;
        bool old_borrowed = node->data_borrowed;
        uint64_t new_size = end > node->size ? end : node->size;
        uint8_t *new_data = kmalloc((size_t)new_size + 1);
        if (!new_data) {
            return -1;
        }
        if (node->data && node->size) {
            memcpy(new_data, node->data, (size_t)node->size);
        }
        if (new_size > node->size) {
            memset(new_data + node->size, 0, (size_t)(end - node->size));
        }
        node->data = new_data;
        node->size = new_size;
        node->data[node->size] = 0;
        node->data_borrowed = false;
        if (old_data && !old_borrowed) {
            kfree(old_data);
        }
    }
    memcpy(node->data + offset, buf, count);
    node->modified = ++vfs_clock;
    sync_if_persistent_node(node);
    return (ssize_t)count;
}

void vfs_list(struct vfs_node *dir, void (*emit)(struct vfs_node *node, void *ctx), void *ctx)
{
    if (!dir || dir->type != VFS_NODE_DIR || !emit) {
        return;
    }
    for (struct vfs_node *n = dir->first_child; n; n = n->next_sibling) {
        emit(n, ctx);
    }
}
