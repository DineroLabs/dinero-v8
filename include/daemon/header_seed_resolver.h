#pragma once

#include "common/status.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

namespace dinero {

class ChainDB;

namespace consensus {
class HeaderChainSelector;
}

namespace daemon {

enum class StartupHeaderSource {
    ChainDB,
    HeaderChainSelector,
};

struct StartupHeaderResolution {
    BlockHeader header;
    StartupHeaderSource source{StartupHeaderSource::ChainDB};
};

/**
 * Resolve an active-chain header for HeaderChainSelector startup seeding.
 *
 * A normal full node stores headers in ChainDB. An AssumeUTXO consumer can
 * instead know its pre-base chain only through the separately persisted
 * HeaderChainSelector store. Promotion does not manufacture the missing
 * ChainDB header rows, so restart must accept that verified header source.
 *
 * ChainDB errors other than NotFound are returned unchanged; the selector
 * fallback must not conceal database corruption or serialization failures.
 */
StatusOr<StartupHeaderResolution> ResolveStartupHeader(
    const ChainDB& chain_db,
    const consensus::HeaderChainSelector* header_chain,
    const uint256& hash);

}  // namespace daemon
}  // namespace dinero
