#include "wallet.h"
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>

using namespace dinero::wallet::reference;

void PrintUsage() {
    std::cout << "Dinero Reference Wallet CLI\n";
    std::cout << "================================\n\n";
    std::cout << "Wallet Management:\n";
    std::cout << "  dinero-wallet-cli create <wallet_name> [word_count]  - Create new wallet\n";
    std::cout << "  dinero-wallet-cli load <wallet_name>                 - Load existing wallet\n";
    std::cout << "  dinero-wallet-cli info <wallet_name>                 - Get wallet info\n";
    std::cout << "  dinero-wallet-cli address <wallet_name>              - Get wallet address\n";
    std::cout << "\n";
    std::cout << "Balance & UTXOs:\n";
    std::cout << "  dinero-wallet-cli balance <wallet_name>              - Get wallet balance\n";
    std::cout << "  dinero-wallet-cli list-utxos <wallet_name>           - List unspent outputs\n";
    std::cout << "\n";
    std::cout << "Manual UTXO Injection (for testing):\n";
    std::cout << "  dinero-wallet-cli add-utxo <wallet> <txid> <vout> <amount> <height> [coinbase]\n";
    std::cout << "                                                       - Add UTXO manually\n";
    std::cout << "  dinero-wallet-cli remove-utxo <wallet> <txid> <vout> - Mark UTXO as spent\n";
    std::cout << "  dinero-wallet-cli set-height <wallet> <height>       - Set current blockchain height\n";
    std::cout << "\n";
    std::cout << "Transaction Building:\n";
    std::cout << "  dinero-wallet-cli build-tx <wallet> <address> <amount> [fee]\n";
    std::cout << "                                                       - Build test transaction\n";
    std::cout << "\n";
    std::cout << "Blockchain Synchronization:\n";
    std::cout << "  dinero-wallet-cli sync <wallet> <rpc_url> <rpc_user> <rpc_pass> [start_height] [max_blocks]\n";
    std::cout << "                                                       - Sync wallet with blockchain\n";
    std::cout << "\n";
    std::cout << "Utilities:\n";
    std::cout << "  dinero-wallet-cli validate <address>                 - Validate address\n";
    std::cout << "  dinero-wallet-cli generate-mnemonic [word_count]     - Generate mnemonic\n";
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            PrintUsage();
            return 1;
        }

        std::string command = argv[1];

        if (command == "create") {
            if (argc < 3) {
                std::cerr << "Error: wallet_name required\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            int word_count = (argc >= 4) ? std::stoi(argv[3]) : 24;

            // Generate mnemonic
            std::string mnemonic = ReferenceWallet::GenerateMnemonic(word_count);

            std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                    WALLET CREATED                                ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";
            std::cout << "⚠️  IMPORTANT: Write down your mnemonic phrase!\n";
            std::cout << "This is the ONLY way to recover your wallet.\n\n";
            std::cout << "Mnemonic:\n";
            std::cout << "┌──────────────────────────────────────────────────────────────────┐\n";
            std::cout << "│ " << mnemonic << "\n";
            std::cout << "└──────────────────────────────────────────────────────────────────┘\n\n";

            // Create wallet
            auto wallet = ReferenceWallet::CreateFromMnemonic(wallet_name, mnemonic);

            std::cout << "Wallet Name: " << wallet_name << "\n";
            std::cout << "Address: " << wallet->GetAddress() << "\n";
            std::cout << "Database: ./" << wallet_name << ".db\n";
            std::cout << "\n✅ Wallet created successfully!\n";

        } else if (command == "load") {
            if (argc < 3) {
                std::cerr << "Error: wallet_name required\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            auto wallet = ReferenceWallet::Load(wallet_name);

            std::cout << "✅ Wallet loaded: " << wallet_name << "\n";
            std::cout << "Address: " << wallet->GetAddress() << "\n";

        } else if (command == "address") {
            if (argc < 3) {
                std::cerr << "Error: wallet_name required\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            auto wallet = ReferenceWallet::Load(wallet_name);

            std::cout << wallet->GetAddress() << "\n";

        } else if (command == "balance") {
            if (argc < 3) {
                std::cerr << "Error: wallet_name required\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            auto wallet = ReferenceWallet::Load(wallet_name);

            auto balance = wallet->GetBalance();

            std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                    WALLET BALANCE                                ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";
            std::cout << std::fixed << std::setprecision(8);
            std::cout << "Confirmed:   " << (balance.confirmed / 100000000.0) << " DIN\n";
            std::cout << "Unconfirmed: " << (balance.unconfirmed / 100000000.0) << " DIN\n";
            std::cout << "Immature:    " << (balance.immature / 100000000.0) << " DIN\n";
            std::cout << "───────────────────────────────────────────────────────────\n";
            std::cout << "Total:       " << (balance.total / 100000000.0) << " DIN\n";

        } else if (command == "list-utxos") {
            if (argc < 3) {
                std::cerr << "Error: wallet_name required\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            auto wallet = ReferenceWallet::Load(wallet_name);

            auto utxos = wallet->ListUnspent();

            std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                    UNSPENT OUTPUTS                               ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";
            std::cout << "Count: " << utxos.size() << "\n\n";

            for (const auto& utxo : utxos) {
                std::cout << "TXID: " << utxo.txid << ":" << utxo.vout << "\n";
                std::cout << "Amount: " << std::fixed << std::setprecision(8)
                          << (utxo.amount / 100000000.0) << " DIN\n";
                std::cout << "Height: " << utxo.height
                          << (utxo.is_coinbase ? " (coinbase)" : "") << "\n";
                std::cout << "───────────────────────────────────────────────────────────\n";
            }

        } else if (command == "info") {
            if (argc < 3) {
                std::cerr << "Error: wallet_name required\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            auto wallet = ReferenceWallet::Load(wallet_name);

            auto info = wallet->GetWalletInfo();

            std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                    WALLET INFO                                   ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";
            std::cout << "Name: " << info.name << "\n";
            std::cout << "Address: " << info.address << "\n";
            std::cout << "Created: " << info.creation_time << "\n";
            std::cout << "Last Block: " << info.last_block_height << "\n";
            std::cout << "Block Hash: " << info.last_block_hash << "\n";

        } else if (command == "validate") {
            if (argc < 3) {
                std::cerr << "Error: address required\n";
                return 1;
            }

            std::string address = argv[2];
            bool valid = ReferenceWallet::ValidateAddress(address);

            if (valid) {
                std::cout << "✅ Valid Dinero address\n";
                return 0;
            } else {
                std::cout << "❌ Invalid Dinero address\n";
                return 1;
            }

        } else if (command == "generate-mnemonic") {
            int word_count = (argc >= 3) ? std::stoi(argv[2]) : 24;
            std::string mnemonic = ReferenceWallet::GenerateMnemonic(word_count);

            std::cout << mnemonic << "\n";

        } else if (command == "add-utxo") {
            if (argc < 7) {
                std::cerr << "Error: add-utxo requires: <wallet> <txid> <vout> <amount> <height> [coinbase]\n";
                std::cerr << "Example: add-utxo mywallet abc123... 0 5000000000 100 true\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            std::string txid = argv[3];
            uint32_t vout = std::stoul(argv[4]);
            uint64_t amount = std::stoull(argv[5]);
            uint32_t height = std::stoul(argv[6]);
            bool is_coinbase = (argc >= 8 && std::string(argv[7]) == "true");

            auto wallet = ReferenceWallet::Load(wallet_name);

            // Create UTXO
            UTXO utxo;
            utxo.txid = txid;
            utxo.vout = vout;
            utxo.amount = amount;
            utxo.height = height;
            utxo.is_coinbase = is_coinbase;

            // For now, use a placeholder script_pubkey (P2WPKH - 22 bytes of zeros)
            // In production, this would come from the actual transaction
            utxo.script_pubkey = "0000000000000000000000000000000000000000000000";

            wallet->AddUTXO(utxo);

            std::cout << "✅ UTXO added successfully!\n";
            std::cout << "TXID: " << txid << ":" << vout << "\n";
            std::cout << "Amount: " << std::fixed << std::setprecision(8)
                      << (amount / 100000000.0) << " DIN\n";
            std::cout << "Height: " << height << (is_coinbase ? " (coinbase)" : "") << "\n";

            // Show updated balance
            auto balance = wallet->GetBalance();
            std::cout << "\nUpdated Balance: " << std::fixed << std::setprecision(8)
                      << (balance.total / 100000000.0) << " DIN\n";

        } else if (command == "remove-utxo") {
            if (argc < 5) {
                std::cerr << "Error: remove-utxo requires: <wallet> <txid> <vout>\n";
                std::cerr << "Example: remove-utxo mywallet abc123... 0\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            std::string txid = argv[3];
            uint32_t vout = std::stoul(argv[4]);

            auto wallet = ReferenceWallet::Load(wallet_name);

            // Get current height for spent_at_height
            auto info = wallet->GetWalletInfo();
            uint32_t current_height = info.last_block_height;

            wallet->RemoveUTXO(txid, vout, "manual_removal", current_height);

            std::cout << "✅ UTXO marked as spent!\n";
            std::cout << "TXID: " << txid << ":" << vout << "\n";

            // Show updated balance
            auto balance = wallet->GetBalance();
            std::cout << "\nUpdated Balance: " << std::fixed << std::setprecision(8)
                      << (balance.total / 100000000.0) << " DIN\n";

        } else if (command == "set-height") {
            if (argc < 4) {
                std::cerr << "Error: set-height requires: <wallet> <height>\n";
                std::cerr << "Example: set-height mywallet 250000\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            uint32_t height = std::stoul(argv[3]);

            auto wallet = ReferenceWallet::Load(wallet_name);
            wallet->SetCurrentHeight(height);

            std::cout << "✅ Current height set to: " << height << "\n";

            // Show updated balance
            auto balance = wallet->GetBalance();
            std::cout << "\nUpdated Balance Breakdown:\n";
            std::cout << "Confirmed:   " << std::fixed << std::setprecision(8)
                      << (balance.confirmed / 100000000.0) << " DIN\n";
            std::cout << "Unconfirmed: " << (balance.unconfirmed / 100000000.0) << " DIN\n";
            std::cout << "Immature:    " << (balance.immature / 100000000.0) << " DIN\n";

        } else if (command == "build-tx") {
            if (argc < 5) {
                std::cerr << "Error: build-tx requires: <wallet> <address> <amount> [fee]\n";
                std::cerr << "Amount and fee in una\n";
                std::cerr << "Example: build-tx mywallet din1q... 1000000000 10000\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            std::string to_address = argv[3];
            uint64_t amount = std::stoull(argv[4]);
            uint64_t fee = (argc >= 6) ? std::stoull(argv[5]) : 10000;

            auto wallet = ReferenceWallet::Load(wallet_name);

            std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                    BUILD TRANSACTION                             ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";
            std::cout << "To:     " << to_address << "\n";
            std::cout << "Amount: " << std::fixed << std::setprecision(8)
                      << (amount / 100000000.0) << " DIN\n";
            std::cout << "Fee:    " << (fee / 100000000.0) << " DIN\n";
            std::cout << "Total:  " << ((amount + fee) / 100000000.0) << " DIN\n\n";

            try {
                // Get current height
                auto info = wallet->GetWalletInfo();
                uint32_t current_height = info.last_block_height;

                // Select UTXOs
                auto selected_utxos = wallet->SelectUTXOsForTransaction(amount, fee, current_height);

                uint64_t total_input = 0;
                std::cout << "Selected UTXOs:\n";
                for (const auto& utxo : selected_utxos) {
                    total_input += utxo.amount;
                    std::cout << "  " << utxo.txid << ":" << utxo.vout
                              << " - " << std::fixed << std::setprecision(8)
                              << (utxo.amount / 100000000.0) << " DIN\n";
                }

                uint64_t change_amount = total_input - amount - fee;
                std::cout << "\nTotal Input: " << (total_input / 100000000.0) << " DIN\n";
                std::cout << "Change:      " << (change_amount / 100000000.0) << " DIN\n";

                std::cout << "\n✅ Transaction build successful!\n";
                std::cout << "Note: This is a simulation. No actual transaction was created.\n";

            } catch (const std::exception& e) {
                std::cerr << "\n❌ Transaction build failed: " << e.what() << "\n";
                return 1;
            }

        } else if (command == "sync") {
            if (argc < 6) {
                std::cerr << "Error: sync requires: <wallet> <rpc_url> <rpc_user> <rpc_pass> [start_height] [max_blocks]\n";
                std::cerr << "Example: sync mywallet http://127.0.0.1:8332 dinero password123 0 1000\n";
                return 1;
            }

            std::string wallet_name = argv[2];
            std::string rpc_url = argv[3];
            std::string rpc_user = argv[4];
            std::string rpc_password = argv[5];
            uint32_t start_height = (argc >= 7) ? std::stoul(argv[6]) : 0;
            uint32_t max_blocks = (argc >= 8) ? std::stoul(argv[7]) : 0;

            auto wallet = ReferenceWallet::Load(wallet_name);

            std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                    BLOCKCHAIN SYNC                               ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";
            std::cout << "Wallet:       " << wallet_name << "\n";
            std::cout << "Address:      " << wallet->GetAddress() << "\n";
            std::cout << "RPC URL:      " << rpc_url << "\n";
            std::cout << "Start Height: " << start_height << "\n";
            if (max_blocks > 0) {
                std::cout << "Max Blocks:   " << max_blocks << "\n";
            } else {
                std::cout << "Max Blocks:   Sync to tip\n";
            }
            std::cout << "\n";

            try {
                bool success = wallet->SyncBlockchain(
                    rpc_url,
                    rpc_user,
                    rpc_password,
                    start_height,
                    max_blocks
                );

                if (success) {
                    std::cout << "\n✅ Blockchain sync completed successfully!\n\n";

                    // Show updated balance
                    auto balance = wallet->GetBalance();
                    std::cout << "Updated Balance:\n";
                    std::cout << "  Confirmed:   " << std::fixed << std::setprecision(8)
                              << (balance.confirmed / 100000000.0) << " DIN\n";
                    std::cout << "  Unconfirmed: " << (balance.unconfirmed / 100000000.0) << " DIN\n";
                    std::cout << "  Immature:    " << (balance.immature / 100000000.0) << " DIN\n";
                    std::cout << "  ───────────────────────────────────────────────────────────\n";
                    std::cout << "  Total:       " << (balance.total / 100000000.0) << " DIN\n";
                } else {
                    std::cerr << "\n❌ Blockchain sync failed!\n";
                    return 1;
                }

            } catch (const std::exception& e) {
                std::cerr << "\n❌ Blockchain sync error: " << e.what() << "\n";
                return 1;
            }

        } else {
            std::cerr << "Unknown command: " << command << "\n\n";
            PrintUsage();
            return 1;
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
