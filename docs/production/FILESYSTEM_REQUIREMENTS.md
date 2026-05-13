# Filesystem Requirements for Production Durability

## Overview

DineroCoin storage backends require specific filesystem configurations to guarantee durability in production environments. This document outlines the requirements for different operating systems and filesystems.

## Critical Durability Requirements

### Synchronous Tip Commits
- **Tip updates MUST use `sync=true`** to ensure durability
- All other operations can use `sync=false` for performance
- Power loss during tip commit must not corrupt the blockchain state

### Write Ordering
- Filesystem must respect write ordering with barriers enabled
- WAL (Write-Ahead Log) writes must be durable before data writes
- Tip update is always the last operation in an atomic batch

## Linux Production Requirements

### ext4 (Recommended)
```bash
# Mount options for production
mount -t ext4 -o barrier=1,data=ordered,noatime /dev/sdb1 /var/lib/dinero

# Verify barriers are enabled
tune2fs -l /dev/sdb1 | grep "Filesystem features"
# Should include: has_journal, ext_attr, resize_inode, dir_index, filetype, needs_recovery, extent, flex_bg
```

**Required mount options:**
- `barrier=1` - Enable write barriers (default, but explicit is better)
- `data=ordered` - Ensure data writes before metadata (default)
- `noatime` - Reduce write overhead (optional but recommended)

**Verification:**
```bash
# Check current mount options
mount | grep dinero
# Should show: barrier=1,data=ordered

# Test fsync behavior
echo "test" > /var/lib/dinero/fsync_test
sync
# File should be immediately durable
```

### XFS (Alternative)
```bash
# Mount options for production
mount -t xfs -o noatime,largeio,inode64 /dev/sdb1 /var/lib/dinero

# XFS has barriers enabled by default
xfs_info /var/lib/dinero
```

**XFS advantages:**
- Better performance for large files
- Built-in barriers and write ordering
- Excellent crash recovery

### Avoid These Filesystems
- **tmpfs** - Data lost on reboot
- **NFS without sync** - Network delays can cause data loss
- **FUSE filesystems** - May not respect fsync semantics
- **ext2** - No journaling, poor crash recovery

## Storage Hardware Requirements

### SSD Recommendations
```bash
# Check if SSD supports power-loss protection
smartctl -a /dev/sdb | grep -i "power"
# Look for: Power_Loss_Cap_Test, Power-off_Retract_Count

# Enable write cache if SSD has power-loss protection
hdparm -W1 /dev/sdb  # Only if PLP is confirmed
```

### HDD Considerations
```bash
# Disable write cache for HDDs (safety over performance)
hdparm -W0 /dev/sdb

# Verify write cache status
hdparm -W /dev/sdb
# Should show: write-caching = 0 (off)
```

### RAID Configurations
- **RAID 1/10**: Recommended for durability
- **RAID 5/6**: Acceptable with battery-backed controller
- **RAID 0**: NOT recommended for production
- **Software RAID**: Ensure write-intent bitmap is enabled

## macOS Development Notes

⚠️ **macOS results are NOT representative of Linux production behavior**

### HFS+ Limitations
- Write barriers may not be properly implemented
- fsync behavior differs from Linux
- Not suitable for production deployment

### APFS Behavior
```bash
# Check APFS features
diskutil apfs list
# APFS has better crash consistency than HFS+
```

**Development testing:**
- Use for development and testing only
- Results may not match Linux production behavior
- Always validate on Linux before production deployment

## Verification Procedures

### Pre-Production Checklist
```bash
#!/bin/bash
# Production filesystem verification script

DINERO_DATA_DIR="/var/lib/dinero"

echo "=== Filesystem Verification ==="

# Check filesystem type
FS_TYPE=$(df -T $DINERO_DATA_DIR | tail -1 | awk '{print $2}')
echo "Filesystem type: $FS_TYPE"

# Check mount options
MOUNT_OPTS=$(mount | grep $DINERO_DATA_DIR | sed 's/.*(\(.*\)).*/\1/')
echo "Mount options: $MOUNT_OPTS"

# Verify barriers (ext4)
if [ "$FS_TYPE" = "ext4" ]; then
    if echo "$MOUNT_OPTS" | grep -q "barrier=1\|barrier"; then
        echo "✓ Write barriers enabled"
    else
        echo "✗ Write barriers not confirmed"
        exit 1
    fi
fi

# Test fsync behavior
TEST_FILE="$DINERO_DATA_DIR/fsync_test_$$"
echo "test data" > $TEST_FILE
sync
if [ -f "$TEST_FILE" ]; then
    echo "✓ fsync behavior verified"
    rm -f $TEST_FILE
else
    echo "✗ fsync test failed"
    exit 1
fi

# Check disk space
DISK_USAGE=$(df $DINERO_DATA_DIR | tail -1 | awk '{print $5}' | sed 's/%//')
if [ $DISK_USAGE -lt 75 ]; then
    echo "✓ Disk usage: ${DISK_USAGE}%"
else
    echo "⚠ Disk usage high: ${DISK_USAGE}%"
fi

echo "=== Verification Complete ==="
```

### Runtime Monitoring
```bash
# Monitor filesystem health
iostat -x 1 10  # Check for high wait times
iotop -a        # Monitor I/O patterns

# Check for filesystem errors
dmesg | grep -i "error\|corruption"
journalctl -u dinero | grep -i "storage\|corruption"
```

## Power Loss Testing

### VM-Based Testing
```bash
# Create test VM with proper storage
qemu-system-x86_64 \
  -drive file=dinero-test.qcow2,format=qcow2,cache=none \
  -m 4G -smp 2

# Inside VM: run dinero with test workload
# Outside VM: simulate power loss
virsh destroy dinero-test-vm
```

### Container Testing
```bash
# Use Docker with proper volume mounts
docker run -v /var/lib/dinero:/data:Z \
  --mount type=bind,source=/var/lib/dinero,target=/data,bind-propagation=shared \
  dinero:latest

# Simulate container kill
docker kill -s KILL dinero-container
```

## Troubleshooting

### Common Issues

**Symptom**: Database corruption after restart
```bash
# Check filesystem errors
fsck -n /dev/sdb1  # Read-only check

# Check mount options
mount | grep dinero
# Ensure barriers are enabled
```

**Symptom**: Poor write performance
```bash
# Check I/O scheduler
cat /sys/block/sdb/queue/scheduler
# For SSD: [noop] or [deadline]
# For HDD: [cfq] or [deadline]

# Tune I/O scheduler
echo deadline > /sys/block/sdb/queue/scheduler
```

**Symptom**: fsync taking too long
```bash
# Check write cache settings
hdparm -W /dev/sdb

# Monitor fsync latency
iotop -a | grep fsync
```

### Recovery Procedures

**Database won't start after power loss:**
1. Check filesystem integrity: `fsck -f /dev/sdb1`
2. Check dinero logs for corruption messages
3. If corruption detected, restore from backup
4. Verify filesystem mount options before restart

## Security Considerations

### At-Rest Encryption
- Use LUKS for full-disk encryption
- Ensure encryption doesn't disable write barriers
- Test fsync behavior with encryption enabled

```bash
# Setup LUKS encryption
cryptsetup luksFormat /dev/sdb1
cryptsetup luksOpen /dev/sdb1 dinero-encrypted
mkfs.ext4 /dev/mapper/dinero-encrypted
mount -o barrier=1,data=ordered /dev/mapper/dinero-encrypted /var/lib/dinero
```

### File Permissions
```bash
# Secure data directory
chown -R dinero:dinero /var/lib/dinero
chmod 700 /var/lib/dinero
```

## References

- [Linux fsync(2) man page](https://man7.org/linux/man-pages/man2/fsync.2.html)
- [ext4 documentation](https://www.kernel.org/doc/Documentation/filesystems/ext4.txt)
- [XFS documentation](https://xfs.wiki.kernel.org/)
- [PostgreSQL durability documentation](https://www.postgresql.org/docs/current/wal-reliability.html)
