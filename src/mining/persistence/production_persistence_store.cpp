#include "production_persistence_store.h"

// Ring 4 — Phase 4h.3: RocksDB Integration (Production Persistence)
//
// STATUS: 🚧 IMPLEMENTING persist()
//
// Phase 4h.3 adds:
// ✅ RocksDB backend
// ✅ Deterministic serialization
// ✅ Atomic WriteBatch
// ✅ SHA256 checksums
// ✅ Conservative failure handling
//
// Rules enforced:
// ❌ No API changes (header frozen)
// ❌ No entropy sources
// ❌ No partial state exposure
// ✅ MR1-MR5 must pass unchanged

#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace mining {

// ═══════════════════════════════════════════════════════════════
// Canonical Keys (Deterministic, Immutable)
// ═══════════════════════════════════════════════════════════════

static constexpr const char* KEY_SNAPSHOT = "snapshot";
static constexpr const char* KEY_CHECKSUM = "checksum";
static constexpr const char* KEY_VERSION  = "version";

// ═══════════════════════════════════════════════════════════════
// Deterministic Serialization (Binary, Little-Endian)
// ═══════════════════════════════════════════════════════════════

namespace {

// Write uint32_t in deterministic format (little-endian)
void write_uint32(std::string& buf, uint32_t value) {
    buf.push_back(static_cast<char>(value & 0xFF));
    buf.push_back(static_cast<char>((value >> 8) & 0xFF));
    buf.push_back(static_cast<char>((value >> 16) & 0xFF));
    buf.push_back(static_cast<char>((value >> 24) & 0xFF));
}

// Write uint64_t in deterministic format (little-endian)
void write_uint64(std::string& buf, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
    }
}

// Write bool (deterministic 0/1)
void write_bool(std::string& buf, bool value) {
    buf.push_back(value ? '\x01' : '\x00');
}

// Write optional<uint32_t> (deterministic)
void write_optional_uint32(std::string& buf, const std::optional<uint32_t>& opt) {
    if (opt.has_value()) {
        write_bool(buf, true);
        write_uint32(buf, *opt);
    } else {
        write_bool(buf, false);
    }
}

// Write optional<uint64_t> (deterministic)
void write_optional_uint64(std::string& buf, const std::optional<uint64_t>& opt) {
    if (opt.has_value()) {
        write_bool(buf, true);
        write_uint64(buf, *opt);
    } else {
        write_bool(buf, false);
    }
}

// Write MiningPhase enum (deterministic)
void write_mining_phase(std::string& buf, mining_test::MiningPhase phase) {
    write_uint32(buf, static_cast<uint32_t>(phase));
}

// Serialize MiningState deterministically
std::string serializeMiningState(const mining_test::MiningState& state) {
    std::string buf;
    buf.reserve(256);  // Pre-allocate reasonable size

    // Serialize all fields in fixed order (deterministic)
    write_mining_phase(buf, state.phase);
    write_uint64(buf, state.timestamp);
    write_uint64(buf, state.current_tip);
    write_uint32(buf, state.current_height);
    write_uint32(buf, state.mempool_size);
    write_uint64(buf, state.mempool_total_fees);
    write_optional_uint64(buf, state.template_prev_hash);
    write_optional_uint32(buf, state.template_height);
    write_optional_uint64(buf, state.template_subsidy);
    write_optional_uint32(buf, state.template_tx_count);
    write_uint64(buf, state.hashes_computed);
    write_uint64(buf, state.blocks_found);
    write_uint64(buf, state.templates_created);
    write_bool(buf, state.has_crashed);
    write_uint32(buf, state.restart_count);

    return buf;
}

// Compute SHA256 checksum (deterministic)
std::string computeChecksum(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

    return std::string(reinterpret_cast<const char*>(hash), SHA256_DIGEST_LENGTH);
}

// Read uint32_t in deterministic format (little-endian)
uint32_t read_uint32(const std::string& buf, size_t& offset) {
    if (offset + 4 > buf.size()) {
        throw std::runtime_error("Buffer underflow reading uint32");
    }
    uint32_t value = 0;
    value |= static_cast<uint8_t>(buf[offset++]);
    value |= static_cast<uint8_t>(buf[offset++]) << 8;
    value |= static_cast<uint8_t>(buf[offset++]) << 16;
    value |= static_cast<uint8_t>(buf[offset++]) << 24;
    return value;
}

// Read uint64_t in deterministic format (little-endian)
uint64_t read_uint64(const std::string& buf, size_t& offset) {
    if (offset + 8 > buf.size()) {
        throw std::runtime_error("Buffer underflow reading uint64");
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(buf[offset++])) << (i * 8);
    }
    return value;
}

// Read bool (deterministic 0/1)
bool read_bool(const std::string& buf, size_t& offset) {
    if (offset + 1 > buf.size()) {
        throw std::runtime_error("Buffer underflow reading bool");
    }
    return buf[offset++] != '\x00';
}

// Read optional<uint32_t> (deterministic)
std::optional<uint32_t> read_optional_uint32(const std::string& buf, size_t& offset) {
    bool has_value = read_bool(buf, offset);
    if (has_value) {
        return read_uint32(buf, offset);
    }
    return std::nullopt;
}

// Read optional<uint64_t> (deterministic)
std::optional<uint64_t> read_optional_uint64(const std::string& buf, size_t& offset) {
    bool has_value = read_bool(buf, offset);
    if (has_value) {
        return read_uint64(buf, offset);
    }
    return std::nullopt;
}

// Read MiningPhase enum (deterministic)
mining_test::MiningPhase read_mining_phase(const std::string& buf, size_t& offset) {
    uint32_t phase_val = read_uint32(buf, offset);
    return static_cast<mining_test::MiningPhase>(phase_val);
}

// Deserialize MiningState deterministically (strict)
mining_test::MiningState deserializeMiningState(const std::string& buf) {
    size_t offset = 0;
    mining_test::MiningState state;

    // Deserialize all fields in exact same order as serialize
    state.phase = read_mining_phase(buf, offset);
    state.timestamp = read_uint64(buf, offset);
    state.current_tip = read_uint64(buf, offset);
    state.current_height = read_uint32(buf, offset);
    state.mempool_size = read_uint32(buf, offset);
    state.mempool_total_fees = read_uint64(buf, offset);
    state.template_prev_hash = read_optional_uint64(buf, offset);
    state.template_height = read_optional_uint32(buf, offset);
    state.template_subsidy = read_optional_uint64(buf, offset);
    state.template_tx_count = read_optional_uint32(buf, offset);
    state.hashes_computed = read_uint64(buf, offset);
    state.blocks_found = read_uint64(buf, offset);
    state.templates_created = read_uint64(buf, offset);
    state.has_crashed = read_bool(buf, offset);
    state.restart_count = read_uint32(buf, offset);

    // Validate all bytes consumed (no trailing data)
    if (offset != buf.size()) {
        throw std::runtime_error("Deserialization did not consume entire buffer");
    }

    return state;
}

// Validate state invariants (conservative)
bool isStateValid(const mining_test::MiningState& state) {
    // Invariant 1: blocks_found <= current_height + 1
    // (Can't mine more blocks than height allows)
    if (state.blocks_found > static_cast<uint64_t>(state.current_height) + 1) {
        return false;
    }

    // Invariant 2: If template exists, template_height <= current_height + 1
    // (Template can't be ahead of chain by more than 1)
    if (state.template_height.has_value()) {
        if (*state.template_height > state.current_height + 1) {
            return false;
        }
    }

    // Invariant 3: Template consistency (all or nothing)
    // If any template field exists, key fields must exist
    bool has_any_template = state.template_prev_hash.has_value() ||
                           state.template_height.has_value() ||
                           state.template_subsidy.has_value() ||
                           state.template_tx_count.has_value();

    bool has_key_template = state.template_height.has_value();

    // If we have any template data, we must have height
    if (has_any_template && !has_key_template) {
        return false;  // Partial template (invalid)
    }

    // Invariant 4: Restart count consistency
    // If has_crashed is true, restart_count should be > 0
    // (This is a weak check, but prevents impossible states)
    if (state.has_crashed && state.restart_count == 0) {
        return false;
    }

    // Invariant 5: Phase consistency
    // STOPPED phase should have no template
    if (state.phase == mining_test::MiningPhase::STOPPED) {
        if (has_any_template) {
            return false;  // Stopped but has template (inconsistent)
        }
    }

    // All invariants passed
    return true;
}

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// Private Implementation (Phase 4h.3: RocksDB Integration)
// ═══════════════════════════════════════════════════════════════

struct ProductionPersistenceStore::Impl {
    std::unique_ptr<rocksdb::DB> db;
    uint64_t snapshot_version = 0;

    // Fault injection flags (Phase 4g compatibility)
    bool simulate_partial_write = false;
    bool simulate_corruption = false;
};

// ═══════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════

ProductionPersistenceStore::ProductionPersistenceStore(
    const PersistenceConfig& config
)
    : impl_(std::make_unique<Impl>())
{
    // Phase 4h.3: Open RocksDB with deterministic, crash-safe options
    rocksdb::Options opts;
    opts.create_if_missing = true;
    opts.error_if_exists   = false;
    opts.paranoid_checks   = true;
    opts.compression       = rocksdb::kNoCompression;  // Determinism
    opts.use_fsync         = true;                     // Crash safety

    rocksdb::DB* raw_db = nullptr;
    auto status = rocksdb::DB::Open(opts, config.data_directory, &raw_db);

    if (!status.ok()) {
        // Conservative failure: throw on initialization error
        throw std::runtime_error("Failed to open RocksDB: " + status.ToString());
    }

    impl_->db.reset(raw_db);

    // Initialize version from persisted state (if exists)
    std::string version_str;
    status = impl_->db->Get(rocksdb::ReadOptions(), KEY_VERSION, &version_str);
    if (status.ok()) {
        impl_->snapshot_version = std::stoull(version_str);
    }
    // If not found, snapshot_version stays 0 (initial state)
}

ProductionPersistenceStore::~ProductionPersistenceStore() {
    // Phase 4h.3: RocksDB close is automatic via unique_ptr
    // DB destructor flushes pending writes and closes cleanly
}

// ═══════════════════════════════════════════════════════════════
// Core Persistence API
// ═══════════════════════════════════════════════════════════════

void ProductionPersistenceStore::persist(
    const mining_test::MiningState& state
) {
    // Phase 4h.3: Implement atomic persist with RocksDB WriteBatch

    if (!impl_ || !impl_->db) {
        return;  // Conservative: do nothing if DB not initialized
    }

    // 1. Serialize state deterministically
    const std::string snapshot_bytes = serializeMiningState(state);

    // 2. Compute checksum (SHA256, deterministic)
    const std::string checksum_bytes = computeChecksum(snapshot_bytes);

    // 3. Increment version (monotonic, in-memory)
    const uint64_t next_version = impl_->snapshot_version + 1;

    // 4. Build atomic WriteBatch
    rocksdb::WriteBatch batch;

    batch.Put(KEY_SNAPSHOT, snapshot_bytes);
    batch.Put(KEY_CHECKSUM, checksum_bytes);
    batch.Put(KEY_VERSION, std::to_string(next_version));

    // 5. Fault injection: partial write simulation
    if (impl_->simulate_partial_write) {
        // Simulate torn write by deleting checksum from batch
        batch.Delete(KEY_CHECKSUM);
    }

    // 6. Fault injection: corruption simulation
    if (impl_->simulate_corruption) {
        // Simulate corruption by writing invalid checksum
        std::string corrupted_checksum(SHA256_DIGEST_LENGTH, '\xFF');
        batch.Put(KEY_CHECKSUM, corrupted_checksum);
    }

    // 7. Execute atomic write (sync=true for crash safety)
    rocksdb::WriteOptions wopts;
    wopts.sync = true;  // fsync for durability

    auto status = impl_->db->Write(wopts, &batch);

    if (!status.ok()) {
        // Conservative failure: return without updating version
        // Next persist will retry with same version + 1
        return;
    }

    // 8. Update in-memory version ONLY after successful write
    impl_->snapshot_version = next_version;
}

std::optional<mining_test::MiningState> ProductionPersistenceStore::recover() const {
    // Phase 4h.3: Conservative recovery from RocksDB
    // Returns nullopt on ANY inconsistency (MR3, MR4)

    // Step 1: Validate DB is initialized
    if (!impl_ || !impl_->db) {
        return std::nullopt;  // Conservative: no DB = no recovery
    }

    // Step 2: Load all metadata (all-or-nothing)
    std::string snapshot_data;
    std::string stored_checksum;
    std::string version_str;

    rocksdb::ReadOptions ropts;

    // Read snapshot
    auto status = impl_->db->Get(ropts, KEY_SNAPSHOT, &snapshot_data);
    if (!status.ok()) {
        return std::nullopt;  // No snapshot = clean start
    }

    // Read checksum
    status = impl_->db->Get(ropts, KEY_CHECKSUM, &stored_checksum);
    if (!status.ok()) {
        return std::nullopt;  // Missing checksum = partial write (MR3)
    }

    // Read version
    status = impl_->db->Get(ropts, KEY_VERSION, &version_str);
    if (!status.ok()) {
        return std::nullopt;  // Missing version = partial write (MR3)
    }

    // Step 3: Validate structural integrity

    // Validate version is valid
    uint64_t stored_version = 0;
    try {
        stored_version = std::stoull(version_str);
    } catch (...) {
        return std::nullopt;  // Invalid version = corruption
    }

    if (stored_version == 0) {
        return std::nullopt;  // Version 0 is invalid (persist starts at 1)
    }

    // Validate checksum matches (MR3 enforcement)
    const std::string computed_checksum = computeChecksum(snapshot_data);
    if (computed_checksum != stored_checksum) {
        return std::nullopt;  // Checksum mismatch = corruption (MR3)
    }

    // Step 4: Deserialize state (strict, no defaults)
    mining_test::MiningState recovered_state;
    try {
        recovered_state = deserializeMiningState(snapshot_data);
    } catch (...) {
        return std::nullopt;  // Deserialization failure = corruption
    }

    // Step 5: Validate semantic invariants (MR1-MR4)
    if (!isStateValid(recovered_state)) {
        return std::nullopt;  // Invalid state = refuse recovery
    }

    // Step 6: Determinism guard (MR5)
    // No modifications to recovered_state allowed
    // State is returned bit-identical to persisted state

    // Step 7: Success - return fully validated state
    return recovered_state;
}

// ═══════════════════════════════════════════════════════════════
// Introspection
// ═══════════════════════════════════════════════════════════════

bool ProductionPersistenceStore::hasSnapshot() const {
    // Phase 4h.3: Check if snapshot key exists in RocksDB
    if (!impl_ || !impl_->db) {
        return false;
    }

    std::string value;
    auto status = impl_->db->Get(rocksdb::ReadOptions(), KEY_SNAPSHOT, &value);
    return status.ok();
}

uint64_t ProductionPersistenceStore::snapshotVersion() const {
    // Phase 4h.3: Return in-memory version (loaded from DB in constructor)
    if (!impl_) {
        return 0;
    }
    return impl_->snapshot_version;
}

// ═══════════════════════════════════════════════════════════════
// Fault Injection (NO-OP in production)
// ═══════════════════════════════════════════════════════════════

void ProductionPersistenceStore::injectPartialWrite() {
    // Phase 4h.3: Set fault injection flag (for Phase 4g compatibility)
    if (impl_) {
        impl_->simulate_partial_write = true;
    }
}

void ProductionPersistenceStore::injectCorruption() {
    // Phase 4h.3: Set fault injection flag (for Phase 4g compatibility)
    if (impl_) {
        impl_->simulate_corruption = true;
    }
}

void ProductionPersistenceStore::clearStore() {
    // Phase 4h.3: Clear all persisted state (for testing only)
    if (!impl_ || !impl_->db) {
        return;
    }

    // Delete all keys atomically
    rocksdb::WriteBatch batch;
    batch.Delete(KEY_SNAPSHOT);
    batch.Delete(KEY_CHECKSUM);
    batch.Delete(KEY_VERSION);

    rocksdb::WriteOptions wopts;
    wopts.sync = true;

    impl_->db->Write(wopts, &batch);

    // Reset in-memory version
    impl_->snapshot_version = 0;

    // Clear fault injection flags
    impl_->simulate_partial_write = false;
    impl_->simulate_corruption = false;
}

// ═══════════════════════════════════════════════════════════════
// Internal Helpers
// ═══════════════════════════════════════════════════════════════

void ProductionPersistenceStore::validateState(
    const mining_test::MiningState& state
) const {
    // Phase 4h.2: NO-OP
    // No validation logic yet
    (void)state;  // Suppress unused warning

    // Phase 4h.3+: Validate consensus invariants before persist
}

void ProductionPersistenceStore::ensureDeterministic() const {
    // Phase 4h.2: NO-OP
    // No serialization logic yet

    // Phase 4h.3+: Enforce canonical ordering, no entropy
}

}  // namespace mining
