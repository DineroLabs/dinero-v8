// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                    WALLET LAYER - BIP143 SIGNING                           ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║  Signing operations only. Sighash computation delegated to consensus.      ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#include "wallet/bip143_signer.h"
#include "wallet/transaction.h"
#include "consensus/crypto/sighash_bip143.h"  // Sighash computation (consensus)
#include <secp256k1.h>
#include <iostream>

namespace dinero {

// NOTE: ComputeSighash is now inline in the header, delegating to consensus::SighashBIP143

// Get compressed public key from private key
std::vector<uint8_t> BIP143Signer::GetPublicKey(const std::vector<uint8_t>& private_key) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, private_key.data())) {
        secp256k1_context_destroy(ctx);
        std::cerr << "ERROR: Failed to create public key" << std::endl;
        return {};
    }
    
    // Serialize as compressed (33 bytes)
    std::vector<uint8_t> pubkey_bytes(33);
    size_t len = 33;
    secp256k1_ec_pubkey_serialize(ctx, pubkey_bytes.data(), &len, &pubkey, SECP256K1_EC_COMPRESSED);
    
    secp256k1_context_destroy(ctx);
    return pubkey_bytes;
}

// Sign with secp256k1
std::vector<uint8_t> BIP143Signer::SignECDSA(
    const std::vector<uint8_t>& message_hash,
    const std::vector<uint8_t>& private_key
) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    
    secp256k1_ecdsa_signature sig;
    if (!secp256k1_ecdsa_sign(ctx, &sig, message_hash.data(), private_key.data(), nullptr, nullptr)) {
        secp256k1_context_destroy(ctx);
        std::cerr << "ERROR: ECDSA signing failed" << std::endl;
        return {};
    }
    
    // Normalize signature to low-S (BIP62)
    secp256k1_ecdsa_signature_normalize(ctx, &sig, &sig);
    
    // Extract r and s components (each 32 bytes)
    std::vector<uint8_t> compact(64);
    // Note: secp256k1 stores signature in compact form internally
    // We need to serialize it first
    uint8_t der_sig[72];
    size_t der_len = 72;
    secp256k1_ecdsa_signature_serialize_der(ctx, der_sig, &der_len, &sig);
    
    secp256k1_context_destroy(ctx);
    
    // Return DER-encoded signature
    return std::vector<uint8_t>(der_sig, der_sig + der_len);
}

// Sign a single input
bool BIP143Signer::SignInput(
    Transaction& tx,
    size_t input_index,
    const CanonicalWalletUTXO& utxo,
    const std::vector<uint8_t>& private_key
) {
    // ═══════════════════════════════════════════════════════════════════════════
    // WALLET INVARIANT: Never sign transactions using pathless UTXOs
    // ═══════════════════════════════════════════════════════════════════════════
    // A UTXO without a derivation path is NOT owned. No exceptions.
    // Signing with an unknown path means we can't prove ownership.
    // This could lead to signing someone else's funds or irrecoverable keys.
    // ═══════════════════════════════════════════════════════════════════════════
    if (utxo.path.empty() || utxo.path.size() < 2 || utxo.path[0] != 'm' || utxo.path[1] != '/') {
        std::cerr << "ERROR [SignInput] INVARIANT VIOLATION: Cannot sign UTXO without derivation path" << std::endl;
        std::cerr << "  txid: " << utxo.GetTxIdHex() << std::endl;
        std::cerr << "  vout: " << utxo.vout << std::endl;
        std::cerr << "  path: \"" << utxo.path << "\"" << std::endl;
        std::cerr << "  REFUSING TO SIGN - ownership cannot be verified!" << std::endl;
        return false;
    }

    // For P2WPKH, scriptCode is: OP_DUP OP_HASH160 <20-byte-pubkey-hash> OP_EQUALVERIFY OP_CHECKSIG
    // Extract pubkey hash from scriptPubKey (should be: OP_0 <20-byte-hash>)
    if (utxo.spk.size() != 22) {
        std::cerr << "ERROR: Invalid scriptPubKey size for P2WPKH" << std::endl;
        return false;
    }
    
    std::vector<uint8_t> pubkey_hash(utxo.spk.begin() + 2, utxo.spk.end());
    
    // Build scriptCode: OP_DUP OP_HASH160 <20> <pubkey_hash> OP_EQUALVERIFY OP_CHECKSIG
    std::vector<uint8_t> scriptCode;
    scriptCode.push_back(0x76); // OP_DUP
    scriptCode.push_back(0xa9); // OP_HASH160
    scriptCode.push_back(0x14); // 20 bytes
    scriptCode.insert(scriptCode.end(), pubkey_hash.begin(), pubkey_hash.end());
    scriptCode.push_back(0x88); // OP_EQUALVERIFY
    scriptCode.push_back(0xac); // OP_CHECKSIG
    
    // Compute sighash
    // Phase M.6.2: Extract raw value for signature hashing
    auto sighash = ComputeSighash(tx, input_index, scriptCode, utxo.value.GetUna(), SIGHASH_ALL);
    if (sighash.empty()) {
        return false;
    }
    
    // Sign the sighash
    auto signature = SignECDSA(sighash, private_key);
    if (signature.empty()) {
        return false;
    }
    
    // Append sighash type byte
    signature.push_back(SIGHASH_ALL);
    
    // Get public key
    auto pubkey = GetPublicKey(private_key);
    if (pubkey.empty()) {
        return false;
    }
    
    // Build witness: [signature] [pubkey]
    tx.vin[input_index].witness.clear();
    tx.vin[input_index].witness.push_back(signature);
    tx.vin[input_index].witness.push_back(pubkey);
    
    std::cout << "INFO: Signed input " << input_index 
              << " (sig: " << signature.size() << " bytes, pubkey: " << pubkey.size() << " bytes)" 
              << std::endl;
    
    return true;
}

// Sign all inputs in a transaction
bool BIP143Signer::SignTransaction(
    Transaction& tx,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const std::vector<std::vector<uint8_t>>& private_keys
) {
    if (tx.vin.size() != utxos.size() || tx.vin.size() != private_keys.size()) {
        std::cerr << "ERROR: Mismatched input/UTXO/key counts" << std::endl;
        return false;
    }
    
    for (size_t i = 0; i < tx.vin.size(); i++) {
        if (!SignInput(tx, i, utxos[i], private_keys[i])) {
            std::cerr << "ERROR: Failed to sign input " << i << std::endl;
            return false;
        }
    }
    
    std::cout << "✅ Successfully signed all " << tx.vin.size() << " inputs" << std::endl;
    return true;
}

} // namespace dinero
