#include "consensus/coins_db.h"
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace dinero {
namespace consensus {

// ============================================================================
// CoinsDB Implementation
// ============================================================================

CoinsDB::CoinsDB() {
    // Configure RocksDB options for UTXO set
    options_.create_if_missing = true;
    options_.compression = rocksdb::kNoCompression;
    options_.write_buffer_size = 64 * 1024 * 1024;  // 64MB write buffer
    options_.max_open_files = 1000;

    // Basic configuration - bloom filters and table options can be added later if needed
}

CoinsDB::~CoinsDB() {
    close();
}

Status CoinsDB::open(const std::string& db_path) {
    rocksdb::DB* db_ptr = nullptr;
    rocksdb::Status status = rocksdb::DB::Open(options_, db_path, &db_ptr);

    if (!status.ok()) {
        return Status::Io;
    }

    db_.reset(db_ptr);
    return Status::Ok;
}

void CoinsDB::close() {
    db_.reset();
}

// ============================================================================
// Key Encoding (Binary Format)
// ============================================================================

std::string CoinsDB::encodeCoinKey(const OutPoint& outpoint) {
    // Format: 'C' + txid (32 bytes, little-endian) + vout (4 bytes, little-endian)
    std::string key;
    key.reserve(37);  // 1 + 32 + 4

    // Prefix
    key.push_back('C');

    // Phase M.4.3-B: Explicit DB boundary - unwrap TxId → uint256 for serialization
    const auto& txid_bytes = outpoint.txid.AsUint256();
    for (size_t i = 0; i < 32; i++) {
        key.push_back(static_cast<char>(txid_bytes.data[31 - i]));
    }

    // vout (4 bytes, little-endian)
    uint32_t vout = outpoint.vout;
    key.push_back(static_cast<char>(vout & 0xFF));
    key.push_back(static_cast<char>((vout >> 8) & 0xFF));
    key.push_back(static_cast<char>((vout >> 16) & 0xFF));
    key.push_back(static_cast<char>((vout >> 24) & 0xFF));

    return key;
}

std::string CoinsDB::encodeUndoKey(const std::string& block_hash) {
    // Format: 'U' + block_hash (32 bytes, little-endian)
    std::string key;
    key.reserve(33);

    key.push_back('U');

    // Convert hex block_hash to binary
    std::vector<uint8_t> hash_bytes(32);
    for (size_t i = 0; i < 32; i++) {
        std::string byte_str = block_hash.substr(i * 2, 2);
        hash_bytes[i] = static_cast<uint8_t>(std::strtol(byte_str.c_str(), nullptr, 16));
    }
    key.append(reinterpret_cast<const char*>(hash_bytes.data()), 32);

    return key;
}

OutPoint CoinsDB::decodeCoinKey(const std::string& key) {
    if (key.size() != 37 || key[0] != 'C') {
        return OutPoint();  // Invalid key
    }

    OutPoint op;

    // Phase M.4.3-B: Explicit DB boundary - deserialize to uint256, then wrap in TxId
    uint256 txid_raw;
    const uint8_t* txid_ptr = reinterpret_cast<const uint8_t*>(key.data() + 1);
    for (int i = 0; i < 32; i++) {
        txid_raw.data[i] = txid_ptr[31 - i];  // Reverse from little-endian
    }
    op.txid = TxId(txid_raw);

    // Extract vout (4 bytes, little-endian)
    const uint8_t* vout_ptr = reinterpret_cast<const uint8_t*>(key.data() + 33);
    op.vout = vout_ptr[0] | (vout_ptr[1] << 8) | (vout_ptr[2] << 16) | (vout_ptr[3] << 24);

    return op;
}

// ============================================================================
// UTXO Serialization (Compact Format)
// ============================================================================

std::string CoinsDB::serializeUTXOEntry(const UTXOEntry& coin) {
    std::string data;
    data.reserve(coin.serializedSize());

    // Phase M.6.2: Extract raw value for serialization
    // Value (8 bytes, little-endian)
    uint64_t value = coin.value.GetUna();
    for (int i = 0; i < 8; i++) {
        data.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
    }

    // Height (4 bytes, little-endian)
    uint32_t height = coin.height;
    for (int i = 0; i < 4; i++) {
        data.push_back(static_cast<char>((height >> (i * 8)) & 0xFF));
    }

    // Flags (1 byte: bit 0 = isCoinbase, bit 1 = has confidential commitment)
    uint8_t flags = coin.isCoinbase ? 0x01 : 0x00;
    if (coin.is_confidential && !coin.commitment.empty()) {
        flags |= 0x02;
    }
    data.push_back(static_cast<char>(flags));

    // ScriptPubKey length (CompactSize varint)
    uint64_t script_len = coin.scriptPubKey.size();
    if (script_len < 253) {
        data.push_back(static_cast<char>(script_len));
    } else if (script_len <= 0xFFFF) {
        data.push_back(static_cast<char>(253));
        data.push_back(static_cast<char>(script_len & 0xFF));
        data.push_back(static_cast<char>((script_len >> 8) & 0xFF));
    } else {
        data.push_back(static_cast<char>(254));
        for (int i = 0; i < 4; i++) {
            data.push_back(static_cast<char>((script_len >> (i * 8)) & 0xFF));
        }
    }

    // ScriptPubKey data
    data.append(reinterpret_cast<const char*>(coin.scriptPubKey.data()),
                coin.scriptPubKey.size());

    // Optional confidential commitment (present only when flag bit 1 is set)
    if (coin.is_confidential && !coin.commitment.empty()) {
        uint64_t commitment_len = coin.commitment.size();
        if (commitment_len < 253) {
            data.push_back(static_cast<char>(commitment_len));
        } else if (commitment_len <= 0xFFFF) {
            data.push_back(static_cast<char>(253));
            data.push_back(static_cast<char>(commitment_len & 0xFF));
            data.push_back(static_cast<char>((commitment_len >> 8) & 0xFF));
        } else {
            data.push_back(static_cast<char>(254));
            for (int i = 0; i < 4; i++) {
                data.push_back(static_cast<char>((commitment_len >> (i * 8)) & 0xFF));
            }
        }

        data.append(reinterpret_cast<const char*>(coin.commitment.data()),
                    coin.commitment.size());
    }

    return data;
}

StatusOr<UTXOEntry> CoinsDB::deserializeUTXOEntry(const std::string& data) {
    if (data.size() < 14) {  // Minimum: 8 + 4 + 1 + 1 (empty script)
        return Status::Serialization;
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
    size_t offset = 0;

    UTXOEntry coin;

    // Phase M.6.2: Deserialize raw value then wrap in AmountUna
    // Value (8 bytes)
    uint64_t raw_value = 0;
    for (int i = 0; i < 8; i++) {
        raw_value |= static_cast<uint64_t>(ptr[offset++]) << (i * 8);
    }
    coin.value = AmountUna::Una(raw_value);

    // Height (4 bytes)
    coin.height = 0;
    for (int i = 0; i < 4; i++) {
        coin.height |= static_cast<uint32_t>(ptr[offset++]) << (i * 8);
    }

    // Flags (1 byte)
    uint8_t flags = ptr[offset++];
    coin.isCoinbase = (flags & 0x01) != 0;
    coin.is_confidential = false;
    coin.commitment.clear();

    // ScriptPubKey length (CompactSize)
    if (offset >= data.size()) {
        return Status::Serialization;
    }

    uint64_t script_len = 0;
    uint8_t first_byte = ptr[offset++];

    if (first_byte < 253) {
        script_len = first_byte;
    } else if (first_byte == 253) {
        if (offset + 2 > data.size()) return Status::Serialization;
        script_len = ptr[offset] | (ptr[offset + 1] << 8);
        offset += 2;
    } else if (first_byte == 254) {
        if (offset + 4 > data.size()) return Status::Serialization;
        script_len = ptr[offset] | (ptr[offset + 1] << 8) |
                     (ptr[offset + 2] << 16) | (ptr[offset + 3] << 24);
        offset += 4;
    } else {
        return Status::Serialization;  // 0xFF not supported for script length
    }

    // ScriptPubKey data
    if (offset + script_len > data.size()) {
        return Status::Serialization;
    }

    coin.scriptPubKey.resize(script_len);
    std::memcpy(coin.scriptPubKey.data(), ptr + offset, script_len);
    offset += script_len;

    // Legacy entries stop after scriptPubKey.
    // New-format entries set bit 1 and append a CompactSize commitment payload.
    if ((flags & 0x02) != 0) {
        if (offset >= data.size()) {
            return Status::Serialization;
        }

        uint64_t commitment_len = 0;
        uint8_t commitment_prefix = ptr[offset++];

        if (commitment_prefix < 253) {
            commitment_len = commitment_prefix;
        } else if (commitment_prefix == 253) {
            if (offset + 2 > data.size()) return Status::Serialization;
            commitment_len = ptr[offset] | (ptr[offset + 1] << 8);
            offset += 2;
        } else if (commitment_prefix == 254) {
            if (offset + 4 > data.size()) return Status::Serialization;
            commitment_len = ptr[offset] | (ptr[offset + 1] << 8) |
                             (ptr[offset + 2] << 16) | (ptr[offset + 3] << 24);
            offset += 4;
        } else {
            return Status::Serialization;
        }

        if (offset + commitment_len > data.size()) {
            return Status::Serialization;
        }

        coin.is_confidential = true;
        coin.commitment.resize(commitment_len);
        std::memcpy(coin.commitment.data(), ptr + offset, commitment_len);
        offset += commitment_len;
    }

    return coin;
}

// ============================================================================
// UTXO Operations
// ============================================================================

StatusOr<UTXOEntry> CoinsDB::getCoin(const OutPoint& outpoint) const {
    if (!db_) {
        return Status::Internal;
    }

    std::string key = encodeCoinKey(outpoint);
    std::string value;

    Status status = getImpl(key, value);
    if (status != Status::Ok) {
        return status;
    }

    return deserializeUTXOEntry(value);
}

bool CoinsDB::hasCoin(const OutPoint& outpoint) const {
    if (!db_) {
        return false;
    }

    std::string key = encodeCoinKey(outpoint);
    std::string value;

    return getImpl(key, value) == Status::Ok;
}

Status CoinsDB::addCoin(const OutPoint& outpoint, const UTXOEntry& coin) {
    if (!db_) {
        return Status::Internal;
    }

    std::string key = encodeCoinKey(outpoint);
    std::string value = serializeUTXOEntry(coin);

    return putImpl(key, value);
}

StatusOr<UTXOEntry> CoinsDB::spendCoin(const OutPoint& outpoint) {
    if (!db_) {
        return Status::Internal;
    }

    // Get the coin first (for undo data)
    auto coin_result = getCoin(outpoint);
    if (!coin_result.ok()) {
        return coin_result.status();
    }

    // Delete from database
    std::string key = encodeCoinKey(outpoint);
    Status delete_status = deleteImpl(key);
    if (delete_status != Status::Ok) {
        return delete_status;
    }

    return coin_result.value();
}

Status CoinsDB::writeBatch(
    const std::vector<std::pair<OutPoint, UTXOEntry>>& coins_to_add,
    const std::vector<OutPoint>& coins_to_spend)
{
    if (!db_) {
        return Status::Internal;
    }

    rocksdb::WriteBatch batch;

    // Add new coins
    for (const auto& [outpoint, coin] : coins_to_add) {
        std::string key = encodeCoinKey(outpoint);
        std::string value = serializeUTXOEntry(coin);
        batch.Put(key, value);
    }

    // Spend (delete) coins
    for (const auto& outpoint : coins_to_spend) {
        std::string key = encodeCoinKey(outpoint);
        batch.Delete(key);
    }

    // Write atomically
    rocksdb::WriteOptions write_opts;
    write_opts.sync = true;  // Ensure durability

    rocksdb::Status status = db_->Write(write_opts, &batch);
    return status.ok() ? Status::Ok : Status::Io;
}

// ============================================================================
// Undo Data Operations
// ============================================================================

std::string CoinsDB::serializeUndoCoins(const UndoCoins& undo) {
    std::string data;

    // Number of coins (CompactSize)
    uint64_t count = undo.spent_coins.size();
    if (count < 253) {
        data.push_back(static_cast<char>(count));
    } else if (count <= 0xFFFF) {
        data.push_back(static_cast<char>(253));
        data.push_back(static_cast<char>(count & 0xFF));
        data.push_back(static_cast<char>((count >> 8) & 0xFF));
    } else {
        data.push_back(static_cast<char>(254));
        for (int i = 0; i < 4; i++) {
            data.push_back(static_cast<char>((count >> (i * 8)) & 0xFF));
        }
    }

    // Serialize each spent coin
    for (const auto& [outpoint, coin] : undo.spent_coins) {
        // Phase M.4.3-B: Explicit DB boundary - unwrap TxId → uint256 for serialization
        const auto& txid_bytes = outpoint.txid.AsUint256();
        for (size_t i = 0; i < 32; i++) {
            data.push_back(static_cast<char>(txid_bytes.data[31 - i]));
        }

        uint32_t vout = outpoint.vout;
        for (int i = 0; i < 4; i++) {
            data.push_back(static_cast<char>((vout >> (i * 8)) & 0xFF));
        }

        // Serialize UTXO entry
        data.append(serializeUTXOEntry(coin));
    }

    return data;
}

StatusOr<UndoCoins> CoinsDB::deserializeUndoCoins(const std::string& data) {
    if (data.empty()) {
        return Status::Serialization;
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
    size_t offset = 0;

    // Read count (CompactSize)
    uint64_t count = 0;
    uint8_t first_byte = ptr[offset++];

    if (first_byte < 253) {
        count = first_byte;
    } else if (first_byte == 253) {
        if (offset + 2 > data.size()) return Status::Serialization;
        count = ptr[offset] | (ptr[offset + 1] << 8);
        offset += 2;
    } else if (first_byte == 254) {
        if (offset + 4 > data.size()) return Status::Serialization;
        count = ptr[offset] | (ptr[offset + 1] << 8) |
                (ptr[offset + 2] << 16) | (ptr[offset + 3] << 24);
        offset += 4;
    }

    UndoCoins undo;

    // Read each spent coin
    for (uint64_t i = 0; i < count; i++) {
        if (offset + 36 > data.size()) return Status::Serialization;

        // Read outpoint
        OutPoint outpoint;

        // txid (32 bytes)
        // Phase M.4.3-B: Explicit DB boundary - deserialize to uint256, then wrap in TxId
        uint256 txid_raw;
        for (int j = 0; j < 32; j++) {
            txid_raw.data[j] = ptr[offset + 31 - j];  // Reverse from little-endian
        }
        outpoint.txid = TxId(txid_raw);
        offset += 32;

        // vout (4 bytes)
        outpoint.vout = ptr[offset] | (ptr[offset + 1] << 8) |
                        (ptr[offset + 2] << 16) | (ptr[offset + 3] << 24);
        offset += 4;

        // Read UTXO entry
        std::string coin_data = data.substr(offset);
        auto coin_result = deserializeUTXOEntry(coin_data);
        if (!coin_result.ok()) {
            return coin_result.status();
        }

        offset += coin_result.value().serializedSize();
        undo.addSpentCoin(outpoint, coin_result.value());
    }

    return undo;
}

Status CoinsDB::writeUndoCoins(const std::string& block_hash, const UndoCoins& undo) {
    if (!db_) {
        return Status::Internal;
    }

    std::string key = encodeUndoKey(block_hash);
    std::string value = serializeUndoCoins(undo);

    return putImpl(key, value);
}

StatusOr<UndoCoins> CoinsDB::getUndoCoins(const std::string& block_hash) const {
    if (!db_) {
        return Status::Internal;
    }

    std::string key = encodeUndoKey(block_hash);
    std::string value;

    Status status = getImpl(key, value);
    if (status != Status::Ok) {
        return status;
    }

    return deserializeUndoCoins(value);
}

Status CoinsDB::deleteUndoCoins(const std::string& block_hash) {
    if (!db_) {
        return Status::Internal;
    }

    std::string key = encodeUndoKey(block_hash);
    return deleteImpl(key);
}

// ============================================================================
// Best Block Operations
// ============================================================================

Status CoinsDB::writeBestBlock(const std::string& block_hash) {
    if (!db_) {
        return Status::Internal;
    }

    // Convert hex to binary
    std::vector<uint8_t> hash_bytes(32);
    for (size_t i = 0; i < 32; i++) {
        std::string byte_str = block_hash.substr(i * 2, 2);
        hash_bytes[i] = static_cast<uint8_t>(std::strtol(byte_str.c_str(), nullptr, 16));
    }

    std::string value(reinterpret_cast<const char*>(hash_bytes.data()), 32);
    return putImpl("B", value);
}

StatusOr<std::string> CoinsDB::getBestBlock() const {
    if (!db_) {
        return Status::Internal;
    }

    std::string value;
    Status status = getImpl("B", value);
    if (status != Status::Ok) {
        return status;
    }

    if (value.size() != 32) {
        return Status::Serialization;
    }

    // Convert binary to hex
    std::ostringstream hex;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(value.data());
    for (size_t i = 0; i < 32; i++) {
        hex << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(ptr[i]);
    }

    return hex.str();
}

// ============================================================================
// Statistics
// ============================================================================

uint64_t CoinsDB::getUtxoCount() const {
    if (!db_) {
        return 0;
    }

    uint64_t count = 0;
    rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());

    for (it->Seek("C"); it->Valid() && it->key().starts_with("C"); it->Next()) {
        count++;
    }

    delete it;
    return count;
}

uint64_t CoinsDB::getDatabaseSize() const {
    if (!db_) {
        return 0;
    }

    uint64_t size = 0;
    db_->GetIntProperty("rocksdb.total-sst-files-size", &size);
    return size;
}

Status CoinsDB::flush() {
    if (!db_) {
        return Status::Internal;
    }

    rocksdb::FlushOptions flush_opts;
    flush_opts.wait = true;

    rocksdb::Status status = db_->Flush(flush_opts);
    return status.ok() ? Status::Ok : Status::Io;
}

// ============================================================================
// Internal Helpers
// ============================================================================

Status CoinsDB::getImpl(const std::string& key, std::string& value) const {
    if (!db_) {
        return Status::Internal;
    }

    rocksdb::ReadOptions read_opts;
    rocksdb::Status status = db_->Get(read_opts, key, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }

    return status.ok() ? Status::Ok : Status::Io;
}

Status CoinsDB::putImpl(const std::string& key, const std::string& value) {
    if (!db_) {
        return Status::Internal;
    }

    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Put(write_opts, key, value);

    return status.ok() ? Status::Ok : Status::Io;
}

Status CoinsDB::deleteImpl(const std::string& key) {
    if (!db_) {
        return Status::Internal;
    }

    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Delete(write_opts, key);

    return status.ok() ? Status::Ok : Status::Io;
}

} // namespace consensus
} // namespace dinero
