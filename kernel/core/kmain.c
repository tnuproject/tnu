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

static void early_vga_stage(const char *stage)
{
    static size_t line;
    volatile uint16_t *vga = (volatile uint16_t *)0xb8000;
    size_t row = line++ % 25;
    size_t col = 0;
    const char *prefix = "[TIRAMISU] ";

    while (*prefix && col < 80) {
        vga[row * 80 + col++] = (uint16_t)*prefix++ | 0x0f00;
    }
    while (*stage && col < 80) {
        vga[row * 80 + col++] = (uint16_t)*stage++ | 0x0f00;
    }
    while (col < 80) {
        vga[row * 80 + col++] = (uint16_t)' ' | 0x0f00;
    }
}

static void boot_stage(const char *stage)
{
    serial_write("[Tiramisu] ");
    serial_write(stage);
    serial_write("\n");
    early_vga_stage(stage);
}

void arch_entry(uint32_t magic, uintptr_t mbi_addr)
{
    serial_init();
    log_init();
    boot_stage("KERNEL LOADED");

    boot_info_parse(magic, mbi_addr);
    boot_stage("MULTIBOOT INFO RECEIVED");
    const struct boot_info *boot = boot_info_get();

    heap_init();
    pmm_init(boot);
    vmm_init();
    cpu_init_fpu();
    boot_stage("MEMORY INITIALIZED");

    framebuffer_init();
    console_init();
    console_banner();

    vfs_init();
    if (boot->rootfs.start && boot->rootfs.end > boot->rootfs.start) {
        size_t size = (size_t)(boot->rootfs.end - boot->rootfs.start);
        log_info("rootfs", "module root.tfs at %p-%p (%llu KiB)",
                 (void *)boot->rootfs.start, (void *)boot->rootfs.end,
                 (unsigned long long)(size / 1024));
        if (tfs_mount_image((const void *)boot->rootfs.start, size) < 0) {
            log_warn("rootfs", "root.tfs module was present but not a valid TFS image");
        }
    } else {
        log_warn("rootfs", "no root.tfs module supplied; using built-in empty root");
    }
    devfs_init();
    users_init();
    process_init();
    procfs_init();

    idt_init();
    keyboard_init();
    pit_init(100);
    time_init();
    syscall_init();
    scheduler_init();

    pci_init();
    ata_init();
    ahci_init();
    usb_init();
    net_init();

    cpu_sti();
    tsh_run();

    panic("tsh returned unexpectedly");
}
