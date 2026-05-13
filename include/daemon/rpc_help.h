#pragma once

#include "compat/jsoncpp_compat.h"

namespace dinero {

/**
 * Get comprehensive RPC help documentation
 * @return JSON object containing method documentation, error codes, and examples
 */
Json::Value GetRpcHelp();

} // namespace dinero
