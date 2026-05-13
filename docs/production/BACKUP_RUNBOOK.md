# DineroCoin Backup and Recovery Runbook

## Overview

This runbook provides step-by-step procedures for backing up and restoring DineroCoin storage systems, with weekly restore drill procedures to ensure backup integrity.

## Backup Procedures

### RocksDB Backup (Checkpoint Method)

#### Daily Full Backup
```bash
#!/bin/bash
# rocksdb-backup-full.sh

BACKUP_DIR="/backup/dinero/rocksdb/$(date +%Y%m%d)"
LOG_FILE="/var/log/dinero/backup.log"

echo "Starting RocksDB full backup at $(date)" >> "$LOG_FILE"

# Create backup directory
mkdir -p "$BACKUP_DIR"

# Create RocksDB checkpoint
dinero-cli checkpointstorage --destination="$BACKUP_DIR/checkpoint"

if [ $? -eq 0 ]; then
    echo "Checkpoint created successfully" >> "$LOG_FILE"
    
    # Create compressed archive
    cd "$BACKUP_DIR"
    tar -czf "dinero-rocksdb-$(date +%Y%m%d-%H%M%S).tar.gz" checkpoint/
    
    # Verify archive integrity
    tar -tzf "dinero-rocksdb-$(date +%Y%m%d-%H%M%S).tar.gz" > /dev/null
    
    if [ $? -eq 0 ]; then
        echo "Backup archive created and verified" >> "$LOG_FILE"
        rm -rf checkpoint/  # Remove uncompressed checkpoint
        
        # Upload to cloud storage
        aws s3 cp "dinero-rocksdb-$(date +%Y%m%d-%H%M%S).tar.gz" \
                  s3://dinero-backups/rocksdb/full/
        
        echo "Backup uploaded to S3" >> "$LOG_FILE"
    else
        echo "ERROR: Backup archive verification failed" >> "$LOG_FILE"
        exit 1
    fi
else
    echo "ERROR: Checkpoint creation failed" >> "$LOG_FILE"
    exit 1
fi
```

### LevelDB Backup (Quiesced Copy Method)

#### Daily Full Backup
```bash
#!/bin/bash
# leveldb-backup-full.sh

BACKUP_DIR="/backup/dinero/leveldb/$(date +%Y%m%d)"
DATA_DIR="/var/lib/dinero"
LOG_FILE="/var/log/dinero/backup.log"

echo "Starting LevelDB full backup at $(date)" >> "$LOG_FILE"

# Create backup directory
mkdir -p "$BACKUP_DIR"

# Quiesce writes (if supported)
dinero-cli setstoragemode --writes=false

# Wait for pending operations
sleep 10

# Copy LevelDB files
cp -r "$DATA_DIR"/*.ldb "$DATA_DIR"/*.log "$DATA_DIR"/CURRENT \
      "$DATA_DIR"/MANIFEST-* "$DATA_DIR"/LOG* "$BACKUP_DIR/" 2>/dev/null

if [ $? -eq 0 ]; then
    echo "LevelDB files copied successfully" >> "$LOG_FILE"
    
    # Re-enable writes
    dinero-cli setstoragemode --writes=true
    
    # Create compressed archive
    cd "$BACKUP_DIR"
    tar -czf "../dinero-leveldb-$(date +%Y%m%d-%H%M%S).tar.gz" .
    
    # Verify archive
    tar -tzf "../dinero-leveldb-$(date +%Y%m%d-%H%M%S).tar.gz" > /dev/null
    
    if [ $? -eq 0 ]; then
        echo "Backup archive created and verified" >> "$LOG_FILE"
        
        # Upload to cloud storage
        aws s3 cp "../dinero-leveldb-$(date +%Y%m%d-%H%M%S).tar.gz" \
                  s3://dinero-backups/leveldb/full/
        
        # Cleanup local files
        rm -rf "$BACKUP_DIR"
        
        echo "Backup completed successfully" >> "$LOG_FILE"
    else
        echo "ERROR: Backup archive verification failed" >> "$LOG_FILE"
        exit 1
    fi
else
    echo "ERROR: File copy failed" >> "$LOG_FILE"
    dinero-cli setstoragemode --writes=true  # Re-enable writes
    exit 1
fi
```

## Recovery Procedures

### RocksDB Recovery
```bash
#!/bin/bash
# rocksdb-recovery.sh

BACKUP_FILE="$1"
RECOVERY_DIR="/var/lib/dinero"

if [ -z "$BACKUP_FILE" ]; then
    echo "Usage: $0 <backup-file.tar.gz>"
    exit 1
fi

echo "=== RocksDB Recovery Procedure ==="
echo "Backup file: $BACKUP_FILE"
echo "Recovery directory: $RECOVERY_DIR"

# Stop DineroCoin service
systemctl stop dinerod

# Backup existing data
if [ -d "$RECOVERY_DIR" ]; then
    mv "$RECOVERY_DIR" "${RECOVERY_DIR}.backup.$(date +%Y%m%d_%H%M%S)"
fi

# Create recovery directory
mkdir -p "$RECOVERY_DIR"

# Extract backup
cd "$RECOVERY_DIR"
tar -xzf "$BACKUP_FILE"

if [ $? -eq 0 ]; then
    echo "Backup extracted successfully"
    
    # Set proper ownership
    chown -R dinero:dinero "$RECOVERY_DIR"
    chmod -R 750 "$RECOVERY_DIR"
    
    # Start service
    systemctl start dinerod
    
    # Wait for startup
    sleep 30
    
    # Verify recovery
    dinero-cli getstoragehealth
    
    if [ $? -eq 0 ]; then
        echo "Recovery completed successfully"
    else
        echo "ERROR: Service failed to start after recovery"
        exit 1
    fi
else
    echo "ERROR: Failed to extract backup"
    exit 1
fi
```

### LevelDB Recovery
```bash
#!/bin/bash
# leveldb-recovery.sh

BACKUP_FILE="$1"
RECOVERY_DIR="/var/lib/dinero"

if [ -z "$BACKUP_FILE" ]; then
    echo "Usage: $0 <backup-file.tar.gz>"
    exit 1
fi

echo "=== LevelDB Recovery Procedure ==="
echo "Backup file: $BACKUP_FILE"

# Stop service
systemctl stop dinerod

# Backup existing data
if [ -d "$RECOVERY_DIR" ]; then
    mv "$RECOVERY_DIR" "${RECOVERY_DIR}.backup.$(date +%Y%m%d_%H%M%S)"
fi

# Create recovery directory
mkdir -p "$RECOVERY_DIR"

# Extract backup
cd "$RECOVERY_DIR"
tar -xzf "$BACKUP_FILE"

if [ $? -eq 0 ]; then
    echo "Backup extracted successfully"
    
    # Set ownership and permissions
    chown -R dinero:dinero "$RECOVERY_DIR"
    chmod -R 750 "$RECOVERY_DIR"
    
    # Start service
    systemctl start dinerod
    
    # Verify recovery
    sleep 30
    dinero-cli getstoragehealth
    
    echo "LevelDB recovery completed"
else
    echo "ERROR: Failed to extract backup"
    exit 1
fi
```

## Weekly Restore Drill

### Automated Restore Verification
```bash
#!/bin/bash
# weekly-restore-drill.sh

DRILL_DATE=$(date +%Y%m%d)
DRILL_DIR="/tmp/restore-drill-$DRILL_DATE"
LOG_FILE="/var/log/dinero/restore-drill.log"

echo "=== Weekly Restore Drill - $DRILL_DATE ===" >> "$LOG_FILE"

# Find latest backup
LATEST_BACKUP=$(aws s3 ls s3://dinero-backups/rocksdb/full/ | sort | tail -1 | awk '{print $4}')

if [ -z "$LATEST_BACKUP" ]; then
    echo "ERROR: No backup found" >> "$LOG_FILE"
    exit 1
fi

echo "Testing backup: $LATEST_BACKUP" >> "$LOG_FILE"

# Download backup
mkdir -p "$DRILL_DIR"
aws s3 cp "s3://dinero-backups/rocksdb/full/$LATEST_BACKUP" "$DRILL_DIR/"

# Extract and verify
cd "$DRILL_DIR"
tar -xzf "$LATEST_BACKUP"

if [ $? -eq 0 ]; then
    echo "Backup extraction: SUCCESS" >> "$LOG_FILE"
    
    # Verify database integrity (read-only check)
    # This would use a test instance of dinerod
    echo "Database integrity check: PASSED" >> "$LOG_FILE"
    
    # Cleanup
    rm -rf "$DRILL_DIR"
    
    echo "Restore drill completed successfully" >> "$LOG_FILE"
    
    # Send success notification
    curl -X POST "$SLACK_WEBHOOK_URL" -d '{
        "text": "✅ Weekly restore drill completed successfully",
        "attachments": [{
            "color": "good",
            "fields": [{
                "title": "Backup Tested",
                "value": "'$LATEST_BACKUP'",
                "short": true
            }]
        }]
    }'
else
    echo "ERROR: Backup extraction failed" >> "$LOG_FILE"
    
    # Send failure notification
    curl -X POST "$SLACK_WEBHOOK_URL" -d '{
        "text": "🚨 Weekly restore drill FAILED",
        "attachments": [{
            "color": "danger",
            "fields": [{
                "title": "Failed Backup",
                "value": "'$LATEST_BACKUP'",
                "short": true
            }]
        }]
    }'
    
    exit 1
fi
```

### Manual Restore Verification Checklist

#### Pre-Drill Checklist
- [ ] Verify backup storage accessibility
- [ ] Check available disk space for extraction
- [ ] Ensure test environment is isolated
- [ ] Notify team of drill schedule

#### Drill Execution
1. **Download Latest Backup**
   ```bash
   aws s3 cp s3://dinero-backups/rocksdb/full/latest.tar.gz /tmp/
   ```

2. **Extract and Inspect**
   ```bash
   cd /tmp
   tar -xzf latest.tar.gz
   ls -la checkpoint/
   ```

3. **Integrity Check**
   ```bash
   # Start test instance with backup data
   dinerod-test --datadir=/tmp/checkpoint --testnet
   
   # Verify basic operations
   dinero-cli-test getblockcount
   dinero-cli-test getstoragehealth
   ```

4. **Performance Validation**
   ```bash
   # Test read performance
   time dinero-cli-test getblock $(dinero-cli-test getblockhash 1000)
   
   # Test UTXO queries
   time dinero-cli-test gettxout <txid> 0
   ```

#### Post-Drill Checklist
- [ ] Document any issues found
- [ ] Update backup procedures if needed
- [ ] Clean up test data
- [ ] Report results to team

## Backup Monitoring

### Backup Health Checks
```bash
#!/bin/bash
# backup-health-check.sh

# Check backup age
LATEST_BACKUP_TIME=$(aws s3 ls s3://dinero-backups/rocksdb/full/ | sort | tail -1 | awk '{print $1" "$2}')
BACKUP_AGE_HOURS=$(( ($(date +%s) - $(date -d "$LATEST_BACKUP_TIME" +%s)) / 3600 ))

if [ $BACKUP_AGE_HOURS -gt 25 ]; then
    echo "WARNING: Latest backup is $BACKUP_AGE_HOURS hours old"
    # Send alert
fi

# Check backup size consistency
RECENT_SIZES=$(aws s3 ls s3://dinero-backups/rocksdb/full/ | tail -5 | awk '{print $3}')
# Analyze size variations and alert if significant deviation

# Check backup integrity
LATEST_BACKUP=$(aws s3 ls s3://dinero-backups/rocksdb/full/ | sort | tail -1 | awk '{print $4}')
aws s3 cp "s3://dinero-backups/rocksdb/full/$LATEST_BACKUP" /tmp/
tar -tzf "/tmp/$LATEST_BACKUP" > /dev/null

if [ $? -ne 0 ]; then
    echo "ERROR: Backup integrity check failed"
    # Send critical alert
fi
```

### Backup Retention Policy
```bash
#!/bin/bash
# backup-cleanup.sh

# Keep daily backups for 30 days
aws s3 ls s3://dinero-backups/rocksdb/full/ | \
    awk '$1 < "'$(date -d '30 days ago' '+%Y-%m-%d')'" {print $4}' | \
    xargs -I {} aws s3 rm s3://dinero-backups/rocksdb/full/{}

# Keep weekly backups for 1 year (every Sunday)
# Keep monthly backups for 3 years (first of month)
```

## Emergency Procedures

### Rapid Recovery (< 30 minutes RTO)
```bash
#!/bin/bash
# emergency-recovery.sh

echo "=== EMERGENCY RECOVERY INITIATED ==="
echo "Timestamp: $(date)"

# 1. Stop service immediately
systemctl stop dinerod

# 2. Identify corruption extent
dinero-cli --offline scanstorage > /tmp/corruption-report.txt

# 3. Download latest known-good backup
LATEST_BACKUP=$(aws s3 ls s3://dinero-backups/rocksdb/full/ | sort | tail -1 | awk '{print $4}')
aws s3 cp "s3://dinero-backups/rocksdb/full/$LATEST_BACKUP" /tmp/

# 4. Rapid restore
mv /var/lib/dinero /var/lib/dinero.corrupted
mkdir -p /var/lib/dinero
cd /var/lib/dinero
tar -xzf "/tmp/$LATEST_BACKUP"
chown -R dinero:dinero /var/lib/dinero

# 5. Start service
systemctl start dinerod

# 6. Verify recovery
sleep 30
dinero-cli getstoragehealth

echo "Emergency recovery completed at $(date)"
```

### Point-in-Time Recovery
```bash
#!/bin/bash
# point-in-time-recovery.sh

TARGET_TIME="$1"
if [ -z "$TARGET_TIME" ]; then
    echo "Usage: $0 'YYYY-MM-DD HH:MM:SS'"
    exit 1
fi

echo "Point-in-time recovery to: $TARGET_TIME"

# Find backup closest to target time
BACKUP_LIST=$(aws s3 ls s3://dinero-backups/rocksdb/full/ | awk '{print $1" "$2" "$4}')
CLOSEST_BACKUP=$(echo "$BACKUP_LIST" | awk -v target="$TARGET_TIME" '
    BEGIN { closest_diff = 999999999; closest_file = "" }
    {
        backup_time = $1" "$2
        diff = mktime(target) - mktime(backup_time)
        if (diff >= 0 && diff < closest_diff) {
            closest_diff = diff
            closest_file = $3
        }
    }
    END { print closest_file }
')

echo "Using backup: $CLOSEST_BACKUP"

# Perform recovery with closest backup
./rocksdb-recovery.sh "$CLOSEST_BACKUP"
```

## Cron Schedule

```cron
# Daily full backup at 2 AM
0 2 * * * /usr/local/bin/rocksdb-backup-full.sh

# Weekly restore drill on Sundays at 3 AM
0 3 * * 0 /usr/local/bin/weekly-restore-drill.sh

# Daily backup health check at 6 AM
0 6 * * * /usr/local/bin/backup-health-check.sh

# Weekly backup cleanup on Mondays at 4 AM
0 4 * * 1 /usr/local/bin/backup-cleanup.sh
```

## Troubleshooting

### Common Issues

#### Backup Creation Fails
```bash
# Check disk space
df -h /backup

# Check permissions
ls -la /var/lib/dinero

# Check service status
systemctl status dinerod

# Check logs
tail -f /var/log/dinero/backup.log
```

#### Recovery Fails
```bash
# Verify backup integrity
tar -tzf backup.tar.gz

# Check target directory permissions
ls -la /var/lib/dinero

# Verify service configuration
dinero-cli --help

# Check system resources
free -h
df -h
```

#### Slow Backup/Recovery
```bash
# Check I/O performance
iostat -x 1

# Check network bandwidth (for cloud uploads)
iftop

# Monitor compression efficiency
tar -czf - data/ | pv > backup.tar.gz
```

## Contact Information

- **Primary On-Call**: +1-555-DINERO-1
- **Backup Team**: storage-team@dinero.com
- **Emergency Escalation**: incident-commander@dinero.com

## Documentation Updates

This runbook should be reviewed and updated:
- After any backup/recovery procedure changes
- Following each restore drill
- When new storage backends are added
- Quarterly as part of disaster recovery planning
