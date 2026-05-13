#pragma once
/**
 * V7 Pay-to-Merkle-Root address codec.
 *
 * Spec: docs/consensus/V7_GENESIS_SPEC.md § "Address Format".
 *
 * Addresses are standard bech32m (BIP-350) with witness version 3.
 *   HRP:              "din" (mainnet) / "tdin" (testnet) / "rdin" (regtest)
 *   Witness version:  3  (first data char after '1' is 'r')
 *   Witness program:  32-byte Merkle root over one or more PQ public keys
 *
 * The scriptPubKey for a P2MR output is always exactly 34 bytes:
 *   [0x53] [0x20] [<32-byte Merkle root>]
 *
 * This header is wallet-layer, not consensus. Consensus recognizes the
 * scriptPubKey pattern directly via IsFreezeForkAllowedScript() /
 * IsP2MRScript() (Phase 5). Address encoding is purely for human / wallet
 * handling.
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dinero::wallet {

/** Fixed byte length of the Merkle-root commitment in a P2MR scriptPubKey. */
constexpr std::size_t P2MR_MERKLE_ROOT_BYTES = 32;

/** Witness version that P2MR occupies. */
constexpr int P2MR_WITNESS_VERSION = 3;

/**
 * Encode a 32-byte Merkle root as a bech32m address.
 *
 * @param hrp           Human-readable part per chain params (e.g. "din").
 * @param merkle_root   32 bytes. Length strictly checked.
 * @return Encoded string (e.g. "din1r..."), or empty string on error
 *         (wrong merkle_root length, invalid HRP).
 */
std::string EncodeP2MRAddress(const std::string& hrp,
                              const std::array<uint8_t, P2MR_MERKLE_ROOT_BYTES>& merkle_root);

/** Overload accepting a vector; returns "" if size != 32. */
std::string EncodeP2MRAddress(const std::string& hrp,
                              const std::vector<uint8_t>& merkle_root);

struct DecodedP2MR {
    std::string hrp;
    std::array<uint8_t, P2MR_MERKLE_ROOT_BYTES> merkle_root;
};

/**
 * Decode a bech32m P2MR address. Returns nullopt if the string is not a
 * valid bech32m witness-v3 address with a 32-byte program. Caller is
 * responsible for checking `hrp` against the expected chain.
 */
std::optional<DecodedP2MR> DecodeP2MRAddress(const std::string& address);

/**
 * Build the 34-byte P2MR scriptPubKey for a given Merkle root.
 * Pattern: 0x53 (OP_3) || 0x20 (PUSH32) || <32 bytes>.
 */
std::vector<uint8_t> BuildP2MRScriptPubKey(
    const std::array<uint8_t, P2MR_MERKLE_ROOT_BYTES>& merkle_root);

} // namespace dinero::wallet
