#pragma once

// DINERO_RELEASE mode: ALWAYS use vendored JsonCpp (ABI compatibility)
// Use relative path to force vendored headers (bypasses -isystem /opt/homebrew/include)
#include "../../third_party/jsoncpp/include/json/json.h"
