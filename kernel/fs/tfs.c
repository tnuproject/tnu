#include <tnu/log.h>
#include <tnu/string.h>
#include <tnu/tfs.h>
#include <tnu/vfs.h>

int tfs_mount_image(const void *image, size_t size)
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

    log_info("tfs", "header version=%u entries=%u table=%u image=%llu KiB",
             header->version, header->entry_count, header->entries_offset,
             (unsigned long long)(size / 1024));

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
                node->data = (uint8_t *)((const uint8_t *)image + e->offset);
                node->size = e->size;
                node->modified = e->mtime;
                node->data_borrowed = true;
            }
            files++;
            file_bytes += e->size;
        }
    }

    log_info("tfs", "mounted %u dirs, %u files, %llu KiB file data",
             dirs, files, (unsigned long long)(file_bytes / 1024));
    return 0;
}
