// Test-only stub for BlockRelayManager methods referenced by
// CompactBlockService.
//
// CompactBlockService::getBlocksProcessed / getReconstructionRate /
// getBandwidthSaved each guard with `if (ctx_ && ctx_->block_relay)`
// before calling the BlockRelayManager methods. The test target leaves
// ctx_->block_relay null, so these methods are NEVER invoked at runtime.
// They only need to exist as symbols so the linker can resolve the call
// sites in compact_block_service.cpp.
//
// Linking the real src/daemon/block_relay_manager.cpp would pull a deep
// transitive set (peer manager, consensus, network code) into the test
// binary — exactly what the boundary-smoke filter is trying to avoid.
// This stub is the minimal alternative.
//
// Production code never links this stub: it lives only in the
// test_compact_block_service target's sources list. If a future change
// to compact_block_service.cpp adds calls to MORE BlockRelayManager
// methods, the linker will fail on the test target and the new methods
// will need to be added here.

#include "daemon/block_relay_manager.h"

namespace dinero {

BlockRelayManager::Stats BlockRelayManager::GetStats() const {
    return Stats{};
}

double BlockRelayManager::GetCompactBlockSuccessRate() const {
    return 0.0;
}

} // namespace dinero
