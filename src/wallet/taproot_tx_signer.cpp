#include "wallet/taproot_tx_signer.h"
#include "wallet/taproot_keys.h"
#include "consensus/script_interpreter.h"
#include "crypto/sha256.h"
#include "crypto/tagged_hash.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>  // For secp256k1_keypair, secp256k1_xonly_pubkey
#include <openssl/rand.h>         // For RAND_bytes (cryptographically secure RNG)
#include <algorithm>
#include <cstring>
#include <iostream>  // For std::cerr error messages

namespace dinero {

namespace {

bool ParseInternalTaprootKey(const std::vector<uint8_t>& private_key,
                             std::array<uint8_t, 32>& internal_privkey,
                             std::array<uint8_t, 32>& internal_xonly_pubkey) {
    if (private_key.size() != 32) {
        return false;
    }

    std::copy(private_key.begin(), private_key.end(), internal_privkey.begin());
    int parity = 0;
    return TaprootKeys::DeriveXOnlyPubkey(internal_privkey, internal_xonly_pubkey, parity);
}

bool VerifyTweakedPubkeyMatchesScript(const CanonicalWalletUTXO& utxo,
                                      const std::array<uint8_t, 32>& internal_xonly_pubkey,
                                      const char* caller) {
    if (utxo.spk.size() != 34) {
        std::cerr << "ERROR [" << caller << "]: Invalid scriptPubKey size (expected 34)" << std::endl;
        return false;
    }

    std::array<uint8_t, 32> tweaked_xonly{};
    if (!TaprootKeys::ComputeTweakedPubkey(internal_xonly_pubkey, tweaked_xonly)) {
        std::cerr << "ERROR [" << caller << "]: Failed to compute tweaked pubkey" << std::endl;
        return false;
    }

    if (std::memcmp(tweaked_xonly.data(), utxo.spk.data() + 2, 32) != 0) {
        std::cerr << "ERROR [" << caller << "]: Tweaked pubkey mismatch vs scriptPubKey" << std::endl;
        return false;
    }

    return true;
}

bool ParseSighash32(const std::vector<uint8_t>& sighash, std::array<uint8_t, 32>& out) {
    if (sighash.size() != 32) {
        return false;
    }
    std::copy(sighash.begin(), sighash.end(), out.begin());
    return true;
}

bool SignTaprootKeyPath(const std::array<uint8_t, 32>& sighash32,
                        const std::array<uint8_t, 32>& internal_privkey,
                        const std::array<uint8_t, 32>& internal_xonly_pubkey,
                        std::vector<uint8_t>& signature_out) {
    uint8_t aux_rand[32];
    if (RAND_bytes(aux_rand, 32) != 1) {
        std::cerr << "ERROR: Failed to generate secure random bytes for aux_rand" << std::endl;
        return false;
    }

    std::array<uint8_t, 64> signature{};
    if (!TaprootKeys::SignSchnorrWithInternalKey(signature, sighash32, internal_privkey, internal_xonly_pubkey, aux_rand)) {
        std::cerr << "ERROR: Schnorr signing failed" << std::endl;
        return false;
    }

    signature_out.assign(signature.begin(), signature.end());
    return true;
}

consensus::ScriptExecutionContext BuildTaprootExecutionContext(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos
) {
    std::vector<uint64_t> all_amounts;
    std::vector<std::vector<uint8_t>> all_scriptpubkeys;
    std::vector<uint8_t> all_confidential_flags;
    std::vector<std::vector<uint8_t>> all_input_commitments;
    all_amounts.reserve(utxos.size());
    all_scriptpubkeys.reserve(utxos.size());
    all_confidential_flags.reserve(utxos.size());
    all_input_commitments.reserve(utxos.size());

    for (const auto& utxo : utxos) {
        all_amounts.push_back(utxo.value.GetUna());
        all_scriptpubkeys.push_back(utxo.spk);
        all_confidential_flags.push_back(utxo.is_confidential ? 1 : 0);
        all_input_commitments.push_back(utxo.commitment);
    }

    return consensus::ScriptExecutionContext(
        &tx,
        static_cast<uint32_t>(input_index),
        utxos[input_index].value.GetUna(),
        consensus::SCRIPT_VERIFY_TAPROOT,
        all_amounts,
        all_scriptpubkeys,
        all_confidential_flags,
        all_input_commitments
    );
}

bool HasConfidentialPrevouts(const std::vector<CanonicalWalletUTXO>& utxos) {
    return std::any_of(utxos.begin(), utxos.end(), [](const CanonicalWalletUTXO& utxo) {
        return utxo.is_confidential;
    });
}

void WriteTaprootCTPrevoutDescriptor(
    std::vector<uint8_t>& out,
    const CanonicalWalletUTXO& utxo
) {
    out.push_back(utxo.is_confidential ? 1 : 0);
    if (!utxo.is_confidential) {
        out.push_back(0x00);
        return;
    }

    if (utxo.commitment.size() < 0xFD) {
        out.push_back(static_cast<uint8_t>(utxo.commitment.size()));
    } else {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(utxo.commitment.size() & 0xff));
        out.push_back(static_cast<uint8_t>((utxo.commitment.size() >> 8) & 0xff));
    }
    out.insert(out.end(), utxo.commitment.begin(), utxo.commitment.end());
}

std::array<uint8_t, 32> ComputeConfidentialTaprootExtension(
    const std::vector<CanonicalWalletUTXO>& utxos,
    size_t input_index,
    bool anyonecanpay
) {
    std::array<uint8_t, 32> result{};
    if (!HasConfidentialPrevouts(utxos)) {
        return result;
    }

    std::vector<uint8_t> payload;
    payload.push_back(0x01);
    payload.push_back(anyonecanpay ? 0x01 : 0x00);

    if (anyonecanpay) {
        payload.push_back(0x01);
        WriteTaprootCTPrevoutDescriptor(payload, utxos[input_index]);
    } else {
        if (utxos.size() < 0xFD) {
            payload.push_back(static_cast<uint8_t>(utxos.size()));
        } else {
            payload.push_back(0xFD);
            payload.push_back(static_cast<uint8_t>(utxos.size() & 0xff));
            payload.push_back(static_cast<uint8_t>((utxos.size() >> 8) & 0xff));
        }
        for (const auto& utxo : utxos) {
            WriteTaprootCTPrevoutDescriptor(payload, utxo);
        }
    }

    const std::vector<uint8_t> hash = dinero::crypto::TaggedHash("dinero/ct-prevouts/v1", payload);
    std::copy(hash.begin(), hash.end(), result.begin());
    return result;
}

std::array<uint8_t, 32> ComposeTaprootExtensionCommitment(
    const std::array<uint8_t, 32>& ct_extension,
    const std::array<uint8_t, 32>& extra_extension
) {
    const bool has_ct =
        std::any_of(ct_extension.begin(), ct_extension.end(), [](uint8_t byte) { return byte != 0; });
    const bool has_extra =
        std::any_of(extra_extension.begin(), extra_extension.end(), [](uint8_t byte) { return byte != 0; });

    if (!has_ct) {
        return extra_extension;
    }
    if (!has_extra) {
        return ct_extension;
    }

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), ct_extension.begin(), ct_extension.end());
    payload.insert(payload.end(), extra_extension.begin(), extra_extension.end());

    std::array<uint8_t, 32> combined{};
    const auto hash = dinero::crypto::TaggedHash("dinero/sighash/ext/v1", payload);
    std::copy(hash.begin(), hash.end(), combined.begin());
    return combined;
}

}  // namespace

bool TaprootTxSigner::SignTransaction(
    Transaction& tx,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const std::vector<std::vector<uint8_t>>& private_keys) {

    if (tx.vin.size() != utxos.size() || tx.vin.size() != private_keys.size()) {
        std::cerr << "ERROR: Input count mismatch (tx inputs: " << tx.vin.size()
                  << ", utxos: " << utxos.size() << ", keys: " << private_keys.size() << ")" << std::endl;
        return false;
    }

    bool all_signed = true;
    for (size_t i = 0; i < tx.vin.size(); i++) {
        // Only sign Taproot inputs
        if (IsTaprootUTXO(utxos[i])) {
            // Pass FULL UTXO set - BIP341 requires all inputs for sighash
            if (!SignInput(tx, i, utxos, private_keys[i])) {
                std::cerr << "ERROR: Failed to sign Taproot input " << i << std::endl;
                all_signed = false;
            }
        }
    }

    return all_signed;
}

bool TaprootTxSigner::SignInput(
    Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& all_utxos,
    const std::vector<uint8_t>& private_key) {

    if (input_index >= tx.vin.size()) {
        std::cerr << "ERROR: Input index out of range" << std::endl;
        return false;
    }

    if (all_utxos.size() != tx.vin.size()) {
        std::cerr << "ERROR: UTXO count mismatch (utxos: " << all_utxos.size()
                  << ", inputs: " << tx.vin.size() << ")" << std::endl;
        return false;
    }

    const CanonicalWalletUTXO& utxo = all_utxos[input_index];

    // ═══════════════════════════════════════════════════════════════════════════
    // WALLET INVARIANT: Never sign transactions using pathless UTXOs
    // ═══════════════════════════════════════════════════════════════════════════
    // A UTXO without a derivation path is NOT owned. No exceptions.
    // Signing with an unknown path means we can't prove ownership.
    // This could lead to signing someone else's funds or irrecoverable keys.
    // ═══════════════════════════════════════════════════════════════════════════
    if (utxo.path.empty() || utxo.path.size() < 2 || utxo.path[0] != 'm' || utxo.path[1] != '/') {
        // CT (confidential) inputs may not have a derivation path — the key
        // was provided by the caller via deriveKeyForScriptPubKey(). Only warn,
        // don't refuse to sign. The private key is already validated upstream.
        if (!utxo.is_confidential) {
            std::cerr << "ERROR [SignInput] Cannot sign non-CT UTXO without derivation path" << std::endl;
            std::cerr << "  txid: " << utxo.GetTxIdHex() << std::endl;
            std::cerr << "  path: \"" << utxo.path << "\"" << std::endl;
            return false;
        }
    }

    if (!IsTaprootUTXO(utxo)) {
        std::cerr << "ERROR: UTXO is not a Taproot output" << std::endl;
        return false;
    }

    if (private_key.size() != 32) {
        std::cerr << "ERROR: Invalid private key size (expected 32 bytes)" << std::endl;
        return false;
    }

    std::array<uint8_t, 32> internal_privkey{};
    std::array<uint8_t, 32> internal_xonly_pubkey{};
    if (!ParseInternalTaprootKey(private_key, internal_privkey, internal_xonly_pubkey)) {
        std::cerr << "ERROR: Failed to parse internal Taproot key material" << std::endl;
        return false;
    }

    if (!VerifyTweakedPubkeyMatchesScript(utxo, internal_xonly_pubkey, "SignInput")) {
        return false;
    }

    // Compute sighash using the FULL UTXO set (BIP341 requirement)
    // BIP341: sighash commits to ALL input amounts and scriptPubKeys
    std::vector<uint8_t> sighash = ComputeTaprootSighash(tx, input_index, all_utxos, SIGHASH_DEFAULT);
    std::array<uint8_t, 32> sighash32{};
    if (!ParseSighash32(sighash, sighash32)) {
        std::cerr << "ERROR: Invalid sighash" << std::endl;
        return false;
    }

    std::vector<uint8_t> sig_vec;
    if (!SignTaprootKeyPath(sighash32, internal_privkey, internal_xonly_pubkey, sig_vec)) {
        return false;
    }

    // For SIGHASH_DEFAULT (0x00), we don't append the sighash byte
    // For other sighash types, append the sighash type byte
    // BIP341: "If the sighash type is SIGHASH_DEFAULT, it is omitted."

    // Store signature in witness (Taproot key-path spending has 1 witness element)
    tx.vin[input_index].witness.clear();
    tx.vin[input_index].witness.push_back(sig_vec);

    return true;
}

std::vector<uint8_t> TaprootTxSigner::ComputeTaprootSighash(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    uint8_t sighash_type) {

    if (input_index >= tx.vin.size() || utxos.size() != tx.vin.size()) {
        return {};
    }

    consensus::ScriptExecutionContext ctx = BuildTaprootExecutionContext(tx, input_index, utxos);

    return consensus::SignatureHashTaproot(ctx, sighash_type, {}, {});
}

std::vector<uint8_t> TaprootTxSigner::ComputeScriptPathSighash(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const std::vector<uint8_t>& script,
    uint8_t leaf_version,
    uint8_t sighash_type) {

    if (input_index >= tx.vin.size() || utxos.size() != tx.vin.size()) {
        return {};
    }

    std::vector<uint8_t> tapleaf_hash = ComputeTapleafHash(script, leaf_version);
    consensus::ScriptExecutionContext ctx = BuildTaprootExecutionContext(tx, input_index, utxos);

    return consensus::SignatureHashTaproot(ctx, sighash_type, tapleaf_hash, {});
}

std::vector<uint8_t> TaprootTxSigner::ComputeSighashMessage(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    uint8_t sighash_type) {

    // BIP341 signature message construction (epoch 0)
    std::vector<uint8_t> message;

    // 1. Control byte (epoch = 0, sighash_type)
    message.push_back(0x00);  // epoch
    message.push_back(sighash_type);

    // 2. nVersion (4 bytes, little-endian)
    WriteUint32LE(message, tx.version);

    // 3. nLockTime (4 bytes, little-endian)
    WriteUint32LE(message, tx.lockTime);

    // 4. sha_prevouts (32 bytes)
    auto prevouts_hash = ComputePrevoutsHash(tx);
    message.insert(message.end(), prevouts_hash.begin(), prevouts_hash.end());

    // 5. sha_amounts (32 bytes)
    auto amounts_hash = ComputeAmountsHash(utxos);
    message.insert(message.end(), amounts_hash.begin(), amounts_hash.end());

    // 6. sha_scriptpubkeys (32 bytes)
    auto scriptpubkeys_hash = ComputeScriptPubKeysHash(utxos);
    message.insert(message.end(), scriptpubkeys_hash.begin(), scriptpubkeys_hash.end());

    // 7. sha_sequences (32 bytes)
    auto sequences_hash = ComputeSequencesHash(tx);
    message.insert(message.end(), sequences_hash.begin(), sequences_hash.end());

    // 8. sha_outputs (32 bytes)
    auto outputs_hash = ComputeOutputsHash(tx);
    message.insert(message.end(), outputs_hash.begin(), outputs_hash.end());

    // 9. spend_type (1 byte) - 0x00 for key-path spending (no annex, no script)
    message.push_back(0x00);

    // 10. input_index (4 bytes, little-endian)
    WriteUint32LE(message, static_cast<uint32_t>(input_index));

    return message;
}

std::vector<uint8_t> TaprootTxSigner::ComputeScriptPathSighashMessage(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const std::vector<uint8_t>& tapleaf_hash,
    uint8_t sighash_type) {

    // BIP341 signature message construction (epoch 0) for script-path spending
    std::vector<uint8_t> message;

    // 1. Control byte (epoch = 0, sighash_type)
    message.push_back(0x00);  // epoch
    message.push_back(sighash_type);

    // 2. nVersion (4 bytes, little-endian)
    WriteUint32LE(message, tx.version);

    // 3. nLockTime (4 bytes, little-endian)
    WriteUint32LE(message, tx.lockTime);

    // 4. sha_prevouts (32 bytes)
    auto prevouts_hash = ComputePrevoutsHash(tx);
    message.insert(message.end(), prevouts_hash.begin(), prevouts_hash.end());

    // 5. sha_amounts (32 bytes)
    auto amounts_hash = ComputeAmountsHash(utxos);
    message.insert(message.end(), amounts_hash.begin(), amounts_hash.end());

    // 6. sha_scriptpubkeys (32 bytes)
    auto scriptpubkeys_hash = ComputeScriptPubKeysHash(utxos);
    message.insert(message.end(), scriptpubkeys_hash.begin(), scriptpubkeys_hash.end());

    // 7. sha_sequences (32 bytes)
    auto sequences_hash = ComputeSequencesHash(tx);
    message.insert(message.end(), sequences_hash.begin(), sequences_hash.end());

    // 8. sha_outputs (32 bytes)
    auto outputs_hash = ComputeOutputsHash(tx);
    message.insert(message.end(), outputs_hash.begin(), outputs_hash.end());

    // 9. spend_type (1 byte) - For script-path: ext_flag = 1
    // spend_type = (ext_flag << 1) + annex_present
    // ext_flag = 1 for script path (indicates extension data follows)
    // annex_present = 0 (no annex)
    message.push_back(0x02);  // (1 << 1) + 0 = 2

    // 10. input_index (4 bytes, little-endian)
    WriteUint32LE(message, static_cast<uint32_t>(input_index));

    // EXTENSION DATA (only for script-path, when ext_flag = 1):
    // 11. tapleaf_hash (32 bytes)
    message.insert(message.end(), tapleaf_hash.begin(), tapleaf_hash.end());

    // 12. key_version (1 byte) - always 0x00 for BIP342 (Tapscript)
    message.push_back(0x00);

    // 13. codesep_pos (4 bytes) - 0xffffffff if no OP_CODESEPARATOR
    WriteUint32LE(message, 0xffffffff);

    return message;
}

std::vector<uint8_t> TaprootTxSigner::ComputeTapleafHash(
    const std::vector<uint8_t>& script,
    uint8_t leaf_version) {

    // BIP341: tapleaf_hash = TaggedHash("TapLeaf", [leaf_version || compact_size(script) || script])
    std::vector<uint8_t> data;

    // 1. Leaf version (1 byte)
    data.push_back(leaf_version);

    // 2. Compact size of script
    if (script.size() < 253) {
        data.push_back(static_cast<uint8_t>(script.size()));
    } else if (script.size() <= 0xffff) {
        data.push_back(0xfd);
        data.push_back(script.size() & 0xff);
        data.push_back((script.size() >> 8) & 0xff);
    } else {
        data.push_back(0xfe);
        WriteUint32LE(data, static_cast<uint32_t>(script.size()));
    }

    // 3. Script
    data.insert(data.end(), script.begin(), script.end());

    // Return TaggedHash("TapLeaf", data)
    return TaggedHash("TapLeaf", data);
}

std::vector<uint8_t> TaprootTxSigner::ComputePrevoutsHash(const Transaction& tx) {
    std::vector<uint8_t> data;

    for (const auto& input : tx.vin) {
        // Serialize txid (32 bytes) - Phase M.4: Access underlying uint256
        const auto& txid_data = input.prevout.txid.AsUint256().data;
        data.insert(data.end(), txid_data, txid_data + 32);

        // Serialize vout (4 bytes, little-endian)
        WriteUint32LE(data, input.prevout.vout);
    }

    // BIP341: Single SHA256 (NOT double!)
    uint8_t hash[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash);

    return std::vector<uint8_t>(hash, hash + 32);
}

std::vector<uint8_t> TaprootTxSigner::ComputeAmountsHash(const std::vector<CanonicalWalletUTXO>& utxos) {
    std::vector<uint8_t> data;

    for (const auto& utxo : utxos) {
        // Phase M.6.2: Extract raw value for serialization
        WriteUint64LE(data, utxo.is_confidential ? 0 : utxo.value.GetUna());
    }

    // BIP341: Single SHA256 (NOT double!)
    uint8_t hash[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash);

    return std::vector<uint8_t>(hash, hash + 32);
}

std::vector<uint8_t> TaprootTxSigner::ComputeScriptPubKeysHash(const std::vector<CanonicalWalletUTXO>& utxos) {
    std::vector<uint8_t> data;

    for (const auto& utxo : utxos) {
        // Serialize scriptPubKey with CompactSize length prefix
        const auto& spk = utxo.spk;
        if (spk.size() < 253) {
            data.push_back(static_cast<uint8_t>(spk.size()));
        } else {
            // For larger scripts (unlikely for P2TR which is 34 bytes)
            data.push_back(0xfd);
            data.push_back(spk.size() & 0xff);
            data.push_back((spk.size() >> 8) & 0xff);
        }
        data.insert(data.end(), spk.begin(), spk.end());
    }

    // BIP341: Single SHA256 (NOT double!)
    uint8_t hash[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash);

    return std::vector<uint8_t>(hash, hash + 32);
}

std::vector<uint8_t> TaprootTxSigner::ComputeSequencesHash(const Transaction& tx) {
    std::vector<uint8_t> data;

    for (const auto& input : tx.vin) {
        WriteUint32LE(data, input.sequence);
    }

    // BIP341: Single SHA256 (NOT double!)
    uint8_t hash[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash);

    return std::vector<uint8_t>(hash, hash + 32);
}

std::vector<uint8_t> TaprootTxSigner::ComputeOutputsHash(const Transaction& tx) {
    std::vector<uint8_t> data;

    for (const auto& output : tx.vout) {
        // Serialize value (8 bytes, little-endian)
        // Phase M.6.2: Extract raw value for serialization
        WriteUint64LE(data, output.value.GetUna());

        // Serialize scriptPubKey with CompactSize length prefix
        const auto& scriptPubKey = output.scriptPubKey;  // TxOutput uses scriptPubKey (primitives layer)
        if (scriptPubKey.size() < 253) {
            data.push_back(static_cast<uint8_t>(scriptPubKey.size()));
        } else {
            data.push_back(0xfd);
            data.push_back(scriptPubKey.size() & 0xff);
            data.push_back((scriptPubKey.size() >> 8) & 0xff);
        }
        data.insert(data.end(), scriptPubKey.begin(), scriptPubKey.end());
    }

    // BIP341: Single SHA256 (NOT double!)
    uint8_t hash[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash);

    return std::vector<uint8_t>(hash, hash + 32);
}

std::vector<uint8_t> TaprootTxSigner::TaggedHash(const std::string& tag, const std::vector<uint8_t>& data) {
    return dinero::crypto::TaggedHash(tag, data);
}

std::vector<uint8_t> TaprootTxSigner::SignSchnorr(
    const std::vector<uint8_t>& message_hash,
    const std::vector<uint8_t>& private_key) {

    if (message_hash.size() != 32 || private_key.size() != 32) {
        return {};
    }

    // Create secp256k1 context
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!ctx) {
        return {};
    }

    // Verify private key
    if (!secp256k1_ec_seckey_verify(ctx, private_key.data())) {
        secp256k1_context_destroy(ctx);
        return {};
    }

    // Create keypair for Schnorr signing
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, private_key.data())) {
        secp256k1_context_destroy(ctx);
        return {};
    }

    // Generate auxiliary random data (for nonce generation)
    // SECURITY: Use cryptographically secure RNG for Schnorr nonce generation
    uint8_t aux_rand[32];
    if (RAND_bytes(aux_rand, 32) != 1) {
        secp256k1_context_destroy(ctx);
        return {};  // Failed to generate secure random bytes
    }

    // Sign with BIP340 Schnorr
    uint8_t signature[64];
    if (!secp256k1_schnorrsig_sign32(ctx, signature, message_hash.data(), &keypair, aux_rand)) {
        secp256k1_context_destroy(ctx);
        return {};
    }

    secp256k1_context_destroy(ctx);
    return std::vector<uint8_t>(signature, signature + 64);
}

bool TaprootTxSigner::IsTaprootUTXO(const CanonicalWalletUTXO& utxo) {
    // P2TR scriptPubKey: OP_1 (0x51) + push 32 bytes (0x20) + 32-byte pubkey
    const auto& spk = utxo.spk;
    return spk.size() == 34 && spk[0] == 0x51 && spk[1] == 0x20;
}

// ============================================================================
// SigHash V1: Dinero-specific sighash with extension field
// Tag: "dinero/sighash/v1" (consensus change from BIP341 "TapSighash")
// ============================================================================

std::vector<uint8_t> TaprootTxSigner::ComputeTaprootSighashV1(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const std::array<uint8_t, 32>& ext_commitment,
    uint8_t sighash_type) {

    if (input_index >= tx.vin.size()) {
        return {};
    }

    const uint8_t effective_hash_type = (sighash_type == SIGHASH_DEFAULT) ? SIGHASH_ALL : sighash_type;
    const bool anyonecanpay = (effective_hash_type & SIGHASH_ANYONECANPAY) != 0;
    const auto composed_ext = ComposeTaprootExtensionCommitment(
        ComputeConfidentialTaprootExtension(utxos, input_index, anyonecanpay),
        ext_commitment
    );

    // Compute V1 sighash message (key-path with ext_commitment)
    std::vector<uint8_t> message = ComputeSighashMessageV1(
        tx, input_index, utxos, composed_ext, sighash_type
    );

    // V1 tag: "dinero/sighash/v1" instead of "TapSighash"
    return TaggedHash("dinero/sighash/v1", message);
}

std::vector<uint8_t> TaprootTxSigner::ComputeScriptPathSighashV1(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const std::vector<uint8_t>& script,
    const std::array<uint8_t, 32>& ext_commitment,
    uint8_t leaf_version,
    uint8_t sighash_type) {

    if (input_index >= tx.vin.size()) {
        return {};
    }

    const uint8_t effective_hash_type = (sighash_type == SIGHASH_DEFAULT) ? SIGHASH_ALL : sighash_type;
    const bool anyonecanpay = (effective_hash_type & SIGHASH_ANYONECANPAY) != 0;
    const auto composed_ext = ComposeTaprootExtensionCommitment(
        ComputeConfidentialTaprootExtension(utxos, input_index, anyonecanpay),
        ext_commitment
    );

    // Compute tapleaf hash (same as BIP341)
    std::vector<uint8_t> tapleaf_hash = ComputeTapleafHash(script, leaf_version);

    // Compute V1 sighash message (script-path with ext_commitment)
    std::vector<uint8_t> message = ComputeScriptPathSighashMessageV1(
        tx, input_index, utxos, tapleaf_hash, composed_ext, sighash_type
    );

    // V1 tag: "dinero/sighash/v1"
    return TaggedHash("dinero/sighash/v1", message);
}

std::vector<uint8_t> TaprootTxSigner::ComputeSighashMessageV1(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const std::array<uint8_t, 32>& ext_commitment,
    uint8_t sighash_type) {

    // V1 key-path sighash message: BIP341 layout + 32-byte ext_commitment
    // Total: 207 bytes (175 base + 32 ext_commitment)
    std::vector<uint8_t> message;
    message.reserve(207);

    // 1. epoch (0x00)
    message.push_back(0x00);

    // 2. sighash_type
    message.push_back(sighash_type);

    // 3. nVersion (4 bytes LE)
    WriteUint32LE(message, tx.version);

    // 4. nLockTime (4 bytes LE)
    WriteUint32LE(message, tx.lockTime);

    // 5. sha_prevouts (32 bytes)
    auto prevouts_hash = ComputePrevoutsHash(tx);
    message.insert(message.end(), prevouts_hash.begin(), prevouts_hash.end());

    // 6. sha_amounts (32 bytes)
    auto amounts_hash = ComputeAmountsHash(utxos);
    message.insert(message.end(), amounts_hash.begin(), amounts_hash.end());

    // 7. sha_scriptpubkeys (32 bytes)
    auto scriptpubkeys_hash = ComputeScriptPubKeysHash(utxos);
    message.insert(message.end(), scriptpubkeys_hash.begin(), scriptpubkeys_hash.end());

    // 8. sha_sequences (32 bytes)
    auto sequences_hash = ComputeSequencesHash(tx);
    message.insert(message.end(), sequences_hash.begin(), sequences_hash.end());

    // 9. sha_outputs (32 bytes)
    auto outputs_hash = ComputeOutputsHash(tx);
    message.insert(message.end(), outputs_hash.begin(), outputs_hash.end());

    // 10. spend_type (1 byte) - 0x00 for key-path
    message.push_back(0x00);

    // 11. input_index (4 bytes LE)
    WriteUint32LE(message, static_cast<uint32_t>(input_index));

    // 12. ext_commitment (32 bytes) ← V1 EXTENSION
    message.insert(message.end(), ext_commitment.begin(), ext_commitment.end());

    return message;
}

std::vector<uint8_t> TaprootTxSigner::ComputeScriptPathSighashMessageV1(
    const Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const std::vector<uint8_t>& tapleaf_hash,
    const std::array<uint8_t, 32>& ext_commitment,
    uint8_t sighash_type) {

    // V1 script-path sighash message: BIP341 script-path layout + 32-byte ext_commitment
    // Total: 244 bytes (212 base + 32 ext_commitment)
    std::vector<uint8_t> message;
    message.reserve(244);

    // 1. epoch (0x00)
    message.push_back(0x00);

    // 2. sighash_type
    message.push_back(sighash_type);

    // 3. nVersion (4 bytes LE)
    WriteUint32LE(message, tx.version);

    // 4. nLockTime (4 bytes LE)
    WriteUint32LE(message, tx.lockTime);

    // 5-9. Same shared hashes as key-path
    auto prevouts_hash = ComputePrevoutsHash(tx);
    message.insert(message.end(), prevouts_hash.begin(), prevouts_hash.end());

    auto amounts_hash = ComputeAmountsHash(utxos);
    message.insert(message.end(), amounts_hash.begin(), amounts_hash.end());

    auto scriptpubkeys_hash = ComputeScriptPubKeysHash(utxos);
    message.insert(message.end(), scriptpubkeys_hash.begin(), scriptpubkeys_hash.end());

    auto sequences_hash = ComputeSequencesHash(tx);
    message.insert(message.end(), sequences_hash.begin(), sequences_hash.end());

    auto outputs_hash = ComputeOutputsHash(tx);
    message.insert(message.end(), outputs_hash.begin(), outputs_hash.end());

    // 10. spend_type (1 byte) - ext_flag=1 for script-path
    message.push_back(0x02);  // (1 << 1) + 0 = 2

    // 11. input_index (4 bytes LE)
    WriteUint32LE(message, static_cast<uint32_t>(input_index));

    // EXTENSION DATA (script-path):
    // 12. tapleaf_hash (32 bytes)
    message.insert(message.end(), tapleaf_hash.begin(), tapleaf_hash.end());

    // 13. key_version (1 byte) - 0x00 for BIP342 Tapscript
    message.push_back(0x00);

    // 14. codesep_pos (4 bytes) - 0xffffffff if no OP_CODESEPARATOR
    WriteUint32LE(message, 0xffffffff);

    // 15. ext_commitment (32 bytes) ← V1 EXTENSION
    message.insert(message.end(), ext_commitment.begin(), ext_commitment.end());

    return message;
}

bool TaprootTxSigner::SignInputV1(
    Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& all_utxos,
    const std::vector<uint8_t>& private_key,
    const std::array<uint8_t, 32>& ext_commitment) {

    if (input_index >= tx.vin.size()) {
        std::cerr << "ERROR: Input index out of range" << std::endl;
        return false;
    }

    if (all_utxos.size() != tx.vin.size()) {
        std::cerr << "ERROR: UTXO count mismatch" << std::endl;
        return false;
    }

    const CanonicalWalletUTXO& utxo = all_utxos[input_index];

    // Pathless UTXO invariant check — allow CT inputs (key derived from SPK)
    if (utxo.path.empty() || utxo.path.size() < 2 || utxo.path[0] != 'm' || utxo.path[1] != '/') {
        if (!utxo.is_confidential) {
            std::cerr << "ERROR [SignInputV1] Cannot sign non-CT UTXO without derivation path" << std::endl;
            return false;
        }
    }

    if (!IsTaprootUTXO(utxo)) {
        std::cerr << "ERROR: UTXO is not a Taproot output" << std::endl;
        return false;
    }

    if (private_key.size() != 32) {
        std::cerr << "ERROR: Invalid private key size" << std::endl;
        return false;
    }

    std::array<uint8_t, 32> internal_privkey{};
    std::array<uint8_t, 32> internal_xonly_pubkey{};
    if (!ParseInternalTaprootKey(private_key, internal_privkey, internal_xonly_pubkey)) {
        std::cerr << "ERROR [SignInputV1]: Failed to parse internal Taproot key material" << std::endl;
        return false;
    }

    if (!VerifyTweakedPubkeyMatchesScript(utxo, internal_xonly_pubkey, "SignInputV1")) {
        return false;
    }

    // Compute V1 sighash with ext_commitment
    std::vector<uint8_t> sighash = ComputeTaprootSighashV1(tx, input_index, all_utxos, ext_commitment, SIGHASH_DEFAULT);
    std::array<uint8_t, 32> sighash32{};
    if (!ParseSighash32(sighash, sighash32)) {
        return false;
    }

    std::vector<uint8_t> sig_vec;
    if (!SignTaprootKeyPath(sighash32, internal_privkey, internal_xonly_pubkey, sig_vec)) {
        return false;
    }

    // Store signature in witness
    tx.vin[input_index].witness.clear();
    tx.vin[input_index].witness.push_back(sig_vec);

    return true;
}

void TaprootTxSigner::WriteUint32LE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
}

void TaprootTxSigner::WriteUint64LE(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
    out.push_back((value >> 32) & 0xff);
    out.push_back((value >> 40) & 0xff);
    out.push_back((value >> 48) & 0xff);
    out.push_back((value >> 56) & 0xff);
}

} // namespace dinero
