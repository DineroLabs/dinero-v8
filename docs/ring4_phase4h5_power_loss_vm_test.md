# Ring 4 Phase 4h.5 — Category B: Power-Loss Simulation

**Status**: Manual testing procedure
**Requires**: VM environment (QEMU/VMware/VirtualBox)
**Goal**: Validate filesystem + disk guarantees under power loss

---

## Purpose

Validate that MR1-MR5 properties hold even when:
- Power is lost mid-write
- Filesystem buffers are not flushed
- Disk writes are reordered
- WAL is incomplete

This tests the boundary between RocksDB and the OS/filesystem.

---

## Test Setup

### Option 1: VM Hard Power-Off (Recommended)

**Requirements**:
- QEMU, VMware, or VirtualBox
- Guest OS: Linux or macOS
- Snapshot capability enabled

**Steps**:
1. Install DineroCoin node in VM
2. Enable VM snapshot/restore
3. Start mining process
4. During persist operations, hard power-off VM
5. Restart VM
6. Run full Ring 4 persistence test suite

**Expected outcome**:
- Either old snapshot OR new snapshot loads
- Never partial state
- MR1-MR5 all pass unchanged

---

### Option 2: fsync Suppression (Fallback)

**Requirements**:
- Linux system with ext4/xfs
- Root access for mount options

**Method A: Mount with delayed allocation**
```bash
# Remount filesystem with aggressive write buffering
sudo mount -o remount,data=writeback,nobarrier /persistence/mount/point

# Run persistence tests
$BUILD_DIR/bin/test_mining_persistence_oracle_mr3

# Simulate crash
kill -9 <test_pid>

# Restore filesystem guarantees
sudo mount -o remount,data=ordered,barrier /persistence/mount/point
```

**Method B: LD_PRELOAD fsync stub**
```bash
# Create fsync stub library
cat > stub_fsync.c <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
int fsync(int fd) { return 0; }
int fdatasync(int fd) { return 0; }
int __fsync(int fd) { return 0; }
EOF

gcc -shared -fPIC stub_fsync.c -o libstub_fsync.so

# Run with fsync disabled
LD_PRELOAD=./libstub_fsync.so $BUILD_DIR/bin/test_mining_persistence_oracle_mr3

# Clean up
rm libstub_fsync.so stub_fsync.c
```

---

## Validation Criteria

After each power-loss simulation:

1. Run **MR1**: State Survives Restart
   ```bash
   $BUILD_DIR/bin/test_mining_persistence_oracle_mr1
   ```

2. Run **MR2**: No State Duplication
   ```bash
   $BUILD_DIR/bin/test_mining_persistence_oracle_mr2
   ```

3. Run **MR3**: Safe Recovery from Corruption
   ```bash
   $BUILD_DIR/bin/test_mining_persistence_oracle_mr3
   ```

4. Run **MR4**: Convergence to Valid State
   ```bash
   $BUILD_DIR/bin/test_mining_persistence_oracle_mr4
   ```

5. Run **MR5**: Determinism Preserved
   ```bash
   $BUILD_DIR/bin/test_mining_persistence_oracle_mr5
   ```

**All tests must pass unchanged.**

---

## Expected Outcomes

### Allowed Behaviors
- Old snapshot recovered (write didn't complete)
- New snapshot recovered (write completed atomically)
- Empty state (no snapshot exists yet)

### Forbidden Behaviors
- Partial snapshot exposed
- Corrupted state returned as valid
- Duplicate blocks/heights/subsidy
- Non-deterministic recovery

---

## Exit Criteria

Category B is complete when:
- ✅ MR1-MR5 pass after VM hard power-off
- ✅ MR1-MR5 pass after fsync suppression + kill-9
- ✅ No test modifications required
- ✅ Behavior matches abstract model (Phase 4g)

---

## Notes

- This test is **optional but strongly recommended** for production deployments
- Requires manual VM setup
- Cannot be fully automated in CI/CD
- Validates assumptions about OS/filesystem guarantees
- If tests pass, Ring 4 is **physically proven**, not just logically proven

---

## Status

**Phase 4h.5 Category B**: ⏳ PENDING MANUAL EXECUTION

See `ring4_phase4h5_os_crash_testing_report.md` for final results.
