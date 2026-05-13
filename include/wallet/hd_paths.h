#pragma once
#include <cstdint>
#include <string>

namespace dinero {
namespace wallet {

// ─── BIP32 purpose codes ─────────────────────────────────────────────────────
// 77'  — Dinero shielded key namespace
// 86'  — Taproot spend keys (BIP-86)
// 84'  — Native SegWit spend keys (BIP-84)

static constexpr uint32_t PURPOSE_SHIELDED = 77;   // m/77'/...
static constexpr uint32_t PURPOSE_TAPROOT  = 86;   // m/86'/...
static constexpr uint32_t PURPOSE_SEGWIT   = 84;   // m/84'/...

} // namespace wallet
} // namespace dinero
