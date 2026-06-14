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

    /* ------------------------------------------------------------------ */
    /* Phase 5: hand off to userspace                                      */
    /* ------------------------------------------------------------------ */

    console_banner();
    cpu_sti();
    net_wifi_autoconnect();
    tsh_run();

    panic("tsh returned unexpectedly");
}
