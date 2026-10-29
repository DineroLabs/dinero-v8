#include "daemon/block_relay_manager.h"
#include "daemon/services/block_ingress_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/daemon_context.h"
#include "consensus/orchard_profile.h"
#include "consensus/limits.h"
#include "util/hex.h"
#include <limits>
#include <cstdlib>

namespace dinero {
OrchardNetworkDisposition BlockRelayManager::ReceiveOrchardBlock(const std::string& peer_address,
                                          const std::vector<uint8_t>& bytes) {
    auto disposition = OrchardNetworkDisposition::Refused;
    try {
        const auto source = orchard_block_source_.lock();
        const auto ingress = orchard_block_ingress_.lock();
        const auto* context = DaemonContext::instance();
        if (!source || !ingress || !ingress->IsHealthy() || !context || context->chainstate != source ||
            context->block_ingress != ingress.get() || bytes.size() < 128 ||
            bytes.size() > consensus::MAX_BLOCK_WEIGHT) return OrchardNetworkDisposition::Refused;
        const auto header = BlockHeader::Deserialize(std::vector<uint8_t>(bytes.begin(),bytes.begin()+128));
        if (!header) return OrchardNetworkDisposition::Refused;
        const auto hash = header->GetHash();
        uint32_t height = 0;
        {
            auto selected = source->AcquireBlockIngressActivationLock();
            auto* db = source->GetChainDB();
            if (!db || !consensus::OrchardProfileConfigurationValid(Params())) return OrchardNetworkDisposition::Refused;
            const auto parent = db->getBlockHeight(header->prev_block_hash);
            if (!parent.ok() || *parent < 0 || *parent >= INT32_MAX) return OrchardNetworkDisposition::Refused;
            height = static_cast<uint32_t>(*parent) + 1;
            if (!consensus::OrchardActiveForHeight(Params(),height)) return OrchardNetworkDisposition::Refused;
        }
        // The selected lock is released before queue waiting. The actual queue
        // and canonical owner recheck the current context, profile and parent.
        const auto result = ingress->SubmitHex(util::hex(bytes),BlockOrigin::P2P);
        if (result.block_hash != hash || result.height != height) return OrchardNetworkDisposition::Refused;
        if (result.retained() && !result.connected && !result.relayed)
            return OrchardNetworkDisposition::Stored;
        if (!result.accepted() || !result.connected) return OrchardNetworkDisposition::Refused;
        disposition = OrchardNetworkDisposition::Connected;

        // These are retryable transport/telemetry effects after consensus. A
        // failure here must not turn committed acceptance into a refusal.
        ConsumeBlockRequest(hash);
        if (download_scheduler_) download_scheduler_->notifyBlockReceived(hash);
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(seen_blocks_mutex_);
            first = seen_blocks_.insert(hash).second;
        }
        if (first) {
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.blocks_seen;
                ++stats_.blocks_validated;
            }
            RecordBlockDelivery(peer_address);
        }
    } catch (...) { /* A missing owner or operational refusal grants no peer penalty. */ }
    return disposition;
}
bool BlockRelayManager::HandleOrchardBlock(const std::string& peer_address,
                                          const std::vector<uint8_t>& bytes) {
    return ReceiveOrchardBlock(peer_address,bytes)==OrchardNetworkDisposition::Connected;
}
bool BlockRelayManager::AnnounceOrchardBlock(const uint256& hash,uint32_t height) noexcept {
    bool handed=false;
    try {
        const auto source=orchard_block_source_.lock();
        const auto* context=DaemonContext::instance();
        const auto send=send_message_callback_;
        if (!source || !send || !context || context->chainstate!=source ||
            context->block_relay.get()!=this) return false;
        const char* suppressed=std::getenv("DINERO_TEST_SUPPRESS_ANNOUNCEMENTS");
        if (Params().name=="regtest" && suppressed && std::string(suppressed)=="1") return false;
        const auto captured=source->getOrchardAnnouncementSnapshot(hash,height);
        if (!captured.ok()) return false;
        const auto payload=SerializeInv(hash);
        if (DaemonContext::instance()!=context || context->chainstate!=source ||
            context->block_relay.get()!=this) return false;
        {
            std::lock_guard<std::mutex> lock(orchard_announcement_mutex_);
            if (orchard_announcement_pending_) return false;
            if (orchard_announced_source_.lock()==source &&
                orchard_announced_sequence_==captured->event_sequence &&
                orchard_announced_digest_==captured->event_digest) return true;
            orchard_announcement_pending_=true;
        }
        struct PendingReset {
            std::mutex& mutex;bool& pending;
            ~PendingReset(){std::lock_guard<std::mutex> lock(mutex);pending=false;}
        } reset{orchard_announcement_mutex_,orchard_announcement_pending_};
        // No chain, relay or mempool mutex is held across external transport.
        // This is an as-of snapshot; a subsequent reorg does not unsend it.
        send("","inv_all",payload);handed=true;
        {
            std::lock_guard<std::mutex> lock(orchard_announcement_mutex_);
            orchard_announced_sequence_=captured->event_sequence;
            orchard_announced_digest_=captured->event_digest;
            orchard_announced_source_=source;
        }
        {std::lock_guard<std::mutex> lock(stats_mutex_);++stats_.blocks_relayed;}
    } catch (...) { /* A failed transport handoff remains explicitly retryable. */ }
    return handed;
}
} // namespace dinero
