/**
 * Wallet RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Replaces old registerHandler() pattern with self-documenting, introspectable methods.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_wallet.h"
#include "wallet/wallet_api.h"
#include "common/logger.h"
#include <iostream>

namespace din {
namespace rpc {

// Forward declarations for implementation functions (defined in methods_wallet.cpp)
extern din::Json getbalance_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getnewaddress_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json listaddresses_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json listunspent_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getwalletinfo_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json validateaddress_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json walletlock_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json walletunlock_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json encryptwallet_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json walletpassphrasechange_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json sendtoaddress_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json listtransactions_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json backupwallet_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json deriveaddress_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json dumpprivkey_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json walletcreatefundedpsbt_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json walletprocesspsbt_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json finalizepsbt_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json combinepsbt_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json createrawtransaction_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json signrawtransactionwithwallet_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json decoderawtransaction_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getrawtransaction_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json sendrawtransaction_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json importprivkey_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json dumpwallet_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json importwallet_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json setlabel_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getlabel_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json walletrescan_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json settxfee_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json listaddresseswithbalances_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json exportcsv_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json generateqrcode_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json createhdwallet_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json restorewallet_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json notarizebackup_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json scanutxos_impl(const ExecutionContext& ctx, const din::Json& params);

void registerWalletMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // WALLET BALANCE & ADDRESS OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.getbalance", "wallet")
        .description("Returns the total available balance in the wallet")
        .params({})
        .result("object", "Balance information including confirmed and unconfirmed amounts")
        .handler(getbalance_impl)
        .examples({
            "wallet.getbalance"
        });

    RPC_METHOD("wallet.getnewaddress", "wallet")
        .description("Generates a new receiving address for the wallet")
        .param("label", "string", "Optional label for the address", false)
        .result("string", "The newly generated address")
        .handler(getnewaddress_impl)
        .examples({
            "wallet.getnewaddress",
            "wallet.getnewaddress \"donations\""
        });

    RPC_METHOD("wallet.listaddresses", "wallet")
        .description("Lists all addresses in the wallet")
        .params({})
        .result("array", "List of addresses with labels and metadata")
        .handler(listaddresses_impl)
        .examples({
            "wallet.listaddresses"
        });

    RPC_METHOD("wallet.listunspent", "wallet")
        .description("Returns array of unspent transaction outputs")
        .param("minconf", "number", "Minimum confirmations (default: 1)", false)
        .param("maxconf", "number", "Maximum confirmations (default: 9999999)", false)
        .param("addresses", "array", "Filter by specific addresses", false)
        .result("array", "Array of unspent outputs")
        .handler(listunspent_impl)
        .examples({
            "wallet.listunspent",
            "wallet.listunspent 6",
            "wallet.listunspent 1 999999 '[\"din1q...\"]'"
        });

    RPC_METHOD("wallet.getwalletinfo", "wallet")
        .description("Returns an object containing various wallet state info")
        .params({})
        .result("object", "Wallet information including balance, tx count, HD status")
        .handler(getwalletinfo_impl)
        .examples({
            "wallet.getwalletinfo"
        });

    RPC_METHOD("wallet.validateaddress", "wallet")
        .description("Validates a Dinero address and returns detailed information")
        .param("address", "string", "The Dinero address to validate", true)
        .result("object", "Validation result with isvalid, ismine, network fields")
        .handler(validateaddress_impl)
        .examples({
            "wallet.validateaddress \"din1q...\""
        });

    // ═══════════════════════════════════════════════════════════════
    // WALLET SECURITY OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.walletlock", "wallet")
        .description("Locks the wallet by removing encryption keys from memory")
        .params({})
        .result("object", "Success status")
        .handler(walletlock_impl)
        .examples({
            "wallet.walletlock"
        });

    RPC_METHOD("wallet.walletunlock", "wallet")
        .description("Unlocks the wallet for operations requiring private keys")
        .param("passphrase", "string", "The wallet passphrase", true)
        .param("timeout", "number", "Timeout in seconds (default: 60)", false)
        .result("object", "Success status")
        .handler(walletunlock_impl)
        .examples({
            "wallet.walletunlock \"mypassphrase\" 300"
        });

    RPC_METHOD("wallet.encryptwallet", "wallet")
        .description("Encrypts the wallet with a passphrase")
        .param("passphrase", "string", "The passphrase to encrypt with", true)
        .result("object", "Encryption status")
        .handler(encryptwallet_impl)
        .examples({
            "wallet.encryptwallet \"strongpassphrase\""
        });

    RPC_METHOD("wallet.walletpassphrasechange", "wallet")
        .description("Changes the wallet passphrase")
        .param("oldpassphrase", "string", "Current passphrase", true)
        .param("newpassphrase", "string", "New passphrase", true)
        .result("object", "Success status")
        .handler(walletpassphrasechange_impl)
        .examples({
            "wallet.walletpassphrasechange \"oldpass\" \"newpass\""
        });

    // ═══════════════════════════════════════════════════════════════
    // TRANSACTION OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    // TEMPORARILY DISABLED: Using context-aware version from methods_wallet_context.cpp for TEST_ONLY mode
    // RPC_METHOD("wallet.sendtoaddress", "wallet")
    //     .description("Send an amount to a given address")
    //     .param("address", "string", "The receiving address", true)
    //     .param("amount", "number", "The amount in DIN", true)
    //     .param("comment", "string", "Transaction comment", false)
    //     .result("string", "Transaction ID")
    //     .handler(sendtoaddress_impl)
    //     .examples({
    //         "wallet.sendtoaddress \"din1q...\" 10.5",
    //         "wallet.sendtoaddress \"din1q...\" 1.0 \"payment for services\""
    //     });

    RPC_METHOD("wallet.listtransactions", "wallet")
        .description("Returns up to 'count' most recent transactions")
        .param("count", "number", "Number of transactions to return (default: 10)", false)
        .param("skip", "number", "Number of transactions to skip (default: 0)", false)
        .result("array", "Array of transaction objects")
        .handler(listtransactions_impl)
        .examples({
            "wallet.listtransactions",
            "wallet.listtransactions 20",
            "wallet.listtransactions 10 5"
        });

    // ═══════════════════════════════════════════════════════════════
    // BACKUP & RECOVERY OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.backupwallet", "wallet")
        .description("Safely copies wallet.dat to destination")
        .param("destination", "string", "Destination file path", true)
        .result("object", "Backup status")
        .handler(backupwallet_impl)
        .examples({
            "wallet.backupwallet \"/backup/wallet-backup.dat\""
        });

    RPC_METHOD("wallet.deriveaddress", "wallet")
        .description("Derives an address from HD wallet at specific path")
        .param("path", "string", "BIP32 derivation path", true)
        .result("string", "Derived address")
        .handler(deriveaddress_impl)
        .examples({
            "wallet.deriveaddress \"m/86'/1448'/0'/0/0\""
        });

    RPC_METHOD("wallet.dumpprivkey", "wallet")
        .description("Reveals the private key corresponding to an address")
        .param("address", "string", "The address", true)
        .result("string", "The private key in WIF format")
        .handler(dumpprivkey_impl)
        .examples({
            "wallet.dumpprivkey \"din1q...\""
        });

    // ═══════════════════════════════════════════════════════════════
    // PSBT (Partially Signed Bitcoin Transaction) OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.walletcreatefundedpsbt", "wallet")
        .description("Creates and funds a PSBT with wallet UTXOs")
        .param("outputs", "object", "Output addresses and amounts", true)
        .param("fee_rate", "number", "Fee rate in una/vbyte (default: 1)", false)
        .result("object", "PSBT in base64 and hex formats")
        .handler(walletcreatefundedpsbt_impl)
        .examples({
            "wallet.walletcreatefundedpsbt '{\"din1q...\":10.5}'",
            "wallet.walletcreatefundedpsbt '{\"din1q...\":5.0}' 10"
        });

    RPC_METHOD("wallet.walletprocesspsbt", "wallet")
        .description("Signs inputs in a PSBT")
        .param("psbt", "string", "Base64-encoded PSBT", true)
        .result("object", "Updated PSBT with signatures")
        .handler(walletprocesspsbt_impl)
        .examples({
            "wallet.walletprocesspsbt \"cHNidP8BAH...\""
        });

    RPC_METHOD("wallet.finalizepsbt", "wallet")
        .description("Finalizes a PSBT if possible")
        .param("psbt", "string", "Base64-encoded PSBT", true)
        .result("object", "Finalized transaction hex if complete")
        .handler(finalizepsbt_impl)
        .examples({
            "wallet.finalizepsbt \"cHNidP8BAH...\""
        });

    RPC_METHOD("wallet.combinepsbt", "wallet")
        .description("Combines multiple PSBTs into one")
        .param("psbts", "array", "Array of base64 PSBTs", true)
        .result("object", "Combined PSBT")
        .handler(combinepsbt_impl)
        .examples({
            "wallet.combinepsbt '[\"psbt1...\", \"psbt2...\"]'"
        });

    // ═══════════════════════════════════════════════════════════════
    // RAW TRANSACTION OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.createrawtransaction", "wallet")
        .description("Creates a raw transaction")
        .param("inputs", "array", "Array of transaction inputs", true)
        .param("outputs", "object", "Output addresses and amounts", true)
        .result("string", "Raw transaction hex")
        .handler(createrawtransaction_impl)
        .examples({
            "wallet.createrawtransaction '[{\"txid\":\"abc...\",\"vout\":0}]' '{\"din1q...\":1.0}'"
        });

    RPC_METHOD("wallet.signrawtransactionwithwallet", "wallet")
        .description("Signs a raw transaction with wallet keys")
        .param("hexstring", "string", "Raw transaction hex", true)
        .result("object", "Signed transaction with complete flag")
        .handler(signrawtransactionwithwallet_impl)
        .examples({
            "wallet.signrawtransactionwithwallet \"01000000...\""
        });

    RPC_METHOD("wallet.decoderawtransaction", "wallet")
        .description("Decodes a raw transaction hex")
        .param("hexstring", "string", "Transaction hex", true)
        .result("object", "Decoded transaction structure")
        .handler(decoderawtransaction_impl)
        .examples({
            "wallet.decoderawtransaction \"01000000...\""
        });

    RPC_METHOD("wallet.getrawtransaction", "wallet")
        .description("Returns raw transaction data")
        .param("txid", "string", "Transaction ID", true)
        .param("verbose", "boolean", "Decode transaction (default: false)", false)
        .result("string|object", "Raw hex or decoded transaction")
        .handler(getrawtransaction_impl)
        .examples({
            "wallet.getrawtransaction \"abc123...\"",
            "wallet.getrawtransaction \"abc123...\" true"
        });

    RPC_METHOD("wallet.sendrawtransaction", "wallet")
        .description("Broadcasts a raw transaction to the network")
        .param("hexstring", "string", "Signed transaction hex", true)
        .result("string", "Transaction ID")
        .handler(sendrawtransaction_impl)
        .examples({
            "wallet.sendrawtransaction \"01000000...\""
        });

    // ═══════════════════════════════════════════════════════════════
    // IMPORT/EXPORT OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.importprivkey", "wallet")
        .description("Imports a private key into the wallet")
        .param("privkey", "string", "Private key in WIF format", true)
        .param("label", "string", "Optional label", false)
        .param("rescan", "boolean", "Rescan blockchain (default: true)", false)
        .result("object", "Import status")
        .handler(importprivkey_impl)
        .examples({
            "wallet.importprivkey \"L1aW4...\" \"imported\" false"
        });

    RPC_METHOD("wallet.dumpwallet", "wallet")
        .description("Dumps all wallet keys to a file")
        .param("filename", "string", "Output file path", true)
        .result("object", "Dump status with file path")
        .handler(dumpwallet_impl)
        .examples({
            "wallet.dumpwallet \"/backup/wallet-dump.txt\""
        });

    RPC_METHOD("wallet.importwallet", "wallet")
        .description("Imports keys from a wallet dump file")
        .param("filename", "string", "Input file path", true)
        .result("object", "Import status")
        .handler(importwallet_impl)
        .examples({
            "wallet.importwallet \"/backup/wallet-dump.txt\""
        });

    // ═══════════════════════════════════════════════════════════════
    // LABEL & METADATA OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.setlabel", "wallet")
        .description("Sets the label associated with an address")
        .param("address", "string", "The address", true)
        .param("label", "string", "The label", true)
        .result("object", "Success status")
        .handler(setlabel_impl)
        .examples({
            "wallet.setlabel \"din1q...\" \"savings\""
        });

    RPC_METHOD("wallet.getlabel", "wallet")
        .description("Gets the label associated with an address")
        .param("address", "string", "The address", true)
        .result("string", "The label")
        .handler(getlabel_impl)
        .examples({
            "wallet.getlabel \"din1q...\""
        });

    // ═══════════════════════════════════════════════════════════════
    // ADVANCED WALLET OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.walletrescan", "wallet")
        .description("Rescans the blockchain for wallet transactions")
        .param("start_height", "number", "Starting block height (default: 0)", false)
        .result("object", "Rescan status and progress")
        .handler(walletrescan_impl)
        .examples({
            "wallet.walletrescan",
            "wallet.walletrescan 1000"
        });

    RPC_METHOD("wallet.settxfee", "wallet")
        .description("Sets the transaction fee rate")
        .param("amount", "number", "Fee rate in DIN/kB", true)
        .result("object", "Success status")
        .handler(settxfee_impl)
        .examples({
            "wallet.settxfee 0.001"
        });

    RPC_METHOD("wallet.listaddresseswithbalances", "wallet")
        .description("Lists all addresses with their balances")
        .params({})
        .result("array", "Array of addresses with balance information")
        .handler(listaddresseswithbalances_impl)
        .examples({
            "wallet.listaddresseswithbalances"
        });

    RPC_METHOD("wallet.exportcsv", "wallet")
        .description("Exports transaction history to CSV file")
        .param("filename", "string", "Output CSV file path", true)
        .result("object", "Export status")
        .handler(exportcsv_impl)
        .examples({
            "wallet.exportcsv \"/exports/transactions.csv\""
        });

    RPC_METHOD("wallet.generateqrcode", "wallet")
        .description("Generates a QR code for an address or payment URI")
        .param("data", "string", "Address or payment URI", true)
        .param("format", "string", "Output format: png, svg (default: png)", false)
        .result("object", "QR code data or file path")
        .handler(generateqrcode_impl)
        .examples({
            "wallet.generateqrcode \"din1q...\"",
            "wallet.generateqrcode \"dinero:din1q...?amount=10\" \"svg\""
        });

    // ═══════════════════════════════════════════════════════════════
    // HD WALLET OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("wallet.createhdwallet", "wallet")
        .description("Creates a new HD (Hierarchical Deterministic) wallet")
        .param("mnemonic", "string", "Optional BIP39 mnemonic (generates new if not provided)", false)
        .param("passphrase", "string", "Optional passphrase for encryption", false)
        .result("object", "HD wallet creation status and seed phrase (if new)")
        .handler(createhdwallet_impl)
        .examples({
            "wallet.createhdwallet",
            "wallet.createhdwallet \"word1 word2 ... word24\"",
            "wallet.createhdwallet \"\" \"mypassphrase\""
        });

    RPC_METHOD("wallet.restorewallet", "wallet")
        .description("Restores HD wallet from BIP39 mnemonic seed phrase")
        .param("mnemonic", "string", "BIP39 mnemonic seed phrase", true)
        .param("passphrase", "string", "Optional passphrase", false)
        .result("object", "Restore status")
        .handler(restorewallet_impl)
        .examples({
            "wallet.restorewallet \"word1 word2 ... word24\"",
            "wallet.restorewallet \"word1 word2 ... word24\" \"mypassphrase\""
        });

    RPC_METHOD("wallet.notarizebackup", "wallet")
        .description("Creates a notarized backup with blockchain timestamp proof")
        .param("destination", "string", "Backup file path", true)
        .result("object", "Notarization status with blockchain proof")
        .handler(notarizebackup_impl)
        .examples({
            "wallet.notarizebackup \"/backup/notarized-backup.dat\""
        });

    RPC_METHOD("wallet.scanutxos", "wallet")
        .description("Scans for UTXOs matching specific criteria")
        .param("filter", "object", "Filter criteria (addresses, amounts, etc.)", false)
        .result("array", "Matching UTXOs")
        .handler(scanutxos_impl)
        .examples({
            "wallet.scanutxos",
            "wallet.scanutxos '{\"minAmount\":1.0}'"
        });

    std::cout << "[Wallet RPC vNext] ✅ Registered 38 wallet methods with full metadata" << std::endl;
}

} // namespace rpc
} // namespace din

// Auto-register at startup
static auto _wallet_vnext_init = (din::rpc::registerWalletMethodsVNext(), 0);
