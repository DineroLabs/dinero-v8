# Witness Mining Verification

This document describes the witness mining verification system that serves as the single source of truth for mining health in the Dinero project.

## Overview

The witness mining verification system ensures that:
- ✅ Coinbase transactions are created directly from cached witness data
- ✅ ScriptPubKey format is correct (0014 + 20 bytes for P2WPKH)
- ✅ No Bech32 decode errors occur during mining
- ✅ HRP consistency is maintained across the system
- ✅ Mining operations complete successfully

## Quick Start

### Basic Verification
```bash
# Quick verification (single block)
make verify-quick

# Full verification (2 blocks, with artifacts)
make verify

# Full verification with debugging enabled
make verify-debug
```

### Command Line Usage
```bash
# Basic verification
./scripts/verify-witness-mining.sh --spinup --spinup-blocks 1 --strict

# With JSON output
./scripts/verify-witness-mining.sh --spinup --spinup-blocks 2 \
  --json /tmp/verify.json --json-stdout

# Custom configuration
./scripts/verify-witness-mining.sh \
  --spinup --spinup-blocks 3 \
  --timeout 120 \
  --spinup-keep \
  --strict
```

## Makefile Targets

| Target | Description |
|--------|-------------|
| `test` | **Default** - Run full verification + negative tests |
| `verify` | Run witness mining verification only |
| `test-negative` | Run negative tests only (prove guardrails work) |
| `verify-quick` | Quick verification (single block) |
| `verify-debug` | Full verification with debugging enabled |
| `clean` | Remove test artifacts |
| `help` | Show available targets |

## CLI Options

| Option | Description | Default |
|--------|-------------|---------|
| `--strict` | Exit with code 1 if any required check fails | false |
| `--json <file>` | Write machine-readable results to file | none |
| `--json-stdout` | Also output JSON to STDOUT (for CI logs) | false |
| `--blocks <N>` | Check last N blocks | 5 |
| `--log <file>` | Log file to analyze | auto-detected |
| `--datadir <path>` | Override datadir auto-detection | auto-detected |
| `--rpcport <port>` | Override RPC port auto-detection | auto-detected |
| `--spinup` | Start throwaway regtest node, mine blocks, verify, cleanup | false |
| `--spinup-blocks <N>` | Number of blocks to mine in spinup mode | 3 |
| `--spinup-datadir <path>` | Datadir for spinup node | auto-generated |
| `--spinup-rpcport <port>` | RPC port for spinup node | auto-assigned |
| `--spinup-keep` | Keep spinup node running after verification (for debugging) | false |
| `--timeout <sec>` | Timeout for mining and shutdown operations | 60 |

## Verification Process

### 1. Witness-Direct Coinbase Creation
**Check**: Looks for `'Creating coinbase transaction directly from witness data'`
**Purpose**: Ensures mining uses cached witness data instead of re-decoding addresses

### 2. Correct Script Format
**Check**: Looks for `'Coinbase script from cached witness (len=20): 0014...'`
**Purpose**: Validates P2WPKH script format (0014 + 20-byte witness program)

### 3. No Bech32 Decode Errors
**Check**: Looks for `'Failed to decode Bech32'`
**Purpose**: Ensures no address decoding failures during mining

### 4. HRP Consistency
**Check**: Looks for `'HRP=rdin'` (regtest) or `'HRP=din'` (mainnet)
**Purpose**: Prevents HRP drift across the system

### 5. Mining Success
**Check**: Looks for `'Block added successfully at height'`
**Purpose**: Confirms mining operations complete successfully

### 6. On-Chain Script Validation (Optional)
**Check**: Validates actual blockchain data via RPC
**Purpose**: Provides on-chain proof of script format correctness

## JSON Output Schema

```json
{
  "ok": true,
  "rpc": {
    "port": "23812",
    "datadir": "/tmp/verify-spinup-90813"
  },
  "log": "/tmp/verify-spinup-90813/daemon.log",
  "checks": {
    "witness_coinbase": {"found": 1},
    "script_format": {"count": 1, "format": "0014+20bytes"},
    "bech32_errors": 0,
    "hrp": "rdin",
    "blocks_mined": 0,
    "onchain_validated": {
      "available": true,
      "note": "RPC verbosity=2 needed for full validation"
    }
  }
}
```

## CI Integration

### GitHub Actions
The verification script is integrated into CI via `.github/workflows/verify-witness-mining.yml`:

```yaml
- name: Run verification
  run: |
    set -euo pipefail
    ./scripts/verify-witness-mining.sh \
      --spinup --spinup-blocks 2 --strict \
      --json /tmp/verify.json --json-stdout | tee /tmp/verify.log

- name: Verify results
  run: |
    jq -e '.ok==true' /tmp/verify.json
    jq -e '.checks.witness_coinbase.found>0' /tmp/verify.json
    jq -e '.checks.script_format.count>0' /tmp/verify.json
    jq -e '.checks.bech32_errors==0' /tmp/verify.json
```

### Local CI Testing
```bash
# Test the full CI workflow locally
make test

# Test just the verification
make verify

# Test just the negative tests
make test-negative
```

## Negative Testing

The system includes negative tests to prove guardrails work:

```bash
# Run negative tests
make test-negative

# Or directly
./scripts/test-negative-witness-mining.sh
```

**Negative Test Scenarios:**
1. **Non-existent log file** - Script should fail with clear error
2. **Bech32 errors in logs** - Script should detect errors in strict mode
3. **HRP drift** - Script should detect mainnet HRP in regtest logs
4. **No witness mining** - Script should detect missing witness-based mining

## Troubleshooting

### Common Issues

#### Port Conflicts
```bash
# Check for port conflicts
lsof -iTCP:20998

# Use custom port
./scripts/verify-witness-mining.sh --spinup --spinup-rpcport 20996
```

#### Daemon Already Running
```bash
# Check running daemons
ps aux | grep dinerod

# Kill conflicting daemon
pkill -f dinerod
```

#### Cleanup Issues
```bash
# Manual cleanup
make clean

# Force cleanup of spinup nodes
rm -rf /tmp/verify-spinup-*
```

### Debug Mode
```bash
# Enable debugging
make verify-debug

# Or with CLI
./scripts/verify-witness-mining.sh --spinup --spinup-keep --json /tmp/debug.json
```

## Architecture

### Spinup Mode
- **Hermetic**: Creates isolated regtest environment
- **Automatic**: Self-contained setup, mining, verification, cleanup
- **Configurable**: Customizable block count, timeouts, cleanup behavior

### Verification Engine
- **Log Analysis**: Parses daemon logs for verification patterns
- **RPC Validation**: Optional on-chain script validation
- **Failure Detection**: Comprehensive error detection and reporting

### Cleanup System
- **Trap-based**: Guaranteed cleanup on exit/interrupt/CTRL-C
- **Graceful**: Attempts graceful shutdown before force killing
- **Configurable**: Can preserve nodes for debugging

## Best Practices

### For Developers
1. **Run verification before commits**: `make verify-quick`
2. **Use strict mode in CI**: Always include `--strict` flag
3. **Check artifacts**: Review `/tmp/verify.json` for detailed results
4. **Debug with keep mode**: Use `--spinup-keep` to inspect failed nodes

### For CI/CD
1. **Gate on verification**: Block merges if verification fails
2. **Collect artifacts**: Always upload verification results
3. **Use spinup mode**: Ensures hermetic testing environment
4. **Assert results**: Programmatically verify JSON output

### For Monitoring
1. **Track metrics**: Monitor verification success rates
2. **Alert on failures**: Set up alerts for verification failures
3. **Trend analysis**: Track verification performance over time
4. **Dashboard integration**: Use JSON output for monitoring systems

## Contributing

### Adding New Checks
1. Add verification logic to the script
2. Update JSON schema to include new check results
3. Add negative test cases for failure scenarios
4. Update documentation

### Modifying Verification Logic
1. Ensure backward compatibility
2. Update JSON schema version if needed
3. Test with both positive and negative cases
4. Update CI assertions if necessary

## Support

For issues or questions:
1. Check the troubleshooting section
2. Run with debug mode: `make verify-debug`
3. Review the verification logs
4. Check the JSON output for detailed results
