// swap.* RPCs: DIN <-> BTC atomic swaps from the local wallet (milestone 6 of
// docs/design/din-btc-atomic-swaps-v1-plan.md). Thin layer over
// dinero::swap::SwapManager, which the SwapService ticks in the background.
//
//   swap.offer  {din_amount_una, btc_amount_sat, btc_address,
//                [btc_lock_hours, din_lock_hours, expires_minutes, n_din, n_btc]}
//               Alice: returns {id, offer} — send the offer text to Bob.
//   swap.accept {text, [btc_refund_address]}
//               Bob pastes the offer (btc_refund_address required): returns
//               {id, accept} — send it back. Alice pastes the accept: the swap starts.
//   swap.list / swap.status {id} / swap.cancel {id}
//
// No RPC returns a secret, a private key or a raw session. The wallet must be
// encrypted and unlocked to start a swap; while it is locked swaps pause.
#include "daemon/services/swap_service.h"
#include "rpc/rpc_registry.h"
#include "wallet/wallet_manager.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"

#include <ctime>

namespace {

using namespace dinero;
using namespace dinero::swap;

din::Json Error(const std::string& message) {
    din::Json r(Json::objectValue);
    r["error"] = message;
    return r;
}

const char* RoleName(Role r) { return r == Role::DinSeller ? "din-seller" : "btc-seller"; }

din::Json ToJson(const SwapSummary& s) {
    din::Json j(Json::objectValue);
    j["id"] = s.id;
    j["index"] = s.index;
    j["wallet_locked"] = s.wallet_locked;
    if (s.wallet_locked) {
        j["state"] = "paused: wallet locked";
        return j;
    }
    j["role"] = RoleName(s.role);
    j["state"] = s.pending_accept ? "offer-sent" : StateName(s.state);
    j["din_amount_una"] = Json::UInt64(s.din_amount_una);
    j["btc_amount_sat"] = Json::UInt64(s.btc_amount_sat);
    j["t_btc_unix"] = s.t_btc_unix;
    j["t_din_unix"] = s.t_din_unix;
    const int64_t now = std::time(nullptr);
    j["btc_lock_seconds_left"] = Json::Int64(int64_t(s.t_btc_unix) - now);
    j["din_lock_seconds_left"] = Json::Int64(int64_t(s.t_din_unix) - now);
    j["tower_armed"] = s.tower_armed;
    j["events"] = Json::Value(Json::arrayValue);
    for (const auto& e : s.last_events) j["events"].append(e);
    return j;
}

// Shared preconditions; returns an error object or nullopt.
std::optional<din::Json> Refuse(const ExecutionContext& ctx, bool starting) {
    if (!SwapService::Active() || !SwapService::Active()->manager()) {
        return Error("swaps are disabled: start dinerod with swap.enable=1 and swap.btc_rpc=HOST:PORT");
    }
    if (starting) {
        if (!ctx.daemon || !ctx.daemon->wallet || !ctx.daemon->wallet->hasActiveWallet()) return Error("no active wallet");
        auto& wm = ctx.daemon->wallet->get();
        if (!wm.isWalletEncrypted()) return Error("swaps need an encrypted wallet (wallet.encrypt)");
        if (wm.isWalletLocked()) return Error("wallet is locked: wallet.unlock first");
    }
    return std::nullopt;
}

// A fresh Taproot address of this wallet (DIN payouts).
std::string NewWalletAddress(const ExecutionContext& ctx) {
    ::RpcHandler* h = g_rpcRegistry.lookup("wallet.getnewaddress");
    if (!h) throw std::runtime_error("wallet.getnewaddress unavailable");
    din::Json p(Json::arrayValue);
    p.append("taproot");
    const din::Json r = (*h)(ctx, p);
    if (r.isString()) return r.asString();
    if (r.isObject() && r["address"].isString()) return r["address"].asString();
    throw std::runtime_error("could not get a new wallet address");
}

const din::Json& Arg(const din::Json& params, const char* key) {
    static const din::Json null_value;
    if (params.isObject()) return params.isMember(key) ? params[key] : null_value;
    if (params.isArray() && params.size() == 1 && params[0].isObject()) {
        return params[0].isMember(key) ? params[0][key] : null_value;
    }
    return null_value;
}

din::Json RpcOffer(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, true)) return *e;
    try {
        OfferRequest r;
        if (!Arg(params, "din_amount_una").isUInt64() || !Arg(params, "btc_amount_sat").isUInt64() ||
            !Arg(params, "btc_address").isString()) {
            return Error("usage: swap.offer {din_amount_una, btc_amount_sat, btc_address, ...}");
        }
        r.din_amount_una = Arg(params, "din_amount_una").asUInt64();
        r.btc_amount_sat = Arg(params, "btc_amount_sat").asUInt64();
        r.btc_claim_address = Arg(params, "btc_address").asString();
        r.din_refund_address = NewWalletAddress(ctx);
        if (Arg(params, "btc_lock_hours").isUInt()) r.btc_lock_hours = Arg(params, "btc_lock_hours").asUInt();
        if (Arg(params, "din_lock_hours").isUInt()) r.din_lock_hours = Arg(params, "din_lock_hours").asUInt();
        if (Arg(params, "expires_minutes").isUInt()) r.expires_minutes = Arg(params, "expires_minutes").asUInt();
        if (Arg(params, "n_din").isUInt()) r.n_din_confirmations = Arg(params, "n_din").asUInt();
        if (Arg(params, "n_btc").isUInt()) r.n_btc_confirmations = Arg(params, "n_btc").asUInt();
        auto* m = SwapService::Active()->manager();
        const std::string text = m->MakeOffer(r, static_cast<uint32_t>(std::time(nullptr)));
        din::Json out(Json::objectValue);
        out["id"] = SwapId(DecodeOffer(text));
        out["offer"] = text;
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

din::Json RpcAccept(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, true)) return *e;
    try {
        if (!Arg(params, "text").isString()) return Error("usage: swap.accept {text, [btc_refund_address]}");
        const std::string text = Arg(params, "text").asString();
        std::string din_payout, btc_refund;
        if (text.rfind(kOfferPrefix, 0) == 0) {  // Bob
            if (!Arg(params, "btc_refund_address").isString()) {
                return Error("accepting an offer needs btc_refund_address (where your BTC returns on refund)");
            }
            btc_refund = Arg(params, "btc_refund_address").asString();
            din_payout = NewWalletAddress(ctx);
        }
        const auto r = SwapService::Active()->manager()->Accept(text, din_payout, btc_refund,
                                                                static_cast<uint32_t>(std::time(nullptr)));
        din::Json out(Json::objectValue);
        out["id"] = r.id;
        if (r.accept_text) out["accept"] = *r.accept_text;
        out["started"] = !r.accept_text.has_value();
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

din::Json RpcList(const ExecutionContext& ctx, const din::Json&) {
    if (auto e = Refuse(ctx, false)) return *e;
    din::Json out(Json::arrayValue);
    for (const auto& s : SwapService::Active()->manager()->List()) out.append(ToJson(s));
    return out;
}

din::Json RpcStatus(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, false)) return *e;
    const din::Json& id = params.isArray() && params.size() == 1 && params[0].isString() ? params[0] : Arg(params, "id");
    if (!id.isString()) return Error("usage: swap.status {id}");
    try {
        return ToJson(SwapService::Active()->manager()->Status(id.asString()));
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

din::Json RpcCancel(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, false)) return *e;
    const din::Json& id = params.isArray() && params.size() == 1 && params[0].isString() ? params[0] : Arg(params, "id");
    if (!id.isString()) return Error("usage: swap.cancel {id}");
    try {
        SwapService::Active()->manager()->Cancel(id.asString());
        din::Json out(Json::objectValue);
        out["cancelled"] = id.asString();
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

}  // namespace

void registerSwapMethods() {
    g_rpcRegistry.registerHandler("swap.offer", RpcOffer, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.accept", RpcAccept, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.list", RpcList, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.status", RpcStatus, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.cancel", RpcCancel, RegisterMode::Overwrite, "context-aware");
}
