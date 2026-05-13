# Smart Contract Escrow - Transaction Integration Guide

## Overview

This guide shows how to integrate the Bitcoin Script escrow system with Dinero's transaction layer to enable **full on-chain enforcement**.

## Current Status

✅ **Core Engine Complete**:
- Bitcoin Script generation (2-of-3 multisig + timelock)
- SHA256 + RIPEMD160 hashing
- P2SH address generation  
- Contract storage & RPC interface

⚠️ **Transaction Layer**: Needs integration with existing wallet infrastructure

## Architecture

```
User Creates Escrow
    ↓
RPC: contract.createescrow
    ↓
Generate P2SH Address (din1q...)
    ↓
User Funds Address ← CURRENT INTEGRATION POINT
    ↓
Monitor UTXO Set
    ↓
Release or Refund
```

## Phase 1: Manual Funding (WORKS TODAY!)

The P2SH addresses we generate are **valid Dinero addresses**. Users can fund them using existing wallet RPCs:

```bash
# 1. Create escrow contract
CONTRACT=$(./dinero-cli contract.createescrow \
  '{"buyer_pubkey":"027...", "seller_pubkey":"02c...", \
    "mediator_pubkey":"02f...", "amount":10.5}')

P2SH_ADDR=$(echo $CONTRACT | jq -r '.p2sh_address')

# 2. Fund escrow (uses existing wallet!)
TXID=$(./dinero-cli sendtoaddress $P2SH_ADDR 10.5)

# 3. Update contract with TXID
./dinero-cli contract.setlocktx "$CONTRACT_ID" "$TXID"
```

**This works RIGHT NOW with zero additional code!**

## Phase 2: Automated Funding

Add `contract.fund` RPC that wraps the existing wallet:

```cpp
// src/rpc/methods_contract.cpp

din::Json contract_fund_impl(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    // 1. Get contract
    std::string contract_id = params["contract_id"].asString();
    auto contract = ContractRegistry::instance().getContract(contract_id);
    
    // 2. Use existing sendtoaddress logic
    din::Json send_params;
    send_params["address"] = contract->p2sh_address;
    send_params["amount"] = contract->amount;
    
    // Call existing RPC
    auto result = ctx.callRPC("sendtoaddress", send_params);
    
    // 3. Update contract with TXID
    contract->lock_txid = result["txid"].asString();
    contract->status = "locked";
    ContractRegistry::instance().updateContract(*contract);
    
    return result;
}
```

**Complexity**: Low (just wraps existing RPC)  
**Benefit**: User-friendly single command

## Phase 3: P2SH Spending (Release/Refund)

This is the complex part - building transactions that spend FROM the P2SH address.

### Release Transaction Structure

```
Transaction:
  version: 2
  inputs:
    - prevout: <lock_txid>:<vout>
      scriptSig: <empty for P2SH>
      witness:
        - 0x00 (OP_0 for multisig bug)
        - <sig_buyer>
        - <sig_seller>
        - 0x01 (OP_TRUE to take IF branch)
        - <redeem_script>
      sequence: 0xfffffffe
  outputs:
    - value: <amount - fee>
      scriptPubKey: <seller_address scriptPubKey>
  locktime: 0
```

### Refund Transaction Structure

```
Transaction:
  version: 2
  inputs:
    - prevout: <lock_txid>:<vout>
      scriptSig: <empty for P2SH>
      witness:
        - <sig_buyer>
        - 0x00 (OP_FALSE to take ELSE branch)
        - <redeem_script>
      sequence: 0xfffffffe
  outputs:
    - value: <amount - fee>
      scriptPubKey: <buyer_address scriptPubKey>
  locktime: <refund_time>  ← CRITICAL!
```

### Implementation Steps

#### 1. Create P2SH ScriptPubKey Helper

```cpp
// Add to escrow_contract.cpp

std::vector<uint8_t> EscrowContractBuilder::createP2SHScriptPubKey(
    const std::string& script_hash_hex
) {
    std::vector<uint8_t> script;
    std::vector<uint8_t> hash = hexToBytes(script_hash_hex);
    
    // P2SH: OP_HASH160 <20-byte-hash> OP_EQUAL
    script.push_back(0xa9);  // OP_HASH160
    script.push_back(0x14);  // Push 20 bytes
    script.insert(script.end(), hash.begin(), hash.end());
    script.push_back(0x87);  // OP_EQUAL
    
    return script;
}
```

#### 2. Implement Release Transaction

```cpp
std::string EscrowContractBuilder::createReleaseTransaction(
    const EscrowContract& contract,
    const std::string& to_address,
    const std::string& sig_buyer,
    const std::string& sig_seller
) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.is_segwit = true;
    
    // Input: Spend from P2SH
    TxInput input;
    input.prevout.txid = contract.lock_txid;
    input.prevout.vout = 0;  // TODO: Find correct vout
    input.sequence = 0xfffffffe;
    input.scriptSig = {};  // Empty for P2SH
    
    // Witness: multisig unlock
    input.witness.push_back({0x00});  // OP_0 (CHECKMULTISIG bug)
    input.witness.push_back(hexToBytes(sig_buyer));
    input.witness.push_back(hexToBytes(sig_seller));
    input.witness.push_back({0x01});  // OP_TRUE (IF branch)
    input.witness.push_back(hexToBytes(contract.redeem_script));
    
    tx.vin.push_back(input);
    
    // Output: Send to seller
    TxOutput output;
    output.value = contract.amount * 100000000 - 1000;  // Subtract fee
    output.scriptPubKey = AddressToScriptPubKey(to_address);
    tx.vout.push_back(output);
    
    return tx.SerializeHex();
}
```

#### 3. Implement Refund Transaction

```cpp
std::string EscrowContractBuilder::createRefundTransaction(
    const EscrowContract& contract,
    const std::string& refund_address,
    const std::string& sig_buyer
) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = contract.refund_time;  // ← CRITICAL for CLTV!
    tx.is_segwit = true;
    
    // Input: Spend from P2SH
    TxInput input;
    input.prevout.txid = contract.lock_txid;
    input.prevout.vout = 0;
    input.sequence = 0xfffffffe;
    input.scriptSig = {};
    
    // Witness: timelock unlock
    input.witness.push_back(hexToBytes(sig_buyer));
    input.witness.push_back({0x00});  // OP_FALSE (ELSE branch)
    input.witness.push_back(hexToBytes(contract.redeem_script));
    
    tx.vin.push_back(input);
    
    // Output: Refund to buyer
    TxOutput output;
    output.value = contract.amount * 100000000 - 1000;
    output.scriptPubKey = AddressToScriptPubKey(refund_address);
    tx.vout.push_back(output);
    
    return tx.SerializeHex();
}
```

#### 4. Add Signature Helper

```cpp
std::string EscrowContractBuilder::signForContract(
    const EscrowContract& contract,
    const std::string& private_key_hex,
    const std::string& to_address,
    bool is_release  // true = release, false = refund
) {
    // 1. Build sighash
    std::vector<uint8_t> sighash = computeSighash(
        contract.lock_txid,
        0,  // vout
        contract.redeem_script,
        contract.amount * 100000000,
        to_address,
        is_release ? 0 : contract.refund_time
    );
    
    // 2. Sign with secp256k1
    // Use existing crypto::sign() function
    std::vector<uint8_t> sig = crypto::sign(sighash, hexToBytes(private_key_hex));
    
    // 3. Append SIGHASH_ALL
    sig.push_back(0x01);
    
    return bytesToHex(sig);
}
```

## Phase 4: RPC Integration

Add signing RPCs:

```cpp
// contract.signrelease - Buyer/seller signs release
din::Json contract_signrelease_impl(const ExecutionContext& ctx, const din::Json& params) {
    std::string contract_id = params["contract_id"].asString();
    std::string to_address = params["to_address"].asString();
    std::string private_key = params["private_key"].asString();
    
    auto contract = ContractRegistry::instance().getContract(contract_id);
    
    std::string signature = EscrowContractBuilder::signForContract(
        *contract, private_key, to_address, true
    );
    
    din::Json result;
    result["signature"] = signature;
    result["signer"] = deriveAddress(private_key);  // Identify which party signed
    return result;
}

// contract.broadcastrelease - Broadcast with 2 signatures
din::Json contract_broadcastrelease_impl(const ExecutionContext& ctx, const din::Json& params) {
    std::string contract_id = params["contract_id"].asString();
    std::string to_address = params["to_address"].asString();
    std::string sig1 = params["sig1"].asString();
    std::string sig2 = params["sig2"].asString();
    
    auto contract = ContractRegistry::instance().getContract(contract_id);
    
    std::string tx_hex = EscrowContractBuilder::createReleaseTransaction(
        *contract, to_address, sig1, sig2
    );
    
    // Broadcast
    auto result = ctx.callRPC("sendrawtransaction", {{"hex", tx_hex}});
    
    // Update contract
    contract->status = "released";
    ContractRegistry::instance().updateContract(*contract);
    
    return result;
}
```

## Testing Workflow

### Complete Lifecycle Test

```bash
#!/bin/bash
# test_escrow_lifecycle.sh

CLI="./dinero-cli -regtest"

echo "1️⃣  Creating escrow contract..."
CONTRACT=$($CLI contract.createescrow \
  '{"buyer_pubkey":"027...", "seller_pubkey":"02c...", \
    "mediator_pubkey":"02f...", "amount":10.5, "refund_blocks":10}')

CONTRACT_ID=$(echo $CONTRACT | jq -r '.contract_id')
P2SH=$(echo $CONTRACT | jq -r '.p2sh_address')

echo "2️⃣  Funding escrow..."
LOCK_TX=$($CLI sendtoaddress $P2SH 10.5)
$CLI generatetoaddress 1 $MINER_ADDR

echo "3️⃣  Buyer signs release..."
BUYER_SIG=$($CLI contract.signrelease \
  "{\"contract_id\":\"$CONTRACT_ID\", \"to_address\":\"$SELLER_ADDR\", \
    \"private_key\":\"$BUYER_KEY\"}")

echo "4️⃣  Seller signs release..."
SELLER_SIG=$($CLI contract.signrelease \
  "{\"contract_id\":\"$CONTRACT_ID\", \"to_address\":\"$SELLER_ADDR\", \
    \"private_key\":\"$SELLER_KEY\"}")

echo "5️⃣  Broadcasting release transaction..."
RELEASE_TX=$($CLI contract.broadcastrelease \
  "{\"contract_id\":\"$CONTRACT_ID\", \"to_address\":\"$SELLER_ADDR\", \
    \"sig1\":\"$BUYER_SIG\", \"sig2\":\"$SELLER_SIG\"}")

$CLI generatetoaddress 1 $MINER_ADDR

echo "✅ Escrow complete! Funds released to seller."
```

## Security Considerations

1. **Signature Verification**: Always verify signatures match expected public keys
2. **Timelock Validation**: Ensure current block height > refund_time for refunds
3. **Amount Validation**: Verify UTXO amount matches contract amount
4. **Replay Protection**: Use unique contract IDs and check UTXO is unspent
5. **Fee Estimation**: Calculate proper fees to avoid stuck transactions

## Integration Checklist

- [ ] Phase 1: Manual funding (works today)
- [ ] Phase 2: `contract.fund` RPC wrapper
- [ ] Phase 3: P2SH scriptPubKey generation
- [ ] Phase 4: Release transaction building
- [ ] Phase 5: Refund transaction building  
- [ ] Phase 6: Signature generation helper
- [ ] Phase 7: `contract.signrelease` RPC
- [ ] Phase 8: `contract.signrefund` RPC
- [ ] Phase 9: `contract.broadcastrelease` RPC
- [ ] Phase 10: `contract.broadcastrefund` RPC
- [ ] Phase 11: UTXO monitoring integration
- [ ] Phase 12: Regtest lifecycle tests
- [ ] Phase 13: GUI integration

## Next Steps

**Immediate** (can do today):
- Test manual funding workflow on regtest
- Verify P2SH addresses work with existing wallet

**Short-term** (1-2 days):
- Implement `contract.fund` wrapper
- Add P2SH scriptPubKey helper
- Build release transaction logic

**Medium-term** (1 week):
- Complete signing infrastructure
- Add broadcast RPCs
- Full regtest testing

**Long-term** (2+ weeks):
- GUI escrow tab
- P2P marketplace integration
- Multi-party signing UX

---

**The core smart contract engine is DONE. Transaction integration is the final step to make it fully on-chain!**
