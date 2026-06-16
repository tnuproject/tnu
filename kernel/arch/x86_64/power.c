#include <arch/power.h>
#include <arch/cpu.h>
#include <arch/io.h>
#include <tnu/log.h>
#include <tnu/tfs.h>

/* 
 * ACPI power management for x86_64
 * Provides shutdown and reboot functionality
 */

void power_shutdown(void)
{
    log_info("power", "System shutdown requested");
    
    /* Sync filesystem before shutdown */
    if (tfs_is_persistent()) {
        log_info("power", "Syncing filesystem...");
        tfs_sync();
    }
    
    log_info("power", "Goodbye!");
    
    /* Try ACPI shutdown (QEMU/VirtualBox) */
    outw(0x604, 0x2000);  /* QEMU */
    outw(0xB004, 0x2000); /* Bochs/old QEMU */
    
    /* Try VirtualBox power off */
    outw(0x4004, 0x3400);
    
    /* If ACPI fails, try APM */
    outw(0x1000, 0x01 | 0x3000);
    
    /* Last resort: halt CPU */
    log_warn("power", "ACPI shutdown failed, halting CPU");
    cpu_cli();
    while (1) {
        cpu_halt();
    }
}

void power_reboot(void)
{
    log_info("power", "System reboot requested");
    
    /* Sync filesystem before reboot */
    if (tfs_is_persistent()) {
        log_info("power", "Syncing filesystem...");
        tfs_sync();
    }
    
    log_info("power", "Rebooting...");
    
    /* Disable interrupts */
    cpu_cli();
    
    /* Try keyboard controller reboot (8042) */
    uint8_t good = 0x02;
    while (good & 0x02) {
        good = inb(0x64);
    }
    outb(0x64, 0xFE);
    
    /* Wait a bit */
    for (volatile int i = 0; i < 1000000; i++);
    
    /* Try ACPI reset */
    outb(0x0CF9, 0x06);
    
    /* Wait again */
    for (volatile int i = 0; i < 1000000; i++);
    
    /* Triple fault as last resort */
    log_warn("power", "Standard reboot methods failed, attempting triple fault");
    
    /* Load invalid IDT to cause triple fault */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) invalid_idt = { 0, 0 };
    
    __asm__ volatile("lidt %0" : : "m"(invalid_idt));
    __asm__ volatile("int $0x03"); /* Trigger interrupt with invalid IDT */
    
    /* Should never reach here */
    while (1) {
        cpu_halt();
    }
}
