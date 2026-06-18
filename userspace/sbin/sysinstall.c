/*
 * sysinstall - Tiramisu Installation Tool
 * Real disk-oriented installer skeleton with selectable root filesystem.
 * Default root filesystem: TFS (Tiramisu File System image written raw from /boot/root.tfs).
 *
 * Supports:
 *  - disk discovery: /dev/sdX (SATA/legacy ATA target; NVMe intentionally disabled here)
 *  - GPT creation with valid ESP + root partitions
 *  - root filesystem choice: tfs, ext2, ext4, fat32
 *  - ESP FAT32 formatting
 *  - TFS raw root image installation
 *  - optional external mkfs/copy hooks for ext2/ext4/fat32 root installs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_DISKS 16
#define SECTOR_SIZE 512ULL
#define GPT_HEADER_SIZE 92
#define GPT_ENTRY_SIZE 128
#define GPT_ENTRY_COUNT 128
#define ESP_START_LBA 2048ULL
#define ESP_SIZE_SECTORS (512ULL * 1024ULL) /* 256 MiB */
#define ROOT_START_LBA (ESP_START_LBA + ESP_SIZE_SECTORS)
#define MIN_DISK_SECTORS (ROOT_START_LBA + 262144ULL)

#define DEFAULT_ROOTFS FS_TFS
#define DEFAULT_ROOTFS_NAME "tfs"
#define DEFAULT_ESP_LABEL "TIRAMISU"
#define DEFAULT_ROOT_LABEL "TIRAMISU"
#define SYSINSTALL_CONFIG_PATH "/etc/sysinstall.conf"
#define ROOT_IMAGE_PATH "/boot/root.tfs"
#define KERNEL_IMAGE_PATH "/boot/kernel.elf"
#define BOOTX64_IMAGE_PATH "/EFI/BOOT/BOOTX64.EFI"

/* GPT type GUIDs, encoded in on-disk little-endian layout. */
static const uint8_t GUID_EFI_SYSTEM[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

static const uint8_t GUID_LINUX_FS[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
};

/* Private Tiramisu root FS type GUID: generated/stable for this project. */
static const uint8_t GUID_TIRAMISU_FS[16] = {
    0x54, 0x4e, 0x55, 0x00, 0x13, 0x37, 0x42, 0x42,
    0x80, 0x86, 0x54, 0x49, 0x52, 0x41, 0x4d, 0x49
};

typedef enum {
    FS_TFS = 0,
    FS_EXT2,
    FS_EXT4,
    FS_FAT32
} root_fs_t;

typedef struct {
    char device[64];
    char model[128];
    uint64_t size_sectors;
    uint64_t size_bytes;
    int is_nvme; /* kept for ABI/printing, but this SATA build does not add NVMe disks */
} disk_info_t;

typedef struct {
    root_fs_t root_fs;
    int root_fs_set;
    char target[64];
    char hostname[64];
    char username[32];
    char esp_label[32];
    char root_label[32];
    int assume_yes;
    int install_esp;
    int install_bootloader;
    int dry_run;
} install_config_t;

typedef struct {
    uint8_t type_guid[16];
    uint8_t partition_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attrs;
    uint16_t name[36]; /* UTF-16LE */
} __attribute__((packed)) gpt_entry_t;

typedef struct {
    uint8_t signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} __attribute__((packed)) gpt_header_t;

static disk_info_t disks[MAX_DISKS];
static int disk_count = 0;
static uint32_t rng_state = 0x544e5501u;

static void config_defaults(install_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->root_fs = DEFAULT_ROOTFS;
    cfg->install_esp = 1;
    cfg->install_bootloader = 1;
    strncpy(cfg->hostname, "tiramisu", sizeof(cfg->hostname) - 1);
    strncpy(cfg->username, "root", sizeof(cfg->username) - 1);
    strncpy(cfg->esp_label, DEFAULT_ESP_LABEL, sizeof(cfg->esp_label) - 1);
    strncpy(cfg->root_label, DEFAULT_ROOT_LABEL, sizeof(cfg->root_label) - 1);
}

static void print_header(void)
{
    printf("\nTiramisu sysinstall\n");
    printf("===================\n\n");
}

static uint32_t pseudo_rand(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 8) ^ rng_state;
}

static void generate_guid(uint8_t *guid)
{
    for (int i = 0; i < 16; i++) {
        guid[i] = (uint8_t)(pseudo_rand() & 0xffu);
    }
    guid[6] = (guid[6] & 0x0f) | 0x40;
    guid[8] = (guid[8] & 0x3f) | 0x80;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t crc32(const void *data, size_t len)
{
    return crc32_update(0, data, len);
}

static void gpt_set_name(gpt_entry_t *e, const char *name)
{
    memset(e->name, 0, sizeof(e->name));
    for (size_t i = 0; name[i] && i < 36; i++) {
        e->name[i] = (uint16_t)(uint8_t)name[i];
    }
}

static const char *root_fs_name(root_fs_t fs)
{
    switch (fs) {
    case FS_TFS: return "tfs";
    case FS_EXT2: return "ext2";
    case FS_EXT4: return "ext4";
    case FS_FAT32: return "fat32";
    default: return "unknown";
    }
}

static int parse_root_fs(const char *name, root_fs_t *out)
{
    if (!name || !out) {
        return -1;
    }
    if (strcmp(name, "tfs") == 0) {
        *out = FS_TFS;
    } else if (strcmp(name, "ext2") == 0) {
        *out = FS_EXT2;
    } else if (strcmp(name, "ext4") == 0) {
        *out = FS_EXT4;
    } else if (strcmp(name, "fat32") == 0) {
        *out = FS_FAT32;
    } else {
        return -1;
    }
    return 0;
}

static const uint8_t *root_fs_guid(root_fs_t fs)
{
    return fs == FS_TFS ? GUID_TIRAMISU_FS : GUID_LINUX_FS;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static int parse_bool_value(const char *value, int fallback)
{
    if (!value) {
        return fallback;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "true") == 0 || strcmp(value, "on") == 0) {
        return 1;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "false") == 0 || strcmp(value, "off") == 0) {
        return 0;
    }
    return fallback;
}

static void config_apply_pair(install_config_t *cfg, const char *key, const char *value)
{
    if (strcmp(key, "rootfs") == 0 || strcmp(key, "fs") == 0) {
        root_fs_t fs;
        if (strcmp(value, "ask") == 0 || strcmp(value, "interactive") == 0) {
            cfg->root_fs_set = 0;
        } else if (parse_root_fs(value, &fs) == 0) {
            cfg->root_fs = fs;
            cfg->root_fs_set = 1;
        }
    } else if (strcmp(key, "target") == 0) {
        strncpy(cfg->target, value, sizeof(cfg->target) - 1);
    } else if (strcmp(key, "hostname") == 0) {
        strncpy(cfg->hostname, value, sizeof(cfg->hostname) - 1);
    } else if (strcmp(key, "user") == 0 || strcmp(key, "username") == 0) {
        strncpy(cfg->username, value, sizeof(cfg->username) - 1);
    } else if (strcmp(key, "esp_label") == 0) {
        strncpy(cfg->esp_label, value, sizeof(cfg->esp_label) - 1);
    } else if (strcmp(key, "root_label") == 0) {
        strncpy(cfg->root_label, value, sizeof(cfg->root_label) - 1);
    } else if (strcmp(key, "confirm") == 0) {
        cfg->assume_yes = !parse_bool_value(value, 1);
    } else if (strcmp(key, "install_esp") == 0) {
        cfg->install_esp = parse_bool_value(value, cfg->install_esp);
    } else if (strcmp(key, "install_bootloader") == 0) {
        cfg->install_bootloader = parse_bool_value(value, cfg->install_bootloader);
    } else if (strcmp(key, "dry_run") == 0) {
        cfg->dry_run = parse_bool_value(value, cfg->dry_run);
    }
}

static int load_config_file(install_config_t *cfg, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';

    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *p = trim(line);
        if (*p && *p != '#') {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq++ = '\0';
                char *key = trim(p);
                char *value = trim(eq);
                config_apply_pair(cfg, key, value);
            }
        }
        line = next;
    }
    return 0;
}

static void print_usage(void)
{
    printf("Usage: sysinstall [options]\n\n");
    printf("Options:\n");
    printf("  --fs tfs|ext2|ext4|fat32\n");
    printf("  --target /dev/sdX\n");
    printf("  --hostname NAME\n");
    printf("  --user NAME\n");
    printf("  --esp-label LABEL\n");
    printf("  --root-label LABEL\n");
    printf("  --yes\n");
    printf("  --no-esp\n");
    printf("  --no-bootloader\n");
    printf("  --dry-run\n");
}

static int apply_cli(install_config_t *cfg, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 1;
        } else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
            cfg->assume_yes = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            cfg->dry_run = 1;
        } else if (strcmp(argv[i], "--no-esp") == 0) {
            cfg->install_esp = 0;
        } else if (strcmp(argv[i], "--no-bootloader") == 0) {
            cfg->install_bootloader = 0;
        } else if ((strcmp(argv[i], "--fs") == 0 || strcmp(argv[i], "--rootfs") == 0) && i + 1 < argc) {
            if (parse_root_fs(argv[++i], &cfg->root_fs) < 0) {
                printf("ERROR: unknown filesystem '%s'.\n", argv[i]);
                return -1;
            }
            cfg->root_fs_set = 1;
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            strncpy(cfg->target, argv[++i], sizeof(cfg->target) - 1);
        } else if (strcmp(argv[i], "--hostname") == 0 && i + 1 < argc) {
            strncpy(cfg->hostname, argv[++i], sizeof(cfg->hostname) - 1);
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            strncpy(cfg->username, argv[++i], sizeof(cfg->username) - 1);
        } else if (strcmp(argv[i], "--esp-label") == 0 && i + 1 < argc) {
            strncpy(cfg->esp_label, argv[++i], sizeof(cfg->esp_label) - 1);
        } else if (strcmp(argv[i], "--root-label") == 0 && i + 1 < argc) {
            strncpy(cfg->root_label, argv[++i], sizeof(cfg->root_label) - 1);
        } else {
            printf("ERROR: unknown or incomplete option '%s'.\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

static int path_is_block_or_regular(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        return 0;
    }
    return S_ISBLK(st.st_mode) || S_ISREG(st.st_mode);
}

static uint64_t get_device_size_sectors(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    off_t end = lseek(fd, 0, SEEK_END);
    close(fd);
    if (end > 0) {
        return (uint64_t)end / SECTOR_SIZE;
    }

    /* Tiramisu block nodes may not implement SEEK_END yet. Use safe defaults. */
    if (strstr(path, "nvme")) {
        return 500118192ULL;
    }
    return 1953525168ULL;
}

static int add_disk(const char *path, int is_nvme)
{
    if (disk_count >= MAX_DISKS || !path_is_block_or_regular(path)) {
        return -1;
    }
    disk_info_t *d = &disks[disk_count];
    memset(d, 0, sizeof(*d));
    strncpy(d->device, path, sizeof(d->device) - 1);
    d->is_nvme = is_nvme;
    d->size_sectors = get_device_size_sectors(path);
    d->size_bytes = d->size_sectors * SECTOR_SIZE;
    snprintf(d->model, sizeof(d->model), "%s %s (%llu MiB)",
             is_nvme ? "NVMe" : "Disk", path,
             (unsigned long long)(d->size_bytes / (1024ULL * 1024ULL)));
    printf("  [%d] %s\n", disk_count, d->model);
    disk_count++;
    return 0;
}

static int list_disks(void)
{
    disk_count = 0;
    printf("Available disks:\n\n");

    for (int i = 0; i < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/sd%c", 'a' + i);
        add_disk(path, 0);
    }

    /* NVMe intentionally disabled in this SATA-focused build.
     * The current nvme.c still has stubbed read/write paths, so exposing NVMe
     * here would make sysinstall appear to succeed while writing nothing.
     */

    if (disk_count == 0) {
        printf("  No usable block disks found.\n");
    }
    printf("\n");
    return disk_count;
}

static int write_all_at(int fd, uint64_t offset, const void *buf, size_t size)
{
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        return -1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n <= 0) {
            return -1;
        }
        p += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}

static int copy_file_to_fd_at(const char *src_path, int dst_fd, uint64_t dst_offset, uint64_t max_bytes)
{
    int src = open(src_path, O_RDONLY);
    if (src < 0) {
        printf("  ERROR: cannot open %s: %s\n", src_path, strerror(errno));
        return -1;
    }
    if (lseek(dst_fd, (off_t)dst_offset, SEEK_SET) < 0) {
        close(src);
        return -1;
    }

    uint8_t buf[4096];
    uint64_t written = 0;
    for (;;) {
        ssize_t r = read(src, buf, sizeof(buf));
        if (r < 0) {
            close(src);
            return -1;
        }
        if (r == 0) {
            break;
        }
        if (written + (uint64_t)r > max_bytes) {
            printf("  ERROR: %s is larger than target partition.\n", src_path);
            close(src);
            return -1;
        }
        uint8_t *p = buf;
        ssize_t left = r;
        while (left > 0) {
            ssize_t w = write(dst_fd, p, (size_t)left);
            if (w <= 0) {
                close(src);
                return -1;
            }
            p += w;
            left -= w;
            written += (uint64_t)w;
        }
    }
    close(src);
    printf("  Wrote %llu KiB from %s.\n", (unsigned long long)(written / 1024), src_path);
    return 0;
}

static int write_protective_mbr(int fd, uint64_t disk_size_sectors)
{
    uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    mbr[510] = 0x55;
    mbr[511] = 0xaa;

    uint8_t *part = &mbr[446];
    part[4] = 0xee;
    uint32_t start_lba = 1;
    uint32_t size = disk_size_sectors > 0xffffffffULL ? 0xffffffffu : (uint32_t)(disk_size_sectors - 1);
    memcpy(&part[8], &start_lba, sizeof(start_lba));
    memcpy(&part[12], &size, sizeof(size));

    return write_all_at(fd, 0, mbr, sizeof(mbr));
}

static void build_gpt_entries(gpt_entry_t entries[GPT_ENTRY_COUNT], uint64_t disk_size_sectors, root_fs_t root_fs)
{
    memset(entries, 0, sizeof(gpt_entry_t) * GPT_ENTRY_COUNT);

    gpt_entry_t *esp = &entries[0];
    memcpy(esp->type_guid, GUID_EFI_SYSTEM, 16);
    generate_guid(esp->partition_guid);
    esp->start_lba = ESP_START_LBA;
    esp->end_lba = ESP_START_LBA + ESP_SIZE_SECTORS - 1;
    gpt_set_name(esp, "EFI System Partition");

    gpt_entry_t *root = &entries[1];
    memcpy(root->type_guid, root_fs_guid(root_fs), 16);
    generate_guid(root->partition_guid);
    root->start_lba = ROOT_START_LBA;
    root->end_lba = disk_size_sectors - 2048ULL;
    gpt_set_name(root, root_fs == FS_TFS ? "Tiramisu TFS Root" : "Tiramisu Root");
}

static int write_gpt_header(int fd, uint64_t disk_size_sectors, int primary,
                            const gpt_entry_t entries[GPT_ENTRY_COUNT])
{
    gpt_header_t header;
    memset(&header, 0, sizeof(header));
    memcpy(header.signature, "EFI PART", 8);
    header.revision = 0x00010000;
    header.header_size = GPT_HEADER_SIZE;
    header.my_lba = primary ? 1ULL : disk_size_sectors - 1ULL;
    header.alternate_lba = primary ? disk_size_sectors - 1ULL : 1ULL;
    header.first_usable_lba = 34ULL;
    header.last_usable_lba = disk_size_sectors - 34ULL;
    generate_guid(header.disk_guid);
    header.partition_entry_lba = primary ? 2ULL : disk_size_sectors - 33ULL;
    header.num_partition_entries = GPT_ENTRY_COUNT;
    header.partition_entry_size = GPT_ENTRY_SIZE;
    header.partition_array_crc32 = crc32(entries, GPT_ENTRY_COUNT * sizeof(gpt_entry_t));
    header.header_crc32 = 0;
    header.header_crc32 = crc32(&header, GPT_HEADER_SIZE);

    return write_all_at(fd, header.my_lba * SECTOR_SIZE, &header, sizeof(header));
}

static int flush_disk_writes(int fd)
{
    /*
     * Tiramisu libc currently does not expose fsync(), so calling it causes
     * an implicit-declaration build error. For now, successful write()+close()
     * is the portable flush boundary for this userspace installer.
     *
     * When fsync is added to libtnu, replace this with:
     *     return fsync(fd);
     */
    (void)fd;
    return 0;
}

static int create_gpt_partitions(int fd, uint64_t disk_size_sectors, root_fs_t root_fs)
{
    if (disk_size_sectors < MIN_DISK_SECTORS) {
        printf("  ERROR: disk is too small for the default layout.\n");
        return -1;
    }

    gpt_entry_t entries[GPT_ENTRY_COUNT];
    build_gpt_entries(entries, disk_size_sectors, root_fs);

    printf("  Creating protective MBR...\n");
    if (write_protective_mbr(fd, disk_size_sectors) < 0) return -1;

    printf("  Writing primary GPT entries...\n");
    if (write_all_at(fd, 2ULL * SECTOR_SIZE, entries, sizeof(entries)) < 0) return -1;

    printf("  Writing backup GPT entries...\n");
    if (write_all_at(fd, (disk_size_sectors - 33ULL) * SECTOR_SIZE, entries, sizeof(entries)) < 0) return -1;

    printf("  Writing primary GPT header...\n");
    if (write_gpt_header(fd, disk_size_sectors, 1, entries) < 0) return -1;

    printf("  Writing backup GPT header...\n");
    if (write_gpt_header(fd, disk_size_sectors, 0, entries) < 0) return -1;

    flush_disk_writes(fd);
    printf("  GPT created: ESP + %s root.\n", root_fs_name(root_fs));
    return 0;
}

static void partition_paths(const char *disk, char esp[64], char root[64])
{
    /* SATA/ATA naming: /dev/sda -> /dev/sda1 and /dev/sda2. */
    snprintf(esp, 64, "%s1", disk);
    snprintf(root, 64, "%s2", disk);
}

static int run_command(const char *cmd)
{
#if defined(__unix__) || defined(__linux__)
    int rc = system(cmd);
    if (rc != 0) {
        printf("  WARN: command failed: %s\n", cmd);
        return -1;
    }
    return 0;
#else
    (void)cmd;
    return -1;
#endif
}

static int format_partition_with_tool(const char *part, root_fs_t fs, const char *label)
{
    char cmd[256];
    switch (fs) {
    case FS_EXT2:
        snprintf(cmd, sizeof(cmd), "mkfs.ext2 -F -L %s %s", label, part);
        return run_command(cmd);
    case FS_EXT4:
        snprintf(cmd, sizeof(cmd), "mkfs.ext4 -F -L %s %s", label, part);
        return run_command(cmd);
    case FS_FAT32:
        snprintf(cmd, sizeof(cmd), "mkfs.vfat -F 32 -n %s %s", label, part);
        return run_command(cmd);
    case FS_TFS:
    default:
        return -1;
    }
}

static int format_esp(const char *esp_part, const install_config_t *cfg)
{
    if (!cfg->install_esp) {
        printf("  Skipping ESP format by configuration.\n");
        return 0;
    }
    printf("  Formatting ESP %s as FAT32...\n", esp_part);
    if (format_partition_with_tool(esp_part, FS_FAT32, cfg->esp_label) == 0) {
        return 0;
    }
    printf("  WARN: mkfs.vfat unavailable or failed; ESP formatting needs FAT32 mkfs support in the live environment.\n");
    return -1;
}

static int install_tfs_root_raw(const char *disk_device, uint64_t disk_size_sectors)
{
    int fd = open(disk_device, O_RDWR);
    if (fd < 0) {
        printf("  ERROR: cannot reopen %s: %s\n", disk_device, strerror(errno));
        return -1;
    }

    uint64_t root_offset = ROOT_START_LBA * SECTOR_SIZE;
    uint64_t root_bytes = (disk_size_sectors - 2048ULL - ROOT_START_LBA + 1ULL) * SECTOR_SIZE;
    printf("  Installing TFS root image from %s...\n", ROOT_IMAGE_PATH);
    int rc = copy_file_to_fd_at(ROOT_IMAGE_PATH, fd, root_offset, root_bytes);
    flush_disk_writes(fd);
    close(fd);
    return rc;
}

static int install_file_tree_to_root(const char *root_part, root_fs_t fs)
{
    char cmd[512];
    printf("  Installing file tree to %s (%s)...\n", root_part, root_fs_name(fs));

    /* This path is intentionally external-tool based until Tiramisu has real VFS-backed mounts in userspace. */
    snprintf(cmd, sizeof(cmd),
             "mkdir -p /mnt/tiramisu-root && mount %s /mnt/tiramisu-root && cp -a /rootfs/. /mnt/tiramisu-root/ && sync && umount /mnt/tiramisu-root",
             root_part);
    if (run_command(cmd) == 0) {
        return 0;
    }

    printf("  ERROR: could not copy rootfs to %s.\n", root_part);
    printf("  Hint: for non-TFS installs the live environment needs mount + cp + mkfs support.\n");
    return -1;
}

static int format_and_install_root(const char *disk_device, const char *root_part,
                                   uint64_t disk_size_sectors,
                                   const install_config_t *cfg)
{
    root_fs_t root_fs = cfg->root_fs;
    if (root_fs == FS_TFS) {
        printf("  Root filesystem: TFS (default, raw root.tfs image)\n");
        return install_tfs_root_raw(disk_device, disk_size_sectors);
    }

    printf("  Formatting root %s as %s...\n", root_part, root_fs_name(root_fs));
    if (format_partition_with_tool(root_part, root_fs, cfg->root_label) < 0) {
        printf("  ERROR: mkfs for %s failed or is missing.\n", root_fs_name(root_fs));
        return -1;
    }
    return install_file_tree_to_root(root_part, root_fs);
}

static int install_bootloader(const char *esp_part, const install_config_t *cfg)
{
    if (!cfg->install_bootloader) {
        printf("\nSkipping bootloader installation by configuration.\n");
        return 0;
    }

    printf("\nInstalling bootloader files...\n");
    printf("  ESP: %s\n", esp_part);

    /* Prefer real mounted ESP workflow when available. */
    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "mkdir -p /mnt/tiramisu-esp/EFI/BOOT /mnt/tiramisu-esp/boot/grub "
             "&& mount %s /mnt/tiramisu-esp "
             "&& mkdir -p /mnt/tiramisu-esp/EFI/BOOT /mnt/tiramisu-esp/boot/grub "
             "&& cp -f %s /mnt/tiramisu-esp/EFI/BOOT/BOOTX64.EFI "
             "&& cp -f %s /mnt/tiramisu-esp/boot/kernel.elf "
             "&& if [ -f /boot/root.tfs ]; then cp -f /boot/root.tfs /mnt/tiramisu-esp/boot/root.tfs; fi "
             "&& printf 'set timeout=3\\nmenuentry \"Tiramisu\" {\\n    multiboot2 /boot/kernel.elf\\n    boot\\n}\\n' > /mnt/tiramisu-esp/boot/grub/grub.cfg "
             "&& sync && umount /mnt/tiramisu-esp",
             esp_part, BOOTX64_IMAGE_PATH, KERNEL_IMAGE_PATH);

    if (run_command(cmd) == 0) {
        printf("  Bootloader files installed.\n");
        return 0;
    }

    printf("  WARN: bootloader copy failed. ESP may need manual mounting/copying.\n");
    return -1;
}

static int write_text_file(const char *path, const char *text)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static int configure_system(const install_config_t *cfg, const char *root_part)
{
    char text[512];
    printf("\nConfiguring system metadata...\n");
    printf("  Root filesystem selected: %s\n", root_fs_name(cfg->root_fs));
    printf("  Hostname: %s\n", cfg->hostname);
    printf("  Primary user: %s\n", cfg->username);

    snprintf(text, sizeof(text), "%s\n", cfg->hostname);
    if (write_text_file("/etc/hostname", text) < 0) {
        printf("  WARN: could not update /etc/hostname in live root.\n");
    }

    snprintf(text, sizeof(text),
             "%s / %s rw 0 1\n"
             "proc /proc proc rw 0 0\n"
             "devtmpfs /dev devtmpfs rw 0 0\n"
             "tmpfs /tmp tmpfs rw 0 0\n",
             cfg->root_fs == FS_TFS ? "tfs-root" : root_part,
             root_fs_name(cfg->root_fs));
    if (write_text_file("/etc/fstab", text) < 0) {
        printf("  WARN: could not write /etc/fstab in live root.\n");
    }

    snprintf(text, sizeof(text),
             "target=%s\n"
             "rootfs=%s\n"
             "hostname=%s\n"
             "user=%s\n"
             "esp_label=%s\n"
             "root_label=%s\n",
             cfg->target[0] ? cfg->target : "(interactive)",
             root_fs_name(cfg->root_fs),
             cfg->hostname,
             cfg->username,
             cfg->esp_label,
             cfg->root_label);
    if (write_text_file("/etc/sysinstall.last", text) < 0) {
        printf("  WARN: could not write /etc/sysinstall.last in live root.\n");
    }

    if (cfg->root_fs == FS_TFS) {
        printf("  Note: TFS target installs copy /boot/root.tfs raw; rebuild the ISO or boot installed TFS to bake new defaults into the target image.\n");
    }
    return 0;
}

static root_fs_t ask_root_filesystem(void)
{
    char input[32];
    printf("Choose root filesystem:\n");
    printf("  [1] tfs   - Tiramisu proprietary FS image (default)\n");
    printf("  [2] ext2  - simple Unix filesystem\n");
    printf("  [3] ext4  - Linux ext4 (requires mkfs.ext4 + safer readonly kernel support unless journal is handled)\n");
    printf("  [4] fat32 - not recommended for root, useful for testing\n");
    printf("Selection [1]: ");
    fflush(stdout);

    if (!fgets(input, sizeof(input), stdin) || input[0] == '\n' || input[0] == '\0') {
        return DEFAULT_ROOTFS;
    }

    if (input[0] == '2') return FS_EXT2;
    if (input[0] == '3') return FS_EXT4;
    if (input[0] == '4') return FS_FAT32;
    return FS_TFS;
}

static int perform_installation(const char *disk_device, uint64_t disk_size_sectors,
                                const install_config_t *cfg)
{
    char esp_part[64];
    char root_part[64];
    partition_paths(disk_device, esp_part, root_part);

    printf("\nStarting installation\n");
    printf("  Disk: %s\n", disk_device);
    printf("  ESP:  %s (FAT32, 256 MiB)\n", esp_part);
    printf("  Root: %s (%s)\n", root_part, root_fs_name(cfg->root_fs));
    printf("  ESP label:  %s\n", cfg->esp_label);
    printf("  Root label: %s\n\n", cfg->root_label);

    if (cfg->dry_run) {
        printf("Dry run selected; no disk writes performed.\n");
        return 0;
    }

    int fd = open(disk_device, O_RDWR);
    if (fd < 0) {
        printf("ERROR: cannot open %s for writing: %s\n", disk_device, strerror(errno));
        return 1;
    }

    printf("Step 1: Creating GPT partition table...\n");
    if (create_gpt_partitions(fd, disk_size_sectors, cfg->root_fs) < 0) {
        close(fd);
        printf("ERROR: GPT creation failed.\n");
        return 1;
    }
    close(fd);

    printf("\nStep 2: Formatting partitions...\n");
    if (format_esp(esp_part, cfg) < 0) {
        printf("ERROR: ESP format failed.\n");
        return 1;
    }
    if (format_and_install_root(disk_device, root_part, disk_size_sectors, cfg) < 0) {
        printf("ERROR: root filesystem install failed.\n");
        return 1;
    }

    printf("\nStep 3: Installing bootloader...\n");
    if (install_bootloader(esp_part, cfg) < 0) {
        printf("ERROR: bootloader installation failed.\n");
        return 1;
    }

    printf("\nStep 4: Configuring system...\n");
    if (configure_system(cfg, root_part) < 0) {
        printf("ERROR: system configuration failed.\n");
        return 1;
    }

    /* Flush the in-memory TFS image to disk so any changes made during
     * installation (e.g. writing /etc/fstab) are stored persistently. */
    printf("\nStep 5: Syncing filesystem...\n");
    sync();
    printf("  Filesystem synced.\n");

    printf("\nInstallation complete. Reboot and select %s in firmware/boot menu.\n", disk_device);
    return 0;
}

static disk_info_t *find_configured_disk(const char *target)
{
    if (!target || !target[0]) {
        return NULL;
    }
    const char *name = target;
    if (strncmp(name, "/dev/", 5) == 0) {
        name += 5;
    }
    for (int i = 0; i < disk_count; i++) {
        const char *dev = disks[i].device;
        if (strcmp(dev, target) == 0 || strcmp(dev + 5, name) == 0) {
            return &disks[i];
        }
    }
    return NULL;
}

static void demo_workflow(void)
{
    printf("Demo workflow:\n");
    printf("  1. Create GPT: ESP + root partition\n");
    printf("  2. Format ESP as FAT32\n");
    printf("  3. Ask root filesystem, default = %s\n", DEFAULT_ROOTFS_NAME);
    printf("  4. If TFS: write /boot/root.tfs raw into partition 2\n");
    printf("  5. If ext2/ext4/fat32: mkfs + mount + copy /rootfs\n");
    printf("  6. Copy BOOTX64.EFI, kernel.elf and grub.cfg into ESP\n");
}

int main(int argc, char **argv)
{
    install_config_t cfg;
    config_defaults(&cfg);
    load_config_file(&cfg, SYSINSTALL_CONFIG_PATH);
    int cli = apply_cli(&cfg, argc, argv);
    if (cli != 0) {
        return cli > 0 ? 0 : 1;
    }

    print_header();
    printf("WARNING: this installer erases the selected disk.\n\n");

    if (!cfg.root_fs_set) {
        cfg.root_fs = ask_root_filesystem();
    }
    printf("Selected root filesystem: %s\n", root_fs_name(cfg.root_fs));
    printf("Configuration: %s\n\n", SYSINSTALL_CONFIG_PATH);

    if (list_disks() == 0) {
        demo_workflow();
        printf("\nNo disk was modified.\n");
        return 0;
    }

    char input[32];
    disk_info_t *disk = find_configured_disk(cfg.target);
    if (!disk) {
        printf("Select disk [0-%d]: ", disk_count - 1);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            printf("ERROR: invalid input.\n");
            return 1;
        }

        int selected = atoi(input);
        if (selected < 0 || selected >= disk_count) {
            printf("ERROR: invalid disk selection.\n");
            return 1;
        }
        disk = &disks[selected];
        strncpy(cfg.target, disk->device, sizeof(cfg.target) - 1);
    }

    printf("\nSelected disk: %s\n", disk->model);
    printf("Root filesystem: %s\n", root_fs_name(cfg.root_fs));
    printf("Hostname: %s\n", cfg.hostname);

    if (!cfg.assume_yes && !cfg.dry_run) {
        printf("\nType ERASE to continue: ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin) || strncmp(input, "ERASE", 5) != 0) {
            printf("Installation cancelled.\n");
            return 0;
        }
    }

    return perform_installation(disk->device, disk->size_sectors, &cfg);
}
