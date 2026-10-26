#pragma once

#include "storage/orchard_catalog_state.h"
#include "storage/orchard_state.h"
#include "storage/legacy_retirement.h"
#include "consensus/orchard_state_transition.h"
#include "consensus/snapshot_binding.h"
#include "consensus/utxo_snapshot.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/orchard_snapshot_catalog.h"
#include <istream>
#include <functional>
#include <ostream>
#include <map>
#include <span>
#include <vector>

namespace dinero::consensus {
// v6 section codec. Parsing/framing is deliberately separate from
// selected-header authentication and canonical publication. A decoded section
// is untrusted input and cannot enroll a compact chainstate owner.
enum class OrchardSnapshotProfile : uint8_t { Full=1, Compact=2 };
struct OrchardSnapshotRawLess {
    bool operator()(const uint256& a,const uint256& b) const noexcept {
        return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end());
    }
};
struct OrchardSnapshotAnchor {
    uint256 anchor;
    uint64_t references=0;
    bool operator==(const OrchardSnapshotAnchor&) const = default;
};
struct OrchardSnapshotSection {
    OrchardSnapshotProfile profile=OrchardSnapshotProfile::Full;
    storage::OrchardStoredState state;
    storage::LegacyRetirementState retirement;
    // Strict raw-byte order, with no duplicate membership or zero references.
    // Nullifier owners and pre-base undo are intentionally absent; import must
    // establish a reorg floor, then give imported nullifiers the base owner.
    std::vector<uint256> nullifiers;
    std::vector<OrchardSnapshotAnchor> anchors;
    storage::catalog::State parent_catalog,base_catalog;
    // Exactly the union reachable from the six parent/base roots. No historical
    // abandoned nodes, partial path-only exports, or unreferenced payloads.
    std::map<uint256,std::string,OrchardSnapshotRawLess> nodes;
    // Production capture/import/restart use the sealed disk inventory. The
    // in-memory map above remains available for small isolated codec fixtures;
    // using both inventories at once is invalid, never a fallback/overlay.
    std::shared_ptr<const OrchardSnapshotCatalog> disk_nodes;
    // Full serialized base body is necessary to derive the base catalog from
    // its committed parent without introducing a coinbase self-reference.
    std::vector<uint8_t> base_body;
    // Full profile: serialized forest; compact profile: canonical base stump.
    std::vector<uint8_t> accumulator;
};
uint64_t OrchardSnapshotNodeCount(const OrchardSnapshotSection&);
std::string ReadOrchardSnapshotNode(const OrchardSnapshotSection&,const uint256&);
void ForEachOrchardSnapshotNode(const OrchardSnapshotSection&,
    const std::function<void(const uint256&,const std::string&)>&);
inline constexpr uint32_t ORCHARD_SNAPSHOT_SECTION_VERSION=1;
inline constexpr uint64_t ORCHARD_SNAPSHOT_MAX_SECTION_BYTES=uint64_t(1)<<30;
inline constexpr uint64_t ORCHARD_SNAPSHOT_MAX_BASE_BODY_BYTES=32*1024*1024;
inline constexpr uint64_t ORCHARD_SNAPSHOT_MAX_NODE_BYTES=82;

// All functions throw on malformed, noncanonical, incomplete or oversized
// input. Bounds are storage work policy, not new consensus limits. No caller
// state is mutated. Logical set digests match ChainDB's ONF1/OAN1 encodings.
[[nodiscard]] storage::OrchardCommitmentSets ComputeOrchardSnapshotSets(
    const OrchardSnapshotSection&);
void ValidateOrchardSnapshotSection(const OrchardSnapshotSection&);
// Canonical full-forest framing drops uncommitted internal cache cells and
// deleted leaf hashes. A complete live leaf must be present at every other
// position. Header/count bounds are screened before invoking forest rebuilds.
std::vector<uint8_t> CanonicalOrchardSnapshotForest(const std::vector<uint8_t>&);
UtreexoForest DecodeOrchardSnapshotForest(const std::vector<uint8_t>&);
[[nodiscard]] std::vector<uint8_t> EncodeOrchardSnapshotSection(const OrchardSnapshotSection&);
[[nodiscard]] OrchardSnapshotSection DecodeOrchardSnapshotSection(std::span<const uint8_t>);
// Identical canonical bytes without a section-sized temporary. Callbacks must
// consume/fill the supplied span synchronously, throwing on short I/O. Readers
// remain untrusted until outer checksums and chain authentication both finish.
using OrchardSnapshotSink=std::function<void(std::span<const uint8_t>)>;
using OrchardSnapshotSource=std::function<void(std::span<uint8_t>)>;
uint64_t OrchardSnapshotSectionEncodedSize(const OrchardSnapshotSection&);
uint64_t StreamOrchardSnapshotSection(const OrchardSnapshotSection&,const OrchardSnapshotSink&);
uint256 HashOrchardSnapshotSection(const OrchardSnapshotSection&);
OrchardSnapshotSection ReadOrchardSnapshotSection(uint64_t bytes,const OrchardSnapshotSource&,
    const std::filesystem::path& work_directory={});

// Caller supplies copies from its validated selected best-work header chain
// and retains/rechecks that selection through publication. These are chain
// inputs, never values read from the snapshot. The verifier checks the entire
// base body, DNORST02, exact parent/base catalog transition and accumulator.
// Full-profile UTXO record/forest completeness and durable installation belong
// to the outer container importer. Passing this check alone cannot enroll a
// live compact owner or mark history validated.
void VerifyOrchardSnapshotSection(const OrchardSnapshotSection&,OrchardSnapshotProfile expected_profile,
    const OrchardBlockContext&,const BlockHeader& parent,const BlockHeader& base,
    const arith_uint256& parent_work,const arith_uint256& base_work,
    const std::optional<uint256>& selected_ancestor_at_base,uint32_t best_header_height,
    uint32_t burial_depth,const OrchardBranchMtpLookup&,bool require_witness_commitment);

using OrchardSnapshotCoins=std::unordered_map<OutPoint,UTXOEntry>;
struct OrchardSnapshotFile {
    SnapshotMetadata metadata;
    OrchardSnapshotSection orchard;
    OrchardSnapshotCoins coins;
    uint256 checksum;
    uint64_t bytes=0;
};
// Streamed outer container: canonical 68-byte header, canonical v5 transparent
// coin records, u64 section length, ORCH section, SHA256 footer. Compact carries
// zero coins. Parsing is not chain authentication. The caller screens transport,
// selected ancestry, activation and profile before publication.
OrchardSnapshotFile ReadOrchardSnapshotFile(std::istream&,uint64_t maximum_bytes,
    OrchardSnapshotProfile expected_profile,const std::filesystem::path& work_directory={});
SnapshotMetadata ReadOrchardSnapshotMetadata(std::istream&,uint64_t maximum_bytes);
void WriteOrchardSnapshotFile(std::ostream&,const SnapshotMetadata&,
    const OrchardSnapshotSection&,const OrchardSnapshotCoins&);
void VerifyOrchardSnapshotCoins(const OrchardSnapshotSection&,const OrchardSnapshotCoins&);
// Read-only COMPLETE parent-coin/checkpoint audit against the transfer's
// authenticated parent catalog. Does not fabricate an Orchard state/event,
// install storage, or certify execution of the imported prehistory.
void VerifyOrchardSnapshotParentCoins(const OrchardSnapshotSection&,
    const std::vector<uint8_t>& parent_forest,const OrchardSnapshotCoins&);
SnapshotExportResult WriteOrchardSnapshotFileAtomically(const std::filesystem::path&,
    const SnapshotMetadata&,const OrchardSnapshotSection&,const OrchardSnapshotCoins&);
} // namespace dinero::consensus
