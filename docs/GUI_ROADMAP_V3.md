# DineroCoin GUI Roadmap - No Scope Creep

**Date:** 2026-01-11
**Principle:** Ship incrementally, avoid feature creep
**Target:** v3.0.0-alpha1 → v3.0.0-beta1 → v3.0.0 final

---

## Guiding Principles

### 1. Protocol First, GUI Second
- ✅ Protocol v3.0 complete (Phases 8-12)
- ⏳ GUI lags by design (standard practice)
- 🎯 Expose v3.0 features incrementally

### 2. No Scope Creep
- ❌ No "nice-to-have" features
- ❌ No experimental widgets
- ✅ Only production-critical bindings

### 3. Incremental Delivery
- Alpha: Ship existing GUI (v2.x feature set)
- Beta: Add Stateless + Lightning basics
- Final: Complete Mobile + Covenant + advanced features

---

## Current State (v3.0.0-alpha1)

**GUI Feature Set:** v2.x (complete)

**What works:**
- ✅ Send/receive/balance
- ✅ HD wallet (encrypt, lock, derive)
- ✅ Mining (external miner)
- ✅ Transaction history
- ✅ Network monitoring

**What's missing:**
- ❌ Stateless mode toggle
- ❌ Lightning operations
- ❌ Proof network stats
- ❌ Mobile mode control

**Architectural Note:**
- Lightning is a decoupled subsystem, not part of consensus-critical dinerod core
- Missing Lightning GUI bindings cannot affect block validation, chain safety, or on-chain wallet funds

**Decision:** ✅ **SHIP AS-IS for alpha**

**Rationale:**
- Alpha tests **protocol completeness** (done)
- GUI completeness not required for alpha
- Matches industry practice (Bitcoin Core, geth, etc.)

---

## Phase 1: v3.0.0-beta1 (Essential v3.0 Features)

**Timeline:** 2-3 weeks after alpha
**Focus:** Stateless + Lightning basics only

### Feature 1.1: Stateless Mode Control

**Priority:** P0 (blocking beta)

**Add to Overview Tab:**
```
┌─ Stateless Mode ──────────────────────┐
│ ⚫ Stateful (Full Node)              │
│   Storage: 2.3 GB UTXO database       │
│                                        │
│ ○ Stateless (Utreexo)                │
│   Storage: 150 MB headers only        │
│   Proofs: Fetched from network        │
│                                        │
│ [Toggle Mode]                          │
└────────────────────────────────────────┘
```

**RPC Bindings:**
```cpp
void getUtreexoState();       // Get current mode, accumulator state
void toggleStatelessMode();   // Switch stateful ↔ stateless
```

**Effort:** 1 day (UI) + 1 day (RPC wiring) = 2 days

---

### Feature 1.2: Proof Network Stats

**Priority:** P1 (nice-to-have for beta)

**Add to Overview Tab:**
```
┌─ Proof Network ────────────────────────┐
│ Cache: 12/16 MB (75%)                  │
│ Hit rate: 92%                           │
│ Requests: 1,234 (last hour)            │
│ Gossip: 56 announcements/min           │
│ Compression: 45% average               │
└────────────────────────────────────────┘
```

**RPC Bindings:**
```cpp
void getProofCacheStats();    // Cache size, hit rate, evictions
void getProofNetworkStats();  // Requests, gossip, compression
```

**Effort:** 2 days (UI + RPC + polling)

---

### Feature 1.3: Lightning Basics

**Priority:** P0 (blocking beta)

**Add New Tab: "Lightning"**

```
┌─ Lightning Network ────────────────────┐
│                                        │
│ Balance: 0.05 DIN (50,000 una)      │
│ Channels: 2 open, 0 pending           │
│                                        │
│ ┌─ Channels ─────────────────────────┐│
│ │ Node ID        | Capacity | State ││
│ │ 02abc...def    | 0.1 DIN  | Open  ││
│ │ 03xyz...123    | 0.2 DIN  | Open  ││
│ └────────────────────────────────────┘│
│                                        │
│ [Open Channel] [Close Channel]        │
│ [Send Payment] [Create Invoice]       │
│                                        │
└────────────────────────────────────────┘
```

**RPC Bindings:**
```cpp
void lightningGetBalance();
void lightningListChannels();
void lightningOpenChannel(QString nodeId, quint64 amount);
void lightningCloseChannel(QString channelId);
void lightningSendPayment(QString invoice);
void lightningCreateInvoice(quint64 amount, QString desc);
```

**Effort:** 5 days (UI + RPC + dialogs + validation)

---

### Phase 1 Total Effort: ~10 days

**Deliverables for Beta:**
- ✅ Stateless mode toggle
- ✅ Proof cache stats display
- ✅ Lightning channel management
- ✅ Lightning send/receive

**NOT included in beta:**
- ❌ Lightning routing graph
- ❌ Lightning autopilot
- ❌ Mobile mode toggle
- ❌ Covenant UI
- ❌ PSBT support

---

## Phase 2: v3.0.0-rc1 (Polish & Advanced Features)

**Timeline:** 2-3 weeks after beta
**Focus:** Mobile mode + advanced wallet features

### Feature 2.1: Mobile Node Mode

**Priority:** P1 (nice-to-have for rc)

**Add to Settings:**
```
┌─ Mobile Node Settings ─────────────────┐
│                                         │
│ ☑ Enable Mobile Mode                   │
│   • Burst sync (30s windows)           │
│   • Battery-friendly (1-3% daily)      │
│   • iOS background compliant           │
│                                         │
│ Memory Budget: 16 MB (mobile profile)  │
│ Cache: 8/16 MB (50%)                    │
│ Last sync: 2 minutes ago               │
│                                         │
│ ☑ Auto-sync when charging              │
│ ☑ Wi-Fi only (no cellular data)       │
│                                         │
└─────────────────────────────────────────┘
```

**RPC Bindings:**
```cpp
void getMobileNodeStatus();  // Burst mode, memory, last sync
void toggleBurstMode();      // Enable/disable burst
void getResourceStats();     // Battery, memory, data usage
```

**Effort:** 3 days (UI + RPC + settings persistence)

---

### Feature 2.2: Coin Control

**Priority:** P2 (optional for rc)

**Add to Send Tab:**
```
┌─ Advanced ─────────────────────────────┐
│ ☑ Manual coin selection                │
│                                         │
│ ┌─ UTXOs ────────────────────────────┐ │
│ │ ☑ txid:0  0.5 DIN  (100 confirms) │ │
│ │ ☐ txid:1  0.3 DIN  (50 confirms)  │ │
│ │ ☑ txid:2  0.2 DIN  (10 confirms)  │ │
│ └────────────────────────────────────┘ │
│                                         │
│ Selected: 0.7 DIN (2 UTXOs)            │
│                                         │
└─────────────────────────────────────────┘
```

**RPC Bindings:**
```cpp
void lockUTXO(QString txid, quint32 vout);
void unlockUTXO(QString txid, quint32 vout);
void listLockedUTXOs();
```

**Effort:** 2 days (UI + RPC)

---

### Feature 2.3: RBF / CPFP

**Priority:** P2 (optional for rc)

**Add to Transactions Tab:**
```
Right-click menu on pending transaction:
┌────────────────────────────────┐
│ ● Bump Fee (RBF)              │  ← If tx has RBF flag
│ ● Child-Pays-For-Parent       │  ← Always available
│ ○ Cancel Transaction           │  ← If RBF enabled
│ ─────────────────────────────  │
│ Copy TXID                      │
│ View on Explorer               │
└────────────────────────────────┘
```

**RPC Bindings:**
```cpp
void bumpFee(QString txid, double newFeeRate);
void cpfpTransaction(QString parentTxid, double feeRate);
void cancelTransaction(QString txid);  // RBF to self
```

**Effort:** 2 days (UI + RPC + validation)

---

### Phase 2 Total Effort: ~7 days

**Deliverables for RC:**
- ✅ Mobile mode toggle + status
- ✅ Coin control (UTXO selection)
- ✅ RBF / CPFP support

**NOT included in rc:**
- ❌ PSBT import/export
- ❌ Multi-sig wallet wizard
- ❌ Hardware wallet support
- ❌ Covenant UI

---

## Phase 3: v3.0.0 Final (Nice-to-Haves)

**Timeline:** After rc, when needed
**Focus:** Only if users request these features

### Feature 3.1: PSBT Support

**Priority:** P3 (future, if requested)

**Add to File Menu:**
```
File
├─ Import PSBT...
├─ Export PSBT...
└─ Sign PSBT...
```

**RPC Bindings:**
```cpp
void createPSBT(QJsonArray inputs, QJsonObject outputs);
void signPSBT(QString psbt);
void finalizePSBT(QString psbt);
void analyzePSBT(QString psbt);
```

**Effort:** 3 days (UI + file dialogs + RPC + validation)

---

### Feature 3.2: Multi-Sig Wallet

**Priority:** P3 (future, if requested)

**Add Wizard:**
```
┌─ Create Multi-Sig Wallet ──────────────┐
│                                         │
│ Signers: [2] of [3] (M-of-N)          │
│                                         │
│ Pubkey 1: xpub6...  [Import]          │
│ Pubkey 2: xpub6...  [Import]          │
│ Pubkey 3: xpub6...  [Import]          │
│                                         │
│ Address Type:                           │
│ ● P2WSH (native SegWit)                │
│ ○ P2SH-P2WSH (wrapped SegWit)          │
│                                         │
│ [Create Wallet]                         │
└─────────────────────────────────────────┘
```

**RPC Bindings:**
```cpp
void createMultiSigWallet(int m, int n, QStringList pubkeys);
void importMultiSigWallet(QString descriptor);
void signMultiSigTx(QString psbt);
```

**Effort:** 5 days (wizard + UI + RPC + validation)

---

### Feature 3.3: Hardware Wallet

**Priority:** P3 (future, if requested)

**Add to Wallet Menu:**
```
Wallet
├─ Connect Hardware Wallet
│  ├─ Trezor
│  ├─ Ledger
│  └─ ColdCard
└─ Sign with Hardware Wallet
```

**RPC Bindings:**
```cpp
void enumerateHardwareWallets();
void hwwGetPubKey(QString device);
void hwwSignPSBT(QString device, QString psbt);
void hwwDisplayAddress(QString device, QString path);
```

**Effort:** 7 days (device integration + UI + RPC + testing)

---

### Phase 3 Total Effort: ~15 days (optional)

**Deliverables for Final (if requested):**
- ⚠️ PSBT import/export/sign
- ⚠️ Multi-sig wallet wizard
- ⚠️ Hardware wallet support

---

## What We Will NOT Build

### Explicitly Out of Scope

❌ **Lightning routing graph visualization**
   - Reason: Complex, low user value, high maintenance
   - Alternative: Users can use external Lightning explorers

❌ **Lightning autopilot**
   - Reason: Algorithmic channel management is risky
   - Alternative: Manual channel management only

❌ **Contract deployment UI**
   - Reason: Contracts are Layer 2, not core wallet
   - Alternative: CLI or web-based tools

❌ **CoinJoin wizard**
   - Reason: Privacy features require coordination servers
   - Alternative: Standalone tools (Wasabi, Samourai style)

❌ **Taproot key path spending wizard**
   - Reason: Taproot is advanced, low adoption initially
   - Alternative: CLI for advanced users

❌ **Block explorer (full)**
   - Reason: Maintenance burden, external explorers exist
   - Alternative: Basic block lookup only (already exists)

❌ **Miner profitability calculator**
   - Reason: Requires market data APIs, high maintenance
   - Alternative: Users can use external calculators

❌ **Portfolio tracker**
   - Reason: Not a wallet responsibility
   - Alternative: Standalone tools

❌ **Tax reporting**
   - Reason: Jurisdiction-specific, legal liability
   - Alternative: Export CSV for external tools

❌ **Built-in exchange**
   - Reason: Regulatory risk, security risk
   - Alternative: Users can use external exchanges

---

## Effort Summary

### Alpha → Beta: ~10 days
- Stateless mode toggle (2 days)
- Proof stats display (2 days)
- Lightning basics (6 days)

### Beta → RC: ~7 days
- Mobile mode toggle (3 days)
- Coin control (2 days)
- RBF/CPFP (2 days)

### RC → Final: ~15 days (optional)
- PSBT support (3 days)
- Multi-sig wizard (5 days)
- Hardware wallet (7 days)

**Total Effort: 32 days (~6 weeks)**

**With one developer:**
- Alpha → Beta: 2 weeks
- Beta → RC: 1 week
- RC → Final: 3 weeks (if requested)

---

## Implementation Order (Priority)

### P0 (Blocking Beta) - MUST HAVE
1. Stateless mode toggle (2 days)
2. Lightning channel management (5 days)
3. Lightning send/receive (included in #2)

**Total P0: 7 days**

### P1 (Nice-to-Have for Beta) - SHOULD HAVE
4. Proof network stats (2 days)
5. Mobile mode toggle (3 days)

**Total P1: 5 days**

### P2 (Nice-to-Have for RC) - COULD HAVE
6. Coin control (2 days)
7. RBF/CPFP (2 days)

**Total P2: 4 days**

### P3 (Future) - WON'T HAVE (unless requested)
8. PSBT support (3 days)
9. Multi-sig wizard (5 days)
10. Hardware wallet (7 days)

**Total P3: 15 days (deferred)**

---

## Definition of Done (Per Feature)

**For each feature to be considered "done":**

1. ✅ **RPC binding added** to `gui/src/rpcclient.h`
2. ✅ **RPC implementation** in `gui/src/rpcclient.cpp`
3. ✅ **UI widget created** (Qt Designer or code)
4. ✅ **Signal/slot connections** wired
5. ✅ **Error handling** implemented (show errors to user)
6. ✅ **Input validation** (prevent invalid inputs)
7. ✅ **Local testing** (manual smoke test)
8. ✅ **Documentation** (one-line comment in code)

**NOT required:**
- ❌ Automated UI tests (too fragile)
- ❌ Localization (future work)
- ❌ Accessibility (future work)
- ❌ Mobile app (separate project)

---

## Success Metrics

**How to know if GUI is "complete enough":**

### Alpha Success Criteria
- ✅ All v2.x features work
- ✅ Zero fake data
- ✅ No crashes on basic operations

**Status:** ✅ **MET** (current GUI)

### Beta Success Criteria
- ✅ Stateless mode can be toggled
- ✅ Lightning channels can be opened/closed
- ✅ Lightning payments can be sent/received
- ⚠️ Proof stats visible (optional)

**Status:** ⏳ **IN PROGRESS**

### RC Success Criteria
- ✅ All beta features stable
- ✅ Mobile mode toggle works
- ⚠️ Coin control available (optional)
- ⚠️ RBF/CPFP available (optional)

**Status:** ⏳ **FUTURE**

### Final Success Criteria
- ✅ All rc features stable
- ✅ No user-reported critical bugs
- ⚠️ PSBT support (only if requested)
- ⚠️ Multi-sig support (only if requested)

**Status:** ⏳ **FUTURE**

---

## Risks & Mitigation

### Risk 1: Scope Creep
**Likelihood:** High
**Impact:** High (delays release)

**Mitigation:**
- ✅ Strict "P0/P1/P2/P3" prioritization
- ✅ Explicit "out of scope" list
- ✅ One feature at a time

### Risk 2: RPC API Changes
**Likelihood:** Medium
**Impact:** Medium (breaks GUI)

**Mitigation:**
- ✅ Version RPC methods (e.g., `lightning.v1.openchannel`)
- ✅ Test with actual daemon (not mocks)
- ✅ Handle errors gracefully

### Risk 3: User Confusion (Stateless Mode)
**Likelihood:** Medium
**Impact:** Low (support burden)

**Mitigation:**
- ✅ Clear mode descriptions in UI
- ✅ Warning dialog before switching
- ✅ Default to stateful mode (safer)

### Risk 4: Lightning Complexity
**Likelihood:** High
**Impact:** Medium (incomplete feature)

**Mitigation:**
- ✅ Start with basics only (open/close/send/receive)
- ✅ No routing, no autopilot, no graph
- ✅ Defer advanced features to post-v3.0

---

## Conclusion

**GUI Strategy for v3.0:**

1. ✅ **Alpha:** Ship existing GUI (v2.x features) - READY NOW
2. ⏳ **Beta:** Add Stateless + Lightning basics - 2 weeks effort
3. ⏳ **RC:** Add Mobile mode + coin control - 1 week effort
4. ⏳ **Final:** Add optional features IF requested - 3 weeks effort

**Total Timeline:** 6 weeks (beta → final), but **alpha ships immediately**.

**Next Action:** Implement P0 features (Stateless + Lightning) after alpha release.

---

**Document Date:** 2026-01-11
**Author:** Claude Code
**Status:** Ready for implementation
**Commitment:** NO SCOPE CREEP - stick to P0/P1 only
