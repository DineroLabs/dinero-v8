// Stub for dinero::Params() used by HDWallet when dinero_consensus is not linked
// This provides minimal ChainParams functionality for tests and lightweight builds

#include "consensus/chainparams.h"
#include <cstring>

// Platform-specific weak symbol attribute
#ifdef _MSC_VER
    // MSVC: static library linker only pulls in .obj files to resolve unresolved
    // symbols, so if dinero_consensus provides ParamsImpl(), this stub is skipped.
    #define WEAK_SYMBOL
#elif defined(__GNUC__) || defined(__clang__)
    #define WEAK_SYMBOL __attribute__((weak))
#else
    // Fallback: no weak symbol support
    #define WEAK_SYMBOL
#endif

namespace dinero {
namespace detail {

// Weak symbol: Overridden by chainparams_impl.cpp if linked
// This prevents duplicate symbol errors when both are present
WEAK_SYMBOL const ChainParams& ParamsImpl() {
    // Static stub with minimal initialization
    static ChainParams stub = []() {
        ChainParams p{};
        p.hrp = "din";              // Mainnet Bech32 HRP
        p.name = "mainnet";
        p.magic = 0xD1A0C0DEu;      // Dinero mainnet magic bytes
        p.rpc_port = 20998;
        p.p2p_port = 20999;
        return p;
    }();

    return stub;
}

} // namespace detail
} // namespace dinero
