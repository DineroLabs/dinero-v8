#include "daemon/orchard_light_node_notice.h"

#include <iostream>
#include <string>

int main() {
    using namespace dinero::daemon;
    dinero::ChainParams params{};
    params.orchard_activation_height = 150000;
    const auto require = [](bool ok, const char* label) {
        if (!ok) std::cerr << "FAIL " << label << '\n';
        return ok;
    };
    bool pass = true;
    pass &= require(OrchardStorageUpgradeNotice(params, StorageUpgradeIssue::None).empty(), "healthy notice");
    pass &= require(ClassifyStorageUpgradeIssue("not enough free disk: /private/secret") ==
                    StorageUpgradeIssue::InsufficientDisk, "disk classification");
    pass &= require(ClassifyStorageUpgradeIssue("IO error: Too many open files: /private/secret") ==
                    StorageUpgradeIssue::OpenFileLimit, "open-file classification");
    pass &= require(ClassifyStorageUpgradeIssue("unexpected ChainDB state: /private/secret") ==
                    StorageUpgradeIssue::Other, "other classification");
    for (const auto issue : {StorageUpgradeIssue::InsufficientDisk,
                             StorageUpgradeIssue::OpenFileLimit, StorageUpgradeIssue::Other}) {
        const auto notice = OrchardStorageUpgradeNotice(params, issue);
        pass &= require(notice.find("150000") != std::string::npos, "activation height");
        pass &= require(notice.find("/private/secret") == std::string::npos, "no raw path");
        pass &= require(notice.find("restart") != std::string::npos, "remediation");
    }
    pass &= require(std::string(StorageUpgradeIssueCode(StorageUpgradeIssue::OpenFileLimit)) == "open_file_limit",
                    "reason code");
    return pass ? 0 : 1;
}
