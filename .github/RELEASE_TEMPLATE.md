# DineroCoin CLI v1.0.0

## 🎉 Production-Ready Release

DineroCoin CLI v1.0.0 introduces enterprise-grade paging, filtering, and profile management capabilities designed for mainnet operations, exchange integrations, and production deployments.

## 📦 Downloads

### Binaries
- [macOS Apple Silicon](https://github.com/dinerocoin/dinerocoin/releases/download/v1.0.0/dinero-cli-v1.0.0-darwin-arm64.tar.gz)
- [macOS Intel](https://github.com/dinerocoin/dinerocoin/releases/download/v1.0.0/dinero-cli-v1.0.0-darwin-x86_64.tar.gz)
- [Linux x86_64](https://github.com/dinerocoin/dinerocoin/releases/download/v1.0.0/dinero-cli-v1.0.0-linux-amd64.tar.gz)
- [Linux ARM64](https://github.com/dinerocoin/dinerocoin/releases/download/v1.0.0/dinero-cli-v1.0.0-linux-arm64.tar.gz)

### Verification
```bash
# Download checksums
curl -LO https://github.com/dinerocoin/dinerocoin/releases/download/v1.0.0/SHA256SUMS
curl -LO https://github.com/dinerocoin/dinerocoin/releases/download/v1.0.0/SHA256SUMS.asc

# Verify checksums
sha256sum -c SHA256SUMS

# Verify GPG signature (optional)
gpg --verify SHA256SUMS.asc SHA256SUMS
```

### Package Managers
```bash
# Homebrew (macOS)
brew tap dinero/din
brew install din

# Docker
docker pull dinerocoin/cli:1.0.0
```

## ✨ What's New

### 📄 Paging & Filtering System
Handle large datasets safely with memory protection and terminal flooding prevention:

```bash
# Page through large UTXO sets
dinero-cli wallet utxos --confirmed-only --min-amount 0.01 --limit 500

# Filter transaction history by time
dinero-cli wallet history --since "2024-01-01T00:00:00Z" --min-conf 6 --limit 1000

# Command-specific filters for transactions, UTXOs, peers, mempool
dinero-cli net peers --state connected --min-version 70016 --limit 200
```

### 🔧 Profile Management
Quick environment switching for development, testing, and production:

```bash
# Setup profiles
mkdir -p ~/.dinero-cli
cp examples/profiles.json ~/.dinero-cli/

# Switch environments instantly
dinero-cli --profile dev --nodeinfo
dinero-cli --profile prod wallet balance
```

### 📊 Enhanced JSON Output
Stable automation contract with backward compatibility:

```json
{
  "schema": "din.cli.v1",
  "command": "wallet_history",
  "ok": true,
  "data": [ /* results */ ],
  "page": {
    "limit": 100,
    "returned": 100,
    "has_more": true,
    "next_offset": 100
  }
}
```

## 🚀 Production Features

### Exchange Operations
- **Large UTXO handling**: Memory-safe pagination for 100k+ UTXO sets
- **Transaction filtering**: Efficient address/amount/confirmation filtering
- **Batch processing**: Deterministic pagination for reliable automation

### Node Operations
- **Peer management**: State and version filtering for connection analysis
- **Mempool monitoring**: Fee-rate and transaction-specific filtering
- **Health checking**: `--wait-ready` with configurable timeouts

### Development Workflows
- **Environment switching**: Instant dev/test/prod profile switching
- **Local testing**: Regtest-optimized configurations
- **Integration testing**: Stable JSON contracts for CI/CD

## 📋 Quick Start

### Installation
```bash
# Extract binary
tar -xzf dinero-cli-v1.0.0-*.tar.gz
sudo mv dinero-cli /usr/local/bin/

# Verify installation
dinero-cli --version
```

### Basic Usage
```bash
# Check node status
dinero-cli --nodeinfo

# Get wallet balance
dinero-cli wallet balance

# Page through transaction history
dinero-cli wallet history --limit 10 --format json
```

### Profile Setup
```bash
# Create profile directory
mkdir -p ~/.dinero-cli

# Create profiles.json
cat > ~/.dinero-cli/profiles.json << 'EOF'
{
  "default": "dev",
  "profiles": {
    "dev": {
      "network": "regtest",
      "rpc_url": "http://127.0.0.1:18443"
    },
    "prod": {
      "network": "main",
      "rpc_url": "https://node.example.com:20998"
    }
  }
}
EOF

# Use profiles
dinero-cli --profile prod --nodeinfo
```

## 🔒 Security & Compatibility

### Security Features
- **Cookie permission validation**: Automatic security checks
- **HTTPS enforcement**: Production-grade TLS support
- **Profile isolation**: No credential storage, secure configuration

### Compatibility
- **Schema**: `din.cli.v1` with backward compatibility guarantee
- **Exit codes**: Standard sysexits.h compatible codes
- **Daemon**: Compatible with dinerod v0.6.0+

## 📚 Documentation

- **[CLI Profiles Guide](docs/CLI_PROFILES.md)**: Complete profile setup and usage
- **[Schema Contract](docs/CLI_SCHEMA_CONTRACT.md)**: JSON envelope specification  
- **[Operational Runbook](docs/CLI_OPERATIONAL_RUNBOOK.md)**: Production workflows
- **[Support Matrix](docs/SUPPORT_MATRIX.md)**: Platform and compatibility info

## 🔧 For Developers

### Build from Source
```bash
git clone https://github.com/dinerocoin/dinerocoin.git
cd dinerocoin
git checkout v1.0.0
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target dinero-cli
```

### Docker Usage
```bash
# Run with profiles
docker run -v ~/.dinero-cli:/home/dinero/.dinero-cli \
  dinerocoin/cli:1.0.0 --profile prod --nodeinfo

# One-off commands
docker run dinerocoin/cli:1.0.0 --version
```

## 🐛 Known Issues

None. This release has been thoroughly tested for production deployment.

## 📞 Support

- **Documentation**: [GitHub Wiki](https://github.com/dinerocoin/dinerocoin/wiki)
- **Issues**: [GitHub Issues](https://github.com/dinerocoin/dinerocoin/issues)
- **Community**: [Discord](https://discord.gg/dinerocoin)

---

**Ready for production deployment! 🚀**

This release represents a significant milestone in DineroCoin's production readiness, enabling safe mainnet operations for exchanges, monitoring systems, and enterprise deployments.
