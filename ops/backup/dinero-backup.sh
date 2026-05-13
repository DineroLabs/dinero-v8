#!/usr/bin/env bash
# 💾 Dinero Online Backup Script
# Performs safe online backups using SQLite .backup command

set -euo pipefail

# Configuration
DINERO_DATA_DIR="${DINERO_DATA_DIR:-/var/lib/dinero}"
BACKUP_BASE_DIR="${BACKUP_BASE_DIR:-/var/backups/dinero}"
RETENTION_DAYS="${RETENTION_DAYS:-14}"
COMPRESS="${COMPRESS:-true}"
VERIFY_BACKUP="${VERIFY_BACKUP:-true}"

# Database files to backup
DATABASES=("blockchain.db" "wallet.db" "mempool.db" "peers.db")

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() {
    echo -e "${BLUE}[$(date +'%Y-%m-%d %H:%M:%S')]${NC} $*"
}

warn() {
    echo -e "${YELLOW}[$(date +'%Y-%m-%d %H:%M:%S')] WARN:${NC} $*"
}

error() {
    echo -e "${RED}[$(date +'%Y-%m-%d %H:%M:%S')] ERROR:${NC} $*"
}

success() {
    echo -e "${GREEN}[$(date +'%Y-%m-%d %H:%M:%S')] SUCCESS:${NC} $*"
}

# Check if running as correct user
check_permissions() {
    if [[ $EUID -eq 0 ]] && [[ "$(stat -c %U "$DINERO_DATA_DIR" 2>/dev/null)" != "root" ]]; then
        warn "Running as root but data directory owned by $(stat -c %U "$DINERO_DATA_DIR")"
        warn "Consider running as the dinero user for better security"
    fi
    
    if [[ ! -r "$DINERO_DATA_DIR" ]]; then
        error "Cannot read data directory: $DINERO_DATA_DIR"
        exit 1
    fi
    
    if [[ ! -w "$BACKUP_BASE_DIR" ]]; then
        error "Cannot write to backup directory: $BACKUP_BASE_DIR"
        exit 1
    fi
}

# Create backup directory with timestamp
create_backup_dir() {
    local timestamp=$(date -u +%Y%m%dT%H%M%SZ)
    BACKUP_DIR="$BACKUP_BASE_DIR/$timestamp"
    
    mkdir -p "$BACKUP_DIR"
    log "Created backup directory: $BACKUP_DIR"
}

# Backup a single database using SQLite online backup
backup_database() {
    local db_name="$1"
    local src_path="$DINERO_DATA_DIR/$db_name"
    local dst_path="$BACKUP_DIR/$db_name"
    
    if [[ ! -f "$src_path" ]]; then
        warn "Database not found: $src_path (skipping)"
        return 0
    fi
    
    log "Backing up $db_name..."
    
    # Use SQLite online backup API - safe even while daemon is running
    if sqlite3 "$src_path" ".backup '$dst_path'"; then
        success "Backed up $db_name"
        
        # Verify backup integrity
        if [[ "$VERIFY_BACKUP" == "true" ]]; then
            if sqlite3 "$dst_path" "PRAGMA integrity_check;" | grep -q "ok"; then
                success "Backup integrity verified: $db_name"
            else
                error "Backup integrity check failed: $db_name"
                return 1
            fi
        fi
        
        # Get file sizes for logging
        local src_size=$(du -h "$src_path" | cut -f1)
        local dst_size=$(du -h "$dst_path" | cut -f1)
        log "$db_name: $src_size → $dst_size"
        
    else
        error "Failed to backup $db_name"
        return 1
    fi
}

# Create backup manifest
create_manifest() {
    local manifest_file="$BACKUP_DIR/MANIFEST.txt"
    
    log "Creating backup manifest..."
    
    cat > "$manifest_file" << EOF
Dinero Database Backup Manifest
===============================

Backup Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Backup Directory: $BACKUP_DIR
Source Directory: $DINERO_DATA_DIR
Hostname: $(hostname)
User: $(whoami)

Database Files:
EOF
    
    for db in "${DATABASES[@]}"; do
        local src_path="$DINERO_DATA_DIR/$db"
        local dst_path="$BACKUP_DIR/$db"
        
        if [[ -f "$dst_path" ]]; then
            local size=$(du -h "$dst_path" | cut -f1)
            local checksum=$(sha256sum "$dst_path" | cut -d' ' -f1)
            echo "  $db: $size (SHA256: $checksum)" >> "$manifest_file"
        else
            echo "  $db: NOT FOUND" >> "$manifest_file"
        fi
    done
    
    # Add system information
    cat >> "$manifest_file" << EOF

System Information:
  Disk Usage: $(df -h "$DINERO_DATA_DIR" | tail -1 | awk '{print $3 "/" $2 " (" $5 " used)"}')
  Load Average: $(uptime | awk -F'load average:' '{print $2}')
  Memory Usage: $(free -h | grep '^Mem:' | awk '{print $3 "/" $2}')

Backup Process:
  Compression: $COMPRESS
  Verification: $VERIFY_BACKUP
  Retention: $RETENTION_DAYS days
EOF
    
    success "Created manifest: $manifest_file"
}

# Compress backup if requested
compress_backup() {
    if [[ "$COMPRESS" != "true" ]]; then
        return 0
    fi
    
    log "Compressing backup..."
    
    local archive_name="$(basename "$BACKUP_DIR").tar.gz"
    local archive_path="$BACKUP_BASE_DIR/$archive_name"
    
    if tar -C "$BACKUP_BASE_DIR" -czf "$archive_path" "$(basename "$BACKUP_DIR")"; then
        # Generate checksum for archive
        local checksum=$(sha256sum "$archive_path" | cut -d' ' -f1)
        echo "$checksum  $archive_name" > "$archive_path.sha256"
        
        # Remove uncompressed directory
        rm -rf "$BACKUP_DIR"
        
        local size=$(du -h "$archive_path" | cut -f1)
        success "Created compressed backup: $archive_name ($size)"
        success "Checksum: $checksum"
        
        BACKUP_DIR="$archive_path"  # Update for cleanup reference
    else
        error "Failed to compress backup"
        return 1
    fi
}

# Clean up old backups
cleanup_old_backups() {
    log "Cleaning up backups older than $RETENTION_DAYS days..."
    
    local deleted_count=0
    
    # Find and delete old backup directories
    while IFS= read -r -d '' old_backup; do
        log "Removing old backup: $(basename "$old_backup")"
        rm -rf "$old_backup"
        ((deleted_count++))
    done < <(find "$BACKUP_BASE_DIR" -maxdepth 1 -type d -name "20*" -mtime +$RETENTION_DAYS -print0 2>/dev/null)
    
    # Find and delete old compressed backups
    while IFS= read -r -d '' old_archive; do
        log "Removing old archive: $(basename "$old_archive")"
        rm -f "$old_archive" "$old_archive.sha256"
        ((deleted_count++))
    done < <(find "$BACKUP_BASE_DIR" -maxdepth 1 -name "*.tar.gz" -mtime +$RETENTION_DAYS -print0 2>/dev/null)
    
    if [[ $deleted_count -gt 0 ]]; then
        success "Cleaned up $deleted_count old backups"
    else
        log "No old backups to clean up"
    fi
}

# Check daemon status
check_daemon_status() {
    log "Checking daemon status..."
    
    # Try to connect to health endpoint
    if curl -sf --max-time 5 "http://127.0.0.1:20998/healthz" >/dev/null 2>&1; then
        success "Daemon is healthy - safe to backup"
    elif systemctl is-active --quiet dinerod 2>/dev/null; then
        warn "Daemon is running but health check failed - proceeding with backup"
    else
        warn "Daemon appears to be stopped - backup will still work"
    fi
}

# Send notification (optional)
send_notification() {
    local status="$1"
    local message="$2"
    
    # Webhook notification (if configured)
    if [[ -n "${WEBHOOK_URL:-}" ]]; then
        local payload="{\"text\":\"Dinero Backup $status: $message\"}"
        curl -sf -X POST -H "Content-Type: application/json" -d "$payload" "$WEBHOOK_URL" >/dev/null 2>&1 || true
    fi
    
    # Email notification (if configured)
    if [[ -n "${EMAIL_TO:-}" ]] && command -v mail >/dev/null 2>&1; then
        echo "$message" | mail -s "Dinero Backup $status" "$EMAIL_TO" || true
    fi
}

# Main backup process
main() {
    log "🚀 Starting Dinero database backup..."
    
    # Pre-flight checks
    check_permissions
    check_daemon_status
    
    # Create backup directory
    create_backup_dir
    
    # Backup each database
    local backup_success=true
    for db in "${DATABASES[@]}"; do
        if ! backup_database "$db"; then
            backup_success=false
        fi
    done
    
    if [[ "$backup_success" != "true" ]]; then
        error "Some databases failed to backup"
        send_notification "FAILED" "One or more databases failed to backup"
        exit 1
    fi
    
    # Create manifest
    create_manifest
    
    # Compress if requested
    compress_backup
    
    # Clean up old backups
    cleanup_old_backups
    
    # Final status
    local backup_size
    if [[ "$COMPRESS" == "true" ]]; then
        backup_size=$(du -h "$BACKUP_DIR" | cut -f1)
    else
        backup_size=$(du -sh "$BACKUP_DIR" | cut -f1)
    fi
    
    success "🎉 Backup completed successfully!"
    success "Backup location: $BACKUP_DIR"
    success "Backup size: $backup_size"
    
    send_notification "SUCCESS" "Backup completed successfully ($backup_size)"
}

# Handle script arguments
case "${1:-backup}" in
    backup)
        main
        ;;
    list)
        log "Available backups in $BACKUP_BASE_DIR:"
        ls -la "$BACKUP_BASE_DIR" | grep -E "(^d|\.tar\.gz$)" || echo "No backups found"
        ;;
    verify)
        if [[ -z "${2:-}" ]]; then
            error "Usage: $0 verify <backup_path>"
            exit 1
        fi
        log "Verifying backup: $2"
        # Implementation for backup verification
        ;;
    help|--help|-h)
        echo "Dinero Database Backup Script"
        echo
        echo "Usage: $0 [command]"
        echo
        echo "Commands:"
        echo "  backup    Perform database backup (default)"
        echo "  list      List available backups"
        echo "  verify    Verify a backup"
        echo "  help      Show this help"
        echo
        echo "Environment Variables:"
        echo "  DINERO_DATA_DIR    Data directory (default: /var/lib/dinero)"
        echo "  BACKUP_BASE_DIR    Backup directory (default: /var/backups/dinero)"
        echo "  RETENTION_DAYS     Keep backups for N days (default: 14)"
        echo "  COMPRESS           Compress backups (default: true)"
        echo "  VERIFY_BACKUP      Verify backup integrity (default: true)"
        echo "  WEBHOOK_URL        Webhook for notifications (optional)"
        echo "  EMAIL_TO           Email for notifications (optional)"
        ;;
    *)
        error "Unknown command: $1"
        echo "Use '$0 help' for usage information"
        exit 1
        ;;
esac
