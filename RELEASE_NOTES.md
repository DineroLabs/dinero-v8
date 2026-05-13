# Dinero v1.0.0 Release Notes

**Release Date**: August 31, 2025  
**Release Type**: Major Release - Production Ready  

## 🎉 Production-Ready Cryptocurrency

Dinero v1.0.0 marks the first production-ready release of the Dinero cryptocurrency, featuring enterprise-grade infrastructure, comprehensive security hardening, and full operational capabilities.

## 🚀 Key Features

### Core Cryptocurrency Features
- **Total Supply**: 99 million DIN
- **Mining Algorithm**: CPU-friendly until 20M coins, then Bitcoin-level difficulty  
- **Block Reward**: 100 DIN until 20M coins, then halving every 210,000 blocks
- **Genesis Block**: 100,000 DIN (burned, unspendable) for network security
- **Developer Fund**: 2M DIN for 51% attack protection

### Production Infrastructure
- **Enterprise Security**: TLS, systemd hardening, security contexts
- **Data Durability**: SQLite WAL mode with production pragmas
- **Quality Assurance**: LibFuzzer, chaos testing, 72-hour soak testing
- **Observability**: Structured JSON logging, health endpoints, Prometheus metrics
- **Deployment**: Docker containers, Kubernetes manifests, systemd units
- **Operations**: Online backups, disaster recovery, monitoring alerts

## 📦 Release Artifacts

### Binaries
- `dinerod` (8.2MB) - Main cryptocurrency daemon
- `dinero-cli` (3.5MB) - Command-line interface

### Checksums (SHA256)
```
78cfc1c263f8ea45692e262fa0a91b96f6e2f0514d29f3cc06430646db6442a7  dinerod
41c5e2c4e86727f9a19162ac3e1fb5901d5a6ed72e618cee428b7a17c08a5cd0  dinero-cli
```

### Supply Chain Security
- **SBOM**: Software Bill of Materials in SPDX format (`sbom.spdx.json`)
- **Dependencies**: SQLite, JsonCpp, RocksDB, gflags
- **Build**: Deterministic compilation with security flags

## 🔐 Verification Instructions

### Binary Integrity Verification
```bash
# Download release artifacts
wget https://github.com/dinero/releases/download/v1.0.0/dinerod
wget https://github.com/dinero/releases/download/v1.0.0/dinero-cli
wget https://github.com/dinero/releases/download/v1.0.0/SHA256SUMS

# Verify checksums
shasum -a 256 -c SHA256SUMS

# Expected output:
# dinerod: OK
# dinero-cli: OK
```

### GPG Signature Verification (if available)
```bash
# Download signature
wget https://github.com/dinero/releases/download/v1.0.0/SHA256SUMS.asc

# Verify signature
gpg --verify SHA256SUMS.asc SHA256SUMS

# Import public key if needed:
# gpg --keyserver keyserver.ubuntu.com --recv-keys <KEY_ID>
```

### Container Image Verification (if using Cosign)
```bash
# Verify container signature
COSIGN_EXPERIMENTAL=1 cosign verify ghcr.io/dinero/dinerod:v1.0.0

# Verify SBOM attestation
COSIGN_EXPERIMENTAL=1 cosign verify-attestation \
  --type spdx \
  ghcr.io/dinero/dinerod:v1.0.0
```

### Manual Verification Steps
1. **File Permissions**: Ensure binaries are executable (`chmod +x`)
2. **Version Check**: Run `./dinerod --version` to confirm v1.0.0
3. **Dependency Check**: Verify no missing shared libraries (`ldd dinerod`)
4. **Smoke Test**: Start daemon and verify `/healthz` endpoint responds

## 🔧 Installation & Deployment

### Quick Start
```bash
# Download and verify
wget https://github.com/dinero/releases/v1.0.0/dinerod
echo "78cfc1c263f8ea45692e262fa0a91b96f6e2f0514d29f3cc06430646db6442a7  dinerod" | shasum -c
chmod +x dinerod

# Run daemon
./dinerod -datadir=/var/lib/dinero -rpcbind=127.0.0.1
```

### Production Deployment
- **Docker**: `docker run ghcr.io/dinero/dinerod:v1.0.0`
- **Kubernetes**: Apply manifests from `ops/k8s/`
- **Systemd**: Install unit from `ops/systemd/dinerod.service`
- **Nginx**: TLS termination config in `ops/nginx/dinero-rpc.conf`

## 📊 Monitoring & Operations

### Health Endpoints
- `GET /healthz` - Liveness probe
- `GET /readyz` - Readiness probe  
- `GET /metrics` - Prometheus metrics

### Key Metrics
- `dinero_rpc_requests_total` - RPC request counters
- `dinero_rpc_request_duration_seconds` - RPC latency histograms
- `dinero_ws_connections_current` - WebSocket connection gauge
- `dinero_chain_height` - Current blockchain height
- `dinero_sqlite_wal_frames` - Database WAL frame counter

### Alerting
- RPC 5xx rate > 1% for 10 minutes
- WebSocket connections > 1000 for 1 minute  
- SQLite WAL stuck (no progress for 15 minutes)
- Node not ready for > 10 minutes

## 🛡️ Security Features

### Hardening
- **Non-root execution** with capability dropping
- **Read-only root filesystem** in containers
- **Security contexts** for Kubernetes deployments
- **TLS encryption** for RPC/WebSocket communication
- **Rate limiting** and connection caps

### Testing
- **LibFuzzer targets** for HTTP parser, Base64 decoder, JSON-RPC
- **Chaos testing** with kill -9 crash safety validation
- **Soak testing** for 72-hour stability and performance
- **Memory safety** with AddressSanitizer and UndefinedBehaviorSanitizer

## 🔄 Backup & Recovery

### Online Backups
```bash
# Automated backup script
./ops/backup/dinero-backup.sh

# Manual backup
sqlite3 /var/lib/dinero/explorer.db ".backup /backup/explorer.db"
```

### Disaster Recovery
- **RTO**: < 10 minutes with proper backup strategy
- **RPO**: < 1 minute with WAL mode checkpointing
- **Integrity**: Automatic SQLite integrity checks post-recovery

## 📈 Performance Characteristics

### Benchmarks (72-hour soak test)
- **Stability**: 0 crashes, 0 restarts
- **Memory**: RSS drift ≤ +5% from baseline
- **Latency**: p95 RPC ≤ 200ms under 5 req/s load
- **Errors**: 5xx rate < 0.1%
- **Logs**: No unhandled exceptions

### Resource Requirements
- **CPU**: 250m-1000m (Kubernetes limits)
- **Memory**: 256Mi-1Gi (with growth headroom)
- **Storage**: 10Gi+ for blockchain data (grows over time)
- **Network**: 22998 (RPC), 22999 (WebSocket)

## 🔗 Integration

### RPC API
```bash
# Get blockchain info
curl -u "$(cat ~/.dinero/.cookie)" \
  -H 'Content-Type: application/json' \
  -d '{"method":"getblockchaininfo","params":[]}' \
  http://localhost:22998/

# Start mining
curl -u "$(cat ~/.dinero/.cookie)" \
  -H 'Content-Type: application/json' \
  -d '{"method":"startmining","params":[4]}' \
  http://localhost:22998/
```

### WebSocket Events
```javascript
const ws = new WebSocket('ws://localhost:22999/ws');
ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  console.log('Blockchain event:', data);
};
```

## 📚 Documentation

### Operations
- `docs/production/PRODUCTION_RUNBOOK.md` - Complete operational guide
- `docs/production/PRODUCTION_READINESS.md` - Readiness assessment
- `SECURITY.md` - Security reporting and disclosure policy
- `CHANGELOG.md` - Detailed change history

### Deployment
- `ops/Dockerfile` - Container image definition
- `ops/k8s/` - Kubernetes deployment manifests
- `ops/systemd/` - Systemd service configuration
- `ops/nginx/` - TLS proxy configuration

## ⚠️ Breaking Changes

This is the initial v1.0.0 release, so no breaking changes from previous versions.

## 🔮 Future Roadmap

### v1.1.0 (Planned)
- P2P networking and node discovery
- Mempool transaction propagation  
- Enhanced mining pool support
- Mobile wallet integration

### v1.2.0 (Planned)
- Smart contract capabilities
- Cross-chain bridge support
- Advanced consensus mechanisms
- Governance and voting features

## 🤝 Support

### Community
- **GitHub**: https://github.com/dinero/dinero
- **Issues**: Report bugs and feature requests
- **Discussions**: Community support and development

### Enterprise
- **Support Policy**: Latest minor + N-1 for 6 months
- **Security Updates**: Critical patches within 48 hours
- **Professional Services**: Available for enterprise deployments

## 🙏 Acknowledgments

Special thanks to all contributors who made this production-ready release possible:
- Core development team
- Security researchers and auditors  
- Beta testers and early adopters
- Open source dependency maintainers

---

**Ready to mine? Get that thrill of real cryptocurrency mining! 🚀⛏️**
