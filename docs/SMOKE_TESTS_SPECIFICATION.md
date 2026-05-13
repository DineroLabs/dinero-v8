# Smoke Tests Specification - v3.0.0-alpha1

**Date:** 2026-01-11
**Purpose:** Platform-specific smoke tests for release verification
**Scope:** macOS, Linux, Windows

---

## Overview

**Smoke tests** are minimal functional tests that verify basic daemon/client operations before release. They complement the comprehensive test suite by catching **runtime** issues that unit tests might miss (e.g., missing shared libraries, file permissions, network binding).

### Objectives

- ✅ Verify daemon starts successfully
- ✅ Verify RPC connectivity works
- ✅ Verify basic blockchain operations (generate blocks)
- ✅ Verify wallet operations (addresses, balance)
- ✅ Verify daemon shuts down cleanly

### Execution Context

**When:** After build, before packaging
**Where:** CI runners (GitHub Actions) + local developer testing
**Duration:** < 2 minutes per platform
**Mode:** Regtest (isolated, fast, deterministic)

---

## Test Categories

### Category 1: Process Management
- Daemon startup
- Daemon health check
- Daemon shutdown

### Category 2: RPC Interface
- RPC connectivity
- Authentication
- Command response validation

### Category 3: Blockchain Operations
- Block generation
- Block retrieval
- Chain state queries

### Category 4: Wallet Operations
- Address generation
- Balance queries
- UTXO listing
- Transaction creation (optional for alpha)

### Category 5: P2P Network (Optional)
- Port binding
- Peer connectivity (future)

---

## Universal Smoke Test Script

**File:** `scripts/smoke-test.sh`

```bash
#!/usr/bin/env bash
# DineroCoin Smoke Test Script
# Tests basic daemon functionality in regtest mode

set -euo pipefail

PLATFORM=$(uname -s | tr A-Z a-z)
DINEROD="${DINEROD:-./build/dinerod}"
DINERO_CLI="${DINERO_CLI:-./build/dinero-cli}"
DATADIR=$(mktemp -d)

# Cleanup function
cleanup() {
  echo "🧹 Cleaning up..."
  $DINERO_CLI -regtest -datadir=$DATADIR stop 2>/dev/null || true
  sleep 2
  rm -rf $DATADIR
}
trap cleanup EXIT

# Test counter
TESTS_RUN=0
TESTS_PASSED=0

# Test helper
run_test() {
  local test_name=$1
  local test_command=$2

  TESTS_RUN=$((TESTS_RUN + 1))
  echo ""
  echo "▶️  Test $TESTS_RUN: $test_name"

  if eval "$test_command"; then
    echo "   ✅ PASSED"
    TESTS_PASSED=$((TESTS_PASSED + 1))
    return 0
  else
    echo "   ❌ FAILED"
    return 1
  fi
}

# ═══════════════════════════════════════════════════════════════════════════
# TEST SUITE
# ═══════════════════════════════════════════════════════════════════════════

echo "════════════════════════════════════════════════════════════════"
echo "DineroCoin Smoke Tests - $PLATFORM"
echo "════════════════════════════════════════════════════════════════"
echo "Daemon:   $DINEROD"
echo "CLI:      $DINERO_CLI"
echo "Data dir: $DATADIR"
echo "════════════════════════════════════════════════════════════════"

# Test 1: Daemon binary exists
run_test "Daemon binary exists" "test -f $DINEROD"

# Test 2: CLI binary exists
run_test "CLI binary exists" "test -f $DINERO_CLI"

# Test 3: Daemon is executable
run_test "Daemon is executable" "test -x $DINEROD"

# Test 4: Start daemon
run_test "Start daemon in regtest mode" \
  "$DINEROD -regtest -daemon -datadir=$DATADIR -printtoconsole=0"

# Wait for startup
echo "⏳ Waiting for daemon startup..."
sleep 5

# Test 5: Daemon process running
run_test "Daemon process is running" \
  "pgrep -f 'dinerod.*regtest' > /dev/null"

# Test 6: RPC connectivity
run_test "RPC responds to ping" \
  "$DINERO_CLI -regtest -datadir=$DATADIR ping > /dev/null"

# Test 7: Get blockchain info
run_test "Get blockchain info" \
  "$DINERO_CLI -regtest -datadir=$DATADIR getblockchaininfo | grep -q '\"chain\"'"

# Test 8: Generate address
run_test "Generate new address" \
  "ADDRESS=\$($DINERO_CLI -regtest -datadir=$DATADIR getnewaddress) && [ ! -z \"\$ADDRESS\" ]"

# Test 9: Check initial balance (should be 0)
run_test "Check initial balance is 0" \
  "$DINERO_CLI -regtest -datadir=$DATADIR getbalance | grep -q '^0'"

# Test 10: Generate blocks
run_test "Generate 101 blocks (for coinbase maturity)" \
  "$DINERO_CLI -regtest -datadir=$DATADIR generatetoaddress 101 \$($DINERO_CLI -regtest -datadir=$DATADIR getnewaddress) | grep -q '\"' "

# Wait for block processing
sleep 2

# Test 11: Check balance after mining
run_test "Check balance is non-zero after mining" \
  "test \$($DINERO_CLI -regtest -datadir=$DATADIR getbalance | sed 's/\\..*//') -gt 0"

# Test 12: List unspent outputs
run_test "List unspent outputs (should have coinbase UTXOs)" \
  "$DINERO_CLI -regtest -datadir=$DATADIR listunspent | grep -q 'txid'"

# Test 13: Get block count
run_test "Get block count (should be 101)" \
  "test \$($DINERO_CLI -regtest -datadir=$DATADIR getblockcount) -eq 101"

# Test 14: Get best block hash
run_test "Get best block hash" \
  "$DINERO_CLI -regtest -datadir=$DATADIR getbestblockhash | grep -qE '^[0-9a-f]{64}$'"

# Test 15: Stop daemon
run_test "Stop daemon cleanly" \
  "$DINERO_CLI -regtest -datadir=$DATADIR stop"

# Wait for shutdown
sleep 3

# Test 16: Daemon process terminated
run_test "Daemon process terminated" \
  "! pgrep -f 'dinerod.*regtest' > /dev/null"

# ═══════════════════════════════════════════════════════════════════════════
# RESULTS
# ═══════════════════════════════════════════════════════════════════════════

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "SMOKE TEST RESULTS"
echo "════════════════════════════════════════════════════════════════"
echo "Tests run:    $TESTS_RUN"
echo "Tests passed: $TESTS_PASSED"
echo "Tests failed: $((TESTS_RUN - TESTS_PASSED))"
echo "════════════════════════════════════════════════════════════════"

if [ $TESTS_PASSED -eq $TESTS_RUN ]; then
  echo "✅ ALL SMOKE TESTS PASSED"
  exit 0
else
  echo "❌ SOME SMOKE TESTS FAILED"
  exit 1
fi
```

---

## Platform-Specific Tests

### macOS Smoke Tests

**Additional Checks:**
- Universal binary support (arm64/x86_64)
- No Homebrew library dependencies
- GUI bundle structure (if dinero-qt built)

```bash
# Test: Binary architecture
run_test "Verify binary architecture" \
  "file $DINEROD | grep -q 'arm64'"

# Test: No Homebrew dependencies
run_test "No Homebrew dependencies" \
  "! otool -L $DINEROD | grep -q '/opt/homebrew'"

# Test: GUI bundle structure (optional)
if [ -d "build/gui/dinero-qt.app" ]; then
  run_test "GUI bundle exists" \
    "test -f build/gui/dinero-qt.app/Contents/MacOS/dinero-qt"
fi
```

### Linux Smoke Tests

**Additional Checks:**
- glibc compatibility
- No gRPC/protobuf dynamic links
- Proper ELF binary format

```bash
# Test: ELF binary format
run_test "Verify ELF binary" \
  "file $DINEROD | grep -q 'ELF.*LSB.*executable'"

# Test: No gRPC dependencies
run_test "No gRPC dependencies" \
  "! ldd $DINEROD | grep -iq 'grpc'"

# Test: No protobuf dependencies
run_test "No protobuf dependencies" \
  "! ldd $DINEROD | grep -iq 'protobuf'"

# Test: No abseil dependencies
run_test "No abseil dependencies" \
  "! ldd $DINEROD | grep -iq 'absl'"
```

### Windows Smoke Tests

**Additional Checks:**
- PE executable format
- DLL dependencies bundled
- PowerShell execution

```powershell
# Test: Binary exists
Test-Path -Path "build\dinerod.exe" -PathType Leaf

# Test: Run daemon
Start-Process -FilePath "build\dinerod.exe" -ArgumentList "-regtest","-daemon"

# Wait for startup
Start-Sleep -Seconds 5

# Test: RPC connectivity
& "build\dinero-cli.exe" -regtest getblockchaininfo

# Test: Stop daemon
& "build\dinero-cli.exe" -regtest stop
```

---

## CI Integration

### GitHub Actions Step

```yaml
- name: Run smoke tests
  shell: bash
  run: |
    chmod +x scripts/smoke-test.sh
    ./scripts/smoke-test.sh
```

### Failure Handling

```yaml
- name: Upload smoke test logs (if failed)
  if: failure()
  uses: actions/upload-artifact@v4
  with:
    name: smoke-test-logs-${{ matrix.platform }}
    path: |
      /tmp/dinero-smoke-test-*/debug.log
      /tmp/dinero-smoke-test-*/regtest/debug.log
```

---

## Extended Smoke Tests (Optional for Beta)

### Lightning Smoke Test

```bash
# Test: Lightning daemon starts
run_test "Start Lightning daemon" \
  "./build/lightningd -regtest -daemon"

# Test: Lightning RPC
run_test "Lightning RPC responds" \
  "./build/lightning-cli -regtest getinfo | grep -q 'version'"

# Test: Lightning can connect to dinerod
run_test "Lightning connects to base layer" \
  "./build/lightning-cli -regtest getinfo | grep -q 'blockheight'"
```

### Performance Smoke Test

```bash
# Test: Block generation performance
run_test "Generate 100 blocks in <30s" \
  "timeout 30 $DINERO_CLI -regtest generatetoaddress 100 \$ADDRESS"

# Test: RPC response time
run_test "RPC responds in <1s" \
  "timeout 1 $DINERO_CLI -regtest getblockchaininfo"
```

### Memory Smoke Test

```bash
# Test: Memory usage stays below 500MB
run_test "Memory usage under 500MB" \
  "test \$(ps -o rss= -p \$(pgrep dinerod) | awk '{print int(\$1/1024)}') -lt 500"
```

---

## Smoke Test Matrix

| Test | macOS | Linux | Windows | Critical |
|------|-------|-------|---------|----------|
| Binary exists | ✅ | ✅ | ✅ | Yes |
| Binary executable | ✅ | ✅ | ✅ | Yes |
| Daemon starts | ✅ | ✅ | ✅ | Yes |
| RPC connectivity | ✅ | ✅ | ✅ | Yes |
| Get blockchain info | ✅ | ✅ | ✅ | Yes |
| Generate address | ✅ | ✅ | ✅ | Yes |
| Initial balance = 0 | ✅ | ✅ | ✅ | Yes |
| Generate blocks | ✅ | ✅ | ✅ | Yes |
| Balance > 0 after mining | ✅ | ✅ | ✅ | Yes |
| List UTXOs | ✅ | ✅ | ✅ | Yes |
| Block count = 101 | ✅ | ✅ | ✅ | Yes |
| Get best block hash | ✅ | ✅ | ✅ | Yes |
| Daemon stops cleanly | ✅ | ✅ | ✅ | Yes |
| Process terminated | ✅ | ✅ | ✅ | Yes |
| No Homebrew deps | ✅ | ❌ | ❌ | Yes (macOS) |
| No gRPC deps | ❌ | ✅ | ✅ | Yes (Linux/Win) |
| ELF binary | ❌ | ✅ | ❌ | No |
| PE binary | ❌ | ❌ | ✅ | No |
| GUI bundle | ✅ | ✅ | ❌ | No (optional) |

---

## Success Criteria

### Green Smoke Test (PASS)
- ✅ All 16 core tests pass
- ✅ Daemon starts in <5 seconds
- ✅ Daemon stops in <3 seconds
- ✅ No errors in debug.log
- ✅ Platform-specific checks pass

### Red Smoke Test (FAIL)
- ❌ Any core test fails
- ❌ Daemon crashes or hangs
- ❌ RPC timeouts
- ❌ Memory leaks detected
- ❌ Unexpected errors in debug.log

---

## Debugging Failed Smoke Tests

### Test 4 Fails: Daemon won't start

**Symptoms:**
```
❌ Test 4: Start daemon in regtest mode - FAILED
```

**Debug Steps:**
1. Check if port is already in use: `lsof -i :21001` (regtest default)
2. Check debug.log: `tail -50 /tmp/dinero-smoke-test-*/regtest/debug.log`
3. Try manual start: `./build/dinerod -regtest -printtoconsole`
4. Check file permissions: `ls -la ./build/dinerod`

**Common Causes:**
- Port conflict (another daemon running)
- Missing shared library
- Insufficient permissions
- Corrupted binary

### Test 6 Fails: RPC not responding

**Symptoms:**
```
❌ Test 6: RPC responds to ping - FAILED
```

**Debug Steps:**
1. Check if daemon is running: `ps aux | grep dinerod`
2. Check RPC port: `lsof -i :20996` (regtest RPC default)
3. Check .cookie file exists: `ls -la /tmp/dinero-smoke-test-*/.cookie`
4. Try manual RPC call: `./build/dinero-cli -regtest -datadir=... ping`

**Common Causes:**
- Daemon crashed during startup
- RPC not initialized yet (increase sleep time)
- Auth cookie not created
- Firewall blocking localhost

### Test 10 Fails: Block generation fails

**Symptoms:**
```
❌ Test 10: Generate 101 blocks - FAILED
```

**Debug Steps:**
1. Check miner is working: `./build/dinero-cli -regtest getmininginfo`
2. Check for errors: `./build/dinero-cli -regtest getinfo`
3. Try generating 1 block: `./build/dinero-cli -regtest generatetoaddress 1 <address>`

**Common Causes:**
- Invalid address format
- Consensus rules not satisfied
- Mining subsystem failure

---

## Maintenance

### When to Update Smoke Tests

- **New RPC commands:** Add test for new commands
- **New features:** Add smoke test for feature (e.g., Lightning, mobile mode)
- **Platform support:** Add platform-specific tests
- **Regressions found:** Add regression test to smoke suite

### Test Execution Frequency

- **Pre-commit:** Optional (developer discretion)
- **Pre-push:** Recommended (local smoke test)
- **CI (PR):** Required (all platforms)
- **CI (release tag):** Required (all platforms)
- **Nightly:** Full test suite (not just smoke tests)

---

## Appendix: Manual Smoke Test Procedure

**For developers without CI access:**

1. Build binaries:
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --parallel
   ```

2. Run smoke tests:
   ```bash
   chmod +x scripts/smoke-test.sh
   ./scripts/smoke-test.sh
   ```

3. Verify output:
   ```
   ✅ ALL SMOKE TESTS PASSED
   Tests run: 16
   Tests passed: 16
   Tests failed: 0
   ```

4. If failures occur:
   - Check debug.log in temp directory
   - Run failed test manually
   - Report issue with full output

---

## Conclusion

These smoke tests provide a **fast, automated safety net** to catch runtime issues before release. They complement the comprehensive test suite by verifying:

- ✅ **Binary integrity:** Files exist, are executable, have correct format
- ✅ **Basic functionality:** Daemon starts, RPC works, blocks generate
- ✅ **Platform compliance:** No unwanted dependencies, correct architecture
- ✅ **Clean shutdown:** No resource leaks, graceful termination

**Next Step:** Integrate smoke tests into CI pipeline.

---

**Document Date:** 2026-01-11
**Author:** Claude Code
**Status:** Ready for implementation
