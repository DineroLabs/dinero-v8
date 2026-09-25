#pragma once
#include "orchard_state_test_fixture.h"
#include "primitives/orchard_block_reader.h"
#include "consensus/merkle_root.h"
#include "consensus/witness_commitment.h"

// Honest proof fixtures inside a syntactically valid block with real identity
// commitments. No mined PoW, Utreexo proof or full block validity is claimed.
// Independent test encoding, matching the miner's minimal Script-number push.
static Bytes CoinbaseHeightScript(uint32_t height) {
    if(height>=1&&height<=16)return Bytes{static_cast<uint8_t>(0x50+height),0};
    Bytes value;for(uint32_t n=height;n;n>>=8)value.push_back(n&255);
    if(value.empty())value.push_back(0);
    if(value.back()&0x80)value.push_back(0);
    Bytes script{static_cast<uint8_t>(value.size())};script.insert(script.end(),value.begin(),value.end());return script;
}
static OrchardBlockCandidate CandidateWires(const OrchardBlockContext& context,
    std::vector<Bytes> wires, uint32_t nonce = 0, uint64_t coinbase_amount = 1, std::optional<Bytes> coinbase_script = std::nullopt) {
    Require(wires.size()<252);
    Transaction coinbase;coinbase.version=2;
    TxInput input;input.prevout.vout=UINT32_MAX;input.scriptSig=coinbase_script.value_or(CoinbaseHeightScript(context.height));coinbase.vin={input};
    coinbase.vout.emplace_back(AmountUna::Una(coinbase_amount),Bytes{0x51});
    std::vector<WTxId> wids{WTxId(uint256())};
    for(const auto& wire:wires) {
        wids.push_back(ParsedTransaction::DecodeExact(wire,TransactionReadMode::StagedOrchard).GetWtxid());
    }
    coinbase.vout.emplace_back(AmountUna::Zero(),consensus::BuildWitnessCommitmentFromRoot(
        consensus::ComputeWitnessMerkleRootFromIds(wids)));
    wires.insert(wires.begin(),coinbase.Serialize(TxSerializationMode::WithWitness));
    std::vector<TxId> ids;
    for(const auto& wire:wires)ids.push_back(ParsedTransaction::DecodeExact(wire,TransactionReadMode::StagedOrchard).GetTxid());
    BlockHeader header{};header.version=1;header.prev_block_hash=context.parent_hash;
    header.timestamp=context.height;header.nonce=nonce;header.merkle_root=consensus::ComputeTransactionMerkleRoot(ids);
    const auto prefix=header.SerializeForHash();Bytes bytes(prefix.begin(),prefix.end());bytes.push_back(wires.size());
    for(const auto& wire:wires)bytes.insert(bytes.end(),wire.begin(),wire.end());bytes.push_back(0);
    return OrchardBlockCandidate::DecodeExact(bytes);
}
static OrchardBlockCandidate Candidate(const OrchardBlockContext& context,
    std::span<const VerifiedOrchardAuthorizations> authorizations,
    const std::vector<Transaction>& extra = {}, uint32_t nonce = 0) {
    std::vector<Bytes> wires;
    for(const auto& auth:authorizations) wires.push_back(auth.Orchard().CanonicalBytes());
    for(const auto& tx:extra) wires.push_back(tx.Serialize(TxSerializationMode::WithWitness));
    return CandidateWires(context,std::move(wires),nonce);
}
