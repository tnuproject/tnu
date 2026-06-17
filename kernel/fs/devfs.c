#include <tnu/devfs.h>
#include <tnu/log.h>
#include <tnu/vfs.h>

static void make_dev(const char *path, uint32_t mode)
{
    vfs_create_file(path, "/", VFS_S_IFDEV | mode, 0, 0);
    struct vfs_node *node = vfs_lookup(path, "/");
    if (node) {
        node->type = VFS_NODE_DEV;
        node->mode = VFS_S_IFDEV | mode;
    }
}

void devfs_init(void)
{
    vfs_mkdir("/dev", "/", VFS_S_IFDIR | 0755, 0, 0);
    make_dev("/dev/console", 0600);
    make_dev("/dev/tty", 0600);
    make_dev("/dev/null", 0666);
    make_dev("/dev/zero", 0666);
    make_dev("/dev/random", 0666);
    make_dev("/dev/urandom", 0666);
    make_dev("/dev/fb0", 0600);
    vfs_mkdir("/dev/input", "/", VFS_S_IFDIR | 0755, 0, 0);
    make_dev("/dev/input/kbd", 0600);
    make_dev("/dev/sda", 0600);
    log_info("devfs", "created console, tty, null, zero, random, urandom, fb0, input/kbd, and sda nodes");
}
