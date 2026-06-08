#pragma once

#include <cstdint>

namespace dinero::rpc {

inline uint64_t SelectGbtTemplateTime(uint64_t header_timestamp, uint64_t sampled_wall_time) {
    (void)sampled_wall_time;
    return header_timestamp;
}

}  // namespace dinero::rpc
