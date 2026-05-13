#include "wallet/lp_router.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dinero {

void LPRouter::RegisterProvider(const ProviderConfig& config) {
    // Replace if provider_id already exists
    for (auto& p : providers_) {
        if (p.provider_id == config.provider_id) {
            p = config;
            return;
        }
    }
    providers_.push_back(config);
}

bool LPRouter::RemoveProvider(const std::string& provider_id) {
    auto it = std::remove_if(providers_.begin(), providers_.end(),
        [&](const ProviderConfig& c) { return c.provider_id == provider_id; });
    if (it == providers_.end()) return false;
    providers_.erase(it, providers_.end());
    return true;
}

std::vector<ProviderConfig> LPRouter::GetProviders() const {
    return providers_;
}

bool LPRouter::IsEligible(const ProviderConfig& config, const QuoteRequest& request) const {
    if (!config.is_enabled) return false;

    // Check currency support
    bool currency_ok = false;
    for (auto c : config.supported_currencies) {
        if (c == request.currency) {
            currency_ok = true;
            break;
        }
    }
    if (!currency_ok) return false;

    // Check country support (empty = all countries)
    if (!config.supported_countries.empty() && !request.country_code.empty()) {
        bool country_ok = false;
        for (const auto& cc : config.supported_countries) {
            if (cc == request.country_code) {
                country_ok = true;
                break;
            }
        }
        if (!country_ok) return false;
    }

    return true;
}

std::vector<ProviderQuote> LPRouter::GetQuotes(const QuoteRequest& request) const {
    std::vector<ProviderQuote> quotes;

    for (const auto& config : providers_) {
        if (!IsEligible(config, request)) continue;

        // v1: Generate mock quotes from provider config.
        // v2: HTTP fetch from config.api_base_url + "/quote"
        ProviderQuote quote;
        quote.provider_id = config.provider_id;
        quote.provider_name = config.provider_name;
        quote.rate = 0.0;              // v1: populated by caller or mock
        quote.min_amount_una = 0;
        quote.max_amount_una = std::numeric_limits<uint64_t>::max();
        quote.fee_percent = 0.0;
        quote.fee_fixed_fiat = 0.0;
        quote.estimated_seconds = 0;
        quote.payment_method = "unknown";
        quote.requires_kyc = false;

        // Filter by amount range
        if (request.amount_una > 0) {
            if (request.amount_una < quote.min_amount_una ||
                request.amount_una > quote.max_amount_una) {
                continue;
            }
        }

        quotes.push_back(std::move(quote));
    }

    return quotes;
}

double LPRouter::ComputeEffectiveFiat(const ProviderQuote& quote, uint64_t amount_una) {
    double amount_dpi = static_cast<double>(amount_una) / 1e8;
    double gross = amount_dpi * quote.rate;
    double fee = gross * (quote.fee_percent / 100.0) + quote.fee_fixed_fiat;
    return gross - fee;  // For SELL: net fiat received. For BUY: negate to get cost.
}

std::optional<ProviderQuote> LPRouter::SelectBestProvider(const QuoteRequest& request) const {
    auto quotes = GetQuotes(request);
    if (quotes.empty()) return std::nullopt;

    // Select by highest effective rate (best deal for the user)
    const ProviderQuote* best = nullptr;
    double best_effective = -std::numeric_limits<double>::infinity();

    uint64_t amount = request.amount_una;
    if (amount == 0) amount = 100000000;  // Default 1 DPI for comparison

    for (const auto& q : quotes) {
        if (q.rate <= 0.0) continue;  // Skip unpriced quotes

        double effective = ComputeEffectiveFiat(q, amount);
        if (request.direction == QuoteDirection::BUY) {
            // For BUY, lower cost is better (negate)
            effective = -effective;
        }

        if (effective > best_effective) {
            best_effective = effective;
            best = &q;
        }
    }

    if (!best) return std::nullopt;
    return *best;
}

} // namespace dinero
