#pragma once

namespace dinero {
namespace consensus {

/**
 * Initial Block Download (IBD) mode
 * Phase 5: Stateless IBD support
 */
enum class IBDMode {
    Stateful,   // Traditional mode: uses UTXO database for validation
    Stateless   // Stateless mode: uses Utreexo proofs for validation
};

/**
 * IBD configuration
 */
struct IBDConfig {
    IBDMode mode = IBDMode::Stateful;

    bool isStateful() const { return mode == IBDMode::Stateful; }
    bool isStateless() const { return mode == IBDMode::Stateless; }
};

} // namespace consensus
} // namespace dinero
