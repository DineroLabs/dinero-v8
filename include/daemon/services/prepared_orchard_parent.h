#pragma once
#include "storage/legacy_retirement.h"
#include <memory>
namespace dinero {
class ChainstateService;
class PreparedOrchardCatalog;
class HistoricalCompactReplay;
class PreparedHistoricalCatalog;
class PreparedHistoricalCatalogRange;
// The actual service captures real storage owners and validates outside the
// selected lock. This opaque owner has the same public type in ON/OFF builds;
// backend-specific state is constructed only by an enabled service. Thread
// affinity applies to request lifetimes and unfinished replay. Completed
// immutable proof material may be shared by the service cache. No result
// authorizes activation without the selected owner's full state comparison.
class PreparedOrchardParent final {
public:
    ~PreparedOrchardParent();
    PreparedOrchardParent(const PreparedOrchardParent&) = delete;
    PreparedOrchardParent& operator=(const PreparedOrchardParent&) = delete;
    const storage::LegacyRetirementRecord& Record() const;
    // Immutable preparation nodes only; this owner publishes no canonical root.
    const PreparedOrchardCatalog& Catalog() const;
    // Optional independently completed prefix below activation parent. Absence
    // throws; this is historical proof material, not a retirement certificate.
    const HistoricalCompactReplay& HistoricalPrefix() const;
    const PreparedHistoricalCatalog& HistoricalCatalog() const;
    const PreparedHistoricalCatalogRange& HistoricalRange() const;
private:
    friend class ChainstateService;
    struct State;
    explicit PreparedOrchardParent(std::unique_ptr<State> state);
    const std::unique_ptr<State> state_;
};
}
