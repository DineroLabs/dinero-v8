# Dinero Global Node Registry

A lightweight REST service that monitors and aggregates status information from Dinero nodes across the network.

## Features

✅ **Real-time Monitoring** - Periodic health checks for all configured nodes
✅ **Latency Tracking** - Measures and reports response times
✅ **Uptime Statistics** - Per-node reliability scoring
✅ **Historical Data** - Tracks node availability over time
✅ **REST API** - JSON endpoints for programmatic access
✅ **Web Dashboard** - Simple HTML status page
✅ **CORS Support** - Ready for browser-based clients

## Quick Start

### Requirements

```bash
pip3 install requests
```

### Basic Usage

```bash
cd /Users/haydarevich/Documents/DineroCoin/registry
chmod +x dinero_registry.py
./dinero_registry.py
```

Access the registry at:
- **Dashboard**: http://localhost:8080/
- **Node List**: http://localhost:8080/nodes.json
- **Status**: http://localhost:8080/api/status

### Custom Configuration

```bash
# Custom port
./dinero_registry.py --port 9000

# Custom nodes
./dinero_registry.py --nodes \
  "http://173.249.195.59:21999/serverinfo.json" \
  "http://172.93.160.131:21999/serverinfo.json"

# Faster refresh (every 30 seconds)
./dinero_registry.py --interval 30

# Longer timeout for slow nodes
./dinero_registry.py --timeout 10
```

## API Endpoints

### GET /nodes.json

Full registry with all active nodes:

```json
{
  "timestamp": "2025-11-03T12:00:00Z",
  "total_nodes": 2,
  "total_configured": 3,
  "nodes": [
    {
      "name": "Virginia",
      "ip": "173.249.195.59",
      "rpc_port": 21999,
      "ws_port": 21000,
      "p2p_port": 20999,
      "network": "mainnet",
      "uptime": 86400,
      "connections": 8,
      "features": ["contracts", "bridge"],
      "latency_ms": 42.5,
      "uptime_percentage": 99.8,
      "avg_latency_ms": 45.2,
      "source_url": "http://173.249.195.59:21999/serverinfo.json"
    }
  ]
}
```

### GET /api/status

Quick health check:

```json
{
  "status": "ok",
  "total_nodes_alive": 2,
  "total_nodes_configured": 3,
  "last_update": "2025-11-03T12:00:00Z"
}
```

### GET /api/stats

Per-node reliability statistics:

```json
{
  "173.249.195.59:21999": {
    "total_queries": 1440,
    "successful_queries": 1438,
    "failed_queries": 2,
    "avg_latency_ms": 45.2,
    "last_seen": "2025-11-03T12:00:00Z",
    "uptime_percentage": 99.86
  }
}
```

### GET /api/history

Historical uptime snapshots (last 100):

```json
[
  {
    "timestamp": "2025-11-03T11:59:00Z",
    "total_nodes": 2
  },
  {
    "timestamp": "2025-11-03T12:00:00Z",
    "total_nodes": 2
  }
]
```

## Production Deployment

### Option 1: Systemd Service (Linux)

Create `/etc/systemd/system/dinero-registry.service`:

```ini
[Unit]
Description=Dinero Global Node Registry
After=network.target

[Service]
Type=simple
User=dinero
WorkingDirectory=/opt/dinero/registry
ExecStart=/usr/bin/python3 /opt/dinero/registry/dinero_registry.py --port 8080
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl enable dinero-registry
sudo systemctl start dinero-registry
sudo systemctl status dinero-registry
```

### Option 2: Docker

Create `Dockerfile`:

```dockerfile
FROM python:3.11-slim
WORKDIR /app
COPY dinero_registry.py .
RUN pip install requests
EXPOSE 8080
CMD ["python3", "dinero_registry.py", "--port", "8080"]
```

Build and run:

```bash
docker build -t dinero-registry .
docker run -d -p 8080:8080 --name registry dinero-registry
```

### Option 3: Nginx Reverse Proxy

Expose registry at `https://status.dinero-coin.com/nodes.json`:

```nginx
server {
    listen 443 ssl;
    server_name status.dinero-coin.com;

    ssl_certificate /etc/letsencrypt/live/status.dinero-coin.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/status.dinero-coin.com/privkey.pem;

    location / {
        proxy_pass http://localhost:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
```

## Integration Examples

### Dinero-Qt / GUI Wallet

Auto-discover healthy nodes:

```cpp
// src/qt/networkpage.cpp
QNetworkReply* reply = manager->get(
    QNetworkRequest(QUrl("https://status.dinero-coin.com/nodes.json"))
);

connect(reply, &QNetworkReply::finished, [reply]() {
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonArray nodes = doc["nodes"].toArray();

    for (const QJsonValue& val : nodes) {
        QJsonObject node = val.toObject();
        QString ip = node["ip"].toString();
        int wsPort = node["ws_port"].toInt();
        int latency = node["latency_ms"].toInt();

        if (latency < 100) {
            // Connect to low-latency node
            connectToNode(ip, wsPort);
        }
    }
});
```

### JavaScript / Web Wallet

```javascript
async function findBestNode() {
    const resp = await fetch('https://status.dinero-coin.com/nodes.json');
    const data = await resp.json();

    // Sort by latency
    const sorted = data.nodes.sort((a, b) =>
        a.latency_ms - b.latency_ms
    );

    // Connect to fastest node
    if (sorted.length > 0) {
        const best = sorted[0];
        ws = new WebSocket(`ws://${best.ip}:${best.ws_port}`);
        console.log(`Connected to ${best.name} (${best.latency_ms}ms)`);
    }
}
```

### CLI Monitoring

```bash
# Watch live status
watch -n 5 'curl -s http://localhost:8080/api/status | jq'

# Get fastest node
curl -s http://localhost:8080/nodes.json | \
  jq -r '.nodes | sort_by(.latency_ms) | .[0] | "\(.name): \(.ip):\(.ws_port)"'

# Check uptime stats
curl -s http://localhost:8080/api/stats | jq
```

## Node Registration (Future Enhancement)

To allow nodes to self-register, extend `dinero_registry.py` with:

```python
def do_POST(self):
    if self.path == '/api/register':
        content_length = int(self.headers['Content-Length'])
        body = self.rfile.read(content_length)
        node_info = json.loads(body)

        # Validate signature (optional)
        if verify_node_signature(node_info):
            # Add to registry
            add_node(node_info['source_url'])
            self._set_headers(201)
            self.wfile.write(b'{"status": "registered"}')
        else:
            self._set_headers(403)
            self.wfile.write(b'{"error": "invalid signature"}')
```

Then nodes can auto-register:

```bash
curl -X POST https://status.dinero-coin.com/api/register \
  -H "Content-Type: application/json" \
  -d @/var/lib/dinero/serverinfo.json
```

## Daemon Integration

Add registry sync to `dinerod`:

```cpp
// src/httprpc.cpp
void SyncToRegistry() {
    if (!gArgs.GetBoolArg("-registry", false)) return;

    std::string registryUrl = gArgs.GetArg("-registryurl",
        "https://status.dinero-coin.com/api/register");

    Json::Value info = GetServerInfoJson();
    std::string payload = info.toStyledString();

    // POST to registry
    HttpPostJson(registryUrl, payload);
    LogPrintf("Synced to global registry: %s\n", registryUrl);
}
```

Enable with:

```bash
dinerod -registry=1 -registryurl=https://status.dinero-coin.com/api/register
```

## Monitoring & Alerts

### Prometheus Metrics Export

Add `/metrics` endpoint:

```python
def do_GET(self):
    if path == '/metrics':
        metrics = f"""
# HELP dinero_nodes_total Total configured nodes
# TYPE dinero_nodes_total gauge
dinero_nodes_total {node_data['total_configured']}

# HELP dinero_nodes_alive Number of responsive nodes
# TYPE dinero_nodes_alive gauge
dinero_nodes_alive {node_data['total_nodes']}
        """
        self._set_headers(content_type='text/plain')
        self.wfile.write(metrics.encode('utf-8'))
```

### Uptime Monitoring

Use with Uptime Kuma, Pingdom, or StatusCake:

```bash
# Health check endpoint
curl https://status.dinero-coin.com/api/status
```

## Security Considerations

1. **Rate Limiting**: Add nginx rate limiting to prevent abuse
2. **HTTPS Only**: Always use TLS in production
3. **Input Validation**: Sanitize all node URLs before fetching
4. **Node Signatures**: Verify authenticity with cryptographic signatures
5. **Firewall Rules**: Restrict POST /api/register to known IP ranges

## Troubleshooting

### Registry shows 0 nodes

Check connectivity to nodes:

```bash
curl -v http://173.249.195.59:21999/serverinfo.json
```

### High latency measurements

Increase timeout:

```bash
./dinero_registry.py --timeout 10
```

### Registry crashes on startup

Ensure requests library is installed:

```bash
pip3 install --upgrade requests
```

## Architecture

```
┌─────────────┐     HTTP GET      ┌──────────────┐
│   Node 1    │◄──────────────────│              │
│ serverinfo  │                   │   Registry   │
└─────────────┘                   │   Server     │
                                  │              │
┌─────────────┐     HTTP GET      │  (Python)    │
│   Node 2    │◄──────────────────│              │
│ serverinfo  │                   │              │
└─────────────┘                   └───────┬──────┘
                                          │
┌─────────────┐     HTTP GET             │
│   Node 3    │◄─────────────────────────┘
│ serverinfo  │                           │
└─────────────┘                           │
                                          │
                                  ┌───────▼──────┐
                                  │   HTTP API   │
                                  │ /nodes.json  │
                                  └──────────────┘
                                          │
                    ┌─────────────────────┼─────────────────────┐
                    │                     │                     │
            ┌───────▼───────┐     ┌──────▼──────┐      ┌──────▼──────┐
            │  Dinero-Qt    │     │ Web Wallet  │      │  Monitoring │
            │  GUI Wallet   │     │  Browser    │      │   Grafana   │
            └───────────────┘     └─────────────┘      └─────────────┘
```

## License

Same as Dinero Core (MIT)

## Support

- Issues: https://github.com/dinerocoin/dinero/issues
- Docs: https://docs.dinero-coin.com/
