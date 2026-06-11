#include <tnu/fs_probe.h>
#include <tnu/string.h>

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

static void copy_label(char out[32], const char *label, size_t len)
{
    size_t n = 0;
    while (n < len && n + 1 < 32 && label[n] && label[n] != ' ') {
        out[n] = label[n];
        n++;
    }
    out[n] = '\0';
}

int fat32_probe(const void *image, size_t size, struct fs_probe_result *out)
{
    if (!image || !out || size < sizeof(struct fat32_bpb)) {
        return -1;
    }
    const struct fat32_bpb *bpb = image;
    const uint8_t *bytes = image;
    if (size < 512 || bytes[510] != 0x55 || bytes[511] != 0xaa) {
        return -1;
    }
    if (bpb->bytes_per_sector == 0 || bpb->sectors_per_cluster == 0 ||
        bpb->fat_count == 0 || bpb->fat_size_32 == 0 || bpb->root_cluster < 2) {
        return -1;
    }
    if (bpb->root_entry_count != 0 || bpb->fat_size_16 != 0) {
        return -1;
    }

    uint64_t sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
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
