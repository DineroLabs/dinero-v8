# 🚀 Dinero Production Deployment

**Deploy a production-ready Dinero cryptocurrency node in under 5 minutes.**

## ⚡ Quick Start

### 🐧 Linux (Recommended)

```bash
# One-liner deployment
sudo ./scripts/deploy-oneliner.sh
```

### 🍎 macOS (Development)

```bash
# Quick start
DATADIR="$(mktemp -d -t din-prod.XXXX)"
./build-debug/bin/dinerod -server=1 -daemon=0 -printtoconsole \
  -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 \
  -rpcport=0 -wsport=0 -p2p -port=0 -autowallet=default \
  -nodeinfo="$DATADIR/nodeinfo.json" -datadir="$DATADIR"
```

### 🐳 Docker

```bash
# Container deployment
docker compose -f docker-compose.production.yml up -d
```

## 📚 Documentation

| File | Purpose |
|------|---------|
| **[DEPLOY_ANYWHERE.md](DEPLOY_ANYWHERE.md)** | Copy-paste commands for all platforms |
| **[PRODUCTION_LAUNCH_KIT.md](PRODUCTION_LAUNCH_KIT.md)** | Complete production guide |
| **[scripts/deploy-oneliner.sh](scripts/deploy-oneliner.sh)** | Automated Linux deployment |
| **[scripts/deploy-production.sh](scripts/deploy-production.sh)** | Full production setup |

## 🧪 Test Before Deploy

```bash
# Test all deployment methods
./scripts/test-deploy-methods.sh
```

## ✅ Verify Deployment

```bash
# Check service (Linux)
systemctl status dinerod
dinero-health

# Check RPC endpoints
NODEINFO=/var/lib/dinero/nodeinfo.json  # Linux
RPC=$(jq -r .rpc "$NODEINFO")
COOKIE=/var/lib/dinero/data/.cookie
AUTH=$(tr -d '\r\n' < "$COOKIE")

curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq '.result.p2p'
```

## 🎯 Success Indicators

**Healthy Node Shows:**
- ✅ `gethealth.p2p.peers ≥ 1`
- ✅ `getblockchaininfo.headers ≥ blocks`
- ✅ Service active and stable
- ✅ Clean logs with sync progress

## 🔒 Security Features

- **Localhost-only RPC** (127.0.0.1)
- **Systemd hardening** (NoNewPrivileges, ProtectSystem)
- **Dedicated user** (dinero)
- **Secure file permissions** (.cookie 0600)
- **Rate limiting ready** (reverse proxy configs included)

## 📊 Monitoring

```bash
# Built-in health check
dinero-health

# Service logs
journalctl -u dinerod -f

# Prometheus metrics (optional)
python3 scripts/prometheus-exporter.py
```

## 🔧 Operations

```bash
# Restart node
sudo systemctl restart dinerod

# Stop node
sudo systemctl stop dinerod

# Upgrade binary
sudo systemctl stop dinerod
sudo cp build-debug/bin/dinerod /usr/local/bin/dinerod
sudo systemctl start dinerod
```

## 🏆 Production Ready

This deployment includes:

- ✅ **Automated setup** - One command deployment
- ✅ **Security hardening** - Systemd protection, localhost-only
- ✅ **Health monitoring** - Built-in health checks
- ✅ **Observability** - Structured logs, metrics export
- ✅ **Resilience** - Auto-restart, crash recovery
- ✅ **Operations** - Easy upgrades, backups, maintenance

## 🚀 Deploy Now!

**Choose your deployment method and run the commands above. Your Dinero node will be live and syncing in minutes!**

**Ready for mainnet. Ready for production. Ready to mine!** ⛏️🔥
