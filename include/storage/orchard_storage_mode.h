#pragma once
#include "storage/chain_db.h"
#include "storage/orchard_catalog_state.h"
#include "storage/historical_catalog_state.h"
#include <optional>
#include <stdexcept>

namespace dinero::storage {
// Compatibility binding only. Neither presence nor absence authenticates a
// selected catalog or permits startup enrollment. A new compact live owner
// still requires independently validated history and the startup handoff.
inline constexpr const char* OrchardCompactStorageKey="orchard_compact_storage:v1";
inline std::optional<std::string> ReadOrchardCompactStorageBinding(const ChainDB& db) {
    std::string bytes;const auto status=db.getRaw(OrchardCompactStorageKey,bytes);
    if(status==Status::NotFound)return {};
    if(status!=Status::Ok)throw std::runtime_error("Orchard storage mode unavailable");
    return bytes;
}
inline std::string EncodeOrchardCompactStorageBinding(const catalog::State& state) {
    state.Validate();std::string bytes="DNOCM01";
    catalog::Number(bytes,state.network,1);
    bytes.append(reinterpret_cast<const char*>(state.genesis.data),32);
    catalog::Number(bytes,state.branch,4);catalog::Number(bytes,state.activation,4);
    catalog::Number(bytes,state.leaf_activation,4);
    const auto digest=catalog::Hash(bytes);bytes.append(reinterpret_cast<const char*>(digest.data),32);
    return bytes;
}
inline std::string EncodeOrchardCompactStorageBinding(const catalog::HistoricalState& state) {
    state.Validate();std::string bytes="DNOCM01";
    catalog::Number(bytes,state.network,1);
    bytes.append(reinterpret_cast<const char*>(state.genesis.data),32);
    catalog::Number(bytes,state.branch,4);catalog::Number(bytes,state.activation,4);
    catalog::Number(bytes,state.leaf_activation,4);
    const auto digest=catalog::Hash(bytes);bytes.append(reinterpret_cast<const char*>(digest.data),32);
    return bytes;
}
inline void RequireFullOrchardStorage(const ChainDB& db) {
    // Unknown/malformed compact bindings also refuse. Never default a marked
    // database to full coin/forest storage after a config change or boundary undo.
    if(ReadOrchardCompactStorageBinding(db))
        throw std::runtime_error("Orchard compact storage requires authenticated compact startup");
}
} // namespace dinero::storage
