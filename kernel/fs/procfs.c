#include <arch/cpu.h>
#include <arch/pit.h>
#include <tnu/log.h>
#include <tnu/framebuffer.h>
#include <tnu/memory.h>
#include <tnu/net.h>
#include <tnu/printf.h>
#include <tnu/process.h>
#include <tnu/procfs.h>
#include <tnu/string.h>
#include <tnu/time.h>
#include <tnu/usb.h>
#include <tnu/version.h>
#include <tnu/vfs.h>

static void make_proc(const char *path)
{
    vfs_create_file(path, "/", VFS_S_IFPROC | 0444, 0, 0);
    struct vfs_node *node = vfs_lookup(path, "/");
    if (node) {
        node->type = VFS_NODE_PROC;
        node->mode = VFS_S_IFPROC | 0444;
    }
}

static void proc_process_visitor(const struct process *proc, void *ctx)
{
    char *buf = ctx;
    char line[128];
    ksnprintf(line, sizeof(line), "%d\t%d\t%s\t%s\n", proc->pid, proc->ppid,
              process_state_name(proc->state), proc->name);
    if (strlen(buf) + strlen(line) < 2048) {
        strcat(buf, line);
    }
}

void procfs_refresh(void)
{
    char buf[2048];
    const struct memory_stats *mem = memory_stats_get();

    ksnprintf(buf, sizeof(buf), "%s %s \"%s\" %s\n",
              TNU_NAME, TNU_VERSION, TNU_CODENAME, TNU_ARCH);
    vfs_write_file("/proc/version", "/", buf, strlen(buf));

    ksnprintf(buf, sizeof(buf),
              "MemTotal: %llu kB\nMemUsable: %llu kB\nFramesAllocated: %llu\n"
              "HeapUsed: %llu bytes\nHeapTotal: %llu bytes\n",
              mem->total_bytes / 1024, mem->usable_bytes / 1024,
              mem->allocated_frames, mem->heap_used, mem->heap_size);
    vfs_write_file("/proc/meminfo", "/", buf, strlen(buf));

    ksnprintf(buf, sizeof(buf), "%llu\n", time_uptime_seconds());
    vfs_write_file("/proc/uptime", "/", buf, strlen(buf));

    char cpu[128];
    cpu_get_brand(cpu, sizeof(cpu));
    ksnprintf(buf, sizeof(buf), "model name\t: %s\narch\t\t: %s\n", cpu, TNU_ARCH);
    vfs_write_file("/proc/cpuinfo", "/", buf, strlen(buf));

    const struct framebuffer_info *fb = framebuffer_info();
    const char *fb_kind = fb->kind == FB_KIND_LINEAR ? "linear" :
                          fb->kind == FB_KIND_VGA_TEXT ? "vga-text" : "none";
    ksnprintf(buf, sizeof(buf),
              "type: %s\naddress: %p\nwidth: %u\nheight: %u\npitch: %u\nbpp: %u\n",
              fb_kind, (void *)fb->address, fb->width, fb->height, fb->pitch, fb->bpp);
    vfs_write_file("/proc/framebuffer", "/", buf, strlen(buf));

    strcpy(buf, "Type\tDriver\tPCI\tVendor\tDevice\tHID\n");
    for (size_t i = 0; i < usb_controller_count(); i++) {
        const struct usb_controller_info *usb = usb_controller_get(i);
        char line[160];
        ksnprintf(line, sizeof(line), "%s\t%s\t%02x:%02x.%u\t%04x\t%04x\t%s\n",
                  usb_controller_type_name(usb->type), usb->driver,
                  usb->bus, usb->slot, usb->function, usb->vendor_id,
                  usb->device_id, usb->hid_ready ? "ready" : "not-ready");
        if (strlen(buf) + strlen(line) < sizeof(buf)) {
            strcat(buf, line);
        }
    }
    if (usb_controller_count() == 0) {
        strcat(buf, "none\t-\t-\t-\t-\tnot-ready\n");
    }
    vfs_write_file("/proc/usb", "/", buf, strlen(buf));

    strcpy(buf, "PID\tPPID\tSTATE\tNAME\n");
    process_each(proc_process_visitor, buf);
    vfs_write_file("/proc/processes", "/", buf, strlen(buf));

    strcpy(buf,
           "rootfs / tfs rw 0 0\n"
           "proc /proc proc rw,nosuid,nodev,noexec 0 0\n"
           "devtmpfs /dev devtmpfs rw,nosuid 0 0\n"
           "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n");
    vfs_write_file("/proc/mounts", "/", buf, strlen(buf));

    strcpy(buf, "Iface\tType\tDriver\tState\tLink\tIPv4\tGateway\tRX-pkts\tTX-pkts\tMAC\n");
    for (size_t i = 0; i < net_iface_count(); i++) {
        const struct net_iface *iface = net_iface_get(i);
        char ip[32];
        char gateway[32];
        char line[192];
        net_format_ipv4(iface->ipv4, ip, sizeof(ip));
        net_format_ipv4(iface->gateway, gateway, sizeof(gateway));
        ksnprintf(line, sizeof(line),
                  "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%llu\t%llu\t%02x:%02x:%02x:%02x:%02x:%02x\n",
                  iface->name, net_iface_type_name(iface->type), iface->driver,
                  iface->up ? "up" : "down", iface->link ? "yes" : "no",
                  iface->ipv4 ? ip : "-", iface->gateway ? gateway : "-",
                  iface->rx_packets, iface->tx_packets,
                  iface->mac[0], iface->mac[1], iface->mac[2],
                  iface->mac[3], iface->mac[4], iface->mac[5]);
        if (strlen(buf) + strlen(line) < sizeof(buf)) {
            strcat(buf, line);
        }
    }
    vfs_write_file("/proc/net/dev", "/", buf, strlen(buf));

    strcpy(buf, "Destination\tGateway\tIface\tFlags\tDNS\n");
    strcat(buf, "127.0.0.0\t0.0.0.0\tlo\tU\t-\n");
    for (size_t i = 0; i < net_iface_count(); i++) {
        const struct net_iface *iface = net_iface_get(i);
        char gateway[32];
        char dns[32];
        char line[128];
        if (!iface->gateway || !iface->up) {
            continue;
        }
        net_format_ipv4(iface->gateway, gateway, sizeof(gateway));
        net_format_ipv4(iface->dns_server, dns, sizeof(dns));
        ksnprintf(line, sizeof(line), "0.0.0.0\t%s\t%s\tUG\t%s\n",
                  gateway, iface->name, iface->dns_server ? dns : "-");
        if (strlen(buf) + strlen(line) < sizeof(buf)) {
            strcat(buf, line);
        }
    }
    vfs_write_file("/proc/net/route", "/", buf, strlen(buf));

    ksnprintf(buf, sizeof(buf),
              "TCP sockets: 0\nUDP sockets: no userspace API\nUDP DNS client: %s\nICMP echo: %s\n"
              "External TCP/IP transport: %s\n",
              net_has_external_transport() ? "ready" : "offline",
              net_has_external_transport() ? "ethernet" : "loopback only",
              net_has_external_transport() ? "ready" : "offline");
    vfs_write_file("/proc/net/sockstat", "/", buf, strlen(buf));
}

void procfs_init(void)
{
    vfs_mkdir("/proc", "/", VFS_S_IFDIR | 0555, 0, 0);
    make_proc("/proc/version");
    make_proc("/proc/meminfo");
    make_proc("/proc/uptime");
    make_proc("/proc/cpuinfo");
    make_proc("/proc/framebuffer");
    make_proc("/proc/usb");
    make_proc("/proc/processes");
    vfs_mkdir("/proc/self", "/", VFS_S_IFDIR | 0555, 0, 0);
    vfs_mkdir("/proc/self/fd", "/", VFS_S_IFDIR | 0555, 0, 0);
    make_proc("/proc/mounts");
    vfs_mkdir("/proc/net", "/", VFS_S_IFDIR | 0555, 0, 0);
    make_proc("/proc/net/dev");
    make_proc("/proc/net/route");
    make_proc("/proc/net/sockstat");
    procfs_refresh();
    log_info("procfs", "created process and kernel information files");
}
