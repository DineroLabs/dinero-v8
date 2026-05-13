#include "contracts/escrow_contract.h"
#include "crypto/hash.h"
#include "common/logger.h"
#include "wallet/transaction.h"
#include "external/bech32/bech32.hpp"
#include "consensus/chainparams.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>

namespace dinero {
namespace contracts {

// Bitcoin Script opcodes
constexpr uint8_t OP_IF = 0x63;
constexpr uint8_t OP_ELSE = 0x67;
constexpr uint8_t OP_ENDIF = 0x68;
constexpr uint8_t OP_2 = 0x52;
constexpr uint8_t OP_3 = 0x53;
constexpr uint8_t OP_CHECKMULTISIG = 0xae;
constexpr uint8_t OP_CHECKLOCKTIMEVERIFY = 0xb1;
constexpr uint8_t OP_DROP = 0x75;
constexpr uint8_t OP_CHECKSIG = 0xac;

// Helper: Convert hex string to bytes
static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtol(byteString.c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

// Helper: Convert bytes to hex string
static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    return oss.str();
}

// Helper: Generate unique contract ID
static std::string generateContractId() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(10000, 99999);

    std::ostringstream oss;
    oss << "contract_" << std::hex << timestamp << "_" << dis(gen);
    return oss.str();
}

// Encode integer as Bitcoin Script format (little-endian)
std::vector<uint8_t> EscrowContractBuilder::encodeScriptInt(uint32_t value) {
    std::vector<uint8_t> result;

    // Compact encoding for small values (OP_1 through OP_16)
    if (value >= 1 && value <= 16) {
        result.push_back(0x50 + value);  // OP_1 = 0x51, OP_2 = 0x52, etc.
        return result;
    }

    // Full encoding for larger values
    std::vector<uint8_t> bytes;
    while (value > 0) {
        bytes.push_back(value & 0xff);
        value >>= 8;
    }

    // Check if high bit is set (would be interpreted as negative)
    if (!bytes.empty() && (bytes.back() & 0x80)) {
        bytes.push_back(0x00);  // Add padding byte
    }

    // Push length prefix
    result.push_back(static_cast<uint8_t>(bytes.size()));
    result.insert(result.end(), bytes.begin(), bytes.end());

    return result;
}

// Encode public key for script
std::vector<uint8_t> EscrowContractBuilder::encodePubKey(const std::string& pubkey_hex) {
    std::vector<uint8_t> pubkey_bytes = hexToBytes(pubkey_hex);

    // Validate key size (33 bytes compressed or 65 bytes uncompressed)
    if (pubkey_bytes.size() != 33 && pubkey_bytes.size() != 65) {
        dinero::g_logger.error("[EscrowContract] Invalid pubkey size: " +
                              std::to_string(pubkey_bytes.size()));
        return {};
    }

    // Push length prefix
    std::vector<uint8_t> result;
    result.push_back(static_cast<uint8_t>(pubkey_bytes.size()));
    result.insert(result.end(), pubkey_bytes.begin(), pubkey_bytes.end());

    return result;
}

// Build 2-of-3 multisig script segment
std::vector<uint8_t> EscrowContractBuilder::buildMultisigScript(const EscrowKeys& keys) {
    std::vector<uint8_t> script;

    // OP_2
    script.push_back(OP_2);

    // Push buyer pubkey
    auto buyer_encoded = encodePubKey(keys.buyer_pubkey);
    script.insert(script.end(), buyer_encoded.begin(), buyer_encoded.end());

    // Push seller pubkey
    auto seller_encoded = encodePubKey(keys.seller_pubkey);
    script.insert(script.end(), seller_encoded.begin(), seller_encoded.end());

    // Push mediator pubkey
    auto mediator_encoded = encodePubKey(keys.mediator_pubkey);
    script.insert(script.end(), mediator_encoded.begin(), mediator_encoded.end());

    // OP_3
    script.push_back(OP_3);

    // OP_CHECKMULTISIG
    script.push_back(OP_CHECKMULTISIG);

    return script;
}

// Build timelock refund script segment
std::vector<uint8_t> EscrowContractBuilder::buildTimelockScript(
    const std::string& buyer_pubkey,
    uint32_t refund_time
) {
    std::vector<uint8_t> script;

    // Push refund time
    auto time_encoded = encodeScriptInt(refund_time);
    script.insert(script.end(), time_encoded.begin(), time_encoded.end());

    // OP_CHECKLOCKTIMEVERIFY
    script.push_back(OP_CHECKLOCKTIMEVERIFY);

    // OP_DROP
    script.push_back(OP_DROP);

    // Push buyer pubkey
    auto buyer_encoded = encodePubKey(buyer_pubkey);
    script.insert(script.end(), buyer_encoded.begin(), buyer_encoded.end());

    // OP_CHECKSIG
    script.push_back(OP_CHECKSIG);

    return script;
}

// Build complete redeem script
std::string EscrowContractBuilder::buildRedeemScript(
    const EscrowKeys& keys,
    uint32_t refund_time
) {
    std::vector<uint8_t> script;

    // OP_IF
    script.push_back(OP_IF);

    // Release path: 2-of-3 multisig
    auto multisig = buildMultisigScript(keys);
    script.insert(script.end(), multisig.begin(), multisig.end());

    // OP_ELSE
    script.push_back(OP_ELSE);

    // Refund path: timelock + single sig
    auto timelock = buildTimelockScript(keys.buyer_pubkey, refund_time);
    script.insert(script.end(), timelock.begin(), timelock.end());

    // OP_ENDIF
    script.push_back(OP_ENDIF);

    dinero::g_logger.info("[EscrowContract] Generated redeem script: " +
                         std::to_string(script.size()) + " bytes");

    return bytesToHex(script);
}

// Hash redeem script: RIPEMD160(SHA256(script))
std::string EscrowContractBuilder::hashRedeemScript(const std::string& redeem_script) {
    std::vector<uint8_t> script_bytes = hexToBytes(redeem_script);

    // Use HASH160: RIPEMD160(SHA256(data)) - Bitcoin standard
    din::crypto::Ripemd160 hash = din::crypto::HASH160(script_bytes);

    // Convert to hex
    std::vector<uint8_t> hash_vec(hash.begin(), hash.end());
    return bytesToHex(hash_vec);
}

// Create P2SH address from script hash
std::string EscrowContractBuilder::createP2SHAddress(const std::string& script_hash) {
    // TODO: Implement proper Bech32 encoding for Dinero addresses
    // For now, return a placeholder format
    // In production, this should use the same address encoding as regular Dinero addresses

    std::string address = "din1q" + script_hash.substr(0, 40);  // Placeholder

    dinero::g_logger.info("[EscrowContract] Generated P2SH address: " + address);

    return address;
}

// Helper: Create P2SH scriptPubKey from script hash
static std::vector<uint8_t> createP2SHScriptPubKey(const std::string& script_hash_hex) {
    std::vector<uint8_t> script;
    std::vector<uint8_t> hash = hexToBytes(script_hash_hex);

    // P2SH: OP_HASH160 <20-byte-hash> OP_EQUAL
    script.push_back(0xa9);  // OP_HASH160
    script.push_back(0x14);  // Push 20 bytes
    script.insert(script.end(), hash.begin(), hash.end());
    script.push_back(0x87);  // OP_EQUAL

    return script;
}

// Convert Dinero address to scriptPubKey
std::vector<uint8_t> EscrowContractBuilder::addressToScriptPubKey(const std::string& address) {
    dinero::g_logger.info("[EscrowContract] Converting address to scriptPubKey: " + address);

    // Get HRP for active network
    std::string hrp = dinero::Params().hrp;
    if (hrp.empty()) {
        hrp = "rdin";  // Fallback to regtest
        dinero::g_logger.info("[EscrowContract] Using fallback HRP: " + hrp);
    }

    // Decode bech32 address
    auto result = bech32::Decode(hrp, address);
    if (!result) {
        dinero::g_logger.error("[EscrowContract] Failed to decode address: " + address);
        return {};  // Return empty on failure
    }

    // Build scriptPubKey: OP_<version> <program>
    std::vector<uint8_t> scriptPubKey;
    scriptPubKey.push_back(static_cast<uint8_t>(result->witver));  // Witness version
    scriptPubKey.push_back(static_cast<uint8_t>(result->program.size()));  // Program length
    scriptPubKey.insert(scriptPubKey.end(), result->program.begin(), result->program.end());

    dinero::g_logger.info("[EscrowContract] ScriptPubKey created successfully (" +
                          std::to_string(scriptPubKey.size()) + " bytes)");

    return scriptPubKey;
}

// Build complete contract
EscrowContract EscrowContractBuilder::buildContract(
    const EscrowKeys& keys,
    double amount,
    uint32_t refund_blocks,
    EscrowType type,
    uint32_t seller_window_blocks,
    uint32_t current_height
) {
    EscrowContract contract;

    // Generate contract ID
    contract.contract_id = generateContractId();

    // Set contract type
    contract.type = type;

    // Store keys
    contract.keys = keys;

    // Store amount
    contract.amount = amount;

    // Store seller window
    contract.seller_window_blocks = seller_window_blocks;

    // Store creation height
    contract.created_height = current_height;

    // Calculate refund time (current height + refund_blocks)
    contract.refund_time = current_height + refund_blocks;

    // Build redeem script based on type
    if (type == EscrowType::TwoOfTwo) {
        // TODO: Build 2-of-2 multisig script (no mediator)
        // For now, use the 2-of-3 script with empty mediator as placeholder
        dinero::g_logger.warning("[EscrowContract] 2-of-2 script not yet implemented, using 2-of-3");
        contract.redeem_script = buildRedeemScript(keys, contract.refund_time);
    } else {
        // 2-of-3 multisig (both Auto and Manual)
        contract.redeem_script = buildRedeemScript(keys, contract.refund_time);
    }

    // Hash redeem script
    contract.script_hash = hashRedeemScript(contract.redeem_script);

    // Create P2SH address
    contract.p2sh_address = createP2SHAddress(contract.script_hash);

    // Set initial state
    contract.lock_txid = "";  // Set when funds are locked
    contract.lock_vout = 0;   // Set when funds are locked
    contract.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    contract.status = "pending";  // Waiting for funding
    contract.confirmations = 0;

    std::string type_str;
    switch (type) {
        case EscrowType::TwoOfTwo:
            type_str = "2-of-2";
            break;
        case EscrowType::TwoOfThreeAuto:
            type_str = "2-of-3 (auto)";
            break;
        case EscrowType::TwoOfThreeManual:
            type_str = "2-of-3 (manual)";
            break;
    }

    dinero::g_logger.info("[EscrowContract] Built " + type_str + " contract: " + contract.contract_id);
    dinero::g_logger.info("[EscrowContract]   P2SH Address: " + contract.p2sh_address);
    dinero::g_logger.info("[EscrowContract]   Amount: " + std::to_string(amount) + " DIN");
    dinero::g_logger.info("[EscrowContract]   Refund Time: " +
                         std::to_string(contract.refund_time) + " (height)");
    dinero::g_logger.info("[EscrowContract]   Seller Window: " +
                         std::to_string(seller_window_blocks) + " blocks");

    return contract;
}

// Create lock transaction (buyer sends to P2SH)
std::string EscrowContractBuilder::createLockTransaction(
    const EscrowContract& contract,
    const std::string& from_address
) {
    // TODO: Implement actual transaction building
    // This requires integration with Dinero's transaction builder

    dinero::g_logger.info("[EscrowContract] Creating lock transaction");
    dinero::g_logger.info("[EscrowContract]   From: " + from_address);
    dinero::g_logger.info("[EscrowContract]   To: " + contract.p2sh_address);
    dinero::g_logger.info("[EscrowContract]   Amount: " + std::to_string(contract.amount));

    // Placeholder: Return empty string for now
    // In production, this should build a proper transaction
    return "";
}

// Create release transaction (2-of-3 multisig spend)
std::string EscrowContractBuilder::createReleaseTransaction(
    const EscrowContract& contract,
    const std::string& to_address,
    const std::string& sig_buyer,
    const std::string& sig_seller
) {
    dinero::g_logger.info("[EscrowContract] Creating release transaction");
    dinero::g_logger.info("[EscrowContract]   To: " + to_address);
    dinero::g_logger.info("[EscrowContract]   Amount: " + std::to_string(contract.amount));

    if (contract.lock_txid.empty()) {
        dinero::g_logger.error("[EscrowContract] Contract not funded (no lock_txid)");
        return "";
    }

    // Build transaction
    dinero::Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;  // No locktime for release
    tx.witness_version = 0;  // SegWit v0 (P2WPKH)

    // Input: Spend from P2SH
    dinero::TxInput input;
    input.prevout.txid = dinero::TxId(dinero::uint256::FromHexUnsafe(contract.lock_txid));  // Phase M.4: Wrap in TxId
    input.prevout.vout = 0;  // Assume first output (TODO: find correct vout)
    input.sequence = 0xfffffffe;  // Enable RBF
    input.scriptSig = {};  // Empty for witness

    // Witness stack for 2-of-3 multisig (IF branch)
    // Stack: <sig1> <sig2> <1> <redeemScript>
    input.witness.push_back({0x00});  // OP_0 for CHECKMULTISIG bug
    input.witness.push_back(hexToBytes(sig_buyer));
    input.witness.push_back(hexToBytes(sig_seller));
    input.witness.push_back({0x01});  // OP_TRUE (take IF branch)
    input.witness.push_back(hexToBytes(contract.redeem_script));

    tx.vin.push_back(input);

    // Output: Send to recipient (seller)
    dinero::TxOutput output;
    int64_t amount_sats = static_cast<int64_t>(contract.amount * 100000000);
    output.value = AmountUna::Una(amount_sats - 1000);  // Subtract 1000 sat fee

    // Convert address to scriptPubKey
    output.scriptPubKey = addressToScriptPubKey(to_address);
    if (output.scriptPubKey.empty()) {
        dinero::g_logger.error("[EscrowContract] Failed to convert address to scriptPubKey");
        return "";
    }

    tx.vout.push_back(output);

    std::string tx_hex = tx.SerializeHex();
    dinero::g_logger.info("[EscrowContract] Release TX: " + tx_hex);

    return tx_hex;
}

// Create refund transaction (timelock spend)
std::string EscrowContractBuilder::createRefundTransaction(
    const EscrowContract& contract,
    const std::string& refund_address,
    const std::string& sig_buyer
) {
    dinero::g_logger.info("[EscrowContract] Creating refund transaction");
    dinero::g_logger.info("[EscrowContract]   To: " + refund_address);
    dinero::g_logger.info("[EscrowContract]   Amount: " + std::to_string(contract.amount));
    dinero::g_logger.info("[EscrowContract]   Refund Time: " + std::to_string(contract.refund_time));

    if (contract.lock_txid.empty()) {
        dinero::g_logger.error("[EscrowContract] Contract not funded (no lock_txid)");
        return "";
    }

    // Build transaction
    dinero::Transaction tx;
    tx.version = 2;
    tx.lockTime = contract.refund_time;  // CRITICAL: Set locktime for CLTV!
    tx.witness_version = 0;  // SegWit v0 (P2WPKH)

    // Input: Spend from P2SH
    dinero::TxInput input;
    input.prevout.txid = dinero::TxId(dinero::uint256::FromHexUnsafe(contract.lock_txid));  // Phase M.4: Wrap in TxId
    input.prevout.vout = 0;  // Assume first output
    input.sequence = 0xfffffffe;  // Enable RBF
    input.scriptSig = {};  // Empty for witness

    // Witness stack for timelock refund (ELSE branch)
    // Stack: <sig> <0> <redeemScript>
    input.witness.push_back(hexToBytes(sig_buyer));
    input.witness.push_back({0x00});  // OP_FALSE (take ELSE branch)
    input.witness.push_back(hexToBytes(contract.redeem_script));

    tx.vin.push_back(input);

    // Output: Refund to buyer
    dinero::TxOutput output;
    int64_t amount_sats = static_cast<int64_t>(contract.amount * 100000000);
    output.value = AmountUna::Una(amount_sats - 1000);  // Subtract 1000 sat fee

    // Convert address to scriptPubKey
    output.scriptPubKey = addressToScriptPubKey(refund_address);
    if (output.scriptPubKey.empty()) {
        dinero::g_logger.error("[EscrowContract] Failed to convert address to scriptPubKey");
        return "";
    }

    tx.vout.push_back(output);

    std::string tx_hex = tx.SerializeHex();
    dinero::g_logger.info("[EscrowContract] Refund TX: " + tx_hex);
    dinero::g_logger.info("[EscrowContract] ⚠️  Note: Can only be broadcast after block " +
                         std::to_string(contract.refund_time));

    return tx_hex;
}

// Verify contract is valid
bool EscrowContractBuilder::verifyContract(const EscrowContract& contract) {
    // Basic validation
    if (contract.contract_id.empty()) {
        dinero::g_logger.error("[EscrowContract] Invalid contract: empty ID");
        return false;
    }

    if (contract.amount <= 0) {
        dinero::g_logger.error("[EscrowContract] Invalid contract: non-positive amount");
        return false;
    }

    if (contract.redeem_script.empty()) {
        dinero::g_logger.error("[EscrowContract] Invalid contract: empty redeem script");
        return false;
    }

    if (contract.p2sh_address.empty()) {
        dinero::g_logger.error("[EscrowContract] Invalid contract: empty P2SH address");
        return false;
    }

    // Verify script hash matches redeem script
    std::string computed_hash = hashRedeemScript(contract.redeem_script);
    if (computed_hash != contract.script_hash) {
        dinero::g_logger.error("[EscrowContract] Invalid contract: script hash mismatch");
        return false;
    }

    dinero::g_logger.info("[EscrowContract] Contract verified: " + contract.contract_id);
    return true;
}

} // namespace contracts
} // namespace dinero
