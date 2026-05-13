# Node Registration Guide
## Registering 173.249.195.59:18222 with Dinero Registry

### ✅ Prerequisites Verified
1. **Host reachable**: 173.249.195.59 responds to ping (46ms latency)
2. **Registry implemented**: Your registry code is complete and functional
3. **Serverinfo endpoint**: The `/serverinfo` HTTP endpoint is implemented in dinerod

### 🔧 Method 1: Manual Registration (Add to DEFAULT_NODES)

Edit `dinero_registry_extended.py` line 20-23:

```python
DEFAULT_NODES = [
    "https://173.249.195.59:18222/serverinfo",  # Your Virginia node
    "http://172.93.160.131:21999/serverinfo",   # California node
]
```

**Note**: Use `https://` if your node has SSL/TLS enabled, otherwise use `http://`

### 🚀 Method 2: Self-Registration via API

From your node server (173.249.195.59), run:

```bash
curl -X POST http://YOUR_REGISTRY_IP:8080/api/register \
  -H "Content-Type: application/json" \
  -d '{"serverinfo_url": "https://173.249.195.59:18222/serverinfo"}'
```

Expected response:
```json
{"status": "success", "message": "Node registered successfully"}
```

### 🧪 Method 3: Test Registration Locally

```bash
# Start registry with your node
cd /Users/haydarevich/Documents/DineroCoin/registry
python3 dinero_registry_extended.py \
  --port 8080 \
  -i 30 \
  -n "https://173.249.195.59:18222/serverinfo"

# In another terminal, check status
curl http://localhost:8080/api/status | python3 -m json.tool
```

### ⚠️ Troubleshooting

#### Issue 1: Connection Timeout
**Symptoms**: Registry shows node as offline
**Solutions**:
1. Verify `/serverinfo` endpoint is accessible:
   ```bash
   curl -k https://173.249.195.59:18222/serverinfo
   ```
2. Check firewall rules on 173.249.195.59:
   ```bash
   # On the node server
   sudo firewall-cmd --list-ports  # Or ufw status
   ```
3. Ensure port 18222 is open for HTTP/HTTPS traffic

#### Issue 2: SSL/TLS Certificate Error
**Symptoms**: `SSL: CERTIFICATE_VERIFY_FAILED`
**Solutions**:
1. Use HTTP instead of HTTPS (for testing):
   ```python
   "http://173.249.195.59:18222/serverinfo"
   ```
2. Or disable SSL verification (NOT recommended for production):
   - Modify registry to use `requests.get(url, verify=False)`

#### Issue 3: Wrong Port
**Symptoms**: Connection refused
**Solutions**:
- Verify RPC port in your dinerod config:
  ```bash
  # On node server
  grep rpcport ~/.dinero/dinero.conf
  ```
- Common Dinero ports:
  - **18222**: Your custom RPC port (HTTPS)
  - **21999**: Default mainnet RPC port
  - **20999**: Default mainnet P2P port

### ✅ Verification Steps

After registration, verify the node appears in registry:

```bash
# 1. Check API status
curl http://localhost:8080/api/status

# 2. Get full node list
curl http://localhost:8080/nodes.json | python3 -m json.tool

# 3. View web dashboard
open http://localhost:8080/

# 4. Check registered nodes specifically
curl http://localhost:8080/api/registered
```

### 📊 Expected Output

Once successfully registered, you should see:

```json
{
  "timestamp": "2025-11-04T04:00:00Z",
  "total_nodes": 1,
  "total_nodes_alive": 1,
  "nodes": [
    {
      "name": "173.249.195.59",
      "ip": "173.249.195.59",
      "rpc_port": 18222,
      "network": "mainnet",
      "uptime": 12345,
      "connections": 8,
      "latency_ms": 46.2,
      "uptime_percentage": 100.0,
      "status": "alive"
    }
  ]
}
```

### 🔐 Production Recommendations

1. **Enable HMAC Validation**:
   ```bash
   python3 dinero_registry_extended.py --secret "your-secret-key"
   ```

2. **Set up Nginx reverse proxy** (see `nginx.conf.example`)

3. **Enable SSL/TLS** with Let's Encrypt:
   ```bash
   sudo certbot --nginx -d status.dinero-coin.com
   ```

4. **Configure systemd service**:
   ```bash
   sudo cp dinero-registry.service /etc/systemd/system/
   sudo systemctl enable --now dinero-registry
   ```

### 📝 Next Steps

1. ✅ Verify `/serverinfo` endpoint is accessible from external networks
2. ✅ Choose registration method (manual or API)
3. ✅ Start registry with your node URL
4. ✅ Confirm node appears as "alive" in dashboard
5. ✅ Deploy to production server (status.dinero-coin.com)

---

**Questions?** Check the main [README.md](README.md) or [QUICKSTART.md](QUICKSTART.md)
