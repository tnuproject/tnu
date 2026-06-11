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

    for (uint32_t i = 0; i < header->entry_count; i++) {
        const struct tfs_entry *e = &entries[i];
        if (e->path[0] == '\0' || strcmp(e->path, "/") == 0) {
            continue;
        }
        if (e->type == TFS_ENTRY_DIR) {
            vfs_mkdir(e->path, "/", VFS_S_IFDIR | (e->mode & 07777), e->uid, e->gid);
        } else if (e->type == TFS_ENTRY_FILE) {
            if (e->offset + e->size > size) {
                return -1;
            }
            vfs_create_file(e->path, "/", VFS_S_IFREG | (e->mode & 07777), e->uid, e->gid);
            struct vfs_node *node = vfs_lookup(e->path, "/");
            if (node) {
                node->uid = e->uid;
                node->gid = e->gid;
                node->mode = VFS_S_IFREG | (e->mode & 07777);
            }
            vfs_write_file(e->path, "/", (const uint8_t *)image + e->offset, (size_t)e->size);
        }
    }

    log_info("tfs", "mounted image with %u entries", header->entry_count);
    return 0;
}
