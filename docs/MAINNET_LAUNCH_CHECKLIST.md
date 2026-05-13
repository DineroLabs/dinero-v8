# DineroCoin Mainnet Launch Checklist & Timeline

**Document Version:** 1.0
**Last Updated:** 2026-01-07
**Maintainer:** Trucker2827
**Target Launch:** TBD (4-month timeline recommended)

---

## Table of Contents

1. [Pre-Launch Checklist](#pre-launch-checklist)
2. [Launch Timeline](#launch-timeline)
3. [Launch Day Procedures](#launch-day-procedures)
4. [Post-Launch Monitoring](#post-launch-monitoring)
5. [Critical Path (Minimum Viable)](#critical-path-minimum-viable-mainnet)
6. [Current Status](#current-status)
7. [Decision Log](#decision-log)

---

## Pre-Launch Checklist

### ⚠️ CRITICAL (Must Complete Before Launch)

#### 1. Network Infrastructure ⚠️ BLOCKING

**Seed Nodes** (minimum 3-5 reliable bootstrap nodes)
- [ ] Deploy seed node 1 (US East)
  - Location: _________________
  - IP: _________________
  - Status: ❌ Not deployed

- [ ] Deploy seed node 2 (US West)
  - Location: _________________
  - IP: _________________
  - Status: ❌ Not deployed

- [ ] Deploy seed node 3 (Europe)
  - Location: _________________
  - IP: _________________
  - Status: ❌ Not deployed

- [ ] Deploy seed node 4 (Asia - optional but recommended)
  - Location: _________________
  - IP: _________________
  - Status: ❌ Not deployed

- [ ] Configure DNS seeds
  - [ ] Register domain (e.g., dinero-coin.com)
  - [ ] Set up DNS A records (seed1.dinero-coin.com, seed2.dinero-coin.com)
  - [ ] Update src/consensus/chainparams_impl.cpp with DNS seeds
  - [ ] Update vFixedSeeds with IP addresses

- [ ] Test peer discovery from fresh node
  - [ ] Clean install connects to seed nodes
  - [ ] Peer count reaches 8+ within 5 minutes
  - [ ] Block sync works from genesis

**Network Testing**
- [ ] Run 10+ node testnet for 7+ days continuously
  - Start date: _________________
  - End date: _________________
  - Issues found: _________________

- [ ] Test P2P connections (nodes connecting/disconnecting)
- [ ] Test block propagation (< 5 second target)
- [ ] Test transaction relay
- [ ] Test chain sync from genesis
- [ ] Simulate network partitions and healing
- [ ] Test nodes behind NAT/firewall

---

#### 2. Security Audit ⚠️ BLOCKING

**Code Review**
- [ ] External security review
  - Firm/Researcher: _________________
  - Budget: _________________
  - Timeline: _________________
  - Report: _________________

- [ ] Consensus code audit
  - [ ] Block validation (src/daemon/block_acceptor.cpp)
  - [ ] Transaction validation (src/consensus/tx_validation.cpp)
  - [ ] Mining (src/daemon/mining.cpp)
  - [ ] Difficulty adjustment (include/consensus/asert.h)

- [ ] Wallet security audit
  - [ ] Key generation (src/crypto/secp256k1_wrapper.cpp)
  - [ ] Transaction signing
  - [ ] Private key storage (encrypted wallet.db)

- [ ] P2P network security review
  - [ ] DoS prevention
  - [ ] Message validation
  - [ ] Peer banning logic

- [ ] RPC authentication review
  - [ ] Cookie auth mechanism
  - [ ] Authorization checks
  - [ ] Input validation

**Penetration Testing**
- [ ] DoS attack resistance
  - [ ] Large transaction spam
  - [ ] Connection flooding
  - [ ] Bandwidth exhaustion

- [ ] Eclipse attack resistance
  - [ ] Minimum peer diversity
  - [ ] Checkpoint validation

- [ ] Double-spend attack testing
  - [ ] Race conditions
  - [ ] Chain reorganization handling

- [ ] 51% attack cost analysis
  - Hashrate needed: _________________
  - Estimated cost: _________________
  - Mitigation: Checkpoints + minimum chainwork

**Bug Bounty Program**
- [ ] Set up bug bounty platform
  - Platform: _________________ (HackerOne, Bugcrowd, or custom)
  - Budget: _________________

- [ ] Define severity levels and rewards
  - Critical (consensus bug): _________________
  - High (fund loss): _________________
  - Medium (DoS): _________________
  - Low (UI bug): _________________

- [ ] Run for minimum 30 days before launch
  - Start date: _________________
  - End date: _________________
  - Issues found: _________________

---

#### 3. Software Releases ⚠️ BLOCKING

**Build Release Binaries**
- [ ] macOS x86_64 (Intel)
  - Build tested: ❌
  - File: dinero-qt-macos-x86_64-v1.0.0.dmg

- [ ] macOS arm64 (Apple Silicon)
  - Build tested: ❌
  - File: dinero-qt-macos-arm64-v1.0.0.dmg

- [ ] Windows x64
  - Build tested: ❌
  - File: dinero-qt-windows-x64-v1.0.0.exe

- [ ] Windows arm64
  - Build tested: ❌
  - File: dinero-qt-windows-arm64-v1.0.0.exe

- [ ] Linux x86_64 (Ubuntu/Debian .deb)
  - Build tested: ❌
  - File: dinerocoin_1.0.0_amd64.deb

- [ ] Linux x86_64 (Fedora/RHEL .rpm)
  - Build tested: ❌
  - File: dinerocoin-1.0.0-1.x86_64.rpm

- [ ] Linux arm64
  - Build tested: ❌
  - File: dinerocoin_1.0.0_arm64.deb

**Installation Packages**
- [ ] macOS .dmg installer
  - [ ] Drag-to-Applications flow
  - [ ] Code signing (Apple Developer cert)
  - [ ] Notarization (required for macOS 10.15+)

- [ ] Windows .exe installer
  - [ ] Inno Setup or NSIS
  - [ ] Start menu shortcuts
  - [ ] Uninstaller
  - [ ] Code signing (Authenticode certificate)

- [ ] Linux package repositories
  - [ ] PPA for Ubuntu/Debian
  - [ ] COPR for Fedora
  - [ ] AUR for Arch (community-maintained)

- [ ] Auto-update mechanism (optional)
  - [ ] Check for updates on startup
  - [ ] Download and verify signatures
  - [ ] Prompt user to install

**Release Testing**
- [ ] Fresh install on clean VM (each platform)
  - [ ] macOS 12+: ❌
  - [ ] macOS 13+: ❌
  - [ ] macOS 14+: ❌
  - [ ] Windows 10: ❌
  - [ ] Windows 11: ❌
  - [ ] Ubuntu 20.04: ❌
  - [ ] Ubuntu 22.04: ❌
  - [ ] Ubuntu 24.04: ❌

- [ ] Upgrade from previous version (if applicable)
- [ ] Verify no dependency issues (ldd/otool checks)
- [ ] Test wallet creation and backup
- [ ] Test send/receive transactions
- [ ] Test mining functionality

---

#### 4. Documentation ⚠️ BLOCKING

**User Documentation** (for non-developers)
- [ ] "Getting Started" guide (< 5 steps, with screenshots)
  - File: docs/user/GETTING_STARTED.md
  - Screenshots: docs/user/screenshots/
  - Status: ❌ Not written

- [ ] "How to Install" (each platform)
  - macOS: ❌
  - Windows: ❌
  - Linux: ❌

- [ ] "How to Create Wallet"
  - File: docs/user/CREATE_WALLET.md
  - Status: ❌ Not written

- [ ] "How to Receive Coins"
  - File: docs/user/RECEIVE_COINS.md
  - Status: ❌ Not written

- [ ] "How to Send Coins"
  - File: docs/user/SEND_COINS.md
  - Status: ❌ Not written

- [ ] "How to Backup Wallet" (CRITICAL - prevent fund loss)
  - File: docs/user/BACKUP_WALLET.md
  - Status: ❌ Not written
  - **WARNING:** Users MUST understand this before using mainnet

- [ ] "How to Mine" (if PoW)
  - File: docs/user/MINING_GUIDE.md
  - Status: ❌ Not written

- [ ] FAQ (common questions)
  - File: docs/user/FAQ.md
  - Status: ❌ Not written

- [ ] Troubleshooting guide
  - File: docs/user/TROUBLESHOOTING.md
  - Status: ❌ Not written

**Developer Documentation**
- [ ] RPC API reference
  - File: docs/RPC_API.md
  - Status: ⚠️ Partial (exists but needs review)

- [ ] Integration guide (for exchanges/services)
  - File: docs/EXCHANGE_INTEGRATION.md
  - Status: ❌ Not written

- [ ] Build instructions
  - File: docs/BUILD.md
  - Status: ⚠️ Partial (needs Windows/Linux)

- [ ] Network protocol specification
  - File: docs/NETWORK_PROTOCOL.md
  - Status: ❌ Not written

**Legal/Compliance**
- [ ] Terms of Service
  - File: TERMS_OF_SERVICE.md
  - Status: ❌ Not written
  - **Note:** Consult legal counsel

- [ ] Privacy Policy
  - File: PRIVACY_POLICY.md
  - Status: ❌ Not written

- [ ] Disclaimer (no investment advice, use at own risk)
  - File: DISCLAIMER.md
  - Status: ❌ Not written

- [ ] License
  - Current: _________________ (verify license file)
  - Clarify third-party licenses
  - Attribution requirements

---

#### 5. Initial Distribution ⚠️ BLOCKING

**Distribution Plan** (choose one or combine)

Current Economic Parameters (from subsidy.h):
- ✅ Block reward: 100 DIN
- ✅ Halving interval: 1,314,000 blocks (5 years @ 2 min blocks)
- ✅ Max supply: 265,428,000 DIN
- ✅ Block time: 2 minutes (120 seconds)
- ✅ Genesis allocation: 100 DIN burned (OP_RETURN)
- ✅ Premine: 2,627,900 DIN at block 1 (~1% of total supply)

**Distribution Strategy:**
- [ ] Fair Launch (everyone mines from block 0)
  - ✅ Consensus supports this (no special genesis allocation)
  - Date/time: _________________

- [ ] Faucet (give away small amounts for testing)
  - Amount per claim: _________________
  - Frequency limit: _________________
  - Total budget: _________________

- [ ] Airdrop (initial allocation to early adopters)
  - Criteria: _________________
  - Amount per user: _________________
  - Total budget: _________________

**Premine Transparency** (Block 1: 2,627,900 DIN)
- [ ] Public disclosure of premine address
  - Address: _________________ (will be visible in block 1)
  - Purpose: _________________

- [ ] Multi-sig or time-lock for premine
  - Strategy: _________________
  - Keyholders: _________________

- [ ] Clear explanation of premine purpose
  - Document: docs/PREMINE_DISCLOSURE.md
  - Status: ❌ Not written

- [ ] Vesting schedule (if applicable)
  - Schedule: _________________
  - Enforcement: _________________

---

### 🔶 HIGH PRIORITY (Should Complete Before Launch)

#### 6. Website & Community

**Website** (dinero-coin.com or similar)
- [ ] Domain registration
  - Domain: _________________
  - Registrar: _________________
  - Expiry: _________________

- [ ] Landing page with value proposition
  - Hosting: _________________
  - URL: _________________

- [ ] Download links (all platforms)
- [ ] Documentation portal
- [ ] Roadmap
- [ ] Team/About section
- [ ] Blog/News section
- [ ] Contact/Support page

**Community Channels**
- [ ] Discord server
  - URL: _________________
  - Admin: _________________

- [ ] Telegram group
  - URL: _________________
  - Admin: _________________

- [ ] Twitter/X account
  - Handle: @_________________
  - Followers: _________________

- [ ] Reddit community
  - Subreddit: r/_________________
  - Moderators: _________________

- [ ] GitHub Discussions
  - Enabled: ❌

- [ ] Email newsletter
  - Platform: _________________
  - Subscribers: _________________

**Social Media Presence**
- [ ] Announce launch date
- [ ] Regular updates (2-3x per week)
- [ ] Engage with community
- [ ] Share milestones

---

#### 7. Monitoring & Operations

**Monitoring Dashboard**
- [ ] Node health monitoring
  - Tool: _________________ (Prometheus, Grafana, custom)

- [ ] Network hashrate tracking
- [ ] Peer count tracking
- [ ] Block propagation metrics
- [ ] Mempool size monitoring
- [ ] Alert system (email/SMS for critical issues)
  - Service: _________________
  - Contacts: _________________

**Operational Procedures**
- [ ] Incident response plan
  - Document: docs/operations/INCIDENT_RESPONSE.md
  - Status: ❌ Not written

- [ ] Emergency contact list
  - Primary: _________________
  - Secondary: _________________
  - Escalation: _________________

- [ ] Rollback procedure (if critical bug found)
  - Document: docs/operations/ROLLBACK_PROCEDURE.md
  - Status: ❌ Not written

- [ ] Communication plan (how to alert users)
  - Channels: Discord, Twitter, Email
  - Template: docs/operations/ALERT_TEMPLATE.md

- [ ] Backup/restore procedures
  - Document: docs/operations/BACKUP_RESTORE.md
  - Status: ❌ Not written

---

#### 8. Block Explorer

**Block Explorer Deployment**
- [ ] Choose platform
  - Custom build: ❌
  - Fork existing: _________________ (Insight, Blockbook, etc.)

- [ ] Features
  - [ ] Search by address, transaction, block
  - [ ] Network statistics (hashrate, difficulty, nodes)
  - [ ] Rich list (top addresses)
  - [ ] Mempool viewer
  - [ ] API for developers
  - [ ] Mobile-responsive design

- [ ] Deployment
  - URL: _________________
  - Server: _________________
  - Status: ❌ Not deployed

---

### 🟢 NICE-TO-HAVE (Can Launch Without)

#### 9. Enhanced Features
- [ ] Mobile wallets (iOS/Android)
- [ ] Hardware wallet support (Ledger/Trezor)
- [ ] Light wallet (SPV mode)
- [ ] Web wallet
- [ ] Paper wallet generator
- [ ] Multisig support UI

#### 10. Exchange Listings
- [ ] Integration documentation for exchanges
  - File: docs/EXCHANGE_INTEGRATION.md

- [ ] Reach out to exchanges
  - Tier 3 (small): _________________
  - Tier 2 (medium): _________________
  - Tier 1 (major): _________________ (later)

- [ ] Provide technical support for integration
- [ ] Market makers (optional)

#### 11. Developer Tools
- [ ] Python SDK
- [ ] JavaScript SDK
- [ ] REST API (in addition to RPC)
- [ ] Testnet faucet
- [ ] Developer sandbox

---

## Launch Timeline

### Recommended Timeline: 4 Months

#### Month 1-2: Foundation (Weeks 1-8)
**Goal: Infrastructure & Security**

**Week 1-2: Network Infrastructure**
- [ ] Week 1
  - [ ] Deploy seed node infrastructure
  - [ ] Configure monitoring
  - [ ] Set up DNS

- [ ] Week 2
  - [ ] Test multi-node network
  - [ ] Verify peer discovery
  - [ ] Document seed node setup

**Week 3-4: Security Audit**
- [ ] Week 3
  - [ ] Engage security firm OR launch bug bounty
  - [ ] Internal code review
  - [ ] Create security checklist

- [ ] Week 4
  - [ ] Penetration testing
  - [ ] Review findings
  - [ ] Begin fixes

**Week 5-6: Build & Package**
- [ ] Week 5
  - [ ] Create build pipeline
  - [ ] Generate binaries for all platforms
  - [ ] Set up code signing

- [ ] Week 6
  - [ ] Test installations on clean VMs
  - [ ] Fix installation issues
  - [ ] Document build process

**Week 7-8: Documentation**
- [ ] Week 7
  - [ ] Write user guides
  - [ ] Create screenshots
  - [ ] Record video tutorials (optional)

- [ ] Week 8
  - [ ] Developer documentation
  - [ ] FAQ and troubleshooting
  - [ ] Legal documents review

---

#### Month 3: Polish & Testing (Weeks 9-12)
**Goal: User Experience & Extended Testing**

**Week 9-10: Extended Testnet**
- [ ] Week 9
  - [ ] Deploy 10+ node testnet
  - [ ] Run continuously
  - [ ] Monitor for issues

- [ ] Week 10
  - [ ] Stress test (high transaction volume)
  - [ ] Simulate network conditions
  - [ ] Test edge cases

**Week 11: Website & Community**
- [ ] Launch website
- [ ] Set up community channels
- [ ] Create social media accounts
- [ ] Begin building anticipation

**Week 12: Block Explorer**
- [ ] Deploy block explorer
- [ ] Test with testnet
- [ ] Prepare for mainnet
- [ ] Document API

---

#### Month 4: Pre-Launch (Weeks 13-16)
**Goal: Final Preparation**

**Week 13-14: Beta Testing**
- [ ] Week 13
  - [ ] Release beta to select users
  - [ ] Gather feedback
  - [ ] Monitor usage

- [ ] Week 14
  - [ ] Fix UX issues
  - [ ] Iterate based on feedback
  - [ ] Final polish

**Week 15: Final Review**
- [ ] Code freeze (no more changes except critical bugs)
- [ ] Final security review
- [ ] Release candidate testing (RC1)
- [ ] Go/no-go checklist review

**Week 16: LAUNCH WEEK** 🚀
- [ ] T-7: Announce launch date publicly
- [ ] T-6: Prepare press release
- [ ] T-5: Alert community (all channels)
- [ ] T-4: Final binary builds
- [ ] T-3: Deploy seed nodes to production
- [ ] T-2: Deploy block explorer
- [ ] T-1: Final checks, team meeting
- [ ] T-0: MAINNET LAUNCH

---

### Aggressive Timeline: 2 Months (Minimum Viable)

#### Week 1-2: Infrastructure
- [ ] Deploy 3 seed nodes
- [ ] Quick security review
- [ ] Build binaries for major platforms (macOS, Windows, Linux x64)

#### Week 3-4: Documentation
- [ ] Create basic user docs (install, wallet, backup)
- [ ] Set up website with downloads
- [ ] Run 1-week testnet

#### Week 5-6: Testing
- [ ] Fix critical issues found
- [ ] Set up community (Discord/Twitter)
- [ ] Deploy basic block explorer

#### Week 7-8: Launch
- [ ] Final testing
- [ ] Release candidate
- [ ] LAUNCH

**Risk Level:** ⚠️ HIGH (not recommended unless absolutely necessary)

---

## Launch Day Procedures

### T-7 Days (Launch Week)
- [ ] Final security audit review
  - Reviewer: _________________
  - Sign-off: ❌

- [ ] Code freeze (no more changes except critical bugs)
  - Last commit: _________________
  - Git tag: v1.0.0-mainnet

- [ ] Release candidate testing (RC1)
  - Testers: _________________
  - Issues found: _________________

- [ ] Announce launch date publicly
  - Date/Time: _________________ UTC
  - Announcement: ❌

- [ ] Prepare press release
  - Draft: docs/press/MAINNET_LAUNCH_PR.md
  - Status: ❌

- [ ] Alert community
  - [ ] Discord announcement
  - [ ] Twitter announcement
  - [ ] Email newsletter
  - [ ] Reddit post

### T-3 Days
- [ ] Deploy seed nodes to production
  - [ ] Seed node 1: ❌
  - [ ] Seed node 2: ❌
  - [ ] Seed node 3: ❌
  - [ ] Seed node 4: ❌

- [ ] Final binary builds
  - [ ] Tag release in git: v1.0.0
  - [ ] Build all platforms
  - [ ] Generate checksums (SHA256)

- [ ] Upload binaries to GitHub releases
  - URL: _________________
  - Status: ❌

- [ ] Update website with download links
  - Status: ❌

- [ ] Deploy block explorer
  - URL: _________________
  - Status: ❌

- [ ] Deploy monitoring dashboard
  - URL: _________________
  - Status: ❌

### T-1 Day
- [ ] Final pre-launch checks
  - [ ] All seed nodes online and syncing
  - [ ] Test fresh node sync from seed nodes
  - [ ] Verify all download links work
  - [ ] Verify documentation is accessible
  - [ ] Test block explorer connectivity
  - [ ] Test monitoring dashboard

- [ ] Team meeting (go/no-go decision)
  - Attendees: _________________
  - Decision: ❌ GO / ❌ NO-GO
  - Notes: _________________

- [ ] Announce launch time (specific UTC time)
  - Time: _________________ UTC
  - [ ] Posted to Discord
  - [ ] Posted to Twitter
  - [ ] Posted to website
  - [ ] Email sent

### Launch Day (T-0)

**Hour 0: Genesis** 🚀
- [ ] Start seed nodes with mainnet genesis
  - Command: `dinerod -daemon`
  - Genesis hash: 00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74

- [ ] Monitor peer connections
  - Target: 8+ peers within 10 minutes
  - Actual: _________________

- [ ] Watch for first block (after genesis)
  - Block 1 time: _________________
  - Block 1 hash: _________________
  - Contains premine: ✅ (2,627,900 DIN)

**Hour 1-6: Early Monitoring**
- [ ] Monitor hashrate
  - Hour 1: _________________
  - Hour 3: _________________
  - Hour 6: _________________

- [ ] Monitor peer count
  - Hour 1: _________________
  - Hour 3: _________________
  - Hour 6: _________________

- [ ] Check block propagation
  - Average time: _________________ seconds
  - Target: < 5 seconds

- [ ] Respond to community questions
  - Primary support: Discord
  - Response time: < 30 minutes

**Hour 6-24: Initial Stability**
- [ ] Verify difficulty adjustment working
  - Expected: ASERT adjusts per block
  - Actual: _________________

- [ ] Check for any forks/reorgs
  - Forks detected: _________________
  - Max reorg depth: _________________

- [ ] Monitor for unusual activity
  - Large transactions: _________________
  - Attack attempts: _________________

- [ ] Communicate status to community
  - [ ] 6-hour update posted
  - [ ] 12-hour update posted
  - [ ] 24-hour update posted

**Day 1-7: First Week**
- [ ] Daily status updates
  - Day 1: ❌
  - Day 2: ❌
  - Day 3: ❌
  - Day 4: ❌
  - Day 5: ❌
  - Day 6: ❌
  - Day 7: ❌

- [ ] Monitor network health
  - Hashrate trend: _________________
  - Node count trend: _________________
  - Transaction volume: _________________

- [ ] Address any issues quickly
  - Issues found: _________________
  - Resolution time: _________________

- [ ] Gather user feedback
  - Feedback channel: Discord #feedback
  - Common issues: _________________

---

## Post-Launch Monitoring (First 30 Days)

### Week 1 (Days 1-7)
- [ ] Daily network health reports
  - Published to: _________________
  - Metrics tracked: hashrate, peers, blocks, txs

- [ ] Fix any critical bugs immediately
  - Process: Hotfix → Test → Release
  - Max time to patch: 24 hours

- [ ] Release patch if needed
  - Version: v1.0.1
  - Changes: _________________

- [ ] Active community support
  - Response time: < 1 hour
  - Support channels: Discord, Twitter DM

### Week 2-4 (Days 8-30)
- [ ] Bi-weekly status updates
  - Week 2: ❌
  - Week 3: ❌
  - Week 4: ❌

- [ ] Monitor for consensus issues
  - Consensus bugs: _________________
  - Forks/reorgs: _________________

- [ ] Track adoption metrics
  - Active nodes: _________________
  - Transactions/day: _________________
  - Unique addresses: _________________

- [ ] Plan first post-launch improvements
  - Priority 1: _________________
  - Priority 2: _________________
  - Priority 3: _________________

### Month 2-3 (Days 31-90)
- [ ] Monthly reports
  - Month 2: ❌
  - Month 3: ❌

- [ ] Implement post-launch features
  - Feature 1: _________________
  - Feature 2: _________________

- [ ] Exchange outreach
  - Exchanges contacted: _________________
  - Listings secured: _________________

- [ ] Developer ecosystem growth
  - Developers onboarded: _________________
  - Tools released: _________________

---

## Critical Path (Minimum Viable Mainnet)

**If you need to launch ASAP, this is the absolute minimum:**

### ⚡ Week 1-2: Infrastructure
1. ⚠️ Deploy 3 seed nodes
2. ⚠️ Quick security review (internal)
3. ⚠️ Build binaries for major platforms (macOS, Windows, Linux x64)

### ⚡ Week 3-4: Documentation & Testing
4. ⚠️ Create basic user docs (install, wallet, backup)
5. ⚠️ Set up website with downloads
6. ⚠️ Run 1-week testnet

### ⚡ Week 5-6: Polish & Community
7. ⚠️ Fix critical issues found
8. ⚠️ Set up community (Discord/Twitter)
9. ⚠️ Deploy basic block explorer

### ⚡ Week 7-8: Launch
10. ⚠️ Final testing
11. ⚠️ LAUNCH

**Timeline:** 2 months minimum (8 weeks)
**Risk Level:** ⚠️⚠️⚠️ HIGH (not recommended)

**Why 4 months is better:**
- Thorough security audit (prevents fund loss)
- Professional polish (better user experience)
- Extended testing (catches edge cases)
- Community building (more users at launch)
- Lower risk of critical bugs

---

## Current Status

**Last Updated:** 2026-01-07

### Completed ✅
- [x] Core blockchain implementation
- [x] 100+ formally verified protocol properties
- [x] Qt GUI wallet (dinero-qt)
- [x] CLI wallet (dinero-wallet-cli)
- [x] Reference wallet with blockchain sync
- [x] CPU miner with ARM SIMD optimization
- [x] Genesis hash consistency fix (commit 91dc2875)
- [x] Multi-node testnet infrastructure (3 VMs)
- [x] Build system (CMake)
- [x] Test suite (46/46 passing)

### In Progress 🔶
- [ ] None currently

### Not Started ❌
- [ ] Seed node deployment
- [ ] Security audit
- [ ] Release binaries
- [ ] User documentation
- [ ] Website
- [ ] Block explorer
- [ ] Community setup

### Blocking Issues ⚠️
1. **No peer network** - Need seed nodes before mainnet launch
2. **No security audit** - Critical for mainnet
3. **No user documentation** - Users can't use it without guides
4. **No installation packages** - Users need .dmg/.exe/.deb files

### Technical Readiness
- Blockchain: ✅ Ready
- Consensus: ✅ Verified (100+ properties)
- Wallet: ✅ Functional (GUI + CLI)
- Mining: ✅ Working (fixed genesis hash)
- Network: ❌ No seed nodes
- Distribution: ❌ No binaries
- Support: ❌ No documentation

**Overall Status:** ~40% ready for mainnet

---

## Decision Log

**Purpose:** Track major decisions made during launch preparation.

### Decision 1: Launch Timeline
- **Date:** _________________
- **Decision:** 4-month timeline OR 2-month aggressive
- **Reasoning:** _________________
- **Approved by:** _________________

### Decision 2: Genesis Parameters
- **Date:** 2025-12-25 (from commit b30a8d62)
- **Decision:**
  - Genesis hash: 00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74
  - Block 1 premine: 2,627,900 DIN
  - Motto: "Dinero: Real Money For Free People"
- **Reasoning:** Bitcoin-correct subsidy model
- **Approved by:** Trucker2827

### Decision 3: Premine Use
- **Date:** _________________
- **Decision:** _________________
- **Reasoning:** _________________
- **Approved by:** _________________

### Decision 4: Distribution Strategy
- **Date:** _________________
- **Decision:** Fair launch / Faucet / Airdrop / Combination
- **Reasoning:** _________________
- **Approved by:** _________________

### Decision 5: Seed Node Locations
- **Date:** _________________
- **Decision:** _________________
- **Reasoning:** _________________
- **Approved by:** _________________

### Decision 6: Security Audit Firm
- **Date:** _________________
- **Decision:** _________________
- **Budget:** _________________
- **Approved by:** _________________

### Decision 7: Launch Date
- **Date:** _________________
- **Decision:** _________________ UTC
- **Reasoning:** _________________
- **Approved by:** _________________

---

## Notes

### Maintenance Period (Year 1)
- **Maintainer:** Trucker2827
- **Duration:** 1 year from launch
- **Transition:** Community handover after year 1

### What Can Change During Maintenance
**✅ Safe to add/modify (non-consensus):**
- Performance optimizations
- New RPC methods
- GUI improvements
- New wallet features
- Mining optimizations
- Documentation
- Bug fixes (non-consensus)

**⚠️ Requires coordination (consensus-critical):**
- Block validation rules
- Transaction validation rules
- Script semantics (Ring 7 - FROZEN)
- Difficulty adjustment
- Subsidy schedule
- Any change that could cause nodes to disagree

**Process for consensus changes:**
1. Versioned deployment (BIP9-style)
2. Activation height (all nodes upgrade by block X)
3. Clear communication to all operators
4. Testnet testing first
5. Minimum 2 months notice

### Emergency Contacts
- **Primary:** _________________
- **Secondary:** _________________
- **Security:** _________________

### External Resources
- **GitHub:** https://github.com/Trucker2827/Dinero-Coin
- **Website:** _________________ (TBD)
- **Discord:** _________________ (TBD)
- **Twitter:** _________________ (TBD)

---

## Appendix: Useful Commands

### Check Genesis Hash
```bash
dinero-cli getblockhash 0
# Expected: 00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74
```

### Check Network Info
```bash
dinero-cli getblockchaininfo
```

### Check Peer Count
```bash
dinero-cli getconnectioncount
```

### Check Mining Status
```bash
dinero-cli getmininginfo
```

### Start Seed Node
```bash
dinerod -daemon -server -rpcuser=dinero -rpcpassword=<password>
```

### Test RPC Connectivity
```bash
curl -s -u dinero:<password> --data-binary '{"jsonrpc":"1.0","id":"test","method":"getblockchaininfo","params":[]}' -H 'content-type: text/plain;' http://127.0.0.1:20997/
```

### Build Release Binary
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

---

**End of Document**

*This is a living document. Update as decisions are made and progress is achieved.*
