#ifndef TNU_TFS_H
#define TNU_TFS_H

#include <tnu/types.h>

#define TFS_MAGIC "TFS1TNU"
#define TFS_MAGIC_LEN 8
#define TFS_VERSION 1
#define TFS_PATH_MAX 256

enum tfs_entry_type {
    TFS_ENTRY_DIR = 1,
    TFS_ENTRY_FILE = 2,
};

struct tfs_header {
    char magic[TFS_MAGIC_LEN];
    uint32_t version;
    uint32_t entry_count;
    uint64_t entries_offset;
    uint64_t data_offset;
};

struct tfs_entry {
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t offset;
    uint64_t mtime;
    char path[TFS_PATH_MAX];
};

int tfs_mount_image(const void *image, size_t size);

#endif
