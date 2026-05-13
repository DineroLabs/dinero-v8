/**
 * Phase 29: Covenant Wallet Implementation
 *
 * Implements wallet-level covenant support for CTV, CSFS, TXHASH, and CCV.
 */

#include "wallet/covenant_wallet.h"
#include "consensus/covenants.h"
#include "consensus/script.h"
#include "crypto/sha256.h"
#include <openssl/rand.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>

namespace dinero {
namespace wallet {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

std::string bytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::string bytesToHex(const std::vector<uint8_t>& data) {
    return bytesToHex(data.data(), data.size());
}

std::string bytesToHex(const std::array<uint8_t, 32>& data) {
    return bytesToHex(data.data(), data.size());
}

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t byte = 0;
        for (int j = 0; j < 2 && i + j < hex.size(); ++j) {
            char c = hex[i + j];
            byte <<= 4;
            if (c >= '0' && c <= '9') byte |= (c - '0');
            else if (c >= 'a' && c <= 'f') byte |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') byte |= (c - 'A' + 10);
        }
        bytes.push_back(byte);
    }
    return bytes;
}

uint64_t currentTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::array<uint8_t, 32> sha256Array(const uint8_t* data, size_t len) {
    std::array<uint8_t, 32> hash{};
    crypto::CSHA256().Write(data, len).Finalize(hash.data());
    return hash;
}

} // anonymous namespace

// ============================================================================
// CTVTemplate Implementation
// ============================================================================

std::string CTVTemplate::toJSON() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"template_id\":\"" << template_id << "\",";
    oss << "\"template_hash\":\"" << bytesToHex(template_hash) << "\",";
    oss << "\"version\":" << version << ",";
    oss << "\"locktime\":" << locktime << ",";
    oss << "\"label\":\"" << label << "\",";
    oss << "\"created_at\":" << created_at << ",";
    oss << "\"is_spent\":" << (is_spent ? "true" : "false") << ",";
    oss << "\"spending_txid\":\"" << spending_txid << "\",";
    oss << "\"outputs\":[";
    for (size_t i = 0; i < outputs.size(); i++) {
        if (i > 0) oss << ",";
        oss << "{\"value\":" << outputs[i].value << ",";
        oss << "\"script_pubkey\":\"" << bytesToHex(outputs[i].script_pubkey) << "\",";
        oss << "\"address\":\"" << outputs[i].address << "\"}";
    }
    oss << "]}";
    return oss.str();
}

std::vector<uint8_t> CTVTemplate::serialize() const {
    std::vector<uint8_t> data;
    // Simple serialization for storage
    data.insert(data.end(), template_hash.begin(), template_hash.end());
    // Add more fields as needed
    return data;
}

// ============================================================================
// CSFSDelegation Implementation
// ============================================================================

std::string CSFSDelegation::toJSON() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"delegation_id\":\"" << delegation_id << "\",";
    oss << "\"pubkey\":\"" << bytesToHex(pubkey) << "\",";
    oss << "\"message\":\"" << bytesToHex(message) << "\",";
    oss << "\"signature\":\"" << bytesToHex(signature) << "\",";
    oss << "\"purpose\":\"" << purpose << "\",";
    oss << "\"label\":\"" << label << "\",";
    oss << "\"created_at\":" << created_at << ",";
    oss << "\"expires_at\":" << expires_at << ",";
    oss << "\"is_signed\":" << (is_signed ? "true" : "false") << ",";
    oss << "\"is_used\":" << (is_used ? "true" : "false");
    oss << "}";
    return oss.str();
}

// ============================================================================
// ContractInstance Implementation
// ============================================================================

std::string ContractInstance::toJSON() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"contract_id\":\"" << contract_id << "\",";
    oss << "\"code_hash\":\"" << bytesToHex(code_hash) << "\",";
    oss << "\"state_counter\":" << state_counter << ",";
    oss << "\"state_data\":\"" << bytesToHex(state_data) << "\",";
    oss << "\"state_hash\":\"" << bytesToHex(state_hash) << "\",";
    oss << "\"current_txid\":\"" << current_txid << "\",";
    oss << "\"current_vout\":" << current_vout << ",";
    oss << "\"locked_value\":" << locked_value << ",";
    oss << "\"label\":\"" << label << "\",";
    oss << "\"contract_type\":\"" << contract_type << "\",";
    oss << "\"created_at\":" << created_at << ",";
    oss << "\"is_active\":" << (is_active ? "true" : "false");
    oss << "}";
    return oss.str();
}

// ============================================================================
// CovenantUTXO Implementation
// ============================================================================

std::string CovenantUTXO::toJSON() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"txid\":\"" << txid << "\",";
    oss << "\"vout\":" << vout << ",";
    oss << "\"value\":" << value << ",";
    oss << "\"script_pubkey\":\"" << bytesToHex(script_pubkey) << "\",";
    oss << "\"height\":" << height << ",";
    oss << "\"is_spent\":" << (is_spent ? "true" : "false") << ",";
    oss << "\"covenant_type\":" << static_cast<int>(covenant_type) << ",";
    oss << "\"covenant_id\":\"" << covenant_id << "\"";
    oss << "}";
    return oss.str();
}

// ============================================================================
// CovenantSpendInfo Implementation
// ============================================================================

bool CovenantSpendInfo::canSpend() const {
    switch (type) {
        case CovenantType::CTV:
            return required_tx_template.has_value();
        case CovenantType::CSFS:
            return required_signature.has_value() && !required_signature->empty();
        case CovenantType::CCV:
            return required_new_state.has_value();
        default:
            return true;
    }
}

std::string CovenantSpendInfo::getMissingRequirement() const {
    switch (type) {
        case CovenantType::CTV:
            if (!required_tx_template.has_value())
                return "Missing CTV template transaction";
            break;
        case CovenantType::CSFS:
            if (!required_signature.has_value() || required_signature->empty())
                return "Missing CSFS signature";
            break;
        case CovenantType::CCV:
            if (!required_new_state.has_value())
                return "Missing new contract state";
            break;
        default:
            break;
    }
    return "";
}

std::string CovenantSpendInfo::toJSON() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"utxo_txid\":\"" << utxo_txid << "\",";
    oss << "\"utxo_vout\":" << utxo_vout << ",";
    oss << "\"type\":" << static_cast<int>(type) << ",";
    oss << "\"can_spend\":" << (canSpend() ? "true" : "false") << ",";
    oss << "\"missing\":\"" << getMissingRequirement() << "\"";
    oss << "}";
    return oss.str();
}

// ============================================================================
// CovenantWallet Implementation
// ============================================================================

CovenantWallet::CovenantWallet(const std::string& wallet_db_path)
    : db_(nullptr)
    , db_path_(wallet_db_path)
    , initialized_(false)
    , stmt_insert_template_(nullptr)
    , stmt_get_template_(nullptr)
    , stmt_insert_delegation_(nullptr)
    , stmt_insert_contract_(nullptr)
    , stmt_insert_covenant_utxo_(nullptr)
{
}

CovenantWallet::~CovenantWallet() {
    shutdown();
}

bool CovenantWallet::initialize() {
    if (initialized_) return true;

    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        return false;
    }

    // Enable WAL mode for better concurrency
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    if (!createSchema()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    if (!prepareStatements()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    initialized_ = true;
    return true;
}

void CovenantWallet::shutdown() {
    if (!initialized_) return;

    finalizeStatements();

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }

    initialized_ = false;
}

bool CovenantWallet::createSchema() {
    const char* schema = R"SQL(
        -- CTV Templates table
        CREATE TABLE IF NOT EXISTS ctv_templates (
            template_id TEXT PRIMARY KEY,
            template_hash BLOB NOT NULL UNIQUE,
            version INTEGER NOT NULL,
            locktime INTEGER NOT NULL,
            outputs_json TEXT NOT NULL,
            input_sequences_json TEXT,
            label TEXT,
            created_at INTEGER NOT NULL,
            is_spent INTEGER DEFAULT 0,
            spending_txid TEXT
        );

        CREATE INDEX IF NOT EXISTS idx_ctv_hash ON ctv_templates(template_hash);

        -- CSFS Delegations table
        CREATE TABLE IF NOT EXISTS csfs_delegations (
            delegation_id TEXT PRIMARY KEY,
            pubkey BLOB NOT NULL,
            message BLOB NOT NULL,
            signature BLOB,
            purpose TEXT,
            label TEXT,
            created_at INTEGER NOT NULL,
            expires_at INTEGER DEFAULT 0,
            is_signed INTEGER DEFAULT 0,
            is_used INTEGER DEFAULT 0
        );

        -- Contract Instances table
        CREATE TABLE IF NOT EXISTS contracts (
            contract_id TEXT PRIMARY KEY,
            code_hash BLOB NOT NULL,
            state_counter INTEGER NOT NULL,
            state_data BLOB,
            state_hash BLOB NOT NULL,
            current_txid TEXT,
            current_vout INTEGER,
            locked_value INTEGER,
            label TEXT,
            contract_type TEXT,
            created_at INTEGER NOT NULL,
            is_active INTEGER DEFAULT 1
        );

        -- Contract State History table
        CREATE TABLE IF NOT EXISTS contract_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            contract_id TEXT NOT NULL,
            from_counter INTEGER NOT NULL,
            to_counter INTEGER NOT NULL,
            txid TEXT NOT NULL,
            timestamp INTEGER NOT NULL,
            old_data BLOB,
            new_data BLOB,
            FOREIGN KEY (contract_id) REFERENCES contracts(contract_id)
        );

        -- Covenant UTXOs table
        CREATE TABLE IF NOT EXISTS covenant_utxos (
            txid TEXT NOT NULL,
            vout INTEGER NOT NULL,
            value INTEGER NOT NULL,
            script_pubkey BLOB NOT NULL,
            height INTEGER NOT NULL,
            is_spent INTEGER DEFAULT 0,
            covenant_type INTEGER NOT NULL,
            covenant_id TEXT,
            ctv_hash BLOB,
            required_pubkey BLOB,
            contract_state_hash BLOB,
            requires_template_match INTEGER DEFAULT 0,
            requires_signature INTEGER DEFAULT 0,
            requires_state_transition INTEGER DEFAULT 0,
            estimated_witness_size INTEGER DEFAULT 0,
            spending_tx_outputs INTEGER DEFAULT 0,
            PRIMARY KEY (txid, vout)
        );

        CREATE INDEX IF NOT EXISTS idx_covenant_type ON covenant_utxos(covenant_type);
        CREATE INDEX IF NOT EXISTS idx_covenant_spent ON covenant_utxos(is_spent);

        -- Schema version
        CREATE TABLE IF NOT EXISTS covenant_meta (
            key TEXT PRIMARY KEY,
            value TEXT
        );

        INSERT OR IGNORE INTO covenant_meta (key, value) VALUES ('schema_version', '1');
    )SQL";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool CovenantWallet::prepareStatements() {
    const char* sql_insert_template = R"SQL(
        INSERT INTO ctv_templates (template_id, template_hash, version, locktime,
            outputs_json, input_sequences_json, label, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )SQL";

    const char* sql_get_template = R"SQL(
        SELECT template_id, template_hash, version, locktime, outputs_json,
            input_sequences_json, label, created_at, is_spent, spending_txid
        FROM ctv_templates WHERE template_id = ?
    )SQL";

    const char* sql_insert_delegation = R"SQL(
        INSERT INTO csfs_delegations (delegation_id, pubkey, message, purpose,
            label, created_at, expires_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )SQL";

    const char* sql_insert_contract = R"SQL(
        INSERT INTO contracts (contract_id, code_hash, state_counter, state_data,
            state_hash, label, contract_type, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )SQL";

    const char* sql_insert_utxo = R"SQL(
        INSERT OR REPLACE INTO covenant_utxos (txid, vout, value, script_pubkey,
            height, covenant_type, covenant_id, ctv_hash, required_pubkey,
            requires_template_match, requires_signature, requires_state_transition,
            estimated_witness_size, spending_tx_outputs)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL";

    if (sqlite3_prepare_v2(db_, sql_insert_template, -1, &stmt_insert_template_, nullptr) != SQLITE_OK)
        return false;
    if (sqlite3_prepare_v2(db_, sql_get_template, -1, &stmt_get_template_, nullptr) != SQLITE_OK)
        return false;
    if (sqlite3_prepare_v2(db_, sql_insert_delegation, -1, &stmt_insert_delegation_, nullptr) != SQLITE_OK)
        return false;
    if (sqlite3_prepare_v2(db_, sql_insert_contract, -1, &stmt_insert_contract_, nullptr) != SQLITE_OK)
        return false;
    if (sqlite3_prepare_v2(db_, sql_insert_utxo, -1, &stmt_insert_covenant_utxo_, nullptr) != SQLITE_OK)
        return false;

    return true;
}

void CovenantWallet::finalizeStatements() {
    if (stmt_insert_template_) { sqlite3_finalize(stmt_insert_template_); stmt_insert_template_ = nullptr; }
    if (stmt_get_template_) { sqlite3_finalize(stmt_get_template_); stmt_get_template_ = nullptr; }
    if (stmt_insert_delegation_) { sqlite3_finalize(stmt_insert_delegation_); stmt_insert_delegation_ = nullptr; }
    if (stmt_insert_contract_) { sqlite3_finalize(stmt_insert_contract_); stmt_insert_contract_ = nullptr; }
    if (stmt_insert_covenant_utxo_) { sqlite3_finalize(stmt_insert_covenant_utxo_); stmt_insert_covenant_utxo_ = nullptr; }
}

std::string CovenantWallet::generateUUID() const {
    uint8_t bytes[16];
    RAND_bytes(bytes, 16);

    // Set version 4 (random) UUID
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    std::ostringstream oss;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) oss << "-";
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// CTV Template Management
// ─────────────────────────────────────────────────────────────────────────────

std::string CovenantWallet::createCTVTemplate(
    const std::vector<CTVTemplate::CommittedOutput>& outputs,
    uint32_t locktime,
    const std::string& label)
{
    if (!initialized_) return "";

    CTVTemplate tmpl;
    tmpl.template_id = generateUUID();
    tmpl.version = 2;  // Default version
    tmpl.locktime = locktime;
    tmpl.outputs = outputs;
    tmpl.label = label;
    tmpl.created_at = currentTimestamp();
    tmpl.is_spent = false;

    // Build a transaction to compute the CTV hash
    Transaction tx;
    tx.version = tmpl.version;
    tx.lockTime = tmpl.locktime;

    // Add a placeholder input (CTV doesn't commit to input prevouts)
    TxInput vin;
    vin.prevout.txid = uint256();  // All zeros (null hash)
    vin.prevout.vout = 0;
    vin.sequence = 0xfffffffe;
    tx.vin.push_back(vin);
    tmpl.input_sequences.push_back(vin.sequence);

    // Add outputs
    for (const auto& out : outputs) {
        TxOutput vout;
        vout.value = out.value;
        vout.scriptPubKey = out.script_pubkey;
        tx.vout.push_back(vout);
    }

    // Compute CTV hash for the NEW template being created
    // Phase C.1: This is ALLOWED - wallet computing hash for template CREATION
    // Wallet CANNOT use ComputeCTVHash() for VALIDATION (checking existing txs)
    tmpl.template_hash = consensus::ComputeCTVHash(tx, 0);

    // Serialize outputs for storage
    std::ostringstream outputs_json;
    outputs_json << "[";
    for (size_t i = 0; i < outputs.size(); i++) {
        if (i > 0) outputs_json << ",";
        outputs_json << "{\"value\":" << outputs[i].value << ",";
        outputs_json << "\"script_pubkey\":\"" << bytesToHex(outputs[i].script_pubkey) << "\",";
        outputs_json << "\"address\":\"" << outputs[i].address << "\"}";
    }
    outputs_json << "]";

    // Store in database
    sqlite3_reset(stmt_insert_template_);
    sqlite3_bind_text(stmt_insert_template_, 1, tmpl.template_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt_insert_template_, 2, tmpl.template_hash.data(), 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_insert_template_, 3, tmpl.version);
    sqlite3_bind_int(stmt_insert_template_, 4, tmpl.locktime);
    sqlite3_bind_text(stmt_insert_template_, 5, outputs_json.str().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_template_, 6, "[]", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_insert_template_, 7, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_insert_template_, 8, tmpl.created_at);

    if (sqlite3_step(stmt_insert_template_) != SQLITE_DONE) {
        return "";
    }

    return tmpl.template_id;
}

std::optional<CTVTemplate> CovenantWallet::getCTVTemplate(const std::string& template_id) const {
    if (!initialized_) return std::nullopt;

    sqlite3_reset(stmt_get_template_);
    sqlite3_bind_text(stmt_get_template_, 1, template_id.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt_get_template_) != SQLITE_ROW) {
        return std::nullopt;
    }

    CTVTemplate tmpl;
    tmpl.template_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_template_, 0));

    const void* hash_blob = sqlite3_column_blob(stmt_get_template_, 1);
    if (hash_blob) {
        std::memcpy(tmpl.template_hash.data(), hash_blob, 32);
    }

    tmpl.version = sqlite3_column_int(stmt_get_template_, 2);
    tmpl.locktime = sqlite3_column_int(stmt_get_template_, 3);

    // Parse outputs_json at column 4
    const char* outputs_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_template_, 4));
    if (outputs_json && strlen(outputs_json) > 2) {
        std::string json(outputs_json);
        // Parse JSON array: [{"value":123,"script_pubkey":"hex","address":"addr"},...]
        size_t pos = 0;
        while ((pos = json.find("{\"value\":", pos)) != std::string::npos) {
            CTVTemplate::CommittedOutput out;

            // Parse value
            size_t val_start = pos + 9;
            size_t val_end = json.find(',', val_start);
            if (val_end != std::string::npos) {
                out.value = std::stoull(json.substr(val_start, val_end - val_start));
            }

            // Parse script_pubkey
            size_t sp_start = json.find("\"script_pubkey\":\"", pos);
            if (sp_start != std::string::npos) {
                sp_start += 17;
                size_t sp_end = json.find('"', sp_start);
                if (sp_end != std::string::npos) {
                    std::string hex_script = json.substr(sp_start, sp_end - sp_start);
                    out.script_pubkey = hexToBytes(hex_script);
                }
            }

            // Parse address
            size_t addr_start = json.find("\"address\":\"", pos);
            if (addr_start != std::string::npos) {
                addr_start += 11;
                size_t addr_end = json.find('"', addr_start);
                if (addr_end != std::string::npos) {
                    out.address = json.substr(addr_start, addr_end - addr_start);
                }
            }

            tmpl.outputs.push_back(out);
            pos = json.find('}', pos) + 1;
        }
    }

    const char* label_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_template_, 6));
    if (label_str) tmpl.label = label_str;
    tmpl.created_at = sqlite3_column_int64(stmt_get_template_, 7);
    tmpl.is_spent = sqlite3_column_int(stmt_get_template_, 8) != 0;
    const char* spending = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_template_, 9));
    if (spending) tmpl.spending_txid = spending;

    return tmpl;
}

std::vector<CTVTemplate> CovenantWallet::listCTVTemplates(bool include_spent) const {
    std::vector<CTVTemplate> templates;
    if (!initialized_) return templates;

    std::string sql = "SELECT template_id, template_hash, version, locktime, outputs_json, "
                      "input_sequences_json, label, created_at, is_spent, spending_txid "
                      "FROM ctv_templates";
    if (!include_spent) {
        sql += " WHERE is_spent = 0";
    }
    sql += " ORDER BY created_at DESC";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return templates;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CTVTemplate tmpl;
        tmpl.template_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

        const void* hash_blob = sqlite3_column_blob(stmt, 1);
        if (hash_blob) {
            std::memcpy(tmpl.template_hash.data(), hash_blob, 32);
        }

        tmpl.version = sqlite3_column_int(stmt, 2);
        tmpl.locktime = sqlite3_column_int(stmt, 3);
        const char* lbl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        if (lbl) tmpl.label = lbl;
        tmpl.created_at = sqlite3_column_int64(stmt, 7);
        tmpl.is_spent = sqlite3_column_int(stmt, 8) != 0;
        const char* spending = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        if (spending) tmpl.spending_txid = spending;

        templates.push_back(tmpl);
    }

    sqlite3_finalize(stmt);
    return templates;
}

std::vector<uint8_t> CovenantWallet::generateCTVScript(
    const std::array<uint8_t, 32>& template_hash) const
{
    std::vector<uint8_t> script;

    // OP_PUSH32 <template_hash> OP_CHECKTEMPLATEVERIFY
    script.push_back(0x20);  // Push 32 bytes
    script.insert(script.end(), template_hash.begin(), template_hash.end());
    script.push_back(static_cast<uint8_t>(consensus::OP_CHECKTEMPLATEVERIFY));

    return script;
}

Transaction CovenantWallet::createCTVSpendingTx(
    const std::string& template_id,
    uint32_t input_index)
{
    Transaction tx;

    auto tmpl_opt = getCTVTemplate(template_id);
    if (!tmpl_opt) return tx;

    const auto& tmpl = *tmpl_opt;

    tx.version = tmpl.version;
    tx.lockTime = tmpl.locktime;

    // Outputs must exactly match the template
    for (const auto& out : tmpl.outputs) {
        TxOutput vout;
        vout.value = out.value;
        vout.scriptPubKey = out.script_pubkey;
        tx.vout.push_back(vout);
    }

    // Input sequences must match
    for (uint32_t seq : tmpl.input_sequences) {
        TxInput vin;
        vin.sequence = seq;
        tx.vin.push_back(vin);
    }

    return tx;
}

// ─────────────────────────────────────────────────────────────────────────────
// CSFS Delegation Management
// ─────────────────────────────────────────────────────────────────────────────

std::string CovenantWallet::createCSFSDelegation(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& message,
    const std::string& purpose,
    uint64_t expires_at)
{
    if (!initialized_) return "";
    if (pubkey.size() != 32) return "";  // Must be x-only pubkey

    std::string delegation_id = generateUUID();

    sqlite3_reset(stmt_insert_delegation_);
    sqlite3_bind_text(stmt_insert_delegation_, 1, delegation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt_insert_delegation_, 2, pubkey.data(), pubkey.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt_insert_delegation_, 3, message.data(), message.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_delegation_, 4, purpose.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_delegation_, 5, "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_insert_delegation_, 6, currentTimestamp());
    sqlite3_bind_int64(stmt_insert_delegation_, 7, expires_at);

    if (sqlite3_step(stmt_insert_delegation_) != SQLITE_DONE) {
        return "";
    }

    return delegation_id;
}

bool CovenantWallet::addCSFSSignature(
    const std::string& delegation_id,
    const std::vector<uint8_t>& signature)
{
    if (!initialized_) return false;
    if (signature.size() != 64) return false;  // Schnorr signatures are 64 bytes

    const char* sql = "UPDATE csfs_delegations SET signature = ?, is_signed = 1 WHERE delegation_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_blob(stmt, 1, signature.data(), signature.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, delegation_id.c_str(), -1, SQLITE_TRANSIENT);

    bool success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);

    return success;
}

std::optional<CSFSDelegation> CovenantWallet::getCSFSDelegation(
    const std::string& delegation_id) const
{
    if (!initialized_) return std::nullopt;

    const char* sql = "SELECT delegation_id, pubkey, message, signature, purpose, "
                      "label, created_at, expires_at, is_signed, is_used "
                      "FROM csfs_delegations WHERE delegation_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, delegation_id.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    CSFSDelegation del;
    del.delegation_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

    const void* pk = sqlite3_column_blob(stmt, 1);
    int pk_len = sqlite3_column_bytes(stmt, 1);
    if (pk && pk_len > 0) {
        del.pubkey.assign(static_cast<const uint8_t*>(pk),
                          static_cast<const uint8_t*>(pk) + pk_len);
    }

    const void* msg = sqlite3_column_blob(stmt, 2);
    int msg_len = sqlite3_column_bytes(stmt, 2);
    if (msg && msg_len > 0) {
        del.message.assign(static_cast<const uint8_t*>(msg),
                           static_cast<const uint8_t*>(msg) + msg_len);
    }

    const void* sig = sqlite3_column_blob(stmt, 3);
    int sig_len = sqlite3_column_bytes(stmt, 3);
    if (sig && sig_len > 0) {
        del.signature.assign(static_cast<const uint8_t*>(sig),
                             static_cast<const uint8_t*>(sig) + sig_len);
    }

    const char* purp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    if (purp) del.purpose = purp;
    const char* lbl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    if (lbl) del.label = lbl;
    del.created_at = sqlite3_column_int64(stmt, 6);
    del.expires_at = sqlite3_column_int64(stmt, 7);
    del.is_signed = sqlite3_column_int(stmt, 8) != 0;
    del.is_used = sqlite3_column_int(stmt, 9) != 0;

    sqlite3_finalize(stmt);
    return del;
}

// ─────────────────────────────────────────────────────────────────────────────
// Contract Management
// ─────────────────────────────────────────────────────────────────────────────

std::string CovenantWallet::registerContract(
    const std::vector<uint8_t>& code,
    const std::vector<uint8_t>& initial_data,
    const std::string& contract_type,
    const std::string& label)
{
    if (!initialized_) return "";

    std::string contract_id = generateUUID();

    // Compute code hash
    std::array<uint8_t, 32> code_hash = sha256Array(code.data(), code.size());

    // Compute initial state hash
    std::array<uint8_t, 32> state_hash;
    std::vector<uint8_t> state_preimage;
    state_preimage.insert(state_preimage.end(), code_hash.begin(), code_hash.end());
    uint32_t counter = 0;
    state_preimage.push_back(counter & 0xFF);
    state_preimage.push_back((counter >> 8) & 0xFF);
    state_preimage.push_back((counter >> 16) & 0xFF);
    state_preimage.push_back((counter >> 24) & 0xFF);
    state_preimage.insert(state_preimage.end(), initial_data.begin(), initial_data.end());

    std::array<uint8_t, 32> state_hash = sha256Array(state_preimage.data(), state_preimage.size());

    sqlite3_reset(stmt_insert_contract_);
    sqlite3_bind_text(stmt_insert_contract_, 1, contract_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt_insert_contract_, 2, code_hash.data(), 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_insert_contract_, 3, 0);  // Initial counter = 0
    sqlite3_bind_blob(stmt_insert_contract_, 4, initial_data.data(), initial_data.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt_insert_contract_, 5, state_hash.data(), 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_contract_, 6, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_contract_, 7, contract_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_insert_contract_, 8, currentTimestamp());

    if (sqlite3_step(stmt_insert_contract_) != SQLITE_DONE) {
        return "";
    }

    return contract_id;
}

// ─────────────────────────────────────────────────────────────────────────────
// Covenant UTXO Tracking
// ─────────────────────────────────────────────────────────────────────────────

bool CovenantWallet::addCovenantUTXO(const CovenantUTXO& utxo) {
    if (!initialized_) return false;

    sqlite3_reset(stmt_insert_covenant_utxo_);
    sqlite3_bind_text(stmt_insert_covenant_utxo_, 1, utxo.txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_insert_covenant_utxo_, 2, utxo.vout);
    sqlite3_bind_int64(stmt_insert_covenant_utxo_, 3, utxo.value);
    sqlite3_bind_blob(stmt_insert_covenant_utxo_, 4, utxo.script_pubkey.data(),
                      utxo.script_pubkey.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_insert_covenant_utxo_, 5, utxo.height);
    sqlite3_bind_int(stmt_insert_covenant_utxo_, 6, static_cast<int>(utxo.covenant_type));
    sqlite3_bind_text(stmt_insert_covenant_utxo_, 7, utxo.covenant_id.c_str(), -1, SQLITE_TRANSIENT);

    if (utxo.ctv_hash) {
        sqlite3_bind_blob(stmt_insert_covenant_utxo_, 8, utxo.ctv_hash->data(), 32, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt_insert_covenant_utxo_, 8);
    }

    if (utxo.required_pubkey) {
        sqlite3_bind_blob(stmt_insert_covenant_utxo_, 9, utxo.required_pubkey->data(),
                          utxo.required_pubkey->size(), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt_insert_covenant_utxo_, 9);
    }

    sqlite3_bind_int(stmt_insert_covenant_utxo_, 10, utxo.requires_template_match ? 1 : 0);
    sqlite3_bind_int(stmt_insert_covenant_utxo_, 11, utxo.requires_signature ? 1 : 0);
    sqlite3_bind_int(stmt_insert_covenant_utxo_, 12, utxo.requires_state_transition ? 1 : 0);
    sqlite3_bind_int(stmt_insert_covenant_utxo_, 13, utxo.estimated_witness_size);
    sqlite3_bind_int(stmt_insert_covenant_utxo_, 14, utxo.spending_tx_outputs);

    return sqlite3_step(stmt_insert_covenant_utxo_) == SQLITE_DONE;
}

std::optional<CovenantUTXO> CovenantWallet::getCovenantUTXO(
    const std::string& txid, uint32_t vout) const
{
    if (!initialized_) return std::nullopt;

    std::string sql = "SELECT txid, vout, value, script_pubkey, height, is_spent, "
                      "covenant_type, covenant_id, ctv_hash, required_pubkey, "
                      "requires_template_match, requires_signature, requires_state_transition, "
                      "estimated_witness_size, spending_tx_outputs FROM covenant_utxos "
                      "WHERE txid = ? AND vout = ?";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, vout);

    std::optional<CovenantUTXO> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        CovenantUTXO utxo;
        utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        utxo.vout = sqlite3_column_int(stmt, 1);
        utxo.value = sqlite3_column_int64(stmt, 2);

        const void* spk = sqlite3_column_blob(stmt, 3);
        int spk_len = sqlite3_column_bytes(stmt, 3);
        if (spk && spk_len > 0) {
            utxo.script_pubkey.assign(static_cast<const uint8_t*>(spk),
                                      static_cast<const uint8_t*>(spk) + spk_len);
        }

        utxo.height = sqlite3_column_int(stmt, 4);
        utxo.is_spent = sqlite3_column_int(stmt, 5) != 0;
        utxo.covenant_type = static_cast<CovenantType>(sqlite3_column_int(stmt, 6));
        const char* cov_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (cov_id) utxo.covenant_id = cov_id;

        const void* ctv = sqlite3_column_blob(stmt, 8);
        if (ctv && sqlite3_column_bytes(stmt, 8) == 32) {
            std::array<uint8_t, 32> hash;
            std::memcpy(hash.data(), ctv, 32);
            utxo.ctv_hash = hash;
        }

        utxo.requires_template_match = sqlite3_column_int(stmt, 10) != 0;
        utxo.requires_signature = sqlite3_column_int(stmt, 11) != 0;
        utxo.requires_state_transition = sqlite3_column_int(stmt, 12) != 0;
        utxo.estimated_witness_size = sqlite3_column_int(stmt, 13);
        utxo.spending_tx_outputs = sqlite3_column_int(stmt, 14);

        result = utxo;
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<CovenantUTXO> CovenantWallet::listCovenantUTXOs(
    CovenantType type,
    bool include_spent) const
{
    std::vector<CovenantUTXO> utxos;
    if (!initialized_) return utxos;

    std::string sql = "SELECT txid, vout, value, script_pubkey, height, is_spent, "
                      "covenant_type, covenant_id, ctv_hash, required_pubkey, "
                      "requires_template_match, requires_signature, requires_state_transition, "
                      "estimated_witness_size, spending_tx_outputs FROM covenant_utxos WHERE 1=1";

    if (type != CovenantType::NONE) {
        sql += " AND covenant_type = " + std::to_string(static_cast<int>(type));
    }
    if (!include_spent) {
        sql += " AND is_spent = 0";
    }

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return utxos;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CovenantUTXO utxo;
        utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        utxo.vout = sqlite3_column_int(stmt, 1);
        utxo.value = sqlite3_column_int64(stmt, 2);

        const void* spk = sqlite3_column_blob(stmt, 3);
        int spk_len = sqlite3_column_bytes(stmt, 3);
        if (spk && spk_len > 0) {
            utxo.script_pubkey.assign(static_cast<const uint8_t*>(spk),
                                      static_cast<const uint8_t*>(spk) + spk_len);
        }

        utxo.height = sqlite3_column_int(stmt, 4);
        utxo.is_spent = sqlite3_column_int(stmt, 5) != 0;
        utxo.covenant_type = static_cast<CovenantType>(sqlite3_column_int(stmt, 6));
        const char* cov_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (cov_id) utxo.covenant_id = cov_id;

        const void* ctv = sqlite3_column_blob(stmt, 8);
        if (ctv && sqlite3_column_bytes(stmt, 8) == 32) {
            std::array<uint8_t, 32> hash;
            std::memcpy(hash.data(), ctv, 32);
            utxo.ctv_hash = hash;
        }

        utxo.requires_template_match = sqlite3_column_int(stmt, 10) != 0;
        utxo.requires_signature = sqlite3_column_int(stmt, 11) != 0;
        utxo.requires_state_transition = sqlite3_column_int(stmt, 12) != 0;
        utxo.estimated_witness_size = sqlite3_column_int(stmt, 13);
        utxo.spending_tx_outputs = sqlite3_column_int(stmt, 14);

        utxos.push_back(utxo);
    }

    sqlite3_finalize(stmt);
    return utxos;
}

bool CovenantWallet::markCovenantSpent(
    const std::string& txid, uint32_t vout,
    const std::string& spending_txid)
{
    if (!initialized_) return false;

    const char* sql = "UPDATE covenant_utxos SET is_spent = 1 WHERE txid = ? AND vout = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, vout);

    bool success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);

    return success;
}

// ─────────────────────────────────────────────────────────────────────────────
// Script Analysis
// ─────────────────────────────────────────────────────────────────────────────

CovenantWallet::ScriptAnalysis CovenantWallet::analyzeScript(
    const std::vector<uint8_t>& script)
{
    ScriptAnalysis result;
    result.type = CovenantType::NONE;
    result.has_ctv = false;
    result.has_csfs = false;
    result.has_txhash = false;
    result.has_ccv = false;

    for (size_t i = 0; i < script.size(); i++) {
        uint8_t op = script[i];

        // Check for covenant opcodes
        if (op == static_cast<uint8_t>(consensus::OP_CHECKTEMPLATEVERIFY)) {
            result.has_ctv = true;
            result.type = CovenantType::CTV;

            // Try to extract CTV hash (should be 32 bytes before this opcode)
            if (i >= 33 && script[i - 33] == 0x20) {  // OP_PUSH32
                std::array<uint8_t, 32> hash;
                std::memcpy(hash.data(), &script[i - 32], 32);
                result.ctv_hash = hash;
            }
        }
        else if (op == static_cast<uint8_t>(consensus::OP_CHECKSIGFROMSTACK) ||
                 op == static_cast<uint8_t>(consensus::OP_CHECKSIGFROMSTACKVERIFY)) {
            result.has_csfs = true;
            if (result.type == CovenantType::NONE) {
                result.type = CovenantType::CSFS;
            } else {
                result.type = CovenantType::COMPOSITE;
            }
        }
        else if (op == static_cast<uint8_t>(consensus::OP_TXHASH)) {
            result.has_txhash = true;
            if (result.type == CovenantType::NONE) {
                result.type = CovenantType::TXHASH;
            } else {
                result.type = CovenantType::COMPOSITE;
            }
        }
        else if (op == static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY)) {
            result.has_ccv = true;
            if (result.type == CovenantType::NONE) {
                result.type = CovenantType::CCV;
            } else {
                result.type = CovenantType::COMPOSITE;
            }
        }

        // Skip push data
        if (op <= 0x4b) {
            i += op;  // Skip op bytes
        } else if (op == 0x4c) {
            if (i + 1 < script.size()) {
                i += 1 + script[i + 1];
            }
        } else if (op == 0x4d) {
            if (i + 2 < script.size()) {
                uint16_t len = script[i + 1] | (script[i + 2] << 8);
                i += 2 + len;
            }
        }
    }

    // Generate human-readable description
    std::ostringstream desc;
    if (result.has_ctv) desc << "CTV (template-locked) ";
    if (result.has_csfs) desc << "CSFS (delegated-signing) ";
    if (result.has_txhash) desc << "TXHASH (introspection) ";
    if (result.has_ccv) desc << "CCV (stateful-contract) ";
    if (desc.str().empty()) desc << "Standard script (no covenants)";
    result.human_readable = desc.str();

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// REMOVED: validateCovenantTx() - Phase C.1 Boundary Violation Fix
// ═══════════════════════════════════════════════════════════════════════════
//
// This method was removed because it violated the consensus/wallet boundary
// by calling consensus validation functions (VIOLATION_MARKER: see below).
//
// Previous implementation (lines 1108-1150):
//   - Called consensus hash computation for validation purposes
//   - Returned ValidationResult with valid/invalid based on covenant rules
//   - This is FORBIDDEN - wallet must NEVER validate consensus rules
//
// Rationale:
//   - Consensus validation MUST have single source of truth
//   - Wallet validation creates second implementation that can diverge
//   - Leads to wallet accepting txs that consensus rejects
//
// Validation happens ONLY in consensus::ScriptInterpreter.
// See include/wallet/covenant_wallet.h for boundary rules.
// ═══════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// Fee Estimation
// ─────────────────────────────────────────────────────────────────────────────

CovenantFeeEstimate CovenantWallet::estimateCovenantFee(
    const std::string& txid, uint32_t vout,
    uint64_t fee_rate_una_vb) const
{
    CovenantFeeEstimate est;
    est.fee_rate_una_vb = fee_rate_una_vb;

    auto utxo_opt = getCovenantUTXO(txid, vout);
    if (!utxo_opt) {
        // Return default estimate for non-covenant UTXOs
        est.input_count = 1;
        est.output_count = 2;  // Assume 2 outputs
        est.witness_vbytes = 108;  // Standard P2WPKH witness
        est.tx_vsize = 140;
        est.tx_weight = est.tx_vsize * 4;
        est.base_fee = est.tx_vsize * fee_rate_una_vb;
        est.witness_fee = 0;
        est.total_fee = est.base_fee;
        return est;
    }

    const auto& utxo = *utxo_opt;

    // Estimate based on covenant type
    switch (utxo.covenant_type) {
        case CovenantType::CTV:
            // CTV spending is very efficient - just needs matching tx structure
            est.input_count = 1;
            est.output_count = utxo.spending_tx_outputs > 0 ? utxo.spending_tx_outputs : 2;
            est.witness_vbytes = 33;  // Just the control block for P2TR
            break;

        case CovenantType::CSFS:
            // CSFS needs pubkey (32) + message (32) + signature (64)
            est.input_count = 1;
            est.output_count = 2;
            est.witness_vbytes = 128 + 40;  // Extra witness data
            break;

        case CovenantType::CCV:
            // Contract transitions are heavier
            est.input_count = 1;
            est.output_count = 2;
            est.witness_vbytes = utxo.estimated_witness_size > 0 ? utxo.estimated_witness_size : 200;
            break;

        default:
            est.witness_vbytes = 108;
            break;
    }

    // Calculate sizes
    uint32_t base_size = 10 + (est.input_count * 41) + (est.output_count * 34);
    uint32_t witness_size = est.witness_vbytes * est.input_count;

    est.tx_weight = base_size * 3 + base_size + witness_size;
    est.tx_vsize = (est.tx_weight + 3) / 4;

    est.base_fee = (base_size * fee_rate_una_vb);
    est.witness_fee = (witness_size * fee_rate_una_vb) / 4;  // Witness discount
    est.total_fee = est.tx_vsize * fee_rate_una_vb;

    return est;
}

// ─────────────────────────────────────────────────────────────────────────────
// Statistics
// ─────────────────────────────────────────────────────────────────────────────

CovenantWallet::CovenantStats CovenantWallet::getStats() const {
    CovenantStats stats = {};
    if (!initialized_) return stats;

    // CTV templates
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM ctv_templates", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.total_ctv_templates = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM ctv_templates WHERE is_spent = 0", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.active_ctv_templates = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Delegations
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM csfs_delegations", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.total_delegations = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM csfs_delegations WHERE is_signed = 1", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.signed_delegations = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Contracts
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM contracts", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.total_contracts = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM contracts WHERE is_active = 1", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.active_contracts = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Covenant UTXOs
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*), COALESCE(SUM(value), 0) FROM covenant_utxos WHERE is_spent = 0", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.covenant_utxos = sqlite3_column_int64(stmt, 0);
            stats.covenant_value_locked = sqlite3_column_int64(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    return stats;
}

// ============================================================================
// CovenantTxBuilder Implementation
// ============================================================================

CovenantTxBuilder::CovenantTxBuilder() {
    tx_.version = 2;
    tx_.lockTime = 0;
}

CovenantTxBuilder& CovenantTxBuilder::withCTVTemplate(const CTVTemplate& tmpl) {
    ctv_template_ = tmpl;
    tx_.version = tmpl.version;
    tx_.lockTime = tmpl.locktime;

    // Set required outputs from template
    tx_.vout.clear();
    for (const auto& out : tmpl.outputs) {
        TxOutput vout;
        vout.value = out.value;
        vout.scriptPubKey = out.script_pubkey;
        tx_.vout.push_back(vout);
    }

    return *this;
}

CovenantTxBuilder& CovenantTxBuilder::withCTVHash(const std::array<uint8_t, 32>& hash) {
    // Store hash for later verification
    CTVTemplate tmpl;
    tmpl.template_hash = hash;
    ctv_template_ = tmpl;
    return *this;
}

CovenantTxBuilder& CovenantTxBuilder::withCSFSSignature(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& message,
    const std::vector<uint8_t>& signature)
{
    csfs_signatures_.push_back({pubkey, message, signature});
    return *this;
}

CovenantTxBuilder& CovenantTxBuilder::setVersion(int32_t version) {
    tx_.version = version;
    return *this;
}

CovenantTxBuilder& CovenantTxBuilder::setLocktime(uint32_t locktime) {
    tx_.lockTime = locktime;
    return *this;
}

CovenantTxBuilder& CovenantTxBuilder::addInput(
    const std::string& txid, uint32_t vout, uint32_t sequence)
{
    TxInput vin;
    vin.prevout.txid = uint256::FromHexUnsafe(txid);
    vin.prevout.vout = vout;
    vin.sequence = sequence;
    tx_.vin.push_back(vin);
    return *this;
}

CovenantTxBuilder& CovenantTxBuilder::addOutput(
    uint64_t value, const std::vector<uint8_t>& script_pubkey)
{
    TxOutput vout;
    vout.value = value;
    vout.scriptPubKey = script_pubkey;
    tx_.vout.push_back(vout);
    return *this;
}

Transaction CovenantTxBuilder::build() const {
    return tx_;
}

bool CovenantTxBuilder::validate() const {
    errors_.clear();

    if (tx_.vin.empty()) {
        errors_.push_back("Transaction has no inputs");
    }

    if (tx_.vout.empty()) {
        errors_.push_back("Transaction has no outputs");
    }

    // If CTV template is set, verify outputs match
    if (ctv_template_ && !ctv_template_->outputs.empty()) {
        if (tx_.vout.size() != ctv_template_->outputs.size()) {
            errors_.push_back("Output count doesn't match CTV template");
        } else {
            for (size_t i = 0; i < tx_.vout.size(); i++) {
                if (tx_.vout[i].value != ctv_template_->outputs[i].value) {
                    errors_.push_back("Output " + std::to_string(i) + " value mismatch");
                }
                if (tx_.vout[i].scriptPubKey != ctv_template_->outputs[i].script_pubkey) {
                    errors_.push_back("Output " + std::to_string(i) + " script mismatch");
                }
            }
        }
    }

    return errors_.empty();
}

std::vector<std::string> CovenantTxBuilder::getErrors() const {
    return errors_;
}

} // namespace wallet
} // namespace dinero
