#include "daemon/runtime_notification_composition.h"
#include "daemon/runtime_delivery_worker.h"
#include "pool/pool_manager.h"
#include <stdexcept>
#include <utility>

namespace dinero {
namespace {
constexpr size_t Count=static_cast<size_t>(RuntimeConsumerKind::Count);
struct ComposedBlock final : PreparedRuntimeBlockNotifications {
    // Owners precede tokens so tokens are destroyed first.
    std::array<std::shared_ptr<RuntimeBlockNotifications>,Count> owners;
    std::array<std::unique_ptr<PreparedRuntimeBlockNotifications>,Count> prepared;
    bool published=false;
    void PublishAfterCommit() noexcept override {
        if(published)std::terminate();
        published=true;
        for(auto& consumer:prepared)if(consumer)consumer->PublishAfterCommit();
    }
};
struct ComposedReorg final : PreparedRuntimeReorgNotifications {
    std::array<std::shared_ptr<RuntimeBlockNotifications>,Count> owners;
    std::array<std::unique_ptr<PreparedRuntimeReorgNotifications>,Count> prepared;
    bool finished=false;
    ~ComposedReorg() override {if(!finished)Finish({});}
    void Finish(RuntimeReorgProgress progress) noexcept override {
        if(finished)std::terminate();
        finished=true;
        for(auto& consumer:prepared)if(consumer)consumer->Finish(progress);
    }
};
class WalletNotifications final : public RuntimeBlockNotifications {
    struct Block final : PreparedRuntimeBlockNotifications {
        explicit Block(RuntimeDeliveryWorker::WakeHandle value):worker(std::move(value)){}
        RuntimeDeliveryWorker::WakeHandle worker;
        void PublishAfterCommit() noexcept override {worker.RequestReplay();}
    };
    struct Reorg final : PreparedRuntimeReorgNotifications {
        explicit Reorg(RuntimeDeliveryWorker::WakeHandle value):worker(std::move(value)){}
        RuntimeDeliveryWorker::WakeHandle worker;
        void Finish(RuntimeReorgProgress) noexcept override {worker.RequestReplay();}
    };
public:
    explicit WalletNotifications(const RuntimeDeliveryWorker& worker):worker_(worker.CaptureWakeHandle()) {}
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
            const RuntimeBlockBody&,uint32_t,RuntimeBlockDirection) override {
        if(!worker_.Running())return {};
        return std::make_unique<Block>(worker_);
    }
    std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(
            std::shared_ptr<const RuntimeReorgPlan> plan) override {
        if(!plan)return {};
        if(!worker_.Running())return {};
        return std::make_unique<Reorg>(worker_);
    }
private:
    RuntimeDeliveryWorker::WakeHandle worker_;
};
class PoolNotifications final : public RuntimeBlockNotifications {
    struct Block final : PreparedRuntimeBlockNotifications {
        explicit Block(pool::PoolManager::MaintenanceWakeHandle value):worker(std::move(value)){}
        pool::PoolManager::MaintenanceWakeHandle worker;
        void PublishAfterCommit() noexcept override {worker.RequestReplay();}
    };
    struct Reorg final : PreparedRuntimeReorgNotifications {
        explicit Reorg(pool::PoolManager::MaintenanceWakeHandle value):worker(std::move(value)){}
        pool::PoolManager::MaintenanceWakeHandle worker;
        void Finish(RuntimeReorgProgress) noexcept override {worker.RequestReplay();}
    };
public:
    explicit PoolNotifications(const pool::PoolManager& worker):worker_(worker.CaptureMaintenanceWakeHandle()) {}
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
            const RuntimeBlockBody&,uint32_t,RuntimeBlockDirection) override {
        if(!worker_.Running())return {};
        return std::make_unique<Block>(worker_);
    }
    std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(
            std::shared_ptr<const RuntimeReorgPlan> plan) override {
        if(!plan)return {};
        if(!worker_.Running())return {};
        return std::make_unique<Reorg>(worker_);
    }
private:
    pool::PoolManager::MaintenanceWakeHandle worker_;
};
}
RuntimeNotificationComposition::RuntimeNotificationComposition(std::span<const RuntimeConsumerBinding> bindings) {
    if(bindings.size()!=Count)throw std::invalid_argument("Incomplete runtime consumer declaration");
    std::array<bool,Count> seen{};size_t active=0;
    for(const auto& binding:bindings) {
        const auto kind=static_cast<size_t>(binding.kind);
        if(kind>=Count || seen[kind])throw std::invalid_argument("Invalid or duplicate runtime consumer family");
        seen[kind]=true;
        if(binding.consumer) {
            for(const auto& prior:consumers_)if(prior==binding.consumer)
                throw std::invalid_argument("Runtime consumer registered in two families");
            ++active;
        }
        consumers_[kind]=binding.consumer;
    }
    if(!active)throw std::invalid_argument("Runtime consumer declaration has no active owner");
}
std::unique_ptr<PreparedRuntimeBlockNotifications> RuntimeNotificationComposition::Prepare(
        const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection direction) {
    auto result=std::make_unique<ComposedBlock>();result->owners=consumers_;
    for(size_t i=0;i<Count;++i)if(consumers_[i]) {
        result->prepared[i]=consumers_[i]->Prepare(body,height,direction);
        if(!result->prepared[i])return {};
    }
    return result;
}
std::unique_ptr<PreparedRuntimeReorgNotifications> RuntimeNotificationComposition::PrepareReorg(
        std::shared_ptr<const RuntimeReorgPlan> plan) {
    if(!plan)return {};
    auto result=std::make_unique<ComposedReorg>();result->owners=consumers_;
    for(size_t i=0;i<Count;++i)if(consumers_[i]) {
        result->prepared[i]=consumers_[i]->PrepareReorg(plan);
        if(!result->prepared[i])return {};
    }
    return result;
}
std::shared_ptr<RuntimeBlockNotifications> MakeRuntimeWalletNotifications(const RuntimeDeliveryWorker& worker) {
    return std::make_shared<WalletNotifications>(worker);
}
std::shared_ptr<RuntimeBlockNotifications> MakeRuntimePoolNotifications(const pool::PoolManager& manager) {
    return std::make_shared<PoolNotifications>(manager);
}
std::shared_ptr<RuntimeBlockNotifications> MakeRuntimeVaultNotifications(const RuntimeDeliveryWorker& worker) {
    return std::make_shared<WalletNotifications>(worker);
}
} // namespace dinero
