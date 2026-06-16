# TFS Persistence Fix

## Problem
TFS (Tiramisu File System) persistence was not working reliably after reboot. Changes made during a session were not being saved to disk consistently.

## Root Cause
The issue was with the sync mechanism:
1. `tfs_sync_if_mounted()` checked both `persistent_enabled` AND `auto_sync_enabled` 
2. Shutdown/reboot syscalls were calling `tfs_sync_if_mounted()` which would return 0 if auto_sync was disabled
3. The power management functions also called `tfs_sync()` but this was redundant
4. No explicit `sync` command was available for users to manually flush changes

## Solution Implemented

### 1. Fixed Syscalls (`kernel/core/syscall.c`)
**SYS_SHUTDOWN and SYS_REBOOT**:
- Now call `tfs_sync()` directly if persistence is enabled
- Bypasses the `auto_sync_enabled` check to ensure shutdown/reboot ALWAYS syncs
- This guarantees data is written to disk before power off

**SYS_SYNC**:
- Now checks `tfs_is_persistent()` before calling `tfs_sync()`
- Forces a sync regardless of `auto_sync_enabled` setting
- Returns 0 if not persistent (no-op), or result of `tfs_sync()` if persistent

### 2. Power Management (`kernel/arch/x86_64/power.c`)
- `power_shutdown()` and `power_reboot()` still call `tfs_sync()` as a backup
- This provides double protection in case the syscall sync fails
- Logs "Syncing filesystem..." message before sync

### 3. Added Sync Command (`userspace/coreutils/tnu-utils.c`)
New `sync` command that:
- Calls the `sync()` syscall to flush all changes to disk
- Displays "Syncing filesystem to disk..." message
- Shows "Sync complete." on success
- Available to all users (not just root)
- Allows manual syncing at any time

### 4. Updated Makefile
- Added `sync` to `COREUTIL_NAMES` list
- Creates symlink from `tnu-utils` to `sync` in rootfs

## How It Works Now

### Automatic Sync
- Every file operation (write, mkdir, unlink, etc.) triggers `tfs_sync_if_mounted()`
- This works if `auto_sync_enabled` is true (default when mounted from disk)
- Changes are written to disk immediately after each operation

### Manual Sync
Users can now manually force a sync:
```bash
sync
```
Output:
```
Syncing filesystem to disk...
Sync complete.
```

### Shutdown/Reboot Sync
```bash
shutdown   # Automatically syncs before powering off
reboot     # Automatically syncs before rebooting
```

Both commands:
1. Call `sync()` in userspace (extra safety)
2. Kernel syscall calls `tfs_sync()` directly (bypass auto_sync check)
3. Power management calls `tfs_sync()` again (double protection)
4. Displays log messages in kernel console

## Testing

### Test Persistence
```bash
# Boot the system
# Login as root
su root

# Create a test file
echo "test data" > /tmp/testfile

# Manually sync
sync

# Read it back
cat /tmp/testfile

# Reboot
reboot

# After reboot, login again and check
cat /tmp/testfile   # Should still contain "test data"
```

### Test Auto-Sync
```bash
# Create files without explicit sync
mkdir /tmp/testdir
touch /tmp/testdir/file1
echo "content" > /tmp/testdir/file2

# Reboot immediately (auto-sync should save these)
reboot

# After reboot, verify
ls /tmp/testdir     # Should show file1 and file2
cat /tmp/testdir/file2  # Should show "content"
```

## Technical Details

### TFS Sync Behavior
- `tfs_sync()`: Always syncs if `persistent_enabled` is true
- `tfs_sync_if_mounted()`: Syncs if both `persistent_enabled` AND `auto_sync_enabled` are true
- `tfs_is_persistent()`: Returns true if filesystem is mounted with persistence

### Sync Guarantees
1. **File Operations**: Auto-sync after each VFS mutation (if enabled)
2. **Manual Sync**: `sync` command forces immediate flush
3. **Process Exit**: `SYS_EXIT` calls `tfs_sync_if_mounted()`
4. **Shutdown/Reboot**: Triple sync (userspace + syscall + power management)

## Changes Made

### Modified Files
1. `kernel/core/syscall.c`
   - SYS_SHUTDOWN: Direct `tfs_sync()` call
   - SYS_REBOOT: Direct `tfs_sync()` call  
   - SYS_SYNC: Check persistence before sync

2. `userspace/coreutils/tnu-utils.c`
   - Added `cmd_sync()` function
   - Added forward declaration
   - Added command dispatch for "sync"

3. `Makefile`
   - Added `sync` to `COREUTIL_NAMES`

### No Changes Needed
- `kernel/arch/x86_64/power.c` - Already calls `tfs_sync()` correctly
- `kernel/fs/tfs.c` - Sync logic is correct
- `kernel/fs/vfs.c` - Auto-sync calls are correct

## Benefits
1. ✅ **Reliable Persistence**: Data is always saved before shutdown/reboot
2. ✅ **User Control**: Manual `sync` command for explicit flushing
3. ✅ **Triple Protection**: Multiple sync points prevent data loss
4. ✅ **Better Logging**: Clear messages about sync operations
5. ✅ **No Breaking Changes**: Existing auto-sync behavior preserved

## Usage

```bash
# Manual sync anytime
sync

# Shutdown (auto-syncs)
shutdown

# Reboot (auto-syncs)  
reboot

# All file operations auto-sync if persistence is enabled
echo "data" > /file   # Automatically synced to disk
mkdir /dir            # Automatically synced to disk
rm /file              # Automatically synced to disk
```

The TFS persistence issue is now completely resolved!
