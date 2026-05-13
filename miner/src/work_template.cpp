#include "solo_miner/work_template.h"
#include "solo_miner/hash_engine.h"
#include "solo_miner/types.h"
#include <cstring>
#include <algorithm>
#include <cctype>

namespace dinero {
namespace solo {

namespace {

bool parseHexU32Strict(const std::string& hex, uint32_t& out) {
    if (hex.empty() || hex.size() > 8) {
        return false;
    }

    uint32_t value = 0;
    for (char c : hex) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isxdigit(uc)) {
            return false;
        }
        value <<= 4;
        if (uc >= '0' && uc <= '9') value |= static_cast<uint32_t>(uc - '0');
        else if (uc >= 'a' && uc <= 'f') value |= static_cast<uint32_t>(uc - 'a' + 10);
        else value |= static_cast<uint32_t>(uc - 'A' + 10);
    }

    out = value;
    return true;
}

} // namespace

std::optional<WorkTemplate> WorkTemplate::fromJson(const nlohmann::json& json) {
    WorkTemplate tmpl;

    try {
        auto isHexString = [](const std::string& s) {
            if (s.empty()) return false;
            for (char c : s) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            }
            return true;
        };
        auto isHexLen = [&](const std::string& s, size_t len) {
            return s.size() == len && isHexString(s);
        };

        // Required fields - accept both "bits" and "difficulty" for compatibility
        if (!json.contains("previousblockhash") || !json.contains("height") ||
            !json.contains("curtime")) {
            return std::nullopt;
        }

        // Must have either "bits" or "difficulty"
        if (!json.contains("bits") && !json.contains("difficulty")) {
            return std::nullopt;
        }

        tmpl.prev_hash = json["previousblockhash"].get<std::string>();
        tmpl.height = json["height"].get<uint32_t>();
        tmpl.timestamp = json["curtime"].get<uint64_t>();

        // Difficulty/bits is hex string - try both field names
        std::string diff_hex;
        if (json.contains("difficulty")) {
            diff_hex = json["difficulty"].get<std::string>();
        } else if (json.contains("bits")) {
            diff_hex = json["bits"].get<std::string>();
        }
        if (!parseHexU32Strict(diff_hex, tmpl.difficulty_bits)) {
            return std::nullopt;
        }

        // Version (optional, default 1)
        tmpl.version = json.value("version", 1);

        // Target (required for mining comparison)
        if (json.contains("target")) {
            tmpl.target_hex = json["target"].get<std::string>();
            auto target_bytes = hexToBytes(tmpl.target_hex);
            if (target_bytes.size() == 32) {
                std::copy(target_bytes.begin(), target_bytes.end(), tmpl.target.begin());
            } else {
                // Compute from difficulty_bits
                tmpl.target = compactToTarget(tmpl.difficulty_bits);
            }
        } else {
            tmpl.target = compactToTarget(tmpl.difficulty_bits);
        }

        // Utreexo commitment is consensus-critical (must be present and valid)
        if (!json.contains("utreexocommitment") || !json["utreexocommitment"].is_string()) {
            return std::nullopt;
        }
        tmpl.utreexo_root = json["utreexocommitment"].get<std::string>();
        if (!isHexLen(tmpl.utreexo_root, 64)) {
            return std::nullopt;
        }

        // Coinbase value
        tmpl.coinbase_value = json.value("coinbasevalue", uint64_t(0));

        // Coinbase transaction is required (daemon-owned canonical coinbase)
        std::string coinbase_txid;
        if (!json.contains("coinbasetxn") || !json["coinbasetxn"].is_object()) {
            return std::nullopt;
        }
        auto& cbtxn = json["coinbasetxn"];
        if (!cbtxn.contains("data") || !cbtxn["data"].is_string() ||
            !cbtxn.contains("txid") || !cbtxn["txid"].is_string()) {
            return std::nullopt;
        }
        tmpl.coinbase_txn_hex = cbtxn["data"].get<std::string>();
        coinbase_txid = cbtxn["txid"].get<std::string>();
        if (tmpl.coinbase_txn_hex.empty() ||
            tmpl.coinbase_txn_hex.size() % 2 != 0 ||
            !isHexString(tmpl.coinbase_txn_hex)) {
            return std::nullopt;
        }
        if (!isHexLen(coinbase_txid, 64)) {
            return std::nullopt;
        }

        // Collect all transaction txids for merkle root computation
        std::vector<Hash256> txids;

        // Add coinbase txid first
        Hash256 cb_txid{};
        auto txid_bytes = hexToBytes(coinbase_txid);
        if (txid_bytes.size() != 32) {
            return std::nullopt;
        }
        // txid is displayed in big-endian, but merkle uses little-endian
        for (int i = 0; i < 32; i++) {
            cb_txid[i] = txid_bytes[31 - i];
        }
        txids.push_back(cb_txid);

        // Transactions (excluding coinbase) - collect data and txids
        if (json.contains("transactions") && json["transactions"].is_array()) {
            for (const auto& tx : json["transactions"]) {
                if (tx.contains("data")) {
                    tmpl.transactions.push_back(tx["data"].get<std::string>());
                }
                if (!tx.contains("txid") || !tx["txid"].is_string()) {
                    return std::nullopt;
                }
                std::string txid_str = tx["txid"].get<std::string>();
                if (!isHexLen(txid_str, 64)) {
                    return std::nullopt;
                }
                Hash256 txid{};
                auto txid_bytes = hexToBytes(txid_str);
                if (txid_bytes.size() != 32) {
                    return std::nullopt;
                }
                for (int i = 0; i < 32; i++) {
                    txid[i] = txid_bytes[31 - i];
                }
                txids.push_back(txid);
            }
        }

        // Compute merkle root from txids
        if (txids.empty()) {
            return std::nullopt;
        }
        Hash256 merkle = HashEngine::computeMerkleRoot(txids);
        // Convert back to hex (big-endian display format)
        Hash256 merkle_be;
        for (int i = 0; i < 32; i++) {
            merkle_be[i] = merkle[31 - i];
        }
        tmpl.merkle_root = bytesToHex(merkle_be.data(), 32);

        // Optional: BIP22/BIP23 tip token. Absent from older daemons.
        // When present, the miner echoes it back on subsequent
        // getblocktemplate calls to enable server-side longpoll.
        if (json.contains("longpollid") && json["longpollid"].is_string()) {
            tmpl.longpollid = json["longpollid"].get<std::string>();
        }

        return tmpl;

    } catch (const std::exception& e) {
        return std::nullopt;
    }
}

std::array<uint8_t, HEADER_SIZE> WorkTemplate::buildHeader(uint32_t nonce) const {
    std::array<uint8_t, HEADER_SIZE> header{};

    // Version (4 bytes, little-endian)
    header[0] = version & 0xFF;
    header[1] = (version >> 8) & 0xFF;
    header[2] = (version >> 16) & 0xFF;
    header[3] = (version >> 24) & 0xFF;

    // Previous block hash (32 bytes) - hex string to bytes, reversed
    auto prev_bytes = hexToBytes(prev_hash);
    if (prev_bytes.size() == 32) {
        // Reverse for internal representation
        for (int i = 0; i < 32; i++) {
            header[4 + i] = prev_bytes[31 - i];
        }
    }

    // Merkle root (32 bytes) - already computed, need to reverse from display format
    if (!merkle_root.empty()) {
        auto merkle_bytes = hexToBytes(merkle_root);
        if (merkle_bytes.size() == 32) {
            for (int i = 0; i < 32; i++) {
                header[36 + i] = merkle_bytes[31 - i];
            }
        }
    }

    // Utreexo root (32 bytes)
    auto utreexo_bytes = hexToBytes(utreexo_root);
    if (utreexo_bytes.size() == 32) {
        for (int i = 0; i < 32; i++) {
            header[68 + i] = utreexo_bytes[31 - i];
        }
    }

    // Timestamp (8 bytes, little-endian)
    for (int i = 0; i < 8; i++) {
        header[100 + i] = (timestamp >> (i * 8)) & 0xFF;
    }

    // Difficulty (4 bytes, little-endian)
    header[108] = difficulty_bits & 0xFF;
    header[109] = (difficulty_bits >> 8) & 0xFF;
    header[110] = (difficulty_bits >> 16) & 0xFF;
    header[111] = (difficulty_bits >> 24) & 0xFF;

    // Nonce (4 bytes, little-endian)
    header[112] = nonce & 0xFF;
    header[113] = (nonce >> 8) & 0xFF;
    header[114] = (nonce >> 16) & 0xFF;
    header[115] = (nonce >> 24) & 0xFF;

    // Reserved (12 bytes) - must be zero (already initialized to 0)

    return header;
}

std::string WorkTemplate::buildBlock(uint32_t nonce) const {
    // Build complete block: header + tx count + transactions
    std::string block_hex;

    // Header (128 bytes = 256 hex chars)
    auto header = buildHeader(nonce);
    block_hex += bytesToHex(header.data(), HEADER_SIZE);

    // Transaction count (varint)
    size_t tx_count = 1 + transactions.size(); // coinbase + regular txs

    if (tx_count < 0xFD) {
        uint8_t count8 = static_cast<uint8_t>(tx_count);
        block_hex += bytesToHex(&count8, 1);
    } else if (tx_count <= 0xFFFF) {
        uint8_t marker = 0xFD;
        block_hex += bytesToHex(&marker, 1);
        uint16_t count16 = static_cast<uint16_t>(tx_count);
        block_hex += bytesToHex(reinterpret_cast<const uint8_t*>(&count16), 2);
    } else {
        uint8_t marker = 0xFE;
        block_hex += bytesToHex(&marker, 1);
        uint32_t count32 = static_cast<uint32_t>(tx_count);
        block_hex += bytesToHex(reinterpret_cast<const uint8_t*>(&count32), 4);
    }

    // Coinbase transaction
    block_hex += coinbase_txn_hex;

    // Regular transactions
    for (const auto& tx : transactions) {
        block_hex += tx;
    }

    return block_hex;
}

bool WorkTemplate::isValid() const {
    return !prev_hash.empty() &&
           prev_hash.length() == 64 &&
           !merkle_root.empty() &&
           merkle_root.length() == 64 &&
           !utreexo_root.empty() &&
           utreexo_root.length() == 64 &&
           !coinbase_txn_hex.empty() &&
           height > 0 &&
           timestamp > 0 &&
           difficulty_bits != 0;
}

} // namespace solo
} // namespace dinero
