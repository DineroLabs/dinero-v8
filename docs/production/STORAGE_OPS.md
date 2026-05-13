# DineroCoin Storage Operations Guide

## Overview

This comprehensive guide covers operational procedures, monitoring, alerting, and troubleshooting for DineroCoin storage systems in production environments.

## Table of Contents

1. [System Architecture](#system-architecture)
2. [Monitoring and Metrics](#monitoring-and-metrics)
3. [Alerting and Notifications](#alerting-and-notifications)
4. [Operational Procedures](#operational-procedures)
5. [Backup and Recovery](#backup-and-recovery)
6. [Performance Tuning](#performance-tuning)
7. [Troubleshooting](#troubleshooting)
8. [Maintenance Tasks](#maintenance-tasks)
9. [Emergency Procedures](#emergency-procedures)
10. [Capacity Planning](#capacity-planning)

## System Architecture

### Storage Backend Overview

DineroCoin supports multiple storage backends with automatic fallback:

```
Primary: RocksDB (High Performance)
    ↓ (fallback)
Secondary: LevelDB (Bitcoin Core Proven)
    ↓ (fallback)
Tertiary: SQLite (Lightweight)
    ↓ (fallback)
Emergency: Memory (Testing Only)
```

### Key Components

- **StorageInterface**: Abstract storage layer
- **AtomicBlockWriter**: Ensures atomic blockchain operations
- **BackupManager**: Handles backup and restore operations
- **CorruptionContainment**: Detects and handles data corruption
- **SchemaManager**: Manages database schema versioning
- **ConfigSafety**: Validates configuration safety

## Monitoring and Metrics

### Core Storage Metrics

#### Performance Metrics
```bash
# Block processing metrics
storage_block_write_duration_seconds{backend="rocksdb"}
storage_block_read_duration_seconds{backend="rocksdb"}
storage_utxo_operations_total{operation="add|remove|update"}

# Database metrics
storage_compaction_duration_seconds{backend="rocksdb"}
storage_cache_hit_ratio{backend="rocksdb"}
storage_write_stall_duration_seconds{backend="rocksdb"}
```

#### Health Metrics
```bash
# Storage health indicators
storage_backend_available{backend="rocksdb"} # 1=available, 0=unavailable
storage_corruption_events_total{severity="low|medium|high|critical"}
storage_backup_success_total
storage_backup_failure_total
```

#### Resource Metrics
```bash
# Resource utilization
storage_disk_usage_bytes{path="/var/lib/dinero"}
storage_memory_usage_bytes{component="cache|buffers|metadata"}
storage_open_files_count{backend="rocksdb"}
storage_compaction_debt_bytes{backend="rocksdb"}
```

### Metrics Collection Setup

#### Prometheus Configuration
```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'dinero-storage'
    static_configs:
      - targets: ['localhost:8334']
    scrape_interval: 10s
    metrics_path: /metrics
    params:
      format: ['prometheus']
```

#### Grafana Dashboard Queries
```promql
# Storage performance overview
rate(storage_block_write_duration_seconds_sum[5m]) / rate(storage_block_write_duration_seconds_count[5m])

# Compaction debt monitoring
storage_compaction_debt_bytes{backend="rocksdb"}

# Error rate tracking
rate(storage_operation_errors_total[5m])
```

### Custom Metrics Integration

#### Application Metrics
```cpp
// Custom metrics in application code
#include "storage/storage_metrics.h"

// Record custom operation timing
auto timer = g_storage_metrics->startTimer("custom_operation");
// ... perform operation ...
timer.stop();

// Record custom counter
g_storage_metrics->incrementCounter("custom_events", {{"type", "important"}});
```

## Alerting and Notifications

### Critical Alerts

#### Storage Unavailable
```yaml
alert: StorageBackendDown
expr: storage_backend_available == 0
for: 30s
severity: critical
description: "Storage backend {{ $labels.backend }} is unavailable"
action: "Immediate investigation required"
```

#### High Error Rate
```yaml
alert: StorageHighErrorRate
expr: rate(storage_operation_errors_total[5m]) > 0.01
for: 2m
severity: critical
description: "Storage error rate is {{ $value }} errors/sec"
action: "Check logs and consider rollback"
```

#### Corruption Detected
```yaml
alert: StorageCorruptionDetected
expr: increase(storage_corruption_events_total[5m]) > 0
for: 0s
severity: critical
description: "Data corruption detected: {{ $labels.severity }}"
action: "Emergency backup and investigation"
```

### Warning Alerts

#### High Compaction Debt
```yaml
alert: StorageHighCompactionDebt
expr: storage_compaction_debt_bytes > 1073741824  # 1GB
for: 5m
severity: warning
description: "Compaction debt is {{ $value | humanizeBytes }}"
action: "Consider manual compaction"
```

#### Low Disk Space
```yaml
alert: StorageLowDiskSpace
expr: (storage_disk_free_bytes / storage_disk_total_bytes) < 0.1
for: 5m
severity: warning
description: "Disk space is {{ $value | humanizePercent }} full"
action: "Clean up old data or expand storage"
```

#### Cache Hit Rate Low
```yaml
alert: StorageLowCacheHitRate
expr: storage_cache_hit_ratio < 0.8
for: 10m
severity: warning
description: "Cache hit rate is {{ $value | humanizePercent }}"
action: "Consider increasing cache size"
```

### Notification Channels

#### Slack Integration
```bash
# Webhook configuration
SLACK_WEBHOOK_URL="https://hooks.slack.com/services/..."
SLACK_CHANNEL="#storage-alerts"

# Alert message format
{
  "channel": "#storage-alerts",
  "username": "DineroCoin Storage",
  "text": "🚨 CRITICAL: {{ .GroupLabels.alertname }}",
  "attachments": [
    {
      "color": "danger",
      "fields": [
        {
          "title": "Description",
          "value": "{{ .CommonAnnotations.description }}",
          "short": false
        }
      ]
    }
  ]
}
```

#### PagerDuty Integration
```yaml
# PagerDuty routing
route:
  group_by: ['alertname']
  group_wait: 10s
  group_interval: 10s
  repeat_interval: 1h
  receiver: 'pagerduty-storage'
  routes:
  - match:
      severity: critical
    receiver: 'pagerduty-critical'

receivers:
- name: 'pagerduty-storage'
  pagerduty_configs:
  - service_key: 'YOUR_PAGERDUTY_SERVICE_KEY'
    description: '{{ .GroupLabels.alertname }}: {{ .CommonAnnotations.description }}'
```

## Operational Procedures

### Daily Operations

#### Health Check Routine
```bash
#!/bin/bash
# daily-health-check.sh

echo "=== Daily Storage Health Check ==="
date

# Check storage backend status
dinero-cli getstoragehealth

# Check database statistics
dinero-cli getdbstats

# Check recent backup status
dinero-cli getbackupstatus

# Check for corruption events
dinero-cli getcorruptionstatus

# Generate health report
dinero-cli generatehealthreport --timerange=24h
```

#### Performance Monitoring
```bash
#!/bin/bash
# performance-check.sh

# Check current performance metrics
curl -s http://localhost:8334/metrics | grep storage_

# Check for performance degradation
dinero-cli getstoragemetrics --format=json | jq '.performance'

# Check compaction status
dinero-cli getcompactionstatus
```

### Weekly Operations

#### Backup Verification
```bash
#!/bin/bash
# weekly-backup-verification.sh

# List recent backups
dinero-cli listbackups --days=7

# Verify backup integrity
for backup in $(dinero-cli listbackups --format=json | jq -r '.backups[].id'); do
    echo "Verifying backup: $backup"
    dinero-cli verifybackup --backup-id="$backup"
done

# Test restore procedure (on test environment)
if [ "$ENVIRONMENT" = "test" ]; then
    dinero-cli testrestorebackup --latest
fi
```

#### Performance Analysis
```bash
#!/bin/bash
# weekly-performance-analysis.sh

# Generate performance report
dinero-cli generateperformancereport --timerange=7d

# Check for performance trends
dinero-cli analyzestoragemetrics --timerange=7d --format=summary

# Identify slow operations
dinero-cli getslowoperations --threshold=1000ms --timerange=7d
```

### Monthly Operations

#### Capacity Planning Review
```bash
#!/bin/bash
# monthly-capacity-review.sh

# Analyze storage growth
dinero-cli analyzestoragegrowth --timerange=30d

# Project future capacity needs
dinero-cli projectcapacity --timerange=90d

# Review compaction efficiency
dinero-cli analyzecompaction --timerange=30d

# Generate capacity report
dinero-cli generatecapacityreport --timerange=30d
```

## Backup and Recovery

### Backup Procedures

#### Automated Backup Schedule
```bash
# Crontab configuration
# Full backup daily at 2 AM
0 2 * * * /usr/local/bin/dinero-backup-full.sh

# Incremental backup every 4 hours
0 */4 * * * /usr/local/bin/dinero-backup-incremental.sh

# Backup verification daily at 3 AM
0 3 * * * /usr/local/bin/dinero-backup-verify.sh
```

#### Full Backup Script
```bash
#!/bin/bash
# dinero-backup-full.sh

BACKUP_DIR="/backup/dinero/$(date +%Y%m%d)"
LOG_FILE="/var/log/dinero/backup.log"

echo "Starting full backup at $(date)" >> "$LOG_FILE"

# Create backup directory
mkdir -p "$BACKUP_DIR"

# Perform full backup
dinero-cli backupstorage \
    --type=full \
    --destination="$BACKUP_DIR" \
    --verify=true \
    --compress=true \
    --encrypt=true

if [ $? -eq 0 ]; then
    echo "Full backup completed successfully at $(date)" >> "$LOG_FILE"
    
    # Upload to cloud storage
    aws s3 sync "$BACKUP_DIR" s3://dinero-backups/full/$(date +%Y%m%d)/
    
    # Clean up old local backups (keep 7 days)
    find /backup/dinero -type d -mtime +7 -exec rm -rf {} \;
else
    echo "Full backup failed at $(date)" >> "$LOG_FILE"
    # Send alert
    curl -X POST "$SLACK_WEBHOOK_URL" -d '{"text":"🚨 DineroCoin full backup failed"}'
fi
```

#### Incremental Backup Script
```bash
#!/bin/bash
# dinero-backup-incremental.sh

BACKUP_DIR="/backup/dinero/incremental/$(date +%Y%m%d_%H%M)"
LOG_FILE="/var/log/dinero/backup.log"

echo "Starting incremental backup at $(date)" >> "$LOG_FILE"

# Create backup directory
mkdir -p "$BACKUP_DIR"

# Perform incremental backup
dinero-cli backupstorage \
    --type=incremental \
    --destination="$BACKUP_DIR" \
    --verify=true \
    --compress=true

if [ $? -eq 0 ]; then
    echo "Incremental backup completed successfully at $(date)" >> "$LOG_FILE"
    
    # Upload to cloud storage
    aws s3 sync "$BACKUP_DIR" s3://dinero-backups/incremental/$(date +%Y%m%d_%H%M)/
else
    echo "Incremental backup failed at $(date)" >> "$LOG_FILE"
fi
```

### Recovery Procedures

#### Emergency Recovery
```bash
#!/bin/bash
# emergency-recovery.sh

echo "=== EMERGENCY RECOVERY PROCEDURE ==="
echo "Timestamp: $(date)"

# Stop DineroCoin service
systemctl stop dinerod

# Backup current corrupted data
mv /var/lib/dinero /var/lib/dinero.corrupted.$(date +%Y%m%d_%H%M%S)

# Find latest valid backup
LATEST_BACKUP=$(dinero-cli listbackups --format=json | jq -r '.backups[0].path')

echo "Restoring from backup: $LATEST_BACKUP"

# Restore from backup
dinero-cli restorestorage \
    --source="$LATEST_BACKUP" \
    --destination="/var/lib/dinero" \
    --verify=true

if [ $? -eq 0 ]; then
    echo "Recovery completed successfully"
    
    # Start service
    systemctl start dinerod
    
    # Verify recovery
    sleep 30
    dinero-cli getstoragehealth
else
    echo "Recovery failed - manual intervention required"
    exit 1
fi
```

#### Point-in-Time Recovery
```bash
#!/bin/bash
# point-in-time-recovery.sh

TARGET_TIME="$1"
if [ -z "$TARGET_TIME" ]; then
    echo "Usage: $0 <target-time> (format: YYYY-MM-DD HH:MM:SS)"
    exit 1
fi

echo "=== POINT-IN-TIME RECOVERY ==="
echo "Target time: $TARGET_TIME"

# Find backup closest to target time
BACKUP_ID=$(dinero-cli findbackup --time="$TARGET_TIME" --format=json | jq -r '.backup_id')

echo "Using backup: $BACKUP_ID"

# Stop service
systemctl stop dinerod

# Restore from specific backup
dinero-cli restorestorage --backup-id="$BACKUP_ID" --verify=true

# Start service
systemctl start dinerod

echo "Point-in-time recovery completed"
```

## Performance Tuning

### RocksDB Optimization

#### Configuration Tuning
```cpp
// Optimal RocksDB configuration for DineroCoin
StorageConfig config;
config.backend = "rocksdb";
config.cache_size_mb = 2048;           // 2GB cache
config.write_buffer_size_mb = 256;     // 256MB write buffer
config.max_write_buffer_number = 4;    // 4 write buffers
config.target_file_size_base_mb = 64;  // 64MB SST files
config.max_bytes_for_level_base_mb = 512; // 512MB L1
config.compression_enabled = true;
config.bloom_filter_enabled = true;
config.bloom_filter_bits_per_key = 10;
```

#### Compaction Tuning
```bash
# Manual compaction during low-traffic periods
dinero-cli compactstorage --level=0 --wait=true

# Check compaction statistics
dinero-cli getcompactionstats

# Optimize compaction schedule
dinero-cli setcompactionschedule --max-background-jobs=4
```

### LevelDB Optimization

#### Configuration Tuning
```cpp
// Optimal LevelDB configuration
StorageConfig config;
config.backend = "leveldb";
config.cache_size_mb = 512;        // 512MB cache
config.write_buffer_size_mb = 64;  // 64MB write buffer
config.max_open_files = 1000;      // Increase file handles
config.compression_enabled = true;
```

### System-Level Optimization

#### Filesystem Tuning
```bash
# Mount options for optimal performance
mount -o noatime,data=ordered,barrier=1 /dev/sdb1 /var/lib/dinero

# I/O scheduler optimization
echo deadline > /sys/block/sdb/queue/scheduler

# Kernel parameters
echo 'vm.swappiness = 1' >> /etc/sysctl.conf
echo 'vm.dirty_ratio = 15' >> /etc/sysctl.conf
echo 'vm.dirty_background_ratio = 5' >> /etc/sysctl.conf
```

#### Memory Optimization
```bash
# Increase file descriptor limits
echo 'dinero soft nofile 65536' >> /etc/security/limits.conf
echo 'dinero hard nofile 65536' >> /etc/security/limits.conf

# Optimize memory allocation
export MALLOC_ARENA_MAX=4
export MALLOC_MMAP_THRESHOLD_=131072
```

## Troubleshooting

### Common Issues

#### High Write Latency
```bash
# Diagnosis steps
1. Check compaction debt: dinero-cli getdbstats | grep compaction_debt
2. Check write stalls: dinero-cli getstoragemetrics | grep write_stall
3. Check disk I/O: iostat -x 1
4. Check memory usage: free -h

# Solutions
- Increase write buffer size
- Add more background compaction threads
- Upgrade storage hardware
- Reduce write batch sizes
```

#### Cache Miss Rate High
```bash
# Diagnosis
dinero-cli getstoragemetrics | grep cache_hit_ratio

# Solutions
- Increase cache size in configuration
- Analyze access patterns
- Consider cache warming strategies
- Check for memory pressure
```

#### Compaction Falling Behind
```bash
# Diagnosis
dinero-cli getcompactionstats

# Solutions
- Increase max_background_compactions
- Reduce target_file_size_base
- Schedule manual compactions during low traffic
- Consider hardware upgrades
```

### Log Analysis

#### Error Pattern Detection
```bash
# Find storage errors in logs
grep -E "(storage|rocksdb|leveldb)" /var/log/dinero/dinero.log | grep -i error

# Analyze error patterns
awk '/storage.*error/ {print $1, $2, $NF}' /var/log/dinero/dinero.log | sort | uniq -c

# Check for corruption indicators
grep -i "corrupt\|checksum\|invalid" /var/log/dinero/dinero.log
```

#### Performance Analysis
```bash
# Find slow operations
grep "slow.*storage" /var/log/dinero/dinero.log

# Analyze operation timing
awk '/storage.*duration/ {print $NF}' /var/log/dinero/dinero.log | sort -n | tail -20
```

### Debug Commands

#### Storage Debugging
```bash
# Detailed storage status
dinero-cli getstoragehealth --verbose=true

# Database internal state
dinero-cli getdbstats --internal=true

# Corruption scan
dinero-cli scancorruption --full=true

# Performance profiling
dinero-cli profilestorage --duration=60s
```

## Maintenance Tasks

### Regular Maintenance

#### Weekly Tasks
```bash
#!/bin/bash
# weekly-maintenance.sh

# Compact database
dinero-cli compactstorage --wait=true

# Clean up old logs
find /var/log/dinero -name "*.log" -mtime +30 -delete

# Update statistics
dinero-cli updatestats

# Check for schema updates
dinero-cli checkschemaversion
```

#### Monthly Tasks
```bash
#!/bin/bash
# monthly-maintenance.sh

# Full database analysis
dinero-cli analyzestorage --full=true

# Optimize database structure
dinero-cli optimizestorage

# Clean up old backups
find /backup/dinero -type f -mtime +90 -delete

# Update monitoring dashboards
curl -X POST "$GRAFANA_API/dashboards/db" -d @storage-dashboard.json
```

### Upgrade Procedures

#### Storage Backend Upgrade
```bash
#!/bin/bash
# upgrade-storage-backend.sh

NEW_VERSION="$1"
if [ -z "$NEW_VERSION" ]; then
    echo "Usage: $0 <new-version>"
    exit 1
fi

echo "=== STORAGE BACKEND UPGRADE ==="
echo "Target version: $NEW_VERSION"

# Pre-upgrade backup
dinero-cli backupstorage --type=full --tag="pre-upgrade-$NEW_VERSION"

# Stop service
systemctl stop dinerod

# Backup configuration
cp /etc/dinero/dinero.conf /etc/dinero/dinero.conf.backup

# Install new version
# ... installation steps ...

# Migrate schema if needed
dinero-cli migrateschema --target-version="$NEW_VERSION"

# Start service
systemctl start dinerod

# Verify upgrade
dinero-cli getstoragehealth
dinero-cli getschemaversion

echo "Upgrade completed successfully"
```

## Emergency Procedures

### Data Corruption Response

#### Immediate Actions
```bash
#!/bin/bash
# corruption-response.sh

echo "=== DATA CORRUPTION DETECTED ==="
echo "Timestamp: $(date)"

# Stop writes immediately
dinero-cli setstoragemode --writes=false

# Create emergency backup
dinero-cli emergencybackup --destination="/emergency/backup/$(date +%Y%m%d_%H%M%S)"

# Assess corruption extent
dinero-cli scancorruption --full=true > /tmp/corruption-report.txt

# Notify team
curl -X POST "$SLACK_WEBHOOK_URL" -d '{
    "text": "🚨 CRITICAL: Data corruption detected in DineroCoin storage",
    "attachments": [{
        "color": "danger",
        "text": "Immediate investigation required. Writes have been disabled."
    }]
}'

# Generate incident report
dinero-cli generateincidentreport --type=corruption --output=/tmp/incident-report.json
```

#### Recovery Decision Tree
```
Data Corruption Detected
├── Corruption < 1% of data
│   ├── Attempt repair: dinero-cli repairstorage
│   └── Monitor closely
├── Corruption 1-10% of data
│   ├── Restore from latest backup
│   └── Validate restored data
└── Corruption > 10% of data
    ├── Emergency team assembly
    ├── Restore from multiple backup sources
    └── Consider blockchain resync
```

### Service Outage Response

#### Outage Response Checklist
```bash
# 1. Assess situation
dinero-cli ping
systemctl status dinerod
df -h /var/lib/dinero

# 2. Check logs
tail -100 /var/log/dinero/dinero.log

# 3. Attempt restart
systemctl restart dinerod

# 4. If restart fails, check storage
dinero-cli getstoragehealth

# 5. If storage corrupted, initiate recovery
./emergency-recovery.sh

# 6. Notify stakeholders
./notify-outage.sh
```

## Capacity Planning

### Growth Analysis

#### Historical Growth Tracking
```sql
-- Query for storage growth analysis
SELECT 
    DATE(timestamp) as date,
    AVG(storage_size_bytes) as avg_size,
    MAX(storage_size_bytes) as max_size,
    (MAX(storage_size_bytes) - MIN(storage_size_bytes)) as daily_growth
FROM storage_metrics 
WHERE timestamp >= DATE_SUB(NOW(), INTERVAL 90 DAY)
GROUP BY DATE(timestamp)
ORDER BY date;
```

#### Projection Calculations
```python
#!/usr/bin/env python3
# capacity-projection.py

import pandas as pd
import numpy as np
from sklearn.linear_model import LinearRegression

def project_storage_capacity():
    # Load historical data
    df = pd.read_csv('storage_metrics.csv')
    df['timestamp'] = pd.to_datetime(df['timestamp'])
    
    # Calculate daily growth
    daily_growth = df.groupby(df['timestamp'].dt.date)['storage_size_bytes'].max().diff()
    
    # Linear regression for projection
    X = np.arange(len(daily_growth)).reshape(-1, 1)
    y = daily_growth.values
    
    model = LinearRegression()
    model.fit(X, y)
    
    # Project next 90 days
    future_X = np.arange(len(daily_growth), len(daily_growth) + 90).reshape(-1, 1)
    projected_growth = model.predict(future_X)
    
    current_size = df['storage_size_bytes'].iloc[-1]
    projected_size = current_size + projected_growth.sum()
    
    print(f"Current storage size: {current_size / (1024**3):.2f} GB")
    print(f"Projected size in 90 days: {projected_size / (1024**3):.2f} GB")
    print(f"Recommended capacity: {projected_size * 1.5 / (1024**3):.2f} GB")

if __name__ == "__main__":
    project_storage_capacity()
```

### Scaling Recommendations

#### Vertical Scaling Thresholds
```yaml
storage_scaling_thresholds:
  disk_usage:
    warning: 70%    # Start planning expansion
    critical: 85%   # Immediate action required
  
  memory_usage:
    warning: 80%    # Consider cache increase
    critical: 90%   # Immediate memory upgrade
  
  iops:
    warning: 80%    # Monitor performance
    critical: 95%   # Storage upgrade needed
```

#### Horizontal Scaling Considerations
```bash
# Sharding readiness assessment
dinero-cli assesssharding --analyze-keys=true

# Replication setup preparation
dinero-cli preparereplication --mode=master-slave

# Load balancing configuration
dinero-cli configureloadbalancing --strategy=round-robin
```

## Conclusion

This Storage Operations Guide provides comprehensive procedures for managing DineroCoin storage systems in production. Regular adherence to these procedures ensures optimal performance, data integrity, and system reliability.

For additional support or questions, contact the DineroCoin Storage Team at storage-team@dinero.com.

---

**Document Version**: 1.0  
**Last Updated**: $(date +%Y-%m-%d)  
**Next Review**: $(date -d "+3 months" +%Y-%m-%d)
