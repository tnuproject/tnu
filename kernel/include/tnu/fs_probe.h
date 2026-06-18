#ifndef TNU_FS_PROBE_H
#define TNU_FS_PROBE_H

#include <tnu/types.h>

struct fs_probe_result {
    const char *name;
    bool valid;
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t total_bytes;
    uint64_t root_inode;
    char label[32];
};

int ext2_probe(const void *image, size_t size, struct fs_probe_result *out);
int ext2_mkfs(void *image, size_t size, const char *label);
int ext4_probe(const void *image, size_t size, struct fs_probe_result *out);
int ext4_can_mount_rw(const void *image, size_t size);
int ext4_needs_journal_replay(const void *image, size_t size);
int fat32_probe(const void *image, size_t size, struct fs_probe_result *out);

#endif
