# Shutdown and Reboot Implementation

## Overview
Implemented full shutdown and reboot functionality for the TNU operating system, including kernel-level power management and userspace commands.

## Implementation Details

### 1. Kernel Power Management (`kernel/arch/x86_64/power.c`)
Created new power management module with:

**Shutdown (`power_shutdown`)**:
- Syncs TFS filesystem before shutdown
- Attempts ACPI shutdown via multiple methods:
  - QEMU (port 0x604)
  - Bochs/old QEMU (port 0xB004)
  - VirtualBox (port 0x4004)
  - APM fallback
- Halts CPU if ACPI fails

**Reboot (`power_reboot`)**:
- Syncs TFS filesystem before reboot
- Disables interrupts
- Tries keyboard controller (8042) reset
- Falls back to ACPI reset (port 0x0CF9)
- Final fallback: triple fault via invalid IDT

### 2. Syscalls
Added two new syscalls to `kernel/include/tnu/syscall.h`:
- `SYS_SHUTDOWN = 44`: System shutdown (requires root)
- `SYS_REBOOT = 45`: System reboot (requires root)

Both syscalls:
- Check root permissions
- Call `tfs_sync_if_mounted()` to ensure data persistence
- Invoke the appropriate power management function

### 3. Userspace Library
Added functions to `userspace/libc`:
- `int shutdown(void)` - syscall wrapper
- `int reboot(void)` - syscall wrapper

Declared in `unistd.h` with note that root privileges are required.

### 4. Coreutils Commands
Updated `userspace/coreutils/tnu-utils.c`:
- `shutdown` command - initiates system shutdown
- `reboot` command - initiates system reboot

Both commands:
- Call `sync()` before the power operation
- Display appropriate error message if not running as root
- Return error code 1 on permission denial

## Usage

### As Root
```bash
# Shutdown the system
shutdown

# Reboot the system  
reboot
```

### As Regular User
Both commands will fail with:
```
shutdown: permission denied (requires root)
```
or
```
reboot: permission denied (requires root)
```

## Security
- Both syscalls check `is_root(proc)` before executing
- Non-root users cannot shutdown or reboot the system
- This prevents denial-of-service attacks from regular user accounts

## Data Safety
- TFS filesystem is automatically synced before shutdown/reboot
- Both kernel syscalls call `tfs_sync_if_mounted()`
- Userspace commands also call `sync()` for extra safety
- Ensures all pending writes are flushed to disk

## Compatibility
Works with:
- QEMU/KVM
- VirtualBox
- Bochs
- Real hardware with ACPI support
- Fallback mechanisms for older systems

## Files Modified
1. `kernel/arch/x86_64/power.c` - New file (power management implementation)
2. `kernel/arch/x86_64/include/arch/power.h` - New file (power management API)
3. `kernel/include/tnu/syscall.h` - Added SYS_SHUTDOWN and SYS_REBOOT
4. `kernel/core/syscall.c` - Added syscall handlers
5. `userspace/libc/include/unistd.h` - Added shutdown() and reboot() declarations
6. `userspace/libc/src/syscall.c` - Added shutdown() and reboot() implementations
7. `userspace/coreutils/tnu-utils.c` - Added shutdown and reboot commands

## Testing
Build and run:
```bash
make clean
make -j4 iso
qemu-system-x86_64 -cdrom build/tiramisu-*.iso -m 512M
```

Then in the system:
```bash
# Login as root
su root

# Test reboot
reboot

# Or test shutdown
shutdown
```

The system should properly sync the filesystem and reboot/shutdown via ACPI.
