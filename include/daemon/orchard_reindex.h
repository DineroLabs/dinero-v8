#pragma once
#include "consensus/reindexer.h"
#include "daemon/runtime_block_outbox.h"

namespace dinero {
class ChainWriteToken;
// Startup-only owner. The caller holds the datadir guard, keeps Params fixed,
// and has not started services. Source remains open and unmodified; candidate
// is unpublished and is promoted only by DaemonApp's existing cohort journal.
class OrchardReindexOwner final {
public:
    static StatusOr<consensus::BlockReindexer::Stats> Run(
        const ChainDB& source, ChainDB& candidate, const ChainWriteToken&,
        BlockStorage&, const std::filesystem::path& datadir,
        const std::filesystem::path& candidate_path,
        const consensus::BlockReindexer::Config&);
private:
    // Only Run may carry an independently replayed prefix into a replacement
    // historical candidate. These bytes preserve existing consumer identities;
    // copying them does not establish consensus validity or consumer readiness.
    static void CopyValidatedOutboxPrefix(const ChainDB& source, ChainDB& candidate,
        const ChainWriteToken&, const consensus::OrchardBlockContext&,
        RuntimeOutboxCursor through, RuntimeOutboxCursor source_head);
};
} // namespace dinero
