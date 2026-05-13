// BIP39 Demo - Real Seed Phrase Generation and Wallet Restoration
// Compile: g++ -std=c++17 -I../include examples/bip39_demo.cpp -L../build -ldinero_wallet -ldinero_crypto -lsecp256k1 -framework Security

#include "wallet/bip39.h"
#include "wallet/hd_wallet.h"
#include <iostream>
#include <iomanip>

void printHeader(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "  " << title << std::endl;
    std::cout << std::string(60, '=') << std::endl << std::endl;
}

void demo1_generate_mnemonic() {
    printHeader("Demo 1: Generate New BIP39 Mnemonic");
    
    // Generate 12-word mnemonic
    std::string mnemonic = dinero::bip39::Generate(dinero::bip39::WordCount::Words12);
    
    std::cout << "🔑 Generated Mnemonic (12 words):\n";
    std::cout << "   " << mnemonic << "\n\n";
    
    // Validate it
    bool valid = dinero::bip39::ValidateMnemonic(mnemonic);
    std::cout << "✅ Checksum: " << (valid ? "VALID" : "INVALID") << "\n\n";
    
    // Convert to seed
    std::vector<uint8_t> seed;
    if (dinero::bip39::MnemonicToSeed(mnemonic, "", seed)) {
        std::cout << "🌱 Seed (first 32 bytes):\n   ";
        for (size_t i = 0; i < std::min(size_t(32), seed.size()); i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') 
                     << (int)seed[i];
            if (i % 8 == 7) std::cout << " ";
        }
        std::cout << std::dec << "\n";
    }
}

void demo2_create_wallet() {
    printHeader("Demo 2: Create Wallet with Mnemonic");
    
    std::string mnemonic;
    auto wallet = HDWallet::CreateNew("/tmp/dinero_demo_wallet", 1, mnemonic);
    
    std::cout << "💼 New Wallet Created!\n\n";
    std::cout << "🔑 YOUR SEED PHRASE (SAVE THIS!):\n";
    std::cout << "   " << mnemonic << "\n\n";
    std::cout << "⚠️  NEVER SHARE THIS WITH ANYONE!\n";
    std::cout << "⚠️  WRITE IT DOWN AND STORE IT SAFELY!\n\n";
    
    // Generate some addresses
    std::cout << "📬 Addresses:\n";
    for (int i = 0; i < 3; i++) {
        std::string addr = wallet->DeriveNextAddress();
        std::cout << "   Address #" << i+1 << ": " << addr << "\n";
    }
}

void demo3_restore_wallet() {
    printHeader("Demo 3: Restore Wallet from Mnemonic");
    
    // Use a known mnemonic
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    
    std::cout << "🔄 Restoring wallet from:\n";
    std::cout << "   " << mnemonic << "\n\n";
    
    auto wallet = HDWallet::Restore("/tmp/dinero_demo_restore", 1, mnemonic);
    
    std::cout << "✅ Wallet Restored!\n\n";
    std::cout << "📬 First 3 addresses:\n";
    for (int i = 0; i < 3; i++) {
        std::string addr = wallet->DeriveNextAddress();
        std::cout << "   Address #" << i+1 << ": " << addr << "\n";
    }
    
    std::cout << "\n💡 Tip: Restoring same mnemonic always gives same addresses!\n";
}

void demo4_passphrase() {
    printHeader("Demo 4: Passphrase Protection (13th Word)");
    
    std::string mnemonic = "legal winner thank year wave sausage worth useful legal winner thank yellow";
    
    std::cout << "🔑 Base Mnemonic:\n";
    std::cout << "   " << mnemonic << "\n\n";
    
    // Without passphrase
    std::vector<uint8_t> seed1;
    dinero::bip39::MnemonicToSeed(mnemonic, "", seed1);
    
    std::cout << "Seed (no passphrase): ";
    for (size_t i = 0; i < 16; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)seed1[i];
    }
    std::cout << "...\n";
    
    // With passphrase
    std::vector<uint8_t> seed2;
    dinero::bip39::MnemonicToSeed(mnemonic, "TREZOR", seed2);
    
    std::cout << "Seed (passphrase=\"TREZOR\"): ";
    for (size_t i = 0; i < 16; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)seed2[i];
    }
    std::cout << std::dec << "...\n\n";
    
    std::cout << "💡 Different passphrase = Different wallet (Plausible deniability!)\n";
}

int main() {
    std::cout << R"(
    ██████╗ ██╗███╗   ██╗███████╗██████╗  ██████╗ 
    ██╔══██╗██║████╗  ██║██╔════╝██╔══██╗██╔═══██╗
    ██║  ██║██║██╔██╗ ██║█████╗  ██████╔╝██║   ██║
    ██║  ██║██║██║╚██╗██║██╔══╝  ██╔══██╗██║   ██║
    ██████╔╝██║██║ ╚████║███████╗██║  ██║╚██████╔╝
    ╚═════╝ ╚═╝╚═╝  ╚═══╝╚══════╝╚═╝  ╚═╝ ╚═════╝ 
    
    BIP39 Mnemonic Seed Phrase Demo
    )" << std::endl;
    
    try {
        demo1_generate_mnemonic();
        demo2_create_wallet();
        demo3_restore_wallet();
        demo4_passphrase();
        
        printHeader("Demo Complete!");
        std::cout << "✅ All demos ran successfully!\n\n";
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}

