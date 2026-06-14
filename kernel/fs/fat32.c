#include <tnu/fs_probe.h>
#include <tnu/string.h>

#define FAT32_EOC 0x0ffffff8u

struct fat32_bpb {
    uint8_t jump[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} __attribute__((packed));

struct fat32_fsinfo {
    uint32_t lead_sig;
    uint8_t reserved1[480];
    uint32_t struct_sig;
    uint32_t free_count;
    uint32_t next_free;
    uint8_t reserved2[12];
    uint32_t trail_sig;
} __attribute__((packed));

static void copy_label(char out[32], const char *label, size_t len)
{
    size_t n = 0;
    while (n < len && n + 1 < 32 && label[n] && label[n] != ' ') out[n++] = label[n];
    out[n] = '\0';
}

static void write_fat_label(char out[11], const char *label)
{
    memset(out, ' ', 11);
    if (!label) label = "TIRAMISU";
    for (size_t i = 0; i < 11 && label[i]; i++) out[i] = label[i];
}

int fat32_probe(const void *image, size_t size, struct fs_probe_result *out)
{
    if (!image || !out || size < sizeof(struct fat32_bpb)) return -1;
    const struct fat32_bpb *bpb = image;
    const uint8_t *bytes = image;
    if (size < 512 || bytes[510] != 0x55 || bytes[511] != 0xaa) return -1;
    if (bpb->bytes_per_sector != 512 && bpb->bytes_per_sector != 1024 &&
        bpb->bytes_per_sector != 2048 && bpb->bytes_per_sector != 4096) return -1;
    if (bpb->sectors_per_cluster == 0 || bpb->fat_count == 0 ||
        bpb->fat_size_32 == 0 || bpb->root_cluster < 2) return -1;
    if (bpb->root_entry_count != 0 || bpb->fat_size_16 != 0) return -1;

    uint64_t sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    if (sectors == 0 || sectors * (uint64_t)bpb->bytes_per_sector > size) return -1;

    memset(out, 0, sizeof(*out));
    out->name = "fat32";
    out->valid = true;
    out->block_size = bpb->bytes_per_sector;
    out->total_blocks = sectors;
    out->total_bytes = sectors * (uint64_t)bpb->bytes_per_sector;
    out->root_inode = bpb->root_cluster;
    copy_label(out->label, bpb->volume_label, sizeof(bpb->volume_label));
    return 0;
}

/* Minimal FAT32 formatter for EFI System Partition creation by sysinstall.
 * It creates a valid empty FAT32 volume with FSInfo, backup boot sector,
 * two FATs, and root directory cluster 2 allocated.
 */
int fat32_mkfs(void *image, size_t size, const char *label)
{
    if (!image || size < 32 * 1024 * 1024) return -1;
    memset(image, 0, size);

    const uint32_t bytes_per_sector = 512;
    const uint32_t total_sectors = (uint32_t)(size / bytes_per_sector);
    uint8_t sectors_per_cluster = 1;
    if (total_sectors > 262144) sectors_per_cluster = 8;
    else if (total_sectors > 65536) sectors_per_cluster = 4;
    else sectors_per_cluster = 1;

    const uint16_t reserved = 32;
    const uint8_t fats = 2;
    uint32_t clusters = total_sectors / sectors_per_cluster;
    uint32_t fat_sectors = ((clusters + 2) * 4 + bytes_per_sector - 1) / bytes_per_sector;
    clusters = (total_sectors - reserved - fats * fat_sectors) / sectors_per_cluster;
    fat_sectors = ((clusters + 2) * 4 + bytes_per_sector - 1) / bytes_per_sector;
    if (clusters < 65525) return -1;

    struct fat32_bpb *bpb = (struct fat32_bpb *)image;
    bpb->jump[0] = 0xeb; bpb->jump[1] = 0x58; bpb->jump[2] = 0x90;
    memcpy(bpb->oem, "TIRAMISU", 8);
    bpb->bytes_per_sector = bytes_per_sector;
    bpb->sectors_per_cluster = sectors_per_cluster;
    bpb->reserved_sector_count = reserved;
    bpb->fat_count = fats;
    bpb->root_entry_count = 0;
    bpb->media = 0xf8;
    bpb->fat_size_16 = 0;
    bpb->sectors_per_track = 63;
    bpb->head_count = 255;
    bpb->total_sectors_32 = total_sectors;
    bpb->fat_size_32 = fat_sectors;
    bpb->root_cluster = 2;
    bpb->fs_info = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_signature = 0x29;
    bpb->volume_id = 0x544e5531;
    write_fat_label(bpb->volume_label, label ? label : "TIRAMISU");
    memcpy(bpb->fs_type, "FAT32   ", 8);
    ((uint8_t *)image)[510] = 0x55;
    ((uint8_t *)image)[511] = 0xaa;

    struct fat32_fsinfo *fsi = (struct fat32_fsinfo *)((uint8_t *)image + 512);
    fsi->lead_sig = 0x41615252;
    fsi->struct_sig = 0x61417272;
    fsi->free_count = clusters - 1;
    fsi->next_free = 3;
    fsi->trail_sig = 0xaa550000;

    memcpy((uint8_t *)image + 6 * 512, image, 512);
    memcpy((uint8_t *)image + 7 * 512, fsi, 512);

    for (uint8_t f = 0; f < fats; f++) {
        uint32_t *fat = (uint32_t *)((uint8_t *)image + (reserved + f * fat_sectors) * 512);
        fat[0] = 0x0ffffff8;
        fat[1] = 0x0fffffff;
        fat[2] = FAT32_EOC;
    }
    return 0;
}
