#include <tnu/fs_probe.h>
#include <tnu/string.h>

struct ext2_superblock {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t reserved_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
} __attribute__((packed));

static void copy_label(char out[32], const char *label, size_t len)
{
    size_t n = 0;
    while (n < len && n + 1 < 32 && label[n]) {
        out[n] = label[n];
        n++;
    }
    out[n] = '\0';
}

int ext2_probe(const void *image, size_t size, struct fs_probe_result *out)
{
    if (!image || !out || size < 1024 + sizeof(struct ext2_superblock)) {
        return -1;
    }
    const struct ext2_superblock *sb =
        (const struct ext2_superblock *)((const uint8_t *)image + 1024);
    if (sb->magic != 0xef53) {
        return -1;
    }
    uint64_t block_size = 1024ull << sb->log_block_size;
    memset(out, 0, sizeof(*out));
    out->name = "ext2";
    out->valid = true;
    out->block_size = block_size;
    out->total_blocks = sb->blocks_count;
    out->total_bytes = block_size * (uint64_t)sb->blocks_count;
    out->root_inode = 2;
    copy_label(out->label, sb->volume_name, sizeof(sb->volume_name));
    return 0;
}
