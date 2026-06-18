#ifndef TNU_TFS_H
#define TNU_TFS_H

#include <tnu/types.h>

#define TFS_MAGIC "TFS1TNU"
#define TFS_MAGIC_LEN 8
#define TFS_VERSION 1
#define TFS_PATH_MAX 256

/* Fallback used by sysinstall's current GPT layout when no partition layer exists. */
#define TFS_DEFAULT_ROOT_LBA 526336u
/* Reduce the in‑memory sync buffer to limit RAM consumption.
 * The original 64 MiB buffer contributed significantly to the ~600 MiB usage.
 * An 8 MiB buffer is sufficient for the typical rootfs used in this project
 * while keeping memory footprint low.
 */
/* Restore original buffer size to accommodate the full root TFS image (~250 MiB).
 * A smaller buffer caused sync failures and warnings on boot.
 */
/* Increase buffer to hold the full root TFS image (≈256 MiB) while keeping the
 * overall memory footprint reasonable.
 */
/* Sync buffer size – 64 MiB is sufficient for typical rootfs and keeps RAM
 * consumption modest. A larger buffer caused allocation failures on low‑mem
 * systems during the initial sync.
 */
/* Sync buffer size – 512 MiB gives enough headroom for the full root TFS image
 * (≈250 MiB) plus overhead while still fitting within a system that has about
 * 512 MiB of RAM. The previous 256 MiB buffer was insufficient on this hardware,
 * leading to "initial sync failed … will retry on next write" warnings.
 */
#define TFS_SYNC_BUFFER_MAX  (512u * 1024u * 1024u)

enum tfs_entry_type {
    TFS_ENTRY_DIR = 1,
    TFS_ENTRY_FILE = 2,
};

struct tfs_header {
    char magic[TFS_MAGIC_LEN];
    uint32_t version;
    uint32_t entry_count;
    uint64_t entries_offset;
    uint64_t data_offset;
};

struct tfs_entry {
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t offset;
    uint64_t mtime;
    char path[TFS_PATH_MAX];
};

int tfs_mount_image(const void *image, size_t size);

/* Persistent root support. These functions serialize the in-memory VFS back to
 * the TFS image on disk. They are intentionally simple for now: every VFS
 * mutation rewrites the compact TFS image. This is slower than a real inode
 * allocator, but makes /etc/passwd, /etc/shadow, /home and user files persist. */
int tfs_mount_disk(const char *device, uint64_t start_lba);
int tfs_mount_installed_root(void);
int tfs_sync(void);
bool tfs_is_persistent(void);
void tfs_set_auto_sync(bool enabled);
int tfs_sync_if_mounted(void);
/* Called after a RAM-module boot to attach the on-disk TFS for persistence.
 * Scans known block devices for a GPT root partition whose first sector
 * contains a valid TFS header.  If found, attaches it and enables auto-sync so
 * subsequent changes are persisted.  The initial full-image sync is deferred to
 * avoid blocking boot on real hardware.  Safe to call even if
 * persistent_enabled is already true (it returns 0 immediately in that case). */
int tfs_attach_persistent_disk(void);

#endif
