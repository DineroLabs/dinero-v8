/**
 * Ledger Production Safety Test
 *
 * CRITICAL: Verifies that production builds NEVER fabricate signatures
 *
 * This test MUST pass in production builds (DIN_HW_WALLET_MOCK=OFF)
 * This test validates that mock signing code cannot be activated accidentally.
 */

#include "wallet/hardware_wallet.h"
#include <iostream>
#include <cassert>

using namespace dinero;

int main() {
    std::cout << "=== Ledger Production Safety Test ===" << std::endl;
    std::cout << "Verifying mock signing is DISABLED\n" << std::endl;

#ifdef DIN_HW_WALLET_MOCK
    std::cerr << "❌ FATAL: Mock signing is ENABLED in this build" << std::endl;
    std::cerr << "This build is NOT production-safe" << std::endl;
    std::cerr << "DIN_HW_WALLET_MOCK flag should be OFF for production" << std::endl;
    return 1;
#else
    std::cout << "✅ Mock signing compile flag: DISABLED (correct)" << std::endl;
#endif

    std::cout << "\nTesting runtime behavior..." << std::endl;

    LedgerWallet ledger;
    ledger.initialize();

    // Create a mock signing request
    SigningRequest request;
    request.transaction_hash = "test_tx_hash";

    std::cout << "  Calling signTransaction() without device..." << std::endl;

    // This MUST fail in production builds
    SigningResult result = ledger.signTransaction(request);

    if (result.success) {
        std::cerr << "\n❌ FATAL: signTransaction() returned success without device!" << std::endl;
        std::cerr << "This violates the hardware wallet trust model" << std::endl;
        std::cerr << "Production builds MUST NOT fabricate signatures" << std::endl;
        return 1;
    }

    if (!result.signatures.empty()) {
        std::cerr << "\n❌ FATAL: signTransaction() returned signatures without device!" << std::endl;
        std::cerr << "Production builds MUST NOT fabricate signatures" << std::endl;
        return 1;
    }

    if (result.error_message.empty()) {
        std::cerr << "\n❌ FATAL: signTransaction() failed silently (no error message)" << std::endl;
        std::cerr << "Production builds MUST provide clear error messages" << std::endl;
        return 1;
    }

    std::cout << "  ✅ Correctly returned error: " << result.error_message << std::endl;

    // Test signMessage as well
    std::cout << "\n  Calling signMessage() without device..." << std::endl;
    SigningResult msg_result = ledger.signMessage("test_message", "m/44'/1447'/0'/0/0");

    if (msg_result.success || !msg_result.signatures.empty()) {
        std::cerr << "\n❌ FATAL: signMessage() fabricated signature!" << std::endl;
        return 1;
    }

    std::cout << "  ✅ Correctly returned error: " << msg_result.error_message << std::endl;

    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "✅ PRODUCTION SAFETY VERIFIED" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "\nValidated:" << std::endl;
    std::cout << "  • Mock signing compile flag is OFF" << std::endl;
    std::cout << "  • signTransaction() refuses to fabricate signatures" << std::endl;
    std::cout << "  • signMessage() refuses to fabricate signatures" << std::endl;
    std::cout << "  • Errors are reported clearly" << std::endl;
    std::cout << "\n✅ This build is PRODUCTION-SAFE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    return 0;
}
