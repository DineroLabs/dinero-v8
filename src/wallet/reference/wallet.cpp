#include "wallet.h"
#include "database.h"
#include "crypto.h"
#include "utxo_manager.h"
#include "transaction_builder.h"
#include "blockchain_sync.h"
#include <stdexcept>
#include <ctime>
#include <sstream>

namespace dinero {
namespace wallet {
namespace reference {

// Factory methods
std::unique_ptr<ReferenceWallet> ReferenceWallet::CreateFromMnemonic(
    const std::string& wallet_name,
    const std::string& mnemonic,
    const std::string& passphrase,
    const std::string& data_dir
) {
    // Validate mnemonic
    if (!crypto::BIP39::ValidateMnemonic(mnemonic)) {
        throw std::invalid_argument("Invalid BIP39 mnemonic");
    }

    // Create wallet instance
    auto wallet = std::unique_ptr<ReferenceWallet>(new ReferenceWallet(wallet_name, data_dir));

    // Initialize database
    wallet->InitializeDatabase();

    // Derive keys from mnemonic
    wallet->DeriveKeysFromSeed(mnemonic, passphrase);

    // Store metadata
    wallet->database_->SetMetadata("name", wallet_name);
    wallet->database_->SetMetadata("address", wallet->address_);
    wallet->database_->SetMetadata("creation_time", std::to_string(std::time(nullptr)));
    wallet->database_->SetMetadata("mnemonic_hash", ""); // Don't store mnemonic

    return wallet;
}

std::string ReferenceWallet::GenerateMnemonic(int word_count) {
    return crypto::BIP39::GenerateMnemonic(word_count);
}

std::unique_ptr<ReferenceWallet> ReferenceWallet::Load(
    const std::string& wallet_name,
    const std::string& data_dir
) {
    auto wallet = std::unique_ptr<ReferenceWallet>(new ReferenceWallet(wallet_name, data_dir));

    // Check if wallet exists
    std::string stored_name = wallet->database_->GetMetadata("name", "");
    if (stored_name.empty()) {
        throw std::runtime_error("Wallet not found: " + wallet_name);
    }

    // Load address
    wallet->address_ = wallet->database_->GetMetadata("address", "");
    if (wallet->address_.empty()) {
        throw std::runtime_error("Wallet address not found");
    }

    // Note: Private key is not stored in database for security
    // User must provide mnemonic to unlock wallet for sending

    return wallet;
}

// Constructor / Destructor
ReferenceWallet::ReferenceWallet(const std::string& wallet_name, const std::string& data_dir)
    : wallet_name_(wallet_name)
    , data_dir_(data_dir) {

    // Create database
    std::string db_path = data_dir + "/" + wallet_name + ".db";
    database_ = std::make_unique<Database>(db_path);

    // Create UTXO manager
    utxo_manager_ = std::make_unique<UTXOManager>(database_.get());
}

ReferenceWallet::~ReferenceWallet() {
    // Clear sensitive data
    std::fill(private_key_.begin(), private_key_.end(), 0);
}

void ReferenceWallet::InitializeDatabase() {
    database_->InitializeSchema();
}

void ReferenceWallet::DeriveKeysFromSeed(const std::string& mnemonic, const std::string& passphrase) {
    // Convert mnemonic to seed
    auto seed = crypto::BIP39::MnemonicToSeed(mnemonic, passphrase);

    // Derive master key
    auto master = crypto::BIP32::MasterKeyFromSeed(seed);

    // Derive BIP84 path: m/84'/1448'/0'/0/0
    // m/84' - BIP84 (native SegWit)
    // m/84'/1448' - Dinero coin type
    // m/84'/1448'/0' - Account 0
    // m/84'/1448'/0'/0 - External chain
    // m/84'/1448'/0'/0/0 - First address

    auto purpose = crypto::BIP32::DeriveChild(master, 0x80000000 + 84); // 84'
    auto coin_type = crypto::BIP32::DeriveChild(purpose, 0x80000000 + 1448); // 1448'
    auto account = crypto::BIP32::DeriveChild(coin_type, 0x80000000 + 0); // 0'
    auto external = crypto::BIP32::DeriveChild(account, 0); // 0 (external)
    auto address_key = crypto::BIP32::DeriveChild(external, 0); // 0 (first address)

    // Store keys
    private_key_ = address_key.private_key;
    public_key_ = address_key.public_key;

    // Derive address
    address_ = crypto::Address::PublicKeyToAddress(public_key_);

    // Create transaction builder
    tx_builder_ = std::make_unique<TransactionBuilder>(private_key_, public_key_, address_);
}

// Public API
std::string ReferenceWallet::GetAddress() const {
    return address_;
}

Balance ReferenceWallet::GetBalance(uint32_t min_confirmations) const {
    uint32_t current_height = std::stoul(database_->GetMetadata("last_block_height", "0"));
    return utxo_manager_->CalculateBalance(min_confirmations, current_height);
}

std::vector<UTXO> ReferenceWallet::ListUnspent(uint32_t min_confirmations) const {
    uint32_t current_height = std::stoul(database_->GetMetadata("last_block_height", "0"));
    return utxo_manager_->GetUnspentUTXOs(min_confirmations, current_height);
}

Transaction ReferenceWallet::SendToAddress(
    const std::string& to_address,
    uint64_t amount,
    uint64_t fee
) {
    if (!tx_builder_) {
        throw std::runtime_error("Wallet not unlocked. Load with mnemonic first.");
    }

    // Validate address
    if (!crypto::Address::Validate(to_address, "din")) {
        throw std::invalid_argument("Invalid recipient address");
    }

    // Get current height
    uint32_t current_height = std::stoul(database_->GetMetadata("last_block_height", "0"));

    // Select UTXOs
    auto selected_utxos = utxo_manager_->SelectUTXOs(amount, fee, 1, current_height);

    // Build and sign transaction
    auto build_result = tx_builder_->BuildTransaction(selected_utxos, to_address, amount, fee);

    // Store transaction
    Database::TransactionRow tx_row;
    tx_row.txid = build_result.txid;
    tx_row.hex = build_result.hex;
    tx_row.amount_sent = amount;
    tx_row.fee = fee;
    tx_row.to_address = to_address;
    tx_row.height = 0; // Unconfirmed
    tx_row.timestamp = std::time(nullptr);

    database_->InsertTransaction(tx_row);

    // Mark UTXOs as spent
    for (const auto& utxo : selected_utxos) {
        utxo_manager_->RemoveUTXO(utxo.txid, utxo.vout, build_result.txid, 0);
    }

    // Return transaction
    Transaction tx;
    tx.txid = build_result.txid;
    tx.hex = build_result.hex;
    tx.amount_sent = amount;
    tx.fee = fee;
    tx.to_address = to_address;
    tx.height = 0;
    tx.timestamp = tx_row.timestamp;

    return tx;
}

Transaction ReferenceWallet::GetTransaction(const std::string& txid) const {
    auto tx_row = database_->GetTransaction(txid);

    Transaction tx;
    tx.txid = tx_row.txid;
    tx.hex = tx_row.hex;
    tx.amount_sent = tx_row.amount_sent;
    tx.fee = tx_row.fee;
    tx.to_address = tx_row.to_address;
    tx.height = tx_row.height;
    tx.timestamp = tx_row.timestamp;

    return tx;
}

std::vector<Transaction> ReferenceWallet::ListTransactions(uint32_t limit, uint32_t offset) const {
    auto tx_rows = database_->GetTransactions(limit, offset);

    std::vector<Transaction> transactions;
    for (const auto& row : tx_rows) {
        Transaction tx;
        tx.txid = row.txid;
        tx.hex = row.hex;
        tx.amount_sent = row.amount_sent;
        tx.fee = row.fee;
        tx.to_address = row.to_address;
        tx.height = row.height;
        tx.timestamp = row.timestamp;
        transactions.push_back(tx);
    }

    return transactions;
}

void ReferenceWallet::ProcessBlock(
    uint32_t block_height,
    const std::string& block_hash,
    const std::vector<std::string>& transactions
) {
    // Update metadata
    database_->SetMetadata("last_block_height", std::to_string(block_height));
    database_->SetMetadata("last_block_hash", block_hash);

    // TODO: Parse transactions and update UTXO set
    // This would require a transaction parser which is beyond the scope of this initial implementation
    // For now, this is a placeholder that would be implemented in Phase 2
}

void ReferenceWallet::ProcessMempoolTransaction(const std::string& tx_hex) {
    // TODO: Parse mempool transaction and add relevant UTXOs
    // Placeholder for Phase 2
}

ReferenceWallet::WalletInfo ReferenceWallet::GetWalletInfo() const {
    WalletInfo info;
    info.name = database_->GetMetadata("name", wallet_name_);
    info.address = address_;
    info.creation_time = std::stol(database_->GetMetadata("creation_time", "0"));
    info.last_block_height = std::stoul(database_->GetMetadata("last_block_height", "0"));
    info.last_block_hash = database_->GetMetadata("last_block_hash", "");

    return info;
}

std::string ReferenceWallet::DumpPrivateKey() const {
    if (private_key_.empty()) {
        throw std::runtime_error("Wallet not unlocked");
    }

    return crypto::WIF::Encode(private_key_, true, 0x80);
}

bool ReferenceWallet::ValidateAddress(const std::string& address) {
    return crypto::Address::Validate(address, "din");
}

// ===== Manual UTXO Injection (for testing) =====

void ReferenceWallet::AddUTXO(const UTXO& utxo) {
    utxo_manager_->AddUTXO(utxo);
}

void ReferenceWallet::RemoveUTXO(
    const std::string& txid,
    uint32_t vout,
    const std::string& spent_in_txid,
    uint32_t spent_at_height
) {
    utxo_manager_->RemoveUTXO(txid, vout, spent_in_txid, spent_at_height);
}

std::vector<UTXO> ReferenceWallet::SelectUTXOsForTransaction(
    uint64_t amount,
    uint64_t fee,
    uint32_t current_height
) const {
    return utxo_manager_->SelectUTXOs(amount, fee, 1, current_height);
}

void ReferenceWallet::SetCurrentHeight(uint32_t height) {
    database_->SetMetadata("last_block_height", std::to_string(height));
}

// ===== Blockchain Synchronization =====

bool ReferenceWallet::SyncBlockchain(
    const std::string& rpc_url,
    const std::string& rpc_user,
    const std::string& rpc_password,
    uint32_t start_height,
    uint32_t max_blocks
) {
    // Create blockchain sync configuration
    BlockchainSyncConfig config;
    config.rpc_url = rpc_url;
    config.rpc_user = rpc_user;
    config.rpc_password = rpc_password;
    config.start_height = start_height;
    config.verbose = true;  // Enable verbose output for CLI usage

    // Create sync instance
    BlockchainSync sync(this, config);

    // Initialize connection
    if (!sync.Initialize()) {
        throw std::runtime_error("Failed to initialize blockchain sync: " + sync.GetLastError());
    }

    // Perform sync
    bool success = sync.Sync(start_height, max_blocks);

    if (!success) {
        throw std::runtime_error("Blockchain sync failed: " + sync.GetLastError());
    }

    return success;
}

} // namespace reference
} // namespace wallet
} // namespace dinero
