// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "din_json.h"

namespace dinero { class ConfigService; }

namespace dinero::rpc {

din::Json HandleSeederStatus(dinero::ConfigService* config);
din::Json HandleSeederStart(dinero::ConfigService* config,
                            const din::Json& params);
din::Json HandleSeederStop(dinero::ConfigService* config);

}  // namespace dinero::rpc
