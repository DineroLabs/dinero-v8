#pragma once
#include "daemon/mempool_transaction.h"
#include <functional>
#include <utility>

namespace dinero {
// Captured before admission publication. This is an observer capability, not
// transaction authorization or a durable pending-payment acknowledgement.
class MempoolAcceptanceObserver {
public:
    using HistoricalCallback = std::function<void(const Transaction&)>;
    using BodyCallback = std::function<void(const MempoolTransaction&)>;
    using Notification = std::function<void()>;

    static MempoolAcceptanceObserver ForHistorical(HistoricalCallback callback) {
        MempoolAcceptanceObserver result;
        result.historical_ = std::move(callback);
        return result;
    }
    static MempoolAcceptanceObserver ForBody(BodyCallback callback) {
        MempoolAcceptanceObserver result;
        result.body_ = std::move(callback);
        return result;
    }
    void Swap(MempoolAcceptanceObserver& other) noexcept {
        historical_.swap(other.historical_);
        body_.swap(other.body_);
    }
    Notification Prepare(const MempoolTransaction& body) const {
        if (!body.HasBody()) throw std::invalid_argument("Acceptance observer body unavailable");
        if (historical_) {
            // Validate capability before effects. Retain the immutable owner
            // with its original representation; no conversion in notification.
            const Transaction* historical = &body.Historical();
            return [owner=body, callback=historical_, historical] { callback(*historical); };
        }
        if (body_) return [owner=body, callback=body_] { callback(owner); };
        return {};
    }
private:
    HistoricalCallback historical_;
    BodyCallback body_;
};
} // namespace dinero
