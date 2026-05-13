# GUI Wallet Smoke Test

Comprehensive smoke test script for DineroCoin's GUI wallet features including encryption/unlock, address generation, balance queries, import operations, backup/restore, and blockchain rescanning.

## Features Tested

### 🔐 **Wallet Security**
- **Encryption**: Automatic wallet encryption with passphrase
- **Unlock**: Session-based wallet unlocking with timeout
- **Authentication**: Bearer token and cookie-based auth

### 💰 **Core Wallet Operations**
- **Address Generation**: HD wallet address derivation (`getnewaddress`)
- **Address Validation**: Bech32 format validation (`validateaddress`)
- **Balance Queries**: Real-time balance checking (`getbalance`)
- **Transaction History**: Transaction listing (`listtransactions`)

### 📥 **Import Operations**
- **WIF Import**: Private key import from WIF format (`wallet.import`)
- **Vault Import**: Encrypted vault blob import (`wallet.vault.import`)
- **Rescan Control**: Optional blockchain rescanning

### 💾 **Backup & Recovery**
- **Wallet Backup**: Encrypted wallet file backup (`wallet.backup`)
- **Wallet Restore**: Backup file restoration (`wallet.restore`)
- **Blockchain Rescan**: Historical transaction discovery (`wallet.rescan`)

### 🌐 **Network Integration**
- **Auto-Discovery**: Automatic daemon port detection (20996, 40998, or dynamic)
- **Health Checks**: Daemon health monitoring via `/healthz`
- **Multi-Auth**: Bearer token with cookie fallback

## Usage

### Basic Usage
```bash
# Run with defaults (regtest mode, test passphrase)
./scripts/gui_wallet_smoke.sh
```

### Advanced Usage
```bash
# Custom configuration
export APP_BIN="build/src/gui-desktop/dinero-desktop.app/Contents/MacOS/dinero-desktop"
export PASSPHRASE="my-secure-passphrase"
export UNLOCK_SECS="300"
export WIF_TEST="KwYgW8gcSSiNjbYd2HNiqdaUTAhLE4WQVhGjdBFfDJYCiPzUXhfA"
export VAULT_BLOB="base64-encoded-vault-data"
export VAULT_PASSPHRASE="vault-passphrase"

./scripts/gui_wallet_smoke.sh
```

## Configuration Options

| Variable | Default | Description |
|----------|---------|-------------|
| `APP_BIN` | `build/src/gui-desktop/dinero-desktop.app/Contents/MacOS/dinero-desktop` | GUI application binary path |
| `PASSPHRASE` | `test-pass-123` | Wallet encryption passphrase |
| `UNLOCK_SECS` | `600` | Wallet unlock timeout (seconds) |
| `REGTEST` | `1` | Use regtest mode |
| `WIF_TEST` | `""` | Optional WIF key for import testing |
| `VAULT_BLOB` | `""` | Optional encrypted vault blob |
| `VAULT_PASSPHRASE` | `""` | Vault decryption passphrase |

## Cookie Locations

The script automatically searches for authentication cookies in:

1. `$DINERO_COOKIE_FILE` (when set)
2. `$HOME/.dinero/regtest/.cookie` (primary)
3. `$HOME/.dinero/.cookie`

## Port Detection

Automatic daemon port detection tries:

1. **20996** (standard regtest RPC port)
2. **40998** (alternative port)
3. **Dynamic detection** via `lsof` for `dinerod` processes

## Sample Output

```bash
🔧 Killing leftovers

🚀 Launching GUI in background

🌐 Waiting for daemon health...
Daemon RPC port = 20996

🎫 Minting Bearer token
Bearer token acquired

📊 getnetworkinfo
{
  "result": {
    "connections": 0,
    "networks": [],
    "version": "1.0.0"
  }
}

🔐 Encrypting wallet
{
  "result": {
    "encrypted": true,
    "message": "Wallet encrypted successfully"
  }
}

🔓 Unlocking wallet for 600s
{
  "result": {
    "unlocked_until": 1694889234
  }
}

🏠 New address
Address: rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh

✅ Smoke test complete.
```

## Validation Script

Use the companion validation script to test RPC method availability:

```bash
./scripts/validate_wallet_rpcs.sh
```

This will test all required RPC methods and report their availability status.

## Requirements

- **macOS** with DineroCoin built in `build/` directory
- **jq** for JSON processing (`brew install jq`)
- **curl** for HTTP requests
- **lsof** for port detection
- **Running daemon** (launched automatically by GUI)

## Troubleshooting

### "Daemon health not ready"
- Ensure GUI binary exists at specified path
- Check if daemon is already running on another port
- Verify build completed successfully

### "Cookie malformed"
- Delete existing cookie files and restart
- Check daemon permissions and data directory access

### "Bearer mint failed"
- Normal behavior - script falls back to cookie authentication
- Indicates `rpc.createauth` method not available (expected)

### Import operations fail
- Set `WIF_TEST` to a valid WIF private key
- Ensure `VAULT_BLOB` is properly base64 encoded
- Check that wallet is unlocked before import

## Integration with CI/CD

The script is designed for automated testing:

```bash
# CI-friendly run with timeout
timeout 120 ./scripts/gui_wallet_smoke.sh || echo "Smoke test timed out"

# Validate specific features
export WIF_TEST="KwYgW8gcSSiNjbYd2HNiqdaUTAhLE4WQVhGjdBFfDJYCiPzUXhfA"
./scripts/gui_wallet_smoke.sh | grep "✅ Smoke test complete" || exit 1
```

## Security Notes

- **Test passphrases only**: Never use production passphrases
- **Regtest mode**: Script defaults to regtest for safety
- **Temporary files**: Backup files created in `/tmp` are cleaned up
- **No logging of secrets**: Sensitive data is not logged to console

## Related Files

- `scripts/gui_wallet_smoke.sh` - Main smoke test script
- `scripts/validate_wallet_rpcs.sh` - RPC method validation
- `scripts/regtest_wallet_smoke.sh` - Legacy regtest-specific tests
- `scripts/test_gui_integration.sh` - GUI integration tests
