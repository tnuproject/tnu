#include <tnu/fs_probe.h>
#include <tnu/string.h>

#define EXT4_SUPER_MAGIC 0xef53
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040u
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080u
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE 0x0008u

struct ext4_superblock {
    uint32_t inodes_count;
    uint32_t blocks_count_lo;
    uint32_t reserved_blocks_count_lo;
    uint32_t free_blocks_count_lo;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_cluster_size;
    uint32_t blocks_per_group;
    uint32_t clusters_per_group;
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
    char last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;
    uint8_t journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;
    uint32_t hash_seed[4];
    uint8_t def_hash_version;
    uint8_t jnl_backup_type;
    uint16_t desc_size;
    uint32_t default_mount_opts;
    uint32_t first_meta_bg;
    uint32_t mkfs_time;
    uint32_t journal_blocks[17];
    uint32_t blocks_count_hi;
    uint32_t reserved_blocks_count_hi;
    uint32_t free_blocks_count_hi;
} __attribute__((packed));

static void copy_label(char out[32], const char *label, size_t len)
{
    size_t n = 0;
    while (n < len && n + 1 < 32 && label[n]) out[n++] = label[n];
    out[n] = '\0';
}

int ext4_probe(const void *image, size_t size, struct fs_probe_result *out)
{
    if (!image || !out || size < 1024 + sizeof(struct ext4_superblock)) return -1;
    const struct ext4_superblock *sb = (const struct ext4_superblock *)((const uint8_t *)image + 1024);
    if (sb->magic != EXT4_SUPER_MAGIC) return -1;
    if (sb->log_block_size > 6 || sb->blocks_count_lo == 0) return -1;

    bool extents = (sb->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) != 0;
    bool huge_file = (sb->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) != 0;
    bool sixtyfour = (sb->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
    bool journal = (sb->feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) != 0;
    bool is_ext4 = extents || huge_file || sixtyfour || journal || sb->blocks_count_hi != 0 || sb->desc_size > 32;
    if (!is_ext4) return -1;

    uint64_t blocks = ((uint64_t)sb->blocks_count_hi << 32) | sb->blocks_count_lo;
    uint64_t block_size = 1024ull << sb->log_block_size;
    uint64_t total_bytes = block_size * blocks;
    if (total_bytes > size) return -1;

    memset(out, 0, sizeof(*out));
    out->name = "ext4";
    out->valid = true;
    out->block_size = block_size;
    out->total_blocks = blocks;
    out->total_bytes = total_bytes;
    out->root_inode = 2;
    copy_label(out->label, sb->volume_name, sizeof(sb->volume_name));
    return 0;
}

/* Policy helper for the VFS layer: safe ext4 support is read-only until the
 * kernel has a journal replay/checkpoint implementation. Return 0 only for
 * ext4 images that can be mounted without pretending journal writes work.
 */
int ext4_can_mount_rw(const void *image, size_t size)
{
    if (!image || size < 1024 + sizeof(struct ext4_superblock)) return 0;
    const struct ext4_superblock *sb = (const struct ext4_superblock *)((const uint8_t *)image + 1024);
    if (sb->magic != EXT4_SUPER_MAGIC) return 0;
    if (sb->feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) return 0;
    return 1;
}
