# Dinero v1.0.0 – Economics Lock-In & Tooling Hardening

## Consensus & Economics
- **Block time locked**: 520 seconds (8.7 min)
- **Retarget**: 60 blocks (~8.7h)
- **Premine**: 7,000,000 DIN
- **CPU-friendly phase**: 7M→25M supply at 99 DIN/block (≈3.0 years)
- **Halving schedule**: 99 → 66 → 33 → 16 → 8 → 4 → 2 → 1 (tail emission)
- **Supply (pre-tail)**: 180.5M DIN

## Daemon
- Startup banner prints locked-in economics and correct HRP per network
- Clear separation of ports; stable log message for HTTP JSON-RPC detection
- Improved help text distinguishing core RPC vs HTTP JSON-RPC ports

## CLI
- Deterministic connection discovery (flags → nodeinfo → defaults)
- Tolerant nodeinfo.json parsing with deprecation warnings
- `--no-nodeinfo` behavior documented; robust error messages
- Network Info section in help showing locked economics

## Tests
- Reward schedule, block-time economics, smoke tests, and HRP checks all passing
- CI recommended: `ctest -R "(block_time_economics|reward_schedule|smoke_daemon|cli_integration)"`

## Upgrade Notes
- **Regtest/testnet users**: Remove old datadirs after upgrading due to consensus parameter changes
- **Tools parsing ports**: Should rely on the daemon's "Separate ports mode" log line or an explicit HTTP port flag when available
- **Port semantics**: Daemon uses separate ports for core RPC vs HTTP JSON-RPC

## Security & Stability
- Multiple regression prevention mechanisms prevent accidental parameter changes
- Comprehensive test suite validates all economics calculations
- Clear error messages and documentation for troubleshooting

## Files Changed
- `src/consensus/chainparams_*.cpp` - Block time constants locked to 520s
- `include/consensus/dinero_algorithm.h` - Economics constants updated
- `src/daemon/main.cpp` - Runtime banner and HRP consistency
- `src/cli/main_new.cpp` - Connection discovery and help text
- `scripts/smoke_daemon.sh` - Robust port parsing and flag handling
- `tests/test_*_economics.cpp` - Comprehensive validation tests

## Breaking Changes
- Block time changed from 180s to 520s (affects all networks)
- Premine increased from 2M to 7M DIN
- CPU-friendly target increased from 20M to 25M DIN
- Total supply increased from 99M to 180.5M DIN

## Migration Guide
1. **Stop daemon**: `dinero-cli stop`
2. **Backup data**: Copy your datadir to a safe location
3. **Remove old datadirs**: Delete regtest/testnet datadirs (mainnet can be preserved)
4. **Start daemon**: Launch with new version
5. **Verify**: Check startup banner shows correct economics
