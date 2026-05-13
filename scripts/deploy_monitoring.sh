#!/bin/bash
# Deploy Prometheus metrics and configure alert thresholds for DineroCoin production

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
MONITORING_DIR="$PROJECT_ROOT/monitoring"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=== Deploying DineroCoin Production Monitoring ===${NC}"

# Create monitoring directory structure
mkdir -p "$MONITORING_DIR"/{prometheus,grafana,alertmanager}

# Generate Prometheus configuration
cat > "$MONITORING_DIR/prometheus/prometheus.yml" << 'EOF'
global:
  scrape_interval: 15s
  evaluation_interval: 15s

rule_files:
  - "dinero_alerts.yml"

alerting:
  alertmanagers:
    - static_configs:
        - targets:
          - alertmanager:9093

scrape_configs:
  - job_name: 'dinero-daemon'
    static_configs:
      - targets: ['localhost:8332']
    metrics_path: '/metrics'
    scrape_interval: 10s
    scrape_timeout: 5s

  - job_name: 'node-exporter'
    static_configs:
      - targets: ['localhost:9100']
    scrape_interval: 15s

  - job_name: 'prometheus'
    static_configs:
      - targets: ['localhost:9090']
EOF

# Generate alert rules matching go-live checklist thresholds
cat > "$MONITORING_DIR/prometheus/dinero_alerts.yml" << 'EOF'
groups:
- name: dinero_storage_alerts
  rules:
  # Compaction debt alerts
  - alert: CompactionDebtWarning
    expr: dinero_storage_compaction_debt_bytes > 1073741824  # 1GB
    for: 10m
    labels:
      severity: warning
    annotations:
      summary: "DineroCoin compaction debt exceeds 1GB"
      description: "Compaction debt is {{ $value | humanize1024 }}B, exceeding 1GB threshold for 10 minutes"

  - alert: CompactionDebtCritical
    expr: dinero_storage_compaction_debt_bytes > 2147483648  # 2GB
    for: 10m
    labels:
      severity: critical
    annotations:
      summary: "DineroCoin compaction debt exceeds 2GB"
      description: "Compaction debt is {{ $value | humanize1024 }}B, exceeding 2GB critical threshold"

  # Block connect performance alerts
  - alert: BlockConnectP99Critical
    expr: histogram_quantile(0.99, dinero_storage_block_connect_duration_seconds) > 0.5
    for: 5m
    labels:
      severity: critical
    annotations:
      summary: "DineroCoin p99 block connect time exceeds 500ms"
      description: "p99 block connect time is {{ $value }}s, exceeding 500ms threshold"

  # Disk usage alerts
  - alert: DiskUsageCritical
    expr: (1 - node_filesystem_avail_bytes{mountpoint="/var/lib/dinero"} / node_filesystem_size_bytes{mountpoint="/var/lib/dinero"}) * 100 > 90
    for: 0s
    labels:
      severity: critical
    annotations:
      summary: "DineroCoin data disk usage exceeds 90%"
      description: "Disk usage is {{ $value }}%, exceeding 90% threshold"

  - alert: DiskUsageRefuseBlocks
    expr: (1 - node_filesystem_avail_bytes{mountpoint="/var/lib/dinero"} / node_filesystem_size_bytes{mountpoint="/var/lib/dinero"}) * 100 > 95
    for: 0s
    labels:
      severity: critical
    annotations:
      summary: "DineroCoin refusing new blocks due to disk usage"
      description: "Disk usage is {{ $value }}%, exceeding 95% - new blocks will be refused"

  # Reorg rate alerts
  - alert: ReorgRateWarning
    expr: rate(dinero_blockchain_reorgs_total[1h]) * 3600 > 3
    for: 5m
    labels:
      severity: warning
    annotations:
      summary: "DineroCoin reorg rate exceeds 3 per hour"
      description: "Reorg rate is {{ $value }} per hour, exceeding 3/hour threshold"

  # Storage backend health
  - alert: StorageBackendDown
    expr: dinero_storage_backend_available == 0
    for: 1m
    labels:
      severity: critical
    annotations:
      summary: "DineroCoin storage backend unavailable"
      description: "Primary storage backend has been unavailable for 1 minute"

  # Memory usage alerts
  - alert: MemoryUsageHigh
    expr: dinero_process_resident_memory_bytes > 8589934592  # 8GB
    for: 15m
    labels:
      severity: warning
    annotations:
      summary: "DineroCoin memory usage exceeds 8GB"
      description: "Memory usage is {{ $value | humanize1024 }}B, exceeding 8GB threshold"

- name: dinero_network_alerts
  rules:
  # Peer connectivity
  - alert: PeerCountLow
    expr: dinero_network_peers_connected < 8
    for: 10m
    labels:
      severity: warning
    annotations:
      summary: "DineroCoin peer count below minimum"
      description: "Connected to {{ $value }} peers, below minimum of 8"

  # Sync status
  - alert: NodeOutOfSync
    expr: dinero_blockchain_blocks_behind > 10
    for: 5m
    labels:
      severity: critical
    annotations:
      summary: "DineroCoin node out of sync"
      description: "Node is {{ $value }} blocks behind network tip"
EOF

# Generate Alertmanager configuration
cat > "$MONITORING_DIR/alertmanager/alertmanager.yml" << 'EOF'
global:
  smtp_smarthost: 'localhost:587'
  smtp_from: 'alerts@dinero.local'

route:
  group_by: ['alertname']
  group_wait: 10s
  group_interval: 10s
  repeat_interval: 1h
  receiver: 'web.hook'
  routes:
  - match:
      severity: critical
    receiver: 'critical-alerts'
  - match:
      severity: warning
    receiver: 'warning-alerts'

receivers:
- name: 'web.hook'
  webhook_configs:
  - url: 'http://localhost:5001/webhook'

- name: 'critical-alerts'
  slack_configs:
  - api_url: 'YOUR_SLACK_WEBHOOK_URL'
    channel: '#dinero-alerts'
    title: '🚨 DineroCoin Critical Alert'
    text: '{{ range .Alerts }}{{ .Annotations.summary }}\n{{ .Annotations.description }}{{ end }}'

- name: 'warning-alerts'
  slack_configs:
  - api_url: 'YOUR_SLACK_WEBHOOK_URL'
    channel: '#dinero-alerts'
    title: '⚠️ DineroCoin Warning'
    text: '{{ range .Alerts }}{{ .Annotations.summary }}\n{{ .Annotations.description }}{{ end }}'
EOF

# Generate Grafana dashboard
cat > "$MONITORING_DIR/grafana/dinero_dashboard.json" << 'EOF'
{
  "dashboard": {
    "id": null,
    "title": "DineroCoin Production Dashboard",
    "tags": ["dinero", "blockchain", "storage"],
    "timezone": "browser",
    "panels": [
      {
        "id": 1,
        "title": "Storage Compaction Debt",
        "type": "stat",
        "targets": [
          {
            "expr": "dinero_storage_compaction_debt_bytes",
            "legendFormat": "Compaction Debt"
          }
        ],
        "fieldConfig": {
          "defaults": {
            "unit": "bytes",
            "thresholds": {
              "steps": [
                {"color": "green", "value": null},
                {"color": "yellow", "value": 1073741824},
                {"color": "red", "value": 2147483648}
              ]
            }
          }
        },
        "gridPos": {"h": 8, "w": 12, "x": 0, "y": 0}
      },
      {
        "id": 2,
        "title": "Block Connect Performance",
        "type": "graph",
        "targets": [
          {
            "expr": "histogram_quantile(0.99, dinero_storage_block_connect_duration_seconds)",
            "legendFormat": "p99"
          },
          {
            "expr": "histogram_quantile(0.95, dinero_storage_block_connect_duration_seconds)",
            "legendFormat": "p95"
          },
          {
            "expr": "histogram_quantile(0.50, dinero_storage_block_connect_duration_seconds)",
            "legendFormat": "p50"
          }
        ],
        "yAxes": [
          {
            "unit": "s",
            "max": 1.0
          }
        ],
        "gridPos": {"h": 8, "w": 12, "x": 12, "y": 0}
      },
      {
        "id": 3,
        "title": "Disk Usage",
        "type": "stat",
        "targets": [
          {
            "expr": "(1 - node_filesystem_avail_bytes{mountpoint=\"/var/lib/dinero\"} / node_filesystem_size_bytes{mountpoint=\"/var/lib/dinero\"}) * 100",
            "legendFormat": "Disk Usage %"
          }
        ],
        "fieldConfig": {
          "defaults": {
            "unit": "percent",
            "max": 100,
            "thresholds": {
              "steps": [
                {"color": "green", "value": null},
                {"color": "yellow", "value": 80},
                {"color": "red", "value": 90}
              ]
            }
          }
        },
        "gridPos": {"h": 8, "w": 12, "x": 0, "y": 8}
      },
      {
        "id": 4,
        "title": "Reorg Rate",
        "type": "graph",
        "targets": [
          {
            "expr": "rate(dinero_blockchain_reorgs_total[1h]) * 3600",
            "legendFormat": "Reorgs/hour"
          }
        ],
        "yAxes": [
          {
            "unit": "short",
            "max": 10
          }
        ],
        "gridPos": {"h": 8, "w": 12, "x": 12, "y": 8}
      }
    ],
    "time": {
      "from": "now-1h",
      "to": "now"
    },
    "refresh": "10s"
  }
}
EOF

# Generate Docker Compose for monitoring stack
cat > "$MONITORING_DIR/docker-compose.yml" << 'EOF'
version: '3.8'

services:
  prometheus:
    image: prom/prometheus:latest
    container_name: dinero-prometheus
    ports:
      - "9090:9090"
    volumes:
      - ./prometheus:/etc/prometheus
      - prometheus_data:/prometheus
    command:
      - '--config.file=/etc/prometheus/prometheus.yml'
      - '--storage.tsdb.path=/prometheus'
      - '--web.console.libraries=/etc/prometheus/console_libraries'
      - '--web.console.templates=/etc/prometheus/consoles'
      - '--storage.tsdb.retention.time=30d'
      - '--web.enable-lifecycle'
    restart: unless-stopped

  grafana:
    image: grafana/grafana:latest
    container_name: dinero-grafana
    ports:
      - "3000:3000"
    volumes:
      - grafana_data:/var/lib/grafana
      - ./grafana:/etc/grafana/provisioning
    environment:
      - GF_SECURITY_ADMIN_PASSWORD=dinero123
      - GF_USERS_ALLOW_SIGN_UP=false
    restart: unless-stopped

  alertmanager:
    image: prom/alertmanager:latest
    container_name: dinero-alertmanager
    ports:
      - "9093:9093"
    volumes:
      - ./alertmanager:/etc/alertmanager
    restart: unless-stopped

  node-exporter:
    image: prom/node-exporter:latest
    container_name: dinero-node-exporter
    ports:
      - "9100:9100"
    volumes:
      - /proc:/host/proc:ro
      - /sys:/host/sys:ro
      - /:/rootfs:ro
    command:
      - '--path.procfs=/host/proc'
      - '--path.rootfs=/rootfs'
      - '--path.sysfs=/host/sys'
      - '--collector.filesystem.mount-points-exclude=^/(sys|proc|dev|host|etc)($$|/)'
    restart: unless-stopped

volumes:
  prometheus_data:
  grafana_data:
EOF

# Generate monitoring deployment script
cat > "$MONITORING_DIR/deploy.sh" << 'EOF'
#!/bin/bash
# Deploy DineroCoin monitoring stack

set -e

echo "Deploying DineroCoin monitoring stack..."

# Start monitoring services
docker-compose up -d

# Wait for services to start
echo "Waiting for services to start..."
sleep 30

# Import Grafana dashboard
curl -X POST \
  http://admin:dinero123@localhost:3000/api/dashboards/db \
  -H 'Content-Type: application/json' \
  -d @grafana/dinero_dashboard.json

echo "Monitoring stack deployed successfully!"
echo "Access points:"
echo "- Prometheus: http://localhost:9090"
echo "- Grafana: http://localhost:3000 (admin/dinero123)"
echo "- Alertmanager: http://localhost:9093"
EOF

chmod +x "$MONITORING_DIR/deploy.sh"

# Generate monitoring validation script
cat > "$MONITORING_DIR/validate.sh" << 'EOF'
#!/bin/bash
# Validate monitoring deployment

set -e

echo "=== Validating DineroCoin Monitoring Deployment ==="

# Check if services are running
services=("dinero-prometheus" "dinero-grafana" "dinero-alertmanager" "dinero-node-exporter")
for service in "${services[@]}"; do
    if docker ps | grep -q "$service"; then
        echo "✓ $service is running"
    else
        echo "✗ $service is not running"
        exit 1
    fi
done

# Check Prometheus targets
echo "Checking Prometheus targets..."
targets_response=$(curl -s http://localhost:9090/api/v1/targets)
if echo "$targets_response" | grep -q '"health":"up"'; then
    echo "✓ Prometheus targets are healthy"
else
    echo "✗ Some Prometheus targets are down"
fi

# Check Grafana API
echo "Checking Grafana API..."
if curl -s -f http://localhost:3000/api/health > /dev/null; then
    echo "✓ Grafana API is responding"
else
    echo "✗ Grafana API is not responding"
    exit 1
fi

# Test alert rules
echo "Checking alert rules..."
rules_response=$(curl -s http://localhost:9090/api/v1/rules)
if echo "$rules_response" | grep -q "dinero_storage_alerts"; then
    echo "✓ Alert rules are loaded"
else
    echo "✗ Alert rules are not loaded"
    exit 1
fi

echo "✅ All monitoring components validated successfully!"
EOF

chmod +x "$MONITORING_DIR/validate.sh"

echo -e "${GREEN}✓ Monitoring configuration generated${NC}"
echo -e "${YELLOW}Next steps:${NC}"
echo "1. cd $MONITORING_DIR"
echo "2. ./deploy.sh"
echo "3. ./validate.sh"
echo ""
echo -e "${BLUE}Monitoring endpoints:${NC}"
echo "- Prometheus: http://localhost:9090"
echo "- Grafana: http://localhost:3000 (admin/dinero123)"
echo "- Alertmanager: http://localhost:9093"
