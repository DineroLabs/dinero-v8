#include "daemon/services/p2p_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/address_manager_service.h"
#include "daemon/services/peer_scoring_service.h"
#include "daemon/daemon_context.h"
#include "util/thread_util.h"  // #298: SetThreadName for gdb backtraces
#include "config/seed_nodes.h"
#include "consensus/chainparams.h"
#include "consensus/block_download_scheduler.h"
#include "dinero/network/network_invariants.h"
#include "consensus/header_chain.h"  // P1 reorg fix: for HeaderChainSelector::GetBestHeader()
#include "network/local_interfaces.h"  // Self-loop filter (shared with P2PManager)
#include "network/port_mapper.h"
#include "network/stun_client.h"       // NAT traversal Phase C1: public-IP discovery
#include "network/quic_transport.h"    // Stage B: log QUIC relay-transport readiness at startup
#include "daemon/node_identity.h"      // NAT traversal Phase 1A: keypair for dineroid handshake
#include "p2p/block_download_scheduler.h"
#include "p2p/peer_governor.h"
#include "p2p/peer_quality.h"
#include "p2p/peer_quality_derivation.h"
#include "storage/chain_db.h"
#include "common/ilogger.h"  // For ILogger interface dependency injection
#include "network/types.h"   // For dinero::ServiceFlags
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <unordered_set>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

namespace dinero {

namespace {

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsRelayModeOn(const std::string& mode) {
    const std::string value = LowerAscii(mode);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool IsRelayModeOff(const std::string& mode) {
    const std::string value = LowerAscii(mode);
    return value == "0" || value == "false" || value == "no" || value == "off";
}

bool IsDynamicP2PActiveMode(const std::string& mode) {
    const std::string value = LowerAscii(mode);
    return value == "1" || value == "true" || value == "yes" ||
           value == "on" || value == "active" || value == "active_slow_churn";
}

bool ParseEndpoint(const std::string& endpoint,
                   uint16_t default_port,
                   std::string* out_host,
                   uint16_t* out_port) {
    if (!out_host || !out_port) {
        return false;
    }
    if (endpoint.empty()) {
        return false;
    }

    const size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
        *out_host = endpoint;
        *out_port = default_port;
        return true;
    }

    *out_host = endpoint.substr(0, colon);
    if (out_host->empty()) {
        return false;
    }

    try {
        unsigned long parsed = std::stoul(endpoint.substr(colon + 1));
        if (parsed == 0 || parsed > 65535) {
            return false;
        }
        *out_port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsOnionAddress(const std::string& address) {
    return EndsWith(ToLowerAscii(address), ".onion");
}

bool IsOnionAutoValue(const std::string& value) {
    const std::string lower = ToLowerAscii(value);
    return lower == "auto" || lower == "detect" || lower == "1" ||
           lower == "true" || lower == "yes";
}

void AddReconnectTarget(std::vector<std::pair<std::string, uint16_t>>& targets,
                        std::unordered_set<std::string>& seen,
                        const std::string& host,
                        uint16_t port) {
    if (host.empty() || port == 0) {
        return;
    }
    const std::string key = host + ":" + std::to_string(port);
    if (seen.insert(key).second) {
        targets.emplace_back(host, port);
    }
}

std::string BytesToHex(const std::string& bytes) {
    static const char kHexChars[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(kHexChars[c >> 4]);
        out.push_back(kHexChars[c & 0x0F]);
    }
    return out;
}


// Self-loop filtering helpers (LocalInterfaceIps + IsLocalInterfaceIp) live
// in include/network/local_interfaces.h so P2PManager::connect_to_peer can
// share the same definition. See that header for the v5-reset incident notes.
using dinero::network::IsLocalInterfaceIp;

bool BuildProactiveHeaderBurst(ChainDB* chain_db,
                               uint32_t peer_height,
                               std::vector<std::string>* out_header_hexes,
                               int* out_start_height,
                               int* out_end_height) {
    if (!chain_db || !out_header_hexes || !out_start_height || !out_end_height) {
        return false;
    }
    out_header_hexes->clear();

    auto tip_result = chain_db->getTip();
    if (tip_result.status() != Status::Ok) {
        return false;
    }

    const int tip_height = tip_result.value().height;
    if (tip_height <= static_cast<int>(peer_height)) {
        return false;
    }

    // Include overlap so peers with a slightly stale locator can still connect
    // this header batch without waiting for another request round.
    constexpr int kMaxHeadersPerMessage = 2000;
    constexpr int kOverlap = 128;
    int start_height = static_cast<int>(peer_height) - kOverlap + 1;
    start_height = std::max(1, start_height);
    const int end_height = std::min(tip_height, start_height + kMaxHeadersPerMessage - 1);

    out_header_hexes->reserve(static_cast<size_t>(std::max(0, end_height - start_height + 1)));
    for (int h = start_height; h <= end_height; ++h) {
        auto hash_result = chain_db->getBlockHashByHeight(h);
        if (hash_result.status() != Status::Ok) {
            break;
        }
        auto header_result = chain_db->getHeader(hash_result.value());
        if (header_result.status() != Status::Ok) {
            break;
        }
        out_header_hexes->push_back(BytesToHex(header_result.value().Serialize()));
    }

    if (out_header_hexes->empty()) {
        return false;
    }

    *out_start_height = start_height;
    *out_end_height = start_height + static_cast<int>(out_header_hexes->size()) - 1;
    return true;
}

}  // namespace

P2PService::~P2PService() {
    StopSchedulerTickLoop();
    StopPortMapping();
}

bool P2PService::SetNetworkActive(bool active) {
    if (!p2p_mgr_) {
        return false;
    }

    if (offline_mode_) {
        p2p_mgr_->set_network_active(false);
        if (logger_interface_ && active) {
            logger_interface_->warning(
                "[P2PService] Ignoring setnetworkactive(true) while p2p.offline=1");
        }
        return !active;
    }

    p2p_mgr_->set_network_active(active);
    if (active) {
        StartSchedulerTickLoop();
    } else {
        StopSchedulerTickLoop();
    }
    return true;
}

size_t P2PService::SendPingToAll() {
    if (!p2p_mgr_ || !p2p_mgr_->is_network_active()) {
        return 0;
    }

    static std::atomic<uint64_t> nonce_counter{1};
    size_t pinged = 0;
    const auto peers = p2p_mgr_->get_connected_peers();
    for (const auto& peer : peers) {
        if (!peer.is_connected) {
            continue;
        }
        const auto ping = ::P2PMessage::create_ping(
            nonce_counter.fetch_add(1, std::memory_order_relaxed));
        if (p2p_mgr_->send_to_peer(peer.to_string(), ping)) {
            ++pinged;
        }
    }
    return pinged;
}

P2PService::NetworkTotals P2PService::GetNetworkTotals() const {
    NetworkTotals totals;
    if (!p2p_mgr_) {
        return totals;
    }

    const auto peers = p2p_mgr_->get_connected_peers();
    for (const auto& peer : peers) {
        totals.bytes_recv += peer.bytes_recv;
        totals.bytes_sent += peer.bytes_sent;
    }
    return totals;
}

P2PService::NetworkStatus P2PService::GetNetworkStatus() const {
    NetworkStatus status;
    status.relay_mode = RelayMode();
    status.relay_active = relay_active_.load(std::memory_order_acquire);
    status.local_relay = IsRelayRoleEnabled();
    status.dynamic_p2p_enabled = IsDynamicP2PActive();
    status.dynamic_p2p_mode = DynamicP2PMode();
    if (!p2p_mgr_) {
        std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
        status.port_mapping_requested = port_mapping_requested_;
        status.port_mapping_active = port_mapping_active_;
        status.port_mapping_mode = port_mapping_mode_;
        status.port_mapping_protocol = port_mapping_protocol_;
        status.port_mapping_external_address = port_mapping_external_address_;
        status.port_mapping_external_port = port_mapping_external_port_;
        status.port_mapping_message = port_mapping_message_;
        return status;
    }

    status.network_active = p2p_mgr_->is_network_active();
    status.listening = p2p_mgr_->IsListening();
    status.listen_port = p2p_mgr_->get_listen_port();
    status.advertised_addresses = p2p_mgr_->get_advertised_addresses();
    status.has_explicit_advertised = p2p_mgr_->has_explicit_advertised();
    status.onion_transport_configured = onion_proxy_configured_;
    status.onion_transport_enabled = p2p_mgr_->onion_proxy_enabled();
    status.onion_transport_reachable = onion_proxy_reachable_;
    status.onion_transport_auto_detected = onion_proxy_auto_detected_;
    status.onion_proxy = p2p_mgr_->onion_proxy_endpoint();
    status.onion_transport_message = onion_proxy_message_;
    status.relay_hints_received_self = p2p_mgr_->relay_hints_received_self_count();
    status.relay_hints_received_relay = p2p_mgr_->relay_hints_received_relay_count();
    status.relay_hints_evicted_expired = p2p_mgr_->relay_hints_evicted_expired_count();
    status.relay_hints_evicted_failure = p2p_mgr_->relay_hints_evicted_failure_count();
    status.relay_directory_entries = p2p_mgr_->relay_registry_entry_count();
    status.relay_directory_grace_pending = p2p_mgr_->relay_registry_grace_pending_count();

    if (address_manager_ && address_manager_->getManager()) {
        auto* manager = address_manager_->getManager();
        const auto stats = manager->getStats();
        status.addrman.available = true;
        status.addrman.total_addresses = stats.total_addresses;
        status.addrman.new_addresses = stats.new_addresses;
        status.addrman.tried_addresses = stats.tried_addresses;
        status.addrman.terrible_addresses = stats.terrible_addresses;
        status.addrman.banned_addresses = stats.banned_addresses;
        status.addrman.avg_success_rate = stats.avg_success_rate;
        status.addrman.relay_candidates =
            manager->countAddressesByService(dinero::ServiceFlags::NODE_RELAY);
    }

    const auto configured_seeds = p2p_mgr_->get_seed_nodes();
    status.configured_seed_peers = configured_seeds.size();
    auto is_configured_seed = [&](const PeerInfo& peer) {
        return std::any_of(configured_seeds.begin(), configured_seeds.end(),
                           [&](const auto& seed) {
                               return seed.first == peer.address && seed.second == peer.port;
                           });
    };

    std::vector<dinero::p2p::PeerGovernorCandidate> governor_candidates;
    const auto peers = p2p_mgr_->get_connected_peers();
    governor_candidates.reserve(peers.size());
    for (const auto& peer : peers) {
        if (!peer.is_connected) {
            continue;
        }
        const bool configured_seed = is_configured_seed(peer);
        const bool relay_capable =
            (peer.service_flags & dinero::ServiceFlags::NODE_RELAY) != 0;
        ++status.connections;
        if (peer.is_outbound) {
            ++status.outbound;
        } else {
            ++status.inbound;
        }
        if (configured_seed) {
            ++status.configured_seed_connections;
        } else {
            ++status.discovered_connections;
        }
        if (relay_capable) {
            ++status.relay_peer_connections;
        }

        dinero::p2p::PeerGovernorCandidate candidate;
        candidate.endpoint = peer.address + ":" + std::to_string(peer.port);
        candidate.quality = dinero::p2p::BuildDynamicP2PQualitySnapshot(peer);
        candidate.connected = true;
        candidate.outbound = peer.is_outbound;
        candidate.configured_seed = configured_seed;
        candidate.relay_capable = relay_capable;
        governor_candidates.push_back(std::move(candidate));
    }
    {
        const dinero::p2p::PeerGovernor governor;
        const auto decision = governor.Evaluate(governor_candidates);
        status.dynamic_p2p_governor.available = true;
        status.dynamic_p2p_governor.mode =
            status.dynamic_p2p_enabled ? "active_slow_churn" : "dry_run";
        status.dynamic_p2p_governor.connected_outbound = decision.connected_outbound;
        status.dynamic_p2p_governor.configured_seed_hot = decision.configured_seed_hot;
        status.dynamic_p2p_governor.relay_capable_seen = decision.relay_capable_seen;
        status.dynamic_p2p_governor.hot_peers = decision.hot_peers;
        status.dynamic_p2p_governor.warm_candidates = decision.warm_candidates;
        status.dynamic_p2p_governor.relay_registration_candidates =
            decision.relay_registration_candidates;
        status.dynamic_p2p_governor.demote_candidates = decision.demote_candidates;
    }

    {
        std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
        status.port_mapping_requested = port_mapping_requested_;
        status.port_mapping_active = port_mapping_active_;
        status.port_mapping_mode = port_mapping_mode_;
        status.port_mapping_protocol = port_mapping_protocol_;
        status.port_mapping_external_address = port_mapping_external_address_;
        status.port_mapping_external_port = port_mapping_external_port_;
        status.port_mapping_message = port_mapping_message_;
    }
    {
        // NAT traversal Phase C1: mirror STUN result into NetworkStatus.
        std::lock_guard<std::mutex> lock(stun_status_mutex_);
        status.stun_discovered_address = stun_discovered_address_;
        status.stun_server_used = stun_server_used_;
        status.stun_message = stun_message_;
    }
    return status;
}

std::string P2PService::RelayMode() const {
    return config_ ? LowerAscii(config_->GetString("p2p.relay", "auto")) : "auto";
}

bool P2PService::IsRelayRoleEnabled() const {
    const std::string mode = RelayMode();
    if (IsRelayModeOn(mode)) {
        return true;
    }
    if (IsRelayModeOff(mode)) {
        return false;
    }
    return relay_active_.load(std::memory_order_acquire);
}

std::string P2PService::DynamicP2PMode() const {
    if (!config_) {
        return "observe";
    }
    const std::string mode = LowerAscii(config_->GetString("p2p.dynamic_p2p", "observe"));
    return IsDynamicP2PActiveMode(mode) ? "active_slow_churn" : "observe";
}

bool P2PService::IsDynamicP2PActive() const {
    if (!config_) {
        return false;
    }
    if (config_->GetBool("p2p.dynamic_p2p.enabled", false)) {
        return true;
    }
    return DynamicP2PMode() == "active_slow_churn";
}

void P2PService::SetRelayActive(bool active) {
    const bool previous = relay_active_.exchange(active, std::memory_order_acq_rel);
    if (previous == active) {
        return;
    }
    if (logger_interface_) {
        logger_interface_->info(std::string("[P2PService] Relay role auto-mode ") +
                                (active ? "engaged" : "disengaged") +
                                " (p2p.relay=" + RelayMode() + ")");
    }
}

void P2PService::MaybeRunDynamicP2PActiveChurn(std::chrono::steady_clock::time_point now) {
    if (!p2p_mgr_ || !IsDynamicP2PActive()) {
        return;
    }

    const auto read_seconds = [this](const std::string& key, int fallback, int minimum) {
        const int value = config_ ? config_->GetInt(key, fallback) : fallback;
        return std::chrono::seconds(std::max(value, minimum));
    };

    const auto startup_grace =
        read_seconds("p2p.dynamic_p2p.startup_grace_seconds", 300, 60);
    const auto churn_interval =
        read_seconds("p2p.dynamic_p2p.churn_interval_seconds", 600, 60);
    const size_t min_peers = static_cast<size_t>(
        config_ ? std::max(config_->GetInt("p2p.dynamic_p2p.min_peers", 4), 1) : 4);

    if (dynamic_p2p_started_at_ == std::chrono::steady_clock::time_point{}) {
        dynamic_p2p_started_at_ = now;
        last_dynamic_p2p_churn_ = now;
        return;
    }
    if (now - dynamic_p2p_started_at_ < startup_grace) {
        return;
    }
    if (now - last_dynamic_p2p_churn_ < churn_interval) {
        return;
    }
    last_dynamic_p2p_churn_ = now;

    const auto configured_seeds = p2p_mgr_->get_seed_nodes();
    const auto is_configured_seed = [&](const PeerInfo& peer) {
        return std::any_of(configured_seeds.begin(), configured_seeds.end(),
                           [&](const auto& seed) {
                               return seed.first == peer.address && seed.second == peer.port;
                           });
    };

    const auto peers = p2p_mgr_->get_connected_peers();
    size_t connected = 0;
    std::vector<dinero::p2p::PeerGovernorCandidate> candidates;
    candidates.reserve(peers.size());
    std::unordered_set<std::string> protected_configured_seeds;
    for (const auto& peer : peers) {
        if (!peer.is_connected) {
            continue;
        }
        ++connected;
        const bool configured_seed = is_configured_seed(peer);
        const bool relay_capable =
            (peer.service_flags & dinero::ServiceFlags::NODE_RELAY) != 0;
        const std::string endpoint = peer.address + ":" + std::to_string(peer.port);
        if (configured_seed) {
            protected_configured_seeds.insert(endpoint);
        }

        dinero::p2p::PeerGovernorCandidate candidate;
        candidate.endpoint = endpoint;
        candidate.quality = dinero::p2p::BuildDynamicP2PQualitySnapshot(peer);
        candidate.connected = true;
        candidate.outbound = peer.is_outbound;
        candidate.configured_seed = configured_seed;
        candidate.relay_capable = relay_capable;
        candidates.push_back(std::move(candidate));
    }

    if (connected <= min_peers) {
        if (logger_interface_) {
            logger_interface_->info("[DynamicP2P] active slow-churn skipped: peer floor " +
                                    std::to_string(connected) + "/" +
                                    std::to_string(min_peers));
        }
        return;
    }

    const dinero::p2p::PeerGovernor governor;
    const auto decision = governor.Evaluate(candidates);
    for (const auto& endpoint : decision.demote_candidates) {
        if (protected_configured_seeds.count(endpoint) > 0) {
            continue;
        }
        auto it = std::find_if(peers.begin(), peers.end(), [&](const PeerInfo& peer) {
            return peer.is_connected && peer.is_outbound &&
                   peer.address + ":" + std::to_string(peer.port) == endpoint;
        });
        if (it == peers.end()) {
            continue;
        }
        if (connected - 1 < min_peers) {
            break;
        }
        if (logger_interface_) {
            logger_interface_->warning("[DynamicP2P] active slow-churn disconnecting weak outbound peer " +
                                       endpoint + " (one-peer canary action)");
        }
        p2p_mgr_->disconnect_peer(endpoint);
        return;
    }

    if (logger_interface_) {
        logger_interface_->info("[DynamicP2P] active slow-churn evaluated: no eligible peer to rotate");
    }
}

// NAT traversal Phase C1: spin up a STUN client and fire off a discovery
// round. Hardcoded server list: DineroLabs-operated primaries (placeholder
// hostnames until ops brings them up) plus two well-known public fallbacks.
// DineroLabs servers go FIRST so a node never leaks its IP to a third party
// when our own infrastructure is healthy.
//
// Failure modes are all non-fatal:
//   - getaddrinfo fails for every server → "STUN: all server resolutions
//     failed", logged, status mirrors the error, no advertised address
//     mutation.
//   - All servers time out within 8 seconds → "STUN: all servers timed
//     out", same handling.
//   - Single server succeeds → status records discovered_address + server
//     used, and add_advertised_address gets the result.
void P2PService::StartStunDiscoveryIfEnabled() {
    if (!p2p_mgr_) return;
    const bool enabled = config_ ? config_->GetBool("p2p.stun.enabled", true) : true;
    if (!enabled) {
        std::lock_guard<std::mutex> lock(stun_status_mutex_);
        stun_message_ = "disabled";
        return;
    }

    stun_client_ = std::make_unique<dinero::network::StunClient>();
    std::vector<std::pair<std::string, uint16_t>> servers = {
        // DineroLabs primaries (TODO ops: bring these up before rc cut)
        {"stun1.dinerolabs.org", 3478},
        {"stun2.dinerolabs.org", 3478},
        {"stun3.dinerolabs.org", 3478},
        // Public fallbacks — last in the list so we only hit them when
        // DineroLabs servers are unreachable.
        {"stun.l.google.com", 19302},
        {"stun.cloudflare.com", 3478},
    };
    stun_client_->SetServers(std::move(servers));

    {
        std::lock_guard<std::mutex> lock(stun_status_mutex_);
        stun_message_ = "discovering (background)";
    }
    if (logger_interface_) {
        logger_interface_->info("[P2PService] STUN discovery dispatched (5 servers, 8s deadline)");
    }

    stun_client_->Discover(std::chrono::seconds(8),
                           [this](const dinero::network::StunResult& r) {
        if (!r.public_addr.empty()) {
            const std::string addr_str = r.public_addr.to_string();
            {
                std::lock_guard<std::mutex> lock(stun_status_mutex_);
                stun_discovered_address_ = addr_str;
                stun_server_used_ = r.server_endpoint;
                stun_message_ = "ok";
            }
            // STUN discovers the router's reflexive UDP endpoint. It is useful
            // diagnostic data and a future hole-punching input, but it does
            // not prove that the daemon's TCP/QUIC P2P listener is reachable.
            // Do not feed it into advertised_addresses_: relay auto-register
            // treats that list as confirmed direct inbound reachability. UPnP,
            // NAT-PMP, and explicit externalip remain the sources for direct
            // P2P address advertisement.
            if (logger_interface_) {
                logger_interface_->info(
                    "[P2PService] STUN discovered public address " + addr_str +
                    " via " + r.server_endpoint +
                    " — keeping as diagnostic data, not advertising as reachable P2P");
            }
        } else {
            std::lock_guard<std::mutex> lock(stun_status_mutex_);
            stun_message_ = r.error_message.empty() ? "no response" : r.error_message;
            if (logger_interface_) {
                logger_interface_->warning("[P2PService] STUN discovery: " + stun_message_ +
                                           " — outbound P2P still works");
            }
        }
    });
}

void P2PService::StartSchedulerTickLoop() {
    if (scheduler_tick_running_.exchange(true)) {
        return;
    }

    scheduler_tick_thread_ = std::thread([this]() {
        util::SetThreadName("din-sched");  // #298: readable gdb backtraces
        if (logger_interface_) {
            logger_interface_->info("[P2PService] Scheduler tick loop started (interval=" +
                                    std::to_string(scheduler_tick_interval_.count()) + "ms)");
        }

        int activate_tick_counter = 0;
        size_t reconnect_cursor = 0;
        auto last_invariant_check = std::chrono::steady_clock::now();

        while (scheduler_tick_running_.load(std::memory_order_relaxed)) {
            if (auto* ctx = DaemonContext::instance()) {
                // AssumeUTXO body backfill: while the lifecycle is validating
                // history, keep the backfill queue armed for heights 1..base,
                // anchored on the snapshot base HASH (the trust root — the
                // best header chain may diverge below the base, so a height
                // anchor could walk a fork). Disarm on every non-validating
                // tick: covers retirement (FullyValidated), fatal AND
                // operator reset with zero extra plumbing. EnableBackfill is
                // idempotent for the same (range, anchor) so this 5s re-arm
                // is cheap; it is also the RETRY for an Enable the scheduler
                // refused while the base header was not yet known (a refused
                // Enable leaves backfill disabled by contract). This loop —
                // not the OnHeaders handler — is the arm site because it
                // keeps firing after header traffic stops, which is exactly
                // when a snapshot-loaded node sits validating history.
                if (ctx->block_download && ctx->chainstate) {
                    if (auto* lc = ctx->chainstate->GetAssumeUtxoLifecycle()) {
                        const auto st = lc->GetStatus(std::chrono::steady_clock::now());
                        const bool validating =
                            st.assumeutxo_active && !st.history_fully_validated && !st.fatal;
                        if (validating && st.snapshot_base_height > 0) {
                            ctx->block_download->EnableBackfill(
                                1, st.snapshot_base_height, st.snapshot_base_block);
                        } else {
                            ctx->block_download->DisableBackfill();
                        }
                    }
                }
                if (ctx->block_download) {
                    ctx->block_download->Tick();
                }
                if (ctx->parallel_block_download) {
                    ctx->parallel_block_download->processQueue();
                }

                // #298 no-progress hang watchdog. Hosted HERE, in the
                // always-running scheduler tick loop, on purpose: it is
                // INDEPENDENT of the bg-validation thread, so it still fires
                // when that thread is the one parked (the incident). The
                // lifecycle stall watchdog is driven only from inside the
                // bg-validation worker's Tick(), so it goes silent if that
                // worker wedges; this one does not. Diagnose only — it never
                // restarts anything.
                if (ctx->chainstate) {
                    ctx->chainstate->CheckHangWatchdog();
                }

                // P1 reorg fix: Periodic ActivateBestChain safety net (every 30s).
                // Only fires when header chain is ahead of active tip (fork detected).
                if (++activate_tick_counter >= 6) {  // 5s × 6 = 30s
                    activate_tick_counter = 0;
                    if (ctx->chainstate && ctx->header_chain) {
                        auto* best = ctx->header_chain->GetBestHeader();
                        auto* active = ctx->chainstate->GetActiveTip();
                        if (best && active && best->height > active->height) {
                            if (logger_interface_) {
                                logger_interface_->info("[P1] Periodic reorg check: header=" +
                                    std::to_string(best->height) + " > active=" +
                                    std::to_string(active->height) + " — triggering ActivateBestChain");
                            }
                            ctx->chainstate->ActivateBestChain();
                        }
                    }
                }
            }

            if (p2p_mgr_) {
                auto now = std::chrono::steady_clock::now();

                if (now - last_invariant_check >= std::chrono::seconds(60)) {
                    last_invariant_check = now;
                    network::NetworkInvariants checker(p2p_mgr_.get());
                    const auto violations = checker.checkAll();
                    for (const auto& violation : violations) {
                        const std::string message =
                            "[P2P_INVARIANT] " + violation.invariant_name + ": " + violation.description;
                        if (!logger_interface_) {
                            continue;
                        }
                        if (violation.severity == "CRITICAL") {
                            logger_interface_->error(message);
                        } else if (violation.severity == "WARNING") {
                            logger_interface_->warning(message);
                        } else {
                            logger_interface_->info(message);
                        }
                    }
                }

                // Anchor peer auto-reconnect: ensure anchor peers stay connected
                if (now - last_reconnect_probe_ >= reconnect_probe_interval_) {
                    auto anchors = dinero::config::getAnchorPeers(Params().name);
                    uint16_t anchor_port = Params().p2p_port;
                    auto connected_peers = p2p_mgr_->get_connected_peers();
                    std::unordered_set<std::string> connected_ips;
                    for (const auto& peer : connected_peers) {
                        connected_ips.insert(peer.address);
                    }
                    for (const auto& anchor : anchors) {
                        // Never reconnect to an anchor that resolves to a local interface —
                        // self-loops poison the BlockDownloadScheduler (see LocalInterfaceIps).
                        if (IsLocalInterfaceIp(anchor.hostname)) {
                            continue;
                        }
                        if (connected_ips.find(anchor.hostname) == connected_ips.end()) {
                            uint16_t port = (anchor.port != 0) ? anchor.port : anchor_port;
                            if (p2p_mgr_->connect_to_peer(anchor.hostname, port)) {
                                if (logger_interface_) {
                                    logger_interface_->info("[P2PService] Reconnecting anchor peer: " +
                                                            anchor.hostname + ":" + std::to_string(port));
                                }
                            }
                        }
                    }
                }

                if (now - last_reconnect_probe_ >= reconnect_probe_interval_) {
                    last_reconnect_probe_ = now;

                    const size_t peer_count = p2p_mgr_->get_peer_count();
                    if (peer_count == 0 && !reconnect_targets_.empty()) {
                        const size_t attempts = std::min<size_t>(3, reconnect_targets_.size());
                        size_t kick_count = 0;

                        for (size_t i = 0; i < attempts; ++i) {
                            const size_t idx = (reconnect_cursor + i) % reconnect_targets_.size();
                            const auto& target = reconnect_targets_[idx];
                            if (p2p_mgr_->connect_to_peer(target.first, target.second)) {
                                ++kick_count;
                            }
                        }

                        reconnect_cursor = (reconnect_cursor + attempts) % reconnect_targets_.size();
                        if (logger_interface_) {
                            logger_interface_->info("[P2PService] Zero-peer reconnect kick: attempts=" +
                                                    std::to_string(attempts) + ", accepted=" +
                                                    std::to_string(kick_count));
                        }
                    }
                }

                MaybeRunDynamicP2PActiveChurn(now);

                // issue #214: detect a frozen best-header (lost announcements on
                // stale connections) and recover in-daemon (re-getheaders /
                // rotate stalest peer) instead of relying on an external restart.
                MaybeRecoverStaleTip(now);
            }
            std::this_thread::sleep_for(scheduler_tick_interval_);
        }

        if (logger_interface_) {
            logger_interface_->info("[P2PService] Scheduler tick loop stopped");
        }
    });
}

void P2PService::MaybeRecoverStaleTip(std::chrono::steady_clock::time_point now) {
    // issue #214: detect a frozen best-header (the node stopped LEARNING about
    // new blocks — lost inv/headers announcements on stale long-lived
    // connections) and recover without an external restart.
    if (!p2p_mgr_) {
        return;
    }
    auto* ctx = DaemonContext::instance();
    if (!ctx || !ctx->chainstate || !ctx->header_chain) {
        return;
    }

    const auto* best = ctx->header_chain->GetBestHeader();
    const uint32_t best_h = best ? best->height : 0;
    const size_t peer_count = p2p_mgr_->get_peer_count();

    // The WHEN-to-act decision is a pure state machine (unit-tested in
    // test_stale_tip_recovery.cpp). Everything below only runs when it says the
    // tip is stale enough to re-probe.
    if (daemon::decideStaleTipAction(best_h, peer_count, now, staleness_threshold_,
                                     staleness_getheaders_interval_, stale_tip_state_) !=
        daemon::StaleTipAction::SEND_GETHEADERS) {
        return;
    }

    const auto stale_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now - stale_tip_state_.last_header_advance_time).count();

    // Recovery: re-issue getheaders to every peer. getheaders is a PULL, so
    // peers answer it even when their announcement (push) path to us has gone
    // quiet — this recovers the common "lost announcements" stall without
    // dropping any connection.
    //
    // Send per-peer with the SYNCHRONOUS send_to_peer(), NOT broadcast_message():
    // the async broadcast outbox can silently drop messages under congestion,
    // and a recovery probe must actually reach peers precisely when the node is
    // wedged. Mirrors the block-getdata callback in daemon_app.cpp.
    auto locator = ctx->chainstate->GenerateBlockLocator();
    int sent = 0;
    if (!locator.empty()) {
        std::vector<std::string> locator_hex;
        locator_hex.reserve(locator.size());
        for (const auto& hash_item : locator) {
            locator_hex.push_back(hash_item.GetHex());
        }
        ::P2PMessage getheaders_msg = ::P2PMessage::create_getheaders(locator_hex);
        for (const auto& peer : p2p_mgr_->get_connected_peers()) {
            if (p2p_mgr_->send_to_peer(peer.to_string(), getheaders_msg)) {
                ++sent;
            }
        }
    }
    if (logger_interface_) {
        logger_interface_->warning(
            "[P2PService] Stale tip: best header frozen at " + std::to_string(best_h) +
            " for " + std::to_string(stale_secs) + "s — re-issued getheaders to " +
            std::to_string(sent) + "/" + std::to_string(peer_count) +
            " peers (attempt " + std::to_string(stale_tip_state_.staleness_getheaders_count) + ")");
    }

    // NOTE: a stalest-peer rotation tier (disconnect the longest-silent peer to
    // force a fresh connection) is deliberately deferred — getheaders-refresh is
    // the safe v1, and the external height-watchdog backstops the rare case of a
    // genuinely dead connection that won't answer getheaders. Revisit rotation
    // once this is confirmed against a real stall (issue #214).
}

void P2PService::MaybeRequestHeadersForPeerTip(const std::string& peer_addr,
                                               uint32_t peer_height,
                                               const char* reason) {
    if (!p2p_mgr_ || !chainstate_ || peer_height == 0) {
        return;
    }

    auto* ctx = DaemonContext::instance();
    if (!ctx || !ctx->chainstate) {
        return;
    }

    const auto* best_header = ctx->header_chain ? ctx->header_chain->GetBestHeader() : nullptr;
    const auto* active_tip = ctx->chainstate->GetActiveTip();
    const uint32_t best_header_height = best_header ? best_header->height : 0;
    const uint32_t active_height = active_tip ? active_tip->height : chainstate_->getBlockHeight();
    const uint32_t known_height = std::max(best_header_height, active_height);

    chainstate_->UpdateNetworkHeight(peer_height);

    if (peer_height <= known_height) {
        if (ctx->block_download && ctx->header_chain && best_header && active_tip &&
            best_header->height > active_tip->height &&
            ctx->block_download->HasSendGetDataCallback()) {
            ctx->block_download->OnHeadersProcessed();
            ctx->block_download->Tick();
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(peer_tip_getheaders_mutex_);
        const auto last_it = last_peer_tip_getheaders_height_.find(peer_addr);
        if (last_it != last_peer_tip_getheaders_height_.end() &&
            peer_height <= last_it->second) {
            return;
        }
    }

    auto locator = chainstate_->GenerateBlockLocator();
    if (locator.empty()) {
        return;
    }

    std::vector<std::string> locator_hex;
    locator_hex.reserve(locator.size());
    for (const auto& hash_item : locator) {
        locator_hex.push_back(hash_item.GetHex());
    }

    auto getheaders_msg = ::P2PMessage::create_getheaders(locator_hex);
    const bool sent = p2p_mgr_->send_to_peer(peer_addr, getheaders_msg);
    if (sent) {
        {
            std::lock_guard<std::mutex> lock(peer_tip_getheaders_mutex_);
            last_peer_tip_getheaders_height_[peer_addr] = peer_height;
        }
        if (logger_interface_) {
            logger_interface_->info(
                "[P2PService] Peer " + peer_addr + " reports higher tip " +
                std::to_string(peer_height) + " (" + std::string(reason ? reason : "height-update") +
                ", known=" + std::to_string(known_height) + ") — requested headers");
        }
    } else if (logger_interface_) {
        logger_interface_->warning(
            "[P2PService] Peer " + peer_addr + " reports higher tip " +
            std::to_string(peer_height) + " but getheaders send failed");
    }
}

void P2PService::StopSchedulerTickLoop() {
    scheduler_tick_running_.store(false, std::memory_order_relaxed);
    if (scheduler_tick_thread_.joinable()) {
        scheduler_tick_thread_.join();
    }
}

void P2PService::StartPortMappingIfEnabled() {
    if (!config_ || !p2p_mgr_) {
        return;
    }

    const std::string mode_value = config_->GetString("p2p.portmap", "");
    const bool upnp_enabled = config_->GetBool("p2p.upnp", false);
    const bool natpmp_enabled = config_->GetBool("p2p.natpmp", false);
    auto mode = network::ParsePortMappingMode(mode_value, upnp_enabled, natpmp_enabled);
    {
        std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
        port_mapping_requested_ = !mode_value.empty() || upnp_enabled || natpmp_enabled;
        port_mapping_active_ = false;
        if (p2p_mgr_) p2p_mgr_->set_port_mapping_active(false);  // Gap 2 A.1 mirror
        port_mapping_mode_ = network::PortMappingModeName(mode);
        port_mapping_protocol_.clear();
        port_mapping_external_address_.clear();
        port_mapping_external_port_ = 0;
        port_mapping_message_ = port_mapping_requested_ ? "pending" : "not requested";
    }

    if (mode_value.empty() && !upnp_enabled && !natpmp_enabled) {
        return;
    }

    if (mode == network::PortMappingMode::Disabled) {
        {
            std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
            port_mapping_message_ = "disabled";
        }
        if (logger_interface_) {
            logger_interface_->info("[P2PService] P2P port mapping disabled");
        }
        return;
    }

    const bool listen_enabled = config_->GetBool("p2p.listen", true);
    if (!listen_enabled) {
        {
            std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
            port_mapping_message_ = "requested but listen=0";
        }
        if (logger_interface_) {
            logger_interface_->warning("[P2PService] P2P port mapping requested but listen=0; skipping");
        }
        return;
    }

    if (!p2p_mgr_->WaitUntilListening(std::chrono::seconds(5))) {
        {
            std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
            port_mapping_message_ = "requested but listener was not ready";
        }
        if (logger_interface_) {
            logger_interface_->warning("[P2PService] P2P port mapping requested but listener is not ready; skipping");
        }
        return;
    }

    network::PortMappingConfig portmap_config;
    portmap_config.mode = mode;
    portmap_config.internal_port = p2p_mgr_->get_listen_port();
    portmap_config.external_port = static_cast<uint16_t>(
        config_->GetInt("p2p.external_port", portmap_config.internal_port));
    portmap_config.lifetime_seconds = config_->GetInt("p2p.portmap_lifetime", 7200);
    const int discovery_timeout_seconds = std::max(
        5, config_->GetInt("p2p.portmap_discovery_timeout", 45));
    {
        std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
        port_mapping_external_port_ = portmap_config.external_port;
        port_mapping_message_ = "discovering (background)";
    }

    // Defensive: if a prior worker is still around (e.g. Start was somehow
    // re-entered), join it before launching another.
    if (port_mapping_worker_.joinable()) {
        port_mapping_cancel_.store(true);
        port_mapping_worker_.join();
    }
    port_mapping_cancel_.store(false);

    if (logger_interface_) {
        logger_interface_->info("[P2PService] P2P port mapping discovery dispatched in background (" +
                                network::PortMappingModeName(mode) + ", timeout " +
                                std::to_string(discovery_timeout_seconds) + "s)");
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(discovery_timeout_seconds);
    portmap_config.should_abort = [this, deadline]() {
        return port_mapping_cancel_.load() ||
               std::chrono::steady_clock::now() >= deadline;
    };

    port_mapping_worker_ = std::thread([this, portmap_config, mode, deadline,
                                        discovery_timeout_seconds]() {
        // Run the slow miniupnpc / libnatpmp discovery off the daemon
        // init thread. The session is local to this thread until either
        // it's handed off (success) or torn down (failure / cancel).
        auto session = std::make_unique<network::PortMappingSession>();
        const auto result = session->Start(portmap_config);

        if (port_mapping_cancel_.load()) {
            // Shutdown raced ahead; release any mapping the router accepted
            // and exit without publishing.
            session->Stop();
            return;
        }

        // Deadline hit while session->Start was inside a blocking syscall.
        // Don't publish a router mapping the user already gave up on — drop
        // it and surface the timeout in the status.
        if (std::chrono::steady_clock::now() >= deadline) {
            session->Stop();
            {
                std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
                port_mapping_active_ = false;
                if (p2p_mgr_) p2p_mgr_->set_port_mapping_active(false);  // Gap 2 A.1 mirror
                port_mapping_protocol_.clear();
                port_mapping_external_address_.clear();
                port_mapping_message_ = "discovery timed out after " +
                                        std::to_string(discovery_timeout_seconds) + "s";
            }
            if (logger_interface_) {
                logger_interface_->warning("[P2PService] P2P port mapping discovery timed out after " +
                                           std::to_string(discovery_timeout_seconds) +
                                           "s (" + network::PortMappingModeName(mode) +
                                           "); outbound P2P still works");
            }
            return;
        }

        if (result.success) {
            std::string advertise_host;
            uint16_t advertise_port = 0;
            const bool have_advertise =
                !result.external_address.empty() &&
                ParseEndpoint(result.external_address,
                              portmap_config.external_port,
                              &advertise_host,
                              &advertise_port);
            {
                std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
                port_mapping_ = std::move(session);
                port_mapping_active_ = true;
                if (p2p_mgr_) p2p_mgr_->set_port_mapping_active(true);  // Gap 2 A.1 mirror
                port_mapping_protocol_ = result.protocol;
                port_mapping_external_address_ = result.external_address;
                port_mapping_message_ = result.message;
            }
            if (have_advertise && p2p_mgr_) {
                p2p_mgr_->add_advertised_address(advertise_host, advertise_port);
                if (logger_interface_) {
                    logger_interface_->info("[P2PService] Advertising port-mapped address: " +
                                            advertise_host + ":" + std::to_string(advertise_port));
                }
            } else if (!result.external_address.empty() && logger_interface_) {
                logger_interface_->warning("[P2PService] Port mapping succeeded but no public address was available for addr relay: " +
                                           result.external_address);
            }
            if (logger_interface_) {
                logger_interface_->info("[P2PService] P2P port mapped via " + result.protocol +
                                        ": " + result.message +
                                        (result.external_address.empty() ? "" : " (" + result.external_address + ")"));
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
                port_mapping_active_ = false;
                if (p2p_mgr_) p2p_mgr_->set_port_mapping_active(false);  // Gap 2 A.1 mirror
                port_mapping_protocol_.clear();
                port_mapping_external_address_.clear();
                port_mapping_message_ = result.message.empty() ? "unavailable" : result.message;
            }
            if (logger_interface_) {
                logger_interface_->warning("[P2PService] P2P port mapping unavailable (" +
                                           network::PortMappingModeName(mode) + "): " + result.message +
                                           "; outbound P2P still works");
            }
            // session destructs here; PortMappingSession::~Stop releases any
            // partial state.
        }
    });
}

void P2PService::StopPortMapping() {
    // Signal cancel first so a worker that's mid-discovery exits without
    // publishing or holding a stale router mapping. Then join: the worker
    // is bounded by the underlying miniupnpc / libnatpmp call timing out
    // (a few seconds in the worst case). We intentionally block shutdown
    // here rather than detach so the captured `this` stays valid.
    port_mapping_cancel_.store(true);
    if (port_mapping_worker_.joinable()) {
        port_mapping_worker_.join();
    }
    port_mapping_cancel_.store(false);

    std::unique_ptr<network::PortMappingSession> to_stop;
    {
        std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
        to_stop = std::move(port_mapping_);
    }
    if (to_stop) {
        if (logger_interface_ && to_stop->active()) {
            logger_interface_->info("[P2PService] Removing P2P port mapping via " +
                                    to_stop->protocol());
        }
        to_stop->Stop();
        to_stop.reset();
    }
    {
        std::lock_guard<std::mutex> lock(port_mapping_status_mutex_);
        port_mapping_active_ = false;
        if (p2p_mgr_) p2p_mgr_->set_port_mapping_active(false);  // Gap 2 A.1 mirror
        port_mapping_protocol_.clear();
        if (port_mapping_requested_) {
            port_mapping_message_ = "stopped";
        }
    }
}

bool P2PService::Init(DaemonContext& ctx) {
    // Wire dependencies from context
    logger_ = std::dynamic_pointer_cast<LoggerService>(ctx.logger);
    // Use dedicated p2p logger if available, fallback to shared logger
    logger_interface_ = ctx.p2p_logger ? ctx.p2p_logger : ctx.logger_interface;
    config_ = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    chainstate_ = std::dynamic_pointer_cast<ChainstateService>(ctx.chainstate);
    mempool_ = std::dynamic_pointer_cast<MempoolService>(ctx.mempool);
    prune_ = ctx.prune;
    address_manager_ = ctx.address_manager;

    if (!logger_interface_) {
        throw std::runtime_error("[P2PService] Logger interface dependency missing");
    }
    if (!config_) {
        logger_interface_->error("[P2PService] Config dependency missing");
        return false;
    }

    // Note: Chainstate and Mempool are optional during Init
    // They will be wired in Start() when available

    // Get P2P configuration from config
    listen_port_ = static_cast<uint16_t>(config_->P2PPort());
    external_ip_ = config_->GetString("externalip", "");
    onion_proxy_ = config_->GetString("p2p.onion", "");
    onion_proxy_configured_ = !onion_proxy_.empty();
    onion_proxy_auto_detected_ = false;
    onion_proxy_reachable_ = false;
    onion_proxy_message_ = onion_proxy_configured_ ? "configured, not probed yet" : "disabled";
    offline_mode_ = config_->GetBool("p2p.offline", false);

    // issue #214: regtest-only overrides for the in-daemon staleness-recovery
    // clock, so an integration test can exercise the 600s default in seconds.
    // Gated to regtest (like the announce-suppression hook) so these test-only
    // env vars can never alter staleness behavior on a real mainnet/testnet node.
    // A real ops tuning knob, if ever wanted, belongs in a proper config option.
    if (Params().name == "regtest") {
    if (const char* env = std::getenv("DINERO_TEST_STALENESS_THRESHOLD_SECS")) {
        try {
            staleness_threshold_ = std::chrono::seconds(std::max(1, std::stoi(env)));
            if (logger_interface_) {
                logger_interface_->warning("[P2PService] Override: staleness_threshold=" +
                                           std::to_string(staleness_threshold_.count()) + "s");
            }
        } catch (const std::exception& e) {
            if (logger_interface_) {
                logger_interface_->warning(
                    "[P2PService] Ignoring malformed DINERO_TEST_STALENESS_THRESHOLD_SECS: " +
                    std::string(e.what()));
            }
        }
    }
    if (const char* env = std::getenv("DINERO_TEST_STALENESS_GETHEADERS_INTERVAL_SECS")) {
        try {
            staleness_getheaders_interval_ = std::chrono::seconds(std::max(1, std::stoi(env)));
            if (logger_interface_) {
                logger_interface_->warning("[P2PService] Override: staleness_getheaders_interval=" +
                                           std::to_string(staleness_getheaders_interval_.count()) + "s");
            }
        } catch (const std::exception& e) {
            if (logger_interface_) {
                logger_interface_->warning(
                    "[P2PService] Ignoring malformed DINERO_TEST_STALENESS_GETHEADERS_INTERVAL_SECS: " +
                    std::string(e.what()));
            }
        }
    }
    }  // regtest-only staleness overrides

    // Get datadir for peers.dat persistence
    std::string datadir = config_->DataDir();
    peers_file_path_ = datadir + "/peers.dat";

    // Parse seed nodes from config (support both addnode and connect)
    std::string addnode = config_->GetString("addnode", "");
    std::string connect = config_->GetString("p2p.connect", "");

    // Parse addnode addresses
    if (!addnode.empty()) {
        // Support comma-separated seed nodes
        std::istringstream ss(addnode);
        std::string node;
        while (std::getline(ss, node, ',')) {
            if (!node.empty()) {
                seed_nodes_.push_back(node);
            }
        }
    }

    // Parse connect addresses (--connect flag)
    if (!connect.empty()) {
        // Support comma-separated seed nodes
        std::istringstream ss(connect);
        std::string node;
        while (std::getline(ss, node, ',')) {
            if (!node.empty()) {
                seed_nodes_.push_back(node);
                logger_interface_->info("[P2PService] Will connect to: " + node);
            }
        }
    }

    logger_interface_->info("[P2PService] Initializing P2P networking...");
    logger_interface_->info("[P2PService]   Listen port: " + std::to_string(listen_port_));
    logger_interface_->info("[P2PService]   External IP: " + (external_ip_.empty() ? "(auto-detect)" : external_ip_));
    logger_interface_->info("[P2PService]   Onion proxy: " + (onion_proxy_.empty() ? "(disabled)" : onion_proxy_));
    logger_interface_->info("[P2PService]   Peers database: " + peers_file_path_);
    logger_interface_->info("[P2PService]   Seed nodes: " + std::to_string(seed_nodes_.size()));
    logger_interface_->info("[P2PService]   Offline mode: " + std::string(offline_mode_ ? "true" : "false"));

    try {
        // Create P2PManager instance
        p2p_mgr_ = std::make_unique<::P2PManager>(listen_port_, external_ip_);
        p2p_mgr_->set_peer_height_updated_handler(
            [this](const std::string& peer_addr, uint32_t height) {
                MaybeRequestHeadersForPeerTip(peer_addr, height, "peer-height-update");
            });
        if (!onion_proxy_.empty()) {
            if (IsOnionAutoValue(onion_proxy_)) {
                const std::vector<std::pair<std::string, uint16_t>> candidates = {
                    {"127.0.0.1", 9050},
                    {"127.0.0.1", 9150}
                };
                std::vector<std::string> probe_messages;
                for (const auto& [candidate_host, candidate_port] : candidates) {
                    p2p_mgr_->set_onion_proxy(candidate_host, candidate_port, false);
                    std::string probe_message;
                    if (p2p_mgr_->probe_onion_proxy(&probe_message)) {
                        onion_proxy_auto_detected_ = true;
                        onion_proxy_reachable_ = true;
                        onion_proxy_message_ = "auto-detected " + probe_message;
                        onion_proxy_ = candidate_host + ":" + std::to_string(candidate_port);
                        logger_interface_->info("[P2PService] Onion transport " + onion_proxy_message_);
                        break;
                    }
                    probe_messages.push_back(probe_message);
                }
                if (!onion_proxy_reachable_) {
                    p2p_mgr_->set_onion_proxy("", 0, false);
                    onion_proxy_message_ =
                        "auto-detect did not find Tor SOCKS5 on 127.0.0.1:9050 or 127.0.0.1:9150";
                    if (!probe_messages.empty()) {
                        onion_proxy_message_ += " (" + probe_messages.front() + ")";
                    }
                    logger_interface_->warning("[P2PService] Onion transport " + onion_proxy_message_);
                }
            } else {
                std::string proxy_host;
                uint16_t proxy_port = 0;
                if (ParseEndpoint(onion_proxy_, 9050, &proxy_host, &proxy_port)) {
                    p2p_mgr_->set_onion_proxy(proxy_host, proxy_port);
                    std::string probe_message;
                    onion_proxy_reachable_ = p2p_mgr_->probe_onion_proxy(&probe_message);
                    onion_proxy_message_ = probe_message;
                    if (onion_proxy_reachable_) {
                        logger_interface_->info("[P2PService] Onion transport " + onion_proxy_message_);
                    } else {
                        logger_interface_->warning("[P2PService] Onion transport configured but unavailable: " +
                                                   onion_proxy_message_ +
                                                   "; onion peers will stay unreachable until Tor/SOCKS5 is available");
                    }
                } else {
                    onion_proxy_message_ = "invalid onion proxy endpoint: " + onion_proxy_;
                    logger_interface_->warning("[P2PService] Ignoring " + onion_proxy_message_);
                }
            }
        }

        // NAT traversal Phase 1A: load (or generate) the daemon's node-identity
        // keypair and hand it to P2PManager. Once present, perform_handshake
        // will exchange the proven `dineroid` message with NODE_DINERO_V2 peers.
        // Identity is stored in <datadir>/node_identity.dat. Failure is
        // tolerated — legacy handshake still works without identity, and the
        // relay subsystem in later phases is gated on identity_proven so it
        // simply won't engage on this node.
        {
            auto node_identity = std::make_shared<dinero::daemon::NodeIdentity>();
            const std::string datadir = config_ ? config_->DataDir() : std::string{};
            if (datadir.empty() || !node_identity->initialize(datadir)) {
                logger_interface_->warning(
                    "[P2PService] Could not load/generate node identity at " + datadir +
                    "; dineroid handshake will be skipped, relay subsystem disabled");
            } else {
                p2p_mgr_->set_node_identity(node_identity);
            }
        }

        // NAT traversal Phase C3 slice 4a: parse comma-separated relay
        // endpoints out of `relayregister` config and hand them to the
        // P2PManager. Format: "host:port,host:port,...". Whitespace is
        // tolerated. Empty (the default) means "no client-side
        // registration"; daemon still accepts inbound registrations
        // if NODE_RELAY is advertised.
        {
            const std::string raw = config_ ? config_->GetString("relayregister", "") : "";
            std::vector<std::string> endpoints;
            std::string cur;
            for (char c : raw) {
                if (c == ',') {
                    if (!cur.empty()) endpoints.push_back(cur);
                    cur.clear();
                } else if (!std::isspace(static_cast<unsigned char>(c))) {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) endpoints.push_back(cur);
            if (!endpoints.empty()) {
                p2p_mgr_->set_configured_relay_endpoints(endpoints);
                logger_interface_->info(
                    "[P2PService] Configured " + std::to_string(endpoints.size()) +
                    " relay endpoint(s) via relayregister=");
            }
        }

        // Week 4: Bridge pattern removed - all code now uses ctx_->p2p->get()
        // Legacy global dinero::legacy::g_peer_manager() is no longer set here
        logger_interface_->info("[P2PService] P2PManager created successfully");
        return true;

    } catch (const std::exception& e) {
        logger_interface_->error("[P2PService] Failed to create P2PManager: " + std::string(e.what()));
        return false;
    }
}

bool P2PService::Start() {
    if (!p2p_mgr_) {
        logger_interface_->error("[P2PService] Cannot start - P2P manager not initialized");
        return false;
    }

    logger_interface_->info("[P2PService] Starting P2P networking...");

    try {
        if (offline_mode_) {
            reconnect_targets_.clear();
            p2p_mgr_->set_network_active(false);
            logger_interface_->info(
                "[P2PService] Offline mode enabled (p2p.offline=1) - "
                "skipping peer bootstrap, listeners, and networking threads");
            return true;
        }

        const std::string connect_config = config_ ? config_->GetString("p2p.connect", "") : "";
        const bool connect_only = !connect_config.empty();

        if (address_manager_) {
            p2p_mgr_->set_address_manager(address_manager_->getManager());
        } else {
            logger_interface_->warning("[P2PService] Address manager service unavailable; addr relay will use seeds only");
        }

        // Load persistent peer database
        if (!connect_only && !peers_file_path_.empty()) {
            try {
                p2p_mgr_->load_peers(peers_file_path_);
                logger_interface_->info("[P2PService] Loaded peers from: " + peers_file_path_);
            } catch (const std::exception& e) {
                logger_interface_->warning("[P2PService] Could not load peers database: " +
                               std::string(e.what()));
                logger_interface_->info("[P2PService] Starting with empty peer list");
            }
        } else if (connect_only) {
            logger_interface_->info("[P2PService] Connect-only mode: skipping peers.dat bootstrap");
        }

        if (auto* ctx = DaemonContext::instance(); ctx && ctx->peer_scoring) {
            size_t restored_bans = 0;
            const auto now = std::chrono::system_clock::now();
            for (const auto& banned_peer : ctx->peer_scoring->getBannedPeers()) {
                const auto score = ctx->peer_scoring->getPeerScore(banned_peer);
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::seconds>(score.ban_until - now);
                if (remaining.count() > 0 && p2p_mgr_->ban_peer(banned_peer, remaining)) {
                    ++restored_bans;
                }
            }
            if (restored_bans > 0) {
                logger_interface_->info("[P2PService] Restored " + std::to_string(restored_bans) +
                                        " peer ban(s) into P2P enforcement");
            }
        }

        // Add user-configured seed nodes FIRST (from --connect or --addnode)
        // Priority: User seeds are tried before hardcoded seeds
        reconnect_targets_.clear();
        std::unordered_set<std::string> reconnect_seen;
        std::string network = Params().name;  // "mainnet" | "testnet" | "regtest"
        uint16_t p2p_port = Params().p2p_port;  // Default P2P port for this network

        if (!external_ip_.empty()) {
            std::string advertise_host;
            uint16_t advertise_port = 0;
            if (ParseEndpoint(external_ip_, p2p_port, &advertise_host, &advertise_port)) {
                p2p_mgr_->add_advertised_address(advertise_host, advertise_port);
                const std::string overlay_note = IsOnionAddress(advertise_host)
                    ? " (onion overlay endpoint; clearnet identity remains separate)"
                    : "";
                logger_interface_->info("[P2PService] Advertising configured external address: " +
                                        advertise_host + ":" + std::to_string(advertise_port) +
                                        overlay_note);
            } else {
                logger_interface_->warning("[P2PService] Ignoring invalid externalip for addr relay: " +
                                           external_ip_);
            }
        }

        for (const auto& seed : seed_nodes_) {
            std::string host;
            uint16_t port = 0;
            if (ParseEndpoint(seed, p2p_port, &host, &port)) {
                p2p_mgr_->add_seed_node(host, port);
                AddReconnectTarget(reconnect_targets_, reconnect_seen, host, port);
                logger_interface_->info("[P2PService] Added user seed node (priority): " + seed);
            } else {
                logger_interface_->warning("[P2PService] Invalid seed node format: " + seed +
                               " (expected host[:port])");
            }
        }

        // Register anchor peers for eclipse attack recovery
        // Anchor peers are added FIRST as priority connections and tracked for auto-reconnect
        // Self-loop guard: a node's own external IP appears in MAINNET_ANCHOR_PEERS for fleet
        // members. Skip those entries — see LocalInterfaceIps() comment for the IBD stall this
        // prevents.
        if (!connect_only) {
            auto anchor_peers = dinero::config::getAnchorPeers(network);
            size_t anchors_added = 0;
            for (const auto& anchor : anchor_peers) {
                if (IsLocalInterfaceIp(anchor.hostname)) {
                    logger_interface_->info("[P2PService] Skipping anchor peer (matches local interface): " +
                                            anchor.hostname);
                    continue;
                }
                uint16_t port = (anchor.port != 0) ? anchor.port : p2p_port;
                p2p_mgr_->add_seed_node(anchor.hostname, port);
                AddReconnectTarget(reconnect_targets_, reconnect_seen, anchor.hostname, port);
                logger_interface_->info("[P2PService] Added anchor peer: " + anchor.hostname + ":" +
                                        std::to_string(port) + " (" + anchor.region + ")");
                ++anchors_added;
            }
            logger_interface_->info("[P2PService] Registered " + std::to_string(anchors_added) +
                                    " anchor peers for eclipse protection (" +
                                    std::to_string(anchor_peers.size() - anchors_added) +
                                    " skipped as self)");
        }

        if (!connect_only) {
            // Add hardcoded seed nodes for the active network (tried after user seeds)
            // Same self-loop guard as anchors above.
            auto hardcoded_seeds = dinero::config::getSeedNodes(network);

            logger_interface_->info("[P2PService] Loading " + std::to_string(hardcoded_seeds.size()) +
                         " hardcoded seed nodes for " + network);

            for (const auto& seed : hardcoded_seeds) {
                if (IsLocalInterfaceIp(seed.hostname)) {
                    logger_interface_->info("[P2PService] Skipping hardcoded seed (matches local interface): " +
                                            seed.hostname);
                    continue;
                }
                uint16_t port = (seed.port != 0) ? seed.port : p2p_port;
                p2p_mgr_->add_seed_node(seed.hostname, port);
                AddReconnectTarget(reconnect_targets_, reconnect_seen, seed.hostname, port);
                logger_interface_->info("[P2PService] Added hardcoded seed: " + seed.hostname + ":" +
                             std::to_string(port) + " (" + seed.region + ")");
            }

            // Resolve DNS seeds for additional peer discovery.
            // Resolve first, then drop any A records that point at our own interfaces. Only
            // register the hostname if at least one non-local A record remains — otherwise the
            // hostname resolves entirely to us and would create a self-loop on every reconnect.
            auto dns_seeds = dinero::config::getDnsSeeds(network);
            for (const auto& dns_seed : dns_seeds) {
                struct addrinfo hints{}, *result = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                int err = getaddrinfo(dns_seed.c_str(), nullptr, &hints, &result);

                std::vector<std::string> non_self_ips;
                if (err == 0 && result) {
                    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET,
                                  &reinterpret_cast<struct sockaddr_in*>(rp->ai_addr)->sin_addr,
                                  ip, sizeof(ip));
                        if (IsLocalInterfaceIp(ip)) {
                            logger_interface_->info("[P2PService] Skipping DNS seed " + dns_seed +
                                                    " -> " + ip + " (matches local interface)");
                            continue;
                        }
                        non_self_ips.emplace_back(ip);
                    }
                    freeaddrinfo(result);
                } else {
                    logger_interface_->warning("[P2PService] DNS seed resolution failed for " +
                                     dns_seed + ": " + gai_strerror(err));
                }

                if (non_self_ips.empty()) {
                    logger_interface_->info("[P2PService] DNS seed " + dns_seed +
                                            " resolved only to local interfaces — not registering");
                    continue;
                }

                // Keep hostname in seed list so reconnect attempts can re-resolve later.
                p2p_mgr_->add_seed_node(dns_seed, p2p_port);
                AddReconnectTarget(reconnect_targets_, reconnect_seen, dns_seed, p2p_port);
                for (const auto& ip : non_self_ips) {
                    p2p_mgr_->add_seed_node(ip, p2p_port);
                    AddReconnectTarget(reconnect_targets_, reconnect_seen, ip, p2p_port);
                    logger_interface_->info("[P2PService] DNS seed " + dns_seed + " -> " +
                                 ip + ":" + std::to_string(p2p_port));
                }
            }
        } else {
            logger_interface_->info("[P2PService] Connect-only mode: skipping hardcoded and DNS seeds");
        }

        last_reconnect_probe_ = std::chrono::steady_clock::now();
        logger_interface_->info("[P2PService] Reconnect targets armed: " +
                                std::to_string(reconnect_targets_.size()));

        // Wire message handler
        p2p_mgr_->set_message_handler([this](const std::string& peer_addr,
                                             const ::P2PMessage& msg) {
            HandleP2PMessage(peer_addr, msg);
        });

        // Wire peer connection handlers
        // P2P sync fix: Trigger header sync when peer with higher height connects
        p2p_mgr_->set_peer_connected_handler([this](const std::string& peer_addr) {
            logger_interface_->info("[P2PService] Peer connected: " + peer_addr);

            if (auto* ctx = DaemonContext::instance(); ctx && ctx->parallel_block_download) {
                ctx->parallel_block_download->registerPeers({peer_addr});
            }

            // Phase N.3 Fix: Always request headers from connected peers
            // Bitcoin protocol standard: getheaders on every new connection
            // This ensures header sync initiates even when peer starts at height 0 (genesis)
            if (chainstate_ && p2p_mgr_) {
                auto* peer_info = p2p_mgr_->get_peer_info(peer_addr);
                if (peer_info) {
                    uint32_t our_height = chainstate_->getBlockHeight();
                    uint32_t peer_height = peer_info->best_known_height;

                    // Update network height estimate for IBD detection
                    chainstate_->UpdateNetworkHeight(peer_height);

                    logger_interface_->info("[P2PService] Peer " + peer_addr +
                        " height=" + std::to_string(peer_height) +
                        ", our height=" + std::to_string(our_height));

                    // Always request headers from new peers (Bitcoin protocol standard)
                    // HeaderSyncManager will compare chain work and decide if we need to sync
                    // This handles: divergent chains at same height, genesis sync, and rare
                    // cases where peer has lower height but more cumulative work
                    logger_interface_->info("[P2PService] Requesting headers from peer " + peer_addr);

                    // Generate block locator and send getheaders
                    auto locator = chainstate_->GenerateBlockLocator();
                    std::vector<std::string> locator_hex;
                    for (const auto& hash : locator) {
                        locator_hex.push_back(hash.GetHex());
                    }

                    auto getheaders_msg = ::P2PMessage::create_getheaders(locator_hex);
                    p2p_mgr_->send_to_peer(peer_addr, getheaders_msg);
                    logger_interface_->info("[P2PService] Sent getheaders to " + peer_addr);

                    if (auto* ctx = DaemonContext::instance();
                        ctx && ctx->block_download && ctx->header_chain && ctx->chainstate &&
                        ctx->block_download->HasSendGetDataCallback()) {
                        auto* best = ctx->header_chain->GetBestHeader();
                        auto* active = ctx->chainstate->GetActiveTip();
                        if (best && active && best->height > active->height) {
                            logger_interface_->info("[P2PService] Peer available for persisted header backlog "
                                                   "(header=" + std::to_string(best->height) +
                                                   ", active=" + std::to_string(active->height) +
                                                   ") — bootstrapping block download");
                            ctx->block_download->OnHeadersProcessed();
                            ctx->block_download->Tick();
                        }
                    }

                    if (our_height > peer_height) {
                        ChainDB* chain_db = chainstate_->GetChainDB();
                        std::vector<std::string> proactive_headers;
                        int burst_start_height = 0;
                        int burst_end_height = 0;
                        if (BuildProactiveHeaderBurst(chain_db,
                                                      peer_height,
                                                      &proactive_headers,
                                                      &burst_start_height,
                                                      &burst_end_height)) {
                            auto headers_msg = ::P2PMessage::create_headers(proactive_headers);
                            if (p2p_mgr_->send_to_peer(peer_addr, headers_msg)) {
                                logger_interface_->info("[P2PService] Proactive headers push to " + peer_addr +
                                                        " (" + std::to_string(proactive_headers.size()) +
                                                        " headers, heights " + std::to_string(burst_start_height) +
                                                        "-" + std::to_string(burst_end_height) + ")");
                            }
                        }
                    }
                }
            }
        });

        p2p_mgr_->set_peer_disconnected_handler([this](const std::string& peer_addr) {
            logger_interface_->info("[P2PService] Peer disconnected: " + peer_addr);
            if (auto* ctx = DaemonContext::instance(); ctx && ctx->parallel_block_download) {
                ctx->parallel_block_download->notifyPeerDisconnected(peer_addr);
                ctx->parallel_block_download->unregisterPeer(peer_addr);
            }
        });

        // P2P sync fix: Wire height provider for version handshake
        // This ensures peers receive our actual chain height instead of 0
        if (chainstate_) {
            p2p_mgr_->set_height_provider([this]() -> uint32_t {
                return chainstate_->getBlockHeight();
            });
            logger_interface_->info("[P2PService] Height provider wired to chainstate");
        } else {
            logger_interface_->warning("[P2PService] Chainstate not available - height will be 0 in handshakes");
        }

        // Wire service flags provider (prune-aware: NODE_NETWORK_LIMITED after snapshot)
        // Peers see our actual capability and won't request blocks we can't serve.
        {
            auto prune_svc = prune_;  // capture shared_ptr by value for lambda safety
            const bool bridge_enabled = config_->GetBool("utreexo-bridge", true);
            // NAT traversal: p2p.relay is tri-state. Explicit on/off wins;
            // auto (the default) advertises NODE_RELAY only while mining.
            // Relay data is still guarded by the P2PManager caps: 64 KB/s per
            // circuit, 5 MB/s global, and 50 GB/day.
            p2p_mgr_->set_service_flags_provider([this, prune_svc,
                                                  bridge_enabled]() -> uint64_t {
                uint64_t flags = ServiceFlags::NODE_UTREEXO;
                if (bridge_enabled) {
                    flags |= ServiceFlags::NODE_UTREEXO_BRIDGE;
                }
                if (prune_svc && prune_svc->isEnabled()) {
                    flags |= ServiceFlags::NODE_NETWORK_LIMITED;
                } else {
                    flags |= ServiceFlags::NODE_NETWORK;
                }
                // NAT traversal Phase 1A: announce post-verack dineroid capability.
                flags |= ServiceFlags::NODE_DINERO_V2;
                if (IsRelayRoleEnabled()) {
                    flags |= ServiceFlags::NODE_RELAY;
                }
                return flags;
            });
            bool pruned = prune_svc && prune_svc->isEnabled();
            logger_interface_->info("[P2PService] Service flags provider wired (pruned=" +
                         std::string(pruned ? "true" : "false") +
                         ", bridge=" + std::string(bridge_enabled ? "true" : "false") +
                         ", relay_mode=" + RelayMode() +
                         ", relay=" + std::string(IsRelayRoleEnabled() ? "true" : "false") + ")");
        }

        {
            std::string sync_profile = config_->GetString("sync-profile", "");
            std::transform(sync_profile.begin(), sync_profile.end(), sync_profile.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            std::string user_agent = p2p_mgr_->get_user_agent();
            if (sync_profile == "ios_utreexo") {
                user_agent = "/dinerod-ios-utreexo/";
            } else if (sync_profile == "mac_fullblock") {
                user_agent = "/dinerod-mac-fullblock/";
            }
            p2p_mgr_->set_user_agent(user_agent);
            logger_interface_->info("[P2PService] User agent set to " + user_agent);
        }

        // Start P2P networking (spawns threads)
        if (!p2p_mgr_->start()) {
            logger_interface_->error("[P2PService] Failed to start P2P manager");
            return false;
        }

        logger_interface_->info("[P2PService] P2P networking started successfully");
        logger_interface_->info("[P2PService] Listening on port " + std::to_string(listen_port_));

        // Stage B: surface QUIC relay-transport readiness at startup so the
        // operator can see whether encrypted relay DATA can flow on this binary
        // (relay registration rides TCP regardless; relay data on mainnet needs
        // the QUIC crypto bridge). Mirrors the getnetworkinfo.quic_transport field.
        {
            const auto quic = dinero::network::QuicTransport::CompileInfo();
            const bool relay_data_ready =
                quic.ngtcp2_available && quic.crypto_available && quic.mainnet_relay_ready;
            logger_interface_->info(
                "[P2PService] QUIC relay-transport: ngtcp2=" +
                std::string(quic.ngtcp2_available ? "yes" : "no") +
                " crypto=" + std::string(quic.crypto_available ? "yes" : "no") +
                " backend=" + quic.crypto_backend +
                " mainnet_ready=" + std::string(quic.mainnet_relay_ready ? "yes" : "no") +
                " => relay_data_ready=" + std::string(relay_data_ready ? "YES" : "NO") +
                (quic.disabled_reason.empty() ? "" : " (" + quic.disabled_reason + ")"));
        }

        StartPortMappingIfEnabled();

        // NAT traversal Phase C1: launch async STUN discovery. Independent
        // of UPnP/NAT-PMP — STUN works even on routers that don't speak
        // IGD/PCP, as long as outbound UDP is allowed. Result lands on the
        // StunClient internal reader thread, takes the stun_status_mutex_,
        // and (on success) calls add_advertised_address. Surface in
        // NetworkStatus.stun_discovered_address for the qt UI.
        StartStunDiscoveryIfEnabled();

        // Phase N hotfix: keep headers/block schedulers moving in P2PService mode.
        StartSchedulerTickLoop();

        // Log initial peer count
        size_t peer_count = p2p_mgr_->get_peer_count();
        logger_interface_->info("[P2PService] Connected peers: " + std::to_string(peer_count));

        return true;

    } catch (const std::exception& e) {
        StopSchedulerTickLoop();
        StopPortMapping();
        logger_interface_->error("[P2PService] Failed to start: " + std::string(e.what()));
        return false;
    }
}

void P2PService::Stop() {
    StopSchedulerTickLoop();

    if (!p2p_mgr_) {
        if (logger_interface_) {
            logger_interface_->info("[P2PService] Already stopped");
        }
        return;
    }

    logger_interface_->info("[P2PService] Stopping P2P networking...");

    try {
        // Get final peer count before shutdown
        size_t peer_count = p2p_mgr_->get_peer_count();
        logger_interface_->info("[P2PService] Disconnecting " + std::to_string(peer_count) + " peer(s)...");

        // Save persistent peer database
        if (!offline_mode_ && !peers_file_path_.empty()) {
            try {
                p2p_mgr_->save_peers_with_seeds(peers_file_path_);
                logger_interface_->info("[P2PService] Saved peers to: " + peers_file_path_);
            } catch (const std::exception& e) {
                logger_interface_->warning("[P2PService] Could not save peers database: " +
                               std::string(e.what()));
            }
        }

        StopPortMapping();

        // NAT traversal Phase C1: tear down the STUN client (closes the
        // ephemeral UDP socket + joins the reader thread). Idempotent.
        stun_client_.reset();

        // Stop P2P manager (disconnects all peers, stops threads)
        p2p_mgr_->stop();

        logger_interface_->info("[P2PService] P2P networking stopped cleanly");

        // Week 4: Bridge pattern removed - no need to clear legacy global
        // Reset the unique_ptr
        p2p_mgr_.reset();

    } catch (const std::exception& e) {
        logger_interface_->error("[P2PService] Error during shutdown: " + std::string(e.what()));
        StopPortMapping();
        // Still reset to avoid dangling pointer
        p2p_mgr_.reset();
    }
}

void P2PService::HandleP2PMessage(const std::string& peer_addr, const ::P2PMessage& msg) {
    // Route messages to appropriate handlers
    // Debug: Log all messages for relay debugging
    logger_interface_->info("[P2PService] HandleP2PMessage: cmd='" + msg.command + "' from " + peer_addr);

    if (msg.command == "block" && OnNewBlock) {
        OnNewBlock(peer_addr, msg);
    }
    else if (msg.command == "tx" && OnNewTx) {
        if (!checkTxRate(peer_addr)) {
            logger_interface_->warning("[P2PService] TX rate limit exceeded for " + peer_addr +
                                       " — dropping message (RBF flood protection)");
            return;
        }
        OnNewTx(peer_addr, msg);
    }
    else if (msg.command == "inv" && OnInv) {
        OnInv(peer_addr, msg);
    }
    else if (msg.command == "getdata" && OnGetData) {
        logger_interface_->info("[P2PService] Routing getdata to OnGetData handler");
        OnGetData(peer_addr, msg);
    }
    // Phase C.3: Headers-first sync messages
    else if (msg.command == "getheaders" && OnGetHeaders) {
        OnGetHeaders(peer_addr, msg);
    }
    else if (msg.command == "headers" && OnHeaders) {
        if (!checkHeadersRate(peer_addr)) {
            logger_interface_->warning("[P2PService] Headers rate limit exceeded for " + peer_addr +
                                       " — dropping message (DoS protection)");
            return;
        }
        OnHeaders(peer_addr, msg);
    }
    else if (msg.command == "cmpctblock" && OnCompactBlock) {
        OnCompactBlock(peer_addr, msg);
    }
    else if (msg.command == "getblocktxn" && OnGetBlockTxn) {
        OnGetBlockTxn(peer_addr, msg);
    }
    else if (msg.command == "blocktxn" && OnBlockTxn) {
        OnBlockTxn(peer_addr, msg);
    }
    // Phase P.3: Utreexo block relay (block + proof combined)
    else if (msg.command == "utxoblk" && OnUtxoBlock) {
        logger_interface_->info("[P2PService] Routing utxoblk to OnUtxoBlock handler");
        OnUtxoBlock(peer_addr, msg);
    }
    // Phase #4: Utreexo tx relay (tx + per-input proofs)
    else if (msg.command == "utxotx" && OnUtxoTx) {
        logger_interface_->info("[P2PService] Routing utxotx to OnUtxoTx handler");
        OnUtxoTx(peer_addr, msg);
    }
    // Phase 7.4: Utreexo proof serving protocol (bridge node mode)
    else if ((msg.command == MessageCommands::GETUTREEXOPROOF ||
              msg.command == MessageCommands::GETUTREEXOPROOFS) &&
             OnGetUtreexoProof) {
        logger_interface_->info("[P2PService] Routing " + msg.command +
                                " to OnGetUtreexoProof handler");
        OnGetUtreexoProof(peer_addr, msg);
    }
    else if (msg.command == MessageCommands::GETUTREEXOHDRS && OnGetUtreexoHeaders) {
        logger_interface_->info("[P2PService] Routing " + msg.command +
                                " to OnGetUtreexoHeaders handler");
        OnGetUtreexoHeaders(peer_addr, msg);
    }
    // Phase 9.3: Proof gossip protocol (invproof/getproof/proofdata)
    else if (msg.command == MessageCommands::INVPROOF && OnInvProof) {
        logger_interface_->info("[P2PService] Routing invproof to OnInvProof handler");
        OnInvProof(peer_addr, msg);
    }
    else if (msg.command == MessageCommands::GETPROOF && OnGetProof) {
        logger_interface_->info("[P2PService] Routing getproof to OnGetProof handler");
        OnGetProof(peer_addr, msg);
    }
    else if (msg.command == MessageCommands::PROOFDATA && OnProofData) {
        logger_interface_->info("[P2PService] Routing proofdata to OnProofData handler");
        OnProofData(peer_addr, msg);
    }
    // Handle NOTFOUND via chainstate so only matching block requests are cleared.
    else if (msg.command == "notfound") {
        if (chainstate_) {
            chainstate_->HandleNotFoundFromPeer(peer_addr, msg.payload);
        }
    }
    else {
        // Log unknown messages for debugging
        logger_interface_->debug("[P2PService] Received message '" + msg.command +
                      "' from " + peer_addr + " (" +
                      std::to_string(msg.payload.size()) + " bytes)");
    }
}

bool P2PService::checkTxRate(const std::string& peer_addr) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    auto& state = tx_rate_tracker_[peer_addr];

    if (now_ms - state.window_start > 1000) {
        state.window_start = now_ms;
        state.headers_count = 0;  // reusing struct field for tx count
    }

    state.headers_count++;
    state.last_headers_time = now_ms;

    return state.headers_count <= MAX_TX_PER_SECOND;
}

bool P2PService::checkHeadersRate(const std::string& peer_addr) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    auto& state = headers_rate_tracker_[peer_addr];

    // Reset window if more than 1 second has passed
    if (now_ms - state.window_start > 1000) {
        state.window_start = now_ms;
        state.headers_count = 0;
    }

    state.headers_count++;
    state.last_headers_time = now_ms;

    return state.headers_count <= MAX_HEADERS_PER_SECOND;
}

} // namespace dinero
