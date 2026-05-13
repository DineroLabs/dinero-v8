# Phase 7C: Justice Transaction Oracle Architecture

## Goal
Deterministically construct and broadcast justice (penalty) transactions to punish counterparty breach (broadcasting revoked commitment transactions).

## Non-Goals (Explicit)
❌ No breach detection logic (handled by ChannelManagerCore)
❌ No watchtower client logic (separate component)
❌ No revocation secret storage policy (handled by wallet)
❌ No CSV delay policy decisions (derived from channel params)

## Architecture

```
Counterparty broadcasts revoked commitment
        ↓
ChannelManagerCore detects breach (txid != latest commitment)
        ↓
Creates JusticeRecord with revocation_secret
        ↓
Waits for CSV delay (to_self_delay blocks)
        ↓
IJusticeOracle::buildJusticeTransaction()
        ↓
Signed justice transaction returned
        ↓
IJusticeOracle::broadcastJusticeTransaction()
        ↓
Claims all channel funds to local wallet
```

## Justice Oracle Properties

- **Deterministic** - same inputs → same justice transaction
- **Complete** - builds AND signs (unlike HTLC sweep oracle)
- **Stateless** - no internal state, all facts passed via JusticeRecord
- **Punitive** - claims ALL outputs from revoked commitment (penalty)

## Key Difference from Phase 7B (HTLC Sweep)

| Aspect | HTLC Sweep (7B) | Justice (7C) |
|--------|-----------------|--------------|
| **Purpose** | Claim our own HTLCs after force-close | Punish counterparty breach |
| **Trigger** | CSV/CLTV expiry | Revoked commitment on-chain |
| **Signing** | Separate layer (Phase 7B.2) | Integrated in oracle |
| **Amount** | Single HTLC value | ALL channel funds (penalty) |
| **Key** | HTLC key | Revocation key |
| **Urgency** | Non-urgent (timelock protection) | CRITICAL (race against counterparty) |

**Rationale for integrated signing**: Justice transactions are time-critical. Unlike HTLC sweeps (which have timelock protection), justice transactions must be broadcast IMMEDIATELY to prevent counterparty from claiming funds. Keeping signing integrated ensures atomic build+sign+broadcast.

## Interface Design

```cpp
class IJusticeOracle {
public:
    virtual ~IJusticeOracle() = default;

    // Build AND sign justice transaction (time-critical!)
    virtual Result<JusticeTx> buildJusticeTransaction(
        const JusticeRecord& justice,
        const ChannelRecord& channel
    ) = 0;

    // Broadcast to network
    virtual Status broadcastJusticeTransaction(
        const JusticeTx& tx
    ) = 0;

    // Check confirmation
    virtual std::optional<uint64_t> getJusticeConfirmationHeight(
        const std::string& justice_txid
    ) const = 0;
};

struct JusticeTx {
    std::string tx_hex;        // Fully signed transaction (hex)
    std::string txid;          // Transaction ID (hex)
    uint64_t total_value;      // Total value being claimed (una)
};
```

## Justice Transaction Construction

### Input: Revoked Commitment Outputs

Justice transactions spend **ALL outputs** from the revoked commitment transaction that we can claim:

1. **to_local output** (counterparty's balance with CSV delay)
   - **Script-path**: Revocation branch (we have revocation secret!)
   - **Witness**: `<revocation_signature> <revocation_script>`
   - **CSV delay**: Must wait to_self_delay blocks after breach

2. **to_remote output** (our balance - if exists)
   - **Key-path**: Simple Taproot spend (our key)
   - **Witness**: `<schnorr_signature>`
   - **No delay**: Can spend immediately

3. **HTLC outputs** (outputs 2+) - **Phase 7D Complete ✅**
   - **Script-path**: Revocation branch (same as to_local)
   - **Witness**: `<revocation_signature> <htlc_revocation_script>`
   - **CSV delay**: Same as to_self_delay (HTLC outputs inherit channel CSV)
   - **Detection**: All outputs from index 2 onwards are HTLC outputs
   - **Claiming**: Revocation secret works for ALL outputs in revoked commitment

### Output: Sweep to Wallet

Single output sweeping all claimed funds:

```cpp
TxOut output;
output.value = (to_local_value + to_remote_value + htlc_values) - fee;
output.scriptPubKey = wallet_address;  // Fresh wallet address
```

### Sequence and Locktime

```cpp
// Inputs must satisfy CSV delay for to_local and HTLCs
for (size_t i = 0; i < tx.vin.size(); i++) {
    if (input_has_csv_delay[i]) {
        tx.vin[i].sequence = encodeCSV(to_self_delay);  // BIP68
    } else {
        tx.vin[i].sequence = 0xfffffffe;  // Standard non-RBF
    }
}

tx.lockTime = 0;  // No absolute timelock (CSV is relative)
```

## Revocation Key Derivation

Using `CommitmentBuilder::deriveRevocationPrivkey()`:

```cpp
// Inputs (from JusticeRecord and ChannelRecord):
std::vector<uint8_t> revocation_basepoint_secret = wallet->getRevocationBasepointSecret();
std::vector<uint8_t> per_commitment_secret = hexToBytes(justice.revocation_secret);

// Derive revocation private key
auto revocation_privkey_result = m_commitment_builder.deriveRevocationPrivkey(
    revocation_basepoint_secret,
    per_commitment_secret
);

// Sign with revocation key
std::vector<uint8_t> revocation_signature = TaprootTxSigner::SignSchnorr(
    sighash,
    revocation_privkey_result.unwrap()
);
```

## Witness Stack Construction

### to_local Output (Revocation Branch)

**Script** (from CommitmentBuilder::buildRevocationScript):
```
OP_IF
    <revocation_pubkey> OP_CHECKSIG
OP_ELSE
    <to_self_delay> OP_CSV OP_DROP <delayed_pubkey> OP_CHECKSIG
OP_ENDIF
```

**Witness Stack** (revocation branch = TRUE):
```
<revocation_signature>
<0x01>                    // TRUE - take revocation branch
<revocation_script>
<control_block>           // Taproot control block
```

### to_remote Output (Key-Path)

**Witness Stack**:
```
<schnorr_signature>       // Simple key-path spend
```

## Fee Policy

Justice transactions are **time-critical** and should prioritize confirmation:

```cpp
uint64_t calculateJusticeFee(uint64_t total_input_value) const {
    // Aggressive fee: 1% of total value (10x more than HTLC sweep)
    uint64_t fee = total_input_value / 100;

    // Minimum: 10000 muna (0.01 una) - ensure fast confirmation
    if (fee < 10000) {
        fee = 10000;
    }

    // Maximum: 5% of total value (safety cap)
    uint64_t max_fee = total_input_value / 20;
    if (fee > max_fee) {
        fee = max_fee;
    }

    return fee;
}
```

**Rationale**: Justice transactions race against counterparty. Higher fees ensure fast confirmation.

## CSV Eligibility Check

Unlike HTLC sweeps (pure function), justice transactions are built AFTER CSV delay satisfied:

```cpp
// In ChannelManagerCore::getReadyJusticeActions()
if (current_height < justice.earliest_justice_height) {
    continue;  // Not yet eligible
}

// CSV delay satisfied → build justice transaction
auto justice_tx = m_justice_oracle->buildJusticeTransaction(justice, channel);
```

**Note**: Oracle doesn't check eligibility - assumes caller (ChannelManagerCore) already verified CSV delay.

## Implementation Flow

### 1. Parse Revocation Secret

```cpp
std::vector<uint8_t> per_commitment_secret = hexToBytes(justice.revocation_secret);
if (per_commitment_secret.size() != 32) {
    return Result<JusticeTx>::Err("Invalid revocation secret");
}
```

### 2. Query Revoked Commitment On-Chain

```cpp
// Get commitment transaction from chain
auto tx_result = m_daemon_ctx.chainstate->getTransaction(justice.commitment_txid);
if (!tx_result) {
    return Result<JusticeTx>::Err("Revoked commitment not found on-chain");
}

Transaction revoked_commit = tx_result.value();
```

### 3. Identify Claimable Outputs

```cpp
std::vector<ClaimableOutput> claimable_outputs;

// to_local output (index 0) - counterparty's balance with CSV delay
if (revoked_commit.vout.size() > 0) {
    ClaimableOutput to_local;
    to_local.txid = justice.commitment_txid;
    to_local.vout = 0;
    to_local.amount = revoked_commit.vout[0].value;
    to_local.needs_revocation_key = true;
    to_local.csv_delay = channel.to_self_delay;
    claimable_outputs.push_back(to_local);
}

// to_remote output (index 1) - our balance (if exists)
if (revoked_commit.vout.size() > 1) {
    ClaimableOutput to_remote;
    to_remote.txid = justice.commitment_txid;
    to_remote.vout = 1;
    to_remote.amount = revoked_commit.vout[1].value;
    to_remote.needs_revocation_key = false;  // Our key
    to_remote.csv_delay = 0;
    claimable_outputs.push_back(to_remote);
}

// Phase 7D: HTLC outputs (outputs 2+) - IMPLEMENTED ✅
for (size_t i = 2; i < revoked_commit.vout.size(); i++) {
    ClaimableOutput htlc_output;
    htlc_output.txid = justice.commitment_txid;
    htlc_output.vout = i;
    htlc_output.amount = revoked_commit.vout[i].value;
    htlc_output.needs_revocation_key = true;  // HTLC revocation branch
    htlc_output.csv_delay = channel.to_self_delay;
    claimable_outputs.push_back(htlc_output);
}
```

### 4. Build Transaction Inputs

```cpp
for (const auto& output : claimable_outputs) {
    TxInput input;
    input.prevout.txid = TxId(uint256::FromHexUnsafe(output.txid));
    input.prevout.vout = output.vout;

    if (output.csv_delay > 0) {
        input.sequence = encodeCSV(output.csv_delay);  // BIP68
    } else {
        input.sequence = 0xfffffffe;
    }

    tx.vin.push_back(input);
}
```

### 5. Build Transaction Output

```cpp
uint64_t total_input_value = 0;
for (const auto& output : claimable_outputs) {
    total_input_value += output.amount;
}

uint64_t fee = calculateJusticeFee(total_input_value);
uint64_t output_value = total_input_value - fee;

// Get fresh wallet address
std::string wallet_address = m_wallet_api->getNewAddress();
auto decoded = bech32::Decode(hrp, wallet_address);

TxOutput output;
output.value = output_value;
output.scriptPubKey = buildWitnessScriptPubKey(decoded.witver, decoded.program);
tx.vout.push_back(output);

tx.lockTime = 0;  // No absolute locktime
```

### 6. Sign Transaction

```cpp
// Derive revocation private key
std::vector<uint8_t> revocation_basepoint_secret = m_wallet_api->getRevocationBasepointSecret();
auto revocation_privkey = m_commitment_builder.deriveRevocationPrivkey(
    revocation_basepoint_secret,
    per_commitment_secret
).unwrap();

// Sign each input
for (size_t i = 0; i < tx.vin.size(); i++) {
    if (claimable_outputs[i].needs_revocation_key) {
        // Script-path spend with revocation key
        signRevocationInput(tx, i, claimable_outputs[i], revocation_privkey);
    } else {
        // Key-path spend with our key
        signKeyPathInput(tx, i, claimable_outputs[i]);
    }
}
```

### 7. Return Signed Transaction

```cpp
JusticeTx justice_tx;
justice_tx.tx_hex = tx.SerializeHex();
justice_tx.txid = tx.GetTxid().AsUint256().GetHex();
justice_tx.total_value = output_value;

return Result<JusticeTx>::Ok(justice_tx);
```

## Wiring into ChannelManagerCore

```cpp
// In LightningApp or LightningService initialization:
auto justice_oracle = std::make_shared<ProductionJusticeOracle>(
    wallet_api,
    daemon_ctx,
    commitment_builder
);

auto core = std::make_unique<ChannelManagerCore>(
    chain_oracle,
    wallet_oracle,
    funding_service,
    sweep_oracle,
    justice_oracle,  // ← Wired here
    db,
    node_pubkey,
    time_oracle
);

// In ChannelManagerCore::onNewBlock():
auto ready_justice = getReadyJusticeActions(current_height);
for (const auto& justice : ready_justice) {
    auto channel = m_db->getChannel(justice.channel_id);
    auto tx_result = m_justice_oracle->buildJusticeTransaction(justice, channel.value());

    if (tx_result.isOk()) {
        auto status = m_justice_oracle->broadcastJusticeTransaction(tx_result.unwrap());
        if (status == Status::Ok) {
            updateJusticeStatus(justice.justice_id, JusticeStatus::BROADCAST, tx_result.unwrap().txid, 0);
        }
    }
}
```

## Testing Expectations

Phase 7C should unlock:
- Justice creation on breach detection ✅
- No justice on latest commitment ✅
- CSV delay enforcement ✅
- Justice ready after CSV expiry ✅
- Justice status transitions ✅
- Justice idempotence ✅

**Expected**: 27/33 → 33/33 passing tests (all Phase 7C tests pass)

## Things You MUST NOT Do

❌ **Do NOT**:
- Detect breaches (that's ChannelManagerCore's job)
- Manage revocation secret storage (that's wallet's job)
- Decide CSV delay policy (comes from channel.to_self_delay)
- Retry failed broadcasts (that's LightningApp's job)
- Track justice confirmation internally (stateless oracle)

Those responsibilities belong to:
- ChannelManagerCore (breach detection, policy)
- Wallet (revocation secret storage)
- LightningApp (retry logic)

## Critical Security Requirements

1. **Revocation Secret Protection**: Never log or expose revocation secrets
2. **Atomic Build+Sign**: Must not separate - race condition critical
3. **Fee Priority**: Justice transactions MUST confirm quickly
4. **Script Correctness**: CRITICAL - wrong script = funds lost forever
5. **CSV Enforcement**: MUST respect to_self_delay (cannot claim before CSV)

## Dependencies

**Required Components:**
1. `CommitmentBuilder` - Revocation script building and key derivation
2. `TaprootTxSigner` - Script-path signing (already complete from Phase 7B.2)
3. `IWalletAPI` - Revocation basepoint secret, fresh addresses
4. `DaemonContext` - Chainstate queries, mempool broadcast
5. `bech32::Decode` - Address parsing

**All dependencies exist** - ready to implement!

## References

**Specifications:**
- BOLT #5 (On-Chain Handling) - Justice transaction specification
- BOLT #3 (Commitment Transactions) - Revocation branch structure
- BIP68 (Relative Timelocks) - CSV encoding
- BIP340 (Schnorr Signatures) - Signing algorithm
- BIP341 (Taproot) - Script-path spending

**Codebase:**
- `include/lightning/justice_oracle.h` - Interface definition
- `include/lightning/commitment_builder.h` - Revocation key derivation
- `include/wallet/taproot_tx_signer.h` - Script-path signing infrastructure
- `include/lightning/lightning_db_types.h` - JusticeRecord structure

---

## Implementation Status

### ⏳ Phase 7C TODO
1. Create `ProductionJusticeOracle` header file
2. Implement `buildJusticeTransaction()` - core TX building logic
3. Implement `broadcastJusticeTransaction()` - mempool integration
4. Implement `getJusticeConfirmationHeight()` - chain query
5. Add to CMakeLists.txt
6. Test with ChannelManagerCore
7. Verify 33/33 tests passing

### Expected Complexity
**Medium** - Similar to Phase 7B but with:
- ✅ Script-path signing infrastructure already exists (Phase 7B.2)
- ✅ CommitmentBuilder provides revocation key derivation
- ✅ Interface well-defined with clear requirements
- ⚠️ Critical correctness requirement (security-sensitive)

**Lines of Code**: ~300-400 lines

---

*Architecture designed for time-critical breach remediation with deterministic justice transaction construction.*
