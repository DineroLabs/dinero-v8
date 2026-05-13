# Dinero Production Monitoring

This document describes the monitoring infrastructure deployed for the Dinero production network.

## Overview

The monitoring system provides:
- **Prometheus-format metrics** for real-time network visibility
- **HTTP endpoints** for easy integration with monitoring tools
- **Self-service** metrics without requiring direct server access

## Metrics Endpoints

### California Node
- **Health Check**: `http://172.93.160.131:9100/health`
- **Metrics**: `http://172.93.160.131:9100/metrics`

### Virginia Node
- **Health Check**: `http://173.249.195.59:9100/health`
- **Metrics**: `http://173.249.195.59:9100/metrics`

## Available Metrics

The metrics exporter provides the following measurements:

- `dinero_block_height` - Current blockchain height
- `dinero_connection_count` - Total number of peer connections
- `dinero_inbound_peers` - Number of inbound connections
- `dinero_outbound_peers` - Number of outbound connections
- `dinero_difficulty` - Current network difficulty
- `dinero_verification_progress` - Sync progress (0.0 to 1.0)
- `dinero_network_hashrate_hs` - Estimated network hashrate
- `dinero_mempool_size` - Transaction count in mempool
- `dinero_mempool_bytes` - Mempool size in bytes
- `dinero_disk_used_mb` - Blockchain data size
- `dinero_disk_available_mb` - Available disk space
- `dinero_node_uptime_seconds` - System uptime
- `dinero_daemon_healthy` - Daemon health status (1=healthy)

## Testing Metrics

View metrics from command line:
```bash
# California
curl http://172.93.160.131:9100/metrics

# Virginia
curl http://173.249.195.59:9100/metrics
```

Check node health:
```bash
# California
curl http://172.93.160.131:9100/health

# Virginia
curl http://173.249.195.59:9100/health
```

## Service Management

The metrics exporter runs as a systemd service on each node.

### Check Status
```bash
systemctl status dinero-metrics
```

### View Logs
```bash
journalctl -u dinero-metrics -f
```

### Restart Service
```bash
systemctl restart dinero-metrics
```

## Integration with Prometheus

To scrape these metrics with Prometheus, add to `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: 'dinero-nodes'
    static_configs:
      - targets:
        - '172.93.160.131:9100'  # California
        - '173.249.195.59:9100'  # Virginia
    scrape_interval: 15s
    scrape_timeout: 10s
```

## Integration with Grafana

1. Add Prometheus as a data source in Grafana
2. Import a dashboard or create custom panels
3. Example PromQL queries:
   - Connection count: `dinero_connection_count`
   - Block height: `dinero_block_height`
   - Peer breakdown: `dinero_inbound_peers` + `dinero_outbound_peers`
   - Disk usage: `dinero_disk_used_mb`

## Files

- `/usr/local/bin/dinero-metrics-exporter.sh` - Metrics collection script
- `/usr/local/bin/dinero-metrics-server.py` - HTTP server
- `/etc/systemd/system/dinero-metrics.service` - Systemd service definition

## Firewall

Port 9100 is open on both production servers for metrics scraping.

## Future Enhancements

- Add alerting rules for low peer count
- Track block propagation time between nodes
- Monitor RPC response times
- Add hardware metrics (CPU, RAM, Network I/O)
- Set up centralized Grafana dashboard

## Troubleshooting

### Metrics endpoint timeout
- Check if daemon is running: `systemctl status dinerod`
- Check RPC connectivity: `dinero-cli getblockcount`
- Increase timeout in metrics server if needed

### Service not starting
- Check logs: `journalctl -u dinero-metrics --no-pager`
- Verify Python 3 is installed: `python3 --version`
- Test script manually: `/usr/local/bin/dinero-metrics-exporter.sh`

---

**Deployed**: October 27, 2025
**Version**: 1.0
**Port**: 9100 (Standard Prometheus node exporter port)
