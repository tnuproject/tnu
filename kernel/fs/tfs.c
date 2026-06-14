#include <tnu/block.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/tfs.h>
#include <tnu/vfs.h>

#define TFS_SECTOR_SIZE 512u
#define TFS_TABLE_ALIGN 4096u
#define TFS_MAX_ENTRIES 1024u

struct gpt_header_min {
    char signature[8];
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
} __attribute__((packed));

struct gpt_entry_min {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} __attribute__((packed));

struct collect_ctx {
    struct tfs_entry *entries;
    uint32_t count;
    uint32_t capacity;
    uint64_t data_cursor;
    uint8_t *image;
    size_t image_size;
    int failed;
};

static bool persistent_enabled;
static bool auto_sync_enabled;
static bool sync_in_progress;
static char persistent_device[16];
static uint64_t persistent_start_lba;
static size_t last_image_size;

static uint64_t align_up64(uint64_t value, uint64_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

static int copy_path_for_node(struct vfs_node *node, char out[TFS_PATH_MAX])
{
    const char *parts[32];
    size_t count = 0;
    struct vfs_node *n = node;

    if (!node || node == vfs_root()) {
        strncpy(out, "/", TFS_PATH_MAX - 1);
        out[TFS_PATH_MAX - 1] = '\0';
        return 0;
    }

    while (n && n != vfs_root() && count < sizeof(parts) / sizeof(parts[0])) {
        parts[count++] = n->name;
        n = n->parent;
    }
    if (count == 0 || count >= sizeof(parts) / sizeof(parts[0])) {
        return -1;
    }

    out[0] = '\0';
    for (size_t i = count; i > 0; i--) {
        if (strlen(out) + strlen(parts[i - 1]) + 2 >= TFS_PATH_MAX) {
            return -1;
        }
        strcat(out, "/");
        strcat(out, parts[i - 1]);
    }
    return 0;
}

static int copy_file_data(struct collect_ctx *ctx, struct vfs_node *node,
                          struct tfs_entry *entry)
{
    if (!node->size) {
        entry->offset = ctx->data_cursor;
        entry->size = 0;
        return 0;
    }
    uint64_t end = ctx->data_cursor + node->size;
    if (end > ctx->image_size || !node->data) {
        ctx->failed = 1;
        return -1;
    }
    entry->offset = ctx->data_cursor;
    entry->size = node->size;
    memcpy(ctx->image + ctx->data_cursor, node->data, (size_t)node->size);
    ctx->data_cursor = align_up64(end, 16);
    return 0;
}

static void collect_node(struct vfs_node *node, void *opaque)
{
    struct collect_ctx *ctx = opaque;
    if (!node || ctx->failed) {
        return;
    }

    /* Runtime pseudo filesystems/devices should be recreated by devfs/procfs,
     * not stored in the root image. */
    char path[TFS_PATH_MAX];
    if (copy_path_for_node(node, path) < 0) {
        ctx->failed = 1;
        return;
    }
    if (strcmp(path, "/dev") == 0 || strncmp(path, "/dev/", 5) == 0 ||
        strcmp(path, "/proc") == 0 || strncmp(path, "/proc/", 6) == 0) {
        if (node->type == VFS_NODE_DIR) {
            vfs_list(node, collect_node, ctx);
        }
        return;
    }
    if (node->type != VFS_NODE_DIR && node->type != VFS_NODE_FILE) {
        return;
    }
    if (ctx->count >= ctx->capacity) {
        ctx->failed = 1;
        return;
    }

    struct tfs_entry *entry = &ctx->entries[ctx->count++];
    memset(entry, 0, sizeof(*entry));
    entry->type = node->type == VFS_NODE_DIR ? TFS_ENTRY_DIR : TFS_ENTRY_FILE;
    entry->mode = node->mode & 07777;
    entry->uid = node->uid;
    entry->gid = node->gid;
    entry->mtime = node->modified;
    strncpy(entry->path, path, sizeof(entry->path) - 1);

    if (node->type == VFS_NODE_FILE) {
        copy_file_data(ctx, node, entry);
    }

    if (node->type == VFS_NODE_DIR) {
        vfs_list(node, collect_node, ctx);
    }
}

static int write_image_to_disk(uint8_t *image, size_t image_size)
{
    size_t rounded = (image_size + TFS_SECTOR_SIZE - 1u) & ~(size_t)(TFS_SECTOR_SIZE - 1u);
    if (rounded > image_size) {
        memset(image + image_size, 0, rounded - image_size);
    }
    if (block_write_lba28(persistent_device, (uint32_t)persistent_start_lba,
                          image, rounded) < 0) {
        return -1;
    }
    block_sync(persistent_device);
    last_image_size = rounded;
    return 0;
}

int tfs_sync(void)
{
    if (!persistent_enabled || !persistent_device[0] || sync_in_progress) {
        return 0;
    }

    sync_in_progress = true;

    uint8_t *image = kmalloc(TFS_SYNC_BUFFER_MAX + TFS_SECTOR_SIZE);
    if (!image) {
        sync_in_progress = false;
        return -1;
    }
    memset(image, 0, TFS_SYNC_BUFFER_MAX + TFS_SECTOR_SIZE);

    struct tfs_header *header = (struct tfs_header *)image;
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, TFS_MAGIC, TFS_MAGIC_LEN);
    header->version = TFS_VERSION;
    header->entries_offset = TFS_TABLE_ALIGN;
    header->data_offset = align_up64(header->entries_offset +
                                     (uint64_t)TFS_MAX_ENTRIES * sizeof(struct tfs_entry),
                                     TFS_TABLE_ALIGN);

    struct collect_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.entries = (struct tfs_entry *)(image + header->entries_offset);
    ctx.capacity = TFS_MAX_ENTRIES;
    ctx.data_cursor = header->data_offset;
    ctx.image = image;
    ctx.image_size = TFS_SYNC_BUFFER_MAX;

    vfs_list(vfs_root(), collect_node, &ctx);
    if (ctx.failed) {
        sync_in_progress = false;
        return -1;
    }

    header->entry_count = ctx.count;
    size_t final_size = (size_t)align_up64(ctx.data_cursor, TFS_SECTOR_SIZE);
    if (final_size < header->data_offset) {
        final_size = (size_t)header->data_offset;
    }

    int rc = write_image_to_disk(image, final_size);
    sync_in_progress = false;
    if (rc == 0) {
        log_info("tfs", "synced %u entries to %s@LBA%llu (%llu KiB)",
                 header->entry_count, persistent_device,
                 (unsigned long long)persistent_start_lba,
                 (unsigned long long)(final_size / 1024));
    } else {
        log_warn("tfs", "sync failed for %s@LBA%llu", persistent_device,
                 (unsigned long long)persistent_start_lba);
    }
    return rc;
}

bool tfs_is_persistent(void)
{
    return persistent_enabled;
}

void tfs_set_auto_sync(bool enabled)
{
    auto_sync_enabled = enabled;
}

int tfs_sync_if_mounted(void)
{
    if (!persistent_enabled || !auto_sync_enabled || sync_in_progress) {
        return 0;
    }
    return tfs_sync();
}

static int mount_entries_from_memory(const void *image, size_t size, bool borrowed)
{
    if (!image || size < sizeof(struct tfs_header)) {
        return -1;
    }
    const struct tfs_header *header = image;
    if (memcmp(header->magic, TFS_MAGIC, TFS_MAGIC_LEN) != 0 ||
        header->version != TFS_VERSION) {
        return -1;
    }
    if (header->entries_offset + header->entry_count * sizeof(struct tfs_entry) > size) {
        return -1;
    }

    const struct tfs_entry *entries =
        (const struct tfs_entry *)((const uint8_t *)image + header->entries_offset);

    uint32_t dirs = 0;
    uint32_t files = 0;
    uint64_t file_bytes = 0;

    for (uint32_t i = 0; i < header->entry_count; i++) {
        const struct tfs_entry *e = &entries[i];
        if (e->path[0] == '\0' || strcmp(e->path, "/") == 0) {
            continue;
        }
        if (e->type == TFS_ENTRY_DIR) {
            vfs_mkdir(e->path, "/", VFS_S_IFDIR | (e->mode & 07777), e->uid, e->gid);
            dirs++;
        } else if (e->type == TFS_ENTRY_FILE) {
            if (e->offset + e->size > size) {
                log_warn("tfs", "file %s exceeds image bounds offset=%llu size=%llu image=%llu",
                         e->path, (unsigned long long)e->offset,
                         (unsigned long long)e->size, (unsigned long long)size);
                return -1;
            }
            vfs_create_file(e->path, "/", VFS_S_IFREG | (e->mode & 07777), e->uid, e->gid);
            struct vfs_node *node = vfs_lookup(e->path, "/");
            if (node) {
                node->uid = e->uid;
                node->gid = e->gid;
                node->mode = VFS_S_IFREG | (e->mode & 07777);
                node->size = e->size;
                node->modified = e->mtime;
                if (e->size) {
                    if (borrowed) {
                        node->data = (uint8_t *)((const uint8_t *)image + e->offset);
                        node->data_borrowed = true;
                    } else {
                        uint8_t *copy = kmalloc((size_t)e->size + 1);
                        if (!copy) {
                            return -1;
                        }
                        memcpy(copy, (const uint8_t *)image + e->offset, (size_t)e->size);
                        copy[e->size] = 0;
                        node->data = copy;
                        node->data_borrowed = false;
                    }
                }
            }
            files++;
            file_bytes += e->size;
        }
    }

    log_info("tfs", "mounted %u dirs, %u files, %llu KiB file data%s",
             dirs, files, (unsigned long long)(file_bytes / 1024),
             borrowed ? " (module)" : " (disk rw)");
    return 0;
}

int tfs_mount_image(const void *image, size_t size)
{
    persistent_enabled = false;
    auto_sync_enabled = false;
    return mount_entries_from_memory(image, size, true);
}

int tfs_mount_disk(const char *device, uint64_t start_lba)
{
    if (!device || !device[0]) {
        return -1;
    }

    uint8_t sector[TFS_SECTOR_SIZE];
    if (block_read(device, start_lba, sector, sizeof(sector)) < 0) {
        return -1;
    }
    const struct tfs_header *hdr = (const struct tfs_header *)sector;
    if (memcmp(hdr->magic, TFS_MAGIC, TFS_MAGIC_LEN) != 0 || hdr->version != TFS_VERSION) {
        return -1;
    }
    if (hdr->entry_count > TFS_MAX_ENTRIES || hdr->entries_offset > TFS_SYNC_BUFFER_MAX ||
        hdr->data_offset > TFS_SYNC_BUFFER_MAX) {
        return -1;
    }

    size_t entries_bytes = hdr->entry_count * sizeof(struct tfs_entry);
    size_t entries_end = (size_t)hdr->entries_offset + entries_bytes;
    uint8_t *entries_buf = kmalloc(entries_bytes ? entries_bytes : 1);
    if (!entries_buf) {
        return -1;
    }
    if (entries_bytes && block_read(device, start_lba + hdr->entries_offset / TFS_SECTOR_SIZE,
                                    entries_buf, entries_bytes) < 0) {
        return -1;
    }

    size_t max_end = entries_end;
    const struct tfs_entry *entries = (const struct tfs_entry *)entries_buf;
    for (uint32_t i = 0; i < hdr->entry_count; i++) {
        if (entries[i].type == TFS_ENTRY_FILE) {
            uint64_t end = entries[i].offset + entries[i].size;
            if (end > max_end) {
                max_end = (size_t)end;
            }
        }
    }
    size_t image_size = (max_end + TFS_SECTOR_SIZE - 1u) & ~(size_t)(TFS_SECTOR_SIZE - 1u);
    if (image_size < hdr->data_offset) {
        image_size = (size_t)hdr->data_offset;
    }
    if (image_size > TFS_SYNC_BUFFER_MAX) {
        return -1;
    }

    uint8_t *image = kmalloc(image_size);
    if (!image) {
        return -1;
    }
    memset(image, 0, image_size);
    if (block_read(device, start_lba, image, image_size) < 0) {
        return -1;
    }

    /* Temporarily disable sync while we populate VFS from the on-disk image. */
    persistent_enabled = false;
    auto_sync_enabled = false;
    if (mount_entries_from_memory(image, image_size, false) < 0) {
        return -1;
    }

    strncpy(persistent_device, device, sizeof(persistent_device) - 1);
    persistent_device[sizeof(persistent_device) - 1] = '\0';
    persistent_start_lba = start_lba;
    last_image_size = image_size;
    persistent_enabled = true;
    auto_sync_enabled = true;
    log_info("tfs", "persistent root mounted from %s@LBA%llu (%llu KiB)",
             persistent_device, (unsigned long long)persistent_start_lba,
             (unsigned long long)(last_image_size / 1024));
    return 0;
}

static int gpt_find_root_lba(const char *device, uint64_t *out_lba)
{
    uint8_t sector[TFS_SECTOR_SIZE];
    if (block_read(device, 1, sector, sizeof(sector)) < 0) {
        return -1;
    }
    const struct gpt_header_min *gpt = (const struct gpt_header_min *)sector;
    if (memcmp(gpt->signature, "EFI PART", 8) != 0 ||
        gpt->partition_entry_size < sizeof(struct gpt_entry_min) ||
        gpt->num_partition_entries < 2) {
        return -1;
    }

    uint8_t entries_sector[TFS_SECTOR_SIZE];
    if (block_read(device, gpt->partition_entry_lba, entries_sector, sizeof(entries_sector)) < 0) {
        return -1;
    }
    const struct gpt_entry_min *entry =
        (const struct gpt_entry_min *)(entries_sector + gpt->partition_entry_size); /* partition #2 */
    if (entry->first_lba == 0) {
        return -1;
    }
    *out_lba = entry->first_lba;
    return 0;
}

int tfs_mount_installed_root(void)
{
    static const char *const candidates[] = { "sda", "sdb", "sdc", "sdd" };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *dev = candidates[i];
        if (!block_device_find(dev)) {
            continue;
        }
        uint64_t lba = TFS_DEFAULT_ROOT_LBA;
        if (gpt_find_root_lba(dev, &lba) < 0) {
            lba = TFS_DEFAULT_ROOT_LBA;
        }
        if (tfs_mount_disk(dev, lba) == 0) {
            return 0;
        }
    }
    return -1;
}
