#include <tnu/linux_compat.h>
#include <tnu/log.h>
#include <tnu/process.h>
#include <tnu/string.h>
#include <tnu/tfs.h>
#include <tnu/vfs.h>

static void mkdir_if_missing(const char *path, uint32_t mode)
{
    if (!vfs_lookup(path, "/")) {
        vfs_mkdir(path, "/", VFS_S_IFDIR | mode, 0, 0);
    }
}

void linux_compat_init(void)
{
    bool restore_auto_sync = tfs_is_persistent();
    if (restore_auto_sync) {
        tfs_set_auto_sync(false);
    }

    mkdir_if_missing("/sys", 0555);
    mkdir_if_missing("/tmp", 01777);
    mkdir_if_missing("/home", 0755);
    mkdir_if_missing("/usr", 0755);
    mkdir_if_missing("/usr/linux", 0755);
    mkdir_if_missing("/usr/linux/bin", 0755);
    mkdir_if_missing("/usr/linux/etc", 0755);
    mkdir_if_missing("/usr/linux/home", 0755);
    mkdir_if_missing("/usr/linux/lib", 0755);
    mkdir_if_missing("/usr/linux/lib64", 0755);
    mkdir_if_missing("/usr/linux/root", 0700);
    mkdir_if_missing("/usr/linux/sbin", 0755);
    mkdir_if_missing("/usr/linux/usr", 0755);
    mkdir_if_missing("/usr/linux/usr/bin", 0755);
    mkdir_if_missing("/usr/linux/usr/lib", 0755);
    mkdir_if_missing("/usr/linux/usr/sbin", 0755);
    mkdir_if_missing("/usr/linux/var", 0755);
    mkdir_if_missing("/usr/linux/var/tmp", 01777);
    mkdir_if_missing("/usr/linux/proc", 0555);
    mkdir_if_missing("/usr/linux/sys", 0555);
    mkdir_if_missing("/usr/linux/dev", 0755);
    mkdir_if_missing("/usr/linux/tmp", 01777);

    /* Create symlinks for common Linux paths inside the chroot */
    /* These will be resolved by the linux_resolve_path function */

    struct vfs_node *profile = vfs_lookup("/usr/linux/README.TNU", "/");
    if (!profile) {
        static const char msg[] =
            "Tiramisu Linux compatibility root.\n"
            "Import a Linux rootfs here, then enter with linux-run when the userspace tool is present.\n";
        vfs_create_file("/usr/linux/README.TNU", "/", VFS_S_IFREG | 0644, 0, 0);
        vfs_write_file("/usr/linux/README.TNU", "/", msg, strlen(msg));
    }

    if (restore_auto_sync) {
        tfs_set_auto_sync(true);
    }

    log_info("linux-compat", "initialized Linux compatibility namespace");
}
