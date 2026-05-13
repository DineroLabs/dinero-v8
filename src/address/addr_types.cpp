#include "address/addr_types.h"

namespace dinero {

bool IsValidDestination(const Destination& d) {
    // Valid destinations have either:
    // - 20 bytes (P2PKH/P2WPKH hash160)
    // - 32 bytes (P2TR x-only pubkey)
    return d.is_valid && (d.pubkey_hash.size() == 20 || d.pubkey_hash.size() == 32);
}

} // namespace dinero
