# DineroCoin CLI v1.0.0 Support Matrix

## Platform Support

### Operating Systems
| OS | Architecture | Status | Notes |
|----|--------------|--------|-------|
| macOS 13+ | arm64 (Apple Silicon) | ✅ Supported | Primary development platform |
| macOS 13+ | x86_64 (Intel) | ✅ Supported | Full compatibility |
| Linux | amd64 (x86_64) | ✅ Supported | glibc 2.31+ required |
| Linux | arm64 (aarch64) | ✅ Supported | glibc 2.31+ required |
| Windows | x86_64 | 🔄 Planned | Future release |

### Linux Distributions
| Distribution | Version | Status |
|--------------|---------|--------|
| Ubuntu | 20.04+ | ✅ Supported |
| Debian | 11+ | ✅ Supported |
| CentOS/RHEL | 8+ | ✅ Supported |
| Alpine | 3.15+ | ⚠️ Limited | musl libc compatibility |

## Daemon Compatibility

### Minimum Requirements
- **dinerod**: v0.6.0+
- **RPC API**: v2.0+ (backward compatible with v1.0)
- **WebSocket**: Optional, auto-detected

### Network Support
| Network | Status | Default Port | Notes |
|---------|--------|--------------|-------|
| mainnet | ✅ Production | 20998 | Full feature support |
| testnet | ✅ Production | 20998 | Full feature support |
| regtest | ✅ Development | 20996 | Development/testing |

## Schema Compatibility

### JSON Contract
- **Current**: `din.cli.v1`
- **Stability**: No breaking changes within v1.x
- **Migration**: Breaking changes require `din.cli.v2` + CLI v2.0.0

### Exit Codes (sysexits.h compatible)
| Code | Meaning | Usage |
|------|---------|-------|
| 0 | Success | Command completed successfully |
| 64 | Usage Error | Invalid arguments or command syntax |
| 69 | Service Unavailable | Daemon not running or unreachable |
| 75 | Temporary Failure | RPC timeout or temporary network issue |
| 77 | Permission Denied | Authentication failure or insufficient permissions |
| 1 | Generic Error | Unspecified error condition |

## Feature Matrix

### Core Features
| Feature | Status | Since Version |
|---------|--------|---------------|
| Basic RPC calls | ✅ Stable | v0.1.0 |
| Wallet operations | ✅ Stable | v0.2.0 |
| Network commands | ✅ Stable | v0.3.0 |
| Mining operations | ✅ Stable | v0.4.0 |
| Security hardening | ✅ Stable | v0.5.0 |
| Paging & filtering | ✅ Stable | v1.0.0 |
| Profile management | ✅ Stable | v1.0.0 |

### Advanced Features
| Feature | Status | Requirements |
|---------|--------|--------------|
| WebSocket transport | ✅ Auto-detect | dinerod WebSocket support |
| Cookie authentication | ✅ Required | Proper file permissions |
| TLS/HTTPS | ✅ Recommended | Production deployments |
| Profile switching | ✅ Available | ~/.dinero-cli/profiles.json |

## Installation Methods

### Package Managers
| Method | Status | Command |
|--------|--------|---------|
| Homebrew (macOS) | ✅ Available | `brew tap dinero/din && brew install din` |
| Linuxbrew | 🔄 Planned | Future release |
| APT (Ubuntu/Debian) | 🔄 Planned | Future release |
| RPM (CentOS/RHEL) | 🔄 Planned | Future release |

### Manual Installation
| Method | Status | Notes |
|--------|--------|-------|
| GitHub Releases | ✅ Available | Pre-built binaries with checksums |
| Docker Image | ✅ Available | `docker pull dinerocoin/cli:1.0.0` |
| Build from Source | ✅ Available | CMake 3.16+, C++17 compiler |

## Dependencies

### Runtime Dependencies
| Component | Version | Purpose |
|-----------|---------|---------|
| glibc | 2.31+ | Linux runtime (not needed on macOS) |
| ca-certificates | Latest | HTTPS certificate validation |

### Build Dependencies
| Component | Version | Purpose |
|-----------|---------|---------|
| CMake | 3.16+ | Build system |
| C++ Compiler | C++17 | GCC 9+, Clang 10+, MSVC 2019+ |
| Qt6 | 6.0+ | Optional GUI components |
| Boost | 1.70+ | Networking and system utilities |
| JsonCpp | 1.9+ | JSON parsing (vendored) |
| RocksDB | 6.0+ | Database backend (vendored) |

## Performance Characteristics

### Memory Usage
| Operation | Typical RAM | Peak RAM | Notes |
|-----------|-------------|----------|-------|
| Basic commands | <10 MB | <20 MB | Status, balance, etc. |
| Large UTXO list | 50-200 MB | 500 MB | With --limit 5000 cap |
| Transaction history | 20-100 MB | 200 MB | Paginated results |
| Full mempool | 10-50 MB | 100 MB | Network dependent |

### Network Usage
| Operation | Typical | Peak | Notes |
|-----------|---------|------|-------|
| RPC call | 1-10 KB | 100 KB | Command dependent |
| Large dataset | 100 KB-1 MB | 10 MB | With pagination |
| WebSocket | 1-5 KB/msg | N/A | Real-time updates |

## Security Model

### Authentication
- **Cookie-based**: Primary method, file permission validation
- **Basic Auth**: Supported for custom setups
- **TLS**: Required for production (HTTPS/WSS)

### Permissions
- **Cookie file**: Must be readable only by user (600)
- **Config files**: Should be user-readable only (600)
- **Profile data**: No sensitive information stored

### Security Flags
| Flag | Purpose | Production Use |
|------|---------|----------------|
| `--accept-insecure-cookie` | Bypass permission checks | ❌ Never |
| `--http-only` | Force HTTP transport | ❌ Development only |
| `--verbose` | Show connection details | ⚠️ Debug only |

## Troubleshooting

### Common Issues
| Issue | Cause | Solution |
|-------|-------|---------|
| "Cookie security error" | Wrong file permissions | `chmod 600 ~/.dinero/*/cookie` |
| "Service unavailable" | Daemon not running | Start dinerod, check --rpc-url |
| "Network mismatch" | Wrong network config | Check --network flag and daemon |
| "Profile not found" | Missing profiles.json | Create ~/.dinero-cli/profiles.json |

### Debug Commands
```bash
# Check version and build info
dinero-cli --version

# Test connectivity with verbose output
dinero-cli --verbose --nodeinfo

# Validate profile configuration
dinero-cli --profile prod --dry-run --nodeinfo

# Test with minimal timeout
dinero-cli --timeout 5 --retries 1 status
```

## Upgrade Path

### From v0.x to v1.0.0
- **Breaking changes**: None (additive release)
- **New features**: Paging, filtering, profiles
- **Migration**: No action required

### Future Upgrades
- **v1.x**: Backward compatible, additive features only
- **v2.0**: May include breaking changes, migration guide provided
- **Schema**: `din.cli.v2` for breaking JSON contract changes

## Support Channels

- **Documentation**: [GitHub Wiki](https://github.com/dinerocoin/dinerocoin/wiki)
- **Issues**: [GitHub Issues](https://github.com/dinerocoin/dinerocoin/issues)
- **Community**: [Discord](https://discord.gg/dinerocoin)
- **Security**: security@dinero-coin.com
