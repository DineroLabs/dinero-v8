# CI Workflow Testing Instructions - Day 2

**Date:** 2026-01-11
**Purpose:** Manual testing of v3-alpha-release.yml workflow
**Status:** Ready for testing

---

## Overview

The v3-alpha-release.yml workflow has been pushed to origin/main. Before tagging v3.0.0-alpha1, we should test the workflow using the `workflow_dispatch` trigger to verify all platforms build correctly.

---

## Testing Steps

### Step 1: Navigate to GitHub Actions

1. Go to: https://github.com/Trucker2827/Dinero-Coin/actions
2. Find the workflow: **"v3.0.0-alpha Release Build"**
3. Click on the workflow name

### Step 2: Trigger Manual Run

1. Click the **"Run workflow"** button (top right)
2. You'll see a dropdown with:
   - **Branch:** main (should be selected)
   - **Version to build:** Enter `v3.0.0-alpha1-test`
3. Click **"Run workflow"** (green button)

### Step 3: Monitor Build Progress

The workflow will start 5 parallel jobs:
- ✅ Build macOS-arm64 (Apple Silicon)
- ✅ Build macOS-x86_64 (Intel)
- ✅ Build linux-x86_64
- ✅ Build windows-x86_64
- ⏳ Create GitHub Release (waits for all builds)

**Expected Duration:**
- macOS builds: 8-12 minutes
- Linux build: 10-15 minutes
- Windows build: 15-20 minutes
- Total: ~20 minutes

### Step 4: Check Build Stages

Each build job has these stages (click job to see details):

1. **Environment Setup** (1-2 min)
   - Install dependencies (cmake, openssl, boost, qt6, etc.)

2. **Checkout & Submodules** (1 min)
   - Clone repo with full history
   - Initialize submodules (RocksDB, secp256k1-zkp)

3. **CMake Configuration** (30 sec)
   - Verify DINERO_RELEASE=ON
   - Verify ENABLE_TESTS=ON
   - Set SOURCE_DATE_EPOCH for reproducibility

4. **Build Binaries** (5-8 min)
   - Build dinerod, dinero-cli
   - Build test executables

5. **Run Tests** (2-3 min)
   - Ring tests (65 tests, MUST PASS)
   - Consensus tests (MUST PASS)
   - Mobile tests (MAY FAIL - acceptable)
   - Lightning tests (MAY FAIL - acceptable)

6. **Smoke Tests** (2 min)
   - 16 functional tests
   - Daemon startup/shutdown
   - RPC connectivity
   - Block generation
   - Wallet operations

7. **Binary Verification** (30 sec)
   - macOS: Check for Homebrew dependencies
   - Linux: Check for gRPC/protobuf/abseil
   - Windows: Verify binary exists

8. **Package Artifacts** (1 min)
   - Create tar.gz (macOS/Linux) or zip (Windows)
   - Generate SHA256 checksums

9. **Upload Artifacts** (30 sec)
   - Upload to GitHub Actions artifacts
   - Retention: 90 days

### Step 5: Check for Failures

#### Expected Test Results

**MUST PASS (P0 - Blockers):**
- ✅ Ring tests (65/65)
- ✅ Consensus tests (all)
- ✅ Smoke tests (16/16)
- ✅ Binary verification (no unwanted deps)

**MAY FAIL (P1 - Acceptable for alpha):**
- ⚠️ Mobile tests (platform-specific, not critical)
- ⚠️ Lightning tests (incomplete features)

#### If Build Fails

1. **Click on the failed job** to see detailed logs
2. **Look for error messages** in the failing stage
3. **Common issues:**
   - Missing dependencies → Check environment setup stage
   - CMake errors → Check configuration flags
   - Test failures → Check test output (`--output-on-failure`)
   - Link errors → Check for missing libraries
   - Smoke test failures → Check daemon startup logs

4. **Fix and retry:**
   - Make fixes locally
   - Commit changes
   - Push to origin/main
   - Re-run workflow_dispatch

### Step 6: Download Test Artifacts

If all jobs succeed:

1. Go to the workflow run page
2. Scroll to **"Artifacts"** section at the bottom
3. Download artifacts for each platform:
   - `dinero-macos-arm64-v3.0.0-alpha1-test`
   - `dinero-macos-x86_64-v3.0.0-alpha1-test`
   - `dinero-linux-x86_64-v3.0.0-alpha1-test`
   - `dinero-windows-x86_64-v3.0.0-alpha1-test`

4. Verify artifact contents:
   ```bash
   # macOS/Linux
   tar -tzf DineroCoin-v3.0.0-alpha1-test-macos-arm64.tar.gz

   # Should contain:
   # DineroCoin-v3.0.0-alpha1-test-macos-arm64/bin/dinerod
   # DineroCoin-v3.0.0-alpha1-test-macos-arm64/bin/dinero-cli
   # DineroCoin-v3.0.0-alpha1-test-macos-arm64/README.md
   # DineroCoin-v3.0.0-alpha1-test-macos-arm64/CHANGELOG.md
   # DineroCoin-v3.0.0-alpha1-test-macos-arm64/SECURITY.md
   ```

5. Test one artifact locally:
   ```bash
   # Extract
   tar -xzf DineroCoin-v3.0.0-alpha1-test-macos-arm64.tar.gz
   cd DineroCoin-v3.0.0-alpha1-test-macos-arm64

   # Run daemon
   ./bin/dinerod -regtest -daemon

   # Test RPC
   ./bin/dinero-cli -regtest getblockchaininfo

   # Stop daemon
   ./bin/dinero-cli -regtest stop
   ```

### Step 7: Verify Release Job (Should NOT Run)

**IMPORTANT:** Since this is a workflow_dispatch test (not a tag push), the "Create GitHub Release" job should be **SKIPPED**.

- Check that the release job shows: ✓ **Skipped**
- This is correct behavior (only runs on tag push)

---

## Success Criteria

✅ **ALL of these must be true:**

1. ✅ All 4 platform builds complete successfully
2. ✅ Ring tests pass (65/65) on all platforms
3. ✅ Consensus tests pass on all platforms
4. ✅ Smoke tests pass (16/16) on all platforms
5. ✅ Binary verification passes (no Homebrew/gRPC deps)
6. ✅ Artifacts uploaded for all platforms
7. ✅ Artifacts contain dinerod, dinero-cli, docs
8. ✅ SHA256 checksums generated
9. ✅ Release job skipped (correct for workflow_dispatch)
10. ✅ No critical errors in any stage

**If all criteria met:** ✅ **READY FOR DAY 3** (tag v3.0.0-alpha1)

---

## Failure Scenarios & Fixes

### Scenario 1: CMake Configuration Fails

**Error:** `CMake Error: Could not find CMAKE_CXX_COMPILER`

**Fix:**
- Environment setup stage failed to install build tools
- Add missing dependencies to workflow

### Scenario 2: Build Fails with Link Errors

**Error:** `undefined reference to 'grpc::...'`

**Fix:**
- DINERO_RELEASE=ON not working correctly
- Check CMakeLists.txt for static linking logic

### Scenario 3: Ring Tests Fail

**Error:** `Ring test X failed`

**Fix:**
- **BLOCKER** - Do not proceed to Day 3
- Investigate test failure locally
- Fix consensus issue
- Re-test workflow

### Scenario 4: Smoke Tests Fail

**Error:** `Daemon failed to start`

**Fix:**
- Check daemon startup logs in workflow
- Missing runtime dependencies?
- Port conflict? (should use temp datadir)

### Scenario 5: Binary Verification Fails (macOS)

**Error:** `Homebrew dependencies found`

**Fix:**
- Static linking not working
- Check CMake flags: `-DDINERO_RELEASE=ON`
- Check otool output in workflow logs

### Scenario 6: Binary Verification Fails (Linux)

**Error:** `gRPC dependencies found`

**Fix:**
- Linking against system gRPC instead of vendored
- Check CMake vendored dependencies logic

### Scenario 7: Windows Build Timeout

**Error:** `Build timed out after 6 hours`

**Fix:**
- Parallel build not working: check `$env:NUMBER_OF_PROCESSORS`
- Reduce build parallelism
- Split build into multiple jobs

---

## Platform-Specific Notes

### macOS

**Expected Dependencies (otool -L):**
- `/usr/lib/libSystem.B.dylib` ✅
- `/usr/lib/libc++.1.dylib` ✅
- `/usr/lib/libz.1.dylib` ✅
- `/System/Library/Frameworks/...` ✅

**NOT Expected:**
- `/opt/homebrew/*` ❌
- `@rpath/libgrpc*` ❌
- `@rpath/libprotobuf*` ❌

### Linux

**Expected Dependencies (ldd):**
- `linux-vdso.so.1` ✅
- `libc.so.6` ✅
- `libpthread.so.0` ✅
- `libdl.so.2` ✅
- `libm.so.6` ✅
- `ld-linux-x86-64.so.2` ✅

**NOT Expected:**
- `libgrpc*` ❌
- `libprotobuf*` ❌
- `libabsl*` ❌

### Windows

**Expected:**
- `dinerod.exe` exists in `build/Release/`
- Binary runs without missing DLL errors

---

## Timeline

**Day 2 Schedule:**

- ✅ **10:00 AM** - Push workflow to origin/main (DONE)
- ✅ **10:30 AM** - Verify workflow syntax (DONE)
- ⏳ **11:00 AM** - Trigger workflow_dispatch test
- ⏳ **11:20 AM** - Monitor build progress (~20 min)
- ⏳ **11:40 AM** - Review results, download artifacts
- ⏳ **12:00 PM** - Fix any issues (if needed)
- ⏳ **2:00 PM** - Final workflow_dispatch test (if fixes made)
- ⏳ **3:00 PM** - Review Go/No-Go checklist
- ✅ **4:00 PM** - Day 2 complete, ready for Day 3

---

## Next Steps (Day 3)

**If workflow_dispatch test succeeds:**

1. Review final Go/No-Go checklist
2. Tag v3.0.0-alpha1 on main
3. Push tag to trigger automatic release build
4. Monitor GitHub Actions
5. Verify GitHub release created (pre-release)
6. Test release binaries
7. Announce alpha release

---

## Manual Override (If Workflow Fails)

**If CI workflow has issues that cannot be fixed quickly:**

### Option 1: Build Locally
- Build on each platform manually
- Create artifacts manually
- Upload to GitHub release manually
- Document manual build in release notes

### Option 2: Delay Release
- Fix workflow issues
- Re-test with workflow_dispatch
- Push to Day 3 when ready

### Option 3: Partial Release
- Release only working platforms
- Mark failed platforms as "coming soon"
- Not recommended for alpha

---

## Contact

**If stuck:**
- Review workflow logs in detail
- Check CMakeLists.txt for platform-specific logic
- Review GitHub Actions documentation
- Ask for help in GitHub Discussions

**Critical blockers:**
- Ring test failures → Do not proceed
- Build failures on all platforms → Fix before Day 3
- Binary verification failures → Investigate static linking

---

**Document Status:** Ready for Day 2 testing
**Last Updated:** 2026-01-11
**Next Action:** Trigger workflow_dispatch at GitHub Actions UI
