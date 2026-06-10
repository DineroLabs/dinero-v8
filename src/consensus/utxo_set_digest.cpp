#include "consensus/utxo_set_digest.h"

#include <algorithm>
#include <cstring>

namespace dinero::consensus {

// Serializes one UTXO record in the exact byte order that ExportSnapshot
// (chainstate_service.cpp) writes to the snapshot file:
//   txid(32) | vout(4 LE) | value.v(8 LE) | script_len(4 LE) | script |
//   height(4 LE) | is_coinbase(1)
// Byte-identity with ExportSnapshot is the design invariant; if this function
// diverges from the snapshot loop, the golden test (GoldenRecordBytes) will
// catch it.
std::vector<uint8_t> SerializeUtxoRecord(const OutPoint& outpoint, const UTXOEntry& entry) {
    std::vector<uint8_t> out;
    out.reserve(32 + 4 + 8 + 4 + entry.scriptPubKey.size() + 4 + 1);

    // txid: 32 bytes — matches ExportSnapshot: outpoint.txid.AsUint256().data
    const auto& txid_data = outpoint.txid.AsUint256().data;
    out.insert(out.end(), txid_data, txid_data + 32);

    // Helper: append n bytes from p in their native (little-endian on x86) order,
    // matching file.write(reinterpret_cast<const char*>(&field), sizeof(field)).
    auto append_le = [&out](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };

    // vout: 4 bytes LE
    append_le(&outpoint.vout, sizeof(outpoint.vout));

    // value.v: 8 bytes LE (AmountUna.v is uint64_t)
    append_le(&entry.value.v, sizeof(entry.value.v));

    // script_len: 4 bytes LE
    const uint32_t script_len = static_cast<uint32_t>(entry.scriptPubKey.size());
    append_le(&script_len, sizeof(script_len));

    // script bytes
    out.insert(out.end(), entry.scriptPubKey.begin(), entry.scriptPubKey.end());

    // height: 4 bytes LE
    append_le(&entry.height, sizeof(entry.height));

    // is_coinbase: 1 byte
    const uint8_t is_coinbase = entry.isCoinbase ? 1 : 0;
    out.push_back(is_coinbase);

    return out;
}

uint256 ComputeUtxoRecordsDigest(const std::unordered_map<OutPoint, UTXOEntry>& utxos) {
    // Sort outpoints for deterministic order — same sort ExportSnapshot uses.
    std::vector<OutPoint> sorted;
    sorted.reserve(utxos.size());
    for (const auto& [op, _] : utxos) sorted.push_back(op);
    std::sort(sorted.begin(), sorted.end());

    StreamingUtxoDigest stream;
    for (const auto& op : sorted) {
        stream.AddRecord(op, utxos.at(op));
    }
    return stream.Finalize();
}

void StreamingUtxoDigest::AddRecord(const OutPoint& outpoint, const UTXOEntry& entry) {
    const auto bytes = SerializeUtxoRecord(outpoint, entry);
    sha_.Write(bytes.data(), bytes.size());
}

void StreamingUtxoDigest::AddRecordBytes(const uint8_t* data, size_t len) {
    sha_.Write(data, len);
}

uint256 StreamingUtxoDigest::Finalize() {
    uint256 out;
    // CSHA256::Finalize(uint8_t hash[32]) — uint256.data is uint8_t[32].
    sha_.Finalize(out.data);
    return out;
}

}  // namespace dinero::consensus
