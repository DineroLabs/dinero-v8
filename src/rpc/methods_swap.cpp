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
//   swap.decode {text}  read-only: the terms of an offer (and whether it can
//               be accepted now), or the swap id an accept belongs to
//   swap.list / swap.status {id} / swap.cancel {id}
//   swap.refund {id}  recovery: broadcast your own refund now
//
// No RPC returns a secret, a private key or a raw session. The wallet must be
// encrypted and unlocked to start a swap; while it is locked swaps pause.
#include "daemon/services/swap_service.h"
#include "rpc/swap_rpc.h"
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
    j["chain"] = s.chain == CounterChain::Zcash ? "zec" : "btc";
    if (s.pending_deposit) {
        // Bob's deposit, before his accept: a Zcash offer, or a BTC offer on a node without Bitcoin Core.
        const bool zec_deposit = s.chain == CounterChain::Zcash;
        const bool covered = s.deposit && s.deposit->confirmed_zat >= s.deposit_due_zat;
        j["state"] = covered ? "deposit-confirmed" : "awaiting-deposit";
        j["deposit_address"] = s.deposit_address;
        j["deposit_coin"] = zec_deposit ? "ZEC" : "BTC";
        j["confirmations_needed"] = zec_deposit ? 10 : 2;
        j["offer"] = s.deposit_offer;
        j["deposit_due_zat"] = Json::UInt64(s.deposit_due_zat);
        if (s.deposit) {
            j["deposit_seen_zat"] = Json::UInt64(s.deposit->seen_zat);
            j["deposit_confirmed_zat"] = Json::UInt64(s.deposit->confirmed_zat);
            j["deposit_confirmations"] = s.deposit->confirmations;
        } else {
            j["deposit_status"] = zec_deposit ? "Zcash node unreachable" : "no Bitcoin server reachable";
        }
    } else {
        j["state"] = s.pending_accept ? "offer-sent" : s.sweep_pending ? "sweeping" : StateName(s.state);
    }
    j["din_amount_una"] = Json::UInt64(s.din_amount_una);
    j["btc_amount_sat"] = Json::UInt64(s.btc_amount_sat);
    j["t_btc_unix"] = s.t_btc_unix;
    j["t_din_unix"] = s.t_din_unix;
    const int64_t now = std::time(nullptr);
    j["btc_lock_seconds_left"] = Json::Int64(int64_t(s.t_btc_unix) - now);
    j["din_lock_seconds_left"] = Json::Int64(int64_t(s.t_din_unix) - now);
    // Absence stays unknown for legacy and observer-only sessions.
    j["din_funding_submission_status"] = !s.din_funding_submission_acknowledged ? "unknown" :
        *s.din_funding_submission_acknowledged ? "acknowledged" : "unconfirmed";
    j["tower_armed"] = s.tower_armed;
    j["events"] = Json::Value(Json::arrayValue);
    for (const auto& e : s.last_events) j["events"].append(e);
    return j;
}

// Shared preconditions; returns an error object or nullopt.
std::optional<din::Json> Refuse(const ExecutionContext& ctx, bool starting) {
    if (!SwapService::Active() || !SwapService::Active()->manager()) {
        // swap.enable=1 was refused and swap.optional=1 kept the node running: say why.
        const std::string why = SwapService::OffReason();
        if (!why.empty()) return Error("swaps are disabled: " + why);
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
    return dinero::SwapWalletPayoutAddress(ctx);
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
        const std::string chain = Arg(params, "chain").isString() ? Arg(params, "chain").asString() : "btc";
        if (!Arg(params, "din_funding_fee_rate_hint").isUInt64() ||
            !Arg(params, "din_funding_maximum_fee_una").isUInt64()) {
            return Error("usage: swap.offer {..., din_funding_fee_rate_hint, din_funding_maximum_fee_una}: "
                         "DIN funding requires an explicit fee rate and maximum fee");
        }
        if (chain == "zec") {
            // Selling DIN for ZEC: amounts in zatoshi, a t-address for the claim.
            if (!Arg(params, "din_amount_una").isUInt64() || !Arg(params, "zec_amount_zat").isUInt64() ||
                !Arg(params, "zec_address").isString()) {
                return Error("usage: swap.offer {chain: \"zec\", din_amount_una, zec_amount_zat, zec_address, din_funding_fee_rate_hint, din_funding_maximum_fee_una, ...}");
            }
            r.chain = CounterChain::Zcash;
            r.btc_amount_sat = Arg(params, "zec_amount_zat").asUInt64();
            r.btc_claim_address = Arg(params, "zec_address").asString();
            r.n_btc_confirmations = kMinZecConfirmations;
            if (Arg(params, "n_zec").isUInt()) r.n_btc_confirmations = Arg(params, "n_zec").asUInt();
            if (Arg(params, "zec_lock_hours").isUInt()) r.btc_lock_hours = Arg(params, "zec_lock_hours").asUInt();
        } else if (chain != "btc") {
            return Error("unknown chain " + chain + " (btc or zec)");
        } else if (!Arg(params, "din_amount_una").isUInt64() || !Arg(params, "btc_amount_sat").isUInt64() ||
                   !Arg(params, "btc_address").isString()) {
            return Error("usage: swap.offer {din_amount_una, btc_amount_sat, btc_address, din_funding_fee_rate_hint, din_funding_maximum_fee_una, ...}");
        } else {
            r.btc_amount_sat = Arg(params, "btc_amount_sat").asUInt64();
            r.btc_claim_address = Arg(params, "btc_address").asString();
        }
        r.din_amount_una = Arg(params, "din_amount_una").asUInt64();
        r.din_funding_fee_rate_hint = Arg(params, "din_funding_fee_rate_hint").asUInt64();
        r.din_funding_maximum_fee_una = Arg(params, "din_funding_maximum_fee_una").asUInt64();
        if (!r.din_funding_fee_rate_hint || !r.din_funding_maximum_fee_una)
            return Error("DIN funding requires an explicit positive fee rate and maximum fee");
        r.din_refund_address = NewWalletAddress(ctx);
        if (Arg(params, "btc_lock_hours").isUInt()) r.btc_lock_hours = Arg(params, "btc_lock_hours").asUInt();
        if (Arg(params, "din_lock_hours").isUInt()) r.din_lock_hours = Arg(params, "din_lock_hours").asUInt();
        if (Arg(params, "expires_minutes").isUInt()) r.expires_minutes = Arg(params, "expires_minutes").asUInt();
        if (Arg(params, "n_din").isUInt()) r.n_din_confirmations = Arg(params, "n_din").asUInt();
        if (r.chain == CounterChain::Bitcoin && Arg(params, "n_btc").isUInt()) {
            r.n_btc_confirmations = Arg(params, "n_btc").asUInt();
        }
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
        if (text.rfind(kOfferPrefix, 0) == 0 && SwapService::Active()->manager()->LightBitcoin()) {
            // Bob, without Bitcoin Core: the refund address was given with swap.deposit.
            din_payout = NewWalletAddress(ctx);
        } else if (text.rfind(kOfferPrefix, 0) == 0) {  // Bob
            if (!Arg(params, "btc_refund_address").isString()) {
                return Error("accepting an offer needs btc_refund_address (where your BTC returns on refund)");
            }
            btc_refund = Arg(params, "btc_refund_address").asString();
            din_payout = NewWalletAddress(ctx);
        } else if (text.rfind(kOffer2Prefix, 0) == 0) {  // Bob, Zcash: the refund address was given with swap.deposit
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

const char* NetworkName(SwapNetwork n) {
    return n == SwapNetwork::Mainnet ? "mainnet" : n == SwapNetwork::Testnet ? "testnet" : "regtest";
}

// Read-only: lets a user review terms before swap.accept. No state, no keys.
din::Json RpcDecode(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, false)) return *e;
    if (!Arg(params, "text").isString()) return Error("usage: swap.decode {text}");
    const std::string text = Arg(params, "text").asString();
    try {
        din::Json out(Json::objectValue);
        if (text.rfind(kOfferPrefix, 0) == 0 || text.rfind(kOffer2Prefix, 0) == 0) {
            const SwapOffer o = DecodeOffer(text);
            const bool zec = o.chain == CounterChain::Zcash;
            out["kind"] = "offer";
            out["id"] = SwapId(o);
            out["network"] = NetworkName(o.network);
            out["chain"] = zec ? "zec" : "btc";
            out["din_amount_una"] = Json::UInt64(o.din_amount_una);
            out[zec ? "zec_amount_zat" : "btc_amount_sat"] = Json::UInt64(o.btc_amount_sat);
            if (zec) out["btc_amount_sat"] = Json::UInt64(o.btc_amount_sat);  // the counter amount, as for BTC
            out["t_btc_unix"] = o.t_btc_unix;
            out["t_din_unix"] = o.t_din_unix;
            out["expires_unix"] = o.expires_unix;
            out["n_din"] = o.n_din_confirmations;
            out["n_btc"] = o.n_btc_confirmations;
            try {
                RequireAcceptableNow(o, static_cast<uint32_t>(std::time(nullptr)));
                out["acceptable_now"] = true;
            } catch (const std::exception& e) {
                out["acceptable_now"] = false;
                out["reason"] = e.what();
            }
            return out;
        }
        if (text.rfind(kAcceptPrefix, 0) == 0) {
            const SwapAccept a = DecodeAccept(text);
            out["kind"] = "accept";
            out["offer_id_prefix"] = [&] {
                static const char* d = "0123456789abcdef";
                std::string h;
                for (size_t i = 0; i < 8; ++i) { h += d[a.offer_id[i] >> 4]; h += d[a.offer_id[i] & 15]; }
                return h;
            }();
            return out;
        }
        return Error("expected a dinswap1o offer or a dinswap1a accept");
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

// Bob, paying from a deposit (a Zcash offer, or a BTC offer on a node without
// Bitcoin Core): the per-swap deposit to pay before accepting, and its state.
// Amounts are in the counter chain's smallest unit; the *_zat names are kept
// for Zcash wallets, and a BTC deposit carries the same values as *_sat too.
din::Json RpcDeposit(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, true)) return *e;
    const bool zec_offer = Arg(params, "text").isString() && Arg(params, "text").asString().rfind(kOffer2Prefix, 0) == 0;
    const char* refund_name = zec_offer ? "zec_refund_address" : "btc_refund_address";
    if (!Arg(params, "text").isString() || !Arg(params, refund_name).isString()) {
        return Error("usage: swap.deposit {text: <offer>, zec_refund_address | btc_refund_address}: where the deposit "
                     "returns if the swap does not happen");
    }
    try {
        const auto d = SwapService::Active()->manager()->PrepareDeposit(
            Arg(params, "text").asString(), Arg(params, refund_name).asString(),
            static_cast<uint32_t>(std::time(nullptr)));
        din::Json out(Json::objectValue);
        out["id"] = d.id;
        out["deposit_address"] = d.address;
        out["deposit_coin"] = zec_offer ? "ZEC" : "BTC";
        out["deposit_due_zat"] = Json::UInt64(d.amount_due_zat);
        out["confirmations_needed"] = zec_offer ? 10 : 2;
        if (!zec_offer) out["deposit_due_sat"] = Json::UInt64(d.amount_due_zat);
        if (d.status) {
            out["deposit_seen_zat"] = Json::UInt64(d.status->seen_zat);
            out["deposit_confirmed_zat"] = Json::UInt64(d.status->confirmed_zat);
            out["deposit_confirmations"] = d.status->confirmations;
            if (!zec_offer) {
                out["deposit_seen_sat"] = Json::UInt64(d.status->seen_zat);
                out["deposit_confirmed_sat"] = Json::UInt64(d.status->confirmed_zat);
            }
        }
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

// What this node can swap: lets a wallet show only the chains that work here.
din::Json RpcCapabilities(const ExecutionContext& ctx, const din::Json&) {
    if (auto e = Refuse(ctx, false)) return *e;
    din::Json out(Json::objectValue);
    out["chains"] = Json::Value(Json::arrayValue);
    if (SwapService::Active()->manager()->SupportsBtc()) {
        out["chains"].append("btc");
        // "light": Bitcoin without Bitcoin Core. The node can sell DIN for BTC but not buy DIN with BTC.
        out["btc_backend"] = SwapService::Active()->manager()->LightBitcoin() ? "light" : "core";
    }
    if (SwapService::Active()->manager()->SupportsZec()) out["chains"].append("zec");
    return out;
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

// Recovery: broadcast this wallet's own refund now (the lock time is enforced
// by consensus, so an early call is refused with the node's reason).
din::Json RpcRefund(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, false)) return *e;
    const din::Json& id = params.isArray() && params.size() == 1 && params[0].isString() ? params[0] : Arg(params, "id");
    if (!id.isString()) return Error("usage: swap.refund {id}");
    try {
        din::Json out(Json::objectValue);
        out["txid"] = SwapService::Active()->manager()->Refund(id.asString(), static_cast<uint32_t>(std::time(nullptr)));
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

// Sends what is left on this wallet's Bitcoin deposit addresses to an address
// of the user's: late payments, and everything after a restore from the seed.
din::Json RpcRecoverDeposits(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, true)) return *e;
    if (!Arg(params, "btc_address").isString()) {
        return Error("usage: swap.recoverdeposits {btc_address, [extra_indexes]}: where to send BTC found on this "
                     "wallet's swap deposit addresses; extra_indexes (default 20) looks beyond the swaps this node "
                     "knows, for a wallet restored from its seed");
    }
    try {
        const uint32_t extra = Arg(params, "extra_indexes").isUInt() ? Arg(params, "extra_indexes").asUInt() : 20;
        const auto r = SwapService::Active()->manager()->RecoverBtcDeposits(Arg(params, "btc_address").asString(), extra);
        din::Json out(Json::objectValue);
        out["indexes_checked"] = r.indexes_checked;
        out["sweeps"] = Json::Value(Json::arrayValue);
        uint64_t total = 0;
        for (const auto& s : r.sweeps) {
            din::Json j(Json::objectValue);
            j["index"] = s.index;
            j["amount_sat"] = Json::UInt64(s.amount_sat);
            j["txid"] = s.txid;
            out["sweeps"].append(j);
            total += s.amount_sat;
        }
        out["found_sat"] = Json::UInt64(total);
        out["skipped"] = Json::Value(Json::arrayValue);
        for (const auto& why : r.skipped) out["skipped"].append(why);
        return out;
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
    g_rpcRegistry.registerHandler("swap.decode", RpcDecode, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.list", RpcList, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.status", RpcStatus, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.cancel", RpcCancel, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.refund", RpcRefund, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.recoverdeposits", RpcRecoverDeposits, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.deposit", RpcDeposit, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("swap.capabilities", RpcCapabilities, RegisterMode::Overwrite, "context-aware");
}

std::string dinero::SwapWalletPayoutAddress(const ExecutionContext& ctx) {
    ::RpcHandler* h = g_rpcRegistry.lookup("wallet.getnewaddress");
    if (!h) throw std::runtime_error("wallet.getnewaddress unavailable");
    din::Json p(Json::arrayValue);
    p.append("taproot");
    const din::Json r = (*h)(ctx, p);
    if (r.isString()) return r.asString();
    if (r.isObject() && r["address"].isString()) return r["address"].asString();
    if (r.isObject() && r["error"].isString()) throw std::runtime_error(r["error"].asString());
    throw std::runtime_error("could not get a new wallet address");
}
