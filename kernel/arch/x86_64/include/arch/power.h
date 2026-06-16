#ifndef TNU_ARCH_POWER_H
#define TNU_ARCH_POWER_H

/*
 * Power management functions for x86_64
 */

/* Shutdown the system (ACPI power off) */
void power_shutdown(void);

/* Reboot the system (warm reset) */
void power_reboot(void);

#endif
