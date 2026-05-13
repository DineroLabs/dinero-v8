# DineroCoin Script Template Library

Complete reference for the DineroCoin testing and automation script template library.

## Overview

The script template library is a comprehensive collection of reusable testing patterns, automation scripts, and helper utilities for the DineroCoin project. It ensures consistent testing across platforms and provides standardized templates for common development tasks.

## Directory Structure

```
DineroCoin/
├── test_seed_compatibility.sh          # NEW: Seed phrase compatibility testing
├── tools/
│   ├── cli_test_runner.sh              # CLI test harness (main)
│   ├── run_all_tests.sh                # Master test orchestrator
│   ├── boot_regtest.sh                 # Regtest daemon launcher
│   ├── boot_testnet.sh                 # Testnet daemon launcher
│   ├── calculate_expected_addresses.sh # NEW: BIP84 address calculator
│   └── README.md                       # CLI test documentation
├── test/
│   ├── rpc/
│   │   ├── run_rpc_tests.sh           # RPC parity test runner
│   │   ├── rpc_parity_test.cpp        # RPC parity tests
│   │   └── hardware_wallet_rpc_test.cpp
│   ├── integration/                    # Integration test scripts
│   ├── soak/                          # Long-running stability tests
│   └── chaos/                         # Chaos engineering tests
├── TESTING_GUIDE_CROSS_PLATFORM.md    # Cross-platform testing guide
├── SEED_TESTING_README.md              # NEW: Seed testing documentation
└── SCRIPT_TEMPLATE_LIBRARY.md          # NEW: This file
```

## Core Components

### 1. Seed Compatibility Testing

**Location:** `./test_seed_compatibility.sh`

**Purpose:** Validate BIP39/BIP84 seed phrase compatibility between iOS and Qt Desktop wallets

**Usage:**
```bash
./test_seed_compatibility.sh quick  # Quick validation
./test_seed_compatibility.sh full   # Complete test suite
./test_seed_compatibility.sh verify # Manual verification guide
```

**Features:**
- BIP39 seed phrase validation
- BIP84 derivation path testing (m/84'/0'/0'/0/0)
- Multiple test vectors (abandon, zoo, custom seeds)
- Cross-platform compatibility verification
- Automatic cleanup

**Documentation:** [SEED_TESTING_README.md](./SEED_TESTING_README.md)

---

### 2. CLI Test Harness

**Location:** `./tools/cli_test_runner.sh`

**Purpose:** Comprehensive CLI testing framework with multiple test modes

**Usage:**
```bash
./tools/cli_test_runner.sh smoke     # Basic functionality
./tools/cli_test_runner.sh e2e       # End-to-end workflow
./tools/cli_test_runner.sh negative  # Error handling
./tools/cli_test_runner.sh discover  # Command discovery
```

**Features:**
- JSON response validation with `jq`
- RPC connectivity testing
- Wallet operations testing
- Command discovery
- Precedence testing

**Documentation:** [tools/README.md](./tools/README.md)

---

### 3. Master Test Runner

**Location:** `./tools/run_all_tests.sh`

**Purpose:** Orchestrate all test suites in proper sequence

**Usage:**
```bash
./tools/run_all_tests.sh quick  # Essential tests
./tools/run_all_tests.sh full   # Complete test suite
```

**Test Sequence:**
1. Smoke tests (basic functionality)
2. E2E tests (mining + wallet)
3. Discovery tests (command enumeration)
4. Negative tests (error handling) - full mode
5. Seed compatibility (full) - full mode
6. Seed compatibility (quick) - always

**Features:**
- Automatic daemon management
- Binary building if needed
- Comprehensive test summary
- Clean environment between tests

---

### 4. RPC Test Suite

**Location:** `./test/rpc/run_rpc_tests.sh`

**Purpose:** RPC parity and hardware wallet testing

**Usage:**
```bash
./test/rpc/run_rpc_tests.sh           # All RPC tests
./test/rpc/run_rpc_tests.sh --verbose # Verbose output
./test/rpc/run_rpc_tests.sh --rebuild # Rebuild before testing
```

**Features:**
- RPC parity validation
- Hardware wallet RPC testing
- Method registration verification
- Error handling validation

**Documentation:** [test/rpc/README.md](./test/rpc/README.md)

---

### 5. BIP84 Address Calculator

**Location:** `./tools/calculate_expected_addresses.sh`

**Purpose:** Calculate expected BIP84 addresses for test seeds

**Usage:**
```bash
./tools/calculate_expected_addresses.sh
```

**Features:**
- Calculates addresses for all test seeds
- Provides copy-paste values for test scripts
- Supports both CLI and RPC methods
- Automatic cleanup

**Output:**
```
EXPECTED_ADDR_1="din1q..."
EXPECTED_ADDR_2="din1q..."
EXPECTED_ADDR_3="din1q..."
```

---

### 6. Daemon Boot Scripts

**Regtest:** `./tools/boot_regtest.sh`
**Testnet:** `./tools/boot_testnet.sh`

**Purpose:** Launch daemons for testing

**Usage:**
```bash
./tools/boot_regtest.sh  # Start regtest daemon
./tools/boot_testnet.sh  # Start testnet daemon
```

**Features:**
- Automatic data directory creation
- Proper port configuration
- Daemon mode startup

---

## Standard Script Template

All scripts in the library follow this consistent pattern:

```bash
#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

MODE="${1:-default}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Resolve binaries
if [[ -n "${CLI:-}" ]]; then
  CLI="$CLI"
elif [[ -x ./build/dinero-cli ]]; then
  CLI=./build/dinero-cli
else
  echo "❌ Could not find dinero-cli" >&2
  exit 127
fi

# Environment setup
RPC_PORT="${RPC_PORT:-20001}"
RPC_URL="http://127.0.0.1:${RPC_PORT}/"
COOKIE="$DATADIR/.cookie"

# ============================================================================
# Helper Functions
# ============================================================================

_fail() {
  echo "❌ $*" >&2
  cleanup
  exit 1
}

_ok() {
  echo "✅ $*"
}

_warn() {
  echo "⚠️  $*" >&2
}

_info() {
  echo "ℹ️  $*"
}

cleanup() {
  # Cleanup code here
}

assert_jq() {
  local filter="$1"; shift
  local out; out="$("$@" | jq -c .)" || _fail "command failed: $*"
  echo "$out" | jq -e "$filter" >/dev/null || {
    echo "$out" | jq .
    _fail "jq assert failed: $filter"
  }
}

# ============================================================================
# Test Functions
# ============================================================================

test_something() {
  _info "Testing something..."
  # Test implementation
  _ok "Test passed"
}

# ============================================================================
# Main
# ============================================================================

main() {
  _info "Starting script..."

  # Setup
  setup_environment

  # Run tests
  test_something

  # Cleanup
  cleanup

  _ok "Complete!"
}

trap cleanup EXIT INT TERM
main "$@"
```

## Common Patterns

### 1. Binary Resolution

```bash
# Resolve CLI binary
if [[ -n "${CLI:-}" ]]; then
  CLI="$CLI"
elif [[ -x ./build/dinero-cli ]]; then
  CLI=./build/dinero-cli
elif [[ -x ./build/bin/dinero-cli ]]; then
  CLI=./build/bin/dinero-cli
else
  echo "❌ Could not find dinero-cli" >&2
  exit 127
fi
```

### 2. RPC Authentication

```bash
# Read cookie for authentication
AUTH="$(tr -d '\r\n' < "$COOKIE")"

# Make RPC call
curl -sS --user "$AUTH" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getinfo","params":[]}' \
  "$RPC_URL"
```

### 3. JSON Validation

```bash
# Assert JSON field exists and has correct type
assert_jq '.result.balance|type=="number"' run_cli wallet balance

# Assert JSON has specific field
assert_jq '.result|has("hashrate")' run_cli getmininginfo
```

### 4. Daemon Management

```bash
# Start daemon
"$DAEMON" --regtest --rpcport="$RPC_PORT" \
  --datadir="$TEST_DIR" --daemon

# Wait for daemon to be ready
timeout=10
elapsed=0
while [[ $elapsed -lt $timeout ]]; do
  if [[ -f "$TEST_DIR/regtest/.cookie" ]]; then
    sleep 1
    break
  fi
  sleep 1
  ((elapsed++))
done

# Stop daemon
pkill -f "dinerod.*datadir=$TEST_DIR" || true
```

### 5. Cleanup Traps

```bash
cleanup() {
  rm -rf "$TEST_DIR"
  pkill -f "dinerod.*datadir=$TEST_DIR" || true
}

trap cleanup EXIT INT TERM
```

### 6. Test Modes

```bash
MODE="${1:-quick}"

case "$MODE" in
  quick)
    run_quick_tests
    ;;
  full)
    run_full_tests
    ;;
  *)
    _fail "Invalid mode: $MODE"
    ;;
esac
```

## Environment Variables

Standard environment variables used across all scripts:

| Variable | Purpose | Default | Example |
|----------|---------|---------|---------|
| `CLI` | CLI binary path | Auto-detect | `/path/to/dinero-cli` |
| `DAEMON` | Daemon binary path | Auto-detect | `/path/to/dinerod` |
| `RPC_PORT` | RPC server port | 20001 | `28998` |
| `RPC_URL` | RPC server URL | `http://127.0.0.1:20001/` | Custom URL |
| `R` | Regtest data dir | `/tmp/dinero_regtest` | Custom path |
| `T` | Testnet data dir | Project testnet dir | Custom path |
| `TEST_BASE_DIR` | Test directory | `/tmp/dinero-*-test` | Custom path |
| `EXPECTED_ADDR_1` | Expected address 1 | `din1qTODO` | Calculated address |
| `EXPECTED_ADDR_2` | Expected address 2 | `din1qTODO` | Calculated address |
| `EXPECTED_ADDR_3` | Expected address 3 | `din1qTODO` | Calculated address |

## Exit Codes

Standard exit codes across all scripts:

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General failure |
| 2 | Invalid arguments |
| 127 | Binary not found |

## CI/CD Integration

### GitHub Actions

```yaml
name: Script Template Tests
on: [push, pull_request]

jobs:
  all-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build
        run: mkdir build && cd build && cmake .. && make -j4
      - name: Run all tests
        run: ./tools/run_all_tests.sh full
```

### GitLab CI

```yaml
test:
  stage: test
  script:
    - mkdir build && cd build && cmake .. && make -j4 && cd ..
    - ./tools/run_all_tests.sh full
```

### Jenkins

```groovy
stage('All Tests') {
    steps {
        sh 'mkdir -p build && cd build && cmake .. && make -j4'
        sh './tools/run_all_tests.sh full'
    }
}
```

## Testing Best Practices

### 1. Always Use Helper Functions

✅ **Good:**
```bash
assert_jq '.result|type=="object"' run_cli getinfo
_ok "Test passed"
```

❌ **Bad:**
```bash
result=$(run_cli getinfo)
echo "$result" | jq '.result|type=="object"'
echo "Test passed"
```

### 2. Clean Up Resources

✅ **Good:**
```bash
cleanup() {
  rm -rf "$TEST_DIR"
  pkill -f "dinerod.*$TEST_DIR" || true
}
trap cleanup EXIT INT TERM
```

❌ **Bad:**
```bash
# No cleanup - leaves temp files and processes
```

### 3. Provide Clear Output

✅ **Good:**
```bash
_info "Running seed validation..."
_ok "Seed has valid length: 12 words"
_fail "Address mismatch: got $addr, expected $expected"
```

❌ **Bad:**
```bash
echo "Running test"
echo "Test passed"
echo "Error"
```

### 4. Handle Errors Gracefully

✅ **Good:**
```bash
set -euo pipefail
set +e
result=$("$CLI" command 2>&1)
rc=$?
set -e
if [[ $rc -ne 0 ]]; then
  _warn "Command failed, trying alternative..."
fi
```

❌ **Bad:**
```bash
# No error handling - script fails silently
$CLI command
```

## Adding New Test Scripts

When creating a new test script:

1. **Copy the standard template** from this document
2. **Follow naming conventions:** `test_*.sh` or `*_test.sh`
3. **Make it executable:** `chmod +x script.sh`
4. **Add to test suite:** Update `tools/run_all_tests.sh`
5. **Document it:** Add section to this file
6. **Test it:** Run with `quick` and `full` modes
7. **Update CI/CD:** Add to CI configuration if needed

### Example: Adding a new test

```bash
# 1. Create the script
cat > test_new_feature.sh << 'EOF'
#!/usr/bin/env bash
set -euo pipefail
# ... template code ...
EOF

# 2. Make executable
chmod +x test_new_feature.sh

# 3. Add to run_all_tests.sh
# Edit tools/run_all_tests.sh and add:
#   echo "🧪 Test N: New feature tests"
#   ./test_new_feature.sh quick

# 4. Test it
./test_new_feature.sh quick
./test_new_feature.sh full

# 5. Run full suite
./tools/run_all_tests.sh full
```

## Documentation Files

| File | Purpose |
|------|---------|
| `TESTING_GUIDE_CROSS_PLATFORM.md` | Cross-platform testing procedures |
| `SEED_TESTING_README.md` | Seed compatibility testing guide |
| `SCRIPT_TEMPLATE_LIBRARY.md` | This file - complete reference |
| `tools/README.md` | CLI test harness documentation |
| `test/rpc/README.md` | RPC testing documentation |
| `IMPLEMENTATION_SUMMARY.md` | Implementation details |

## Quick Reference

### Run All Tests

```bash
./tools/run_all_tests.sh full
```

### Run Specific Test Suite

```bash
./tools/cli_test_runner.sh smoke
./test_seed_compatibility.sh quick
./test/rpc/run_rpc_tests.sh
```

### Calculate Addresses

```bash
./tools/calculate_expected_addresses.sh
```

### Boot Daemons

```bash
./tools/boot_regtest.sh
./tools/boot_testnet.sh
```

### Clean Environment

```bash
pkill -f dinerod || true
rm -rf /tmp/dinero-*-test
```

## Troubleshooting

### Binary Not Found

```bash
# Build binaries first
mkdir -p build && cd build
cmake ..
make -j4
cd ..
```

### Port Already in Use

```bash
# Kill existing daemons
pkill -f dinerod

# Or use different port
RPC_PORT=29999 ./test_seed_compatibility.sh quick
```

### Tests Hanging

```bash
# Check for stuck daemons
ps aux | grep dinerod

# Kill all daemons
pkill -9 dinerod

# Clean test directories
rm -rf /tmp/dinero-*
```

## Future Enhancements

Planned additions to the script template library:

- [ ] Performance benchmarking scripts
- [ ] Load testing templates
- [ ] Security audit scripts
- [ ] Migration testing scripts
- [ ] Multi-node network testing
- [ ] Hardware wallet integration tests
- [ ] Mobile app testing automation

## Contributing

When contributing to the script template library:

1. Follow the standard template pattern
2. Use consistent helper functions
3. Add comprehensive error handling
4. Include cleanup traps
5. Document in this file
6. Update related documentation
7. Test with `quick` and `full` modes
8. Submit PR with test results

## License

Same as DineroCoin project license.

---

**Last Updated:** 2025-11-03
**Version:** 1.0.0
