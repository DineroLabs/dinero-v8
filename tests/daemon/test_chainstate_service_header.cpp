// Include this interface first: no other header may supply its proof view.
#include "daemon/services/chainstate_service.h"

#include <optional>
#include <span>
#include <type_traits>

// Older libc++ eagerly checks span's range constructor while instantiating
// optional. A forward declaration alone compiled on newer standard libraries
// but broke the macOS Xcode 15.4 operator build.
static_assert(sizeof(dinero::MempoolProofView) > 0);
static_assert(std::is_copy_constructible_v<std::span<const dinero::MempoolProofView>>);
static_assert(std::is_move_constructible_v<std::optional<std::span<const dinero::MempoolProofView>>>);
