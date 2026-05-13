#pragma once

#include <string>
#include "compat/jsoncpp_compat.h"

namespace dinero {
namespace rpc {

/**
 * Get comprehensive storage information including backend status,
 * fallback configuration, and operational metrics
 */
Json::Value getstorageinfo(const Json::Value& params);

/**
 * Enhanced database statistics including schema version,
 * backend version, and build flags for forensics
 */
Json::Value getdbstats_enhanced(const Json::Value& params);

} // namespace rpc
} // namespace dinero
