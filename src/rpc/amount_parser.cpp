#include "rpc/amount_parser.h"

#include "primitives/amount.h"

#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

namespace dinero::rpc {
namespace {

constexpr uint64_t UNA_PER_DIN = 100'000'000ULL;

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

bool ParseDigits(std::string_view digits, uint64_t limit, uint64_t& parsed) {
    if (digits.empty()) {
        return false;
    }

    uint64_t value = 0;
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (digit > limit || value > (limit - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    parsed = value;
    return true;
}

bool ParseDecimalString(std::string_view input,
                        uint64_t& amount_una,
                        std::string& error) {
    input = Trim(input);
    if (input.empty() || input.front() == '-' || input.front() == '+') {
        error = "Amount must be a positive decimal with at most 8 decimal places";
        return false;
    }

    const auto dot = input.find('.');
    if (dot != std::string_view::npos && input.find('.', dot + 1) != std::string_view::npos) {
        error = "Amount is not a valid decimal";
        return false;
    }

    std::string_view whole = dot == std::string_view::npos ? input : input.substr(0, dot);
    std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : input.substr(dot + 1);
    if (whole.empty()) {
        whole = "0";
    }
    if (dot != std::string_view::npos && fraction.empty()) {
        fraction = "0";
    }

    // Extra trailing zeroes do not add precision ("1.000000000" is exact).
    while (fraction.size() > 8 && fraction.back() == '0') {
        fraction.remove_suffix(1);
    }
    if (fraction.size() > 8) {
        error = "Amount has more than 8 decimal places";
        return false;
    }

    uint64_t whole_din = 0;
    if (!ParseDigits(whole, dinero::MAX_SUPPLY_UNA_CONST / UNA_PER_DIN, whole_din)) {
        error = "Amount is not a valid DIN value or exceeds the maximum supply";
        return false;
    }

    uint64_t fractional_una = 0;
    if (!fraction.empty()) {
        if (!ParseDigits(fraction, UNA_PER_DIN - 1, fractional_una)) {
            error = "Amount is not a valid decimal";
            return false;
        }
        for (size_t i = fraction.size(); i < 8; ++i) {
            fractional_una *= 10;
        }
    }

    const uint64_t whole_una = whole_din * UNA_PER_DIN;
    if (whole_una > dinero::MAX_SUPPLY_UNA_CONST - fractional_una) {
        error = "Amount exceeds the maximum supply";
        return false;
    }

    amount_una = whole_una + fractional_una;
    if (amount_una == 0) {
        error = "Amount must be positive";
        return false;
    }
    return true;
}

} // namespace

bool ParseDinAmountToUna(const din::Json& value,
                         uint64_t& amount_una,
                         std::string& error) {
    if (value.isString()) {
        return ParseDecimalString(value.asString(), amount_una, error);
    }

    if (!value.isNumeric()) {
        error = "Amount must be a DIN number or decimal string";
        return false;
    }

    if (value.type() == ::Json::intValue || value.type() == ::Json::uintValue) {
        uint64_t whole_din = 0;
        if (value.isInt64()) {
            const int64_t signed_value = value.asInt64();
            if (signed_value <= 0) {
                error = "Amount must be positive";
                return false;
            }
            whole_din = static_cast<uint64_t>(signed_value);
        } else {
            whole_din = value.asUInt64();
            if (whole_din == 0) {
                error = "Amount must be positive";
                return false;
            }
        }

        if (whole_din > dinero::MAX_SUPPLY_UNA_CONST / UNA_PER_DIN) {
            error = "Amount exceeds the maximum supply";
            return false;
        }
        amount_una = whole_din * UNA_PER_DIN;
        return true;
    }

    const double amount_din = value.asDouble();
    if (!std::isfinite(amount_din) || amount_din <= 0.0) {
        error = "Amount must be a finite positive value";
        return false;
    }

    // JsonCpp stores fractional JSON numbers as doubles. Bound the exact real
    // value by its adjacent representable doubles, then accept it only when
    // that interval contains exactly one integer una amount. This accepts
    // ordinary values such as 0.29 and 0.00000003 without truncation while
    // refusing sub-una values and large ambiguous doubles.
    const long double lower = static_cast<long double>(
        std::nextafter(amount_din, -std::numeric_limits<double>::infinity())) * UNA_PER_DIN;
    const long double upper = static_cast<long double>(
        std::nextafter(amount_din, std::numeric_limits<double>::infinity())) * UNA_PER_DIN;
    const long double first_integer = std::ceil(lower);
    const long double last_integer = std::floor(upper);

    if (first_integer != last_integer) {
        error = "Amount cannot be represented as one exact una value; use a decimal string or amount_una";
        return false;
    }
    if (first_integer < 1.0L ||
        first_integer > static_cast<long double>(dinero::MAX_SUPPLY_UNA_CONST)) {
        error = first_integer < 1.0L ? "Amount must be at least 1 una"
                                     : "Amount exceeds the maximum supply";
        return false;
    }

    amount_una = static_cast<uint64_t>(first_integer);
    return true;
}

bool ParseUnaAmount(const din::Json& value,
                    uint64_t& amount_una,
                    std::string& error) {
    uint64_t parsed = 0;
    if (value.type() == ::Json::uintValue) {
        parsed = value.asUInt64();
    } else if (value.type() == ::Json::intValue) {
        const int64_t signed_value = value.asInt64();
        if (signed_value <= 0) {
            error = "amount_una must be a positive integer";
            return false;
        }
        parsed = static_cast<uint64_t>(signed_value);
    } else if (value.isString()) {
        const std::string stored = value.asString();
        const std::string_view input = Trim(stored);
        if (!ParseDigits(input, dinero::MAX_SUPPLY_UNA_CONST, parsed)) {
            error = "amount_una must be a positive integer within the maximum supply";
            return false;
        }
    } else {
        error = "amount_una must be an integer (not a floating-point value)";
        return false;
    }

    if (parsed == 0 || parsed > dinero::MAX_SUPPLY_UNA_CONST) {
        error = parsed == 0 ? "amount_una must be positive"
                            : "amount_una exceeds the maximum supply";
        return false;
    }
    amount_una = parsed;
    return true;
}

double UnaToDin(uint64_t amount_una) {
    return static_cast<double>(amount_una) / static_cast<double>(UNA_PER_DIN);
}

} // namespace dinero::rpc
