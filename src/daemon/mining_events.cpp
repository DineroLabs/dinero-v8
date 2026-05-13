#include "daemon/mining_events.h"
#include <chrono>
#include <cstdio>

static std::string iso_now() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    char buf[32]; std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static Json::Value base(const char* type) {
    Json::Value j(Json::objectValue);
    j["type"] = type;
    j["ts"] = iso_now();
    return j;
}

void MiningEventBus::pushHashrate(uint64_t hps) {
    Json::Value j = base("hashrate"); 
    j["hps"] = uint64_t(hps);
    std::lock_guard<std::mutex> lk(mtx_);
    j["id"] = int64_t(next_id_++);
    q_.push_back(std::move(j));
    if (q_.size() > cap_) q_.pop_front();
}

void MiningEventBus::pushState(bool running) {
    Json::Value j = base("state"); 
    j["running"] = running;
    std::lock_guard<std::mutex> lk(mtx_);
    j["id"] = int64_t(next_id_++);
    q_.push_back(std::move(j));
    if (q_.size() > cap_) q_.pop_front();
}

void MiningEventBus::pushBlockFound(int h, const std::string& hash, uint32_t bits,
                                    uint64_t rewardAtoms, const std::string& payout) {
    char b[16]; 
    std::snprintf(b, sizeof(b), "0x%08x", bits);
    Json::Value j = base("block_found");
    j["height"] = h;
    j["hash"] = hash;
    j["bits"] = b;
    j["reward"] = uint64_t(rewardAtoms);
    j["payout"] = payout;
    std::lock_guard<std::mutex> lk(mtx_);
    j["id"] = int64_t(next_id_++);
    q_.push_back(std::move(j));
    if (q_.size() > cap_) q_.pop_front();
}

Json::Value MiningEventBus::snapshot(int64_t since, int max) {
    std::lock_guard<std::mutex> lk(mtx_);
    Json::Value arr(Json::arrayValue);
    for (const auto& e : q_) {
        if (e["id"].asInt64() > since) {
            arr.append(e);
            if ((int)arr.size() >= max) break;
        }
    }
    return arr;
}
