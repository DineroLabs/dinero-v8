# Dinero Global Node Registry - Quick Start Guide

## 5-Minute Setup

### 1. Start the Registry (Basic)

```bash
cd /Users/haydarevich/Documents/DineroCoin/registry
./dinero_registry.py
```

**Output:**
```
Dinero Global Node Registry
============================================================
[Registry] Monitoring 3 nodes
[Registry] Refresh interval: 60s
[OK] 173.249.195.59:21999 - 42.5ms - Virginia
[OK] 172.93.160.131:21999 - 38.2ms - California
[WARN] 127.0.0.1:21999 - connection failed
[Registry] Updated: 2/3 nodes alive

============================================================
[Registry] Server running at http://0.0.0.0:8080
[Registry] Access at:
           http://localhost:8080/
           http://localhost:8080/nodes.json
           http://localhost:8080/api/status
============================================================
```

### 2. View the Dashboard

Open browser: **http://localhost:8080/**

You'll see a live table with:
- Node names, networks, peer counts
- WebSocket ports, uptime
- Real-time latency measurements
- Uptime percentage for each node

### 3. Query via API

```bash
# Get all nodes (JSON)
curl http://localhost:8080/nodes.json | jq

# Quick status
curl http://localhost:8080/api/status | jq

# Per-node stats
curl http://localhost:8080/api/stats | jq

# Historical data
curl http://localhost:8080/api/history | jq
```

### 4. Start Registry with Self-Registration (Extended)

```bash
./dinero_registry_extended.py
```

This version allows nodes to register themselves:

```bash
curl -X POST http://localhost:8080/api/register \
  -H "Content-Type: application/json" \
  -d '{
    "serverinfo_url": "http://YOUR_IP:21999/serverinfo.json"
  }'
```

## Common Use Cases

### For GUI Wallets (Auto-connect to best node)

```javascript
const response = await fetch('https://status.dinero-coin.com/nodes.json');
const {nodes} = await response.json();

// Sort by latency, pick fastest
const fastest = nodes.sort((a, b) => a.latency_ms - b.latency_ms)[0];
console.log(`Connecting to ${fastest.name} at ${fastest.ip}:${fastest.ws_port}`);

// Connect WebSocket
const ws = new WebSocket(`ws://${fastest.ip}:${fastest.ws_port}`);
```

### For Node Operators (Auto-register)

Add to your node startup script:

```bash
#!/bin/bash
# start_dinero_node.sh

# Start daemon
dinerod -daemon -registry=1 -externalip=YOUR_IP -nodename="My Node"

# Wait for RPC
sleep 5

# Register with global registry
curl -X POST https://status.dinero-coin.com/api/register \
  -H "Content-Type: application/json" \
  -d "{\"serverinfo_url\": \"http://$(curl -s ifconfig.me):21999/serverinfo.json\"}"

echo "Node registered with global registry"
```

### For Monitoring (Uptime checks)

```bash
# Check if registry is healthy
curl -f http://localhost:8080/api/status || echo "Registry down!"

# Get total alive nodes
ALIVE=$(curl -s http://localhost:8080/api/status | jq -r '.total_nodes_alive')
echo "Nodes online: $ALIVE"

# Alert if less than 2 nodes alive
if [ "$ALIVE" -lt 2 ]; then
    echo "ALERT: Only $ALIVE nodes alive!" | mail -s "Dinero Alert" admin@example.com
fi
```

## Running in Production

### Option 1: Systemd Service

```bash
sudo cp dinero-registry.service /etc/systemd/system/
sudo mkdir -p /opt/dinero/registry
sudo cp *.py /opt/dinero/registry/
sudo useradd -r -s /bin/false dinero

sudo systemctl daemon-reload
sudo systemctl enable dinero-registry
sudo systemctl start dinero-registry
sudo systemctl status dinero-registry
```

View logs:
```bash
sudo journalctl -u dinero-registry -f
```

### Option 2: Docker

```bash
# Build image
docker build -t dinero-registry .

# Run container
docker run -d \
  --name dinero-registry \
  --restart always \
  -p 8080:8080 \
  dinero-registry

# View logs
docker logs -f dinero-registry
```

### Option 3: Behind Nginx (HTTPS)

```nginx
# /etc/nginx/sites-available/registry
server {
    listen 443 ssl http2;
    server_name status.dinero-coin.com;

    ssl_certificate /etc/letsencrypt/live/status.dinero-coin.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/status.dinero-coin.com/privkey.pem;

    location / {
        proxy_pass http://localhost:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }

    # Rate limiting for registration endpoint
    location /api/register {
        limit_req zone=register burst=5 nodelay;
        proxy_pass http://localhost:8080;
    }
}

# Rate limit definition
limit_req_zone $binary_remote_addr zone=register:10m rate=10r/m;
```

## Configuration Examples

### Custom Node List

```bash
./dinero_registry.py --nodes \
  "http://173.249.195.59:21999/serverinfo.json" \
  "http://172.93.160.131:21999/serverinfo.json" \
  "http://my.custom.node:21999/serverinfo.json"
```

### Custom Refresh Interval

```bash
# Check every 30 seconds (faster updates)
./dinero_registry.py --interval 30

# Check every 5 minutes (reduce load)
./dinero_registry.py --interval 300
```

### With HMAC Signature Validation

```bash
# Generate secret
SECRET=$(openssl rand -hex 32)
echo "Registry secret: $SECRET"

# Start registry with secret
./dinero_registry_extended.py --secret "$SECRET"

# Register node with signature
PAYLOAD='{"serverinfo_url":"http://1.2.3.4:21999/serverinfo.json"}'
SIGNATURE=$(echo -n "$PAYLOAD" | openssl dgst -sha256 -hmac "$SECRET" | cut -d' ' -f2)

curl -X POST http://localhost:8080/api/register \
  -H "Content-Type: application/json" \
  -H "X-Signature: $SIGNATURE" \
  -d "$PAYLOAD"
```

## Testing

Run the test suite:

```bash
# Start registry in background
./dinero_registry_extended.py &
REGISTRY_PID=$!

# Wait for startup
sleep 3

# Run tests
./test_registration.sh

# Stop registry
kill $REGISTRY_PID
```

## Integration with Dinero Daemon

See `daemon_integration_example.cpp` for C++ code to add to dinerod.

Key changes:
1. Add `-registry=1` flag to enable auto-registration
2. Add `-registryurl=https://status.dinero-coin.com/api/register`
3. Add `-externalip=YOUR_PUBLIC_IP`
4. Daemon will POST to registry every 10 minutes

Example `dinero.conf`:

```ini
# Enable registry sync
registry=1
registryurl=https://status.dinero-coin.com/api/register

# Public node info
externalip=173.249.195.59
nodename=Virginia
```

## Troubleshooting

### Registry shows 0 nodes

**Problem:** All nodes appear offline.

**Solution:**
```bash
# Test connectivity manually
curl http://173.249.195.59:21999/serverinfo.json

# Check if serverinfo.json is enabled
dinero-cli getserverinfo

# Verify dinerod is running with HTTP server
ps aux | grep dinerod
```

### High latency measurements

**Problem:** Latency >1000ms for all nodes.

**Solution:**
```bash
# Increase timeout
./dinero_registry.py --timeout 10

# Check network connectivity
ping 173.249.195.59

# Test direct connection
time curl http://173.249.195.59:21999/serverinfo.json
```

### Registration fails with 403 error

**Problem:** `{"error": "invalid signature"}`

**Solution:**
- Make sure HMAC signature matches
- Use correct secret key
- Check payload encoding (UTF-8, no extra whitespace)

### Port 8080 already in use

**Solution:**
```bash
# Use different port
./dinero_registry.py --port 9000

# Or kill existing process
lsof -ti:8080 | xargs kill
```

## Next Steps

1. **Deploy to production server** (VPS or cloud)
2. **Set up HTTPS** with Let's Encrypt
3. **Configure monitoring** (Prometheus, Grafana)
4. **Integrate with GUI wallet** for auto-discovery
5. **Add to Dinero docs** and website

## Support

- GitHub: https://github.com/dinerocoin/dinero
- Docs: https://docs.dinero-coin.com/
- Community: https://discord.gg/dinero
