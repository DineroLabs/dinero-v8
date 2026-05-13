/**
 * Stub implementations for MiningManager tests
 *
 * These minimal stubs allow testing MiningManager API surface
 * without linking the entire dinerod executable.
 */

#include <string>
#include <map>
#include "common/logger.h"
#include "metrics/metrics_registry.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/mempool.h"
#include "storage/chain_db.h"

namespace dinero {

// ===================================================================
// Logger stubs
// ===================================================================

Logger g_logger;

void Logger::info(const std::string& msg) {}
void Logger::debug(const std::string& msg) {}
void Logger::error(const std::string& msg) {}
void Logger::warning(const std::string& msg) {}

// ===================================================================
// MetricsRegistry stubs
// ===================================================================

namespace metrics {

void MetricsRegistry::SetMiningHashrate(double hashrate, const LabelMap& labels) {}
void MetricsRegistry::SetMiningThreads(int threads, const LabelMap& labels) {}
void MetricsRegistry::SetMiningJobHeight(uint64_t height, const LabelMap& labels) {}
void MetricsRegistry::SetMiningCurrentBits(uint32_t bits, const LabelMap& labels) {}
void MetricsRegistry::IncrementMiningBlocksFound(const LabelMap& labels) {}

} // namespace metrics

} // namespace dinero
