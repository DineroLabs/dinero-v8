#pragma once

/**
 * @file canonical_wallet_utxo.h
 * @brief Phase M.3: THE canonical UTXO type (replaces all 9 variants)
 *
 * Phase M.3 Rule: This is the ONLY WalletUTXO type.
 * All other WalletUTXO definitions are deleted or aliased to this.
 *
 * Design: Minimal core, everything else derived.
 * - confirmations = computed from height
 * - address = derived from spk
 * - spendable = computed from height, is_coinbase, locks
 *
 * RPC converts this to JSON at boundaries only.
 */

#include "primitives/uint256.h"
#include "primitives/amount.h"       // Phase M.6.2: Monetary type safety
#include <cstdint>
#include <vector>
#include <string>

namespace dinero {

/**
 * @brief THE canonical wallet UTXO (Phase M.3)
 *
 * Minimal core fields - all other data is derived/computed:
 * - txid, vout: OutPoint identity
 * - value, spk: Spending data
 * - height, is_coinbase: Blockchain state
 * - path: Wallet derivation info
 *
 * NO storage of derived fields:
 * - confirmations (computed: tip_height - height + 1)
 * - address (derived from spk)
 * - spendable (computed from height, is_coinbase, maturity)
 */
struct CanonicalWalletUTXO {
    uint256 txid;                    // Phase M.0: uint256 (NOT string)
    uint32_t vout;                   // Output index

    // Phase M.6.2: Type-safe monetary amount
    AmountUna value;                // Amount in una
    std::vector<uint8_t> spk;        // scriptPubKey (binary, NOT hex)

    uint32_t height;                 // Block height (0 = unconfirmed)
    bool is_coinbase;                // Needs 100 confirmations if true

    std::string path;                // BIP32 derivation path

    // Optional CT metadata used for confidential Taproot sighash binding.
    bool is_confidential = false;
    std::vector<uint8_t> commitment;

    // B2: Policy ID (32 bytes if PROTECTED/ESCROW, empty for STANDARD)
    std::vector<uint8_t> policy_id;

    // Minimal constructor
    CanonicalWalletUTXO() : vout(0), value(AmountUna::Zero()), height(0), is_coinbase(false) {}

    // RPC boundary helpers (string conversion ONLY here)
    std::string GetTxIdHex() const { return txid.GetHex(); }
    std::string GetOutpointString() const {
        return txid.GetHex() + ":" + std::to_string(vout);
    }
};

} // namespace dinero
