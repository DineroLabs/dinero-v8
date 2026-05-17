# DineroCoin Release Process

**Document Version:** 1.0
**Status:** CANONICAL
**Date:** 2025-12-13
**Governs:** v0.10.0 and all subsequent releases

---

## Purpose

This document defines the **operational release process** for DineroCoin, covering how to create, test, tag, and distribute releases. It complements the [Consensus Versioning Policy](Consensus_Versioning_Policy.md) by defining the mechanics of releasing software.

**Read This If:**
- You're preparing a release (development, testnet, or mainnet)
- You're creating a git tag
- You're building binaries for distribution
- You're coordinating a network upgrade

---

## Release Phases

### Phase 1: Development Releases (`v0.x.x`)

**Target:** Testnet/Regtest only
**Frequency:** As needed (feature milestones)
**Testing:** Basic functional testing
**Distribution:** Source code only (no binaries)

**Checklist:**
- [ ] All tests pass (`make test`)
- [ ] Build succeeds on all platforms
- [ ] No banned global variables
- [ ] Commit messages follow convention
- [ ] Version bump documented in CHANGELOG.md
- [ ] Tag follows `v0.x.x` format
- [ ] Git tag pushed to repository

**Process:**
1. Complete feature/bugfix work on `dev` branch
2. Update version number in `src/version.h`
3. Update `CHANGELOG.md` with changes
4. Run full test suite
5. Create git tag: `git tag -a v0.x.x -m "Release v0.x.x - <description>"`
6. Push tag: `git push origin v0.x.x`
7. Continue development (no formal release notes needed)

---

### Phase 2: Release Candidates (`v0.x.x-rc1`)

**Target:** Public testnet
**Frequency:** Before major milestones (v0.15.0, v1.0.0)
**Testing:** Comprehensive (security, stress, multi-node)
**Distribution:** Source code + binaries

**Checklist:**
- [ ] All development release requirements met
- [ ] Security audit (external review for v1.0.0-rc1)
- [ ] Testnet deployment (minimum 7 days uptime)
- [ ] Multi-node testing (3+ independent nodes)
- [ ] Performance benchmarks documented
- [ ] Release notes drafted
- [ ] Known issues documented
- [ ] Upgrade guide prepared (if consensus changes)

**Process:**
1. Create release branch: `git checkout -b release/v0.x.x`
2. Update version: `#define CLIENT_VERSION_PRERELEASE "-rc1"`
3. Run extended test suite
4. Build binaries for all platforms (Linux, macOS, Windows)
5. Deploy to testnet
6. Monitor for 7+ days
7. If issues found: fix, increment RC number (`-rc2`), repeat
8. If stable: proceed to final release

**Binary Distribution:**
- Source tarball (`.tar.gz`)
- Linux binary (`.tar.gz` with AppImage or deb/rpm)
- macOS binary (`.dmg`)
- Windows binary (`.exe` installer)
- SHA256 checksums file
- GPG signatures

---

### Phase 3: Mainnet Releases (`v1.x.x`)

**Target:** Production mainnet
**Frequency:** Quarterly (or as needed for critical fixes)
**Testing:** Extensive (RC + 30-day testnet minimum)
**Distribution:** Full release package (source + binaries + docs)

**Checklist:**
- [ ] At least one successful RC deployment
- [ ] 30+ days testnet uptime (no consensus bugs)
- [ ] Security audit passed (for major versions)
- [ ] All tests pass (unit, integration, regression)
- [ ] Performance benchmarks meet targets
- [ ] Documentation complete (RPC docs, user guides)
- [ ] Release notes finalized
- [ ] Upgrade guide published (if needed)
- [ ] Exchange/pool coordination (for hard forks)
- [ ] Community announcement prepared

**Process:**
1. Final RC tested for 30+ days on testnet
2. Create release branch: `git checkout -b release/v1.x.x`
3. Update version: `#define CLIENT_VERSION_MAJOR 1`
4. Update `CHANGELOG.md` with full release notes
5. Build and sign binaries
6. Create git tag: `git tag -a v1.x.x -m "Release v1.x.x - <description>"`
7. Push tag and release branch
8. Publish release on GitHub (attach binaries)
9. Announce to community (website, social media, forums)
10. Monitor network for 48 hours post-release

**Binary Distribution (Same as RC + Additional):**
- Reproducible build instructions
- Gitian build signatures (for v1.0.0+)
- Upgrade guide PDF
- Release announcement blog post

---

## Version Number Management

### File: `src/version.h`

```cpp
#define CLIENT_VERSION_MAJOR 0
#define CLIENT_VERSION_MINOR 10
#define CLIENT_VERSION_REVISION 0
#define CLIENT_VERSION_PRERELEASE ""  // or "-rc1", "-beta", etc.

#define CLIENT_VERSION (1000000 * CLIENT_VERSION_MAJOR + \
                        10000 * CLIENT_VERSION_MINOR + \
                        100 * CLIENT_VERSION_REVISION)
```

**Rules:**
1. Bump `MAJOR` for hard forks (v2.0.0)
2. Bump `MINOR` for soft forks/features (v1.x.0)
3. Bump `REVISION` for bug fixes (v1.x.x)
4. Use `PRERELEASE` for `-rc1`, `-beta`, etc.
5. Update **before** creating the release tag

---

## Git Tag Format

### Standard Tag

```bash
git tag -a v0.10.0 -m "Release v0.10.0 - RPC Layer Complete

Changelog:
- Implemented getblock, getrawtransaction, getblockheader
- Wired getbestblockhash, getblockcount, getblockhash
- Fixed RPC authentication bug
- Performance improvements to ChainDB queries

See CHANGELOG.md for full details.
"
```

### Annotated Tag (Required)

All release tags **must** be annotated (use `-a` flag):
- Lightweight tags (`git tag v0.10.0`) are NOT allowed
- Annotated tags include tagger name, date, and message
- Enables proper version tracking via `git describe`

### Tag Naming

**Development (v0.x.x):**
- `v0.10.0` - RPC layer complete
- `v0.11.0` - Mempool hardening
- `v0.15.0-rc1` - Release candidate 1

**Mainnet (v1.x.x):**
- `v1.0.0` - Mainnet launch
- `v1.1.0` - Lightning Network
- `v1.0.1` - Critical bug fix

**Hard Fork (v2.x.x):**
- `v2.0.0` - Post-quantum upgrade
- `v2.1.0` - New features after hard fork

---

## Changelog Management

### File: `CHANGELOG.md`

Follow [Keep a Changelog](https://keepachangelog.com/en/1.0.0/) format:

```markdown
# Changelog

## [Unreleased]

### Added
- New RPC method `getblockstats`

### Changed
- Improved mempool fee estimation

### Fixed
- TX index rollback bug during reorgs

## [0.10.0] - 2025-12-20

### Added
- RPC methods: getblock, getrawtransaction, getblockheader
- RPC methods: getbestblockhash, getblockcount, getblockhash

### Changed
- ChainDB query performance improvements (20% faster)

### Fixed
- RPC authentication bypass vulnerability

## [0.9.0] - 2025-12-13 (Pre-Policy Era)

### Added
- Block indexing (Height/Hash/TXID)
- P2P sync (getheaders/getblocks)
- Utreexo integration (112-byte headers)

### Fixed
- Critical TX index rollback bug (reorg safety)
```

**Rules:**
1. Update `[Unreleased]` section continuously during development
2. On release, move `[Unreleased]` items to versioned section
3. Add release date in ISO format (YYYY-MM-DD)
4. Group changes: Added, Changed, Deprecated, Removed, Fixed, Security
5. Link version to git tag

---

## Testing Requirements

### Development Releases (v0.x.x)

**Minimum:**
- [ ] `make test` passes (all unit tests)
- [ ] Basic functional tests pass
- [ ] No compiler warnings
- [ ] No banned global variables

**Recommended:**
- [ ] Integration tests pass
- [ ] Regtest mining/validation works
- [ ] RPC methods respond correctly

### Release Candidates (v0.x.x-rc1)

**Required:**
- [ ] All development release tests pass
- [ ] Extended test suite passes (stress tests)
- [ ] Multi-node P2P sync tested
- [ ] Reorg safety validated (test_tx_index_reorg.sh)
- [ ] Mempool stress test (1000+ transactions)
- [ ] RPC fuzzing (malformed inputs)
- [ ] Memory leak testing (valgrind)
- [ ] 7+ days uptime on testnet

### Mainnet Releases (v1.x.x)

**Required:**
- [ ] All RC tests pass
- [ ] 30+ days testnet deployment
- [ ] Security audit (external review)
- [ ] Performance benchmarks documented
- [ ] Backward compatibility verified (v1.x-1 → v1.x)
- [ ] Database migration tested (if schema changed)
- [ ] Upgrade/downgrade path validated
- [ ] Edge case testing (orphan blocks, deep reorgs)

---

## Build Process

### Source Distribution

```bash
# Clean build
make clean
git checkout v0.10.0
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run tests
make test

# Package source
cd ..
git archive --format=tar.gz --prefix=dinerocoin-0.10.0/ v0.10.0 > dinerocoin-0.10.0.tar.gz
```

### Binary Distribution (Linux)

```bash
# Static build (portable)
mkdir build-release && cd build-release
cmake -DCMAKE_BUILD_TYPE=Release \
      -DSTATIC_LINK=ON \
      -DCMAKE_INSTALL_PREFIX=/usr ..
make -j$(nproc)
make install DESTDIR=dinerocoin-0.10.0-linux-x86_64

# Package
tar czf dinerocoin-0.10.0-linux-x86_64.tar.gz dinerocoin-0.10.0-linux-x86_64

# Generate checksums
sha256sum dinerocoin-0.10.0-linux-x86_64.tar.gz > SHA256SUMS
```

### Binary Distribution (macOS)

```bash
# Universal binary (x86_64 + arm64)
mkdir build-release && cd build-release
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" ..
make -j$(nproc)

# Create .dmg
# (Use create-dmg tool or manual .app bundle)
```

### Binary Distribution (Windows)

```bash
# Cross-compile from Linux (MinGW)
mkdir build-windows && cd build-windows
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake ..
make -j$(nproc)

# Create installer (NSIS)
makensis dinerocoin.nsi
```

---

## Release Announcement Template

### Title

```
DineroCoin Core v0.10.0 Released - RPC Layer Complete
```

### Body

```markdown
# DineroCoin Core v0.10.0

We're pleased to announce the release of DineroCoin Core v0.10.0, the first release
fully governed by our [Consensus Versioning Policy](https://github.com/dinerocoin/dinerocoin/blob/dev/Consensus_Versioning_Policy.md).

## What's New

### RPC Layer Complete
- `getblock` - Retrieve block data by hash
- `getrawtransaction` - Lookup transactions by TXID
- `getblockheader` - Get block headers
- `getbestblockhash` - Query current chain tip
- `getblockcount` - Get current block height
- `getblockhash` - Lookup block hash by height

### Performance Improvements
- 20% faster ChainDB queries
- Optimized block indexing

### Bug Fixes
- Fixed RPC authentication vulnerability
- Improved error handling in P2P sync

## Upgrading

Download binaries from the [releases page](https://github.com/dinerocoin/dinerocoin/releases/tag/v0.10.0).

No database migration required. Simply replace binaries and restart.

## Checksums

See [SHA256SUMS](https://github.com/dinerocoin/dinerocoin/releases/download/v0.10.0/SHA256SUMS)
for binary verification.

## Full Changelog

See [CHANGELOG.md](https://github.com/dinerocoin/dinerocoin/blob/v0.10.0/CHANGELOG.md).

---

**Status:** Development Release (Testnet Only)
**Next Milestone:** v0.11.0 - Mempool Hardening
```

---

## Emergency Release Process (Critical Fixes)

### Severity Levels

**Critical (Immediate Release):**
- Consensus bug (chain split risk)
- Remote code execution vulnerability
- Network-wide DoS vector

**High (Release within 48 hours):**
- Memory corruption bug
- Wallet fund loss risk
- P2P network partition

**Medium (Release within 1 week):**
- Performance degradation
- Non-critical RPC bugs
- Minor security issues

### Process for Critical Fixes

1. **Immediate Response (Hour 0):**
   - Identify and validate the bug
   - Assess severity and impact
   - Create private patch (do NOT commit to public repo yet)

2. **Coordinated Disclosure (Hour 1-4):**
   - Notify key stakeholders (exchanges, pools, large node operators)
   - Prepare security advisory
   - Test fix on regtest/testnet

3. **Release (Hour 4-24):**
   - Bump PATCH version (v1.0.1)
   - Commit fix to repository
   - Create emergency release tag
   - Build and distribute binaries
   - Publish security advisory

4. **Post-Release (Hour 24-72):**
   - Monitor network adoption
   - Coordinate with slow upgraders
   - Publish detailed postmortem (after 90% adoption)

**Example Tag:**
```bash
git tag -a v1.0.1 -m "SECURITY RELEASE - Critical Consensus Bug Fix

DO NOT DELAY UPGRADE.

This release fixes a critical consensus bug that could cause chain splits.
All nodes must upgrade immediately.

CVE-2025-XXXXX: Consensus bug in block validation

See security advisory for details.
"
```

---

## Post-Release Checklist

- [ ] GitHub release created with binaries attached
- [ ] Release announcement published (website, Twitter, Reddit)
- [ ] Documentation updated (RPC docs, user guides)
- [ ] `dev` branch synced with release tag
- [ ] Next milestone issue created on GitHub
- [ ] Monitoring dashboards updated (if mainnet)
- [ ] Exchange/pool partners notified (if consensus change)
- [ ] Archive old binaries (keep last 3 versions)

---

## Maintenance Windows

**Development Releases (v0.x.x):**
- No formal maintenance window
- Releases as features complete

**Mainnet Releases (v1.x.x):**
- **MINOR releases:** Quarterly (Q1, Q2, Q3, Q4)
- **PATCH releases:** As needed (critical fixes)
- **MAJOR releases:** Rare (hard forks, 180-day notice minimum)

**Coordinated Upgrade Schedule:**
- **Announcement:** T-90 days (for soft/hard forks)
- **Release Candidate:** T-60 days
- **Final Release:** T-30 days
- **Activation Height:** T-0 (for hard forks)

---

## Appendix A: Quick Checklist

### I'm Ready to Release v0.x.x (Development)

1. [ ] Feature complete
2. [ ] Tests pass
3. [ ] Update `src/version.h`
4. [ ] Update `CHANGELOG.md`
5. [ ] Create annotated tag: `git tag -a v0.x.x -m "..."`
6. [ ] Push tag: `git push origin v0.x.x`
7. [ ] Done!

### I'm Ready to Release v0.x.x-rc1 (Release Candidate)

1. [ ] All development checklist items
2. [ ] Create release branch
3. [ ] Build binaries (Linux, macOS, Windows)
4. [ ] Deploy to testnet (7+ days)
5. [ ] Monitor for issues
6. [ ] Create GitHub release with binaries
7. [ ] If stable → proceed to final release

### I'm Ready to Release v1.x.x (Mainnet)

1. [ ] All RC checklist items
2. [ ] 30+ days testnet uptime
3. [ ] Security audit passed
4. [ ] Update documentation
5. [ ] Build and sign binaries
6. [ ] Create annotated tag
7. [ ] Publish GitHub release
8. [ ] Announce to community
9. [ ] Monitor network for 48 hours

---

## Appendix B: Tools

**Version Bump Script:**
```bash
#!/bin/bash
# scripts/bump-version.sh
MAJOR=$1
MINOR=$2
REVISION=$3

sed -i "s/CLIENT_VERSION_MAJOR.*/CLIENT_VERSION_MAJOR $MAJOR/" src/version.h
sed -i "s/CLIENT_VERSION_MINOR.*/CLIENT_VERSION_MINOR $MINOR/" src/version.h
sed -i "s/CLIENT_VERSION_REVISION.*/CLIENT_VERSION_REVISION $REVISION/" src/version.h
```

**Checksum Generator:**
```bash
#!/bin/bash
# scripts/generate-checksums.sh
sha256sum dinerocoin-*.tar.gz dinerocoin-*.dmg dinerocoin-*.exe > SHA256SUMS
gpg --clearsign SHA256SUMS
```

---

## Document Maintenance

**Authority:** This document is CANONICAL and changes require:
1. GitHub issue discussing proposed change
2. Pull request with rationale
3. Approval from release manager

**Version History:**
- v1.0 (2025-12-13): Initial release process established

**Next Review:** Before v1.0.0 release

---

**Status:** ACTIVE (Governs v0.10.0+)
