#pragma once
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

namespace dinero {
class WalletService;class MempoolService;
// Temporary admission/drain ownership, not wallet/transaction authorization.
// Abort resumes the SAME live owner; CommitToStop leaves admission closed and
// releases the service for ordinary Stop. No owner replacement is permitted
// while an uncommitted fence can borrow its service's read-only state.
class ReopenServiceFence final {
public:
    ~ReopenServiceFence() noexcept {if(release_)Release(true);}
    ReopenServiceFence(const ReopenServiceFence&)=delete;
    ReopenServiceFence& operator=(const ReopenServiceFence&)=delete;
    void CommitToStop(){Check();Release(false);}
private:
    friend class WalletService;friend class MempoolService;
    ReopenServiceFence(const void* owner,std::function<void(bool)> release)
        :owner_(owner),release_(std::move(release)){}
    void Check()const {
        if(thread_!=std::this_thread::get_id()||!release_)throw std::logic_error("Inactive or foreign reopen fence");
    }
    void RequireOwner(const void* owner)const {Check();if(owner!=owner_)throw std::logic_error("Wrong reopen service owner");}
    void Release(bool resume){
        Check();auto release=std::move(release_);
        // A moved-from std::function is valid but need not be empty. Clear
        // explicitly before invoking the one-shot callback or publishing Stop.
        release_=nullptr;release(resume);
    }
    const void* const owner_;const std::thread::id thread_=std::this_thread::get_id();
    std::function<void(bool)> release_;
};
} // namespace dinero
