// nodecore_ffi.h
// C ABI for embedding the Dinero node in iOS (NodeCore.xcframework)
//
// Wraps DaemonApp lifecycle + status + wallet watch + event callbacks.
// Designed for Swift interop via direct C function calls.
//
// Thread safety:
//   - All functions are thread-safe (internally synchronized)
//   - Event callback fires on an internal thread — dispatch to main queue in Swift
//   - node_rpc_call() may block; call from background queue
//
// Memory:
//   - Strings returned by nodecore_ functions must be freed with nodecore_free_string()
//   - Event callback strings are valid only for the duration of the callback

#ifndef NODECORE_FFI_H
#define NODECORE_FFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Error Codes
// ============================================================================

typedef enum {
    NODECORE_OK = 0,
    NODECORE_ERROR_ALREADY_RUNNING = -1,
    NODECORE_ERROR_NOT_RUNNING = -2,
    NODECORE_ERROR_INIT_FAILED = -3,
    NODECORE_ERROR_START_FAILED = -4,
    NODECORE_ERROR_INVALID_ARGS = -5,
    NODECORE_ERROR_RPC_FAILED = -6,
    NODECORE_ERROR_INTERNAL = -7,
} NodeCoreError;

// ============================================================================
// Event Types
// ============================================================================

typedef enum {
    // Sync progress
    NODECORE_EVENT_SYNC_STARTED = 1,
    NODECORE_EVENT_SYNC_HEADERS_PROGRESS = 2,
    NODECORE_EVENT_SYNC_BLOCK_CONNECTED = 3,
    NODECORE_EVENT_SYNC_COMPLETE = 4,

    // Chain state
    NODECORE_EVENT_BLOCK_TIP_UPDATED = 10,
    NODECORE_EVENT_REORG = 11,

    // P2P
    NODECORE_EVENT_PEER_CONNECTED = 20,
    NODECORE_EVENT_PEER_DISCONNECTED = 21,
    NODECORE_EVENT_PEER_COUNT_CHANGED = 22,

    // Wallet-relevant (Option A: node produces relevant events)
    NODECORE_EVENT_RELEVANT_TX_ADDED = 30,       // tx matching watched script found
    NODECORE_EVENT_RELEVANT_TX_REMOVED = 31,     // reorg removed a relevant tx
    NODECORE_EVENT_RELEVANT_TX_CONFIRMED = 32,   // watched tx got confirmed
    NODECORE_EVENT_RELEVANT_UTXO_SPENT = 33,     // watched UTXO was spent

    // System
    NODECORE_EVENT_WARN_LOW_DISK = 40,
    NODECORE_EVENT_WARN_LOW_MEMORY = 41,
    NODECORE_EVENT_SHUTDOWN = 50,
} NodeCoreEventType;

// ============================================================================
// Event Callback
// ============================================================================

/**
 * Event callback signature.
 *
 * @param event_type One of NodeCoreEventType values
 * @param json_data  JSON string with event-specific data (valid only during callback)
 * @param user_data  Opaque pointer passed to nodecore_set_event_callback()
 *
 * JSON schemas per event:
 *
 *   SYNC_HEADERS_PROGRESS:
 *     {"height": 12345, "total": 50000, "progress": 0.247}
 *
 *   SYNC_BLOCK_CONNECTED:
 *     {"height": 12345, "hash": "0a1b2c...", "tx_count": 3, "timestamp": 1700000000}
 *
 *   BLOCK_TIP_UPDATED:
 *     {"height": 12345, "hash": "0a1b2c...", "timestamp": 1700000000}
 *
 *   REORG:
 *     {"old_tip": "0a1b2c...", "new_tip": "3d4e5f...", "depth": 2}
 *
 *   PEER_COUNT_CHANGED:
 *     {"inbound": 0, "outbound": 4, "total": 4}
 *
 *   RELEVANT_TX_ADDED:
 *     {"txid": "ab12...", "height": 12345, "outputs": [
 *       {"index": 0, "script": "5120...", "amount": 5000000000, "address": "din1..."}
 *     ]}
 *
 *   RELEVANT_TX_REMOVED:
 *     {"txid": "ab12...", "height": 12345}
 *
 *   RELEVANT_UTXO_SPENT:
 *     {"txid": "ab12...", "vout": 0, "spent_by": "cd34...", "height": 12346}
 */
typedef void (*nodecore_event_callback_t)(
    int32_t event_type,
    const char* json_data,
    void* user_data
);

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * Initialize and start the embedded node.
 *
 * Creates RocksDB in datadir, connects to P2P network, begins sync.
 * This is an async operation — returns immediately. Monitor progress
 * via the event callback.
 *
 * @param datadir     Absolute path to node data directory
 *                    (e.g., ".../Application Support/Dinero/node")
 * @param config_json JSON string with optional configuration:
 *                    {
 *                      "network": "mainnet"|"testnet"|"regtest",
 *                      "p2p_port": 21001,
 *                      "max_peers": 4,
 *                      "prune": 0,
 *                      "sync_profile": "ios_utreexo"|"mac_fullblock",
 *                      "utreexo_bridge": false,
 *                      "utreexo_stateless": false // optional legacy override
 *                    }
 *                    Profile defaults when omitted:
 *                      iOS/iPadOS = "ios_utreexo"
 *                      macOS/other = "mac_fullblock"
 *                    Conflict rule:
 *                      sync_profile takes precedence over utreexo_stateless.
 *                    Pass NULL for defaults.
 * @return NODECORE_OK on success, error code on failure
 */
int32_t nodecore_start(const char* datadir, const char* config_json);

/**
 * Gracefully stop the node.
 *
 * Flushes databases, disconnects peers, stops all services.
 * Blocks until shutdown is complete (with 10s timeout).
 *
 * Safe to call from any thread. Safe to call if not running (returns OK).
 *
 * @return NODECORE_OK on success
 */
int32_t nodecore_stop(void);

/**
 * Check if the node is currently running.
 */
bool nodecore_is_running(void);

// ============================================================================
// Status
// ============================================================================

/**
 * Get current node status as JSON.
 *
 * Returns a JSON string that the caller must free with nodecore_free_string().
 *
 * Schema:
 * {
 *   "running": true,
 *   "height": 12345,
 *   "headers": 50000,
 *   "best_hash": "0a1b2c...",
 *   "syncing": true,
 *   "sync_progress": 0.247,
 *   "peers": {"inbound": 0, "outbound": 4, "total": 4},
 *   "utreexo": {"leaves": 180, "roots": 3, "commitment": "ab12..."},
 *   "disk_usage_bytes": 52428800,
 *   "mempool_size": 15,
 *   "uptime_seconds": 3600,
 *   "network": "mainnet",
 *   "sync_profile": "ios_utreexo",
 *   "capabilities": 5,
 *   "version": "0.15.0"
 * }
 *
 * @return JSON string (caller must free), or NULL if node not running
 */
char* nodecore_get_status_json(void);

/**
 * Get current validated block height.
 * Returns 0 if node is not running or not synced.
 */
uint64_t nodecore_get_height(void);

/**
 * Get best block hash as hex string.
 * @return Hex string (caller must free), or NULL if not running
 */
char* nodecore_get_best_hash(void);

/**
 * Check if initial block download (IBD) is complete.
 */
bool nodecore_is_synced(void);

// ============================================================================
// Capabilities
// ============================================================================

#define NODECORE_CAP_SYNC_STATELESS (1ULL << 0)
#define NODECORE_CAP_SYNC_FULLBLOCK (1ULL << 1)
#define NODECORE_CAP_MINING_LOCAL   (1ULL << 2)
#define NODECORE_CAP_MINING_POOL    (1ULL << 3)

/**
 * Get runtime capabilities as a bitmask of NODECORE_CAP_* flags.
 *
 * Capabilities are derived from the resolved sync_profile:
 *   ios_utreexo  -> NODECORE_CAP_SYNC_STATELESS
 *   mac_fullblock -> NODECORE_CAP_SYNC_FULLBLOCK | NODECORE_CAP_MINING_LOCAL |
 *                    NODECORE_CAP_MINING_POOL
 */
uint64_t nodecore_capabilities(void);

// ============================================================================
// Event Stream
// ============================================================================

/**
 * Register event callback.
 *
 * Only one callback can be active. Setting a new one replaces the previous.
 * Pass NULL to unregister.
 *
 * The callback fires on an internal thread. In Swift, dispatch to main:
 *   nodecore_set_event_callback({ type, json, ctx in
 *       DispatchQueue.main.async { ... }
 *   }, nil)
 *
 * @param callback  Function pointer (or NULL to unregister)
 * @param user_data Opaque pointer passed to every callback invocation
 */
void nodecore_set_event_callback(nodecore_event_callback_t callback, void* user_data);

// ============================================================================
// Wallet Watch (Option A: node produces relevant events)
// ============================================================================

/**
 * Register a scriptPubKey to watch.
 *
 * The node will emit RELEVANT_TX_ADDED events when transactions matching
 * this script are found (in blocks or mempool).
 *
 * @param script_pubkey  Raw scriptPubKey bytes (e.g., P2TR: 0x5120 + 32-byte key)
 * @param script_len     Length of scriptPubKey
 * @param label          Human-readable label (for logging, can be NULL)
 * @return NODECORE_OK on success
 */
int32_t nodecore_watch_script(
    const uint8_t* script_pubkey,
    size_t script_len,
    const char* label
);

/**
 * Unregister a watched scriptPubKey.
 *
 * @param script_pubkey  Raw scriptPubKey bytes
 * @param script_len     Length of scriptPubKey
 * @return NODECORE_OK on success
 */
int32_t nodecore_unwatch_script(
    const uint8_t* script_pubkey,
    size_t script_len
);

/**
 * Register multiple scripts at once (batch import at startup).
 *
 * More efficient than calling nodecore_watch_script() in a loop.
 *
 * @param scripts    Array of scriptPubKey byte arrays (contiguous)
 * @param lengths    Array of lengths for each script
 * @param count      Number of scripts
 * @return NODECORE_OK on success
 */
int32_t nodecore_watch_scripts_batch(
    const uint8_t* const* scripts,
    const size_t* lengths,
    size_t count
);

/**
 * Clear all watched scripts.
 */
int32_t nodecore_clear_watches(void);

/**
 * Rescan blockchain from a given height for watched scripts.
 *
 * Useful after importing new keys/addresses. Emits RELEVANT_TX_ADDED
 * events for any matches found.
 *
 * @param from_height  Start height for rescan (0 = genesis)
 * @return NODECORE_OK on success
 */
int32_t nodecore_rescan(uint64_t from_height);

// ============================================================================
// Query: UTXOs for Watched Scripts
// ============================================================================

/**
 * Get UTXOs matching watched scripts as JSON.
 *
 * Schema:
 * [
 *   {
 *     "txid": "ab12...",
 *     "vout": 0,
 *     "amount": 5000000000,
 *     "script": "5120...",
 *     "height": 12345,
 *     "confirmations": 100,
 *     "coinbase": false
 *   }
 * ]
 *
 * @param min_confirmations  Minimum confirmation count (1 = confirmed)
 * @return JSON array string (caller must free), or NULL on error
 */
char* nodecore_list_unspent_json(int32_t min_confirmations);

// ============================================================================
// Transaction
// ============================================================================

/**
 * Broadcast a signed raw transaction.
 *
 * @param tx_hex  Hex-encoded signed transaction
 * @return JSON result (caller must free):
 *         {"txid": "ab12..."} on success
 *         {"error": "message"} on failure
 */
char* nodecore_broadcast_tx(const char* tx_hex);

/**
 * Get raw transaction by txid.
 *
 * @param txid_hex  Transaction ID (hex)
 * @return JSON result (caller must free), or NULL if not found
 */
char* nodecore_get_transaction(const char* txid_hex);

// ============================================================================
// RPC Bridge (pass-through to internal RPC)
// ============================================================================

/**
 * Execute an RPC method.
 *
 * This is a general-purpose escape hatch — any RPC method available in
 * dinerod can be called here. Prefer the typed APIs above for common ops.
 *
 * @param method       RPC method name (e.g., "getblockchaininfo")
 * @param params_json  JSON array of positional params, or NULL for no params
 * @return JSON result string (caller must free), or NULL on error
 */
char* nodecore_rpc_call(const char* method, const char* params_json);

/**
 * Scan the UTXO set for outputs matching a given address.
 *
 * @param address  Bech32m address (e.g., "din1p...")
 * @return JSON result string with matching UTXOs and balance (caller must free),
 *         or NULL if node not running / invalid address
 */
char* nodecore_scan_address_json(const char* address);

// ============================================================================
// Local P2MR / Post-Quantum Wallet Primitives
// ============================================================================

/**
 * Derive a user-owned P2MR receive address from already-derived BIP32 material.
 *
 * This is a local wallet primitive: it does not require the embedded node to be
 * running and it does not write to the node wallet DB. The caller should store
 * the returned pq_seed under its own encrypted wallet vault.
 *
 * @param hrp               Bech32m HRP ("din", "tdin", or "rdin")
 * @param bip32_priv        32-byte BIP32 private key for m/88'/1448'/acct'/chg/index
 * @param bip32_priv_len    Must be 32
 * @param bip32_chain       32-byte BIP32 chain code for the same child key
 * @param bip32_chain_len   Must be 32
 * @param leaf_index        P2MR leaf index; v7 single-leaf wallets use 0
 * @return JSON string (caller must free):
 *         {
 *           "ok": true,
 *           "scheme_id": 1,
 *           "address": "din1r...",
 *           "script_pubkey": "5320...",
 *           "merkle_root": "...",
 *           "pubkey": "...",
 *           "pq_seed": "...",
 *           "leaf_index": 0,
 *           "merkle_depth": 0
 *         }
 *         or {"ok":false,"error":"..."} on failure.
 */
char* nodecore_p2mr_derive_address_json(
    const char* hrp,
    const uint8_t* bip32_priv,
    size_t bip32_priv_len,
    const uint8_t* bip32_chain,
    size_t bip32_chain_len,
    uint32_t leaf_index
);

/**
 * Reconstruct the P2MR public metadata for an already-stored 32-byte pq_seed.
 *
 * @param hrp          Bech32m HRP ("din", "tdin", or "rdin")
 * @param pq_seed      32-byte P2MR seed stored in the encrypted app vault
 * @param pq_seed_len  Must be 32
 * @return JSON string with address/script/pubkey metadata, caller frees.
 */
char* nodecore_p2mr_address_from_seed_json(
    const char* hrp,
    const uint8_t* pq_seed,
    size_t pq_seed_len
);

/**
 * Sign a 32-byte BIP341-style sighash and return the canonical P2MR witness.
 *
 * @param pq_seed      32-byte P2MR seed stored in the encrypted app vault
 * @param pq_seed_len  Must be 32
 * @param sighash      32-byte transaction sighash
 * @param sighash_len  Must be 32
 * @return JSON string (caller frees):
 *         {
 *           "ok": true,
 *           "scheme_id": 1,
 *           "pubkey": "...",
 *           "signature": "...",
 *           "witness": "..."
 *         }
 *         where witness is the single canonical witness stack item for P2MR.
 */
char* nodecore_p2mr_sign_witness_json(
    const uint8_t* pq_seed,
    size_t pq_seed_len,
    const uint8_t* sighash,
    size_t sighash_len
);

// ============================================================================
// Memory Management
// ============================================================================

/**
 * Free a string returned by any nodecore_ function.
 */
void nodecore_free_string(char* str);

// ============================================================================
// Version
// ============================================================================

/**
 * Get NodeCore library version string.
 * @return Static string (do NOT free)
 */
const char* nodecore_version(void);

#ifdef __cplusplus
}
#endif

#endif // NODECORE_FFI_H
