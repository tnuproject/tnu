# TFS Sync Command - User Guide

## Overview
The `sync` command flushes all filesystem changes to disk. This ensures your data is safely written and will survive a reboot.

## Usage
```bash
sync
```

## Output Messages

### Success
```
Syncing filesystem to disk...
Sync complete.
```
This means:
- Filesystem is persistent (mounted from disk)
- All changes have been written to disk successfully
- Data will survive a reboot

### Error
```
Syncing filesystem to disk...
sync: failed (error writing to disk)
```
This means:
- Filesystem is persistent but a write error occurred
- Check kernel logs with `dmesg` for details
- Disk may be full, read-only, or hardware failure

## Understanding Filesystem Persistence

### RAM-Only Mode (Live CD)
When you boot from the ISO without installing:
- Filesystem runs from RAM (root.tfs module)
- Changes are **NOT** persistent
- `sync` returns success but does nothing (no disk to write to)
- All changes are lost on reboot

### Installed System
After running `sysinstall` and rebooting:
- Filesystem is mounted from disk partition
- Changes **ARE** persistent  
- `sync` writes to disk
- Changes survive reboots

## How to Check If Filesystem Is Persistent

### Method 1: Check kernel boot messages
```bash
dmesg | grep tfs
```

Look for:
```
[tfs] persistent root mounted from sda@LBA2048 (1234 KiB)
```
If you see this, filesystem IS persistent.

```
[tfs] mounted 42 dirs, 100 files, 2048 KiB file data (module)
```
If you only see this, filesystem is NOT persistent (RAM only).

### Method 2: Check `/proc/mounts` (if implemented)
```bash
cat /proc/mounts | grep tfs
```

### Method 3: Test persistence
```bash
# Create a test file
echo "persistence test" > /tmp/test.txt

# Sync to disk
sync

# Reboot
reboot

# After reboot, check if file exists
cat /tmp/test.txt
```
If the file exists with the correct content, persistence is working.

## When to Use Sync

### Always Sync Before:
- **Shutdown**: `sync && shutdown`
- **Reboot**: `sync && reboot`  
- **Removing USB drive** (if applicable)
- **Powering off** unexpectedly

### Optionally Sync After:
- Creating important files
- Large file operations
- Before testing potentially unstable software
- Any time you want to ensure data is saved

## Auto-Sync Behavior

The kernel automatically syncs after each file operation when persistence is enabled:
- Creating files: `touch`, `echo >`, etc.
- Writing to files: text editors, file I/O
- Creating directories: `mkdir`
- Deleting files: `rm`, `unlink`
- Changing permissions: `chmod`
- Changing ownership: `chown`

However, manual `sync` provides extra assurance.

## Troubleshooting

### "sync: failed" Error

**Possible Causes:**
1. **Disk write error**: Check `dmesg` for I/O errors
2. **Disk full**: Check available space with `df` (if implemented)
3. **Read-only filesystem**: Disk may be mounted read-only
4. **Hardware failure**: Disk may be failing

**Solutions:**
1. Check kernel logs: `dmesg | tail`
2. Try syncing again: `sync`
3. Free up disk space if possible
4. Reboot and run `fsck` (if available)
5. Check disk hardware

### Sync Returns Success But Changes Are Lost After Reboot

**Diagnosis:**
- Filesystem is probably in RAM-only mode (not persistent)
- You booted from ISO without installing
- Installation wasn't completed properly

**Solution:**
1. Run `sysinstall` to install to disk
2. Reboot from the installed disk (not ISO)
3. Verify persistence: `dmesg | grep "persistent root"`

### Slow Sync

**Normal for:**
- First sync after many changes
- Large files being written
- Slow disk hardware

**Abnormal if:**
- Every sync is slow (disk may be failing)
- System freezes during sync (driver issue)

## Technical Details

### What Sync Actually Does

1. **Collects all VFS changes** - Scans entire filesystem tree
2. **Builds TFS image** - Creates header and entry table
3. **Writes to disk** - Uses block device driver
4. **Flushes disk cache** - Calls `block_sync()`
5. **Logs result** - Records sync statistics

### Sync Return Values
- **0**: Success (or filesystem not persistent - both are OK)
- **-1**: Write error occurred

### Kernel Log Messages

Successful sync:
```
[tfs] synced 42 entries to sda@LBA2048 (1234 KiB)
```

Failed sync:
```
[tfs] sync failed (header) for sda@LBA2048
[tfs] sync data write failed for sda@LBA4096
```

## Examples

### Safe Shutdown
```bash
# Ensure all changes are saved
sync
echo "Filesystem synced, safe to shutdown"
shutdown
```

### After Important Work
```bash
# Make changes
echo "important data" > /important.txt
mkdir /backup
cp /config.txt /backup/

# Sync to be sure
sync

# Verify
cat /important.txt
```

### Install and Verify Persistence
```bash
# Install system
sysinstall
# Follow prompts...

# Reboot to installed system
reboot

# After reboot, verify persistence
dmesg | grep "persistent root"
# Should show: [tfs] persistent root mounted...

# Create test file
echo "test" > /tmp/persist_test.txt
sync

# Reboot again
reboot

# Verify file survived
cat /tmp/persist_test.txt
# Should show: test
```

## Summary

- ✅ `sync` flushes all changes to disk
- ✅ Always returns 0 (success) unless there's a write error
- ✅ Safe to call anytime, even if not persistent
- ✅ Automatic syncing happens on file operations
- ✅ Manual sync provides extra assurance
- ✅ Always sync before shutdown/reboot

For most users: **Just run `sync` before shutting down or rebooting to be safe!**
