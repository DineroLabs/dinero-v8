#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "consensus/chainparams.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/p2p_service.h"
#include "storage/chain_direct.h"

namespace dinero::mining {

enum class ReadinessReason {
    Ready,
    SafeMode,
    InitialBlockDownload,
    HeaderChainMismatch,
    NoChainstate,
    NoP2PService,
    P2PNotRunning,
    InsufficientPeers,
    PeerViewStale,
    BehindPeerTip,
    AheadOfNetworkView,
};

struct MiningReadinessPolicy {
    bool allow_isolated = false;
    bool skip_ibd_check = false;
    bool require_fresh_tip = true;
    bool require_header_convergence = true;
    bool require_recent_peer_activity = true;
    bool pause_if_ahead_of_network_view = false;
    int min_peers = 0;
    int max_tip_lag = 0;
    int max_tip_ahead = 0;
    int max_peer_staleness_seconds = 0;
};

struct MiningReadiness {
    bool ready = true;
    ReadinessReason reason = ReadinessReason::Ready;
    std::string reason_code = "ready";
    std::string message;

    bool p2p_running = false;
    bool is_ibd = false;
    bool pause_if_ahead_of_network_view = false;
    int64_t peer_count = 0;
    int64_t min_peers = 0;
    int64_t local_height = 0;
    int64_t network_height_estimate = 0;
    int64_t peer_best_height = 0;
    int64_t peer_median_height = 0;
    int64_t max_tip_lag = 0;
    int64_t max_tip_ahead = 0;
    int64_t peer_freshest_age_seconds = -1;
    int64_t max_peer_staleness_seconds = 0;
};

inline bool GetBoolWithFallback(const ConfigService* config,
                                const std::string& primary_key,
                                const std::string& fallback_key,
                                bool default_value) {
    if (!config) {
        return default_value;
    }
    const bool primary = config->GetBool(primary_key, default_value);
    if (primary != default_value) {
        return primary;
    }
    return config->GetBool(fallback_key, default_value);
}

inline int GetIntWithFallback(const ConfigService* config,
                              const std::string& primary_key,
                              const std::string& fallback_key,
                              int default_value) {
    if (!config) {
        return default_value;
    }
    const int primary = config->GetInt(primary_key, default_value);
    if (primary != default_value) {
        return primary;
    }
    return config->GetInt(fallback_key, default_value);
}

inline MiningReadinessPolicy LoadMiningReadinessPolicy(const ConfigService* config) {
    const auto& chain_params = Params();
    const bool is_regtest = chain_params.name == "regtest";
    const bool is_mainnet = chain_params.name == "mainnet";

    MiningReadinessPolicy policy;
    policy.allow_isolated = is_regtest;
    policy.skip_ibd_check = config ? config->GetBool("mine-during-ibd", false) : false;
    policy.require_fresh_tip = !is_regtest;
    policy.require_header_convergence = !is_regtest;
    policy.require_recent_peer_activity = !is_regtest;
    policy.pause_if_ahead_of_network_view = false;
    policy.min_peers = is_mainnet ? 1 : (is_regtest ? 0 : 1);
    policy.max_tip_lag = is_regtest ? 144 : 10;
    policy.max_tip_ahead = is_regtest ? 144 : 20;
    policy.max_peer_staleness_seconds = is_regtest ? 3600 : 300;

    if (config) {
        policy.allow_isolated = GetBoolWithFallback(
            config, "mining.readiness.allow_isolated", "mining.gbt.allow_isolated", policy.allow_isolated);
        policy.require_fresh_tip = GetBoolWithFallback(
            config, "mining.readiness.require_fresh_tip", "mining.gbt.require_fresh_tip", policy.require_fresh_tip);
        policy.require_header_convergence = config->GetBool(
            "mining.readiness.require_header_convergence", policy.require_header_convergence);
        policy.require_recent_peer_activity = config->GetBool(
            "mining.readiness.require_recent_peer_activity", policy.require_recent_peer_activity);
        policy.pause_if_ahead_of_network_view = config->GetBool(
            "mining.readiness.pause_if_ahead_of_network_view", policy.pause_if_ahead_of_network_view);
        policy.min_peers = GetIntWithFallback(
            config, "mining.readiness.min_peers", "mining.gbt.min_peers", policy.min_peers);
        policy.max_tip_lag = GetIntWithFallback(
            config, "mining.readiness.max_tip_lag", "mining.gbt.max_tip_lag", policy.max_tip_lag);
        policy.max_tip_ahead = config->GetInt("mining.readiness.max_tip_ahead", policy.max_tip_ahead);
        policy.max_peer_staleness_seconds = config->GetInt(
            "mining.readiness.max_peer_staleness_seconds", policy.max_peer_staleness_seconds);
    }

    policy.min_peers = std::max(policy.min_peers, 0);
    policy.max_tip_lag = std::max(policy.max_tip_lag, 0);
    policy.max_tip_ahead = std::max(policy.max_tip_ahead, 0);
    policy.max_peer_staleness_seconds = std::max(policy.max_peer_staleness_seconds, 0);
    return policy;
}

inline MiningReadiness EvaluateMiningReadiness(const ChainstateService* chainstate,
                                               const P2PService* p2p,
                                               const ConfigService* config) {
    MiningReadiness readiness;
    const MiningReadinessPolicy policy = LoadMiningReadinessPolicy(config);

    readiness.min_peers = policy.min_peers;
    readiness.max_tip_lag = policy.max_tip_lag;
    readiness.max_tip_ahead = policy.max_tip_ahead;
    readiness.max_peer_staleness_seconds = policy.max_peer_staleness_seconds;
    readiness.pause_if_ahead_of_network_view = policy.pause_if_ahead_of_network_view;

    if (!chainstate) {
        readiness.ready = false;
        readiness.reason = ReadinessReason::NoChainstate;
        readiness.reason_code = "no_chainstate";
        readiness.message = "Mining paused: chainstate service unavailable";
        return readiness;
    }

    readiness.local_height = static_cast<int64_t>(chainstate->getBlockHeight());
    readiness.is_ibd = chainstate->IsInIBD();
    readiness.network_height_estimate = static_cast<int64_t>(chainstate->GetIBDProgress().network_height);

    bool bootstrap_genesis_allowed = false;
    if (const auto* chain_db = chainstate->GetChainDB()) {
        const std::string best_hash = dinero::storage::GetBestBlockHash(const_cast<ChainDB*>(chain_db));
        bootstrap_genesis_allowed =
            readiness.local_height == 0 &&
            readiness.network_height_estimate == 0 &&
            best_hash == Params().genesis_hash;
    }

    if (chainstate->IsInSafeMode()) {
        readiness.ready = false;
        readiness.reason = ReadinessReason::SafeMode;
        readiness.reason_code = "safe_mode";
        readiness.message = "Mining paused: chainstate safe mode active (" +
                            chainstate->GetSafeModeReason() + ")";
        return readiness;
    }

    if (readiness.is_ibd && !policy.skip_ibd_check && !bootstrap_genesis_allowed) {
        readiness.ready = false;
        readiness.reason = ReadinessReason::InitialBlockDownload;
        readiness.reason_code = "initial_block_download";
        readiness.message = "Mining paused: initial block download still active";
        return readiness;
    }

    if (bootstrap_genesis_allowed) {
        readiness.reason = ReadinessReason::Ready;
        readiness.reason_code = "bootstrap_genesis";
        readiness.message = "Bootstrap mining allowed: fresh chain is still at genesis";
        readiness.p2p_running = p2p ? p2p->get().is_running() : false;
        return readiness;
    }

    if (policy.require_header_convergence &&
        !chainstate->GetSyncSnapshot().IsConverged()) {
        readiness.ready = false;
        readiness.reason = ReadinessReason::HeaderChainMismatch;
        readiness.reason_code = "header_chain_mismatch";
        readiness.message = "Mining backend unavailable: active tip does not match the best known header";
        return readiness;
    }

    if (policy.allow_isolated) {
        readiness.p2p_running = p2p ? p2p->get().is_running() : false;
        return readiness;
    }

    if (!p2p) {
        readiness.ready = false;
        readiness.reason = ReadinessReason::NoP2PService;
        readiness.reason_code = "no_p2p";
        readiness.message = "Mining paused: P2P service unavailable while isolated mining is disabled";
        return readiness;
    }

    readiness.p2p_running = p2p->get().is_running();
    if (!readiness.p2p_running) {
        readiness.ready = false;
        readiness.reason = ReadinessReason::P2PNotRunning;
        readiness.reason_code = "p2p_not_running";
        readiness.message = "Mining paused: P2P is not running while isolated mining is disabled";
        return readiness;
    }

    const auto peers = p2p->GetConnectedPeers();
    readiness.peer_count = static_cast<int64_t>(peers.size());
    if (readiness.peer_count < readiness.min_peers) {
        readiness.ready = false;
        readiness.reason = ReadinessReason::InsufficientPeers;
        readiness.reason_code = "insufficient_peers";
        readiness.message = "Mining paused: connected peers " +
                            std::to_string(readiness.peer_count) +
                            " below required minimum " +
                            std::to_string(readiness.min_peers);
        return readiness;
    }

    if (peers.empty()) {
        readiness.ready = false;
        readiness.reason = ReadinessReason::InsufficientPeers;
        readiness.reason_code = "insufficient_peers";
        readiness.message = "Mining paused: no connected peers";
        return readiness;
    }

    std::vector<uint32_t> heights;
    heights.reserve(peers.size());
    uint32_t peer_best_height = 0;
    auto freshest_seen = std::chrono::system_clock::time_point::min();
    bool have_recent_peer = false;

    for (const auto& peer : peers) {
        const uint32_t peer_height_hint = peer.best_known_height;
        peer_best_height = std::max(peer_best_height, peer_height_hint);
        if (peer_height_hint > 0) {
            heights.push_back(peer_height_hint);
        }
        if (peer.last_message_at > freshest_seen) {
            freshest_seen = peer.last_message_at;
            have_recent_peer = true;
        }
    }

    readiness.peer_best_height = static_cast<int64_t>(peer_best_height);

    if (!heights.empty()) {
        std::sort(heights.begin(), heights.end());
        readiness.peer_median_height = static_cast<int64_t>(heights[heights.size() / 2]);
    }

    if (policy.require_recent_peer_activity && have_recent_peer) {
        const auto now = std::chrono::system_clock::now();
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - freshest_seen).count();
        readiness.peer_freshest_age_seconds = age;
        if (age > readiness.max_peer_staleness_seconds) {
            readiness.ready = false;
            readiness.reason = ReadinessReason::PeerViewStale;
            readiness.reason_code = "peer_view_stale";
            readiness.message = "Mining paused: freshest peer activity is " +
                                std::to_string(age) + "s old";
            return readiness;
        }
    }

    const int64_t best_known_network_height =
        std::max(readiness.network_height_estimate, readiness.peer_best_height);

    if (policy.require_fresh_tip && best_known_network_height > 0) {
        if (best_known_network_height > readiness.local_height + readiness.max_tip_lag) {
            readiness.ready = false;
            readiness.reason = ReadinessReason::BehindPeerTip;
            readiness.reason_code = "behind_peer_tip";
            readiness.message = "Mining paused: local height " +
                                std::to_string(readiness.local_height) +
                                " is behind best known network height " +
                                std::to_string(best_known_network_height) +
                                " by more than " +
                                std::to_string(readiness.max_tip_lag) + " blocks";
            return readiness;
        }

        if (policy.pause_if_ahead_of_network_view &&
            best_known_network_height > 0 &&
            readiness.local_height > best_known_network_height + readiness.max_tip_ahead) {
            readiness.ready = false;
            readiness.reason = ReadinessReason::AheadOfNetworkView;
            readiness.reason_code = "ahead_of_network_view";
            readiness.message = "Mining paused: local height " +
                                std::to_string(readiness.local_height) +
                                " is ahead of best known network height " +
                                std::to_string(best_known_network_height) +
                                " by more than " +
                                std::to_string(readiness.max_tip_ahead) + " blocks";
            return readiness;
        }
    }

    return readiness;
}

}  // namespace dinero::mining
