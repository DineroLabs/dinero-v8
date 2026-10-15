#pragma once
// Light (stateless Utreexo, CSN) nodes cannot follow the chain past Orchard
// activation yet: the compact Orchard path has no first enrollment, no way to
// obtain the activation state without full history, and is kept off at
// startup (storage::RequireFullOrchardStorage). Instead of stalling silently
// at the activation parent, such a node says so through its status RPCs and
// log. Empty when the node is not affected.
#include "consensus/chainparams.h"
#include "daemon/orchard_storage_upgrade_state.h"

#include <cstdint>
#include <string>

namespace dinero::daemon {

inline std::string OrchardLightNodeNotice(const ChainParams& params, bool stateless, uint32_t tip_height) {
    const uint32_t activation = params.orchard_activation_height;
    if (!stateless || activation == UINT32_MAX || activation == 0) return {};
    const std::string stop = std::to_string(activation - 1), at = std::to_string(activation);
    if (uint64_t(tip_height) + 1 < activation)
        return "This light (stateless) node will stop at block " + stop + ": it cannot follow the chain past "
               "Orchard activation at block " + at + ". Switch to a full node, or use the wallet's remote "
               "mode, before then.";
    return "This light (stateless) node stopped at block " + stop + ": it cannot follow the chain past "
           "Orchard activation at block " + at + ". Switch to a full node, or use the wallet's remote mode.";
}

inline std::string OrchardStorageUpgradeNotice(const ChainParams& params, StorageUpgradeIssue issue) {
    if (issue == StorageUpgradeIssue::None || params.orchard_activation_height == UINT32_MAX ||
        params.orchard_activation_height == 0) return {};
    std::string notice = "This full node cannot follow Orchard activation at block " +
           std::to_string(params.orchard_activation_height) +
           " because its shielded-state storage upgrade was refused. ";
    switch (issue) {
        case StorageUpgradeIssue::InsufficientDisk:
            return notice + "Free space on the node's storage volume and restart before activation.";
        case StorageUpgradeIssue::OpenFileLimit:
            return notice + "Raise the node process's open-file limit (for example, ulimit -n 8192) "
                   "and restart before activation.";
        case StorageUpgradeIssue::Other:
            return notice + "Check the local daemon log, resolve the reported cause, and restart before activation.";
        case StorageUpgradeIssue::None: break;
    }
    return {};
}

}  // namespace dinero::daemon
