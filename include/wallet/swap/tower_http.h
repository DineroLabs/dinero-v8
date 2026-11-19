#pragma once
// A watchtower armed over the network: the public tower
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 14).
//
// The tower itself is unchanged: it guards the packages in its inbox. This is
// the door in front of that inbox, and the buyer's way through it.
//
//   POST /v1/packages          body: the package text ("dinswap1t...")
//        200 {"hash": H}       kept (or already held); H is its SHA-256
//        400 {"error": why}    not a package the tower would guard
//        413                   too large        503  the tower is full
//   GET  /v1/armed/H
//        200 {"armed": true, "age": seconds}    the tower confirmed it is
//                                               guarding H that long ago
//        200 {"armed": false}
//   GET  /v1/health            200 {"ok": true}
//
// The tower holds only pre-signed transactions that pay the buyer's own
// addresses: whoever runs it cannot take coins. It can fail to act, which is
// the same as having no tower; the buyer's own node still does everything
// while it is online. It does learn the swap's terms and addresses.
//
// Both halves are plain functions over strings, so they are tested without a
// socket; tools/dinero_swap_tower_intake.cpp puts the first behind HTTP.

#include "wallet/swap/btc_esplora.h"  // BtcHttp
#include "wallet/swap/tower.h"

#include <functional>
#include <string>

namespace dinero::swap {

// ---- The tower's side -------------------------------------------------------

struct TowerHttpReply {
    int status{404};
    std::string body;  // JSON
};
// `method` is "GET" or "POST"; `target` the request path. Never throws.
TowerHttpReply HandleTowerHttpRequest(const std::string& method, const std::string& target, const std::string& body,
                                      const std::string& inbox_dir, const TowerIntakeLimits& limits, uint32_t now_unix);

// ---- The buyer's side -------------------------------------------------------

// Empty if `url` can be a tower's address: https://host[:port][/path], or plain
// http on this machine (tests). Otherwise what is wrong with it.
std::string TowerUrlProblem(const std::string& url);

// What SwapManager::SetTowerSink / SetTowerAck take. The sink throws
// std::runtime_error unless the tower answered that it holds exactly this
// package; the runner then tries again on its next tick and never funds unarmed.
std::function<void(const std::string& package_text)> MakeHttpTowerSink(std::string url, BtcHttp http);
std::function<bool(const std::string& swap_id, const std::string& package_hash)> MakeHttpTowerAck(std::string url,
                                                                                                BtcHttp http);

}  // namespace dinero::swap
