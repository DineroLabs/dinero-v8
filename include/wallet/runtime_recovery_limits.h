#pragma once
#include <cstddef>

namespace dinero {
// Maximum retained wallet facts per recovery projection. This is an
// operational material bound, not a promise about total process memory.
inline constexpr std::size_t kWalletRecoveryMaterialLimitBytes = 128u * 1024u * 1024u;
} // namespace dinero
