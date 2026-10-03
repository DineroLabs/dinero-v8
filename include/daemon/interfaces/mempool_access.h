#pragma once
#include <functional>
#include <memory>

namespace dinero {
class Mempool;

// An operation-scoped owner, acquired before pool locks and retained until
// synchronous work and its callbacks finish. Implementations may be thread-affine;
// acquire, use and destroy on the same thread. No pool reference may escape it.
class MempoolAccess {
public:
    virtual ~MempoolAccess() noexcept = default;
    virtual Mempool& Pool() const = 0;
};
using MempoolAccessFactory = std::function<std::unique_ptr<MempoolAccess>()>;
}
