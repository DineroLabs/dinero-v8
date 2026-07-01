#pragma once

#include "consensus/consensus_utxo_set.h"
#include "crypto/sha256.h"
#include "primitives/uint256.h"

#include <unordered_map>
#include <vector>

namespace dinero::consensus {

// Canonical per-UTXO record encoding — MUST stay byte-identical to the
// snapshot file's record section (ExportSnapshot at
// src/daemon/services/chainstate_service.cpp, the per-UTXO write loop):
//   txid(32) | vout(4) | value.v(8) | script_len(4) | script | height(4) |
//   is_coinbase(1), all integers little-endian.
// This is the unit of the AssumeUTXO content commitment
// (docs/design/assumeutxo-fatal-state-machine.md, Anchor Binding).
std::vector<uint8_t> SerializeUtxoRecord(const OutPoint& outpoint, const UTXOEntry& entry);

// Fail-closed guard for the snapshot content commitment (#281).
// SerializeUtxoRecord intentionally omits UTXOEntry.is_confidential/commitment.
// That is only sound while those fields are consensus-forced empty (v7 is
// transparent-only). This returns false for any UTXO carrying confidential
// data, so the exporter can refuse rather than emit a record whose confidential
// fields are unbound by the digest. Must be re-evaluated (and the record
// encoding extended) before any confidential/shielded UTXO lane is enabled.
bool UtxoRecordIsSnapshotSafe(const UTXOEntry& entry);

// SHA256 over all records in sorted-outpoint order (operator< on OutPoint).
// Pure content function: independent of container iteration order.
uint256 ComputeUtxoRecordsDigest(const std::unordered_map<OutPoint, UTXOEntry>& utxos);

// Streaming variant for LoadSnapshot: records arrive already in the file's
// sorted order; call AddRecord() for each and Finalize() once.
class StreamingUtxoDigest {
public:
    void AddRecord(const OutPoint& outpoint, const UTXOEntry& entry);
    void AddRecordBytes(const uint8_t* data, size_t len);  // pre-serialized bytes
    uint256 Finalize();

private:
    // Fully-qualified to avoid ambiguity: we're inside dinero::consensus,
    // so an unqualified 'crypto' would look for dinero::consensus::crypto.
    ::dinero::crypto::CSHA256 sha_;
};

}  // namespace dinero::consensus
