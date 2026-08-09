// One-shot peer probe: TCP connect, version handshake, getaddr, collect
// peers from the addr reply, close. Single-threaded blocking I/O with a
// per-step timeout. The seeder's main loop calls probe() once per
// candidate; failures are reported via the ProbeResult tag so the caller
// can score them.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dinero {
namespace seeder {

#ifndef DINERO_SEEDER_VERSION
#define DINERO_SEEDER_VERSION "unknown"
#endif

enum class ProbeOutcome {
    Success,                  // version + verack exchanged; addr (possibly empty) received
    ConnectTimeout,           // TCP connect didn't complete in budget
    ConnectRefused,           // peer rejected the TCP connection
    HandshakeTimeout,         // no version reply in budget
    HandshakeMalformed,       // bytes received but wire-format invalid
    ProtocolMismatch,         // peer's protocol_version is below our floor
    Closed,                   // peer closed cleanly without responding
    SocketError,              // generic errno path
};

struct ProbeResult {
    ProbeOutcome outcome = ProbeOutcome::SocketError;
    std::string error_detail;                    // human-readable; goes into state file
    std::string remote_user_agent;               // empty if handshake didn't complete
    uint32_t remote_protocol_version = 0;
    uint64_t remote_services = 0;
    uint32_t remote_best_height = 0;
    std::chrono::milliseconds elapsed{0};
    // Peers learned from the peer's `addr` reply (if any). Each entry is
    // an (ip-string, port) pair, IPv4 only. The seeder feeds these back
    // into its candidate queue.
    std::vector<std::pair<std::string, uint16_t>> learned_addresses;
};

struct ProbeConfig {
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds handshake_timeout{8000};
    std::chrono::milliseconds addr_timeout{5000};   // wait this long after getaddr for addr reply
    uint32_t protocol_version_floor = 70014;        // reject ancient peers
    std::string user_agent = "/dinero-seeder:" DINERO_SEEDER_VERSION "/";
    uint32_t best_height = 0;                       // seeder doesn't track chain state
};

ProbeResult probe_peer(const std::string& host,
                        uint16_t port,
                        const ProbeConfig& cfg);

}  // namespace seeder
}  // namespace dinero
