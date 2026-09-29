#pragma once
#include "primitives/block.h"
#include <memory>
#include <span>
#include <string>

namespace dinero {
struct BlockAcceptResult;
class ChainstateService;
class BlockIngressService;
class BlockDownloadScheduler;
enum class NetworkBlockFamily { Unavailable, Historical, Orchard };
struct NetworkBlockClassification {
    NetworkBlockFamily family{NetworkBlockFamily::Unavailable};
    BlockHeader header{};
    uint32_t height{0};
};
// Routing classification only, never a validity or canonicality certificate.
// Unknown parent, inconsistent owners/profile or a failed read refuses.
NetworkBlockClassification ClassifyNetworkBlock(std::span<const uint8_t> bytes);
enum class OrchardNetworkDisposition { Refused, Stored, Connected };
// Called only for the typed family. Stored does not acknowledge acceptance.
OrchardNetworkDisposition ReceiveOrchardNetworkBlock(
    const std::string& peer, const std::vector<uint8_t>& bytes, bool stateless);
// Actual ordered-download callback: queue validation precedes terminal transport
// completion. Caller releases scheduler/selected locks before entering.
bool AcceptDownloadedOrchardBlock(
    const std::shared_ptr<ChainstateService>& source,
    const std::shared_ptr<BlockIngressService>& ingress,
    const std::shared_ptr<BlockDownloadScheduler>& parallel,
    const std::vector<uint8_t>& bytes, const uint256& hash, uint32_t height);
// Canonical acceptance remains independent of best-effort transport. Call only
// after releasing all selected locks; the relay capture enforces that boundary.
bool AnnounceAcceptedOrchardBlock(const std::shared_ptr<ChainstateService>& source,
                                 const BlockAcceptResult& result) noexcept;
} // namespace dinero
