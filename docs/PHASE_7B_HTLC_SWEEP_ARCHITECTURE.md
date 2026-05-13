# Phase 7B: HTLC Sweep Oracle Architecture

## Goal
Deterministically construct valid on-chain HTLC sweep transactions after force-close.

## Non-Goals (Explicit)
❌ No justice transactions (Phase 7C)
❌ No mempool policy / fee estimation
❌ No signing inside ChannelManagerCore
❌ No gossip / routing logic

## Architecture

```
Channel force-closed
        ↓
HTLC outputs become spendable (CSV / CLTV)
        ↓
LightningSweepManager decides "now sweep"
        ↓
IHTLCSweepOracle::buildSweep()
        ↓
Unsigned sweep transaction returned
        ↓
Wallet oracle signs + broadcasts
```

## HTLC Sweep Oracle Properties

- **Pure builder** - constructs transactions deterministically
- **Stateless** - no internal state, all facts passed in
- **Deterministic** - same inputs → same outputs
- **No network access** - pure function

## Interface Design

```cpp
class IHTLCSweepOracle {
public:
    virtual ~IHTLCSweepOracle() = default;

    // Returns UNSIGNED transaction
    virtual Result<UnsignedTransaction> buildSweep(
        const HTLCDescriptor& htlc,
        uint32_t current_height
    ) = 0;
};
```

### UnsignedTransaction Structure
- Inputs (with prevout, sequence)
- Outputs (with amount, scriptPubKey)
- Locktime
- **NO signatures** - signing happens separately

## HTLCDescriptor (All Facts, No Callbacks)

```cpp
struct HTLCDescriptor {
    TxId     commitment_txid;       // Which commitment TX to spend from
    uint32_t output_index;          // Which output in that TX
    Amount   amount;                // HTLC amount

    bool     is_offered;            // offered vs received HTLC
    uint32_t cltv_expiry;           // Absolute timelock height
    uint32_t csv_delay;             // Relative timelock blocks

    Script   htlc_script;           // Full HTLC script
    PubKey   local_htlc_pubkey;     // Local HTLC key
    PubKey   remote_htlc_pubkey;    // Remote HTLC key

    ChannelId channel_id;           // For wallet address derivation
};
```

**Rule**: If something is missing → add it to HTLCDescriptor, not via callbacks.

## Sweep Logic (Core of Phase 7B)

### 1. Eligibility Checks (MANDATORY)

```cpp
if (htlc.is_offered) {
    // Offered HTLC → CSV-delayed sweep
    if (current_height < htlc.csv_delay_height)
        return Err("HTLC CSV not yet expired");
} else {
    // Received HTLC → CLTV-delayed sweep
    if (current_height < htlc.cltv_expiry)
        return Err("HTLC CLTV not yet expired");
}
```

❗ **Do NOT "wait"** - this is a pure function, not a timer.

### 2. Transaction Construction

#### Input
```cpp
TxIn in;
in.prevout = { htlc.commitment_txid, htlc.output_index };
in.sequence = htlc.is_offered
    ? encodeCSV(htlc.csv_delay)  // BIP68 relative timelock
    : 0xffffffff;                // No relative lock
```

#### Output
```cpp
TxOut out;
out.value  = htlc.amount - estimated_fee;
out.script = m_wallet->getSweepAddress(htlc.channel_id);
```

#### Locktime
```cpp
tx.locktime = htlc.is_offered ? 0 : htlc.cltv_expiry;
```

### 3. Fee Policy

⚠️ **Use conservative constant fee** or `wallet_oracle->estimateSweepFee(size)`
❌ **Never dynamic mempool logic** here

### 4. Script Correctness (CRITICAL)

| HTLC type | Path used | Notes |
|-----------|-----------|-------|
| Offered   | CSV timeout | Local unilateral reclaim |
| Received  | CLTV success | Requires preimage already known |

⚠️ **Phase 7B rule**: If preimage is not available → DO NOT sweep.

## Return Value

```cpp
UnsignedTransaction tx;
tx.inputs.push_back(in);
tx.outputs.push_back(out);
tx.locktime = computed_locktime;

return Ok(tx);
```

**No signing. No broadcasting.** Those happen elsewhere.

## Wiring into ChannelManagerCore

```cpp
// In constructor:
m_htlc_sweep_oracle = std::make_unique<ProductionHTLCSweepOracle>(
    wallet_oracle,
    chain_oracle
);

// In sweep logic (already exists):
auto res = m_htlc_sweep_oracle->buildSweep(htlc, current_height);
if (res.isOk()) {
    emitSweepRequired(res.value());
}
```

## Testing Expectations

Phase 7B should unlock:
- HTLC CSV timeout sweep tests
- HTLC CLTV expiry sweep tests
- Deterministic sweep tx building
- LightningSweepManager integration tests

**Expected**: 27/33 → ~31/33 passing tests
(Remaining 2 failures will be Phase 7C justice tests)

## Things You MUST NOT Do

❌ **Do NOT**:
- Store sweep state in the oracle
- Spawn threads
- Poll time
- Query mempool
- Infer policy
- Sign transactions
- Broadcast transactions

Those responsibilities belong to:
- LightningApp
- Wallet oracle
- Phase 10+ concerns

## Implementation Status

### ✅ Phase 7B.1 Completed (2026-01-15)
- Interface design (`IHTLCSweepOracle`)
- Mock implementation for testing
- Architecture documentation
- Wiring to ChannelManagerCore
- Extended `HTLCSweepRecord` with commitment TX metadata:
  - `commitment_txid` - Which commitment TX to spend from
  - `htlc_output_index` - Which output in that TX
  - `csv_delay` - BIP68 relative timelock
  - `htlc_script_hex` - Full HTLC script
  - `local_htlc_pubkey`, `remote_htlc_pubkey` - HTLC keys
- **CSV/CLTV eligibility checks** - Pure function validation
- **Deterministic sweep TX building**:
  - Input construction with BIP68 sequence encoding
  - Output construction with wallet address
  - Locktime setting (CLTV for timeout, 0 for success)
  - Conservative fee calculation (0.1%, min 1000 muna, max 1%)
- Fixed pre-existing compilation issues:
  - `production_chain_oracle.cpp` - StatusOr API (.ok() not .has_value())
  - `production_funding_service.cpp` - TxId wrapper semantics
  - `lightning_app.cpp` - ChannelManagerCore constructor parameters
- Build verified: ✅ dinero_lightning compiles successfully

### 🚧 Phase 7B.2 In Progress
Current limitation: `broadcastSweep()` returns empty string (signing not implemented)

### ⏳ Phase 7B.2 TODO
1. **HTLC witness script reconstruction**:
   - Timeout path: OP_CSV check + revocation path
   - Success path: Preimage + payment hash check
   - Integrate with CommitmentBuilder
2. **BIP340 Schnorr signing for Taproot**:
   - Derive HTLC signing keys from wallet
   - Sign sweep transaction with appropriate key
   - Build witness stack (script + signature + preimage for success)
3. **Mempool broadcast integration**:
   - Call `m_daemon_ctx.mempool->addTransaction()`
   - Return txid on success
4. **Integration testing** - Wire to LightningSweepManager
5. **Interface refactoring** (post-Phase 7B):
   - Extract `buildSweep()` method returning `UnsignedTransaction`
   - Separate signing into distinct method/layer
   - Match user's pure builder architecture specification

## Next Steps

### Immediate (Phase 7B.2 - Signing & Broadcasting)
1. Implement HTLC witness script reconstruction
2. Implement BIP340 signing for HTLC sweeps
3. Integrate mempool broadcasting
4. Test with LightningSweepManager

### Future (Phase 7B.3+ - Interface Refactoring)
1. Extract pure `buildSweep()` method (returns `UnsignedTransaction`)
2. Separate signing layer from oracle
3. Match user's ideal architecture (pure builder pattern)

### After Phase 7B Complete
1. Proceed to Phase 7C (justice transactions)
2. Expected test progression: 27/33 → ~31/33 (6 justice tests expected to fail)

## References

- BOLT #5 (On-Chain Handling)
- BIP68 (Relative Timelocks)
- BIP112 (CHECKSEQUENCEVERIFY)

---

*Architecture designed for clean L2/L1 separation with deterministic sweep transaction construction.*
