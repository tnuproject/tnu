#include <tnu/fs_probe.h>
#include <tnu/string.h>

#define EXT2_SUPER_MAGIC 0xef53
#define EXT2_ROOT_INO 2
#define EXT2_GOOD_OLD_FIRST_INO 11
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_NDIR_BLOCKS 12
#define EXT2_NAME_LEN 255
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR 2
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u

struct ext2_superblock {
    uint32_t inodes_count, blocks_count, reserved_blocks_count, free_blocks_count;
    uint32_t free_inodes_count, first_data_block, log_block_size, log_frag_size;
    uint32_t blocks_per_group, frags_per_group, inodes_per_group;
    uint32_t mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev_level;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;
    uint16_t inode_size, block_group_nr;
    uint32_t feature_compat, feature_incompat, feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t reserved[12];
} __attribute__((packed));

struct ext2_inode {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links_count;
    uint32_t blocks, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, dir_acl, faddr;
    uint8_t osd2[12];
} __attribute__((packed));

struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
} __attribute__((packed));

static uint32_t div_round_up_u32(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

static void copy_label(char out[32], const char *label, size_t len)
{
    size_t n = 0;
    while (n < len && n + 1 < 32 && label[n]) out[n++] = label[n];
    out[n] = '\0';
}

static void write_label(char dst[16], const char *label)
{
    memset(dst, 0, 16);
    if (!label) return;
    for (size_t i = 0; i < 16 && label[i]; i++) dst[i] = label[i];
}

static void bitmap_set(uint8_t *bitmap, uint32_t index0)
{
    bitmap[index0 / 8] |= (uint8_t)(1u << (index0 % 8));
}

int ext2_probe(const void *image, size_t size, struct fs_probe_result *out)
{
    if (!image || !out || size < 1024 + sizeof(struct ext2_superblock)) return -1;
    const struct ext2_superblock *sb = (const struct ext2_superblock *)((const uint8_t *)image + 1024);
    if (sb->magic != EXT2_SUPER_MAGIC) return -1;
    if (sb->log_block_size > 6 || sb->blocks_count == 0 || sb->inodes_count == 0) return -1;
    if (sb->feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) return -1;
    if (sb->feature_incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE) return -1;

    uint64_t block_size = 1024ull << sb->log_block_size;
    uint64_t total_bytes = block_size * (uint64_t)sb->blocks_count;
    if (total_bytes > size) return -1;

    memset(out, 0, sizeof(*out));
    out->name = "ext2";
    out->valid = true;
    out->block_size = block_size;
    out->total_blocks = sb->blocks_count;
    out->total_bytes = total_bytes;
    out->root_inode = EXT2_ROOT_INO;
    copy_label(out->label, sb->volume_name, sizeof(sb->volume_name));
    return 0;
}

/* Minimal ext2 formatter for sysinstall/root partition bootstrap.
 * Creates a clean one-block-group ext2 image with an empty root directory.
 * It is intentionally simple: 1 KiB blocks, 128-byte inodes, no journal/extents.
 */
int ext2_mkfs(void *image, size_t size, const char *label)
{
    if (!image || size < 4 * 1024 * 1024) return -1;
    memset(image, 0, size);

    const uint32_t block_size = 1024;
    uint32_t blocks = (uint32_t)(size / block_size);
    if (blocks > block_size * 8) {
        blocks = block_size * 8;
    }
    const uint32_t inodes = 1024;
    const uint32_t inode_table_blocks = div_round_up_u32(inodes * EXT2_GOOD_OLD_INODE_SIZE, block_size);
    const uint32_t first_data_block __attribute__((unused)) = 1;
    const uint32_t super_block __attribute__((unused)) = 1;
    const uint32_t gd_block = 2;
    const uint32_t block_bitmap_block = 3;
    const uint32_t inode_bitmap_block = 4;
    const uint32_t inode_table_block = 5;
    const uint32_t root_dir_block = inode_table_block + inode_table_blocks;
    const uint32_t first_free_block = root_dir_block + 1;
    if (first_free_block >= blocks) return -1;

    struct ext2_superblock *sb = (struct ext2_superblock *)((uint8_t *)image + 1024);
    sb->inodes_count = inodes;
    sb->blocks_count = blocks;
    sb->reserved_blocks_count = blocks / 20;
    sb->free_blocks_count = blocks - first_free_block;
    sb->free_inodes_count = inodes - EXT2_GOOD_OLD_FIRST_INO;
    sb->first_data_block = first_data_block;
    sb->log_block_size = 0;
    sb->log_frag_size = 0;
    sb->blocks_per_group = blocks;
    sb->frags_per_group = blocks;
    sb->inodes_per_group = inodes;
    sb->mnt_count = 0;
    sb->max_mnt_count = 32;
    sb->magic = EXT2_SUPER_MAGIC;
    sb->state = 1;
    sb->errors = 1;
    sb->rev_level = 1;
    sb->first_ino = EXT2_GOOD_OLD_FIRST_INO;
    sb->inode_size = EXT2_GOOD_OLD_INODE_SIZE;
    sb->feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE;
    write_label(sb->volume_name, label ? label : "TIRAMISU");

    struct ext2_group_desc *gd = (struct ext2_group_desc *)((uint8_t *)image + gd_block * block_size);
    gd->block_bitmap = block_bitmap_block;
    gd->inode_bitmap = inode_bitmap_block;
    gd->inode_table = inode_table_block;
    gd->free_blocks_count = (uint16_t)sb->free_blocks_count;
    gd->free_inodes_count = (uint16_t)sb->free_inodes_count;
    gd->used_dirs_count = 1;

    uint8_t *bbm = (uint8_t *)image + block_bitmap_block * block_size;
    for (uint32_t b = 0; b < first_free_block; b++) bitmap_set(bbm, b);
    for (uint32_t b = blocks; b < block_size * 8; b++) bitmap_set(bbm, b);

    uint8_t *ibm = (uint8_t *)image + inode_bitmap_block * block_size;
    for (uint32_t i = 0; i < EXT2_GOOD_OLD_FIRST_INO; i++) bitmap_set(ibm, i);
    for (uint32_t i = inodes; i < block_size * 8; i++) bitmap_set(ibm, i);

    struct ext2_inode *itable = (struct ext2_inode *)((uint8_t *)image + inode_table_block * block_size);
    struct ext2_inode *root = &itable[EXT2_ROOT_INO - 1];
    root->mode = 0040755;
    root->size = block_size;
    root->links_count = 2;
    root->blocks = block_size / 512;
    root->block[0] = root_dir_block;

    uint8_t *dir = (uint8_t *)image + root_dir_block * block_size;
    struct ext2_dirent *dot = (struct ext2_dirent *)dir;
    dot->inode = EXT2_ROOT_INO;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';

    struct ext2_dirent *dotdot = (struct ext2_dirent *)(dir + 12);
    dotdot->inode = EXT2_ROOT_INO;
    dotdot->rec_len = (uint16_t)(block_size - 12);
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    return 0;
}
