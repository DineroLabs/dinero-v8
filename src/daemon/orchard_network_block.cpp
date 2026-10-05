#include "daemon/orchard_network_block.h"
#include "daemon/daemon_context.h"
#include "daemon/block_relay_manager.h"
#include "daemon/services/block_ingress_service.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/block_download_scheduler.h"
#include "consensus/block_index.h"
#include "consensus/header_chain.h"
#include "consensus/orchard_profile.h"
#include "p2p/block_download_scheduler.h"
#include "consensus/limits.h"
#include "util/hex.h"
#include <optional>

namespace dinero {
NetworkBlockClassification ClassifyNetworkBlock(std::span<const uint8_t> bytes) {
    try {
        if (!consensus::OrchardProfileConfigurationValid(Params())) return {};
        // Preserve historical network handling while the release is unset.
        if (Params().orchard_activation_height == UINT32_MAX)
            return {NetworkBlockFamily::Historical,{},0};
        if (bytes.size()<128 || bytes.size()>consensus::MAX_BLOCK_WEIGHT) return {};
        const auto header=BlockHeader::Deserialize(std::vector<uint8_t>(bytes.begin(),bytes.begin()+128));
        const auto* context=DaemonContext::instance();
        if (!header || !context) return {};
        const auto source=context->chainstate;
        const auto headers=context->header_chain;
        if (!source) return {};
        std::optional<uint32_t> height;
        {
            auto selected=source->AcquireBlockIngressActivationLock();
            auto* db=source->GetChainDB();
            if (!db) return {};
            const auto parent=db->getBlockHeight(header->prev_block_hash);
            if (parent.ok()) {
                if (*parent<0 || *parent>=INT32_MAX) return {};
                height=static_cast<uint32_t>(*parent)+1;
            } else if (parent.status()!=Status::NotFound) return {};
        }
        // Header-first downloads can precede canonical storage of the parent.
        // Copy only value fields; never follow a selector-owned parent pointer.
        if (headers) {
            consensus::HeaderIndexEntry child{};
            if (headers->GetHeaderCopy(header->GetHash(),child)) {
                if (child.hash!=header->GetHash() || child.header.GetHash()!=child.hash ||
                    child.prev_hash!=header->prev_block_hash || child.height==0 ||
                    child.height>uint32_t(INT32_MAX) || (height && *height!=child.height)) return {};
                height=child.height;
            }
        }
        if (!height || DaemonContext::instance()!=context || context->chainstate!=source ||
            context->header_chain!=headers) return {};
        return {consensus::OrchardActiveForHeight(Params(),*height)
            ? NetworkBlockFamily::Orchard : NetworkBlockFamily::Historical,*header,*height};
    } catch (...) { return {}; }
}

OrchardNetworkDisposition ReceiveOrchardNetworkBlock(
    const std::string& peer,const std::vector<uint8_t>& bytes,bool stateless) {
    try {
        const auto block=ClassifyNetworkBlock(bytes);
        if (stateless || block.family!=NetworkBlockFamily::Orchard)
            return OrchardNetworkDisposition::Refused;
        const auto* context=DaemonContext::instance();
        if (!context) return OrchardNetworkDisposition::Refused;
        const auto source=context->chainstate;
        const auto headers=context->header_chain;
        const auto downloads=context->block_download;
        const auto relay=context->block_relay;
        if (!source || !relay) return OrchardNetworkDisposition::Refused;
        const auto hash=block.header.GetHash();
        const bool known=downloads && downloads->IsBlockKnown(hash);
        const bool requested=relay->IsBlockDownloadInFlight(hash);
        bool backlog=false;
        if (headers) {
            consensus::HeaderIndexEntry best{};
            const bool have_best=headers->GetBestHeaderCopy(best);
            auto selected=source->AcquireBlockIngressActivationLock();
            const auto* tip=source->GetActiveTip();
            if (!tip || tip->height<0) return OrchardNetworkDisposition::Refused;
            backlog=have_best && best.height>static_cast<uint32_t>(tip->height);
        }
        const bool syncing=downloads && (!downloads->IsFullySynchronized() || backlog);
        if (syncing && !known && !requested) return OrchardNetworkDisposition::Refused;
        if (DaemonContext::instance()!=context || context->chainstate!=source ||
            context->header_chain!=headers || context->block_download!=downloads ||
            context->block_relay!=relay) return OrchardNetworkDisposition::Refused;
        if (known) {
            if (!downloads->OnOrchardBlockReceived(bytes)) return OrchardNetworkDisposition::Refused;
            // Storage is an honest partial result even if a later drain fails.
            try { downloads->Tick(); } catch (...) {}
            return OrchardNetworkDisposition::Stored;
        }
        return relay->ReceiveOrchardBlock(peer,bytes);
    } catch (...) { return OrchardNetworkDisposition::Refused; }
}

OrchardNetworkDisposition SubmitDownloadedOrchardBlock(
    const std::shared_ptr<ChainstateService>& source,
    const std::shared_ptr<BlockIngressService>& ingress,
    const std::shared_ptr<BlockDownloadScheduler>& parallel,
    const std::vector<uint8_t>& bytes,const uint256& hash,uint32_t height) {
    auto disposition=OrchardNetworkDisposition::Refused;
    try {
        const auto* context=DaemonContext::instance();
        if (!source || !ingress || !ingress->IsHealthy() || !context ||
            context->chainstate!=source || context->block_ingress!=ingress.get() ||
            context->parallel_block_download!=parallel) return OrchardNetworkDisposition::Refused;
        const auto classified=ClassifyNetworkBlock(bytes);
        if (classified.family!=NetworkBlockFamily::Orchard ||
            classified.header.GetHash()!=hash || classified.height!=height) return OrchardNetworkDisposition::Refused;
        const auto result=ingress->SubmitHex(util::hex(bytes),BlockOrigin::P2P);
        if (result.block_hash!=hash || result.height!=height) return OrchardNetworkDisposition::Refused;
        if (result.retained() && !result.connected && !result.relayed)
            return OrchardNetworkDisposition::Stored;
        if (!result.accepted() || !result.connected) return OrchardNetworkDisposition::Refused;
        disposition=OrchardNetworkDisposition::Connected;
        if (parallel) parallel->notifyBlockReceived(hash);
    } catch (...) { /* Terminal bookkeeping cannot undo canonical acceptance. */ }
    return disposition;
}
bool AcceptDownloadedOrchardBlock(
    const std::shared_ptr<ChainstateService>& source,
    const std::shared_ptr<BlockIngressService>& ingress,
    const std::shared_ptr<BlockDownloadScheduler>& parallel,
    const std::vector<uint8_t>& bytes,const uint256& hash,uint32_t height) {
    return SubmitDownloadedOrchardBlock(source,ingress,parallel,bytes,hash,height)==OrchardNetworkDisposition::Connected;
}
bool AnnounceAcceptedOrchardBlock(const std::shared_ptr<ChainstateService>& source,
                                 const BlockAcceptResult& result) noexcept {
    try {
        const auto* context=DaemonContext::instance();
        if (!source || !result.accepted() || !result.connected || result.height>UINT32_MAX ||
            !context || context->chainstate!=source) return false;
        const auto relay=context->block_relay;
        return relay && relay->AnnounceOrchardBlock(result.block_hash,static_cast<uint32_t>(result.height));
    } catch (...) { return false; }
}
} // namespace dinero
