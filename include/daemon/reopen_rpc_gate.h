#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

namespace dinero {
class DaemonApp;
// Admission only, never wallet or chainstate authority. The host retains this
// shared epoch across old-App destruction and new-App publication. It must
// begin a pause BEFORE closing service admission, and finish only after either
// aborting back to the SAME owners or making the replacement fully queryable.
// Every RPC samples before AND after dispatch: an entire pause/abort between
// those samples must not turn a transient queue error into a permanent refusal.
class ReopenRpcGate final {
public:
    uint64_t Snapshot()const noexcept{return epoch_.load(std::memory_order_acquire);}
    bool Allows(uint64_t epoch)const noexcept{return !(epoch&1)&&Snapshot()==epoch;}
    class Pause final {
    public:
        ~Pause()noexcept {
            if(auto_resume_&&active_)Resume();
        }
        Pause(const Pause&)=delete;Pause& operator=(const Pause&)=delete;
    private:
        friend class ReopenRpcGate;
        friend class DaemonApp;
        void Check()const {
            if(thread_!=std::this_thread::get_id()||!active_||gate_->Snapshot()!=epoch_)
                throw std::logic_error("Inactive or foreign RPC reopen pause");
        }
        // An irreversible close must fail CLOSED if its host abandons the
        // receipt. Only a successfully initialized replacement can Resume.
        void HoldClosed(){Check();auto_resume_=false;}
        void Resume(){Check();active_=false;auto_resume_=false;gate_->epoch_.store(epoch_+1,std::memory_order_release);}
        explicit Pause(std::shared_ptr<ReopenRpcGate> gate):gate_(std::move(gate)){
            if(!gate_)throw std::logic_error("Missing RPC reopen gate");
            auto old=gate_->Snapshot();
            if((old&1)||old>std::numeric_limits<uint64_t>::max()-2||
                !gate_->epoch_.compare_exchange_strong(old,old+1,std::memory_order_acq_rel))
                throw std::logic_error("RPC reopen pause already active or exhausted");
            epoch_=old+1;
        }
        const std::shared_ptr<ReopenRpcGate> gate_;uint64_t epoch_=0;bool active_=true,auto_resume_=true;
        const std::thread::id thread_=std::this_thread::get_id();
    };
    static std::unique_ptr<Pause> Begin(std::shared_ptr<ReopenRpcGate> gate){
        return std::unique_ptr<Pause>(new Pause(std::move(gate)));
    }
private:
    std::atomic<uint64_t> epoch_{0};
};
} // namespace dinero
