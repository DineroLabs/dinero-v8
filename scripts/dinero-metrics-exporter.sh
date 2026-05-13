#!/bin/bash
# Dinero Production Metrics Exporter
# Outputs Prometheus-format metrics for monitoring
# Can be served via HTTP for Prometheus scraping or viewed manually

DINERO_CLI="/root/DineroCoin/build/dinero-cli"
DATA_DIR="/root/.dinero"
NODE_NAME="${NODE_NAME:-unknown}"  # Set via environment: NODE_NAME=california or virginia

# Get uptime in seconds
UPTIME_SECONDS=$(awk '{print int($1)}' /proc/uptime 2>/dev/null || echo "0")

# Get disk usage for blockchain data directory
DISK_USED_MB=$(du -sm "$DATA_DIR" 2>/dev/null | awk '{print $1}' || echo "0")
DISK_AVAILABLE_MB=$(df -m "$DATA_DIR" 2>/dev/null | tail -1 | awk '{print $4}' || echo "0")

# Get metrics from RPC
BLOCK_COUNT=$($DINERO_CLI -datadir=$DATA_DIR getblockcount 2>/dev/null || echo "0")
CONNECTION_COUNT=$($DINERO_CLI -datadir=$DATA_DIR getconnectioncount 2>/dev/null || echo "0")

# Get blockchain info for additional metrics
BLOCKCHAIN_INFO=$($DINERO_CLI -datadir=$DATA_DIR getblockchaininfo 2>/dev/null)
if [ -n "$BLOCKCHAIN_INFO" ]; then
    CHAIN=$(echo "$BLOCKCHAIN_INFO" | grep -oP '"chain":\s*"\K[^"]+' || echo "main")
    DIFFICULTY=$(echo "$BLOCKCHAIN_INFO" | grep -oP '"difficulty":\s*\K[0-9.]+' || echo "0")
    VERIFICATION_PROGRESS=$(echo "$BLOCKCHAIN_INFO" | grep -oP '"verificationprogress":\s*\K[0-9.]+' || echo "1.0")
else
    CHAIN="main"
    DIFFICULTY="0"
    VERIFICATION_PROGRESS="1.0"
fi

# Get peer info for detailed peer metrics
PEER_INFO=$($DINERO_CLI -datadir=$DATA_DIR getpeerinfo 2>/dev/null)
if [ -n "$PEER_INFO" ]; then
    INBOUND_PEERS=$(echo "$PEER_INFO" | grep -c '"inbound": true' || echo "0")
    OUTBOUND_PEERS=$(echo "$PEER_INFO" | grep -c '"inbound": false' || echo "0")
else
    INBOUND_PEERS="0"
    OUTBOUND_PEERS="0"
fi

# Get network hashrate estimate (if available)
NETWORK_HASHRATE=$($DINERO_CLI -datadir=$DATA_DIR getnetworkhashps 2>/dev/null || echo "0")

# Get mempool info
MEMPOOL_INFO=$($DINERO_CLI -datadir=$DATA_DIR getmempoolinfo 2>/dev/null)
if [ -n "$MEMPOOL_INFO" ]; then
    MEMPOOL_SIZE=$(echo "$MEMPOOL_INFO" | grep -oP '"size":\s*\K[0-9]+' || echo "0")
    MEMPOOL_BYTES=$(echo "$MEMPOOL_INFO" | grep -oP '"bytes":\s*\K[0-9]+' || echo "0")
else
    MEMPOOL_SIZE="0"
    MEMPOOL_BYTES="0"
fi

# Check if daemon is responsive (0 = healthy, 1 = unhealthy)
if [ "$BLOCK_COUNT" = "0" ] && [ "$CONNECTION_COUNT" = "0" ]; then
    DAEMON_HEALTHY="0"
else
    DAEMON_HEALTHY="1"
fi

# Output metrics in Prometheus format
cat << EOF
# HELP dinero_block_height Current blockchain height
# TYPE dinero_block_height gauge
dinero_block_height{node="$NODE_NAME",chain="$CHAIN"} $BLOCK_COUNT

# HELP dinero_connection_count Number of peer connections
# TYPE dinero_connection_count gauge
dinero_connection_count{node="$NODE_NAME"} $CONNECTION_COUNT

# HELP dinero_inbound_peers Number of inbound peer connections
# TYPE dinero_inbound_peers gauge
dinero_inbound_peers{node="$NODE_NAME"} $INBOUND_PEERS

# HELP dinero_outbound_peers Number of outbound peer connections
# TYPE dinero_outbound_peers gauge
dinero_outbound_peers{node="$NODE_NAME"} $OUTBOUND_PEERS

# HELP dinero_difficulty Current network difficulty
# TYPE dinero_difficulty gauge
dinero_difficulty{node="$NODE_NAME"} $DIFFICULTY

# HELP dinero_verification_progress Blockchain sync progress (0.0 to 1.0)
# TYPE dinero_verification_progress gauge
dinero_verification_progress{node="$NODE_NAME"} $VERIFICATION_PROGRESS

# HELP dinero_network_hashrate_hs Estimated network hashrate in hashes per second
# TYPE dinero_network_hashrate_hs gauge
dinero_network_hashrate_hs{node="$NODE_NAME"} $NETWORK_HASHRATE

# HELP dinero_mempool_size Number of transactions in mempool
# TYPE dinero_mempool_size gauge
dinero_mempool_size{node="$NODE_NAME"} $MEMPOOL_SIZE

# HELP dinero_mempool_bytes Size of mempool in bytes
# TYPE dinero_mempool_bytes gauge
dinero_mempool_bytes{node="$NODE_NAME"} $MEMPOOL_BYTES

# HELP dinero_disk_used_mb Blockchain data directory size in MB
# TYPE dinero_disk_used_mb gauge
dinero_disk_used_mb{node="$NODE_NAME"} $DISK_USED_MB

# HELP dinero_disk_available_mb Available disk space in MB
# TYPE dinero_disk_available_mb gauge
dinero_disk_available_mb{node="$NODE_NAME"} $DISK_AVAILABLE_MB

# HELP dinero_node_uptime_seconds System uptime in seconds
# TYPE dinero_node_uptime_seconds counter
dinero_node_uptime_seconds{node="$NODE_NAME"} $UPTIME_SECONDS

# HELP dinero_daemon_healthy Daemon health status (1=healthy, 0=unhealthy)
# TYPE dinero_daemon_healthy gauge
dinero_daemon_healthy{node="$NODE_NAME"} $DAEMON_HEALTHY

# HELP dinero_exporter_timestamp Unix timestamp of metric collection
# TYPE dinero_exporter_timestamp gauge
dinero_exporter_timestamp{node="$NODE_NAME"} $(date +%s)
EOF
