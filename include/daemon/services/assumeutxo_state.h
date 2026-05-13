#pragma once

#include "primitives/uint256.h"
#include "wallet/utxo_index.h"

#include <cstdint>
#include <string>

namespace dinero::assumeutxo {

inline constexpr const char* kActiveKey = "assumeutxo_active";
inline constexpr const char* kBaseBlockKey = "assumeutxo_base_block";
inline constexpr const char* kBaseHeightKey = "assumeutxo_base_height";

struct StateRef {
    bool& active;
    uint256& base_block;
    uint32_t& base_height;
    UTXOIndex* utxo_index;
};

// Persist snapshot metadata alongside the in-memory AssumeUTXO state.
// This is used only when loading a fresh snapshot into the node.
inline void PersistMetadata(UTXOIndex* utxo_index, const uint256& base_block, uint32_t base_height) {
    if (!utxo_index) {
        return;
    }

    utxo_index->SetMetadata(kActiveKey, "true");
    utxo_index->SetMetadata(kBaseBlockKey, base_block.GetHex());
    utxo_index->SetMetadata(kBaseHeightKey, std::to_string(base_height));
}

inline void ClearMetadata(UTXOIndex* utxo_index) {
    if (!utxo_index) {
        return;
    }

    utxo_index->DeleteMetadata(kActiveKey);
    utxo_index->DeleteMetadata(kBaseBlockKey);
    utxo_index->DeleteMetadata(kBaseHeightKey);
}

// `persist_metadata=false` is the restart/restore path: rehydrate in-memory
// state from previously persisted metadata without rewriting it.
inline void SetState(StateRef state, const uint256& base_block, uint32_t base_height, bool persist_metadata) {
    state.active = true;
    state.base_block = base_block;
    state.base_height = base_height;

    if (persist_metadata) {
        PersistMetadata(state.utxo_index, base_block, base_height);
    }
}

// `clear_persisted_metadata=true` fully exits AssumeUTXO mode.
// `clear_persisted_metadata=false` clears only in-memory state.
inline void ClearState(StateRef state, bool clear_persisted_metadata) {
    state.active = false;
    state.base_block.SetNull();
    state.base_height = 0;

    if (clear_persisted_metadata) {
        ClearMetadata(state.utxo_index);
    }
}

} // namespace dinero::assumeutxo
