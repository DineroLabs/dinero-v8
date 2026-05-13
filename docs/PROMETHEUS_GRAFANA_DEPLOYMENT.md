# Prometheus/Grafana Production Deployment Guide

**Week 7: Production Monitoring Setup** ✅ **COMPLETE**

## 📋 Overview

Complete guide for deploying Prometheus and Grafana to monitor DineroCoin nodes in production.

## 🏗️ Architecture

```
┌─────────────────┐
│  DineroCoin     │
│  Node(s)        │───┐
│  :20998/metrics │   │
└─────────────────┘   │
                       │ HTTP Scrape
┌─────────────────┐   │
│  Prometheus     │◄──┘
│  :9090          │
│  (TSDB)         │───┐
└─────────────────┘   │ Query API
                       │
┌─────────────────┐   │
│  Grafana        │◄──┘
│  :3000          │
│  (Dashboards)   │
└─────────────────┘
```

## 📦 Prerequisites

- DineroCoin daemon(s) running
- Server with 2GB+ RAM for Prometheus/Grafana
- Ports: 9090 (Prometheus), 3000 (Grafana)

## Step 1: Install Prometheus

### Download & Install

```bash
# Download latest Prometheus
cd /tmp
wget https://github.com/prometheus/prometheus/releases/download/v2.45.0/prometheus-2.45.0.linux-amd64.tar.gz
tar xvfz prometheus-2.45.0.linux-amd64.tar.gz
sudo mv prometheus-2.45.0.linux-amd64 /opt/prometheus

# Create prometheus user
sudo useradd --no-create-home --shell /bin/false prometheus
sudo chown -R prometheus:prometheus /opt/prometheus
```

### Systemd Service

Create `/etc/systemd/system/prometheus.service`:

```ini
[Unit]
Description=Prometheus
Wants=network-online.target
After=network-online.target

[Service]
User=prometheus
Group=prometheus
Type=simple
ExecStart=/opt/prometheus/prometheus \
    --config.file=/opt/prometheus/prometheus.yml \
    --storage.tsdb.path=/var/lib/prometheus/ \
    --web.console.templates=/opt/prometheus/consoles \
    --web.console.libraries=/opt/prometheus/console_libraries \
    --web.listen-address=0.0.0.0:9090 \
    --web.enable-lifecycle

Restart=always

[Install]
WantedBy=multi-user.target
```

### Create Directories

```bash
sudo mkdir -p /var/lib/prometheus
sudo chown prometheus:prometheus /var/lib/prometheus
```

### Start Service

```bash
sudo systemctl daemon-reload
sudo systemctl start prometheus
sudo systemctl enable prometheus
sudo systemctl status prometheus
```

## Step 2: Configure Prometheus

### Main Config: `/opt/prometheus/prometheus.yml`

```yaml
global:
  scrape_interval: 15s
  evaluation_interval: 15s
  external_labels:
    cluster: 'dinero-production'
    environment: 'production'

# Alertmanager configuration
alerting:
  alertmanagers:
    - static_configs:
        - targets:
          # - alertmanager:9093

# Load rules
rule_files:
  - "alerts.yml"

# Scrape configurations
scrape_configs:
  # DineroCoin nodes
  - job_name: 'dinerod'
    scrape_interval: 15s
    metrics_path: '/metrics'
    static_configs:
      - targets:
        - 'node1.example.com:20998'
        - 'node2.example.com:20998'
        - 'node3.example.com:20998'
    relabel_configs:
      - source_labels: [__address__]
        target_label: instance
        regex: '([^:]+):\d+'
        replacement: '${1}'

  # Prometheus itself
  - job_name: 'prometheus'
    static_configs:
      - targets: ['localhost:9090']
```

### Alert Rules: `/opt/prometheus/alerts.yml`

Copy from `prometheus/alerts.yml` in DineroCoin repo.

### Verify Configuration

```bash
# Test config
/opt/prometheus/promtool check config /opt/prometheus/prometheus.yml

# Reload config (if running)
curl -X POST http://localhost:9090/-/reload
```

## Step 3: Install Grafana

### Download & Install

```bash
# Add Grafana repository
sudo apt-get install -y software-properties-common
sudo add-apt-repository "deb https://packages.grafana.com/oss/deb stable main"
wget -q -O - https://packages.grafana.com/gpg.key | sudo apt-key add -
sudo apt-get update
sudo apt-get install grafana

# Start service
sudo systemctl start grafana-server
sudo systemctl enable grafana-server
```

### Configure Grafana

Edit `/etc/grafana/grafana.ini`:

```ini
[server]
http_port = 3000
domain = monitoring.example.com

[security]
admin_user = admin
admin_password = <change-me>

[database]
type = sqlite3
path = /var/lib/grafana/grafana.db
```

### Restart Grafana

```bash
sudo systemctl restart grafana-server
```

## Step 4: Configure Grafana

### Access Grafana

1. Open: `http://your-server:3000`
2. Login: `admin` / `<password>`
3. Change password on first login

### Add Prometheus Data Source

1. **Configuration** → **Data Sources** → **Add data source**
2. Select **Prometheus**
3. **URL**: `http://localhost:9090`
4. **Access**: Server
5. Click **Save & Test**

### Import Dashboard

1. **+** → **Import**
2. Upload `docs/grafana-dashboard.json`
3. Select Prometheus data source
4. Click **Import**

## Step 5: Firewall Configuration

```bash
# Allow Prometheus (internal only)
sudo ufw allow from 10.0.0.0/8 to any port 9090

# Allow Grafana (or restrict to VPN)
sudo ufw allow 3000/tcp

# DineroCoin metrics endpoint (internal)
sudo ufw allow from 10.0.0.0/8 to any port 20998
```

## Step 6: SSL/TLS (Production)

### Nginx Reverse Proxy

```nginx
# /etc/nginx/sites-available/grafana
server {
    listen 443 ssl http2;
    server_name monitoring.example.com;

    ssl_certificate /etc/letsencrypt/live/monitoring.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/monitoring.example.com/privkey.pem;

    location / {
        proxy_pass http://localhost:3000;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
```

## Step 7: Multi-Node Setup

### Add Multiple Nodes

Edit `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: 'dinerod'
    static_configs:
      - targets:
        - 'node1.example.com:20998'
        - 'node2.example.com:20998'
        - 'node3.example.com:20998'
        labels:
          datacenter: 'us-east-1'
      - targets:
        - 'node4.example.com:20998'
        - 'node5.example.com:20998'
        labels:
          datacenter: 'us-west-2'
```

### Service Discovery (Optional)

Use file-based discovery for dynamic nodes:

```yaml
scrape_configs:
  - job_name: 'dinerod'
    file_sd_configs:
      - files:
        - '/opt/prometheus/dinero-nodes.json'
        refresh_interval: 5m
```

Create `/opt/prometheus/dinero-nodes.json`:

```json
[
  {
    "targets": ["node1.example.com:20998"],
    "labels": {
      "instance": "node1",
      "datacenter": "us-east-1"
    }
  }
]
```

## Step 8: Backup & Maintenance

### Backup Prometheus Data

```bash
# Backup TSDB
sudo tar -czf prometheus-backup-$(date +%Y%m%d).tar.gz /var/lib/prometheus/

# Backup Grafana
sudo tar -czf grafana-backup-$(date +%Y%m%d).tar.gz /var/lib/grafana/
```

### Retention Policy

Edit Prometheus service:

```ini
ExecStart=/opt/prometheus/prometheus \
    --storage.tsdb.retention.time=30d \
    --storage.tsdb.retention.size=10GB
```

### Monitoring Prometheus

Add Prometheus self-monitoring:

```yaml
scrape_configs:
  - job_name: 'prometheus'
    static_configs:
      - targets: ['localhost:9090']
```

## Step 9: Verification

### Check Prometheus Targets

```bash
curl http://localhost:9090/api/v1/targets
# Should show all DineroCoin nodes as "up"
```

### Check Grafana Dashboard

1. Open Grafana dashboard
2. Verify all panels show data
3. Check time range includes recent data

### Test Alerts

```bash
# Stop a DineroCoin node
ssh node1.example.com "systemctl stop dinerod"

# Wait 1 minute
# Check Prometheus alerts: http://localhost:9090/alerts
# Should see "DineroNodeDown" alert firing
```

## 📊 Performance Tuning

### Prometheus Resource Limits

Edit service:

```ini
[Service]
LimitNOFILE=65536
MemoryLimit=4G
CPUQuota=200%
```

### Grafana Performance

```ini
[dashboards]
default_home_dashboard_path = /var/lib/grafana/dashboards/home.json

[users]
allow_sign_up = false
```

## 🚨 Troubleshooting

### Prometheus Not Scraping

```bash
# Check targets
curl http://localhost:9090/api/v1/targets

# Check logs
sudo journalctl -u prometheus -f

# Test metrics endpoint
curl http://node1.example.com:20998/metrics
```

### Grafana No Data

1. Check Prometheus data source connection
2. Verify time range includes data
3. Check query syntax in panel editor
4. Verify metric names match exactly

### High Memory Usage

```bash
# Reduce retention
--storage.tsdb.retention.time=7d

# Limit series
--storage.tsdb.max-block-duration=2h
```

## ✅ Production Checklist

- [ ] Prometheus installed and running
- [ ] Grafana installed and running
- [ ] All DineroCoin nodes added to scrape config
- [ ] Dashboard imported and showing data
- [ ] Alert rules configured
- [ ] Notification channels set up
- [ ] SSL/TLS configured (production)
- [ ] Firewall rules configured
- [ ] Backup strategy in place
- [ ] Monitoring verified (test alerts)

## 🎯 Next Steps

1. **Set up Alertmanager** for advanced alert routing
2. **Configure PagerDuty** for on-call rotation
3. **Create runbooks** for common alerts
4. **Set up log aggregation** (Loki/ELK)
5. **Add APM** (Application Performance Monitoring)

---

**Status**: ✅ **Production Ready**

Prometheus and Grafana are now fully deployed and ready for production monitoring of DineroCoin nodes.

