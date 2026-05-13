# Smart Contract Escrow System - Technical Blueprint

## 🎯 Overview

This document describes Dinero's **on-chain smart contract escrow system** - a Bitcoin-style P2SH (Pay-to-Script-Hash) implementation that enables trustless escrow without any central authority.

## 🧩 Core Concept

**Traditional Escrow**: Trust a 3rd party to hold funds
**Smart Contract Escrow**: Trust math and consensus rules

The escrow contract is a **Bitcoin Script** that lives on-chain and enforces rules automatically:

```
IF (release path):
    2 <PK_Buyer> <PK_Seller> <PK_Mediator> 3 OP_CHECKMULTISIG
ELSE (refund path):
    <REFUND_TIME> OP_CHECKLOCKTIMEVERIFY OP_DROP <PK_Buyer> OP_CHECKSIG
ENDIF
```

## 🔐 Security Model

### Three Spending Paths:

1. **Normal Release** (2-of-3 multisig)
   - Buyer + Seller both sign → Funds go to seller
   - Most common path (happy case)

2. **Dispute Resolution** (2-of-3 multisig)
   - Mediator + Buyer → Refund to buyer
   - Mediator + Seller → Release to seller
   - Mediator acts as tiebreaker

3. **Timeout Refund** (1-of-1 + timelock)
   - After `REFUND_TIME` blocks pass
   - Buyer can reclaim funds alone
   - Protection against seller disappearing

## 📐 Contract Structure

### Redeem Script Anatomy:

```assembly
# Branch selector
OP_IF

  # Release path: 2-of-3 multisig
  OP_2                          # Require 2 signatures
  <buyer_pubkey_33_bytes>       # Buyer's compressed public key
  <seller_pubkey_33_bytes>      # Seller's compressed public key
  <mediator_pubkey_33_bytes>    # Mediator's compressed public key
  OP_3                          # Out of 3 total keys
  OP_CHECKMULTISIG              # Validate 2 signatures

OP_ELSE

  # Refund path: timelock + single sig
  <refund_block_height_4_bytes> # e.g., current_height + 2880 (≈6 days)
  OP_CHECKLOCKTIMEVERIFY        # Enforce time lock
  OP_DROP                       # Clean stack
  <buyer_pubkey_33_bytes>       # Buyer's public key
  OP_CHECKSIG                   # Validate buyer's signature

OP_ENDIF
```

### Key Sizes:
- **Public Key**: 33 bytes (compressed secp256k1)
- **Signature**: ~71-73 bytes (DER-encoded)
- **Script**: ~200 bytes total
- **Script Hash**: 20 bytes (RIPEMD160 of SHA256)

## 🔨 Implementation Steps

### Phase 1: Script Builder (`EscrowContractBuilder`)

```cpp
class EscrowContractBuilder {
  // 1. Build redeem script from public keys
  static std::string buildRedeemScript(
      const EscrowKeys& keys,
      uint32_t refund_time
  );

  // 2. Hash script: RIPEMD160(SHA256(redeemScript))
  static std::string hashRedeemScript(
      const std::string& redeem_script
  );

  // 3. Create P2SH address: din1q + script_hash + checksum
  static std::string createP2SHAddress(
      const std::string& script_hash
  );
};
```

### Phase 2: Transaction Builder

```cpp
// A. Lock Transaction (Buyer → Contract)
static std::string createLockTransaction(
    const EscrowContract& contract,
    const std::string& from_address
) {
    Transaction tx;
    tx.addInput(from_utxo);  // Buyer's UTXO
    tx.addOutput(contract.p2sh_address, contract.amount);
    return tx.serialize();
}

// B. Release Transaction (Contract → Seller)
static std::string createReleaseTransaction(
    const EscrowContract& contract,
    const std::string& to_address,
    const std::string& sig_buyer,
    const std::string& sig_seller
) {
    Transaction tx;
    tx.addInput(contract.lock_txid, 0);
    tx.addOutput(to_address, contract.amount - fee);

    // Unlock script: <sig1> <sig2> 1 <redeemScript>
    tx.scriptSig = sig_buyer + sig_seller + "\x01" + contract.redeem_script;
    return tx.serialize();
}

// C. Refund Transaction (Contract → Buyer)
static std::string createRefundTransaction(
    const EscrowContract& contract,
    const std::string& refund_address,
    const std::string& sig_buyer
) {
    Transaction tx;
    tx.nLockTime = contract.refund_time;  // Critical!
    tx.addInput(contract.lock_txid, 0);
    tx.addOutput(refund_address, contract.amount - fee);

    // Unlock script: <sig> 0 <redeemScript>
    tx.scriptSig = sig_buyer + "\x00" + contract.redeem_script;
    return tx.serialize();
}
```

### Phase 3: RPC Methods

```cpp
// contract.createescrow
din::Json contract_createescrow_impl(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    // Parse: buyer_pub, seller_pub, mediator_pub, amount, refund_blocks
    EscrowKeys keys = parseKeys(params);
    double amount = params["amount"].asDouble();
    uint32_t refund_blocks = params["refund_blocks"].asUInt();

    // Build contract
    EscrowContract contract = EscrowContractBuilder::buildContract(
        keys, amount, refund_blocks
    );

    // Return contract details
    result["contract_id"] = contract.contract_id;
    result["p2sh_address"] = contract.p2sh_address;
    result["redeem_script"] = contract.redeem_script;
    result["refund_time"] = contract.refund_time;
    result["message"] = "Send " + std::to_string(amount) +
                        " DIN to " + contract.p2sh_address;
    return result;
}

// contract.release
din::Json contract_release_impl(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    // Parse: contract_id, to_address, sig_buyer, sig_seller
    std::string contract_id = params["contract_id"].asString();

    // Load contract from storage
    EscrowContract contract = loadContract(contract_id);

    // Build release transaction
    std::string tx_hex = EscrowContractBuilder::createReleaseTransaction(
        contract,
        params["to_address"].asString(),
        params["sig_buyer"].asString(),
        params["sig_seller"].asString()
    );

    // Broadcast transaction
    std::string txid = broadcastTransaction(tx_hex);

    result["success"] = true;
    result["txid"] = txid;
    result["message"] = "Escrow released to seller";
    return result;
}

// contract.refund
din::Json contract_refund_impl(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    // Parse: contract_id, refund_address, sig_buyer
    std::string contract_id = params["contract_id"].asString();

    // Load contract
    EscrowContract contract = loadContract(contract_id);

    // Check timelock has passed
    uint32_t current_height = getBlockHeight();
    if (current_height < contract.refund_time) {
        result["error"] = "Refund timelock not yet reached";
        result["blocks_remaining"] = contract.refund_time - current_height;
        return result;
    }

    // Build refund transaction
    std::string tx_hex = EscrowContractBuilder::createRefundTransaction(
        contract,
        params["refund_address"].asString(),
        params["sig_buyer"].asString()
    );

    // Broadcast
    std::string txid = broadcastTransaction(tx_hex);

    result["success"] = true;
    result["txid"] = txid;
    result["message"] = "Escrow refunded to buyer";
    return result;
}

// contract.status
din::Json contract_status_impl(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    std::string contract_id = params["contract_id"].asString();
    EscrowContract contract = loadContract(contract_id);

    // Check on-chain status
    updateContractStatus(contract);  // Query UTXO set

    result["contract_id"] = contract.contract_id;
    result["status"] = contract.status;
    result["p2sh_address"] = contract.p2sh_address;
    result["amount"] = contract.amount;
    result["lock_txid"] = contract.lock_txid;
    result["confirmations"] = contract.confirmations;
    result["refund_time"] = contract.refund_time;
    result["blocks_until_refund"] = std::max(0,
        (int)contract.refund_time - (int)getBlockHeight());

    return result;
}
```

## 🎮 User Workflow

### Creating Escrow:

```bash
# 1. Buyer creates contract
./dinero-cli contract.createescrow \
    "02abc123..." \  # Buyer pubkey
    "03def456..." \  # Seller pubkey
    "04ghi789..." \  # Mediator pubkey
    100.0 \          # Amount (DIN)
    2880             # Refund after 2880 blocks (~6 days)

# Returns:
{
  "contract_id": "esc_1a2b3c...",
  "p2sh_address": "din1qxyz...",
  "redeem_script": "6352210abc...",
  "refund_time": 145680,
  "message": "Send 100.0 DIN to din1qxyz..."
}

# 2. Buyer sends funds to P2SH address
./dinero-cli sendtoaddress din1qxyz... 100.0
```

### Releasing Funds (Happy Path):

```bash
# Both parties sign release
./dinero-cli contract.release \
    "esc_1a2b3c..." \           # Contract ID
    "din1seller..." \           # Seller's address
    "304402201a2b..." \          # Buyer's signature
    "3045022100cd3e..."          # Seller's signature

# Returns:
{
  "success": true,
  "txid": "abc123...",
  "message": "Escrow released to seller"
}
```

### Refund (Timeout):

```bash
# After 6 days, buyer can refund
./dinero-cli contract.refund \
    "esc_1a2b3c..." \           # Contract ID
    "din1buyer..." \            # Buyer's refund address
    "304502210fab..."            # Buyer's signature

# Returns:
{
  "success": true,
  "txid": "def456...",
  "message": "Escrow refunded to buyer"
}
```

### Monitoring Status:

```bash
./dinero-cli contract.status "esc_1a2b3c..."

# Returns:
{
  "contract_id": "esc_1a2b3c...",
  "status": "locked",
  "p2sh_address": "din1qxyz...",
  "amount": 100.0,
  "lock_txid": "abc123...",
  "confirmations": 15,
  "refund_time": 145680,
  "blocks_until_refund": 2500
}
```

## 🧪 Testing on Regtest

```bash
# 1. Generate keys
BUYER_PUB=$(./dinero-cli getnewaddress | jq -r '.pubkey')
SELLER_PUB=$(./dinero-cli getnewaddress | jq -r '.pubkey')
MEDIATOR_PUB=$(./dinero-cli getnewaddress | jq -r '.pubkey')

# 2. Create contract (short refund for testing)
CONTRACT=$(./dinero-cli contract.createescrow \
    "$BUYER_PUB" "$SELLER_PUB" "$MEDIATOR_PUB" 10.0 10)

# 3. Fund contract
P2SH=$(echo $CONTRACT | jq -r '.p2sh_address')
./dinero-cli sendtoaddress $P2SH 10.0

# 4. Mine blocks to confirm
./dinero-cli generate 6

# 5. Check status
./dinero-cli contract.status $(echo $CONTRACT | jq -r '.contract_id')

# 6. Test release
./dinero-cli contract.release ...

# 7. Test refund (after 10 blocks)
./dinero-cli generate 10
./dinero-cli contract.refund ...
```

## 🔗 Integration with P2P Marketplace

Update `p2p_createoffer_impl` to use smart contract escrow:

```cpp
// For SELL offers, create smart contract escrow
if (offer.type == OfferType::SELL) {
    // Get public keys
    EscrowKeys keys;
    keys.buyer_pubkey = "";  // Set when offer accepted
    keys.seller_pubkey = getWalletPubKey();
    keys.mediator_pubkey = DEFAULT_MEDIATOR_PUBKEY;

    // Create contract (6 day refund)
    EscrowContract contract = EscrowContractBuilder::buildContract(
        keys, amount, 2880
    );

    // Store contract
    ContractManager::instance().storeContract(contract);

    // Set offer escrow fields
    offer.escrow_id = contract.contract_id;
    offer.escrow_txid = "";  // Set after funding
    offer.escrow_address = contract.p2sh_address;
}
```

## 📊 Comparison: Old vs New

| Feature | Placeholder Escrow | Smart Contract Escrow |
|---------|-------------------|----------------------|
| **Trust Model** | Trust server | Trust consensus |
| **Enforcement** | Off-chain logic | On-chain script |
| **Refund** | Manual admin | Automatic timelock |
| **Dispute** | Contact support | Mediator co-signs |
| **Verification** | Database query | Blockchain query |
| **Censorship** | Possible | Impossible |
| **Downtime Risk** | Server can fail | Always available |

## 🚀 Upgrade Path

### Phase 1: P2SH Escrow (Current)
- Standard Bitcoin Script
- Works on existing UTXO chain
- No consensus changes

### Phase 2: P2WSH (SegWit Style)
- Witness version for lower fees
- Better script isolation
- Malleability fixes

### Phase 3: Miniscript
- Human-readable policy language
- Safer script composition
- Formal verification

### Phase 4: Dinero VM
- Full Turing-complete contracts
- EVM-style execution
- Advanced DeFi primitives

## ⚠️ Security Considerations

1. **Key Management**
   - Users must securely store private keys
   - Losing buyer key = funds locked forever
   - Use HD wallets for key derivation

2. **Timelock Selection**
   - Too short: Not enough time to complete trade
   - Too long: Funds locked unnecessarily
   - Recommended: 2880-4320 blocks (6-9 days)

3. **Fee Estimation**
   - Contract spends are larger than normal txs
   - Release: ~300-400 bytes (2-of-3 multisig)
   - Refund: ~200-250 bytes (single sig + script)
   - Set appropriate fee rates

4. **Mediator Trust**
   - Mediator can collude with one party
   - Use reputable mediators with reputation system
   - Future: Decentralized mediator pools

5. **Script Bugs**
   - Thoroughly test on regtest
   - Audit script generation code
   - Start with small amounts

## 🎓 Educational Resources

- **Bitcoin Script Reference**: https://en.bitcoin.it/wiki/Script
- **P2SH BIP16**: https://github.com/bitcoin/bips/blob/master/bip-0016.mediawiki
- **CHECKLOCKTIMEVERIFY BIP65**: https://github.com/bitcoin/bips/blob/master/bip-0065.mediawiki
- **Mastering Bitcoin Chapter 7**: Script and Transactions

## 💡 Future Enhancements

1. **Partial Release**: Release funds in stages
2. **Multi-Asset**: Escrow multiple cryptocurrencies
3. **Atomic Swaps**: Cross-chain escrow
4. **Lightning Integration**: Instant micropayments escrow
5. **Privacy**: Schnorr signatures + Taproot

---

**Status**: 🚧 Design Complete - Implementation In Progress
**Priority**: ⭐⭐⭐⭐⭐ (Critical for trustless P2P)
**Risk**: Low (standard Bitcoin techniques)
**Estimated Effort**: 2-3 days full implementation + testing
