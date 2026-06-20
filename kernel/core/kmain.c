#include <arch/tss.h>
#include <arch/cpu.h>
#include <arch/idt.h>
#include <arch/keyboard.h>
#include <arch/pci.h>
#include <arch/pit.h>
#include <arch/serial.h>
#include <tnu/console.h>
#include <tnu/devfs.h>
#include <tnu/drivers.h>
#include <tnu/framebuffer.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/multiboot2.h>
#include <tnu/net.h>
#include <tnu/panic.h>
#include <tnu/printf.h>
#include <tnu/process.h>
#include <tnu/procfs.h>
#include <tnu/scheduler.h>
#include <tnu/shell.h>
#include <tnu/tfs.h>
#include <tnu/time.h>
#include <tnu/user.h>
#include <tnu/version.h>
#include <tnu/vfs.h>

/* Set after console_init() — guards colored output */
static bool g_console_ready = false;

#define BOOT_LINE_WIDTH 56

/*
 * Print a step description padded with dots to BOOT_LINE_WIDTH.
 * Does NOT print a newline — boot_ok/boot_warn/boot_fail close the line.
 */
static void boot_begin(const char *desc)
{
    char buf[BOOT_LINE_WIDTH + 4];
    int n = ksnprintf(buf, sizeof(buf), "%s ", desc);
    while (n < BOOT_LINE_WIDTH - 1) {
        buf[n++] = '.';
    }
    buf[n++] = ' ';
    buf[n]   = '\0';

    serial_write(buf);
    if (g_console_ready) {
        console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
        console_write(buf);
    }
}

static void boot_ok(void)
{
    serial_write("[ DONE ]\n");
    if (g_console_ready) {
        console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
        console_write("[");
        console_set_color(CONSOLE_LIGHT_GREEN, CONSOLE_BLACK);
        console_write(" DONE ");
        console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
        console_write("]\n");
    }
}

static void boot_warn(void)
{
    serial_write("[ WARN ]\n");
    if (g_console_ready) {
        console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
        console_write("[");
        console_set_color(CONSOLE_YELLOW, CONSOLE_BLACK);
        console_write(" WARN ");
        console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
        console_write("]\n");
    }
}

static void __attribute__((unused)) boot_fail(void)
{
    serial_write("[ FAIL ]\n");
    if (g_console_ready) {
        console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
        console_write("[");
        console_set_color(CONSOLE_LIGHT_RED, CONSOLE_BLACK);
        console_write(" FAIL ");
        console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
        console_write("]\n");
    }
}

/* Raw VGA + serial for the very early boot, before the console driver is up */
static void early_boot_msg(const char *msg)
{
    static size_t line;
    volatile uint16_t *vga = (volatile uint16_t *)0xb8000;
    size_t row = line++ % 25;
    size_t col = 0;

    serial_write("[early] ");
    serial_write(msg);
    serial_write("\n");

    const char *p = "[early] ";
    while (*p && col < 80) {
        vga[row * 80 + col++] = (uint16_t)*p++ | 0x0f00;
    }
    p = msg;
    while (*p && col < 80) {
        vga[row * 80 + col++] = (uint16_t)*p++ | 0x0f00;
    }
    while (col < 80) {
        vga[row * 80 + col++] = ' ' | 0x0f00;
    }
}

/* Copy iwlwifi firmware files from /rootfs/lib/firmware/iwlwifi to /lib/firmware/iwlwifi */
static void iwl_copy_firmware_callback(struct vfs_node *node, void *ctx)
{
    const char *dst_dir = (const char *)ctx;
    
    /* Only copy files, not directories */
    if (node->type != VFS_NODE_FILE) return;
    
    /* Build destination path: dst_dir + "/" + node->name */
    char dst_path[256];
    size_t i = 0;
    
    /* Copy dst_dir */
    while (dst_dir[i] && i < 255) {
        dst_path[i] = dst_dir[i];
        i++;
    }
    
    /* Add "/" */
    if (i < 255) dst_path[i++] = '/';
    
    /* Add filename */
    size_t j = 0;
    while (node->name[j] && i < 255 && j < VFS_NAME_MAX) {
        dst_path[i++] = node->name[j++];
    }
    dst_path[i] = '\0';
    
    /* Read source file */
    char *buf = kmalloc(512 * 1024);  /* 512KB max firmware */
    if (!buf) return;
    
    ssize_t bytes_read = vfs_read_node(node, 0, buf, 512 * 1024);
    if (bytes_read <= 0) {
        kfree(buf);
        return;
    }
    
    /* Write to destination */
    if (vfs_write_file(dst_path, "/", buf, bytes_read) == 0) {
        serial_write("[IWL] Copied: ");
        serial_write(node->name);
        serial_write("\n");
    }
    
    kfree(buf);
}

static void iwl_setup_firmware(void)
{
    /* Create destination directory */
    vfs_mkdir("/lib", "/", 0755, 0, 0);
    vfs_mkdir("/lib/firmware", "/", 0755, 0, 0);
    vfs_mkdir("/lib/firmware/iwlwifi", "/", 0755, 0, 0);
    
    /* List source directory and copy each file */
    struct vfs_node *src_dir = vfs_lookup("/rootfs/lib/firmware/iwlwifi", "/");
    if (!src_dir) {
        serial_write("[IWL] Source firmware dir not found, skipping\n");
        return;
    }
    
    vfs_list(src_dir, iwl_copy_firmware_callback, (void *)"/lib/firmware/iwlwifi");
    
    serial_write("[IWL] Firmware setup complete\n");
}

void arch_entry(uint32_t magic, uintptr_t mbi_addr)
{
    serial_init();
    log_init();
    early_boot_msg(TNU_NAME " " TNU_VERSION " (" TNU_ARCH ") starting");

    /* ------------------------------------------------------------------ */
    /* Phase 1: very early — no heap, no console                           */
    /* ------------------------------------------------------------------ */

    early_boot_msg("Parsing multiboot2 info...");
    boot_info_parse(magic, mbi_addr);
    const struct boot_info *boot = boot_info_get();
    early_boot_msg("Multiboot2 info OK");

    early_boot_msg("Initializing memory subsystem...");
    heap_init();
    pmm_init(boot);
    vmm_init();
    cpu_init_fpu();
    tss_init();
    early_boot_msg("Memory subsystem OK");

    /* ------------------------------------------------------------------ */
    /* Phase 2: bring up the console, then pretty-print the rest           */
    /* ------------------------------------------------------------------ */

    framebuffer_init();
    console_init();
    g_console_ready = true;

    boot_begin("Virtual filesystem");
    vfs_init();
    boot_ok();

    boot_begin("Device filesystem");
    devfs_init();
    boot_ok();

    /* ------------------------------------------------------------------ */
    /* Phase 3: arch / interrupt infrastructure                            */
    /* ------------------------------------------------------------------ */

    boot_begin("Interrupt descriptor table");
    idt_init();
    boot_ok();

    boot_begin("PS/2 keyboard");
    keyboard_init();
    boot_ok();

    boot_begin("Programmable interval timer (100 Hz)");
    pit_init(100);
    boot_ok();

    boot_begin("System time");
    time_init();
    boot_ok();

    boot_begin("Syscall interface");
    syscall_init();
    boot_ok();

    boot_begin("Scheduler");
    scheduler_init();
    boot_ok();

    /* ------------------------------------------------------------------ */
    /* Phase 4: device drivers                                             */
    /* ------------------------------------------------------------------ */

    boot_begin("PCI bus scan");
    pci_init();
    boot_ok();

    boot_begin("ATA storage");
    ata_init();
    boot_ok();

    boot_begin("NVMe storage");
    nvme_init();
    boot_ok();

    boot_begin("AHCI storage");
    ahci_init();
    boot_ok();

    boot_begin("USB");
    usb_init();
    boot_ok();

    boot_begin("Root filesystem (TFS)");
    if (tfs_mount_installed_root() == 0) {
        boot_ok();
    } else if (boot->rootfs.start && boot->rootfs.end > boot->rootfs.start) {
        size_t size = (size_t)(boot->rootfs.end - boot->rootfs.start);
        log_info("rootfs", "module root.tfs at %p-%p (%llu KiB)",
                 (void *)boot->rootfs.start, (void *)boot->rootfs.end,
                 (unsigned long long)(size / 1024));
        if (tfs_mount_image((const void *)boot->rootfs.start, size) < 0) {
            log_warn("rootfs", "root.tfs module present but not a valid TFS image");
            boot_warn();
        } else {
            boot_ok();
        }
    } else {
        log_warn("rootfs", "no disk TFS and no root.tfs module supplied; using empty root");
        boot_warn();
    }

    /*
     * Persistence attachment: if we booted from a RAM module (tfs_mount_image)
     * rather than from disk (tfs_mount_installed_root), try to find an
     * installed TFS partition and attach it so changes survive reboots.
     * On a live-CD with no installed disk this returns -1 silently.
     */
    if (!tfs_is_persistent()) {
        // Disabled: TFS persistence is not supported on live systems
        // boot_begin("TFS persistence attach");
        // if (tfs_attach_persistent_disk() == 0) {
        //     boot_ok();
        // } else {
        //     boot_warn();
        // }
    }

    /* Recreate volatile device nodes after mounting root, so /dev is always live. */
    boot_begin("Device filesystem refresh");
    devfs_init();
    boot_ok();

    boot_begin("User accounts");
    users_init();
    boot_ok();

    boot_begin("Process table");
    process_init();
    boot_ok();

    boot_begin("Proc filesystem");
    procfs_init();
    boot_ok();

    boot_begin("Network stack");
    net_init();
    boot_ok();

    /* Copy iwlwifi firmware from /rootfs to /lib for WiFi driver */
    boot_begin("WiFi firmware");
    iwl_setup_firmware();
    boot_ok();

    /* ------------------------------------------------------------------ */
    /* Phase 5: hand off to userspace                                      */
    /* ------------------------------------------------------------------ */

    console_banner();
    cpu_sti();
    tsh_run();

    panic("tsh returned unexpectedly");
}
