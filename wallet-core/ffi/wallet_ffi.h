// wallet-core/ffi/wallet_ffi.h
// C API wrapper for Dinero wallet functionality
// This provides a C-compatible interface for FFI (Rust, etc.)
//
// Usage:
//   - Rust FFI can call these C functions
//   - All C++ classes are wrapped in C API
//   - Memory management: Caller allocates strings, library frees them

#ifndef WALLET_FFI_H
#define WALLET_FFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>  // For size_t

// ============================================================================
// Error Codes (Unified across C++ → Rust → JS)
// ============================================================================

typedef enum {
    DINERO_SUCCESS = 0,
    DINERO_ERROR_GENERIC = -1,
    DINERO_ERROR_WALLET_NOT_FOUND = -2,
    DINERO_ERROR_WALLET_LOCKED = -3,
    DINERO_ERROR_WALLET_ENCRYPTED = -4,
    DINERO_ERROR_INVALID_MNEMONIC = -5,
    DINERO_ERROR_INVALID_ADDRESS = -6,
    DINERO_ERROR_INSUFFICIENT_FUNDS = -7,
    DINERO_ERROR_INVALID_AMOUNT = -8,
    DINERO_ERROR_TX_BROADCAST_FAILED = -9,
    DINERO_ERROR_FILE_IO = -10,
    DINERO_ERROR_INVALID_FORMAT = -11,
    DINERO_ERROR_NETWORK = -12,
    DINERO_ERROR_AUTHENTICATION = -13,
    DINERO_ERROR_NOT_IMPLEMENTED = -14,
    DINERO_ERROR_NOT_FOUND = -15,
} DineroErrorCode;

// ============================================================================
// Types (FFI_ prefix to avoid conflicts with C++ structs)
// ============================================================================

typedef struct {
    double total;
    double confirmed;
    double unconfirmed;
    double immature;
} FFI_WalletBalance;

typedef struct {
    char* address;
    char* path;
    int32_t index;
    double balance;
} FFI_WalletAddress;

typedef struct {
    char* txid;
    int32_t vout;
    char* address;
    double amount;
    int32_t confirmations;
    bool spendable;
    bool coinbase;
} FFI_WalletUTXO;

// ============================================================================
// Wallet Initialization
// ============================================================================

/**
 * Initialize wallet with data directory
 * @param datadir Path to wallet data directory
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_init(const char* datadir);

/**
 * Create new HD wallet
 * @param mnemonic_out Output mnemonic phrase (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_create(char** mnemonic_out);

/**
 * Restore wallet from mnemonic
 * @param mnemonic BIP-39 mnemonic phrase
 * @param passphrase Optional BIP-39 passphrase (can be NULL or empty)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_restore(const char* mnemonic, const char* passphrase);

// ============================================================================
// Wallet Encryption & Locking
// ============================================================================

/**
 * Encrypt wallet with password
 * @param password Encryption password
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_encrypt(const char* password);

/**
 * Unlock wallet
 * @param password Wallet password
 * @param timeout_seconds Auto-lock timeout (seconds)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_unlock(const char* password, int32_t timeout_seconds);

/**
 * Lock wallet
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_lock();

/**
 * Check if wallet is encrypted
 * @return true if encrypted, false otherwise
 */
bool dinero_wallet_is_encrypted();

/**
 * Check if wallet is locked
 * @return true if locked, false otherwise
 */
bool dinero_wallet_is_locked();

// ============================================================================
// Address Operations
// ============================================================================

/**
 * Get wallet balance
 * @return Wallet balance structure
 */
FFI_WalletBalance dinero_wallet_get_balance();

/**
 * Get new receive address
 * @param label Optional address label (can be NULL)
 * @param address_out Output address (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_new_address(const char* label, char** address_out);

/**
 * Get new change address
 * @param address_out Output address (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_change_address(char** address_out);

/**
 * Get mining address
 * @param address_out Output address (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_mining_address(char** address_out);

/**
 * Set address label
 * @param address Address to label
 * @param label Label text
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_set_label(const char* address, const char* label);

/**
 * Get address label
 * @param address Address to query
 * @param label_out Output label (caller must free with dinero_wallet_free_string, NULL if no label)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_label(const char* address, char** label_out);

// ============================================================================
// Transaction Operations
// ============================================================================

/**
 * Send transaction
 * @param to Recipient address
 * @param amount Amount in DIN
 * @param fee_rate Fee rate (una per vbyte)
 * @param note Optional transaction note (can be NULL)
 * @param txid_out Output transaction ID (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_send_transaction(
    const char* to,
    double amount,
    double fee_rate,
    const char* note,
    char** txid_out
);

/**
 * List UTXOs
 * @param min_confirmations Minimum confirmations
 * @param utxos_out Output UTXO array (caller must free with dinero_wallet_free_utxos)
 * @param count_out Output UTXO count
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_list_utxos(
    int32_t min_confirmations,
    FFI_WalletUTXO** utxos_out,
    int32_t* count_out
);

/**
 * List addresses
 * @param addresses_out Output address array (caller must free with dinero_wallet_free_addresses)
 * @param count_out Output address count
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_list_addresses(
    FFI_WalletAddress** addresses_out,
    int32_t* count_out
);

// ============================================================================
// Intent & SigHash V1 (B3)
// ============================================================================

/**
 * Intent descriptor for wallet-level transaction intent binding
 */
typedef struct {
    uint8_t recipient_hash[32];   // SHA256 of intended recipient address
    double amount;                // Intended amount in DIN
    double max_fee;               // Maximum acceptable fee in DIN
    uint32_t expiry_height;       // Block height after which intent expires
    char purpose_tag[65];         // Human-readable purpose (max 64 chars + null)
} FFI_IntentDescriptor;

/**
 * Send a transaction with intent binding (SigHash V1)
 * Signs using "dinero/sighash/v1" with ext_commitment derived from intent.
 * @param to Recipient address
 * @param amount Amount in DIN
 * @param fee_rate Fee rate in una/vB
 * @param intent Intent descriptor (can be NULL for default/zeros ext_commitment)
 * @param txid_out Output transaction ID (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_send_with_intent(
    const char* to,
    double amount,
    double fee_rate,
    const FFI_IntentDescriptor* intent,
    char** txid_out
);

/**
 * Compute ext_commitment from an intent descriptor
 * Returns the 32-byte TaggedHash("dinero/intent/v1", serialized_intent)
 * @param intent Intent descriptor
 * @param ext_commitment_out Output 32-byte commitment
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_compute_ext_commitment(
    const FFI_IntentDescriptor* intent,
    uint8_t ext_commitment_out[32]
);

// ============================================================================
// RBF & Timelock Operations (B1a+B1b)
// ============================================================================

/**
 * Bump fee on an existing RBF-signaled transaction (BIP125)
 * Creates a replacement transaction with higher fee rate.
 * @param original_txid Transaction ID to replace
 * @param new_fee_rate New fee rate in una/vB (must exceed original + incremental relay fee)
 * @param new_txid_out Output replacement transaction ID (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_bump_fee(
    const char* original_txid,
    double new_fee_rate,
    char** new_txid_out
);

/**
 * Send a timelocked transaction
 * Creates a transaction with absolute and/or relative timelocks.
 * @param to Recipient address
 * @param amount Amount in DIN
 * @param fee_rate Fee rate in una/vB
 * @param absolute_locktime Block height for nLockTime (0 to skip)
 * @param relative_locktime_blocks BIP68 relative lock in blocks (0 to skip)
 * @param txid_out Output transaction ID (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_send_timelocked(
    const char* to,
    double amount,
    double fee_rate,
    uint32_t absolute_locktime,
    uint32_t relative_locktime_blocks,
    char** txid_out
);

/**
 * Check if a timelocked UTXO is mature (spendable)
 * Evaluates BIP68 sequence lock against current chain height.
 * @param txid Transaction ID containing the UTXO
 * @param vout Output index
 * @param current_height Current best block height
 * @param is_mature_out Output: true if the UTXO's relative timelock has expired
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_is_timelock_mature(
    const char* txid,
    uint32_t vout,
    uint32_t current_height,
    bool* is_mature_out
);

// ============================================================================
// Policy Output Operations (B2)
// ============================================================================

/**
 * Policy info returned from UTXO lookup
 */
typedef struct {
    uint8_t policy_id[32];          // Deterministic policy ID
    uint8_t template_type;          // 0=STANDARD, 1=PROTECTED, 2=ESCROW
    uint16_t template_version;      // Template version
    char template_name[32];         // Human-readable name
} FFI_PolicyInfo;

/**
 * Create a policy-committed Taproot output (PROTECTED or ESCROW template).
 * Builds the Taproot tree, stores the policy in DB, and returns the address.
 *
 * @param template_type 1=PROTECTED, 2=ESCROW
 * @param params Serialized template parameters
 * @param params_len Length of params
 * @param amount Amount in DIN to send to the policy output
 * @param fee_rate Fee rate in una/vB
 * @param address_out Output Taproot address (caller must free with dinero_wallet_free_string)
 * @param policy_id_out Output 32-byte policy ID
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_create_policy_output(
    uint8_t template_type,
    const uint8_t* params,
    size_t params_len,
    double amount,
    double fee_rate,
    char** address_out,
    uint8_t policy_id_out[32]
);

/**
 * Get policy info for a UTXO.
 * Looks up the policy_id in the utxo_policy table.
 *
 * @param txid Transaction ID (hex string)
 * @param vout Output index
 * @param info_out Output policy info
 * @return 0 on success, DINERO_ERROR_NOT_FOUND if no policy, non-zero on other error
 */
int dinero_wallet_get_policy_for_utxo(
    const char* txid,
    uint32_t vout,
    FFI_PolicyInfo* info_out
);

// ============================================================================
// Safety Profiles & Protected Spending (B4)
// ============================================================================

/**
 * Create a safety profile with panic and recovery parameters.
 * @param panic_window_blocks CSV delay for panic leaf (e.g., 6 blocks ~1h)
 * @param recovery_delay_blocks CSV delay for recovery leaf (e.g., 25920 blocks ~6mo)
 * @param name Profile name (max 255 chars)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_create_safety_profile(
    uint32_t panic_window_blocks,
    uint32_t recovery_delay_blocks,
    const char* name
);

/**
 * Activate a safety profile by name. Only one profile can be active.
 * @param name Profile name to activate
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_activate_safety_profile(const char* name);

/**
 * Send to a PROTECTED Taproot output using the active safety profile.
 * Creates a key-path(user) + panic leaf + recovery leaf output.
 * @param to Recipient address (for the output value)
 * @param amount Amount in DIN
 * @param fee_rate Fee rate in una/vB
 * @param txid_out Output transaction ID (caller must free)
 * @param policy_id_out Output 32-byte policy ID
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_send_protected(
    const char* to,
    double amount,
    double fee_rate,
    char** txid_out,
    uint8_t policy_id_out[32]
);

/**
 * Panic-cancel a PROTECTED UTXO via the panic leaf script path.
 * Sweeps funds to the specified safe address.
 * @param txid Transaction ID of the protected UTXO
 * @param vout Output index
 * @param safe_address Destination address for swept funds
 * @param cancel_txid_out Output cancel transaction ID (caller must free)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_panic_cancel(
    const char* txid,
    uint32_t vout,
    const char* safe_address,
    char** cancel_txid_out
);

/**
 * Recovery-claim a PROTECTED UTXO via the recovery leaf script path.
 * Only succeeds if the recovery delay CSV has matured.
 * @param txid Transaction ID of the protected UTXO
 * @param vout Output index
 * @param recovery_address Destination address for recovered funds
 * @param recovery_txid_out Output recovery transaction ID (caller must free)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_recovery_claim(
    const char* txid,
    uint32_t vout,
    const char* recovery_address,
    char** recovery_txid_out
);

/**
 * Export wallet recovery data as encrypted QR payload.
 * SECURITY: Disabled in hardened builds because mnemonic is never cached in
 * process memory. Returns DINERO_ERROR_NOT_IMPLEMENTED.
 * @param passphrase Reserved for future caller-supplied mnemonic API.
 * @param qr_data_out Unused in hardened mode.
 * @return Always non-zero in hardened mode.
 */
int dinero_wallet_export_recovery_qr(
    const char* passphrase,
    char** qr_data_out
);

/**
 * Export recovery QR from caller-provided mnemonic (ephemeral input only).
 * SECURITY: No mnemonic is read from process-global state.
 *
 * @param mnemonic_bytes UTF-8 mnemonic bytes (not null-terminated)
 * @param mnemonic_len Length of mnemonic_bytes
 * @param passphrase_bytes Optional UTF-8 passphrase bytes (may be NULL when passphrase_len = 0)
 * @param passphrase_len Length of passphrase_bytes
 * @param profile_data Optional serialized SafetyProfile bytes (may be NULL when profile_data_len = 0)
 * @param profile_data_len Length of profile_data
 * @param qr_data_out Output QR payload string (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on validation/encryption error
 */
int dinero_wallet_export_recovery_qr_from_mnemonic_bytes(
    const uint8_t* mnemonic_bytes,
    size_t mnemonic_len,
    const uint8_t* passphrase_bytes,
    size_t passphrase_len,
    const uint8_t* profile_data,
    size_t profile_data_len,
    char** qr_data_out
);

/**
 * Import wallet from encrypted recovery QR payload.
 * @param qr_data Base64-encoded payload with DINERO-RECOVERY-1 header
 * @param passphrase Decryption passphrase
 * @return 0 on success, non-zero on authentication failure or malformed data
 */
int dinero_wallet_import_recovery_qr(
    const char* qr_data,
    const char* passphrase
);

// ============================================================================
// Escrow Operations (B5)
// ============================================================================

/**
 * Create an escrow output (ESCROW template).
 * Builds the Taproot tree with release + timeout leaves.
 *
 * @param buyer_pubkey 32-byte x-only buyer public key
 * @param seller_pubkey 32-byte x-only seller public key
 * @param attestor_pubkeys Array of 32-byte x-only attestor public keys
 * @param n_attestors Number of attestors
 * @param threshold k-of-(n+1) threshold (seller counts toward threshold)
 * @param timeout_blocks CSV timeout for buyer refund
 * @param amount Amount in DIN
 * @param fee_rate Fee rate in una/vB
 * @param address_out Output Taproot address (caller must free)
 * @param policy_id_out Output 32-byte policy ID
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_create_escrow_output(
    const uint8_t buyer_pubkey[32],
    const uint8_t seller_pubkey[32],
    const uint8_t* attestor_pubkeys,
    uint8_t n_attestors,
    uint8_t threshold,
    uint32_t timeout_blocks,
    double amount,
    double fee_rate,
    char** address_out,
    uint8_t policy_id_out[32]
);

/**
 * Sign and broadcast an escrow release transaction.
 * Uses the release leaf script-path with seller sig + attestor sigs.
 *
 * @param txid Funding transaction ID
 * @param vout Funding output index
 * @param receipt_data Serialized ReceiptBundle
 * @param receipt_len Length of receipt data
 * @param release_txid_out Output release transaction ID (caller must free)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_sign_escrow_release(
    const char* txid,
    uint32_t vout,
    const uint8_t* receipt_data,
    size_t receipt_len,
    char** release_txid_out
);

/**
 * Sign and broadcast an escrow refund transaction (timeout path).
 * Only succeeds if the timeout CSV has matured.
 *
 * @param txid Funding transaction ID
 * @param vout Funding output index
 * @param refund_txid_out Output refund transaction ID (caller must free)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_sign_escrow_refund(
    const char* txid,
    uint32_t vout,
    char** refund_txid_out
);

/**
 * Verify a receipt bundle (check all Schnorr signatures).
 *
 * @param receipt_data Serialized ReceiptBundle
 * @param receipt_len Length of receipt data
 * @param outcome "release" or "refund"
 * @param valid_out Output: true if all signatures verify
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_verify_receipt_bundle(
    const uint8_t* receipt_data,
    size_t receipt_len,
    const char* outcome,
    bool* valid_out
);

// ============================================================================
// TX Classification & Extended Details (B7)
// ============================================================================

/**
 * Extended transaction detail for iOS display
 */
typedef struct {
    uint8_t classification;         // 0=STANDARD, 1=PROTECTED, 2=ESCROW, 255=UNKNOWN
    uint8_t policy_id[32];          // Zero if STANDARD
    char template_name[32];         // "Standard", "Protected", "Escrow", etc.
    char policy_description[128];   // Human-readable description
    int32_t panic_remaining;        // Panic window remaining blocks (-1 = N/A)
    int32_t recovery_remaining;     // Recovery delay remaining blocks (-1 = N/A)
    char escrow_state[32];          // Escrow state name (empty if not escrow)
} FFI_ExtendedTxDetail;

/**
 * Get extended transaction/UTXO detail for display.
 * Classifies the UTXO and computes remaining timelocks.
 *
 * @param txid Transaction ID (hex string)
 * @param vout Output index
 * @param current_height Current chain height
 * @param detail_out Output extended detail
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_tx_detail_extended(
    const char* txid,
    uint32_t vout,
    uint32_t current_height,
    FFI_ExtendedTxDetail* detail_out
);

// ============================================================================
// Backup & Recovery
// ============================================================================

/**
 * Backup wallet to file
 * @param filepath Output file path
 * @param hash_out Output SHA256 hash (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_backup_file(const char* filepath, char** hash_out);

/**
 * Get wallet mnemonic.
 * SECURITY: Disabled in hardened builds; mnemonic readback from process memory
 * is prohibited. Returns DINERO_ERROR_NOT_IMPLEMENTED.
 * @param mnemonic_out Unused in hardened mode.
 * @return Always non-zero in hardened mode.
 */
int dinero_wallet_get_mnemonic(char** mnemonic_out);

// ============================================================================
// Network Operations (Optional - for blockchain sync)
// ============================================================================

/**
 * Connect to Dinero RPC node
 * @param rpc_url RPC node URL (e.g., "https://rpc.dinero-coin.com")
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_connect_rpc(const char* rpc_url);

/**
 * Sync balance from network
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_sync_balance();

/**
 * Broadcast transaction to network
 * @param tx_hex Signed transaction hex
 * @param txid_out Output transaction ID (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_broadcast_tx(const char* tx_hex, char** txid_out);

// ============================================================================
// Payment UX Features
// ============================================================================

/**
 * QR Payment structure (parsed from dinero: URI)
 */
typedef struct {
    char address[128];
    double amount;
    char label[128];
} FFI_QRPayment;

// ABI v2: supports the ~132-character shielded Bech32m address plus NUL.
// Legacy structs/functions remain exported for binary compatibility.
#define DINERO_FFI_ADDRESS_V2_CAPACITY 192
typedef struct {
    char address[DINERO_FFI_ADDRESS_V2_CAPACITY];
    double amount;
    char label[128];
} FFI_QRPaymentV2;

/**
 * Export transaction history to CSV or JSON format
 * @param format "csv" or "json"
 * @param dest Path where file should be written
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_export_transactions(const char* format, const char* dest);

/**
 * Export transaction history with async batching (for large datasets)
 * @param format "csv" or "json"
 * @param dest Path where file should be written
 * @param batch_size Number of transactions per batch (0 = auto)
 * @param callback Progress callback (txid, progress 0.0-1.0, can be NULL)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_export_transactions_batched(
    const char* format,
    const char* dest,
    int32_t batch_size,
    void (*callback)(const char* txid, double progress)
);

/**
 * Get transaction confirmation count
 * @param txid Transaction ID
 * @param confirmations_out Output confirmation count
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_tx_confirmations(const char* txid, int32_t* confirmations_out);

/**
 * Parse Dinero payment URI (dinero:address?amount=X&label=Y)
 * @param uri Payment URI string
 * @param out Output QR payment structure
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_parse_uri(const char* uri, FFI_QRPayment* out);
int dinero_wallet_parse_uri_v2(const char* uri, FFI_QRPaymentV2* out);

/**
 * Generate Dinero payment URI for QR code
 * @param address Recipient address
 * @param amount Optional amount (0.0 to omit)
 * @param label Optional label (can be NULL)
 * @param uri_out Output URI string (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_generate_uri(
    const char* address,
    double amount,
    const char* label,
    char** uri_out
);

/**
 * Transaction notification structure
 */
typedef struct {
    char txid[65];
    char address[128];
    double amount;
    int32_t confirmations;
    int64_t timestamp;
    char category[32];  // "send", "receive", "generate"
    bool is_new;        // true if this is a new transaction since last check
} FFI_TransactionNotification;

typedef struct {
    char txid[65];
    char address[DINERO_FFI_ADDRESS_V2_CAPACITY];
    double amount;
    int32_t confirmations;
    int64_t timestamp;
    char category[32];
    bool is_new;
} FFI_TransactionNotificationV2;

/**
 * Check for new transactions since last call
 * @param notifications_out Output array of notifications (caller must free with dinero_wallet_free_notifications)
 * @param count_out Output notification count
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_check_new_transactions(
    FFI_TransactionNotification** notifications_out,
    int32_t* count_out
);
int dinero_wallet_check_new_transactions_v2(
    FFI_TransactionNotificationV2** notifications_out,
    int32_t* count_out
);
void dinero_wallet_free_notifications_v2(FFI_TransactionNotificationV2* ptr,
                                         int32_t count);

// ============================================================================
// Memory Management
// ============================================================================

/**
 * Free string allocated by wallet library
 * @param ptr String pointer to free
 */
void dinero_wallet_free_string(char* ptr);

/**
 * Free address array allocated by wallet library
 * @param ptr Address array pointer
 * @param count Number of addresses
 */
void dinero_wallet_free_addresses(FFI_WalletAddress* ptr, int32_t count);

/**
 * Free UTXO array allocated by wallet library
 * @param ptr UTXO array pointer
 * @param count Number of UTXOs
 */
void dinero_wallet_free_utxos(FFI_WalletUTXO* ptr, int32_t count);

/**
 * Free transaction notification array allocated by wallet library
 * @param ptr Notification array pointer
 * @param count Number of notifications
 */
void dinero_wallet_free_notifications(FFI_TransactionNotification* ptr, int32_t count);

// ============================================================================
// Performance & Diagnostics
// ============================================================================

/**
 * Wallet sync progress structure
 */
typedef struct {
    double progress;        // 0.0 to 1.0
    int32_t current_block;
    int32_t total_blocks;
    bool is_syncing;
    char* status_message;  // "Syncing...", "Complete", etc. (caller must free with dinero_wallet_free_string)
} FFI_SyncProgress;

/**
 * Get wallet synchronization progress
 * @param progress_out Output sync progress structure
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_sync_progress(FFI_SyncProgress* progress_out);

/**
 * Get last error code from wallet operations
 * @return DineroErrorCode
 */
DineroErrorCode dinero_wallet_get_last_error(void);

/**
 * Get error message for error code
 * @param error_code Error code
 * @param message_out Output error message (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_error_message(DineroErrorCode error_code, char** message_out);

// ============================================================================
// Security & Key Management
// ============================================================================

/**
 * Store encrypted wallet data in platform secure storage (Keychain/Keystore)
 * @param wallet_data Encrypted wallet data (JSON string)
 * @param data_length Length of wallet_data
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_store_secure(const char* wallet_data, size_t data_length);

/**
 * Retrieve encrypted wallet data from platform secure storage
 * @param wallet_data_out Output wallet data (caller must free with dinero_wallet_free_string)
 * @param data_length_out Output data length
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_retrieve_secure(char** wallet_data_out, size_t* data_length_out);

/**
 * Check if platform secure storage is available
 * @return true if available, false otherwise
 */
bool dinero_wallet_secure_storage_available(void);

// ============================================================================
// Exchange & Swap Features
// ============================================================================

/**
 * Exchange rate structure
 */
typedef struct {
    char from_symbol[8];
    char to_symbol[8];
    double rate;           // 1 from_symbol = rate to_symbol
    double to_amount;     // Calculated amount after conversion
    double min_amount;     // Minimum swap amount
    double max_amount;     // Maximum swap amount
    int64_t timestamp;     // Rate timestamp
    char provider[32];     // Exchange provider name
} FFI_ExchangeRate;

/**
 * Get exchange rate between two currencies
 * @param from Source currency symbol (e.g., "DIN", "BTC", "USD")
 * @param to Target currency symbol
 * @param amount Amount to convert (for rate calculation)
 * @param rate_out Output exchange rate structure
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_exchange_rate(
    const char* from,
    const char* to,
    double amount,
    FFI_ExchangeRate* rate_out
);

/**
 * Swap transaction structure
 */
typedef struct {
    char txid[65];         // Transaction ID (after execution)
    char from_address[128];
    char to_address[128];
    double from_amount;
    double to_amount;
    char from_symbol[8];
    char to_symbol[8];
    double fee;            // Exchange fee
    char status[32];       // "pending", "processing", "completed", "failed"
    int64_t timestamp;
} FFI_SwapTransaction;

typedef struct {
    char txid[65];
    char from_address[DINERO_FFI_ADDRESS_V2_CAPACITY];
    char to_address[DINERO_FFI_ADDRESS_V2_CAPACITY];
    double from_amount;
    double to_amount;
    char from_symbol[8];
    char to_symbol[8];
    double fee;
    char status[32];
    int64_t timestamp;
} FFI_SwapTransactionV2;

/**
 * Create swap transaction
 * @param from_address Source address (Dinero address)
 * @param to_address Target address (external address)
 * @param amount Amount to swap
 * @param from_symbol Source symbol (e.g., "DIN")
 * @param to_symbol Target symbol (e.g., "BTC", "ETH", "USDT")
 * @param swap_out Output swap transaction structure
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_create_swap_tx(
    const char* from_address,
    const char* to_address,
    double amount,
    const char* from_symbol,
    const char* to_symbol,
    FFI_SwapTransaction* swap_out
);
int dinero_wallet_create_swap_tx_v2(
    const char* from_address,
    const char* to_address,
    double amount,
    const char* from_symbol,
    const char* to_symbol,
    FFI_SwapTransactionV2* swap_out
);

/**
 * Get swap transaction status
 * @param swap_id Swap transaction ID or TXID
 * @param swap_out Output swap transaction structure
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_swap_status(
    const char* swap_id,
    FFI_SwapTransaction* swap_out
);
int dinero_wallet_get_swap_status_v2(
    const char* swap_id,
    FFI_SwapTransactionV2* swap_out
);

// ============================================================================
// Liquidity & On-Ramp Features
// ============================================================================

/**
 * Liquidity pool information
 */
typedef struct {
    char pool_id[64];
    char symbol[8];
    double total_liquidity;
    double available_liquidity;
    double apy;              // Annual percentage yield
    double min_deposit;
    double max_deposit;
    int64_t last_update;
} FFI_LiquidityPool;

/**
 * Get available liquidity pools
 * @param pools_out Output array of pools (caller must free with dinero_wallet_free_pools)
 * @param count_out Output pool count
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_liquidity_pools(
    FFI_LiquidityPool** pools_out,
    int32_t* count_out
);

/**
 * Add liquidity to pool
 * @param pool_id Pool identifier
 * @param amount Amount to deposit
 * @param txid_out Output transaction ID (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_add_liquidity(
    const char* pool_id,
    double amount,
    char** txid_out
);

/**
 * Remove liquidity from pool
 * @param pool_id Pool identifier
 * @param amount Amount to withdraw
 * @param txid_out Output transaction ID (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_remove_liquidity(
    const char* pool_id,
    double amount,
    char** txid_out
);

/**
 * Fiat on-ramp order structure
 */
typedef struct {
    char order_id[64];
    char payment_method[32];  // "card", "bank", "sepa", "wire"
    double fiat_amount;
    char fiat_currency[8];    // "USD", "EUR", "GBP"
    double crypto_amount;
    char crypto_symbol[8];    // "DIN"
    double exchange_rate;
    double fee;
    char status[32];          // "pending", "processing", "completed", "failed"
    char payment_url[256];    // URL for payment completion
    int64_t expires_at;
    int64_t created_at;
} FFI_FiatOrder;

/**
 * Create fiat on-ramp order (buy crypto with fiat)
 * @param amount Fiat amount to spend
 * @param fiat_currency Fiat currency symbol (e.g., "USD")
 * @param crypto_symbol Crypto symbol to buy (e.g., "DIN")
 * @param payment_method Payment method ("card", "bank", etc.)
 * @param order_out Output order structure
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_create_fiat_order(
    double amount,
    const char* fiat_currency,
    const char* crypto_symbol,
    const char* payment_method,
    FFI_FiatOrder* order_out
);

/**
 * Get fiat order status
 * @param order_id Order ID
 * @param order_out Output order structure
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_fiat_order_status(
    const char* order_id,
    FFI_FiatOrder* order_out
);

/**
 * KYC verification status
 */
typedef struct {
    bool is_verified;
    char verification_level[32];  // "none", "basic", "advanced", "institutional"
    char provider[32];             // KYC provider name
    int64_t verified_at;
    int64_t expires_at;
    char country[3];               // ISO country code
} FFI_KYCStatus;

/**
 * Get KYC verification status
 * @param status_out Output KYC status structure
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_kyc_status(FFI_KYCStatus* status_out);

/**
 * Initiate KYC verification
 * @param level Verification level ("basic", "advanced")
 * @param country Country code (ISO 3166-1 alpha-2)
 * @param verification_url_out Output verification URL (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_start_kyc_verification(
    const char* level,
    const char* country,
    char** verification_url_out
);

/**
 * Free liquidity pool array
 * @param ptr Pool array pointer
 * @param count Number of pools
 */
void dinero_wallet_free_pools(FFI_LiquidityPool* ptr, int32_t count);

// ============================================================================
// B6a: LP Router — Provider Quote Aggregation
// ============================================================================

/**
 * Provider quote for on/off-ramp
 */
typedef struct {
    char provider_id[64];
    char provider_name[64];
    double rate;                // Fiat per 1 DPI
    uint64_t min_amount_una;   // Minimum DPI in una
    uint64_t max_amount_una;   // Maximum DPI in una
    double fee_percent;         // Provider fee %
    double fee_fixed_fiat;      // Fixed fiat fee
    uint32_t estimated_seconds; // Estimated completion time
    char payment_method[32];    // e.g., "bank_transfer", "card", "pix"
    bool requires_kyc;
} FFI_ProviderQuote;

/**
 * Get aggregated quotes from all eligible providers.
 *
 * @param direction       0 = BUY (fiat->DPI), 1 = SELL (DPI->fiat)
 * @param currency        Fiat currency code (e.g., "USD", "EUR", "MXN")
 * @param amount_una     DPI amount in una (1 DPI = 1e8 una)
 * @param country_code    ISO 3166-1 alpha-2 country code
 * @param quotes_out      Output array of quotes (caller must free with dinero_wallet_free_quotes)
 * @param count_out       Output quote count
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_router_quotes(
    uint8_t direction,
    const char* currency,
    uint64_t amount_una,
    const char* country_code,
    FFI_ProviderQuote** quotes_out,
    int32_t* count_out
);

/**
 * Select the best provider for the given request.
 *
 * @param direction       0 = BUY, 1 = SELL
 * @param currency        Fiat currency code
 * @param amount_una     DPI amount in una
 * @param country_code    ISO country code
 * @param quote_out       Output best quote
 * @return 0 on success, DINERO_ERROR_NOT_FOUND if no provider available
 */
int dinero_wallet_select_provider(
    uint8_t direction,
    const char* currency,
    uint64_t amount_una,
    const char* country_code,
    FFI_ProviderQuote* quote_out
);

/**
 * Free provider quote array
 * @param ptr Quote array pointer
 * @param count Number of quotes
 */
void dinero_wallet_free_quotes(FFI_ProviderQuote* ptr, int32_t count);

// ============================================================================
// KYC Provider Configuration
// ============================================================================

/**
 * Initialize KYC provider
 * @param provider_type Provider type ("mock", "openkyc", "sumsub", "onfido")
 * @param config Configuration string (JSON or key=value format)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_init_kyc_provider(
    const char* provider_type,
    const char* config
);

/**
 * Get current KYC provider name
 * @param name_out Output provider name (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_kyc_provider_name(char** name_out);

#ifdef __cplusplus
}
#endif

#endif // WALLET_FFI_H
