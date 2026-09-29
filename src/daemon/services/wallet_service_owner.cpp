#include "daemon/services/wallet_service.h"
#include <stdexcept>

namespace dinero {
WalletService::WalletUse::WalletUse(const WalletService& service,std::shared_ptr<WalletService> retained)
    :service_(service),retained_(std::move(retained)) {
    std::lock_guard<std::mutex> lock(service_.operation_mutex_);
    const auto current=service_.operations_by_thread_.find(thread_);
    if ((!service_.accepting_ && current==service_.operations_by_thread_.end()) || !service_.wallet_mgr_)
        throw std::runtime_error("Wallet service unavailable");
    ++service_.operations_by_thread_[thread_];++service_.active_operations_;
    wallet_=service_.wallet_mgr_.get();
}
WalletService::WalletUse::~WalletUse() noexcept {
    if (thread_!=std::this_thread::get_id()) std::terminate();
    std::lock_guard<std::mutex> lock(service_.operation_mutex_);
    auto current=service_.operations_by_thread_.find(thread_);
    if (current==service_.operations_by_thread_.end() || !current->second || !service_.active_operations_)
        std::terminate();
    if (!--current->second) service_.operations_by_thread_.erase(current);
    --service_.active_operations_;service_.operation_changed_.notify_all();
}
WalletManager& WalletService::WalletUse::Wallet() const {
    if (thread_!=std::this_thread::get_id()) throw std::logic_error("Wallet operation is thread-affine");
    return *wallet_;
}
std::unique_ptr<WalletService::WalletUse> WalletService::AcquireWalletUse(std::shared_ptr<WalletService> service) {
    if (!service) throw std::runtime_error("Wallet service unavailable");
    const auto* ptr=service.get();return std::unique_ptr<WalletUse>(new WalletUse(*ptr,std::move(service)));
}
std::unique_ptr<WalletService::WalletUse> WalletService::BorrowWalletUse() const {
    return std::unique_ptr<WalletUse>(new WalletUse(*this,{}));
}

} // namespace dinero
