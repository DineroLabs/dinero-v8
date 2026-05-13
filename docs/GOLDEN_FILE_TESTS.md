# Golden File Test System for DineroCoin CLI

## 🎯 Overview

The Golden File Test System ensures the stability of DineroCoin CLI's JSON output contract by comparing actual command outputs against frozen "golden" reference files. This prevents unintended breaking changes to the CLI's automation-friendly JSON interface.

## 🏗️ Architecture

### Test Components

1. **Golden File Test Suite** (`tests/cli/test_golden_files.py`)
   - Python-based test runner with daemon lifecycle management
   - Normalizes volatile fields (timestamps, memory usage, etc.)
   - Compares actual vs. expected JSON output structures

2. **Test Runner Script** (`scripts/run_golden_tests.sh`)
   - Bash wrapper with prerequisite checking
   - Supports update mode for refreshing golden files
   - Integrated error reporting and colored output

3. **Golden Reference Files** (`tests/cli/golden/*.json`)
   - Frozen JSON outputs for each tested CLI command
   - Version-controlled to track intentional changes
   - Normalized to remove volatile runtime data

### Tested Commands

| Command | Golden File | Purpose |
|---------|-------------|---------|
| `status` | `status.json` | Node health and basic info |
| `nodeinfo print` | `nodeinfo_print.json` | Configuration display |
| `wallet info` | `wallet_info.json` | Wallet metadata |
| `wallet balance` | `wallet_balance.json` | Balance information |
| `wallet addresses` | `wallet_addresses.json` | Address listing |
| `chain info` | `chain_info.json` | Blockchain status |
| `net info` | `net_info.json` | Network information |
| `mining info` | `mining_info.json` | Mining status |

## 🚀 Usage

### Running Tests

```bash
# Run all golden file tests
./scripts/run_golden_tests.sh

# Update golden files (use carefully!)
./scripts/run_golden_tests.sh --update

# Run via CMake/CTest
cd build-qt-free
ctest -R golden_file_tests -V
```

### Test Output

```
Running 8 golden file tests...
==================================================
✅ status: Output matches golden file
✅ nodeinfo_print: Output matches golden file
✅ wallet_info: Output matches golden file
✅ wallet_balance: Output matches golden file
✅ wallet_addresses: Output matches golden file
✅ chain_info: Output matches golden file
✅ net_info: Output matches golden file
✅ mining_info: Output matches golden file
==================================================
Results: 8/8 tests passed
```

### Failure Example

```
❌ wallet_balance: Output differs from golden file
Expected (golden):
{
  "data": {
    "confirmed": 0.0,
    "total": 0.0,
    "unconfirmed": 0.0
  },
  "output_version": "v1"
}
Actual (normalized):
{
  "data": {
    "available": 0.0,  // ← Field name changed!
    "confirmed": 0.0,
    "total": 0.0,
    "unconfirmed": 0.0
  },
  "output_version": "v1"
}
```

## 🔧 Configuration

### Normalization Rules

The test system automatically normalizes volatile fields:

```python
volatile_fields = [
    "timestamp",        # Always changes
    "cli_version",      # May change between versions
    "uptime",          # Runtime-dependent
    "current_time",    # Always changes
    "last_block_time", # Blockchain-dependent
    "connections",     # Network-dependent
    "peer_count",      # Network-dependent
    "memory_usage",    # System-dependent
    "disk_usage"       # System-dependent
]
```

### Address Normalization

Generated addresses are normalized to prevent test failures:
```python
# Normalize addresses (different each run)
if "address" in addr_info:
    addr_info["address"] = "rdin1_normalized_address"
```

## 🔄 CI Integration

### GitHub Actions Integration

Add to `.github/workflows/ci.yml`:

```yaml
- name: Run Golden File Tests
  run: |
    cd build-qt-free
    ctest -R golden_file_tests --output-on-failure
```

### Pre-commit Hook

```bash
#!/bin/bash
# .git/hooks/pre-commit
./scripts/run_golden_tests.sh || {
    echo "Golden file tests failed!"
    echo "If output format changes are intentional, run:"
    echo "  ./scripts/run_golden_tests.sh --update"
    exit 1
}
```

## 📋 Development Workflow

### Adding New Commands

1. **Add test method** to `GoldenFileTest` class:
```python
def test_new_command(self) -> bool:
    """Test new command output stability"""
    result = self.run_cli_command(["new", "command"])
    if not result:
        return False
    
    return self.compare_with_golden("new_command", result)
```

2. **Register test** in `run_all_tests()`:
```python
tests = [
    # ... existing tests ...
    ("new_command", self.test_new_command),
]
```

3. **Generate golden file**:
```bash
./scripts/run_golden_tests.sh --update
```

### Updating Output Format

When intentionally changing CLI output format:

1. **Update CLI implementation**
2. **Run tests to see failures**:
```bash
./scripts/run_golden_tests.sh
```

3. **Review changes carefully**
4. **Update golden files**:
```bash
./scripts/run_golden_tests.sh --update
```

5. **Commit both code and golden file changes**

### Debugging Test Failures

1. **Check normalization rules** - ensure volatile fields are properly excluded
2. **Review actual vs. expected output** - look for unintended changes
3. **Verify CLI command correctness** - test manually with same parameters
4. **Check test environment** - ensure clean daemon state

## 🔒 Security Considerations

### Sensitive Data Handling

Golden files should never contain:
- Real private keys or seeds
- Production cookie values
- Personal information
- Network-specific secrets

The test system uses:
- Isolated regtest environment
- Temporary test directories
- Normalized/redacted sensitive fields

### File Permissions

```bash
# Golden files should be readable
chmod 644 tests/cli/golden/*.json

# Test scripts should be executable
chmod +x scripts/run_golden_tests.sh
chmod +x tests/cli/test_golden_files.py
```

## 🐛 Troubleshooting

### Common Issues

**"Daemon not responding"**
```bash
# Check if daemon is already running
ps aux | grep dinerod
killall dinerod  # If needed

# Check port availability
lsof -i :20998
```

**"CLI binary not found"**
```bash
# Build CLI binary
cd build-qt-free
make dinero-cli-new
```

**"Python dependencies missing"**
```bash
# Install required packages
pip3 install json subprocess pathlib
```

**"Golden file differences"**
- Review changes carefully
- Check if changes are intentional
- Update golden files if format change is desired
- Investigate if unexpected regression

### Debug Mode

Enable verbose output:
```python
# In test_golden_files.py
def run_cli_command(self, args: list, check_error=True):
    cmd = [...] + ["--verbose"] + args  # Add --verbose
```

## 📊 Metrics and Monitoring

### Test Coverage

Current coverage:
- ✅ Core commands (status, nodeinfo)
- ✅ Wallet operations (info, balance, addresses)
- ✅ Blockchain queries (chain info)
- ✅ Network status (net info)
- ✅ Mining operations (mining info)

### Performance Benchmarks

Typical test execution:
- **Setup time:** ~5 seconds (daemon startup)
- **Test execution:** ~10 seconds (8 commands)
- **Cleanup time:** ~2 seconds
- **Total runtime:** ~17 seconds

## 🎯 Success Criteria

Golden file tests are successful when:
- ✅ All 8 core commands pass golden file comparison
- ✅ JSON output contract remains stable across versions
- ✅ Tests complete within 2 minutes
- ✅ No false positives from volatile field changes
- ✅ Clear failure reporting for actual regressions

## 🔮 Future Enhancements

### Planned Improvements

1. **Extended Command Coverage**
   - Transaction commands (`tx get`, `tx decode`)
   - Address validation (`addr validate`)
   - Raw RPC passthrough

2. **Advanced Normalization**
   - Configurable normalization rules
   - Command-specific field exclusions
   - Regex-based field matching

3. **Performance Optimization**
   - Parallel test execution
   - Daemon reuse across tests
   - Incremental golden file updates

4. **Enhanced Reporting**
   - HTML diff reports
   - JSON schema validation
   - Regression trend analysis

---

**The Golden File Test System provides bulletproof protection against unintended CLI output changes while enabling confident evolution of the JSON contract.**
