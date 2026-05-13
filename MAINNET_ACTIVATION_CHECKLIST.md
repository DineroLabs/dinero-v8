# 🚀 DineroCoin Mainnet Activation Checklist

## Mission Critical Deployment Guide

This checklist follows industry standards used by Bitcoin Core, Zcash, Grin, Monero, and Avalanche before mainnet launch.

**WARNING**: Once consensus is frozen and mainnet launches, **NO CHANGES** to consensus code are allowed without a hard fork.

---

## 🟢 PACKAGE 1: MAINNET ACTIVATION (Mission Critical)

### 1. Consensus Freeze ❌ NOT YET FROZEN

**Status**: ⚠️ Confidential transactions complete, but consensus not frozen

**Required Actions**:

- [ ] **Freeze `src/consensus/` directory**
  - Lock all consensus validation rules
  - Document exact version (git hash)
  - Create `CONSENSUS_FROZEN.md` with timestamp

- [ ] **Freeze block header fields**
  - Version: Finalized?
  - Previous block hash: Standard 32-byte
  - Merkle root: Standard 32-byte
  - Timestamp: Unix timestamp
  - Difficulty bits: Finalized?
  - Nonce: 32-bit

- [ ] **Freeze transaction formats**
  - Version 2 (confidential-enabled)
  - Input format: Confirmed
  - Output format: Confirmed
  - Confidential output metadata: Confirmed
  - ScriptPubKey format: `OP_0 <32-byte commitment hash>`

- [ ] **Freeze Pedersen + Bulletproofs integration**
  - Commitment equation: `sum(inputs) = sum(outputs) + fee`
  - Bulletproof verification parameters
  - secp256k1-zkp curve parameters
  - Generator points (G and H)

- [ ] **Freeze sighash algorithm**
  - BIP143-style (confirmed in code)
  - Hash construction documented
  - Test vectors created

- [ ] **Freeze address formats**
  - Transparent: Bech32 `din1q...`
  - Confidential: Base58 `C...` (version 0x42)
  - No changes allowed after freeze

- [ ] **Freeze mempool rules**
  - Min relay fee
  - Max transaction size
  - Standardness rules for confidential outputs
  - Dust threshold (1000 una)

**Freeze Date**: ⚠️ TBD

---

### 2. Genesis Parameters Finalization ❌ NOT FINALIZED

**Status**: ⚠️ Needs mainnet genesis block creation

**Required Actions**:

- [ ] **Final genesis hash**
  - Generate mainnet genesis block
  - Include timestamp, nonce, difficulty
  - Document in `chainparams.cpp`

- [ ] **Final premine block hash**
  - If using premine: Document exact outputs
  - If no premine: Set to genesis

- [ ] **Final premine outputs**
  - Transparent outputs: List addresses + amounts
  - Confidential outputs: List commitments + proofs
  - Total supply cap: 21M DIN (like Bitcoin)

- [ ] **Block reward schedule (fixed)**
  ```
  Initial reward: TBD DIN
  Halving interval: TBD blocks
  Final halving: Block TBD
  ```

- [ ] **Target spacing (fixed)**
  - Block time: TBD seconds (e.g., 120s like Grin, 600s like Bitcoin)

- [ ] **Difficulty anchor (fixed)**
  - Initial difficulty: TBD
  - Retarget algorithm: TBD

- [ ] **Network magic bytes**
  - Mainnet: `0xD1 0x4E 0x45 0x52` ("DINR")
  - Testnet: `0x54 0x44 0x4E 0x52` ("TDIN")
  - Regtest: `0x52 0x44 0x4E 0x52` ("RDIN")

**Finalization Date**: ⚠️ TBD

---

### 3. Peer Network Hardening ⚠️ PARTIALLY COMPLETE

**Status**: Basic P2P exists, needs production hardening

**Required Actions**:

- [ ] **Peer scoring**
  - Track peer reliability
  - Prefer high-quality peers
  - Implementation: `net.cpp`

- [ ] **Ban stale peers**
  - Timeout inactive connections
  - Ban misbehaving nodes
  - Implementation: `net_processing.cpp`

- [ ] **Misbehavior penalty**
  - Point system for bad behavior
  - Automatic banning at threshold
  - Log misbehavior events

- [ ] **Duplicate connection filter**
  - Prevent connecting to same peer twice
  - Check by IP + port
  - Implementation: `addrman.cpp`

- [ ] **Version negotiation strictness**
  - Enforce minimum protocol version
  - Reject incompatible versions
  - Implementation: `version.cpp`

- [ ] **DNS seeds**
  - Setup DNS seeds for mainnet
  - Recommended: 3-5 independent DNS seeds
  - Format: `seed.dinero-coin.com`

- [ ] **Static seed nodes**
  - Hardcode 5-10 reliable IPs
  - Geographic distribution
  - Implementation: `chainparams.cpp`

- [ ] **NAT traversal testing**
  - Test UPnP port forwarding
  - Test NAT-PMP
  - Test manual port forwarding

- [ ] **UPnP + IPv6 testing**
  - Enable UPnP by default
  - Support IPv6 connections
  - Dual-stack support

**Target Date**: Before mainnet launch

---

### 4. Wallet Hardening ✅ MOSTLY COMPLETE

**Status**: ✅ Core wallet features complete, minor cleanup needed

**Required Actions**:

- [x] **Stop using dummy test keys** - ✅ DONE (real keys implemented)

- [ ] **Wipe any placeholder constants**
  - Search for: `0x02`, `0x03`, dummy keys
  - Replace with proper error handling
  - Files to check:
    - `confidential_tx_builder.cpp` (lines 522-524)
    - Any test files with hardcoded keys

- [x] **Confirm view key & spend key separation** - ✅ DONE
  - View key path: `m/77'/1447'/144777'/account'/view'`
  - Spend key path: `m/84'/1447'/0'/0/index`
  - Completely isolated ✅

- [x] **Confirm change address usage** - ✅ DONE
  - Change outputs properly encrypted
  - Using wallet's own view key

- [x] **BIP32 path fixed** - ✅ DONE
  - Path: `m/77'/1447'/144777'/account'/view'`
  - Documented in `hd_wallet.cpp:226-231`

**Remaining Work**: Remove placeholder keys from error paths

---

### 5. Production Build Verification ❌ NOT STARTED

**Status**: ⚠️ Development builds only

**Required Actions**:

- [ ] **Reproducible builds (deterministic)**
  - Use Gitian or Docker-based build system
  - Same source → same binary hash
  - Documentation: `doc/gitian-building.md`

- [ ] **Build with sanitizers**
  - AddressSanitizer (ASan)
  - UndefinedBehaviorSanitizer (UBSan)
  - ThreadSanitizer (TSan)
  - MemorySanitizer (MSan)

- [ ] **Build on multiple platforms**
  - [x] macOS (Darwin 24.6.0 - current dev)
  - [ ] Linux x86_64 (Ubuntu 22.04, Debian 12)
  - [ ] Linux ARM64 (Raspberry Pi, M1 native)
  - [ ] Windows (MSVC + MinGW)

- [ ] **Verify signatures**
  - Code signing certificates
  - GPG signatures on releases
  - Notarization (macOS)

- [ ] **Verify static linking to Rust libs**
  - Bulletproofs FFI statically linked
  - No runtime Rust dependencies
  - Check with: `ldd` (Linux), `otool -L` (macOS)

**Target Date**: 2 weeks before mainnet

---

### 6. Security Audit Round ⚠️ INTERNAL ONLY

**Status**: ⚠️ Self-audited, needs external audit

**Required Actions**:

- [ ] **Review Bulletproof FFI boundary**
  - Buffer overflow checks
  - NULL pointer checks
  - Return value validation
  - Files: `bulletproofs.h`, `lib.rs`

- [ ] **Review mempool logic**
  - Double-spend prevention
  - Fee validation
  - Commitment duplicate detection
  - Files: `validation_mempool.cpp`

- [ ] **Review transaction decision tree**
  - Input validation
  - Output validation
  - Signature verification order
  - Files: `validation_confidential.cpp`

- [ ] **Review signature validation**
  - Sighash computation
  - ECDSA verification
  - Malleability protection
  - Files: `confidential_tx_signer.cpp`

- [ ] **Review nonce encryption**
  - ECDH implementation
  - Shared secret handling
  - Nonce XOR masking
  - Files: `confidential_tx_builder.cpp:174-198`

- [ ] **Review view key scanning edge cases**
  - Null commitment handling
  - Invalid nonce size
  - Decryption failures
  - Files: `view_key_scanner.cpp`

**Recommended**: Hire external audit firm (e.g., Trail of Bits, NCC Group)

---

### 7. Infrastructure ❌ NOT DEPLOYED

**Status**: ⚠️ Development infrastructure only

**Required Actions**:

- [ ] **Mainnet explorers**
  - Block explorer (like blockchain.info)
  - Transaction lookup
  - Address balance
  - Confidential output display
  - Suggested: Fork insight, blockbook, or esplora

- [ ] **Mainnet seed nodes**
  - Deploy 5-10 seed nodes globally
  - AWS, GCP, or bare metal
  - Geographic distribution:
    - North America (2 nodes)
    - Europe (2 nodes)
    - Asia (2 nodes)
    - Australia (1 node)

- [ ] **RPC load-balanced gateway**
  - HAProxy or nginx
  - Round-robin to multiple nodes
  - Rate limiting
  - DDoS protection

- [ ] **Faucet (optional)**
  - For testnet only
  - Drip small amounts for testing
  - reCAPTCHA protection

- [ ] **Mining pool with vardiff**
  - Stratum protocol support
  - Variable difficulty
  - Confidential coinbase outputs
  - Pool software: Fork from p2pool or existing pool

- [ ] **Rate-limit RPC to avoid spam**
  - Requests per IP: 100/hour
  - Authentication required for write operations
  - Public RPC for read-only

**Target Date**: 1 week before mainnet

---

### 8. Public Release Checklist ❌ NOT READY

**Status**: ⚠️ Code complete, release process needed

**Required Actions**:

- [ ] **Version bump to 1.0.0**
  - Update `configure.ac` or `CMakeLists.txt`
  - Update `clientversion.h`
  - Tag release: `git tag -s v1.0.0`

- [ ] **Release blog**
  - Announcement post
  - Feature highlights
  - Security guarantees
  - Exchange integration guide

- [ ] **Whitepaper update**
  - Document confidential transactions
  - Include Bulletproofs specification
  - Mathematical proofs
  - Security analysis

- [ ] **Security disclosures**
  - Responsible disclosure policy
  - Security contact email
  - Bug bounty program (optional)

- [ ] **Installer packages**
  - macOS .dmg
  - Windows .exe
  - Linux .deb, .rpm
  - AppImage

- [ ] **Code signing**
  - Sign all binaries
  - Notarize macOS app
  - Windows Authenticode

- [ ] **Community launch plan**
  - Reddit announcement
  - Twitter/X announcement
  - Discord/Telegram
  - GitHub release notes

**Target Date**: Mainnet launch day

---

## 📊 Overall Progress

| Category | Status | Completion |
|----------|--------|-----------|
| 1. Consensus Freeze | ❌ Not Frozen | 0% |
| 2. Genesis Parameters | ❌ Not Finalized | 0% |
| 3. Network Hardening | ⚠️ Partial | 30% |
| 4. Wallet Hardening | ✅ Mostly Done | 90% |
| 5. Build Verification | ❌ Not Started | 0% |
| 6. Security Audit | ⚠️ Internal Only | 50% |
| 7. Infrastructure | ❌ Not Deployed | 0% |
| 8. Public Release | ❌ Not Ready | 10% |

**Overall Mainnet Readiness**: ~23%

---

## 🎯 Critical Path to Mainnet

### Phase 1: Code Freeze (Week 1-2)
1. ✅ Complete confidential transactions (DONE)
2. ❌ Remove all placeholder keys
3. ❌ Freeze consensus code
4. ❌ Create release branch

### Phase 2: Testing & Audit (Week 3-6)
1. ❌ Reproducible builds
2. ❌ Multi-platform testing
3. ❌ External security audit
4. ❌ Testnet deployment

### Phase 3: Infrastructure (Week 7-8)
1. ❌ Deploy seed nodes
2. ❌ Deploy block explorer
3. ❌ Deploy mining pool
4. ❌ Setup monitoring

### Phase 4: Release (Week 9)
1. ❌ Finalize genesis block
2. ❌ Create release packages
3. ❌ Code signing
4. ❌ Public announcement

### Phase 5: Launch (Week 10)
1. ❌ Mainnet activation
2. ❌ Monitor network health
3. ❌ Emergency response ready

**Estimated Timeline**: 10-12 weeks from today

---

## ⚠️ Critical Risks

1. **Consensus Bug**: Could cause chain fork
   - Mitigation: Extensive testing, external audit

2. **Cryptography Bug**: Could break privacy
   - Mitigation: Use battle-tested libraries (Dalek Bulletproofs)

3. **Network Attack**: DDoS, eclipse attacks
   - Mitigation: Rate limiting, peer scoring, seed diversity

4. **Wallet Bug**: Could lose funds
   - Mitigation: Comprehensive testing, backup mechanisms

5. **Exchange Integration**: Delays adoption
   - Mitigation: Clear RPC documentation, support channels

---

## 📝 Next Steps (Immediate)

1. **This Week**:
   - Remove placeholder keys from error paths
   - Document consensus rules
   - Setup testnet

2. **Next Week**:
   - Freeze consensus code
   - Create reproducible build system
   - Start external audit process

3. **Month 1**:
   - Complete testing on all platforms
   - Deploy infrastructure
   - Finalize genesis parameters

---

**Last Updated**: 2025-11-17
**Document Owner**: DineroCoin Core Team
**Next Review**: Before consensus freeze

---

