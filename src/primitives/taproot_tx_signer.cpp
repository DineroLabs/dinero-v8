#include "primitives/taproot_tx_signer.h"
#include "crypto/tagged_hash.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <openssl/rand.h>  // For RAND_bytes (cryptographically secure RNG)
#include <cstring>
#include <iostream>

namespace dinero {

bool TaprootTxSigner::SignTransaction(
    Transaction& tx,
    const std::vector<SigningUTXO>& utxos,
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
            if (!SignInput(tx, i, utxos[i], private_keys[i])) {
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
    const SigningUTXO& utxo,
    const std::vector<uint8_t>& private_key) {

    if (input_index >= tx.vin.size()) {
        std::cerr << "ERROR: Input index out of range" << std::endl;
        return false;
    }

    if (!IsTaprootUTXO(utxo)) {
        std::cerr << "ERROR: UTXO is not a Taproot output" << std::endl;
        return false;
    }

    if (private_key.size() != 32) {
        std::cerr << "ERROR: Invalid private key size (expected 32 bytes)" << std::endl;
        return false;
    }

    // Compute sighash
    std::vector<SigningUTXO> all_utxos(tx.vin.size());
    all_utxos[input_index] = utxo;

    std::vector<uint8_t> sighash = ComputeTaprootSighash(tx, input_index, all_utxos, SIGHASH_DEFAULT);
    if (sighash.size() != 32) {
        std::cerr << "ERROR: Invalid sighash" << std::endl;
        return false;
    }

    // Sign with Schnorr
    std::vector<uint8_t> signature = SignSchnorr(sighash, private_key);
    if (signature.empty()) {
        std::cerr << "ERROR: Schnorr signing failed" << std::endl;
        return false;
    }

    // For SIGHASH_DEFAULT (0x00), we don't append the sighash byte
    // For other sighash types, append the sighash type byte
    // BIP341: "If the sighash type is SIGHASH_DEFAULT, it is omitted."

    // Store signature in witness (Taproot key-path spending has 1 witness element)
    tx.vin[input_index].witness.clear();
    tx.vin[input_index].witness.push_back(signature);

    return true;
}

std::vector<uint8_t> TaprootTxSigner::ComputeTaprootSighash(
    const Transaction& tx,
    size_t input_index,
    const std::vector<SigningUTXO>& utxos,
    uint8_t sighash_type) {

    if (input_index >= tx.vin.size()) {
        return {};
    }

    // Compute sighash message according to BIP341
    std::vector<uint8_t> message = ComputeSighashMessage(tx, input_index, utxos, sighash_type);

    // Return tagged hash: TapSighash
    return TaggedHash("TapSighash", message);
}

std::vector<uint8_t> TaprootTxSigner::ComputeSighashMessage(
    const Transaction& tx,
    size_t input_index,
    const std::vector<SigningUTXO>& utxos,
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

std::vector<uint8_t> TaprootTxSigner::ComputePrevoutsHash(const Transaction& tx) {
    std::vector<uint8_t> data;

    for (const auto& input : tx.vin) {
        // Serialize txid (32 bytes) - Phase M.4.3-B: Unwrap TxId to uint256
        const auto& txid_bytes = input.prevout.txid.AsUint256();
        data.insert(data.end(), txid_bytes.data, txid_bytes.data + 32);

        // Serialize vout (4 bytes, little-endian)
        WriteUint32LE(data, input.prevout.vout);
    }

    // Single SHA256 (BIP 341 uses single SHA256 for intermediate hashes)
    uint8_t hash[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash);

    return std::vector<uint8_t>(hash, hash + 32);
}

std::vector<uint8_t> TaprootTxSigner::ComputeAmountsHash(const std::vector<SigningUTXO>& utxos) {
    std::vector<uint8_t> data;

    for (const auto& utxo : utxos) {
        // Phase M.6.2: Extract raw value for serialization
        WriteUint64LE(data, utxo.value.GetUna());
    }

    // Single SHA256 (BIP 341 uses single SHA256 for intermediate hashes)
    uint8_t hash[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash);

    return std::vector<uint8_t>(hash, hash + 32);
}

std::vector<uint8_t> TaprootTxSigner::ComputeScriptPubKeysHash(const std::vector<SigningUTXO>& utxos) {
    std::vector<uint8_t> data;

    for (const auto& utxo : utxos) {
        // Serialize scriptPubKey with CompactSize length prefix
        const auto& spk = utxo.scriptPubKey;
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

    // Single SHA256 (BIP 341 uses single SHA256 for intermediate hashes)
    uint8_t hash[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash);

    return std::vector<uint8_t>(hash, hash + 32);
}

std::vector<uint8_t> TaprootTxSigner::ComputeSequencesHash(const Transaction& tx) {
    std::vector<uint8_t> data;

    for (const auto& input : tx.vin) {
        WriteUint32LE(data, input.sequence);
    }

    // Single SHA256 (BIP 341 uses single SHA256 for intermediate hashes)
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
        const auto& spk = output.scriptPubKey;
        if (spk.size() < 253) {
            data.push_back(static_cast<uint8_t>(spk.size()));
        } else {
            data.push_back(0xfd);
            data.push_back(spk.size() & 0xff);
            data.push_back((spk.size() >> 8) & 0xff);
        }
        data.insert(data.end(), spk.begin(), spk.end());
    }

    // Single SHA256 (BIP 341 uses single SHA256 for intermediate hashes)
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

bool TaprootTxSigner::IsTaprootUTXO(const SigningUTXO& utxo) {
    // P2TR scriptPubKey: OP_1 (0x51) + push 32 bytes (0x20) + 32-byte pubkey
    const auto& spk = utxo.scriptPubKey;
    return spk.size() == 34 && spk[0] == 0x51 && spk[1] == 0x20;
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
