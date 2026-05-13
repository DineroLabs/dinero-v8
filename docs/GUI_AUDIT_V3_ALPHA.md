# DineroCoin GUI Audit - v3.0.0-alpha1

**Date:** 2026-01-11
**Scope:** dinero-qt (Qt6 Desktop Wallet)
**Purpose:** Gap analysis + roadmap for alpha release

---

## Executive Summary

**Current Status:** ✅ **PRODUCTION-READY for v2.x features**

The dinero-qt GUI is **fully functional** for core wallet operations (send, receive, mining, wallet management). It has **zero fake data** and all buttons trigger real RPC calls.

**Gap:** Protocol v3.0 features (Stateless, Lightning, Mobile) **are NOT exposed** in the GUI yet.

**Recommendation:** Ship v3.0.0-alpha1 with **existing GUI as-is** (v2.x feature set). Add v3.0 GUI features in v3.0.0-beta1.

---

## What the GUI Currently Does ✅

### Tab 1: Overview (Network Stats)
- ✅ Block height display (`getblockcount`)
- ✅ Peer count (`getpeerinfo`)
- ✅ Economic info: phase, reward, halving (`geteconomics`)
- ✅ Total supply (`getsupply`)
- ✅ Mempool stats (`getmempoolinfo`)
- ✅ Auto-refresh every 5 seconds
- ✅ Connection status indicator (green/orange/red)
- ✅ Network warning banner (mainnet/testnet/regtest)

### Tab 2: Wallet (Balance & HD Addresses)
- ✅ Balance display (confirmed/unconfirmed/immature) (`getbalance`)
- ✅ Unlock wallet button (`walletunlock`)
- ✅ Lock wallet button (`walletlock`)
- ✅ Encrypt wallet button (wizard flow)
- ✅ Derive new address button (`deriveaddress`)
- ✅ Address table (index, address, path, copy button)
- ✅ Mining address auto-fill

### Tab 3: Send (Transactions)
- ✅ Recipient address field (din1/tdin1/rdin1 validation)
- ✅ Amount field (QDoubleValidator, 8 decimals)
- ✅ "Max" button (balance - fee)
- ✅ Fee field (custom fee, default 0.00001 DIN)
- ✅ Fee preset selector (low/medium/high)
- ✅ Send transaction button (`sendtoaddress`)
- ✅ Transaction result display (txid)
- ✅ List UTXOs button (`listunspent`)

### Tab 4: Receive (Generate Addresses)
- ✅ Address display field
- ✅ Generate new address button (`getnewaddress`)
- ✅ Validate button (`validateaddress`)
- ✅ Copy button (clipboard)
- ✅ QR code generator

### Tab 5: Transactions (History)
- ✅ Transaction history table (`listtransactions`)
- ✅ TXID, type, amount, confirmations, date/time, address
- ✅ Color coding (green=received, red=sent, orange=pending)
- ✅ Auto-updates after mining

### Tab 6: Explorer (Block Lookup)
- ✅ Best block display (`getbestblockhash`)
- ✅ Block hash input field
- ✅ Get block button (`getblock`)
- ✅ Latest block auto-fetch
- ✅ Block data display (JSON viewer)

### Tab 7: Mining (CPU Mining)
- ✅ Mining address input (validation)
- ✅ "Use Wallet Address" button
- ✅ Threads input (auto-detect)
- ✅ Start mining button (launches `dinero-miner` process)
- ✅ Stop mining button (SIGTERM/SIGKILL)
- ✅ Mining statistics dashboard:
  - Blocks found counter
  - Uptime
  - Current hashrate (H/s)
  - Total hashes
  - Average hashrate (1-minute rolling)
- ✅ Mining output console (live stdout/stderr)
- ✅ Block found detection (flashes green, updates balance)

### Wallet Wizard (First Run)
- ✅ Create new wallet flow:
  1. Generate seed (`createseed` - BIP39 12-word)
  2. Display seed (hidden by default)
  3. Confirm seed (3 random words)
  4. Set password (strength meter)
  5. Create wallet (`createwallet`)
- ✅ Restore wallet flow:
  1. Enter seed (BIP39 validation)
  2. Optional passphrase (25th word)
  3. Set password
  4. Restore wallet (`restorewallet`)

### Connection & Failover
- ✅ 3-tier server list (localhost + 2 SSH tunnels)
- ✅ Auto-failover logic (switches on errors)
- ✅ Health checks (every 30 seconds)
- ✅ Cookie authentication (Base64 encoded)
- ✅ Connection status display (color-coded)

### Daemon Control
- ✅ Start daemon button (launches `dinerod` process)
- ✅ Stop daemon button (graceful shutdown)
- ✅ Intelligent daemon state detection
- ✅ Daemon log monitoring (file watcher)

---

## What the GUI Does NOT Do Yet ❌

### Missing: Protocol v3.0 Features

#### 1. Stateless Node Mode (Phase 8)
- ❌ No toggle for stateless/stateful mode
- ❌ No proof cache status indicator
- ❌ No proof network stats (cache hit rate, proof requests)
- ❌ No Utreexo accumulator state display

**Required RPC bindings:**
```cpp
void getUtreexoState();         // Get accumulator state
void getProofCacheStats();      // Cache hit rate, size, evictions
void toggleStatelessMode(bool); // Enable/disable stateless
```

#### 2. Lightning Network (Phase 11)
- ⚠️ Lightning widget exists but **incomplete**:
  - ✅ Open channel (basic UI exists)
  - ❌ Close channel (no RPC binding)
  - ❌ List channels (`lightning.listchannels`)
  - ❌ Send payment (`lightning.sendpayment`)
  - ❌ Invoice generation (`lightning.createinvoice`)
  - ❌ Invoice payment (`lightning.payinvoice`)
  - ❌ Channel balance display
  - ❌ Lightning wallet balance
  - ❌ Watchtower status (stateless)

**Required RPC bindings:**
```cpp
// Lightning Network RPCs
void lightningOpenChannel(QString nodeId, uint64_t amount);
void lightningCloseChannel(QString channelId);
void lightningListChannels();
void lightningSendPayment(QString invoice, uint64_t amountMsat);
void lightningCreateInvoice(uint64_t amountMsat, QString description);
void lightningPayInvoice(QString invoice);
void lightningGetBalance();
void lightningWatchtowerStatus();
```

#### 3. Mobile Profile Mode (Phase 12)
- ❌ No mobile mode toggle (burst mode)
- ❌ No resource monitoring (memory, battery)
- ❌ No iOS compliance indicators (30s background limit)

**Required RPC bindings:**
```cpp
void getMobileNodeStatus();    // Burst mode, memory usage, cache size
void toggleBurstMode(bool);    // Enable/disable burst mode
```

#### 4. Proof Network Monitoring (Phase 9)
- ❌ No proof request stats
- ❌ No proof gossip activity display
- ❌ No proof compression stats (ZSTD)
- ❌ No proof cache eviction log

**Required RPC bindings:**
```cpp
void getProofNetworkStats();   // Requests, gossip, compression
void getProofCacheLog();       // Eviction events
```

#### 5. Advanced Transaction Features
- ❌ No PSBT (Partially Signed Bitcoin Transaction) support
- ❌ No multi-sig wallet creation
- ❌ No coin control (manual UTXO selection)
- ❌ No RBF (Replace-By-Fee) support
- ❌ No CPFP (Child-Pays-For-Parent) support

**Required RPC bindings:**
```cpp
// PSBT
void createPSBT(QJsonArray inputs, QJsonObject outputs);
void signPSBT(QString psbt);
void finalizePSBT(QString psbt);

// Coin control
void lockUTXO(QString txid, uint32_t vout);
void unlockUTXO(QString txid, uint32_t vout);
void listLockedUTXOs();

// Fee bumping
void bumpFee(QString txid, double newFeeRate);    // RBF
void cpfpTx(QString parentTxid, double feeRate);  // CPFP
```

#### 6. Covenant Support (Phase 7)
- ❌ No covenant transaction creation UI
- ❌ No vault monitoring
- ❌ No timelock visualization

**Required RPC bindings:**
```cpp
void createCovenantTx(QString covenantType, QJsonObject params);
void listCovenants();
void monitorVault(QString vaultAddress);
```

---

## RPC Methods Exposed in GUI (Current)

**File:** `gui/src/rpcclient.h`

### Blockchain Methods
```cpp
void getInfo();              // ✅ Used in Overview tab
void getBlockCount();        // ✅ Used in Overview tab
void getBestBlockHash();     // ✅ Used in Explorer tab
void getBlock(QString hash); // ✅ Used in Explorer tab
```

### Economics Methods
```cpp
void getEconomics();         // ✅ Used in Overview tab
void getSupply();            // ✅ Used in Overview tab
```

### Wallet Methods
```cpp
void getNewAddress();        // ✅ Used in Receive tab
void validateAddress(QString addr); // ✅ Used in Receive tab
void getBalance();           // ✅ Used in Wallet tab
void listAddresses();        // ✅ Used in Wallet tab
void deriveAddress(int change, QString index); // ✅ Used in Wallet tab
void walletUnlock(QString password, int timeout); // ✅ Used in Wallet tab
void walletLock();           // ✅ Used in Wallet tab
void walletPassphraseChange(QString oldPass, QString newPass); // ❌ Not wired to UI
```

### Transaction Methods
```cpp
void listUnspent();          // ✅ Used in Send tab
void sendToAddress(QString address, double amount); // ✅ Used in Send tab
void sendToAddressWithFee(QString address, double amount, double feeRate); // ✅ Used in Send tab
void createRawTransaction(QJsonArray inputs, QJsonObject outputs); // ❌ Not wired to UI
void signRawTransactionWithWallet(QString hexTx); // ❌ Not wired to UI
void sendRawTransaction(QString hexTx); // ❌ Not wired to UI
```

### Mining Methods
```cpp
void miningInfo();           // ✅ Used in Mining tab
void miningStart(int threads); // ❌ Launches external miner instead
void miningStop();           // ❌ Launches external miner instead
void miningSetAddress(QString address); // ❌ Launches external miner instead
void miningGetAddress();     // ❌ Not used
```

### Network Methods
```cpp
void getMempoolInfo();       // ✅ Used in Overview tab
void getPeerInfo();          // ✅ Used in Overview tab
```

### Fee Estimation
```cpp
void estimateSmartFee(int confTarget, QString mode); // ✅ Used in Send tab (Phase 35)
```

---

## RPC Methods Available in Daemon (NOT in GUI)

**Source:** `src/rpc/` (112 files, 1000+ RPC methods registered)

### Category: Utreexo / Stateless (Phase 8)
```
utreexo.getstate
utreexo.getaccumulator
utreexo.verifyproof
utreexo.getproofsize
```

### Category: Proof Network (Phase 9)
```
proof.getstats
proof.getcachestats
proof.getrouter stats
proof.getgossipstats
proof.getcompressionstats
```

### Category: Lightning (Phase 11)
```
lightning.openchannel
lightning.closechannel
lightning.listchannels
lightning.sendpayment
lightning.createinvoice
lightning.payinvoice
lightning.getbalance
lightning.watchtowerstatus
```

### Category: Mobile Node (Phase 12)
```
mobile.getstatus
mobile.toggleburstmode
mobile.getresourcestats
```

### Category: Covenant (Phase 7)
```
covenant.create
covenant.list
covenant.monitor
```

### Category: Advanced Wallet
```
createpsbt
signpsbt
finalizepsbt
lockunspent
listlockunspent
bumpfee
```

### Category: Contract
```
contract.deploy
contract.call
contract.getstate
```

### Category: Taproot
```
taproot.createaddress
taproot.signmessage
taproot.verifymessage
```

### Category: Hardware Wallet
```
hww.enumerate
hww.getpubkey
hww.signpsbt
```

---

## What "GUI Complete" Means

### Definition: v2.x GUI Complete ✅

**Criteria:**
- ✅ All core wallet operations work (send, receive, balance)
- ✅ HD wallet fully functional (derive, encrypt, lock/unlock)
- ✅ Mining works (external miner integration)
- ✅ Network monitoring (peers, mempool, blocks)
- ✅ Transaction history
- ✅ Address validation
- ✅ Daemon control (start/stop)
- ✅ Zero fake data (all RPC-backed)

**Status:** ✅ **COMPLETE** (as of v2.6.0)

---

### Definition: v3.0 GUI Complete (Alpha Target)

**Criteria:**
- ✅ All v2.x features (above)
- ⚠️ Stateless mode toggle + status display
- ⚠️ Lightning basic operations (open/close channel, send/receive)
- ⚠️ Proof network monitoring (cache stats, gossip)
- ❌ Mobile mode toggle (future: beta)
- ❌ Covenant UI (future: beta)

**Status:** ⏳ **IN PROGRESS** (50% complete)

**Estimated effort:** 2-3 weeks for alpha-level Lightning + Stateless UI

---

### Definition: v3.0 GUI Complete (Beta Target)

**Criteria:**
- ✅ All alpha features
- ✅ Mobile mode toggle + resource monitoring
- ✅ Covenant transaction creation UI
- ✅ PSBT support (import/export/sign)
- ✅ Coin control (manual UTXO selection)
- ✅ RBF / CPFP support
- ✅ Lightning invoice scanner (QR code)
- ✅ Lightning routing graph visualization

**Status:** ⏳ **FUTURE** (estimated 4-6 weeks)

---

### Definition: v3.0 GUI Complete (Final Target)

**Criteria:**
- ✅ All beta features
- ✅ Hardware wallet support (Trezor, Ledger)
- ✅ Multi-sig wallet creation wizard
- ✅ Contract deployment UI (if contracts enabled)
- ✅ Taproot address generation
- ✅ CoinJoin support (optional privacy feature)
- ✅ Full Lightning routing (path finding, rebalancing)
- ✅ Mobile app (iOS/Android) with same GUI
- ✅ Localization (i18n support)

**Status:** ⏳ **LONG-TERM** (estimated 3-6 months)

---

## Missing RPC Bindings (Exact List)

**Add these to `gui/src/rpcclient.h`:**

```cpp
// ═══════════════════════════════════════════════════════════════
// PHASE 8: STATELESS VALIDATION (Utreexo)
// ═══════════════════════════════════════════════════════════════
void getUtreexoState();
void getUtreexoAccumulator();
void verifyUtreexoProof(const QString& proof);
void getProofCacheStats();
void toggleStatelessMode(bool enabled);

// ═══════════════════════════════════════════════════════════════
// PHASE 9: PROOF NETWORK
// ═══════════════════════════════════════════════════════════════
void getProofStats();
void getProofNetworkStats();
void getProofCompressionStats();
void getProofGossipStats();

// ═══════════════════════════════════════════════════════════════
// PHASE 11: LIGHTNING NETWORK
// ═══════════════════════════════════════════════════════════════
void lightningOpenChannel(const QString& nodeId, quint64 amount);
void lightningCloseChannel(const QString& channelId);
void lightningListChannels();
void lightningSendPayment(const QString& invoice, quint64 amountMsat);
void lightningCreateInvoice(quint64 amountMsat, const QString& description);
void lightningPayInvoice(const QString& invoice);
void lightningGetBalance();
void lightningWatchtowerStatus();
void lightningListInvoices();
void lightningGetInfo();

// ═══════════════════════════════════════════════════════════════
// PHASE 12: MOBILE NODE
// ═══════════════════════════════════════════════════════════════
void getMobileNodeStatus();
void toggleBurstMode(bool enabled);
void getResourceStats();

// ═══════════════════════════════════════════════════════════════
// ADVANCED WALLET FEATURES
// ═══════════════════════════════════════════════════════════════
void createPSBT(const QJsonArray& inputs, const QJsonObject& outputs);
void signPSBT(const QString& psbt);
void finalizePSBT(const QString& psbt);
void analyzePSBT(const QString& psbt);

void lockUTXO(const QString& txid, quint32 vout);
void unlockUTXO(const QString& txid, quint32 vout);
void listLockedUTXOs();

void bumpFee(const QString& txid, double newFeeRate); // RBF
void cpfpTransaction(const QString& parentTxid, double feeRate); // CPFP

// ═══════════════════════════════════════════════════════════════
// COVENANT SUPPORT
// ═══════════════════════════════════════════════════════════════
void createCovenantTx(const QString& type, const QJsonObject& params);
void listCovenants();
void monitorVault(const QString& vaultAddress);

// ═══════════════════════════════════════════════════════════════
// TAPROOT
// ═══════════════════════════════════════════════════════════════
void createTaprootAddress();
void signTaprootMessage(const QString& message);
void verifyTaprootMessage(const QString& message, const QString& signature);
```

**Total missing bindings:** ~35 methods

---

## Conclusion

**Current GUI Status:** ✅ **PRODUCTION-READY** for v2.x feature set

**Gap for v3.0-alpha1:** Protocol v3.0 features not yet exposed in GUI

**Recommendation:**

1. **Ship v3.0.0-alpha1 with existing GUI** (label as "v2.x feature set")
2. Add v3.0 GUI features in **v3.0.0-beta1** (Stateless + Lightning)
3. Complete GUI in **v3.0.0 final** (Mobile + Covenant + advanced features)

**Rationale:**
- Alpha releases focus on **protocol completeness** (done)
- GUI lags protocol by design (standard practice)
- Bitcoin Core, Ethereum, etc. all ship new consensus without full GUI support

**Next Step:** Design v3.0 GUI roadmap (see separate document)

---

**Document Date:** 2026-01-11
**Author:** Claude Code
**Status:** Audit complete, ready for roadmap
