# DineroCoin CLI v1.0.0 Release Notes

## 🎉 Production-Ready CLI Release

DineroCoin CLI v1.0.0 introduces enterprise-grade paging, filtering, and profile management capabilities designed for mainnet operations, exchange integrations, and production deployments.

## ✨ New Features

### 📄 **Paging & Filtering System**
- **Pagination flags**: `--limit N` (default 100, max 5000), `--offset N`, `--cursor TOKEN`
- **Time filtering**: `--since <ISO8601|height>`, `--until <ISO8601|height>`
- **Command-specific filters**:
  - `listtransactions`: `--min-conf`, `--address`, `--type`, `--label`
  - `listutxos`: `--min-amount`, `--max-amount`, `--confirmed-only`
  - `peers`: `--state`, `--min-version`
  - `mempool`: `--min-fee-rate`, `--txid`
- **Safety guardrails**: Automatic limit capping at 5000, `--all` requires JSON format

### 🔧 **Profile Management**
- **Quick environment switching**: `--profile NAME` for dev/test/prod configurations
- **Profile precedence**: flags → env → --profile → config → auto
- **Secure configuration**: Profiles in `~/.dinero-cli/profiles.json`
- **Environment injection**: Per-profile environment variables

### 📊 **Enhanced JSON Output**
- **Versioned schema**: `din.cli.v1` with backward compatibility guarantee
- **Pagination metadata**: Complete page info with `has_more`, `next_offset`, active filters
- **Stable automation contract**: Reliable for exchange integrations and monitoring

### 🛡️ **Production Safety**
- **Memory protection**: Prevents crashes from large datasets (100k+ UTXOs)
- **Terminal protection**: `--all` flag requires `--format json`
- **Connection reliability**: `--wait-ready`, `--retries`, `--timeout` flags
- **Security hardening**: Cookie permission checks, HTTPS enforcement

## 🔧 Usage Examples

### Paging Large Datasets
```bash
# Page through confirmed UTXOs
dinero-cli wallet utxos --confirmed-only --min-amount 0.01 --limit 500

# Continue pagination
dinero-cli wallet utxos --confirmed-only --min-amount 0.01 --limit 500 --offset 500

# Filter transaction history
dinero-cli wallet history --since "2024-01-01T00:00:00Z" --min-conf 6 --limit 1000
```

### Profile-Based Operations
```bash
# Quick environment switching
dinero-cli --profile prod --nodeinfo
dinero-cli --profile dev wallet balance

# Profile with overrides
dinero-cli --profile prod --wallet backup wallet balance
```

### JSON Automation
```bash
# Stable pagination contract
dinero-cli --format json wallet history --limit 100 | jq '.page.has_more'

# Extract next page offset
dinero-cli --format json wallet utxos --limit 500 | jq '.page.next_offset'
```

## 📋 JSON Schema Contract

All JSON output follows the versioned `din.cli.v1` envelope:

```json
{
  "schema": "din.cli.v1",
  "command": "wallet_history",
  "network": "main",
  "rpc_url": "https://node1.example.com:20998/wallet/exchange",
  "wallet": "exchange",
  "ok": true,
  "data": [ /* results */ ],
  "page": {
    "limit": 100,
    "returned": 100,
    "offset": 0,
    "has_more": true,
    "next_offset": 100,
    "cursor": null,
    "filters": { "min_conf": 6 }
  }
}
```

## 🔒 Security Enhancements

- **Cookie security**: Automatic permission validation, insecure override warnings
- **HTTPS enforcement**: Production profiles require secure connections  
- **Profile isolation**: No credential storage, path-based cookie references
- **Audit logging**: Client identification headers for server-side monitoring

## 🚀 Production Readiness

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

## 📚 Documentation

- **[CLI Profiles Guide](docs/CLI_PROFILES.md)**: Complete profile setup and usage
- **[Schema Contract](docs/CLI_SCHEMA_CONTRACT.md)**: JSON envelope specification
- **[Operational Runbook](docs/CLI_OPERATIONAL_RUNBOOK.md)**: Production workflows and troubleshooting

## 🔧 Technical Details

### Exit Codes
- `0`: Success
- `64`: Usage error (invalid arguments)
- `69`: Service unavailable (daemon down)
- `75`: Temporary failure (RPC timeout)
- `77`: Permission denied (auth failure)
- `1`: Generic error

### Environment Variables
- `DINERO_PROFILE`: Default profile name
- `DINERO_RPC_URL`: RPC endpoint override
- `DINERO_WALLET`: Default wallet name
- `DINERO_*`: Standard prefix for all CLI environment variables

### Version Information
```bash
dinero-cli --version
# dinero-cli 1.0.0
# Git SHA: abc123def456
# Build Date: Sep 10 2024 15:33:54
# Schema: din.cli.v1
```

## 🏗️ Breaking Changes

This is the initial v1.0.0 release. Future breaking changes will require schema version bump to `din.cli.v2` with migration guidance.

## 🐛 Known Issues

None. This release has been thoroughly tested for production deployment.

## 📦 Installation

### Binary Releases
Download platform-specific binaries from the releases page:
- `dinero-cli-v1.0.0-darwin-arm64.tar.gz` (macOS Apple Silicon)
- `dinero-cli-v1.0.0-darwin-x86_64.tar.gz` (macOS Intel)
- `dinero-cli-v1.0.0-linux-amd64.tar.gz` (Linux x86_64)
- `dinero-cli-v1.0.0-linux-arm64.tar.gz` (Linux ARM64)

### Build from Source
```bash
git clone https://github.com/dinerocoin/dinerocoin.git
cd dinerocoin
git checkout v1.0.0
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target dinero-cli
```

## 🔍 Verification

All release artifacts include SHA256 checksums and GPG signatures:
```bash
# Verify checksum
sha256sum -c SHA256SUMS

# Verify GPG signature
gpg --verify SHA256SUMS.asc SHA256SUMS
```

## 🙏 Acknowledgments

This release represents a significant milestone in DineroCoin's production readiness, enabling safe mainnet operations for exchanges, monitoring systems, and enterprise deployments.

## 📞 Support

- **Documentation**: [docs/](docs/)
- **Issues**: [GitHub Issues](https://github.com/dinerocoin/dinerocoin/issues)
- **Community**: [Discord](https://discord.gg/dinerocoin)

---

**Ready for production deployment! 🚀**
