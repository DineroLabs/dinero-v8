#pragma once
#include "primitives/uint256.h"
#include <cstdint>
namespace dinero::p2p {
// Locally captured selected state identity, never caller/peer-supplied height.
struct OrchardPhoneHistoryTarget {
    uint32_t activation=0,height=0;
    uint256 hash,catalog,base;
    bool operator==(const OrchardPhoneHistoryTarget&)const=default;
};
}
