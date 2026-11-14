#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace dinero::wallet_catchup {

// A wallet must never skip an unreadable selected block and then enqueue a
// later height: the later job can persist a watermark that hides the gap.
// Load returns an optional (selected hash, body) pair. Submit runs only after
// the stored block's header hash matches the selected hash; either callback
// may refuse by throwing. This does not replace block-body validation.
template <class Load, class Submit>
void RequireContiguous(uint32_t scanned_height, uint32_t selected_height,
                       Load&& load, Submit&& submit) {
    if (scanned_height > selected_height)
        throw std::runtime_error("wallet catch-up watermark exceeds selected height");
    for (uint64_t cursor = uint64_t(scanned_height) + 1;
         cursor <= selected_height; ++cursor) {
        const auto height = static_cast<uint32_t>(cursor);
        auto selected = load(height);
        if (!selected || selected->first.IsNull() ||
            selected->second.GetHash() != selected->first)
            throw std::runtime_error("wallet catch-up selected body unavailable at height "
                + std::to_string(height));
        submit(height, selected->first, selected->second);
    }
}

} // namespace dinero::wallet_catchup
