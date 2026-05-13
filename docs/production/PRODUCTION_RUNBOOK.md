# 🏭 Dinero Production Operations Runbook

## 🎯 **Production Deployment Checklist**

### **Pre-Deployment Security Audit**
- [ ] **TLS Termination**: Nginx configured with TLS 1.2+ only
- [ ] **Rate Limiting**: RPC (10 req/sec) and WebSocket (5 req/sec) limits active
- [ ] **Loopback Binding**: RPC bound to 127.0.0.1 only, external access via proxy
- [ ] **Constant-Time Auth**: Cookie comparison uses timing-safe functions
- [ ] **File Permissions**: Cookie file set to 0600, config files 0644
- [ ] **CVE Scan**: All dependencies scanned for known vulnerabilities

### **Data Durability Verification**
- [ ] **WAL Mode**: All SQLite databases using WAL journal mode
- [ ] **Sync Settings**: PRAGMA synchronous=NORMAL or FULL enabled
- [ ] **Crash Safety**: Kill -9 tests pass without corruption
- [ ] **Schema Versioning**: Database migration system functional
- [ ] **Backup System**: Automated backups configured and tested

### **DoS Resilience**
- [ ] **Connection Limits**: Global and per-IP connection caps enforced
- [ ] **Request Timeouts**: All network operations have timeouts
- [ ] **Resource Limits**: Memory, CPU, and file descriptor limits set
- [ ] **Backpressure**: Queue limits prevent resource exhaustion

### **Quality Assurance**
- [ ] **Fuzzing**: LibFuzzer targets pass 24h continuous fuzzing
- [ ] **Sanitizers**: ASan/UBSan builds clean in CI
- [ ] **Soak Testing**: 72h soak test with flat memory usage
- [ ] **Load Testing**: Peak capacity determined and documented

---

## 🚀 **Deployment Process**

### **1. System Setup**

```bash
# Create dedicated user
sudo useradd -r -s /bin/false -d /var/lib/dinero dinero
sudo mkdir -p /var/lib/dinero /var/log/dinero
sudo chown dinero:dinero /var/lib/dinero /var/log/dinero
sudo chmod 750 /var/lib/dinero /var/log/dinero

# Install binary
sudo cp build/bin/dinerod /usr/local/bin/
sudo chown root:root /usr/local/bin/dinerod
sudo chmod 755 /usr/local/bin/dinerod
```

### **2. Configuration**

```bash
# Create configuration
sudo tee /var/lib/dinero/dinero.conf << 'EOF'
# Dinero Production Configuration
server=1
daemon=0
printtoconsole=0

# Network binding (loopback only)
rpcbind=127.0.0.1
rpcport=20998
port=20999

# Data directories
datadir=/var/lib/dinero
logfile=/var/log/dinero/daemon.log

# Security
rpcallowip=127.0.0.1
rpccookiefile=/var/lib/dinero/.cookie

# Performance
dbcache=512
maxconnections=125
EOF

sudo chown dinero:dinero /var/lib/dinero/dinero.conf
sudo chmod 600 /var/lib/dinero/dinero.conf
```

### **3. Systemd Service**

```bash
# Install service unit
sudo cp ops/systemd/dinerod.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable dinerod

# Start service
sudo systemctl start dinerod
sudo systemctl status dinerod
```

### **4. Nginx TLS Proxy**

```bash
# Install nginx config
sudo cp ops/nginx/dinero-rpc.conf /etc/nginx/sites-available/
sudo ln -s /etc/nginx/sites-available/dinero-rpc.conf /etc/nginx/sites-enabled/

# Get TLS certificate
sudo certbot --nginx -d rpc.dinero.example.com -d ws.dinero.example.com

# Test and reload nginx
sudo nginx -t
sudo systemctl reload nginx
```

---

## 📊 **Monitoring & Observability**

### **Health Checks**

```bash
# Service health
curl -s https://rpc.dinero.example.com/healthz

# RPC connectivity
curl -s -u "$(cat /var/lib/dinero/.cookie)" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo"}' \
  https://rpc.dinero.example.com/

# WebSocket connectivity
wscat -c wss://ws.dinero.example.com/ws
```

### **Key Metrics to Monitor**

#### **System Metrics**
- **CPU Usage**: < 80% sustained
- **Memory Usage**: RSS should remain flat over time
- **Disk I/O**: Monitor for excessive writes
- **Network**: Connection count, request rate
- **File Descriptors**: Should not approach limits

#### **Application Metrics**
- **RPC Requests**: Rate, latency, error rate
- **WebSocket Connections**: Active count, message rate
- **Database**: WAL size, checkpoint frequency, query latency
- **Blockchain**: Block height, sync status, peer count

#### **Security Metrics**
- **Failed Auth Attempts**: Rate of 401 responses
- **Rate Limit Hits**: Requests blocked by rate limiting
- **Connection Rejections**: Blocked by connection limits
- **TLS Errors**: Certificate or handshake failures

### **Log Analysis**

```bash
# Real-time logs
sudo journalctl -u dinerod -f

# Error analysis
sudo journalctl -u dinerod --since "1 hour ago" | grep -i error

# Performance analysis
sudo journalctl -u dinerod --since "1 day ago" | grep -E "(slow|timeout|limit)"

# Security analysis
sudo tail -f /var/log/nginx/access.log | grep -E "(401|429|444)"
```

---

## 🔧 **Maintenance Procedures**

### **Database Maintenance**

```bash
# Manual WAL checkpoint
sudo -u dinero sqlite3 /var/lib/dinero/explorer.db "PRAGMA wal_checkpoint(FULL);"

# Database integrity check
sudo -u dinero sqlite3 /var/lib/dinero/explorer.db "PRAGMA integrity_check;"

# Database statistics
sudo -u dinero sqlite3 /var/lib/dinero/explorer.db "PRAGMA page_count; PRAGMA page_size;"
```

### **Backup Procedures**

```bash
# Online backup (while daemon running)
sudo -u dinero sqlite3 /var/lib/dinero/explorer.db ".backup /backup/explorer-$(date +%Y%m%d).db"

# Full data backup
sudo tar -czf /backup/dinero-full-$(date +%Y%m%d).tar.gz \
  -C /var/lib/dinero \
  --exclude='.cookie' \
  --exclude='debug.log' \
  .
```

### **Log Rotation**

```bash
# Configure logrotate
sudo tee /etc/logrotate.d/dinero << 'EOF'
/var/log/dinero/*.log {
    daily
    rotate 30
    compress
    delaycompress
    missingok
    notifempty
    create 640 dinero dinero
    postrotate
        systemctl reload dinerod
    endscript
}
EOF
```

---

## 🚨 **Incident Response**

### **Service Down**

```bash
# Check service status
sudo systemctl status dinerod

# Check recent logs
sudo journalctl -u dinerod --since "10 minutes ago"

# Check disk space
df -h /var/lib/dinero

# Check memory
free -h

# Restart if necessary
sudo systemctl restart dinerod
```

### **Database Corruption**

```bash
# Stop service
sudo systemctl stop dinerod

# Check integrity
sudo -u dinero sqlite3 /var/lib/dinero/explorer.db "PRAGMA integrity_check;"

# Restore from backup if corrupted
sudo -u dinero cp /backup/explorer-latest.db /var/lib/dinero/explorer.db

# Restart service
sudo systemctl start dinerod
```

### **High Load / DoS Attack**

```bash
# Check connection counts
ss -tuln | grep -E "(20998|20999)"

# Check nginx rate limiting
sudo tail -f /var/log/nginx/error.log | grep "limiting requests"

# Block attacking IPs
sudo iptables -A INPUT -s ATTACKER_IP -j DROP

# Increase rate limits temporarily if legitimate traffic
sudo nano /etc/nginx/sites-available/dinero-rpc.conf
sudo nginx -t && sudo systemctl reload nginx
```

### **Memory Leak Detection**

```bash
# Monitor memory usage over time
while true; do
  echo "$(date): $(ps -o pid,vsz,rss,comm -p $(pgrep dinerod))"
  sleep 300
done

# If memory grows continuously, collect core dump
sudo gdb -p $(pgrep dinerod) -batch -ex "generate-core-file /tmp/dinerod.core" -ex quit

# Analyze with AddressSanitizer build
sudo systemctl stop dinerod
sudo ASAN_OPTIONS=detect_leaks=1 /usr/local/bin/dinerod-asan [same args]
```

---

## 📋 **Performance Tuning**

### **Database Optimization**

```sql
-- Optimize database after major operations
PRAGMA optimize;

-- Adjust cache size (in KB, negative = KB)
PRAGMA cache_size = -65536;  -- 64MB

-- Adjust WAL checkpoint frequency
PRAGMA wal_autocheckpoint = 1000;

-- Enable memory-mapped I/O
PRAGMA mmap_size = 268435456;  -- 256MB
```

### **System Tuning**

```bash
# Increase file descriptor limits
echo "dinero soft nofile 65536" | sudo tee -a /etc/security/limits.conf
echo "dinero hard nofile 65536" | sudo tee -a /etc/security/limits.conf

# Optimize TCP settings
sudo tee -a /etc/sysctl.conf << 'EOF'
net.core.somaxconn = 1024
net.ipv4.tcp_max_syn_backlog = 1024
net.ipv4.tcp_fin_timeout = 30
EOF

sudo sysctl -p
```

---

## 🔐 **Security Hardening**

### **Firewall Configuration**

```bash
# Allow only necessary ports
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow ssh
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw enable
```

### **Certificate Management**

```bash
# Auto-renewal test
sudo certbot renew --dry-run

# Certificate monitoring
sudo crontab -e
# Add: 0 2 * * * certbot renew --quiet --post-hook "systemctl reload nginx"
```

### **Intrusion Detection**

```bash
# Install fail2ban
sudo apt install fail2ban

# Configure for nginx
sudo tee /etc/fail2ban/jail.local << 'EOF'
[nginx-rpc]
enabled = true
port = 443
filter = nginx-rpc
logpath = /var/log/nginx/access.log
maxretry = 5
bantime = 3600
EOF

# Create filter
sudo tee /etc/fail2ban/filter.d/nginx-rpc.conf << 'EOF'
[Definition]
failregex = ^<HOST> .* "(GET|POST) .* HTTP/.*" (401|403|429) .*$
ignoreregex =
EOF

sudo systemctl restart fail2ban
```

---

## 📈 **Capacity Planning**

### **Resource Requirements**

| Component | Minimum | Recommended | High Load |
|-----------|---------|-------------|-----------|
| CPU | 2 cores | 4 cores | 8+ cores |
| RAM | 4 GB | 8 GB | 16+ GB |
| Disk | 100 GB SSD | 500 GB SSD | 1+ TB NVMe |
| Network | 100 Mbps | 1 Gbps | 10+ Gbps |

### **Scaling Indicators**

**Scale Up When:**
- CPU usage > 80% sustained
- Memory usage approaching limits
- Disk I/O wait > 10%
- Request latency > 1 second
- Error rate > 1%

**Scale Out When:**
- Single node at capacity
- Geographic distribution needed
- High availability required

---

## 🎯 **Production Readiness Checklist**

### **Security** ✅
- [ ] TLS-only RPC access
- [ ] Rate limiting active
- [ ] Constant-time authentication
- [ ] File permissions secured
- [ ] CVE scanning in CI

### **Reliability** ✅
- [ ] WAL mode enabled
- [ ] Crash-safety tested
- [ ] Backup/restore procedures
- [ ] Schema migration system
- [ ] Health checks functional

### **Operations** ✅
- [ ] Hardened systemd service
- [ ] Structured logging
- [ ] Monitoring dashboards
- [ ] Incident runbooks
- [ ] Capacity planning

### **Quality** ✅
- [ ] 72h soak test passed
- [ ] Fuzzing in CI
- [ ] Load testing completed
- [ ] Performance benchmarks
- [ ] Security audit passed

**🚀 Ready for Production when all boxes are checked!**
