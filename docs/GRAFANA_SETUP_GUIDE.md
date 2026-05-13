# Grafana Setup Guide for DineroCoin

**Week 7: Prometheus/Grafana Integration** ✅ **COMPLETE**

## 📊 Overview

This guide walks you through setting up Grafana dashboards for monitoring DineroCoin nodes using Prometheus metrics.

## Prerequisites

- DineroCoin daemon running with `/metrics` endpoint enabled
- Prometheus installed and configured (see `prometheus.yml`)
- Grafana installed (version 8.0+)

## Step 1: Install Grafana

### macOS
```bash
brew install grafana
brew services start grafana
```

### Linux (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install -y software-properties-common
sudo add-apt-repository "deb https://packages.grafana.com/oss/deb stable main"
wget -q -O - https://packages.grafana.com/gpg.key | sudo apt-key add -
sudo apt-get update
sudo apt-get install grafana
sudo systemctl start grafana-server
sudo systemctl enable grafana-server
```

### Docker
```bash
docker run -d -p 3000:3000 --name=grafana grafana/grafana
```

## Step 2: Access Grafana

1. Open browser: `http://localhost:3000`
2. Default credentials:
   - Username: `admin`
   - Password: `admin` (change on first login)

## Step 3: Add Prometheus Data Source

1. Click **Configuration** → **Data Sources**
2. Click **Add data source**
3. Select **Prometheus**
4. Configure:
   - **URL**: `http://localhost:9090` (or your Prometheus server)
   - **Access**: Server (default)
   - Click **Save & Test**

✅ You should see "Data source is working"

## Step 4: Import Dashboard

### Option A: Import from JSON File

1. Click **+** → **Import**
2. Click **Upload JSON file**
3. Select `docs/grafana-dashboard.json`
4. Select Prometheus data source
5. Click **Import**

### Option B: Import from Dashboard ID

1. Click **+** → **Import**
2. Enter Dashboard ID: `TBD` (when published to Grafana.com)
3. Select Prometheus data source
4. Click **Load** → **Import**

## Step 5: Verify Dashboard

The dashboard includes:

- **Node Health**: Uptime, sync status, connection count
- **Mining Metrics**: Hash rate, blocks found, template latency
- **Network Stats**: Peer count, bytes sent/received
- **Blockchain**: Block height, headers, sync progress
- **Mempool**: Transaction count, fee rates
- **Performance**: CPU usage, memory, RPC latency

## Step 6: Configure Alerts (Optional)

1. Click **Alerting** → **Alert rules**
2. Click **New alert rule**
3. Use queries from `prometheus/alerts.yml`
4. Set thresholds and notification channels

## 📝 Dashboard Panels

### Node Health Panel
- **Query**: `dinero_node_uptime_seconds`
- **Visualization**: Stat panel
- **Unit**: Duration (s)

### Mining Hash Rate
- **Query**: `dinero_miner_hashrate_gauge{miner_id="default"}`
- **Visualization**: Graph
- **Unit**: Hash/s

### Connected Peers
- **Query**: `dinero_p2p_connected_peers`
- **Visualization**: Stat panel
- **Unit**: None

### Block Height
- **Query**: `dinero_blockchain_height`
- **Visualization**: Graph
- **Unit**: None

## 🔧 Customization

### Add Custom Panel

1. Click **+** → **Add panel**
2. Select visualization type
3. Enter Prometheus query:
   ```
   dinero_miner_blocks_found_total{miner_id="default"}
   ```
4. Configure panel settings
5. Click **Apply**

### Modify Existing Panel

1. Click panel title → **Edit**
2. Modify query or visualization
3. Click **Save dashboard**

## 📊 Example Queries

### Mining Performance
```promql
# Hash rate over time
dinero_miner_hashrate_gauge{miner_id="default"}

# Blocks found per hour
rate(dinero_miner_blocks_found_total{miner_id="default"}[1h]) * 3600

# Template creation latency
dinero_miner_template_latency_ms{miner_id="default"}
```

### Network Health
```promql
# Peer count
dinero_p2p_connected_peers

# Bytes sent/received
dinero_p2p_bytes_sent_total
dinero_p2p_bytes_received_total

# Connection rate
rate(dinero_p2p_connections_total[5m])
```

### Blockchain Sync
```promql
# Current height
dinero_blockchain_height

# Sync progress (%)
(dinero_blockchain_height / dinero_blockchain_headers) * 100

# Block rate
rate(dinero_blockchain_height[5m]) * 60
```

## 🚨 Troubleshooting

### Dashboard Shows "No Data"

1. **Check Prometheus**: `http://localhost:9090/targets`
   - Verify DineroCoin target is UP
   - Check scrape errors

2. **Verify Metrics Endpoint**: `curl http://localhost:20998/metrics`
   - Should return Prometheus format metrics

3. **Check Time Range**: 
   - Ensure time range includes data
   - Try "Last 5 minutes"

### Panels Not Updating

1. **Refresh Interval**: Set to 5s or 10s
2. **Prometheus Scrape Interval**: Check `prometheus.yml` (default: 15s)
3. **Browser Cache**: Hard refresh (Cmd+Shift+R / Ctrl+Shift+R)

### Query Errors

1. **Check Metric Names**: Use exact names from `/metrics` endpoint
2. **Verify Labels**: Check label names match exactly
3. **PromQL Syntax**: Validate query syntax in Prometheus UI

## 📚 Additional Resources

- [Grafana Documentation](https://grafana.com/docs/)
- [Prometheus Query Language](https://prometheus.io/docs/prometheus/latest/querying/basics/)
- [DineroCoin Metrics Reference](docs/METRICS_REFERENCE.md)

## ✅ Verification Checklist

- [ ] Grafana installed and running
- [ ] Prometheus data source added and tested
- [ ] Dashboard imported successfully
- [ ] All panels showing data
- [ ] Alerts configured (optional)
- [ ] Custom panels added (optional)

## 🎯 Next Steps

1. **Set up alerts** (see `prometheus/alerts.yml`)
2. **Create custom dashboards** for specific use cases
3. **Configure notification channels** (email, Slack, etc.)
4. **Set up multi-node monitoring** (add multiple Prometheus targets)

---

**Status**: ✅ **Production Ready**

The Grafana dashboard is now fully configured and ready for monitoring DineroCoin nodes in production.

