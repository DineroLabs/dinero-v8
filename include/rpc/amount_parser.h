#pragma once

#include "din_json.h"

#include <cstdint>
#include <string>

namespace dinero::rpc {

// Convert a user-facing DIN amount to its exact una representation. Decimal
// strings retain all eight decimal places exactly. JSON numbers are accepted
// when their floating-point representation identifies one unambiguous una
// value; callers with very large or otherwise ambiguous amounts must use a
// decimal string or the integer amount_una interface.
bool ParseDinAmountToUna(const din::Json& value,
                         uint64_t& amount_una,
                         std::string& error);

// Parse an integer una amount without passing through floating point.
bool ParseUnaAmount(const din::Json& value,
                    uint64_t& amount_una,
                    std::string& error);

double UnaToDin(uint64_t amount_una);

} // namespace dinero::rpc
