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
// `bitcoin_wallet`: a btc.* request, which is also served by a node that runs
// the built-in Bitcoin wallet with swaps off.
std::optional<din::Json> Refuse(const ExecutionContext& ctx, bool starting, bool bitcoin_wallet = false) {
    if (!SwapService::Active() || !SwapService::Active()->manager()) {
        // The settings were refused and swap.optional=1 kept the node running: say why.
        const std::string why = SwapService::OffReason();
        if (bitcoin_wallet) {
            return Error(why.empty() ? "the Bitcoin wallet is not running on this node (swap.btc_wallet=1 starts it)"
                                     : "the Bitcoin wallet is not running on this node: " + why);
        }
        if (!why.empty()) return Error("swaps are disabled: " + why);
        return Error("swaps are disabled: start dinerod with swap.enable=1 and swap.btc_rpc=HOST:PORT");
    }
    if (!bitcoin_wallet && !SwapService::SwapsOn()) {
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

// A Bitcoin address the caller may leave out when the built-in Bitcoin wallet
// is turned on: its current receive address then stands in. The address
// handed out last is used, not a new one each time: a refused or repeated
// request must not use up addresses. Empty when there is neither.
std::string BtcAddressArg(const din::Json& params, const char* key) {
    if (Arg(params, key).isString() && !Arg(params, key).asString().empty()) return Arg(params, key).asString();
    try {
        const auto st = SwapService::Active()->manager()->BtcWalletStatus();
        if (st.enabled) return st.address;
    } catch (const std::exception&) {
    }
    return {};
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
                   BtcAddressArg(params, "btc_address").empty()) {
            return Error("usage: swap.offer {din_amount_una, btc_amount_sat, btc_address (not needed when this wallet's "
                         "Bitcoin wallet is on), din_funding_fee_rate_hint, din_funding_maximum_fee_una, ...}");
        } else {
            r.btc_amount_sat = Arg(params, "btc_amount_sat").asUInt64();
            r.btc_claim_address = BtcAddressArg(params, "btc_address");
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
        std::string btc_refund;
        if (text.rfind(kOfferPrefix, 0) == 0 && !SwapService::Active()->manager()->LightBitcoin()) {  // Bob, with Bitcoin Core
            if (!Arg(params, "btc_refund_address").isString()) {
                return Error("accepting an offer needs btc_refund_address (where your BTC returns on refund)");
            }
            btc_refund = Arg(params, "btc_refund_address").asString();
        }
        // Bob's DIN payout address is taken from the wallet only when the accept
        // goes through (without Bitcoin Core, and for Zcash, the refund address
        // was given with swap.deposit). Alice's start needs none.
        const SwapManager::PayoutAddressSource din_payout = [&ctx] { return NewWalletAddress(ctx); };
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
    const std::string refund_address = !Arg(params, "text").isString() ? std::string()
                                       : zec_offer ? (Arg(params, refund_name).isString() ? Arg(params, refund_name).asString() : std::string())
                                                   : BtcAddressArg(params, refund_name);
    if (refund_address.empty()) {
        return Error("usage: swap.deposit {text: <offer>, zec_refund_address | btc_refund_address}: where the deposit "
                     "returns if the swap does not happen (a BTC refund address is not needed when this wallet's "
                     "Bitcoin wallet is on)");
    }
    try {
        const auto d = SwapService::Active()->manager()->PrepareDeposit(
            Arg(params, "text").asString(), refund_address,
            static_cast<uint32_t>(std::time(nullptr)));
        din::Json out(Json::objectValue);
        out["id"] = d.id;
        out["deposit_address"] = d.address;
        out["deposit_coin"] = zec_offer ? "ZEC" : "BTC";
        out["deposit_due_zat"] = Json::UInt64(d.amount_due_zat);
        out["refund_address"] = refund_address;
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

// The Bitcoin at the addresses this wallet's swaps paid to (watch-only).
din::Json RpcBtcHoldings(const ExecutionContext& ctx, const din::Json&) {
    if (auto e = Refuse(ctx, false)) return *e;
    try {
        const auto h = SwapService::Active()->manager()->BtcHoldingsOfSwaps();
        din::Json out(Json::objectValue);
        out["addresses"] = Json::Value(Json::arrayValue);
        for (const auto& a : h.addresses) {
            din::Json j(Json::objectValue);
            j["address"] = a.address;
            j["known"] = a.known;
            if (a.known) j["sat"] = Json::UInt64(a.sat);
            else j["problem"] = a.problem;
            j["sale_proceeds"] = a.sale_proceeds;
            j["purchase_refund"] = a.purchase_refund;
            j["swaps"] = Json::Value(Json::arrayValue);
            for (const auto& id : a.swap_ids) j["swaps"].append(id);
            out["addresses"].append(j);
        }
        out["known_sat"] = Json::UInt64(h.known_sat);
        out["unreadable_swaps"] = h.unreadable_swaps;
        out["complete"] = h.complete;
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

// ---- The built-in Bitcoin wallet (docs/design/multi-coin-wallet.md) ----------

din::Json BtcWalletStatusJson(const swap::SwapManager::BtcWalletState& st) {
    din::Json out(Json::objectValue);
    out["offered"] = st.offered;
    out["enabled"] = st.enabled;
    out["can_send"] = st.offered || st.enabled;
    if (!st.why_not.empty()) out["why_not"] = st.why_not;
    if (st.enabled) {
        out["address"] = st.address;
        out["addresses_handed_out"] = st.handed_out;
    }
    return out;
}

din::Json RpcBtcStatus(const ExecutionContext& ctx, const din::Json&) {
    if (auto e = Refuse(ctx, false, true)) return *e;
    try {
        return BtcWalletStatusJson(SwapService::Active()->manager()->BtcWalletStatus());
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

// Turns the wallet on. The wallet must be unlocked: the account comes from the seed.
din::Json RpcBtcEnable(const ExecutionContext& ctx, const din::Json&) {
    if (auto e = Refuse(ctx, true, true)) return *e;
    try {
        SwapService::Active()->manager()->EnableBtcWallet();
        return BtcWalletStatusJson(SwapService::Active()->manager()->BtcWalletStatus());
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

din::Json RpcBtcNewAddress(const ExecutionContext& ctx, const din::Json&) {
    if (auto e = Refuse(ctx, false, true)) return *e;
    try {
        din::Json out(Json::objectValue);
        out["address"] = SwapService::Active()->manager()->BtcWalletNewAddress();
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

din::Json RpcBtcBalance(const ExecutionContext& ctx, const din::Json&) {
    if (auto e = Refuse(ctx, false, true)) return *e;
    try {
        const auto b = SwapService::Active()->manager()->BtcWalletBalance();
        din::Json out(Json::objectValue);
        out["sat"] = Json::UInt64(b.sat);
        out["addresses_read"] = b.addresses_read;
        out["addresses"] = Json::Value(Json::arrayValue);
        for (const auto& row : b.rows) {
            din::Json j(Json::objectValue);
            j["address"] = row.address;
            j["sat"] = Json::UInt64(row.sat);
            j["change"] = row.branch == 1;
            j["index"] = row.index;
            out["addresses"].append(j);
        }
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

din::Json BtcPaymentJson(const swap::SwapManager::BtcPayment& p) {
    din::Json out(Json::objectValue);
    out["txid"] = p.txid;
    out["to"] = p.to_address;
    out["amount_sat"] = Json::UInt64(p.amount_sat);
    out["fee_sat"] = Json::UInt64(p.fee_sat);
    out["change_sat"] = Json::UInt64(p.change_sat);
    out["dust_to_fee_sat"] = Json::UInt64(p.dust_to_fee_sat);
    out["total_sat"] = Json::UInt64(p.amount_sat + p.fee_sat);
    out["vsize"] = Json::UInt64(p.vsize);
    out["feerate_sat_per_vb"] = p.feerate_sat_per_vb;
    out["balance_before_sat"] = Json::UInt64(p.balance_before_sat);
    out["inputs"] = Json::UInt64(p.inputs);
    return out;
}

// {address, amount_sat | send_all:true, [target_blocks]}: 1 fast, 6 normal (default), 24 slow.
struct BtcSendArgs {
    std::string address;
    uint64_t amount_sat{};
    bool send_all{false};
    uint32_t target_blocks{6};
};
std::optional<BtcSendArgs> ParseBtcSendArgs(const din::Json& params) {
    BtcSendArgs a;
    if (!Arg(params, "address").isString()) return std::nullopt;
    a.address = Arg(params, "address").asString();
    a.send_all = Arg(params, "send_all").isBool() && Arg(params, "send_all").asBool();
    if (!a.send_all) {
        if (!Arg(params, "amount_sat").isUInt64()) return std::nullopt;
        a.amount_sat = Arg(params, "amount_sat").asUInt64();
    } else if (!Arg(params, "amount_sat").isNull()) {
        return std::nullopt;  // one or the other
    }
    if (Arg(params, "target_blocks").isUInt()) a.target_blocks = Arg(params, "target_blocks").asUInt();
    return a;
}
const char* const kBtcSendUsage = "{address, amount_sat (or send_all: true), [target_blocks: 1 fast, 6 normal, 24 slow]}";

// What a payment would be: signs nothing, sends nothing, needs no unlock.
din::Json RpcBtcPreview(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, false, true)) return *e;
    const auto a = ParseBtcSendArgs(params);
    if (!a) return Error(std::string("usage: btc.preview ") + kBtcSendUsage);
    try {
        return BtcPaymentJson(SwapService::Active()->manager()->PreviewBtcSend(a->address, a->amount_sat, a->send_all, a->target_blocks));
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

// Sends the payment btc.preview showed: `txid` is the preview's, and the
// payment is refused if it would no longer be that one.
din::Json RpcBtcSend(const ExecutionContext& ctx, const din::Json& params) {
    if (auto e = Refuse(ctx, true, true)) return *e;
    const auto a = ParseBtcSendArgs(params);
    if (!a || !Arg(params, "txid").isString()) {
        return Error(std::string("usage: btc.send ") + kBtcSendUsage + " and txid: the txid btc.preview showed for it");
    }
    try {
        din::Json out = BtcPaymentJson(SwapService::Active()->manager()->SendBtc(a->address, a->amount_sat, a->send_all,
                                                                              a->target_blocks, Arg(params, "txid").asString()));
        out["sent"] = true;
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

// A stuck payment again, with a higher fee taken from its change. Without
// new_txid: what it would be. With the new_txid that showed: sent.
din::Json RpcBtcBumpFee(const ExecutionContext& ctx, const din::Json& params) {
    const bool send = Arg(params, "new_txid").isString();
    if (auto e = Refuse(ctx, send, true)) return *e;
    if (!Arg(params, "txid").isString()) {
        return Error("usage: btc.bumpfee {txid, [target_blocks], [new_txid: from a first call, to send it]}");
    }
    const uint32_t target = Arg(params, "target_blocks").isUInt() ? Arg(params, "target_blocks").asUInt() : 1;
    try {
        din::Json out = BtcPaymentJson(SwapService::Active()->manager()->BumpBtcFee(
            Arg(params, "txid").asString(), target, send ? Arg(params, "new_txid").asString() : std::string()));
        out["replaces"] = Arg(params, "txid").asString();
        out["sent"] = send;
        return out;
    } catch (const std::exception& e) {
        return Error(e.what());
    }
}

din::Json RpcBtcListTransactions(const ExecutionContext& ctx, const din::Json&) {
    if (auto e = Refuse(ctx, false, true)) return *e;
    try {
        din::Json out(Json::arrayValue);
        for (const auto& t : SwapService::Active()->manager()->BtcWalletHistory()) {
            din::Json j(Json::objectValue);
            j["txid"] = t.txid;
            j["received"] = t.received;
            j["amount_sat"] = Json::UInt64(t.amount_sat);
            if (!t.received) j["fee_sat"] = Json::UInt64(t.fee_sat);
            if (!t.address.empty()) j["address"] = t.address;
            j["confirmed"] = t.confirmations > 0;
            j["confirmations"] = t.confirmations;
            out.append(j);
        }
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
    g_rpcRegistry.registerHandler("swap.btcholdings", RpcBtcHoldings, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("btc.status", RpcBtcStatus, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("btc.enable", RpcBtcEnable, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("btc.getnewaddress", RpcBtcNewAddress, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("btc.getbalance", RpcBtcBalance, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("btc.preview", RpcBtcPreview, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("btc.send", RpcBtcSend, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("btc.bumpfee", RpcBtcBumpFee, RegisterMode::Overwrite, "context-aware");
    g_rpcRegistry.registerHandler("btc.listtransactions", RpcBtcListTransactions, RegisterMode::Overwrite, "context-aware");
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
