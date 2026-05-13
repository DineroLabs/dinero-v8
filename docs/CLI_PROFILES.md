# DineroCoin CLI Profiles

## Overview

CLI profiles enable quick environment switching for development, testing, and production deployments. Profiles store connection settings, network configurations, and operational parameters in `~/.dinero-cli/profiles.json`.

## Profile Precedence

Settings are resolved in this order (highest to lowest priority):
1. **Command-line flags** - `--rpc-url`, `--wallet`, etc.
2. **Environment variables** - `DINERO_RPC_URL`, `DINERO_WALLET`, etc.
3. **Active profile** - `--profile NAME` or default profile
4. **Config file** - `~/.dinero/dinero.conf`
5. **Auto-detection** - Network discovery and defaults

## Configuration File

Create `~/.dinero-cli/profiles.json`:

```json
{
  "default": "dev",
  "profiles": {
    "dev": {
      "network": "regtest",
      "rpc_url": "http://127.0.0.1:20996",
      "datadir": "/tmp/din-dev",
      "wallet": "dev",
      "read_timeout_ms": 5000
    },
    "testnet": {
      "network": "testnet",
      "rpc_url": "http://127.0.0.1:20998",
      "datadir": "~/.dinero/testnet",
      "wallet": "test",
      "read_timeout_ms": 15000
    },
    "prod": {
      "network": "main",
      "rpc_url": "https://node1.example.com:20998",
      "datadir": "~/.dinero",
      "wallet": "exchange",
      "read_timeout_ms": 30000,
      "retries": 5
    }
  }
}
```

## Usage Examples

### Basic Profile Usage
```bash
# Use specific profile
dinero-cli --profile prod --nodeinfo

# Check wallet balance on testnet
dinero-cli --profile testnet wallet balance

# List transactions with paging
dinero-cli --profile prod wallet history --limit 100 --offset 0
```

### Profile with Overrides
```bash
# Use prod profile but override wallet
dinero-cli --profile prod --wallet backup wallet balance

# Use dev profile but connect to different node
dinero-cli --profile dev --rpc-url http://192.168.1.100:20996 --nodeinfo
```

### Environment Variables
```bash
# Set profile via environment
export DINERO_PROFILE=prod
dinero-cli wallet balance

# Override specific settings
export DINERO_RPC_URL=https://backup-node.example.com:20998
dinero-cli --profile prod --nodeinfo
```

## Profile Fields

| Field | Description | Example |
|-------|-------------|---------|
| `network` | Network type | `"main"`, `"testnet"`, `"regtest"` |
| `rpc_url` | RPC endpoint | `"https://node.example.com:20998"` |
| `datadir` | Data directory | `"~/.dinero"` |
| `wallet` | Default wallet | `"exchange"` |
| `read_timeout_ms` | RPC timeout | `30000` |
| `retries` | Retry attempts | `5` |

## Security Considerations

- **Cookie files**: Profiles store cookie file paths, never contents
- **HTTPS required**: Production profiles should use HTTPS endpoints
- **File permissions**: Ensure `profiles.json` is readable only by user (`600`)
- **No secrets**: Never store private keys or sensitive data in profiles

## Profile Commands

```bash
# Show active profile info (included in --nodeinfo)
dinero-cli --nodeinfo

# List available profiles
dinero-cli profile list

# Show specific profile
dinero-cli profile show prod

# Set default profile
dinero-cli profile set-default testnet
```

## Operational Workflows

### Development
```bash
# Quick regtest setup
dinero-cli --profile dev --wait-ready --timeout 30
dinero-cli --profile dev wallet create_wallet dev
dinero-cli --profile dev wallet balance
```

### Production Monitoring
```bash
# Check node health
dinero-cli --profile prod --wait-ready --timeout 60
dinero-cli --profile prod --nodeinfo

# Monitor wallet
dinero-cli --profile prod wallet balance
dinero-cli --profile prod wallet history --limit 10 --confirmed-only
```

### Exchange Operations
```bash
# Page through large UTXO set
dinero-cli --profile prod wallet utxos --confirmed-only --min-amount 0.01 --limit 500

# Filter recent transactions
dinero-cli --profile prod wallet history --since "2024-01-01T00:00:00Z" --limit 1000
```

## Troubleshooting

### Profile Not Found
```bash
dinero-cli --profile missing --nodeinfo
# Error: Profile 'missing' not found in ~/.dinero-cli/profiles.json
```

### Connection Issues
```bash
# Test connectivity with retries
dinero-cli --profile prod --retries 5 --timeout 30 --nodeinfo

# Use insecure cookie (dev only)
dinero-cli --profile dev --accept-insecure-cookie --nodeinfo
```

### Profile Validation
```bash
# Check profile resolution
dinero-cli --profile prod --verbose --nodeinfo
# Shows: Using profile 'prod': rpc_url=https://..., wallet=exchange
```
