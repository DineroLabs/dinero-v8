#!/bin/bash
# Dinero Node Health Check - Alerts on version drift
# Add to crontab: */30 * * * * ~/DineroCoin/health_check.sh >> ~/dinero-health.log 2>&1

VIRGINIA="173.249.195.59"
CALIFORNIA="172.93.160.131"
USER="root"
ALERT_EMAIL=""  # Set your email for alerts (optional)
SLACK_WEBHOOK=""  # Set Slack webhook URL for alerts (optional)

# Timestamp for logging
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

# Function to get git commit from a server
get_server_commit() {
    local SERVER=$1
    ssh ${USER}@${SERVER} "cd ~/DineroCoin && git rev-parse HEAD 2>/dev/null" || echo "error"
}

# Function to get running daemon version
get_daemon_version() {
    local SERVER=$1
    ssh ${USER}@${SERVER} "~/DineroCoin/build/bin/dinero-cli -rpcport=20998 node.info 2>/dev/null | grep '\"git_commit\"' | cut -d'\"' -f4" || echo "not_running"
}

# Function to get daemon block height
get_block_height() {
    local SERVER=$1
    ssh ${USER}@${SERVER} "~/DineroCoin/build/bin/dinero-cli -rpcport=20998 getblockcount 2>/dev/null" || echo "error"
}

# Function to send alert
send_alert() {
    local MESSAGE="$1"
    
    echo "[$TIMESTAMP] 🚨 ALERT: $MESSAGE"
    
    # Send email if configured
    if [[ -n "$ALERT_EMAIL" ]]; then
        echo "$MESSAGE" | mail -s "Dinero Health Check Alert" "$ALERT_EMAIL" 2>/dev/null || true
    fi
    
    # Send Slack notification if configured
    if [[ -n "$SLACK_WEBHOOK" ]]; then
        curl -X POST -H 'Content-type: application/json' \
            --data "{\"text\":\"🚨 Dinero Alert: $MESSAGE\"}" \
            "$SLACK_WEBHOOK" 2>/dev/null || true
    fi
}

# Get local Mac commit (if running from Mac)
if [[ -d ~/Documents/DineroCoin/.git ]]; then
    LOCAL_COMMIT=$(cd ~/Documents/DineroCoin && git rev-parse HEAD)
else
    LOCAL_COMMIT="unknown"
fi

# Get Virginia status
VA_REPO_COMMIT=$(get_server_commit "$VIRGINIA")
VA_DAEMON_VERSION=$(get_daemon_version "$VIRGINIA")
VA_BLOCK_HEIGHT=$(get_block_height "$VIRGINIA")

# Get California status
CA_REPO_COMMIT=$(get_server_commit "$CALIFORNIA")
CA_DAEMON_VERSION=$(get_daemon_version "$CALIFORNIA")
CA_BLOCK_HEIGHT=$(get_block_height "$CALIFORNIA")

# Log status
echo "[$TIMESTAMP] Health Check:"
echo "  Local (Mac):         ${LOCAL_COMMIT:0:12}"
echo "  Virginia (repo):     ${VA_REPO_COMMIT:0:12}"
echo "  Virginia (daemon):   ${VA_DAEMON_VERSION:0:12} @ height $VA_BLOCK_HEIGHT"
echo "  California (repo):   ${CA_REPO_COMMIT:0:12}"
echo "  California (daemon): ${CA_DAEMON_VERSION:0:12} @ height $CA_BLOCK_HEIGHT"

# Check for issues
ISSUES_FOUND=false

# Check if daemons are running
if [[ "$VA_DAEMON_VERSION" == "not_running" ]]; then
    send_alert "Virginia daemon is NOT RUNNING"
    ISSUES_FOUND=true
fi

if [[ "$CA_DAEMON_VERSION" == "not_running" ]]; then
    send_alert "California daemon is NOT RUNNING"
    ISSUES_FOUND=true
fi

# Check if repos are synced
if [[ "$VA_REPO_COMMIT" != "$CA_REPO_COMMIT" ]] && [[ "$VA_REPO_COMMIT" != "error" ]] && [[ "$CA_REPO_COMMIT" != "error" ]]; then
    send_alert "Virginia and California repos are OUT OF SYNC (VA: ${VA_REPO_COMMIT:0:12}, CA: ${CA_REPO_COMMIT:0:12})"
    ISSUES_FOUND=true
fi

# Check if daemons are running old versions
if [[ "$VA_DAEMON_VERSION" != "not_running" ]] && [[ "$VA_DAEMON_VERSION" != "$VA_REPO_COMMIT" ]] && [[ "$VA_DAEMON_VERSION" != "error" ]]; then
    send_alert "Virginia daemon running OLD VERSION (daemon: ${VA_DAEMON_VERSION:0:12}, repo: ${VA_REPO_COMMIT:0:12})"
    ISSUES_FOUND=true
fi

if [[ "$CA_DAEMON_VERSION" != "not_running" ]] && [[ "$CA_DAEMON_VERSION" != "$CA_REPO_COMMIT" ]] && [[ "$CA_DAEMON_VERSION" != "error" ]]; then
    send_alert "California daemon running OLD VERSION (daemon: ${CA_DAEMON_VERSION:0:12}, repo: ${CA_REPO_COMMIT:0:12})"
    ISSUES_FOUND=true
fi

# Check if block heights are too far apart (indicates sync issue)
if [[ "$VA_BLOCK_HEIGHT" != "error" ]] && [[ "$CA_BLOCK_HEIGHT" != "error" ]]; then
    BLOCK_DIFF=$((VA_BLOCK_HEIGHT - CA_BLOCK_HEIGHT))
    BLOCK_DIFF=${BLOCK_DIFF#-}  # Absolute value
    
    if [[ $BLOCK_DIFF -gt 10 ]]; then
        send_alert "Block height DIVERGENCE: Virginia @ $VA_BLOCK_HEIGHT, California @ $CA_BLOCK_HEIGHT (diff: $BLOCK_DIFF)"
        ISSUES_FOUND=true
    fi
fi

# Success message if no issues
if [[ "$ISSUES_FOUND" == "false" ]]; then
    echo "[$TIMESTAMP] ✅ All nodes healthy and synchronized"
fi

echo ""

