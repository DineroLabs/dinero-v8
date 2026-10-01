#pragma once
#include <memory>
namespace dinero { class ChainstateService; namespace pool {
class PoolManager;
// Actual legacy selected callbacks and periodic maintenance share one owner.
// No wallet or network callback runs under selected/manager/SQLite ownership.
class CanonicalPoolMaintenance final {
public:
    static void Reconcile(const std::shared_ptr<ChainstateService>&,PoolManager&);
    static void ReconcileSelected(ChainstateService&,PoolManager&);
};
} }
