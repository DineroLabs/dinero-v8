#pragma once
#include <deque>
#include <mutex>
#include "compat/jsoncpp_compat.h"

class MiningEventBus {
public:
    void pushHashrate(uint64_t hps);
    void pushState(bool running);
    void pushBlockFound(int height, const std::string& hash, uint32_t bits,
                        uint64_t rewardAtoms, const std::string& payout);

    // return events with id > since; up to `max` (defaults below)
    Json::Value snapshot(int64_t since, int max = 200);

private:
    std::mutex mtx_;
    std::deque<Json::Value> q_;
    int64_t next_id_ = 1;
    size_t cap_ = 2000; // keep last 2k events
};
