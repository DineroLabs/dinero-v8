#include "consensus/block_undo.h"
#if defined(__APPLE__) || defined(_WIN32)
#include <json/json.h>
#else
#include <json/json.h>
#endif
#include <cstring>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace consensus {

namespace {

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(byte);
    }
    return oss.str();
}

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

} // namespace

std::string BlockUndo::ToJson() const {
    Json::Value root;
    root["height"] = height;
    root["block_hash"] = block_hash.GetHex();

    Json::Value spent_array(Json::arrayValue);
    for (const auto& entry : spent_coins) {
        Json::Value coin_obj;
        coin_obj["txid"] = entry.txid.GetHex();
        coin_obj["vout"] = entry.vout;
        // Phase M.6.2: Extract raw value for JSON serialization
        coin_obj["value"] = static_cast<Json::Int64>(entry.coin.value.GetInt64());
        coin_obj["height"] = entry.coin.height;
        coin_obj["is_coinbase"] = entry.coin.isCoinbase;

        // Serialize scriptPubKey as hex
        std::ostringstream spk_hex;
        for (uint8_t byte : entry.coin.scriptPubKey) {
            spk_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        coin_obj["script_pubkey"] = spk_hex.str();

        // Dragon #1 Fix: Serialize confidential data to JSON
        coin_obj["is_confidential"] = entry.coin.is_confidential;
        if (entry.coin.is_confidential && !entry.coin.commitment.empty()) {
            std::ostringstream commit_hex;
            for (uint8_t byte : entry.coin.commitment) {
                commit_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
            }
            coin_obj["commitment"] = commit_hex.str();
        }

        spent_array.append(coin_obj);
    }
    root["spent_coins"] = spent_array;

    if (pre_block_shielded_frontier.has_value()) {
        root["pre_block_shielded_frontier"] = BytesToHex(*pre_block_shielded_frontier);
    }

    if (pre_reset_shielded_epoch.has_value()) {
        Json::Value ep;
        ep["tree_frontier"]  = BytesToHex(pre_reset_shielded_epoch->tree_frontier);
        ep["anchor_history"] = BytesToHex(pre_reset_shielded_epoch->anchor_history);
        ep["nullifiers"]     = BytesToHex(pre_reset_shielded_epoch->nullifiers);
        root["pre_reset_shielded_epoch"] = ep;
    }

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";  // Compact JSON
    return Json::writeString(writer, root);
}

BlockUndo BlockUndo::FromJson(const std::string& json_str) {
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::istringstream stream(json_str);
    std::string errs;
    
    if (!Json::parseFromStream(reader, stream, &root, &errs)) {
        throw std::runtime_error("Failed to parse BlockUndo JSON: " + errs);
    }
    
    BlockUndo undo;
    undo.height = root["height"].asUInt();
    undo.block_hash = uint256::FromHexUnsafe(root["block_hash"].asString());

    const Json::Value& spent_array = root["spent_coins"];
    for (const auto& coin_obj : spent_array) {
        UndoEntry entry;
        entry.txid = uint256::FromHexUnsafe(coin_obj["txid"].asString());
        entry.vout = coin_obj["vout"].asUInt();

        // Phase M.6.2: Wrap raw value in AmountUna (ensure non-negative)
        int64_t raw_value = coin_obj["value"].asInt64();
        entry.coin.value = AmountUna::Una(static_cast<uint64_t>(raw_value));
        entry.coin.height = coin_obj["height"].asInt();
        entry.coin.isCoinbase = coin_obj.get("is_coinbase", false).asBool();

        // Deserialize scriptPubKey from hex
        std::string spk_hex = coin_obj["script_pubkey"].asString();
        entry.coin.scriptPubKey.clear();
        for (size_t i = 0; i < spk_hex.length(); i += 2) {
            std::string byte_str = spk_hex.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
            entry.coin.scriptPubKey.push_back(byte);
        }

        // Dragon #1 Fix: Deserialize confidential data from JSON
        entry.coin.is_confidential = coin_obj.get("is_confidential", false).asBool();
        if (entry.coin.is_confidential && coin_obj.isMember("commitment")) {
            std::string commit_hex = coin_obj["commitment"].asString();
            entry.coin.commitment.clear();
            for (size_t i = 0; i < commit_hex.length(); i += 2) {
                std::string byte_str = commit_hex.substr(i, 2);
                uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
                entry.coin.commitment.push_back(byte);
            }
        }

        undo.spent_coins.push_back(entry);
    }

    if (root.isMember("pre_block_shielded_frontier")) {
        undo.pre_block_shielded_frontier =
            HexToBytes(root["pre_block_shielded_frontier"].asString());
    }

    if (root.isMember("pre_reset_shielded_epoch")) {
        const Json::Value& ep = root["pre_reset_shielded_epoch"];
        shielded::ShieldedEpochSnapshot snap;
        snap.tree_frontier  = HexToBytes(ep["tree_frontier"].asString());
        snap.anchor_history = HexToBytes(ep["anchor_history"].asString());
        snap.nullifiers     = HexToBytes(ep["nullifiers"].asString());
        undo.pre_reset_shielded_epoch = std::move(snap);
    }

    return undo;
}

// Binary serialization helpers
static void WriteUInt32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(value & 0xFF);
    data.push_back((value >> 8) & 0xFF);
    data.push_back((value >> 16) & 0xFF);
    data.push_back((value >> 24) & 0xFF);
}

static void WriteUInt64(std::vector<uint8_t>& data, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        data.push_back((value >> (i * 8)) & 0xFF);
    }
}

static void WriteString(std::vector<uint8_t>& data, const std::string& str) {
    WriteUInt32(data, static_cast<uint32_t>(str.size()));
    data.insert(data.end(), str.begin(), str.end());
}

static void WriteBytes(std::vector<uint8_t>& data, const std::vector<uint8_t>& bytes) {
    WriteUInt32(data, static_cast<uint32_t>(bytes.size()));
    data.insert(data.end(), bytes.begin(), bytes.end());
}

static void WriteUint256(std::vector<uint8_t>& data, const uint256& hash) {
    data.insert(data.end(), hash.data, hash.data + 32);
}

static uint32_t ReadUInt32(const uint8_t*& ptr) {
    uint32_t value = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    ptr += 4;
    return value;
}

static uint64_t ReadUInt64(const uint8_t*& ptr) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= static_cast<uint64_t>(ptr[i]) << (i * 8);
    }
    ptr += 8;
    return value;
}

static std::string ReadString(const uint8_t*& ptr) {
    uint32_t len = ReadUInt32(ptr);
    std::string str(reinterpret_cast<const char*>(ptr), len);
    ptr += len;
    return str;
}

static std::vector<uint8_t> ReadBytes(const uint8_t*& ptr) {
    uint32_t len = ReadUInt32(ptr);
    std::vector<uint8_t> bytes(ptr, ptr + len);
    ptr += len;
    return bytes;
}

static uint256 ReadUint256(const uint8_t*& ptr) {
    uint256 hash;
    std::memcpy(hash.data, ptr, 32);
    ptr += 32;
    return hash;
}

std::vector<uint8_t> BlockUndo::Serialize() const {
    std::vector<uint8_t> data;

    // Header
    WriteUInt32(data, height);
    WriteUint256(data, block_hash);
    WriteUInt32(data, static_cast<uint32_t>(spent_coins.size()));

    // Spent coins
    for (const auto& entry : spent_coins) {
        WriteUint256(data, entry.txid);
        WriteUInt32(data, entry.vout);
        // Phase M.6.2: Extract raw value for binary serialization
        WriteUInt64(data, entry.coin.value.GetUna());
        WriteUInt32(data, static_cast<uint32_t>(entry.coin.height));
        data.push_back(entry.coin.isCoinbase ? 1 : 0);
        WriteBytes(data, entry.coin.scriptPubKey);

        // ═══════════════════════════════════════════════════════════════════════
        // Dragon #1 Fix: Serialize confidential transaction data
        // Without this, confidential UTXOs become transparent after reorg!
        // ═══════════════════════════════════════════════════════════════════════
        data.push_back(entry.coin.is_confidential ? 1 : 0);
        if (entry.coin.is_confidential) {
            WriteBytes(data, entry.coin.commitment);
        }
    }

    data.push_back(pre_block_shielded_frontier.has_value() ? 1 : 0);
    if (pre_block_shielded_frontier.has_value()) {
        WriteBytes(data, *pre_block_shielded_frontier);
    }

    // Shielded epoch reset snapshot (present only on the reset block). Appended
    // after the frontier so old undo records — which end after the frontier —
    // deserialize with this left as nullopt.
    data.push_back(pre_reset_shielded_epoch.has_value() ? 1 : 0);
    if (pre_reset_shielded_epoch.has_value()) {
        WriteBytes(data, pre_reset_shielded_epoch->tree_frontier);
        WriteBytes(data, pre_reset_shielded_epoch->anchor_history);
        WriteBytes(data, pre_reset_shielded_epoch->nullifiers);
    }

    return data;
}

BlockUndo BlockUndo::Deserialize(const std::vector<uint8_t>& data) {
    const uint8_t* ptr = data.data();
    const uint8_t* end = data.data() + data.size();

    BlockUndo undo;
    undo.height = ReadUInt32(ptr);
    undo.block_hash = ReadUint256(ptr);
    uint32_t coin_count = ReadUInt32(ptr);

    undo.spent_coins.reserve(coin_count);
    for (uint32_t i = 0; i < coin_count; i++) {
        UndoEntry entry;
        entry.txid = ReadUint256(ptr);
        entry.vout = ReadUInt32(ptr);
        // Phase M.6.2: Wrap raw uint64_t in AmountUna
        entry.coin.value = AmountUna::Una(ReadUInt64(ptr));
        entry.coin.height = static_cast<int>(ReadUInt32(ptr));
        entry.coin.isCoinbase = (*ptr++ != 0);
        entry.coin.scriptPubKey = ReadBytes(ptr);

        // ═══════════════════════════════════════════════════════════════════════
        // Dragon #1 Fix: Deserialize confidential transaction data
        // Restores commitment so UTXOs remain confidential after reorg
        // ═══════════════════════════════════════════════════════════════════════
        entry.coin.is_confidential = (*ptr++ != 0);
        if (entry.coin.is_confidential) {
            entry.coin.commitment = ReadBytes(ptr);
        }

        undo.spent_coins.push_back(entry);
    }

    if (ptr < end) {
        const bool has_shielded_frontier = (*ptr++ != 0);
        if (has_shielded_frontier && ptr < end) {
            undo.pre_block_shielded_frontier = ReadBytes(ptr);
        }
    }

    if (ptr < end) {
        const bool has_reset_epoch = (*ptr++ != 0);
        if (has_reset_epoch && ptr < end) {
            shielded::ShieldedEpochSnapshot snap;
            snap.tree_frontier  = ReadBytes(ptr);
            snap.anchor_history = ReadBytes(ptr);
            snap.nullifiers     = ReadBytes(ptr);
            undo.pre_reset_shielded_epoch = std::move(snap);
        }
    }

    return undo;
}

} // namespace consensus
} // namespace dinero
