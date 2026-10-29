#pragma once
#include "daemon/mempool_transaction.h"
#include "network/bridge_node.h"
#include <limits>

namespace dinero {
// Owns exact typed bytes, all transparent-input proofs and their captured root.
// Caller serializes provider/selected-chain access. This is proof serving from
// a snapshot, not transaction authorization/admission.
inline std::optional<std::vector<uint8_t>> CaptureUtreexoTransactionPayload(
    const MempoolTransaction& body, network::BridgeNode& bridge) {
    constexpr size_t max_payload=2*1024*1024, max_tx=1024*1024;
    constexpr size_t max_proof=256*1024, max_script=10*1024, max_inputs=1000;
    if (!body.HasBody() || body.Inputs().size()>max_inputs) return std::nullopt;
    const auto wire=body.Serialize();
    if (wire.empty() || wire.size()>max_tx) return std::nullopt;
    const auto txid=body.GetTxid().AsUint256();
    auto captured=bridge.CaptureInputProofs(txid,body.Inputs());
    if (!captured || captured->root.size()!=32 || captured->proofs.size()!=body.Inputs().size())
        return std::nullopt;
    std::vector<uint8_t> payload;
    auto append=[&](const auto& bytes) {
        if (bytes.size()>max_payload-payload.size()) return false;
        payload.insert(payload.end(),bytes.begin(),bytes.end()); return true;
    };
    auto integer=[&](uint64_t value,size_t width) {
        if (width>max_payload-payload.size()) return false;
        for(size_t i=0;i<width;++i) payload.push_back(static_cast<uint8_t>(value>>(8*i)));
        return true;
    };
    if (!integer(2,1)) return std::nullopt;
    payload.insert(payload.end(),txid.begin(),txid.end());
    if (!integer(wire.size(),4) || !append(wire) || !integer(captured->proofs.size(),4)) return std::nullopt;
    for (const auto& [proof,spent] : captured->proofs) {
        // The existing v2 wire format has no confidential commitment field.
        if (spent.is_confidential || spent.scriptPubKey.size()>max_script) return std::nullopt;
        const auto bytes=proof.serialize();
        if (bytes.size()>max_proof || !integer(bytes.size(),4) || !append(bytes) ||
            !integer(spent.value,8) || !integer(spent.scriptPubKey.size(),4) ||
            !append(spent.scriptPubKey) || !integer(spent.created_height,4) ||
            !integer(spent.is_coinbase?1:0,1)) return std::nullopt;
    }
    if (!append(captured->root)) return std::nullopt;
    return payload;
}
} // namespace dinero
