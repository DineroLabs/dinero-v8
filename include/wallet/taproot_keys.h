#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include <string>

namespace dinero {

/**
 * Taproot Key Management (BIP340/BIP86)
 *
 * Provides functions for:
 * - Generating Taproot keypairs (32-byte x-only public keys)
 * - BIP340 Schnorr signature creation
 * - BIP86 Taproot address derivation
 */
class TaprootKeys {
public:
    /**
     * Generate a random Taproot keypair
     *
     * @param privkey Output: 32-byte private key
     * @param xonly_pubkey Output: 32-byte x-only public key
     * @param pubkey_parity Output: parity bit (0 or 1) - needed for signing
     * @return true on success
     */
    static bool GenerateKeypair(std::array<uint8_t, 32>& privkey,
                                std::array<uint8_t, 32>& xonly_pubkey,
                                int& pubkey_parity);

    /**
     * Derive x-only public key from private key
     *
     * @param privkey 32-byte private key
     * @param xonly_pubkey Output: 32-byte x-only public key
     * @param pubkey_parity Output: parity bit (0 or 1)
     * @return true on success
     */
    static bool DeriveXOnlyPubkey(const std::array<uint8_t, 32>& privkey,
                                  std::array<uint8_t, 32>& xonly_pubkey,
                                  int& pubkey_parity);

    /**
     * Create BIP340 Schnorr signature
     *
     * @param sig64 Output: 64-byte Schnorr signature
     * @param msg32 32-byte message hash (e.g., BIP341 sighash)
     * @param privkey 32-byte private key
     * @param aux_rand32 Optional 32-byte auxiliary random data (can be nullptr)
     * @return true on success
     */
    static bool SignSchnorr(std::array<uint8_t, 64>& sig64,
                           const std::array<uint8_t, 32>& msg32,
                           const std::array<uint8_t, 32>& privkey,
                           const uint8_t* aux_rand32 = nullptr);

    /**
     * Verify BIP340 Schnorr signature
     *
     * @param sig64 64-byte Schnorr signature
     * @param msg32 32-byte message hash
     * @param xonly_pubkey 32-byte x-only public key
     * @return true if signature is valid
     */
    static bool VerifySchnorr(const std::array<uint8_t, 64>& sig64,
                             const std::array<uint8_t, 32>& msg32,
                             const std::array<uint8_t, 32>& xonly_pubkey);

    /**
     * Create Taproot address from x-only public key
     *
     * @param xonly_pubkey 32-byte x-only public key
     * @param hrp Human-readable part (e.g., "din")
     * @return Taproot address (bech32m format) or empty string on error
     */
    static std::string CreateTaprootAddress(const std::array<uint8_t, 32>& xonly_pubkey,
                                           const std::string& hrp);

    /**
     * Tweak a private key for Taproot (BIP86 - key path spending)
     *
     * @deprecated This function CANNOT correctly extract the tweaked secret key.
     * The secp256k1 keypair structure is opaque and the library handles Y parity
     * internally without exposing the properly-negated secret key.
     *
     * For Taproot key-path signing, use SignSchnorrWithInternalKey() which signs
     * directly with the tweaked keypair - the ONLY correct approach.
     *
     * See: docs/crypto/taproot_keypath_signing.md
     *
     * @param privkey Input/Output: 32-byte private key (will be tweaked in-place)
     * @param xonly_pubkey 32-byte x-only public key (internal key)
     * @return true on success (but output is NOT suitable for signing!)
     */
    [[deprecated("Taproot key-path signing must use keypair APIs. "
                 "Extracting tweaked private keys is unsupported - use SignSchnorrWithInternalKey().")]]
    static bool TweakPrivkey(std::array<uint8_t, 32>& privkey,
                            const std::array<uint8_t, 32>& xonly_pubkey);

    /**
     * Sign with internal key after applying Taproot tweak (BIP86 key-path spending)
     *
     * This is the CORRECT way to sign for Taproot key-path spending.
     * It handles Y parity correctly by signing directly with the tweaked keypair.
     *
     * @param sig64 Output: 64-byte Schnorr signature
     * @param msg32 32-byte message hash (BIP341 sighash)
     * @param internal_privkey 32-byte internal private key (untweaked)
     * @param internal_xonly_pubkey 32-byte internal x-only public key
     * @param aux_rand32 Optional 32-byte auxiliary random data (can be nullptr)
     * @return true on success
     */
    static bool SignSchnorrWithInternalKey(std::array<uint8_t, 64>& sig64,
                                           const std::array<uint8_t, 32>& msg32,
                                           const std::array<uint8_t, 32>& internal_privkey,
                                           const std::array<uint8_t, 32>& internal_xonly_pubkey,
                                           const uint8_t* aux_rand32 = nullptr);

    /**
     * Compute the tweaked output pubkey from internal pubkey (BIP86)
     *
     * output_key = internal_key + tagged_hash("TapTweak", internal_key) * G
     *
     * @param internal_xonly_pubkey 32-byte internal x-only public key
     * @param tweaked_xonly_pubkey Output: 32-byte tweaked x-only public key (for scriptPubKey)
     * @return true on success
     */
    static bool ComputeTweakedPubkey(const std::array<uint8_t, 32>& internal_xonly_pubkey,
                                     std::array<uint8_t, 32>& tweaked_xonly_pubkey);

    /**
     * Compute tweaked private key for Taproot key-path spending (BIP86)
     *
     * Correct parity-aware tweaking: checks Y coordinate of internal key,
     * negates private key if odd parity, then adds TapTweak hash.
     *
     * @param internal_privkey 32-byte internal (untweaked) private key
     * @param internal_xonly 32-byte x-only public key of the internal key
     * @param tweaked_privkey Output: 32-byte tweaked private key
     * @return true on success
     */
    static bool ComputeTweakedPrivkey(const std::array<uint8_t, 32>& internal_privkey,
                                       const std::array<uint8_t, 32>& internal_xonly,
                                       std::array<uint8_t, 32>& tweaked_privkey);

    // ========================================================================
    // Script-Path Spending (BIP342)
    // ========================================================================

    /**
     * Compute Tapleaf hash for script-path spending (BIP341)
     *
     * tapleaf_hash = TaggedHash("TapLeaf", leaf_version || compact_size(script) || script)
     *
     * @param script The Tapscript bytes
     * @param leaf_version Leaf version byte (0xC0 for Tapscript/BIP342)
     * @param leaf_hash Output: 32-byte tapleaf hash
     * @return true on success
     */
    static bool ComputeTapleafHash(const std::vector<uint8_t>& script,
                                   uint8_t leaf_version,
                                   std::array<uint8_t, 32>& leaf_hash);

    /**
     * Compute TapBranch hash for merkle tree construction (BIP341)
     *
     * tapbranch_hash = TaggedHash("TapBranch", sorted(left || right))
     *
     * @param left 32-byte left child hash
     * @param right 32-byte right child hash
     * @param branch_hash Output: 32-byte branch hash
     * @return true on success
     */
    static bool ComputeTapBranchHash(const std::array<uint8_t, 32>& left,
                                     const std::array<uint8_t, 32>& right,
                                     std::array<uint8_t, 32>& branch_hash);

    /**
     * Compute output key parity for control block (BIP341)
     *
     * Returns the parity of the output key's Y coordinate.
     * Needed for control block construction.
     *
     * @param internal_xonly_pubkey 32-byte internal x-only public key
     * @param merkle_root 32-byte merkle root (or zeros for key-path only)
     * @param parity Output: 0 for even Y, 1 for odd Y
     * @return true on success
     */
    static bool ComputeOutputKeyParity(const std::array<uint8_t, 32>& internal_xonly_pubkey,
                                       const std::array<uint8_t, 32>& merkle_root,
                                       int& parity);
};

/**
 * Stored Taproot keypair
 */
struct TaprootKeypair {
    std::array<uint8_t, 32> privkey;
    std::array<uint8_t, 32> xonly_pubkey;
    int parity;  // 0 or 1
    std::string address;  // Bech32m address

    TaprootKeypair() : parity(0) {
        privkey.fill(0);
        xonly_pubkey.fill(0);
    }
};

} // namespace dinero
