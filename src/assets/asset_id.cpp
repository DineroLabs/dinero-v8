/**
 * Phase 30: Taproot Asset Layer - Asset ID Implementation
 */

#include "assets/asset_id.h"
#include "crypto/sha256.h"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {
namespace assets {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

/**
 * @brief Double SHA256 hash
 */
std::array<uint8_t, 32> sha256d(const std::vector<uint8_t>& data) {
    std::array<uint8_t, 32> hash1, hash2;
    crypto::CSHA256 ctx;
    ctx.Write(data.data(), data.size());
    ctx.Finalize(hash1.data());

    ctx.reset();
    ctx.Write(hash1.data(), 32);
    ctx.Finalize(hash2.data());

    return hash2;
}

/**
 * @brief Single SHA256 hash
 */
std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data) {
    std::array<uint8_t, 32> hash;
    crypto::CSHA256 ctx;
    ctx.Write(data.data(), data.size());
    ctx.Finalize(hash.data());
    return hash;
}

/**
 * @brief Convert bytes to hex string
 */
std::string bytesToHex(const uint8_t* data, size_t len) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        ss << std::setw(2) << static_cast<int>(data[i]);
    }
    return ss.str();
}

/**
 * @brief Convert hex string to bytes
 */
std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoi(byteString, nullptr, 16)));
    }
    return bytes;
}

/**
 * @brief Write uint64_t in little-endian
 */
void writeLE64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        value >>= 8;
    }
}

/**
 * @brief Write uint32_t in little-endian
 */
void writeLE32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        value >>= 8;
    }
}

/**
 * @brief Read uint64_t from little-endian bytes
 */
uint64_t readLE64(const uint8_t* data) {
    uint64_t result = 0;
    for (int i = 7; i >= 0; i--) {
        result = (result << 8) | data[i];
    }
    return result;
}

/**
 * @brief Read uint32_t from little-endian bytes
 */
uint32_t readLE32(const uint8_t* data) {
    uint32_t result = 0;
    for (int i = 3; i >= 0; i--) {
        result = (result << 8) | data[i];
    }
    return result;
}

/**
 * @brief Write variable-length string
 */
void writeVarString(std::vector<uint8_t>& out, const std::string& str) {
    // Write length as varint
    size_t len = str.size();
    if (len < 0xFD) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    } else {
        out.push_back(0xFE);
        writeLE32(out, static_cast<uint32_t>(len));
    }
    // Write string data
    out.insert(out.end(), str.begin(), str.end());
}

/**
 * @brief Read variable-length string
 */
std::string readVarString(const uint8_t*& data, size_t& remaining) {
    if (remaining < 1) return "";

    size_t len = data[0];
    data++; remaining--;

    if (len == 0xFD) {
        if (remaining < 2) return "";
        len = data[0] | (static_cast<size_t>(data[1]) << 8);
        data += 2; remaining -= 2;
    } else if (len == 0xFE) {
        if (remaining < 4) return "";
        len = readLE32(data);
        data += 4; remaining -= 4;
    }

    if (remaining < len) return "";
    std::string result(reinterpret_cast<const char*>(data), len);
    data += len; remaining -= len;
    return result;
}

} // anonymous namespace

// ============================================================================
// AssetID Functions
// ============================================================================

AssetID ComputeAssetID(
    const std::array<uint8_t, 32>& issuer_pubkey,
    const std::array<uint8_t, 32>& creation_txid,
    const std::array<uint8_t, 32>& metadata_hash) {

    // Concatenate: issuer_pubkey || creation_txid || metadata_hash
    std::vector<uint8_t> preimage;
    preimage.reserve(96);
    preimage.insert(preimage.end(), issuer_pubkey.begin(), issuer_pubkey.end());
    preimage.insert(preimage.end(), creation_txid.begin(), creation_txid.end());
    preimage.insert(preimage.end(), metadata_hash.begin(), metadata_hash.end());

    // Double SHA256
    return sha256d(preimage);
}

bool IsNativeAsset(const AssetID& id) {
    static const AssetID zero = {};
    return id == zero;
}

AssetID NullAssetID() {
    return AssetID{};
}

// ============================================================================
// AssetMetadata Implementation
// ============================================================================

std::array<uint8_t, 32> AssetMetadata::hash() const {
    std::vector<uint8_t> data;

    // Serialize all fields
    writeVarString(data, name);
    writeVarString(data, ticker);
    data.push_back(decimals);
    writeVarString(data, description);
    writeVarString(data, icon_url);

    // Extended data
    if (extended_data.size() < 0xFD) {
        data.push_back(static_cast<uint8_t>(extended_data.size()));
    } else {
        data.push_back(0xFD);
        data.push_back(static_cast<uint8_t>(extended_data.size() & 0xFF));
        data.push_back(static_cast<uint8_t>((extended_data.size() >> 8) & 0xFF));
    }
    data.insert(data.end(), extended_data.begin(), extended_data.end());

    return sha256(data);
}

std::vector<uint8_t> AssetMetadata::serialize() const {
    std::vector<uint8_t> data;

    writeVarString(data, name);
    writeVarString(data, ticker);
    data.push_back(decimals);
    writeVarString(data, description);
    writeVarString(data, icon_url);

    // Extended data with length
    if (extended_data.size() < 0xFD) {
        data.push_back(static_cast<uint8_t>(extended_data.size()));
    } else {
        data.push_back(0xFD);
        data.push_back(static_cast<uint8_t>(extended_data.size() & 0xFF));
        data.push_back(static_cast<uint8_t>((extended_data.size() >> 8) & 0xFF));
    }
    data.insert(data.end(), extended_data.begin(), extended_data.end());

    return data;
}

std::optional<AssetMetadata> AssetMetadata::deserialize(const std::vector<uint8_t>& data) {
    if (data.empty()) return std::nullopt;

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    AssetMetadata meta;
    meta.name = readVarString(ptr, remaining);
    meta.ticker = readVarString(ptr, remaining);

    if (remaining < 1) return std::nullopt;
    meta.decimals = *ptr++;
    remaining--;

    meta.description = readVarString(ptr, remaining);
    meta.icon_url = readVarString(ptr, remaining);

    // Extended data
    if (remaining < 1) return std::nullopt;
    size_t ext_len = *ptr++;
    remaining--;

    if (ext_len == 0xFD) {
        if (remaining < 2) return std::nullopt;
        ext_len = ptr[0] | (static_cast<size_t>(ptr[1]) << 8);
        ptr += 2; remaining -= 2;
    }

    if (remaining < ext_len) return std::nullopt;
    meta.extended_data.assign(ptr, ptr + ext_len);

    return meta;
}

std::string AssetMetadata::toJSON() const {
    std::stringstream ss;
    ss << "{";
    ss << "\"name\":\"" << name << "\",";
    ss << "\"ticker\":\"" << ticker << "\",";
    ss << "\"decimals\":" << static_cast<int>(decimals) << ",";
    ss << "\"description\":\"" << description << "\",";
    ss << "\"icon_url\":\"" << icon_url << "\"";
    if (!extended_data.empty()) {
        ss << ",\"extended_data\":\"" << bytesToHex(extended_data.data(), extended_data.size()) << "\"";
    }
    ss << "}";
    return ss.str();
}

std::optional<AssetMetadata> AssetMetadata::fromJSON(const std::string& json) {
    // Simple JSON parsing - find key-value pairs
    AssetMetadata meta;

    auto findValue = [&json](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";

        pos += search.length();
        while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

        if (json[pos] == '"') {
            // String value
            pos++;
            size_t end = json.find('"', pos);
            if (end == std::string::npos) return "";
            return json.substr(pos, end - pos);
        } else {
            // Number value
            size_t end = json.find_first_of(",}", pos);
            if (end == std::string::npos) end = json.length();
            return json.substr(pos, end - pos);
        }
    };

    meta.name = findValue("name");
    meta.ticker = findValue("ticker");

    std::string dec_str = findValue("decimals");
    if (!dec_str.empty()) {
        meta.decimals = static_cast<uint8_t>(std::stoi(dec_str));
    }

    meta.description = findValue("description");
    meta.icon_url = findValue("icon_url");

    std::string ext_hex = findValue("extended_data");
    if (!ext_hex.empty()) {
        meta.extended_data = hexToBytes(ext_hex);
    }

    return meta;
}

// ============================================================================
// AssetSupplyConfig Implementation
// ============================================================================

std::vector<uint8_t> AssetSupplyConfig::serialize() const {
    std::vector<uint8_t> data;

    data.push_back(static_cast<uint8_t>(model));
    writeLE64(data, initial_supply);
    writeLE64(data, max_supply);
    data.push_back(burn_enabled ? 1 : 0);

    // Mint authority
    data.push_back(static_cast<uint8_t>(mint_authority.size()));
    data.insert(data.end(), mint_authority.begin(), mint_authority.end());

    // Burn authority
    data.push_back(static_cast<uint8_t>(burn_authority.size()));
    data.insert(data.end(), burn_authority.begin(), burn_authority.end());

    return data;
}

std::optional<AssetSupplyConfig> AssetSupplyConfig::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 18) return std::nullopt;

    const uint8_t* ptr = data.data();

    AssetSupplyConfig config;
    config.model = static_cast<SupplyModel>(*ptr++);
    config.initial_supply = readLE64(ptr); ptr += 8;
    config.max_supply = readLE64(ptr); ptr += 8;
    config.burn_enabled = (*ptr++ != 0);

    size_t remaining = data.size() - (ptr - data.data());

    // Mint authority
    if (remaining < 1) return std::nullopt;
    size_t mint_len = *ptr++;
    remaining--;

    if (remaining < mint_len) return std::nullopt;
    config.mint_authority.assign(ptr, ptr + mint_len);
    ptr += mint_len; remaining -= mint_len;

    // Burn authority
    if (remaining < 1) return std::nullopt;
    size_t burn_len = *ptr++;
    remaining--;

    if (remaining < burn_len) return std::nullopt;
    config.burn_authority.assign(ptr, ptr + burn_len);

    return config;
}

// ============================================================================
// AssetGenesis Implementation
// ============================================================================

AssetID AssetGenesis::computeID() const {
    return ComputeAssetID(issuer_pubkey,
                          *reinterpret_cast<const std::array<uint8_t, 32>*>(
                              hexToBytes(creation_txid).data()),
                          metadata.hash());
}

std::vector<uint8_t> AssetGenesis::generateGenesisScript() const {
    // Generate CTV-style commitment script
    // OP_PUSH(asset_id) OP_DROP OP_PUSH(supply_hash) OP_CHECKTEMPLATEVERIFY
    std::vector<uint8_t> script;

    // Push asset_id (32 bytes)
    script.push_back(0x20); // OP_PUSH32
    script.insert(script.end(), asset_id.begin(), asset_id.end());

    // OP_DROP
    script.push_back(0x75);

    // Push initial supply configuration hash
    auto supply_data = supply.serialize();
    auto supply_hash = sha256(supply_data);
    script.push_back(0x20); // OP_PUSH32
    script.insert(script.end(), supply_hash.begin(), supply_hash.end());

    // OP_CHECKTEMPLATEVERIFY
    script.push_back(0xB3);

    return script;
}

std::vector<uint8_t> AssetGenesis::serialize() const {
    std::vector<uint8_t> data;

    // Asset ID
    data.insert(data.end(), asset_id.begin(), asset_id.end());

    // Issuer pubkey
    data.insert(data.end(), issuer_pubkey.begin(), issuer_pubkey.end());

    // Creation info
    auto txid_bytes = hexToBytes(creation_txid);
    data.insert(data.end(), txid_bytes.begin(), txid_bytes.end());
    writeLE32(data, creation_output_index);
    writeLE32(data, creation_height);

    // Metadata
    auto meta_data = metadata.serialize();
    writeLE32(data, static_cast<uint32_t>(meta_data.size()));
    data.insert(data.end(), meta_data.begin(), meta_data.end());

    // Supply config
    auto supply_data = supply.serialize();
    writeLE32(data, static_cast<uint32_t>(supply_data.size()));
    data.insert(data.end(), supply_data.begin(), supply_data.end());

    // Timestamp
    writeLE64(data, created_at);

    return data;
}

std::optional<AssetGenesis> AssetGenesis::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 32 + 32 + 32 + 4 + 4 + 4 + 4 + 8) return std::nullopt;

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    AssetGenesis genesis;

    // Asset ID
    std::copy(ptr, ptr + 32, genesis.asset_id.begin());
    ptr += 32; remaining -= 32;

    // Issuer pubkey
    std::copy(ptr, ptr + 32, genesis.issuer_pubkey.begin());
    ptr += 32; remaining -= 32;

    // Creation txid
    genesis.creation_txid = bytesToHex(ptr, 32);
    ptr += 32; remaining -= 32;

    // Creation info
    genesis.creation_output_index = readLE32(ptr); ptr += 4; remaining -= 4;
    genesis.creation_height = readLE32(ptr); ptr += 4; remaining -= 4;

    // Metadata
    if (remaining < 4) return std::nullopt;
    uint32_t meta_len = readLE32(ptr); ptr += 4; remaining -= 4;

    if (remaining < meta_len) return std::nullopt;
    std::vector<uint8_t> meta_data(ptr, ptr + meta_len);
    ptr += meta_len; remaining -= meta_len;

    auto meta = AssetMetadata::deserialize(meta_data);
    if (!meta) return std::nullopt;
    genesis.metadata = *meta;

    // Supply config
    if (remaining < 4) return std::nullopt;
    uint32_t supply_len = readLE32(ptr); ptr += 4; remaining -= 4;

    if (remaining < supply_len) return std::nullopt;
    std::vector<uint8_t> supply_data(ptr, ptr + supply_len);
    ptr += supply_len; remaining -= supply_len;

    auto sup = AssetSupplyConfig::deserialize(supply_data);
    if (!sup) return std::nullopt;
    genesis.supply = *sup;

    // Timestamp
    if (remaining < 8) return std::nullopt;
    genesis.created_at = readLE64(ptr);

    return genesis;
}

// ============================================================================
// AssetCommitment Implementation
// ============================================================================

std::vector<uint8_t> AssetCommitment::toScript() const {
    std::vector<uint8_t> script;

    // OP_PUSH(asset_id) - 32 bytes
    script.push_back(0x20);
    script.insert(script.end(), asset_id.begin(), asset_id.end());

    // OP_PUSH(amount) - 8 bytes
    script.push_back(0x08);
    std::vector<uint8_t> amt;
    writeLE64(amt, amount);
    script.insert(script.end(), amt.begin(), amt.end());

    // OP_PUSH(state_hash) - 32 bytes
    script.push_back(0x20);
    script.insert(script.end(), state_hash.begin(), state_hash.end());

    // OP_CHECKTEMPLATEVERIFY
    script.push_back(0xB3);

    return script;
}

std::optional<AssetCommitment> AssetCommitment::fromScript(const std::vector<uint8_t>& script) {
    // Expected format:
    // 0x20 [32 bytes asset_id] 0x08 [8 bytes amount] 0x20 [32 bytes state_hash] 0xB3

    if (script.size() < 76) return std::nullopt;

    size_t pos = 0;

    // Check asset_id push
    if (script[pos++] != 0x20) return std::nullopt;

    AssetCommitment commit;
    std::copy(script.begin() + pos, script.begin() + pos + 32, commit.asset_id.begin());
    pos += 32;

    // Check amount push
    if (script[pos++] != 0x08) return std::nullopt;

    commit.amount = readLE64(script.data() + pos);
    pos += 8;

    // Check state_hash push
    if (script[pos++] != 0x20) return std::nullopt;

    std::copy(script.begin() + pos, script.begin() + pos + 32, commit.state_hash.begin());
    pos += 32;

    // Check OP_CHECKTEMPLATEVERIFY
    if (script[pos] != 0xB3) return std::nullopt;

    return commit;
}

std::array<uint8_t, 32> AssetCommitment::hash() const {
    std::vector<uint8_t> data;
    data.insert(data.end(), asset_id.begin(), asset_id.end());

    std::vector<uint8_t> amt;
    writeLE64(amt, amount);
    data.insert(data.end(), amt.begin(), amt.end());

    data.insert(data.end(), state_hash.begin(), state_hash.end());

    return sha256(data);
}

// ============================================================================
// Utility Functions
// ============================================================================

std::string AssetIDToHex(const AssetID& id) {
    return bytesToHex(id.data(), id.size());
}

std::optional<AssetID> AssetIDFromHex(const std::string& hex) {
    if (hex.length() != 64) return std::nullopt;

    try {
        auto bytes = hexToBytes(hex);
        if (bytes.size() != 32) return std::nullopt;

        AssetID id;
        std::copy(bytes.begin(), bytes.end(), id.begin());
        return id;
    } catch (...) {
        return std::nullopt;
    }
}

bool AssetIDEqual(const AssetID& a, const AssetID& b) {
    return a == b;
}

std::string AssetIDShort(const AssetID& id) {
    return AssetIDToHex(id).substr(0, 8);
}

} // namespace assets
} // namespace dinero
