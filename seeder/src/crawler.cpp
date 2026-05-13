#include "dinero/seeder/crawler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace dinero {
namespace seeder {

namespace {

bool parse_host_port(const std::string& s,
                      uint16_t default_port,
                      std::string& host_out,
                      uint16_t& port_out) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos) {
        host_out = s;
        port_out = default_port;
        return !host_out.empty();
    }
    host_out = s.substr(0, colon);
    const std::string ps = s.substr(colon + 1);
    if (ps.empty()) {
        port_out = default_port;
        return !host_out.empty();
    }
    char* end = nullptr;
    long port = std::strtol(ps.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || port <= 0 || port > 65535) {
        return false;
    }
    port_out = static_cast<uint16_t>(port);
    return !host_out.empty();
}

}  // namespace

size_t run_crawler(const CrawlerConfig& cfg, std::atomic<bool>& stop_flag) {
    PeerStore store;
    if (!cfg.state_path.empty()) {
        if (store.load(cfg.state_path)) {
            std::cout << "[seeder] loaded " << store.size()
                      << " peers from " << cfg.state_path << "\n";
        } else {
            std::cout << "[seeder] no state file at " << cfg.state_path
                      << " — starting fresh\n";
        }
    }

    // Seed from bootstrap list.
    for (const auto& boot : cfg.bootstrap) {
        std::string host;
        uint16_t port = 0;
        if (!parse_host_port(boot, cfg.default_port, host, port)) {
            std::cerr << "[seeder] skipping malformed bootstrap entry: "
                      << boot << "\n";
            continue;
        }
        store.seed(host, port);
    }

    const auto t_start = std::chrono::steady_clock::now();
    const auto deadline = cfg.total_duration.count() > 0
        ? t_start + cfg.total_duration
        : std::chrono::steady_clock::time_point::max();

    auto last_save = t_start;
    auto last_output = t_start;
    size_t total_probes = 0;

    while (!stop_flag.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        auto batch = store.select_for_probe(cfg.batch_size);
        if (batch.empty()) {
            // Nothing to probe yet (somehow seed list was empty). Wait a tick.
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        for (const auto& candidate : batch) {
            if (stop_flag.load(std::memory_order_acquire)) break;
            if (std::chrono::steady_clock::now() >= deadline) break;

            store.record_attempt(candidate.ip, candidate.port);
            ProbeResult result = probe_peer(candidate.ip, candidate.port, cfg.probe);
            total_probes++;

            if (result.outcome == ProbeOutcome::Success) {
                store.record_success(candidate.ip, candidate.port,
                                     result.remote_user_agent,
                                     result.remote_protocol_version,
                                     result.remote_best_height);
                std::cout << "[seeder] ok   " << candidate.ip << ':' << candidate.port
                          << " ua=\"" << result.remote_user_agent
                          << "\" height=" << result.remote_best_height
                          << " (+ " << result.learned_addresses.size()
                          << " peers)\n";

                // Feed learned addresses back into the queue. The probe
                // is "ok" even if learned_addresses is empty — older
                // nodes may have nothing to share.
                for (const auto& [ip, p] : result.learned_addresses) {
                    if (p != cfg.default_port) continue;  // skip ephemeral
                    store.seed(ip, p);
                }
            } else {
                store.record_failure(candidate.ip, candidate.port,
                                     result.error_detail);
                std::cout << "[seeder] fail " << candidate.ip << ':' << candidate.port
                          << "  " << result.error_detail << "\n";
            }
        }

        const auto cycle_end = std::chrono::steady_clock::now();

        // Periodic persistence.
        if (!cfg.state_path.empty() &&
            cycle_end - last_save >= cfg.state_save_interval) {
            store.save(cfg.state_path);
            last_save = cycle_end;
        }
        if (!cfg.output_path.empty() &&
            cycle_end - last_output >= cfg.output_interval) {
            write_seeds_observed(store, cfg.output_path, cfg.output);
            last_output = cycle_end;
        }

        // Brief pause between batches so we don't hammer the network at
        // full speed.
        if (cfg.cycle_pause.count() > 0 && !stop_flag.load()) {
            std::this_thread::sleep_for(cfg.cycle_pause);
        }
    }

    // Final flush.
    if (!cfg.state_path.empty()) store.save(cfg.state_path);
    if (!cfg.output_path.empty()) write_seeds_observed(store, cfg.output_path, cfg.output);

    std::cout << "[seeder] stopped after " << total_probes
              << " probe attempts. Final peer-store size: "
              << store.size() << "\n";
    return total_probes;
}

}  // namespace seeder
}  // namespace dinero
