/*
 * sysinstall - Tiramisu Installation Tool
 * Real disk-oriented installer for Tiramisu.
 * Default and only root filesystem: TFS.
 *
 * Supports:
 *  - disk discovery: /dev/sdX (SATA/legacy ATA target; NVMe intentionally disabled here)
 *  - GPT creation with BIOS Boot + ESP + TFS root partitions
 *  - native ESP FAT32 formatting and minimal UEFI file installation
 *  - native TFS root installation through the kernel TFS serializer
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
#include <tnu/syscall.h>

long tnu_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);

#define MAX_DISKS 16
#define SECTOR_SIZE 512ULL
#define GPT_HEADER_SIZE 92
#define GPT_ENTRY_SIZE 128
#define GPT_ENTRY_COUNT 128
#define BIOS_BOOT_START_LBA 34ULL
#define BIOS_BOOT_END_LBA 2047ULL
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
#define GRUB_CFG_TEXT "set timeout=5\nset default=0\n\ninsmod all_video\ninsmod ext2\ninsmod part_gpt\n\nterminal_output console\n\nmenuentry \"Tiramisu 1.1.0 (Disk Boot)\" {\n    # Find the partition containing /boot/kernel.elf\n    search --no-floppy --file --set=root /boot/kernel.elf\n    \n    # Load kernel from the found partition\n    multiboot2 ($root)/boot/kernel.elf boot=disk root=tfs\n    \n    # Boot directly - kernel will find root.tfs on disk via GPT/partition scan\n    boot\n}\n\nmenuentry \"Tiramisu 1.1.0 (Recovery Mode)\" {\n    search --no-floppy --file --set=root /boot/kernel.elf\n    multiboot2 ($root)/boot/kernel.elf boot=disk root=tfs recovery\n    boot\n}\n"

/* GPT type GUIDs, encoded in on-disk little-endian layout. */
static const uint8_t GUID_EFI_SYSTEM[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

static const uint8_t GUID_BIOS_BOOT[16] = {
    0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
    0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49
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

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)(v >> 24);
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
    printf("  --target /dev/sdX\n");
    printf("  --hostname NAME\n");
    printf("  --user NAME\n");
    printf("  --esp-label LABEL\n");
    printf("  --root-label LABEL\n");
    printf("  --yes\n");
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
            i++;
            printf("sysinstall: root filesystem is fixed to tfs; ignoring --fs %s\n", argv[i]);
            cfg->root_fs = FS_TFS;
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
    /* Tiramisu devfs currently exposes device nodes with VFS_S_IFDEV,
     * which maps to S_IFCHR in userspace stat(). Accept character devices
     * here so /dev/sdX installer targets are discovered correctly.
     */
    return S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode) || S_ISREG(st.st_mode);
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

    /* Enable NVMe device discovery.
     * The nvme.c driver is now functional with real read/write paths. */
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/nvme%d", i);
        add_disk(path, 1);
    }

    if (disk_count == 0) {
        printf("  No usable block disks found.\n");
    }
    printf("\n");
    return disk_count;
}

static int write_all_at(int fd, uint64_t offset, const void *buf, size_t size)
{
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        printf("  ERROR: lseek failed at byte offset %llu: %s\n",
               (unsigned long long)offset, strerror(errno));
        return -1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = size;
    while (remaining) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) {
            uint64_t failed_at = offset + (uint64_t)(size - remaining);
            printf("  ERROR: write failed at byte offset %llu (wanted %llu more bytes): %s\n",
                   (unsigned long long)failed_at,
                   (unsigned long long)remaining,
                   strerror(errno));
            return -1;
        }
        p += (size_t)n;
        remaining -= (size_t)n;
    }
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

    if (write_all_at(fd, 0, mbr, sizeof(mbr)) < 0) {
        printf("  ERROR: failed writing protective MBR at LBA 0.\n");
        return -1;
    }
    return 0;
}


static void build_gpt_entries(gpt_entry_t entries[GPT_ENTRY_COUNT], uint64_t disk_size_sectors, root_fs_t root_fs)
{
    memset(entries, 0, sizeof(gpt_entry_t) * GPT_ENTRY_COUNT);

    gpt_entry_t *bios = &entries[0];
    memcpy(bios->type_guid, GUID_BIOS_BOOT, 16);
    generate_guid(bios->partition_guid);
    bios->start_lba = BIOS_BOOT_START_LBA;
    bios->end_lba = BIOS_BOOT_END_LBA;
    gpt_set_name(bios, "BIOS Boot Partition");

    gpt_entry_t *esp = &entries[1];
    memcpy(esp->type_guid, GUID_EFI_SYSTEM, 16);
    generate_guid(esp->partition_guid);
    esp->start_lba = ESP_START_LBA;
    esp->end_lba = ESP_START_LBA + ESP_SIZE_SECTORS - 1;
    gpt_set_name(esp, "EFI System Partition");

    gpt_entry_t *root = &entries[2];
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

    if (write_all_at(fd, header.my_lba * SECTOR_SIZE, &header, sizeof(header)) < 0) {
        printf("  ERROR: failed writing %s GPT header at LBA %llu.\n",
               primary ? "primary" : "backup",
               (unsigned long long)header.my_lba);
        return -1;
    }
    return 0;
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
    if (write_protective_mbr(fd, disk_size_sectors) < 0) {
        printf("  ERROR: GPT step failed: protective MBR.\n");
        return -1;
    }

    printf("  Writing primary GPT entries...\n");
    if (write_all_at(fd, 2ULL * SECTOR_SIZE, entries, sizeof(entries)) < 0) {
        printf("  ERROR: GPT step failed: primary GPT entries at LBA 2.\n");
        return -1;
    }

    printf("  Writing backup GPT entries...\n");
    if (write_all_at(fd, (disk_size_sectors - 33ULL) * SECTOR_SIZE, entries, sizeof(entries)) < 0) {
        printf("  ERROR: GPT step failed: backup GPT entries at LBA %llu.\n",
               (unsigned long long)(disk_size_sectors - 33ULL));
        return -1;
    }

    printf("  Writing primary GPT header...\n");
    if (write_gpt_header(fd, disk_size_sectors, 1, entries) < 0) {
        printf("  ERROR: GPT step failed: primary GPT header.\n");
        return -1;
    }

    printf("  Writing backup GPT header...\n");
    if (write_gpt_header(fd, disk_size_sectors, 0, entries) < 0) {
        printf("  ERROR: GPT step failed: backup GPT header.\n");
        return -1;
    }


    flush_disk_writes(fd);
    printf("  GPT created: BIOS boot + ESP + %s root.\n", root_fs_name(root_fs));
    return 0;
}

static void partition_paths(const char *disk, char esp[64], char root[64])
{
    /* SATA/ATA naming: /dev/sda -> /dev/sda2 ESP and /dev/sda3 root.
     * Partition 1 is the GRUB BIOS Boot Partition used on legacy firmware.
     */
    snprintf(esp, 64, "%s2", disk);
    snprintf(root, 64, "%s3", disk);
}

/* Maximum FAT sectors we cache in memory (covers up to 128 KiB of FAT) */
#define FAT_CACHE_SECTORS 256

struct fat32_ctx {
    int fd;
    uint64_t base_lba;
    uint32_t total_sectors;
    uint32_t reserved;
    uint32_t sectors_per_fat;
    uint32_t sectors_per_cluster;
    uint32_t first_data_sector;
    uint32_t next_cluster;
    /* In-memory FAT sector cache — all FAT entries are written here first,
     * then flushed as whole sectors (required by block devices). */
    uint8_t fat_cache[FAT_CACHE_SECTORS * 512];
    uint32_t fat_cache_dirty[FAT_CACHE_SECTORS]; /* 1 = needs flush */
    uint32_t fat_cache_sectors; /* how many sectors are actually used */
    /* Small cluster data cache: one cluster at a time for directory entries */
    uint8_t dir_cache[4096]; /* enough for 8-sector cluster */
    uint32_t dir_cache_cluster;
    int dir_cache_valid;
};

static uint64_t fat_sector_offset(const struct fat32_ctx *ctx, uint32_t sector)
{
    return (ctx->base_lba + sector) * SECTOR_SIZE;
}

static uint64_t fat_cluster_offset(const struct fat32_ctx *ctx, uint32_t cluster)
{
    uint32_t sector = ctx->first_data_sector + (cluster - 2u) * ctx->sectors_per_cluster;
    return fat_sector_offset(ctx, sector);
}

static int write_zero_sectors(int fd, uint64_t offset, uint32_t sectors)
{
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < sectors; i++) {
        if (write_all_at(fd, offset + (uint64_t)i * SECTOR_SIZE, zero, sizeof(zero)) < 0) {
            return -1;
        }
    }
    return 0;
}

static uint32_t fat_alloc_cluster(struct fat32_ctx *ctx)
{
    return ctx->next_cluster++;
}

/* Write all dirty FAT cache sectors to both FAT copies on disk. */
static int fat_flush_cache(struct fat32_ctx *ctx)
{
    for (uint32_t s = 0; s < ctx->fat_cache_sectors; s++) {
        if (!ctx->fat_cache_dirty[s]) {
            continue;
        }
        const uint8_t *buf = ctx->fat_cache + s * SECTOR_SIZE;
        for (uint32_t fat = 0; fat < 2; fat++) {
            uint64_t off = fat_sector_offset(ctx, ctx->reserved + fat * ctx->sectors_per_fat
                                             + s);
            if (write_all_at(ctx->fd, off, buf, SECTOR_SIZE) < 0) {
                return -1;
            }
        }
        ctx->fat_cache_dirty[s] = 0;
    }
    return 0;
}

/* Set a FAT32 entry in the in-memory cache (flushed later). */
static int fat_set_entry(struct fat32_ctx *ctx, uint32_t cluster, uint32_t value)
{
    uint32_t fat_offset = cluster * 4u;       /* byte offset within FAT */
    uint32_t sec = fat_offset / SECTOR_SIZE;  /* which cache sector */
    uint32_t off = fat_offset % SECTOR_SIZE;  /* byte within that sector */
    if (sec >= FAT_CACHE_SECTORS) {
        return -1;
    }
    if (sec >= ctx->fat_cache_sectors) {
        ctx->fat_cache_sectors = sec + 1;
    }
    put32(ctx->fat_cache + sec * SECTOR_SIZE + off, value);
    ctx->fat_cache_dirty[sec] = 1;
    return 0;
}

static int fat_init_cluster(struct fat32_ctx *ctx, uint32_t cluster)
{
    if (fat_set_entry(ctx, cluster, 0x0fffffffu) < 0) {
        return -1;
    }
    if (fat_flush_cache(ctx) < 0) {
        return -1;
    }
    return write_zero_sectors(ctx->fd, fat_cluster_offset(ctx, cluster),
                              ctx->sectors_per_cluster);
}

/* Load a directory cluster into the dir cache (if not already loaded). */
static int fat_load_dir_cluster(struct fat32_ctx *ctx, uint32_t cluster)
{
    if (ctx->dir_cache_valid && ctx->dir_cache_cluster == cluster) {
        return 0;
    }
    size_t cluster_bytes = (size_t)ctx->sectors_per_cluster * SECTOR_SIZE;
    if (cluster_bytes > sizeof(ctx->dir_cache)) {
        cluster_bytes = sizeof(ctx->dir_cache);
    }
    memset(ctx->dir_cache, 0, cluster_bytes);
    ctx->dir_cache_cluster = cluster;
    ctx->dir_cache_valid = 1;
    return 0;
}

/* Flush the dir cache cluster back to disk. */
static int fat_flush_dir_cluster(struct fat32_ctx *ctx)
{
    if (!ctx->dir_cache_valid) {
        return 0;
    }
    size_t cluster_bytes = (size_t)ctx->sectors_per_cluster * SECTOR_SIZE;
    if (cluster_bytes > sizeof(ctx->dir_cache)) {
        cluster_bytes = sizeof(ctx->dir_cache);
    }
    uint64_t off = fat_cluster_offset(ctx, ctx->dir_cache_cluster);
    /* Write in sector-sized chunks */
    for (size_t i = 0; i < cluster_bytes; i += SECTOR_SIZE) {
        if (write_all_at(ctx->fd, off + i, ctx->dir_cache + i, SECTOR_SIZE) < 0) {
            return -1;
        }
    }
    ctx->dir_cache_valid = 0;
    return 0;
}

static int fat_write_dirent(struct fat32_ctx *ctx, uint32_t dir_cluster, uint32_t slot,
                            const char name[11], uint8_t attr,
                            uint32_t first_cluster, uint32_t size)
{
    /* Flush any previously cached directory cluster if it differs. */
    if (ctx->dir_cache_valid && ctx->dir_cache_cluster != dir_cluster) {
        if (fat_flush_dir_cluster(ctx) < 0) {
            return -1;
        }
    }
    if (fat_load_dir_cluster(ctx, dir_cluster) < 0) {
        return -1;
    }
    uint8_t *e = ctx->dir_cache + slot * 32u;
    memset(e, 0, 32);
    memcpy(e, name, 11);
    e[11] = attr;
    put16(e + 20, (uint16_t)(first_cluster >> 16));
    put16(e + 26, (uint16_t)(first_cluster & 0xffff));
    put32(e + 28, size);
    return 0;
}

static int fat_write_text_file(struct fat32_ctx *ctx, uint32_t parent_cluster,
                               uint32_t slot, const char name[11], const char *text)
{
    uint32_t cluster = fat_alloc_cluster(ctx);
    size_t len = strlen(text);
    if (fat_init_cluster(ctx, cluster) < 0 ||
        write_all_at(ctx->fd, fat_cluster_offset(ctx, cluster), text, len) < 0 ||
        fat_write_dirent(ctx, parent_cluster, slot, name, 0x20, cluster, (uint32_t)len) < 0) {
        return -1;
    }
    /* Flush directory and FAT caches after writing file */
    if (fat_flush_dir_cluster(ctx) < 0 || fat_flush_cache(ctx) < 0) {
        return -1;
    }
    return 0;
}

static int fat_write_file_from_path(struct fat32_ctx *ctx, uint32_t parent_cluster,
                                    uint32_t slot, const char name[11], const char *src_path)
{
    int src = open(src_path, O_RDONLY);
    if (src < 0) {
        printf("  WARN: ESP source missing: %s\n", src_path);
        return -1;
    }
    struct stat st;
    if (fstat(src, &st) < 0 || st.st_size < 0) {
        close(src);
        return -1;
    }

    uint32_t first = 0;
    uint32_t prev = 0;
    uint32_t total = 0;
    uint8_t cluster_buf[4096];
    for (;;) {
        ssize_t r = read(src, cluster_buf, sizeof(cluster_buf));
        if (r < 0) {
            close(src);
            return -1;
        }
        if (r == 0) {
            break;
        }
        uint32_t cluster = fat_alloc_cluster(ctx);
        if (!first) {
            first = cluster;
        }
        if (prev && fat_set_entry(ctx, prev, cluster) < 0) {
            close(src);
            return -1;
        }
        if (fat_set_entry(ctx, cluster, 0x0fffffffu) < 0 ||
            write_zero_sectors(ctx->fd, fat_cluster_offset(ctx, cluster),
                               ctx->sectors_per_cluster) < 0 ||
            write_all_at(ctx->fd, fat_cluster_offset(ctx, cluster),
                         cluster_buf, (size_t)r) < 0) {
            close(src);
            return -1;
        }
        prev = cluster;
        total += (uint32_t)r;
    }
    close(src);

    if (!first) {
        first = fat_alloc_cluster(ctx);
        if (fat_init_cluster(ctx, first) < 0) {
            return -1;
        }
    }
    if (fat_write_dirent(ctx, parent_cluster, slot, name, 0x20, first, total) < 0) {
        return -1;
    }
    /* Flush directory and FAT caches after writing file */
    if (fat_flush_dir_cluster(ctx) < 0 || fat_flush_cache(ctx) < 0) {
        return -1;
    }
    return 0;
}

static int format_esp_native(const char *disk_device, const install_config_t *cfg)
{
    if (!cfg->install_esp) {
        printf("  Skipping ESP format by configuration.\n");
        return 0;
    }

    int fd = open(disk_device, O_RDWR);
    if (fd < 0) {
        printf("  ERROR: cannot open %s for ESP format: %s\n", disk_device, strerror(errno));
        return -1;
    }

    struct fat32_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = fd;
    ctx.base_lba = ESP_START_LBA;
    ctx.total_sectors = (uint32_t)ESP_SIZE_SECTORS;
    ctx.reserved = 32;
    ctx.sectors_per_cluster = 8;

    uint32_t fat_secs = 1;
    for (;;) {
        uint32_t data_secs = ctx.total_sectors - ctx.reserved - fat_secs * 2u;
        uint32_t clusters = data_secs / ctx.sectors_per_cluster;
        uint32_t needed = ((clusters + 2u) * 4u + 511u) / 512u;
        if (needed == fat_secs) {
            break;
        }
        fat_secs = needed;
    }
    ctx.sectors_per_fat = fat_secs;
    ctx.first_data_sector = ctx.reserved + ctx.sectors_per_fat * 2u;
    ctx.next_cluster = 3;

    printf("  Formatting ESP as FAT32 natively...\n");
    uint8_t bs[512];
    memset(bs, 0, sizeof(bs));
    bs[0] = 0xeb; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "TIRAMISU", 8);
    put16(bs + 11, 512);
    bs[13] = (uint8_t)ctx.sectors_per_cluster;
    put16(bs + 14, (uint16_t)ctx.reserved);
    bs[16] = 2;
    put32(bs + 32, ctx.total_sectors);
    put32(bs + 36, ctx.sectors_per_fat);
    put32(bs + 44, 2);
    put16(bs + 48, 1);
    put16(bs + 50, 6);
    bs[64] = 0x80;
    bs[66] = 0x29;
    put32(bs + 67, 0x544e5501u);
    memset(bs + 71, ' ', 11);
    for (size_t i = 0; cfg->esp_label[i] && i < 11; i++) {
        bs[71 + i] = (uint8_t)cfg->esp_label[i];
    }
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xaa;

    uint8_t fsinfo[512];
    memset(fsinfo, 0, sizeof(fsinfo));
    put32(fsinfo + 0, 0x41615252u);
    put32(fsinfo + 484, 0x61417272u);
    put32(fsinfo + 488, 0xffffffffu);
    put32(fsinfo + 492, 3);
    fsinfo[510] = 0x55; fsinfo[511] = 0xaa;

    uint64_t base = ESP_START_LBA * SECTOR_SIZE;
    if (write_all_at(fd, base, bs, sizeof(bs)) < 0 ||
        write_all_at(fd, base + SECTOR_SIZE, fsinfo, sizeof(fsinfo)) < 0 ||
        write_all_at(fd, base + 6 * SECTOR_SIZE, bs, sizeof(bs)) < 0 ||
        write_all_at(fd, base + 7 * SECTOR_SIZE, fsinfo, sizeof(fsinfo)) < 0 ||
        write_zero_sectors(fd, base + ctx.reserved * SECTOR_SIZE,
                           ctx.sectors_per_fat * 2u) < 0) {
        close(fd);
        return -1;
    }

    uint8_t fat0[512];
    memset(fat0, 0, sizeof(fat0));
    put32(fat0 + 0, 0x0ffffff8u);
    put32(fat0 + 4, 0xffffffffu);
    put32(fat0 + 8, 0x0fffffffu);
    if (write_all_at(fd, fat_sector_offset(&ctx, ctx.reserved), fat0, sizeof(fat0)) < 0 ||
        write_all_at(fd, fat_sector_offset(&ctx, ctx.reserved + ctx.sectors_per_fat), fat0, sizeof(fat0)) < 0 ||
        write_zero_sectors(fd, fat_cluster_offset(&ctx, 2), ctx.sectors_per_cluster) < 0) {
        close(fd);
        return -1;
    }

    uint32_t efi = fat_alloc_cluster(&ctx);
    uint32_t efi_boot = fat_alloc_cluster(&ctx);
    uint32_t boot = fat_alloc_cluster(&ctx);
    uint32_t grub = fat_alloc_cluster(&ctx);
    if (fat_init_cluster(&ctx, efi) < 0 || fat_init_cluster(&ctx, efi_boot) < 0 ||
        fat_init_cluster(&ctx, boot) < 0 || fat_init_cluster(&ctx, grub) < 0 ||
        fat_write_dirent(&ctx, 2, 0, "EFI        ", 0x10, efi, 0) < 0 ||
        fat_write_dirent(&ctx, 2, 1, "BOOT       ", 0x10, boot, 0) < 0 ||
        fat_write_dirent(&ctx, efi, 0, "BOOT       ", 0x10, efi_boot, 0) < 0 ||
        fat_write_dirent(&ctx, boot, 0, "GRUB       ", 0x10, grub, 0) < 0 ||
        fat_write_file_from_path(&ctx, efi_boot, 0, "BOOTX64 EFI", BOOTX64_IMAGE_PATH) < 0 ||
        fat_write_file_from_path(&ctx, boot, 1, "KERNEL  ELF", KERNEL_IMAGE_PATH) < 0 ||
        fat_write_text_file(&ctx, grub, 0, "GRUB    CFG", GRUB_CFG_TEXT) < 0) {
        close(fd);
        return -1;
    }

    /* Flush all caches before closing */
    fat_flush_dir_cluster(&ctx);
    fat_flush_cache(&ctx);

    close(fd);
    printf("  ESP formatted and UEFI boot files installed.\n");
    return 0;
}

static int install_tfs_root_raw(const char *disk_device, uint64_t disk_size_sectors)
{
    (void)disk_size_sectors;
    const char *name = disk_device;
    if (strncmp(name, "/dev/", 5) == 0) {
        name += 5;
    }

    printf("  Formatting root as TFS from live system...\n");
    long rc = tnu_syscall(SYS_TFS_INSTALL_ROOT, (long)name, ROOT_START_LBA, 0, 0, 0, 0);
    if (rc < 0) {
        printf("  ERROR: kernel TFS install failed for %s@LBA%llu.\n",
               name, (unsigned long long)ROOT_START_LBA);
        return -1;
    }
    printf("  TFS root installed.\n");
    return 0;
}

static int format_and_install_root(const char *disk_device, const char *root_part,
                                   uint64_t disk_size_sectors,
                                   const install_config_t *cfg)
{
    (void)root_part;
    root_fs_t root_fs = cfg->root_fs;
    if (root_fs == FS_TFS) {
        printf("  Root filesystem: TFS\n");
        return install_tfs_root_raw(disk_device, disk_size_sectors);
    }
    printf("  ERROR: unsupported root filesystem %s; sysinstall uses TFS only.\n",
           root_fs_name(root_fs));
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

    printf("  Note: TFS target was generated from the live root filesystem.\n");
    return 0;
}

static int perform_installation(const char *disk_device, uint64_t disk_size_sectors,
                                const install_config_t *cfg)
{
    char esp_part[64];
    char root_part[64];
    partition_paths(disk_device, esp_part, root_part);

    printf("\nStarting installation\n");
    printf("  Disk: %s\n", disk_device);
    printf("  BIOS: %s1 (GRUB BIOS Boot, 1007 KiB)\n", disk_device);
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
    printf("  Partition table written.\n\n");

    printf("Step 2: Formatting partitions...\n");
    if (format_esp_native(disk_device, cfg) < 0) {
        printf("ERROR: ESP format failed.\n");
        return 1;
    }
    if (format_and_install_root(disk_device, root_part, disk_size_sectors, cfg) < 0) {
        printf("ERROR: root filesystem install failed.\n");
        return 1;
    }
    printf("  Partitions formatted.\n\n");

    printf("Step 3: Installing boot files...\n");
    printf("  UEFI removable boot files are installed in the ESP.\n");
    printf("  BIOS Boot partition is reserved for future native GRUB core embedding.\n");
    printf("  Boot files installed.\n\n");

    printf("Step 4: Configuring system...\n");
    if (configure_system(cfg, root_part) < 0) {
        printf("ERROR: system configuration failed.\n");
        return 1;
    }
    printf("  System configured.\n\n");

    /* Flush the in-memory TFS image to disk so any changes made during
     * installation (e.g. writing /etc/fstab) are stored persistently. */
    printf("Step 5: Syncing filesystem...\n");
    sync();
    printf("  Filesystem synced.\n\n");

    printf("Installation complete!\n\n");
    printf("  - Reboot and enter BIOS/UEFI setup\n");
    printf("  - Set boot order to prioritize UEFI: %s\n", disk_device);
    printf("  - Or select 'TNU BOOT' from UEFI boot menu\n\n");
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

/* Read a line from stdin with manual echo for visibility.
 * TNU console may not have terminal echo enabled by default. */
static char *fgets_with_echo(char *s, int size, FILE *stream)
{
    if (!s || size <= 0 || !stream) {
        return NULL;
    }
    int i = 0;
    while (i < size - 1) {
        unsigned char ch;
        ssize_t n = read(fileno(stream), &ch, 1);
        if (n <= 0) {
            if (i == 0) return NULL;
            break;
        }
        /* Echo the character back to stdout for visibility */
        if (ch == '\n' || ch == '\r') {
            putchar('\n');
            fflush(stdout);
            s[i++] = '\n';
            break;
        } else if (ch == 8 || ch == 127) { /* Backspace */
            if (i > 0) {
                i--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        } else if (ch >= 32 && ch < 127) { /* Printable ASCII */
            putchar(ch);
            fflush(stdout);
            s[i++] = (char)ch;
        } else if (ch == 3) { /* Ctrl+C */
            printf("^C\n");
            return NULL;
        }
    }
    s[i] = '\0';
    return s;
}

static void demo_workflow(void)
{
    printf("Demo workflow:\n");
    printf("  1. Create GPT: BIOS Boot + ESP + root partition\n");
    printf("  2. Format ESP as FAT32 natively\n");
    printf("  3. Install UEFI boot files into ESP\n");
    printf("  4. Format root as TFS from the live root filesystem\n");
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

    cfg.root_fs = FS_TFS;
    cfg.root_fs_set = 1;
    cfg.install_esp = 1;
    cfg.install_bootloader = 1;
    printf("Selected root filesystem: %s\n", root_fs_name(cfg.root_fs));
    printf("Configuration: %s\n\n", SYSINSTALL_CONFIG_PATH);

    char input[32];  /* Used for disk selection and confirmation */

    /* If target is explicitly provided, skip disk discovery and use it directly */
    disk_info_t *disk = NULL;
    if (cfg.target[0]) {
        /* Validate the target device/file exists */
        if (!path_is_block_or_regular(cfg.target)) {
            printf("ERROR: target '%s' is not a valid block device or regular file.\n", cfg.target);
            return 1;
        }
        /* Create a disk_info entry for the target */
        if (disk_count >= MAX_DISKS) {
            printf("ERROR: too many disks.\n");
            return 1;
        }
        disk = &disks[disk_count];
        memset(disk, 0, sizeof(*disk));
        strncpy(disk->device, cfg.target, sizeof(disk->device) - 1);
        disk->is_nvme = (strstr(cfg.target, "nvme") != NULL);
        disk->size_sectors = get_device_size_sectors(cfg.target);
        disk->size_bytes = disk->size_sectors * SECTOR_SIZE;
        snprintf(disk->model, sizeof(disk->model), "Target %s (%llu MiB)",
                 cfg.target,
                 (unsigned long long)(disk->size_bytes / (1024ULL * 1024ULL)));
        disk_count++;
    } else {
        if (list_disks() == 0) {
            demo_workflow();
            printf("\nNo disk was modified.\n");
            return 0;
        }

        disk = find_configured_disk(cfg.target);
        if (!disk) {
            printf("Select disk [0-%d]: ", disk_count - 1);
            fflush(stdout);

            if (!fgets_with_echo(input, sizeof(input), stdin)) {
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
    }

    printf("\nSelected disk: %s\n", disk->model);
    printf("Root filesystem: %s\n", root_fs_name(cfg.root_fs));
    printf("Hostname: %s\n", cfg.hostname);

    if (!cfg.assume_yes && !cfg.dry_run) {
        printf("\nType ERASE to continue: ");
        fflush(stdout);

        if (!fgets_with_echo(input, sizeof(input), stdin) || strncmp(input, "ERASE", 5) != 0) {
            printf("Installation cancelled.\n");
            return 0;
        }
    }

    return perform_installation(disk->device, disk->size_sectors, &cfg);
}
