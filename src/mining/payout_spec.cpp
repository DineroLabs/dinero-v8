// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mining/payout_spec.h"
#include "daemon/bech32_decode.h"
#include "dinero/compat/int128.hpp"

#include <algorithm>
#include <unordered_set>
#include <sstream>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <json/json.h>

// Use the bech32 decoder from mining namespace
using dinero::mining::Bech32DecodeSegwit;
using dinero::mining::GetBech32HRP;

namespace dinero {

// ============================================================================
// Static factory methods
// ============================================================================

PayoutSpec PayoutSpec::Single(const std::string& address) {
    PayoutSpec spec;
    spec.entries_.emplace_back(address, 100);  // Weight doesn't matter for single
    return spec;
}

PayoutSpec PayoutSpec::Weighted(const std::vector<PayoutEntry>& entries) {
    PayoutSpec spec;
    spec.entries_ = entries;
    return spec;
}

// ============================================================================
// Validation
// ============================================================================

PayoutSpec::ValidationResult PayoutSpec::Validate() const {
    // Check: at least one entry
    if (entries_.empty()) {
        return ValidationResult::Error("PayoutSpec must have at least one entry");
    }

    // Check: not too many entries
    if (entries_.size() > MAX_PAYOUT_ENTRIES) {
        return ValidationResult::Error(
            "PayoutSpec exceeds maximum entries (" +
            std::to_string(MAX_PAYOUT_ENTRIES) + ")"
        );
    }

    // Track addresses for duplicate detection
    std::unordered_set<std::string> seen_addresses;

    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];

        // Check: weight > 0
        if (entry.weight == 0) {
            return ValidationResult::Error(
                "Entry " + std::to_string(i) + " has zero weight"
            );
        }

        // Check: address not empty
        if (entry.address.empty()) {
            return ValidationResult::Error(
                "Entry " + std::to_string(i) + " has empty address"
            );
        }

        // Check: valid bech32 address
        if (!IsValidPayoutAddress(entry.address)) {
            return ValidationResult::Error(
                "Entry " + std::to_string(i) + " has invalid address: " + entry.address
            );
        }

        // Check: no duplicate addresses
        if (seen_addresses.count(entry.address) > 0) {
            return ValidationResult::Error(
                "Duplicate address: " + entry.address
            );
        }
        seen_addresses.insert(entry.address);
    }

    return ValidationResult::Ok();
}

// ============================================================================
// Weight normalization and amount calculation
// ============================================================================

uint64_t PayoutSpec::TotalWeight() const {
    uint64_t total = 0;
    for (const auto& entry : entries_) {
        total += entry.weight;
    }
    return total;
}

std::vector<ResolvedPayout> PayoutSpec::Resolve(uint64_t total_reward) const {
    // Validate first
    if (!Validate()) {
        return {};
    }

    // Handle edge case: zero reward
    if (total_reward == 0) {
        std::vector<ResolvedPayout> result;
        for (const auto& entry : entries_) {
            result.emplace_back(entry.address, 0);
        }
        return result;
    }

    // Calculate total weight
    uint64_t total_weight = TotalWeight();
    if (total_weight == 0) {
        return {};  // Should not happen after validation
    }

    // Calculate shares using integer arithmetic
    // Formula: share_i = floor(total_reward * weight_i / total_weight)
    std::vector<ResolvedPayout> result;
    result.reserve(entries_.size());

    uint64_t distributed = 0;

    for (const auto& entry : entries_) {
        // share = (total_reward * weight) / total_weight, computed in 128-bit
        // arithmetic to prevent intermediate overflow. The multiply uses the
        // portability shim; the divide is platform-specific because the shim
        // doesn't currently expose a `u128 / uint64_t` operator.
        auto numerator = dinero::compat::mul_u64(total_reward, entry.weight);
#if defined(DINERO_INT128_NATIVE)
        // Native __uint128_t supports `/` directly.
        uint64_t share = static_cast<uint64_t>(numerator / total_weight);
#elif defined(DINERO_INT128_MSVC_X64)
        // _udiv128(hi, lo, divisor, &remainder) -> quotient.
        uint64_t remainder;
        uint64_t share = _udiv128(dinero::compat::hi64(numerator),
                                  dinero::compat::lo64(numerator),
                                  total_weight, &remainder);
#else
#  error "payout_spec: no 128-bit divide available on this target"
#endif

        result.emplace_back(entry.address, share);
        distributed += share;
    }

    // Calculate remainder due to integer division
    uint64_t remainder = total_reward - distributed;

    // Assign remainder to first entry (deterministic)
    // This ensures sum(amounts) == total_reward exactly
    if (remainder > 0 && !result.empty()) {
        result[0].amount += remainder;
    }

    return result;
}

// ============================================================================
// Serialization
// ============================================================================

std::string PayoutSpec::ToJson() const {
    Json::Value array(Json::arrayValue);

    for (const auto& entry : entries_) {
        Json::Value obj(Json::objectValue);
        obj["address"] = entry.address;
        obj["weight"] = entry.weight;
        array.append(obj);
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, array);
}

std::optional<PayoutSpec> PayoutSpec::FromJson(const std::string& json) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(json);

    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        return std::nullopt;
    }

    if (!root.isArray()) {
        return std::nullopt;
    }

    std::vector<PayoutEntry> entries;
    entries.reserve(root.size());

    for (const auto& item : root) {
        if (!item.isObject()) {
            return std::nullopt;
        }

        if (!item.isMember("address") || !item["address"].isString()) {
            return std::nullopt;
        }

        if (!item.isMember("weight") || !item["weight"].isUInt()) {
            return std::nullopt;
        }

        entries.emplace_back(
            item["address"].asString(),
            item["weight"].asUInt()
        );
    }

    return PayoutSpec::Weighted(entries);
}

// ============================================================================
// Address validation
// ============================================================================

bool IsValidPayoutAddress(const std::string& address) {
    if (address.empty()) {
        return false;
    }

    // Get the expected HRP for current network
    std::string hrp = GetBech32HRP();

    // Decode the address
    int witver = -1;
    std::vector<uint8_t> witprog;

    if (!Bech32DecodeSegwit(address, hrp, witver, witprog)) {
        return false;
    }

    // Valid witness versions: 0-16
    if (witver < 0 || witver > 16) {
        return false;
    }

    // Valid program lengths:
    // - v0: 20 bytes (P2WPKH) or 32 bytes (P2WSH)
    // - v1+: 32 bytes (Taproot and future)
    size_t prog_len = witprog.size();
    if (witver == 0) {
        if (prog_len != 20 && prog_len != 32) {
            return false;
        }
    } else {
        if (prog_len < 2 || prog_len > 40) {
            return false;
        }
    }

    return true;
}

} // namespace dinero
