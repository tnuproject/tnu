#ifndef TNU_VFS_H
#define TNU_VFS_H

#include <tnu/types.h>

#define VFS_NAME_MAX 63
#define VFS_PATH_MAX 256
#define VFS_MAX_FDS 32

#define VFS_O_RDONLY 0x0
#define VFS_O_WRONLY 0x1
#define VFS_O_RDWR   0x2
#define VFS_O_CREAT  0x40
#define VFS_O_TRUNC  0x200
#define VFS_O_APPEND 0x400
#define VFS_O_EXCL   0x800

#define VFS_S_IFDIR  0040000
#define VFS_S_IFREG  0100000
#define VFS_S_IFDEV  0020000
#define VFS_S_IFPROC 0010000

enum vfs_node_type {
    VFS_NODE_DIR = 1,
    VFS_NODE_FILE = 2,
    VFS_NODE_DEV = 3,
    VFS_NODE_PROC = 4,
};

struct vfs_node {
    char name[VFS_NAME_MAX + 1];
    enum vfs_node_type type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint8_t *data;
    bool data_borrowed;
    struct vfs_node *parent;
    struct vfs_node *first_child;
    struct vfs_node *next_sibling;
};

struct vfs_stat {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t modified;
    enum vfs_node_type type;
};

struct file_descriptor {
    bool used;
    int flags;
    uint64_t offset;
    struct vfs_node *node;
};

void vfs_init(void);
struct vfs_node *vfs_root(void);
struct vfs_node *vfs_lookup(const char *path, const char *cwd);
int vfs_normalize(const char *path, const char *cwd, char *out, size_t out_size);
int vfs_mkdir(const char *path, const char *cwd, uint32_t mode, uint32_t uid, uint32_t gid);
int vfs_create_file(const char *path, const char *cwd, uint32_t mode, uint32_t uid, uint32_t gid);
int vfs_write_file(const char *path, const char *cwd, const void *data, size_t size);
int vfs_unlink(const char *path, const char *cwd);
int vfs_chmod(const char *path, const char *cwd, uint32_t mode);
int vfs_chown(const char *path, const char *cwd, uint32_t uid, uint32_t gid);
int vfs_stat(const char *path, const char *cwd, struct vfs_stat *st);
ssize_t vfs_read_node(struct vfs_node *node, uint64_t offset, void *buf, size_t count);
ssize_t vfs_write_node(struct vfs_node *node, uint64_t offset, const void *buf, size_t count);
void vfs_list(struct vfs_node *dir, void (*emit)(struct vfs_node *node, void *ctx), void *ctx);

#endif
