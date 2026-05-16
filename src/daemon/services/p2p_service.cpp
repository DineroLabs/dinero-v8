#include "daemon/services/p2p_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/address_manager_service.h"
#include "daemon/daemon_context.h"
#include "config/seed_nodes.h"
#include "consensus/chainparams.h"
#include "consensus/block_download_scheduler.h"
#include "dinero/network/network_invariants.h"
#include "consensus/header_chain.h"  // P1 reorg fix: for HeaderChainSelector::GetBestHeader()
#include "network/local_interfaces.h"  // Self-loop filter (shared with P2PManager)
#include "network/port_mapper.h"
#include "p2p/block_download_scheduler.h"
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

void P2PService::StartSchedulerTickLoop() {
    if (scheduler_tick_running_.exchange(true)) {
        return;
    }

    scheduler_tick_thread_ = std::thread([this]() {
        if (logger_interface_) {
            logger_interface_->info("[P2PService] Scheduler tick loop started (interval=" +
                                    std::to_string(scheduler_tick_interval_.count()) + "ms)");
        }

        int activate_tick_counter = 0;
        size_t reconnect_cursor = 0;
        auto last_invariant_check = std::chrono::steady_clock::now();

        while (scheduler_tick_running_.load(std::memory_order_relaxed)) {
            if (auto* ctx = DaemonContext::instance()) {
                if (ctx->block_download) {
                    ctx->block_download->Tick();
                }
                if (ctx->parallel_block_download) {
                    ctx->parallel_block_download->processQueue();
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
            }
            std::this_thread::sleep_for(scheduler_tick_interval_);
        }

        if (logger_interface_) {
            logger_interface_->info("[P2PService] Scheduler tick loop stopped");
        }
    });
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
    if (mode_value.empty() && !upnp_enabled && !natpmp_enabled) {
        return;
    }

    auto mode = network::ParsePortMappingMode(mode_value, upnp_enabled, natpmp_enabled);
    if (mode == network::PortMappingMode::Disabled) {
        if (logger_interface_) {
            logger_interface_->info("[P2PService] P2P port mapping disabled");
        }
        return;
    }

    const bool listen_enabled = config_->GetBool("p2p.listen", true);
    if (!listen_enabled) {
        if (logger_interface_) {
            logger_interface_->warning("[P2PService] P2P port mapping requested but listen=0; skipping");
        }
        return;
    }

    if (!p2p_mgr_->WaitUntilListening(std::chrono::seconds(5))) {
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

    port_mapping_ = std::make_unique<network::PortMappingSession>();
    const auto result = port_mapping_->Start(portmap_config);
    if (result.success) {
        if (!result.external_address.empty()) {
            std::string advertise_host;
            uint16_t advertise_port = 0;
            if (ParseEndpoint(result.external_address,
                              portmap_config.external_port,
                              &advertise_host,
                              &advertise_port)) {
                p2p_mgr_->add_advertised_address(advertise_host, advertise_port);
                if (logger_interface_) {
                    logger_interface_->info("[P2PService] Advertising port-mapped address: " +
                                            advertise_host + ":" + std::to_string(advertise_port));
                }
            } else if (logger_interface_) {
                logger_interface_->warning("[P2PService] Port mapping succeeded but no public address was available for addr relay: " +
                                           result.external_address);
            }
        }
        if (logger_interface_) {
            logger_interface_->info("[P2PService] P2P port mapped via " + result.protocol +
                                    ": " + result.message +
                                    (result.external_address.empty() ? "" : " (" + result.external_address + ")"));
        }
    } else {
        port_mapping_.reset();
        if (logger_interface_) {
            logger_interface_->warning("[P2PService] P2P port mapping unavailable (" +
                                       network::PortMappingModeName(mode) + "): " + result.message +
                                       "; outbound P2P still works");
        }
    }
}

void P2PService::StopPortMapping() {
    if (port_mapping_) {
        if (logger_interface_ && port_mapping_->active()) {
            logger_interface_->info("[P2PService] Removing P2P port mapping via " +
                                    port_mapping_->protocol());
        }
        port_mapping_->Stop();
        port_mapping_.reset();
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
    offline_mode_ = config_->GetBool("p2p.offline", false);

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
    logger_interface_->info("[P2PService]   Peers database: " + peers_file_path_);
    logger_interface_->info("[P2PService]   Seed nodes: " + std::to_string(seed_nodes_.size()));
    logger_interface_->info("[P2PService]   Offline mode: " + std::string(offline_mode_ ? "true" : "false"));

    try {
        // Create P2PManager instance
        p2p_mgr_ = std::make_unique<::P2PManager>(listen_port_, external_ip_);

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
                logger_interface_->info("[P2PService] Advertising configured external address: " +
                                        advertise_host + ":" + std::to_string(advertise_port));
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
            p2p_mgr_->set_service_flags_provider([prune_svc, bridge_enabled]() -> uint64_t {
                uint64_t flags = ServiceFlags::NODE_UTREEXO;
                if (bridge_enabled) {
                    flags |= ServiceFlags::NODE_UTREEXO_BRIDGE;
                }
                if (prune_svc && prune_svc->isEnabled()) {
                    flags |= ServiceFlags::NODE_NETWORK_LIMITED;
                } else {
                    flags |= ServiceFlags::NODE_NETWORK;
                }
                return flags;
            });
            bool pruned = prune_svc && prune_svc->isEnabled();
            logger_interface_->info("[P2PService] Service flags provider wired (pruned=" +
                         std::string(pruned ? "true" : "false") +
                         ", bridge=" + std::string(bridge_enabled ? "true" : "false") + ")");
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

        StartPortMappingIfEnabled();

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
