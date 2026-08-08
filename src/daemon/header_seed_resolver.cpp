#include "daemon/header_seed_resolver.h"

#include "consensus/header_chain.h"
#include "storage/chain_db.h"

namespace dinero::daemon {

StatusOr<StartupHeaderResolution> ResolveStartupHeader(
    const ChainDB& chain_db,
    const consensus::HeaderChainSelector* header_chain,
    const uint256& hash) {
    auto chain_db_header = chain_db.getHeader(hash);
    if (chain_db_header.status() == Status::Ok) {
        return StartupHeaderResolution{
            std::move(chain_db_header.value()), StartupHeaderSource::ChainDB};
    }
    if (chain_db_header.status() != Status::NotFound) {
        return chain_db_header.status();
    }

    consensus::HeaderIndexEntry selector_entry;
    if (header_chain != nullptr &&
        header_chain->GetHeaderCopy(hash, selector_entry) &&
        selector_entry.header.GetHash() == hash) {
        return StartupHeaderResolution{
            std::move(selector_entry.header),
            StartupHeaderSource::HeaderChainSelector};
    }

    return Status::NotFound;
}

}  // namespace dinero::daemon
