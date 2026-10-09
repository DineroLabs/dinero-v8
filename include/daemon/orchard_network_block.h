#pragma once
#include "primitives/block.h"
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace dinero {
struct BlockAcceptResult;
class ChainstateService;
class BlockIngressService;
class BlockDownloadScheduler;
enum class InventoryType : uint32_t;
// Request format only, never body validation or transport completion. Legacy
// stateless history keeps its proof-bearing format. Active Orchard requires
// a known exact header before requesting bytes. Storage ownership is checked
// by the existing receive/ingress path, never under a request queue lock.
std::optional<InventoryType> SelectBlockRequestInventory(const uint256& hash,
    bool stateless, bool store_only_backfill=false,
    std::optional<uint32_t> expected_height=std::nullopt);
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
OrchardNetworkDisposition SubmitDownloadedOrchardBlock(
    const std::shared_ptr<ChainstateService>& source,
    const std::shared_ptr<BlockIngressService>& ingress,
    const std::shared_ptr<BlockDownloadScheduler>& parallel,
    const std::vector<uint8_t>& bytes, const uint256& hash, uint32_t height);
// Compact drain wrapper: checks the real started compact storage owner before
// entering the existing canonical queue. Stored is still not acknowledgment.
OrchardNetworkDisposition SubmitDownloadedCompactOrchardBlock(
    const std::shared_ptr<ChainstateService>& source,
    const std::shared_ptr<BlockIngressService>& ingress,
    const std::shared_ptr<BlockDownloadScheduler>& parallel,
    const std::vector<uint8_t>& bytes, const uint256& hash, uint32_t height);
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
