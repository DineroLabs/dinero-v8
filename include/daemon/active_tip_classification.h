// Copyright (c) 2026 Dinero Labs.
#pragma once

#include <optional>

#include "primitives/uint256.h"

namespace dinero {
namespace daemon {

/// How a block's parent relates to the active chain.
enum class ExtensionClass {
    ExtendsActiveTip,  ///< parent IS the active consensus tip
    SideChain          ///< anything else
};

/**
 * The classification rule, as a pure function of the ACTIVE consensus tip.
 *
 * Extracted so the rule is testable without a daemon -- including the states
 * that only exist mid-promotion, which is precisely where an unsynchronized
 * read produces a torn answer.
 *
 * The tip passed here MUST be read under the activation lock; see
 * ChainstateService::ExtendsActiveTipLocked, which is the only production
 * caller and asserts that.
 *
 * Passing ChainDB's DURABLE tip instead is the defect this exists to prevent:
 * during AssumeUTXO replay the durable tip trails the snapshot base, so the
 * base's own child classifies as a side chain, which skipped its accept-time
 * Utreexo root check.
 */
inline ExtensionClass ClassifyExtension(const std::optional<uint256>& active_tip,
                                        const uint256& parent_hash) {
    if (!active_tip.has_value()) return ExtensionClass::SideChain;
    return (*active_tip == parent_hash) ? ExtensionClass::ExtendsActiveTip
                                        : ExtensionClass::SideChain;
}

}  // namespace daemon
}  // namespace dinero
