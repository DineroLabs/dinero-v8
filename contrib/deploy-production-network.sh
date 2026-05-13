#!/bin/bash
# Dinero Production Network Deployment Script
# Deploys complete production-grade network infrastructure

set -euo pipefail

# Configuration
NETWORK_NAME="dinero-mainnet"
DOMAIN="dinero-coin.com"
MONITORING_DOMAIN="monitor.dinero-coin.com"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging function
log() {
    echo -e "${GREEN}[$(date +'%Y-%m-%d %H:%M:%S')] $1${NC}"
}

warn() {
    echo -e "${YELLOW}[$(date +'%Y-%m-%d %H:%M:%S')] WARNING: $1${NC}"
}

error() {
    echo -e "${RED}[$(date +'%Y-%m-%d %H:%M:%S')] ERROR: $1${NC}"
    exit 1
}

# Check prerequisites
check_prerequisites() {
    log "Checking prerequisites..."
    
    # Check required tools
    local tools=("docker" "docker-compose" "kubectl" "helm" "terraform" "ansible")
    for tool in "${tools[@]}"; do
        if ! command -v "$tool" &> /dev/null; then
            warn "$tool not found - some deployment options may not work"
        fi
    done
    
    # Check cloud CLI tools
    if command -v aws &> /dev/null; then
        log "AWS CLI found"
    fi
    
    if command -v gcloud &> /dev/null; then
        log "Google Cloud CLI found"
    fi
    
    if command -v az &> /dev/null; then
        log "Azure CLI found"
    fi
}

# Deploy seed nodes
deploy_seed_nodes() {
    log "Deploying seed nodes..."
    
    # Seed node regions and configurations
    declare -A SEED_NODES=(
        ["seed1"]="us-east-1:t3.medium"
        ["seed2"]="us-west-2:t3.medium"
        ["seed3"]="eu-west-1:t3.medium"
        ["seed4"]="eu-central-1:t3.medium"
        ["seed5"]="ap-southeast-1:t3.medium"
        ["seed6"]="ap-northeast-1:t3.medium"
        ["seed7"]="sa-east-1:t3.medium"
        ["seed8"]="ca-central-1:t3.medium"
    )
    
    for seed in "${!SEED_NODES[@]}"; do
        IFS=':' read -r region instance_type <<< "${SEED_NODES[$seed]}"
        log "Deploying $seed in $region ($instance_type)"
        
        # Create Terraform configuration for seed node
        cat > "terraform-$seed.tf" <<EOF
# Seed node: $seed
resource "aws_instance" "$seed" {
  ami           = data.aws_ami.ubuntu.id
  instance_type = "$instance_type"
  key_name      = var.key_name
  
  vpc_security_group_ids = [aws_security_group.dinero_seed.id]
  subnet_id              = aws_subnet.public.id
  
  user_data = base64encode(templatefile("seed-node-userdata.sh", {
    hostname = "$seed.$DOMAIN"
  }))
  
  root_block_device {
    volume_type = "gp3"
    volume_size = 100
    encrypted   = true
  }
  
  tags = {
    Name = "$seed.$DOMAIN"
    Role = "seed-node"
    Network = "$NETWORK_NAME"
  }
}

resource "aws_route53_record" "$seed" {
  zone_id = aws_route53_zone.main.zone_id
  name    = "$seed.$DOMAIN"
  type    = "A"
  ttl     = 300
  records = [aws_instance.$seed.public_ip]
}
EOF
    done
    
    log "Seed node configurations created. Run 'terraform apply' to deploy."
}

# Deploy DNS seeders
deploy_dns_seeders() {
    log "Deploying DNS seeders..."
    
    # Create DNS seeder configuration
    cat > docker-compose-dns-seeder.yml <<EOF
version: '3.8'

services:
  dns-seeder-1:
    build:
      context: ./contrib/dns-seeder
      dockerfile: Dockerfile
    ports:
      - "53:53/udp"
      - "53:53/tcp"
    environment:
      - SEEDER_DOMAIN=seed.$DOMAIN
      - NETWORK_MAGIC=0xD1A0C0DE
      - PROTOCOL_VERSION=70016
      - P2P_PORT=20999
      - LOG_LEVEL=INFO
    volumes:
      - ./dns-seeder-data:/data
    restart: unless-stopped
    
  dns-seeder-2:
    build:
      context: ./contrib/dns-seeder
      dockerfile: Dockerfile
    ports:
      - "5353:53/udp"
      - "5353:53/tcp"
    environment:
      - SEEDER_DOMAIN=dnsseed.$DOMAIN
      - NETWORK_MAGIC=0xD1A0C0DE
      - PROTOCOL_VERSION=70016
      - P2P_PORT=20999
      - LOG_LEVEL=INFO
    volumes:
      - ./dns-seeder-data-2:/data
    restart: unless-stopped
EOF
    
    # Create Dockerfile for DNS seeder
    cat > contrib/dns-seeder/Dockerfile <<EOF
FROM python:3.11-slim

WORKDIR /app

# Install system dependencies
RUN apt-get update && apt-get install -y \\
    dnsutils \\
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy application
COPY dns-seeder.py .

# Create non-root user
RUN useradd -m -u 1000 seeder
USER seeder

EXPOSE 53/udp 53/tcp

CMD ["python", "dns-seeder.py"]
EOF
    
    # Create requirements.txt for DNS seeder
    cat > contrib/dns-seeder/requirements.txt <<EOF
dnslib==0.9.23
asyncio-dns==1.1.6
prometheus-client==0.17.1
EOF
    
    log "DNS seeder configuration created. Run 'docker-compose -f docker-compose-dns-seeder.yml up -d' to deploy."
}

# Deploy monitoring infrastructure
deploy_monitoring() {
    log "Deploying monitoring infrastructure..."
    
    # Create monitoring stack with Docker Compose
    cat > docker-compose-monitoring.yml <<EOF
version: '3.8'

services:
  prometheus:
    image: prom/prometheus:latest
    ports:
      - "9090:9090"
    volumes:
      - ./contrib/monitoring/prometheus.yml:/etc/prometheus/prometheus.yml
      - ./contrib/monitoring/dinero_alerts.yml:/etc/prometheus/dinero_alerts.yml
      - prometheus-data:/prometheus
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
    ports:
      - "3000:3000"
    environment:
      - GF_SECURITY_ADMIN_PASSWORD=admin123
      - GF_USERS_ALLOW_SIGN_UP=false
    volumes:
      - grafana-data:/var/lib/grafana
      - ./contrib/monitoring/grafana-dashboards:/etc/grafana/provisioning/dashboards
    restart: unless-stopped
    
  alertmanager:
    image: prom/alertmanager:latest
    ports:
      - "9093:9093"
    volumes:
      - ./contrib/monitoring/alertmanager.yml:/etc/alertmanager/alertmanager.yml
      - alertmanager-data:/alertmanager
    restart: unless-stopped
    
  blackbox-exporter:
    image: prom/blackbox-exporter:latest
    ports:
      - "9115:9115"
    volumes:
      - ./contrib/monitoring/blackbox.yml:/etc/blackbox_exporter/config.yml
    restart: unless-stopped

volumes:
  prometheus-data:
  grafana-data:
  alertmanager-data:
EOF
    
    # Create Grafana dashboard configuration
    mkdir -p contrib/monitoring/grafana-dashboards
    cat > contrib/monitoring/grafana-dashboards/dinero-network.json <<EOF
{
  "dashboard": {
    "id": null,
    "title": "Dinero Network Overview",
    "tags": ["dinero"],
    "timezone": "browser",
    "panels": [
      {
        "title": "Network Block Height",
        "type": "stat",
        "targets": [
          {
            "expr": "dinero_block_height",
            "legendFormat": "{{instance}}"
          }
        ]
      },
      {
        "title": "Connected Peers",
        "type": "graph",
        "targets": [
          {
            "expr": "dinero_peer_count",
            "legendFormat": "{{instance}}"
          }
        ]
      },
      {
        "title": "Network Hashrate",
        "type": "graph",
        "targets": [
          {
            "expr": "dinero_network_hashrate",
            "legendFormat": "Network Hashrate"
          }
        ]
      }
    ],
    "time": {
      "from": "now-1h",
      "to": "now"
    },
    "refresh": "30s"
  }
}
EOF
    
    log "Monitoring stack configuration created. Run 'docker-compose -f docker-compose-monitoring.yml up -d' to deploy."
}

# Create alerting rules
create_alerting_rules() {
    log "Creating alerting rules..."
    
    cat > contrib/monitoring/dinero_alerts.yml <<EOF
groups:
  - name: dinero_network
    rules:
      - alert: NodeDown
        expr: up{job="dinero-seed-nodes"} == 0
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "Dinero seed node is down"
          description: "Seed node {{ \$labels.instance }} has been down for more than 5 minutes."
          
      - alert: LowPeerCount
        expr: dinero_peer_count < 8
        for: 10m
        labels:
          severity: warning
        annotations:
          summary: "Low peer count on Dinero node"
          description: "Node {{ \$labels.instance }} has only {{ \$value }} peers connected."
          
      - alert: BlockHeightLag
        expr: (max(dinero_block_height) - dinero_block_height) > 5
        for: 10m
        labels:
          severity: warning
        annotations:
          summary: "Dinero node is behind in block height"
          description: "Node {{ \$labels.instance }} is {{ \$value }} blocks behind the network."
          
      - alert: HighMemoryUsage
        expr: (node_memory_MemTotal_bytes - node_memory_MemAvailable_bytes) / node_memory_MemTotal_bytes > 0.9
        for: 15m
        labels:
          severity: warning
        annotations:
          summary: "High memory usage on Dinero node"
          description: "Node {{ \$labels.instance }} memory usage is above 90%."
          
      - alert: HighDiskUsage
        expr: (node_filesystem_size_bytes{fstype!="tmpfs"} - node_filesystem_free_bytes{fstype!="tmpfs"}) / node_filesystem_size_bytes{fstype!="tmpfs"} > 0.8
        for: 15m
        labels:
          severity: warning
        annotations:
          summary: "High disk usage on Dinero node"
          description: "Node {{ \$labels.instance }} disk usage is above 80%."
EOF
}

# Run stress tests
run_stress_tests() {
    log "Running network stress tests..."
    
    # Install Python dependencies for stress testing
    pip3 install aiohttp asyncio statistics
    
    # Run burst test
    log "Running burst stress test..."
    python3 contrib/stress-testing/tx-generator.py \\
        --test-type burst \\
        --transactions 1000 \\
        --workers 20 \\
        --rpc-url http://seed1.$DOMAIN:8332/
    
    # Run sustained test
    log "Running sustained stress test..."
    python3 contrib/stress-testing/tx-generator.py \\
        --test-type sustained \\
        --tps 50 \\
        --duration 300 \\
        --rpc-url http://seed1.$DOMAIN:8332/
}

# Main deployment function
main() {
    log "🚀 Starting Dinero Production Network Deployment"
    log "=================================================="
    
    # Check prerequisites
    check_prerequisites
    
    # Create deployment menu
    echo ""
    echo "Select deployment components:"
    echo "1) Deploy seed nodes"
    echo "2) Deploy DNS seeders"
    echo "3) Deploy monitoring"
    echo "4) Create alerting rules"
    echo "5) Run stress tests"
    echo "6) Deploy everything"
    echo "7) Exit"
    echo ""
    
    read -p "Enter your choice (1-7): " choice
    
    case $choice in
        1)
            deploy_seed_nodes
            ;;
        2)
            deploy_dns_seeders
            ;;
        3)
            deploy_monitoring
            ;;
        4)
            create_alerting_rules
            ;;
        5)
            run_stress_tests
            ;;
        6)
            deploy_seed_nodes
            deploy_dns_seeders
            deploy_monitoring
            create_alerting_rules
            log "🎉 Full deployment configuration created!"
            log "Next steps:"
            log "1. Review and customize the generated configurations"
            log "2. Run 'terraform apply' to deploy infrastructure"
            log "3. Run 'docker-compose up -d' to start services"
            log "4. Configure DNS delegation for $DOMAIN"
            log "5. Run stress tests to validate performance"
            ;;
        7)
            log "Deployment cancelled."
            exit 0
            ;;
        *)
            error "Invalid choice. Please select 1-7."
            ;;
    esac
    
    log "✅ Deployment step completed successfully!"
}

# Run main function
main "$@"
