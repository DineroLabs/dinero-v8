#include "daemon/services/swap_service.h"

#include "consensus/chainparams.h"
#include "daemon/daemon_context.h"
#include "daemon/services/config_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/wallet_service.h"
#include "rpc/rpc_registry.h"
#include "rpc/wallet_request_dispatch.h"
#include "daemon/interfaces/tx_ingress.h"
#include "util/hex.h"
#include "rpc_client.h"
#include "wallet/bip32_deriver.h"
#include "wallet/key_origin.h"
#include "wallet/swap/btc_watcher.h"
#include "wallet/swap/tower.h"
#include "wallet/wallet_manager.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>

namespace dinero {
namespace {

std::atomic<SwapService*> g_active{nullptr};
std::atomic<SwapService*> g_instance{nullptr};  // the node's service, running or not
std::mutex g_start_mu;                          // one start at a time

SwapService::ZecInstaller& ZecInstallerSlot() {
    static SwapService::ZecInstaller installer;
    return installer;
}

SwapService::LightBtcInstaller& LightBtcInstallerSlot() {
    static SwapService::LightBtcInstaller installer;
    return installer;
}

SwapService::TowerUrlInstaller& TowerUrlInstallerSlot() {
    static SwapService::TowerUrlInstaller installer;
    return installer;
}

bool IsWrite(const std::string& m) {
    return m == "sendrawtransaction" || m == "wallet.sendtoaddress" || m == "wallet.getnewaddress";
}

// The node's own RPC handlers, called in-process with the same context the
// HTTP server builds (http_rpc_server.cpp), so the code paths are the same.
swap::DinRpc InProcessRpc(DaemonContext* ctx) {
    return [ctx](const std::string& method, const Json::Value& params) -> std::optional<Json::Value> {
        try {
            ::RpcHandler* handler = g_rpcRegistry.lookup(method);
            if (!handler) return std::nullopt;
            ExecutionContext ec;
            ec.client_id = "swap";
            ec.daemon = ctx;
            ec.logger = (method.rfind("wallet.", 0) == 0 && ctx->wallet_logger) ? ctx->wallet_logger
                                                                                : ctx->logger_interface;
            ec.mempool = nullptr;
            ec.utxo_view = nullptr;
            // Use the same service lifetime owners as the HTTP dispatcher.
            // These retain the pool and manager through this handler call;
            // they do not hold their mutexes or select/authorize a wallet.
            std::unique_ptr<MempoolService::PoolUse> pool_use;
            if (auto mp = std::dynamic_pointer_cast<MempoolService>(ctx->mempool)) {
                pool_use = MempoolService::AcquirePoolUse(std::move(mp));
                ec.mempool_v2 = &pool_use->Pool();
            }
            std::unique_ptr<WalletService::WalletUse> wallet_use;
            ec.wallet_manager = nullptr;
            if (ctx->wallet) {
                wallet_use = WalletService::AcquireWalletUse(ctx->wallet);
                ec.wallet_manager = &wallet_use->Wallet();
            }
            Json::Value result = (*handler)(ec, params);
            // Handlers report failures in-band; keep them for writes (the runner
            // shows the node's reason), treat them as "no answer" for reads.
            if (result.isObject() && result.isMember("error") && !result["error"].isNull() && !IsWrite(method)) {
                return std::nullopt;
            }
            return result;
        } catch (const std::exception&) {
            return std::nullopt;  // the HTTP server would turn this into an error reply
        }
    };
}

swap::DinFundingOwner RetainedFundingOwner(DaemonContext* ctx) {
    return [ctx](const swap::DinFundingRequest& request,swap::DinFundingOperation operation,
                 const std::vector<uint8_t>& expected_body) -> swap::PreparedDinFunding {
        swap::RequireDinFundingBinding(request.binding);
        if(request.offer_id==swap::Bytes32{} || request.address.empty() || !request.amount_una ||
           request.amount_una>MAX_SUPPLY_UNA_CONST || !ctx || !ctx->wallet)
            throw std::runtime_error("Swap funding request or wallet unavailable");
        const auto service=ctx->wallet;
        auto wallet_use=WalletService::AcquireWalletUse(service);
        WalletSigningIdentity identity;
        {
            auto lease=wallet_use->Wallet().AcquireDatabaseLease();
            if (lease->LegacyTransparentState() != WalletManager::LegacyTransparentState::Absent)
                throw std::runtime_error(WalletManager::kSwapLegacyWalletUnsupported);
            lease->RequireOrchardWalletReady();
            if(lease->ReadDeliveryIdentity()!=request.binding.wallet_id)
                throw std::runtime_error("Swap funding selected a different payer database");
            identity={lease->WalletName(),lease->Session(),request.binding.wallet_id};
        } // no wallet/SQLite lock during chain preflight or submission
        PendingPaymentIntent intent{request.address,request.amount_una,""};
        PendingPaymentRequest binding;binding.domain=PendingPaymentRequestDomain::SwapFunding;
        binding.owner=request.offer_id;
        constexpr std::array<uint8_t,16> funding_operation={'D','I','N','-','f','u','n','d','i','n','g','-','v','1',0,1};
        binding.id=funding_operation;binding.fee_rate_hint=request.binding.fee_rate_hint;
        binding.maximum_fee_una=request.binding.maximum_fee_una;
        binding.audit_context="swap-funding-v1:"+util::hex(std::vector<uint8_t>(request.binding.wallet_id.begin(),request.binding.wallet_id.end()));
        intent.request=binding;
        if(operation==swap::DinFundingOperation::Prepare) {
            if(!expected_body.empty())throw std::runtime_error("Unexpected body in swap funding preparation");
            Json::Value p(Json::objectValue),recipient(Json::objectValue),named(Json::objectValue);
            recipient["address"]=request.address;recipient["amount_una"]=Json::UInt64(request.amount_una);
            p["recipients"].append(recipient);named["domain"]="swap_funding";
            named["owner"]=util::hex(std::vector<uint8_t>(binding.owner.begin(),binding.owner.end()));
            named["id"]=util::hex(std::vector<uint8_t>(binding.id.begin(),binding.id.end()));
            named["fee_rate_hint"]=Json::UInt64(binding.fee_rate_hint);named["maximum_fee_una"]=Json::UInt64(binding.maximum_fee_una);
            named["audit_context"]=binding.audit_context;p["request"]=named;
            ExecutionContext ec;ec.daemon=ctx;ec.client_id="swap";ec.walletName=identity.name;
            ec.wallet_manager=&wallet_use->Wallet();ec.logger=ctx->wallet_logger ? ctx->wallet_logger : ctx->logger_interface;
            std::unique_ptr<MempoolService::PoolUse> pool_use;
            if(auto mp=std::dynamic_pointer_cast<MempoolService>(ctx->mempool)) {
                pool_use=MempoolService::AcquirePoolUse(std::move(mp));ec.mempool_v2=&pool_use->Pool();
            }
            const auto result=DispatchBoundWalletRequest(ec,p,service,identity,WalletRequestDispatchMode::RetainOnly);
            if(result.isMember("error") && !result["error"].isNull())
                throw std::runtime_error("Swap funding preparation: "+result["error"].asString());
        }
        const auto retained=FindRetainedWalletPayment(wallet_use->Wallet(),identity,intent);
        if(!retained)throw std::runtime_error("No exact retained swap funding request");
        Transaction tx;size_t consumed=0;
        if(!TransactionSerializer::Deserialize(tx,retained->signed_body,consumed) || consumed!=retained->signed_body.size() ||
           tx.Serialize(TxSerializationMode::WithWitness)!=retained->signed_body ||
           tx.GetTxid().AsUint256().GetHex()!=retained->txid)
            throw std::runtime_error("Retained swap funding body invalid");
        if(operation==swap::DinFundingOperation::SubmitRetained) {
            if(expected_body.empty() || expected_body!=retained->signed_body)
                throw std::runtime_error("Swap submission does not match retained funding");
            if(!ctx->tx_ingress)throw std::runtime_error("Swap funding transaction ingress unavailable");
            const auto result=ctx->tx_ingress->Submit(tx,TxOrigin::WALLET);
            if(result.rejected())throw std::runtime_error("Retained swap funding rejected: "+result.message);
        } else if(operation!=swap::DinFundingOperation::Prepare && operation!=swap::DinFundingOperation::Resolve)
            throw std::runtime_error("Unknown swap funding operation");
        else if(!expected_body.empty())throw std::runtime_error("Unexpected body in swap funding lookup");
        return {retained->signed_body,retained->txid};
    };
}

swap::SwapNetwork NetworkOf(const ChainParams& p) {
    if (p.name == "mainnet") return swap::SwapNetwork::Mainnet;
    if (p.name == "testnet") return swap::SwapNetwork::Testnet;
    return swap::SwapNetwork::Regtest;
}

}  // namespace

swap::DinFundingOwner MakeRetainedSwapFundingOwner(DaemonContext& ctx) {
    return RetainedFundingOwner(&ctx);
}

SwapService* SwapService::Active() { return g_active.load(); }
bool SwapService::SwapsOn() {
    const SwapService* active = g_active.load();
    return active && active->manager_ && !active->bitcoin_wallet_only_;
}

void SwapService::SetZecInstaller(ZecInstaller installer) { ZecInstallerSlot() = std::move(installer); }
void SwapService::SetLightBtcInstaller(LightBtcInstaller installer) { LightBtcInstallerSlot() = std::move(installer); }
void SwapService::SetTowerUrlInstaller(TowerUrlInstaller installer) { TowerUrlInstallerSlot() = std::move(installer); }

namespace {
std::mutex g_off_reason_mu;
std::string g_off_reason;   // the last refusal printed
bool g_off_by_refusal = false;  // Init kept the node running without swaps
std::atomic<bool> g_shutting_down{false};
std::atomic<void (*)()> g_interrupt_hook{nullptr};
}  // namespace

swap::SwapNetwork SwapService::BitcoinNetworkFor(const ConfigService& config, swap::SwapNetwork dinero_network) {
    if (dinero_network == swap::SwapNetwork::Regtest && config.GetString("swap.btc_rehearsal_chain", "") == "testnet4") {
        return swap::SwapNetwork::Testnet;
    }
    return dinero_network;
}

bool SwapService::Refuse(const std::string& why) {
    std::cerr << "[Swap] " << why << std::endl;
    std::lock_guard<std::mutex> lock(g_off_reason_mu);
    g_off_reason = why;
    return false;
}

void SwapService::SetInterruptHook(void (*hook)()) { g_interrupt_hook.store(hook); }
bool SwapService::ShuttingDown() { return g_shutting_down.load(); }
void SwapService::InterruptForShutdown() {
    g_shutting_down.store(true);
    if (auto* hook = g_interrupt_hook.load()) hook();
}

std::string SwapService::OffReason() {
    std::lock_guard<std::mutex> lock(g_off_reason_mu);
    return g_off_by_refusal ? g_off_reason : std::string();
}

bool SwapService::StartBitcoinWalletNow(std::string* why) {
    std::lock_guard<std::mutex> one(g_start_mu);
    SwapService* self = g_instance.load();
    const auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (!self || !self->ctx_) return fail("this node is still starting");
    if (self->manager_) return true;  // running already, alone or with swaps
    {
        std::lock_guard<std::mutex> lock(g_off_reason_mu);
        g_off_reason.clear();
    }
    self->bitcoin_wallet_requested_ = true;
    bool ok = false;
    try {
        ok = self->Setup(*self->ctx_);
    } catch (const std::exception& e) {
        Refuse(e.what());
    }
    if (!ok || !self->manager_) {
        self->manager_.reset();
        self->before_tick_ = nullptr;
        self->bitcoin_wallet_only_ = false;
        self->bitcoin_wallet_requested_ = false;
        std::lock_guard<std::mutex> lock(g_off_reason_mu);
        g_off_by_refusal = true;  // btc.status reports the same reason afterwards
        return fail(g_off_reason.empty() ? "this node cannot run the Bitcoin wallet" : g_off_reason);
    }
    return self->Start();
}

bool SwapService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;
    g_instance = this;
    {
        std::lock_guard<std::mutex> lock(g_off_reason_mu);
        g_off_reason.clear();
        g_off_by_refusal = false;
    }
    bool ok = false;
    try {
        ok = Setup(ctx);
    } catch (const std::exception& e) {
        Refuse(e.what());
    }
    if (ok) return true;
    manager_.reset();
    before_tick_ = nullptr;
    bitcoin_wallet_only_ = false;
    auto config = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    if (!config || !config->GetBool("swap.optional", false)) return false;
    // dinero-qt's switch: the wallet's node must start whatever the swap settings are.
    std::cerr << "[Swap] swap.optional=1: the node starts with swaps off" << std::endl;
    std::lock_guard<std::mutex> lock(g_off_reason_mu);
    if (g_off_reason.empty()) g_off_reason = "this node refused the swap settings (its log says why)";
    g_off_by_refusal = true;
    return true;
}

bool SwapService::Setup(DaemonContext& ctx) {
    ctx_ = &ctx;
    auto config = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    const bool swaps_wanted = config && config->GetBool("swap.enable", false);
    // The built-in Bitcoin wallet alone (swap.btc_wallet=1): the light Bitcoin
    // client and this wallet's Bitcoin account, with swaps off. Someone who
    // only wants to hold bitcoin does not have to turn swaps on for it.
    const bool wallet_wanted = bitcoin_wallet_requested_ || (config && config->GetBool("swap.btc_wallet", false));
    if (!swaps_wanted && !wallet_wanted) return true;  // neither: nothing at all
    bitcoin_wallet_only_ = !swaps_wanted;

    const std::string btc = bitcoin_wallet_only_ ? std::string() : config->GetString("swap.btc_rpc", "");
    const bool zec_wanted = !bitcoin_wallet_only_ && !config->GetString("swap.zec_rpc", "").empty();
    if (zec_wanted && !ZecInstallerSlot()) {
        return Refuse("swap.zec_rpc needs a dinerod built with DINERO_SWAP_ZEC");
    }
    const std::string btc_backend = bitcoin_wallet_only_ ? std::string("light") : config->GetString("swap.btc_backend", "");
    const bool light_btc = btc_backend == "light";
    if (!btc_backend.empty() && btc_backend != "light" && btc_backend != "core") {
        return Refuse("swap.btc_backend must be core or light");
    }
    if (light_btc && !btc.empty()) {
        return Refuse("swap.btc_backend=light and swap.btc_rpc are two ways to reach Bitcoin: set one");
    }
    if (light_btc && !LightBtcInstallerSlot()) {
        return Refuse("swap.btc_backend=light needs a dinerod built with DINERO_SWAP_LIGHT_BTC");
    }
    const auto colon = btc.rfind(':');
    // Without swap.btc_rpc (a Zcash-only node) Bitcoin calls report "unreachable".
    swap::BtcRpc btc_rpc = [](const std::string&, const Json::Value&) { return std::optional<Json::Value>{}; };
    if (colon == std::string::npos && !zec_wanted && !light_btc) {
        return Refuse("swap.enable=1 needs swap.btc_rpc=HOST:PORT (Bitcoin Core RPC)");
    }
    if (colon != std::string::npos) {
        auto client = std::make_shared<rpc::RpcClient>(btc.substr(0, colon), uint16_t(std::stoi(btc.substr(colon + 1))),
                                                       config->GetString("swap.btc_rpc_user", ""),
                                                       config->GetString("swap.btc_rpc_pass", ""));
        btc_rpc = [client](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
            const auto r = client->call(m, p);
            if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) return std::nullopt;
            return (*r)["result"];
        };
    }

    const ChainParams& params = dinero::Params();
    swap::SwapManagerConfig mc;
    mc.dir = config->DataDir() + "/swaps";
    mc.network = NetworkOf(params);
    mc.runner.din_hrp = params.hrp;
    if (const std::string rehearsal = config->GetString("swap.btc_rehearsal_chain", ""); !rehearsal.empty()) {
        if (mc.network != swap::SwapNetwork::Regtest || rehearsal != "testnet4") {
            return Refuse("swap.btc_rehearsal_chain is for a regtest node only, and only testnet4");
        }
    }
    const swap::SwapNetwork btc_network = BitcoinNetworkFor(*config, mc.network);
    mc.runner.btc_hrp = btc_network == swap::SwapNetwork::Mainnet ? "bc"
                      : btc_network == swap::SwapNetwork::Testnet ? "tb" : "bcrt";
    mc.runner.btc_chain = swap::BitcoinChainFor(btc_network);
    const int64_t din_fee = config->GetInt("swap.din_fee_una", int(mc.runner.din_fee_una));
    const int64_t din_fee_urgent = config->GetInt("swap.din_fee_urgent_una", int(mc.runner.din_fee_urgent_una));
    const int64_t btc_fee = config->GetInt("swap.btc_fee_sat", int(mc.runner.btc_fee_sat));
    if (const auto problem = swap::FeeConfigProblem(din_fee, din_fee_urgent, btc_fee)) {
        return Refuse(*problem);
    }
    mc.runner.din_fee_una = uint64_t(din_fee);
    mc.runner.din_fee_urgent_una = uint64_t(din_fee_urgent);
    mc.runner.btc_fee_sat = uint64_t(btc_fee);
    tick_seconds_ = uint32_t(std::max(1, config->GetInt("swap.tick_seconds", 30)));
    // Mainnet beta: explicit opt-in and per-swap caps (swap_manager.h BetaPolicy).
    auto limit = [&](const char* key) -> std::optional<uint64_t> {
        const std::string v = config->GetString(key, "0");
        if (v.empty() || v.size() > 19 || v.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        return std::stoull(v);
    };
    const auto max_btc = limit("swap.max_btc_sat"), max_din = limit("swap.max_din_una");
    if (!max_btc || !max_din) {
        return Refuse("swap.max_btc_sat / swap.max_din_una must be whole numbers");
    }
    const auto beta = swap::BetaPolicy(mc.network, config->GetBool("swap.mainnet_beta", false), *max_btc, *max_din);
    if (beta.refusal && !bitcoin_wallet_only_) {  // the beta opt-in is about swaps
        return Refuse(*beta.refusal);
    }
    mc.max_btc_sat = beta.max_btc_sat;
    mc.require_tower_for_bob = mc.network == swap::SwapNetwork::Mainnet;
    mc.max_din_una = beta.max_din_una;
    mc.btc_configured = colon != std::string::npos || light_btc;

    // Swap keys from the wallet seed at m/SWAP'/... (see swap_manager.h).
    swap::KeyDeriver derive = [&ctx](const std::vector<std::vector<uint32_t>>& paths, bool require_funding_identity)
        -> std::optional<swap::DerivedKeyBatch> {
        if (!ctx.wallet) return std::nullopt;
        if (paths.empty() || paths.size() > 4) throw std::invalid_argument("invalid swap key batch size");
        auto wallet_use = WalletService::AcquireWalletUse(ctx.wallet);
        auto lease = wallet_use->Wallet().AcquireDatabaseLease();
        if (lease->LegacyTransparentState() != WalletManager::LegacyTransparentState::Absent)
                throw std::runtime_error(WalletManager::kSwapLegacyWalletUnsupported);
            lease->RequireOrchardWalletReady();
        auto seed = lease->CopyRecoverySeed(lease->Session());
        // One actual seed owner spans ALL paths. Derivers and temporary keys
        // are wiped before this lease ends. No chain/RPC callback under it.
        swap::DerivedKeyBatch result;
        if (require_funding_identity) result.wallet_id = lease->ReadDeliveryIdentity();
        result.keys.resize(paths.size());
        for (size_t i = 0; i < paths.size(); ++i) {
            BIP32Deriver deriver(seed->Bytes().data(), seed->Bytes().size());
            for (uint32_t c : paths[i]) deriver.deriveHardened(c & ~BIP32Deriver::HARDENED);
            result.keys[i] = deriver.getPrivateKey();
        }
        return result;
    };
    // The built-in Bitcoin wallet's account, m/84'/coin'/0' of the same seed
    // (swap/btc_wallet_keys.h). Only its public half leaves this lease.
    swap::BtcAccountSource btc_account = [&ctx](uint32_t coin_type) -> std::optional<swap::BtcWalletAccount> {
        if (!ctx.wallet) return std::nullopt;
        auto wallet_use = WalletService::AcquireWalletUse(ctx.wallet);
        if (wallet_use->Wallet().isWalletLocked()) return std::nullopt;
        auto lease = wallet_use->Wallet().AcquireDatabaseLease();
        auto seed = lease->CopyRecoverySeed(lease->Session());
        BIP32Deriver deriver(seed->Bytes().data(), seed->Bytes().size());
        deriver.deriveHardened(swap::kBtcWalletPurpose);
        deriver.deriveHardened(coin_type);
        deriver.deriveHardened(0);
        return swap::BtcWalletAccountFromSecret(deriver.getPrivateKey(), deriver.getChainCode());
    };
    // While the wallet is new, one payment sends at most this much on mainnet
    // (the swap beta's ceiling); test networks have no limit.
    mc.btc_wallet_max_send_sat = mc.network == swap::SwapNetwork::Mainnet ? 1'000'000 : 0;
    manager_ = std::make_unique<swap::SwapManager>(mc, derive, InProcessRpc(&ctx), btc_rpc, MakeRetainedSwapFundingOwner(ctx));
    manager_->SetBtcAccountSource(btc_account);
    // Which wallet is open: its durable public identity, readable while it
    // is locked. Each wallet gets its own Bitcoin wallet files.
    manager_->SetBtcWalletTagSource([&ctx]() -> std::string {
        if (!ctx.wallet) return {};
        auto wallet_use = WalletService::AcquireWalletUse(ctx.wallet);
        auto lease = wallet_use->Wallet().AcquireDatabaseLease();
        const auto id = lease->ReadDeliveryIdentity();
        static const char* const digits = "0123456789abcdef";
        std::string tag;
        for (size_t i = 0; i < 12; ++i) { tag += digits[id[i] >> 4]; tag += digits[id[i] & 15]; }
        return tag;
    });
    // The same account's private half, for signing a payment. Used inside
    // SwapManager::SendBtc only, and wiped there.
    manager_->SetBtcAccountSecretSource([&ctx](uint32_t coin_type) -> std::optional<swap::BtcAccountSecret> {
        if (!ctx.wallet) return std::nullopt;
        auto wallet_use = WalletService::AcquireWalletUse(ctx.wallet);
        if (wallet_use->Wallet().isWalletLocked()) return std::nullopt;
        auto lease = wallet_use->Wallet().AcquireDatabaseLease();
        auto seed = lease->CopyRecoverySeed(lease->Session());
        BIP32Deriver deriver(seed->Bytes().data(), seed->Bytes().size());
        deriver.deriveHardened(swap::kBtcWalletPurpose);
        deriver.deriveHardened(coin_type);
        deriver.deriveHardened(0);
        return swap::BtcAccountSecret{deriver.getPrivateKey(), deriver.getChainCode()};
    });
    if (zec_wanted && !ZecInstallerSlot()(*manager_, *config, mc.network, InProcessRpc(&ctx))) {
        manager_.reset();
        return false;
    }
    if (light_btc) {
        before_tick_ = LightBtcInstallerSlot()(*manager_, *config, mc.network, InProcessRpc(&ctx));
        if (!before_tick_) {
            manager_.reset();
            return false;
        }
    }
    const std::string inbox = bitcoin_wallet_only_ ? std::string() : config->GetString("swap.tower_inbox", "");
    const std::string tower_url = bitcoin_wallet_only_ ? std::string() : config->GetString("swap.tower_url", "");
    if (!tower_url.empty()) {
        if (!inbox.empty()) return Refuse("swap.tower_url and swap.tower_inbox are two ways to reach a watchtower: set one");
        if (!TowerUrlInstallerSlot()) return Refuse("swap.tower_url needs a dinerod built with DINERO_SWAP_LIGHT_BTC");
        if (!TowerUrlInstallerSlot()(*manager_, *config)) return false;
    }
    if (!inbox.empty()) {
        manager_->SetTowerSink([inbox](const std::string& package) { swap::WriteTowerInbox(inbox, package); });
        manager_->SetTowerAck([inbox](const std::string& id, const std::string& hash) {
            return swap::TowerAckFresh(inbox, id, hash, static_cast<uint32_t>(std::time(nullptr)));
        });
    }
    if (bitcoin_wallet_only_) {
        std::cout << "[Swap] the Bitcoin wallet is available; swaps are off (swap.btc_wallet=1 without swap.enable=1)"
                  << std::endl;
        return true;
    }
    std::cout << "[Swap] enabled: dir=" << mc.dir << " btc_rpc=" << btc
              << (zec_wanted ? " zec_rpc=" + config->GetString("swap.zec_rpc", "") : std::string()) << " tick="
              << tick_seconds_ << "s"
              << (mc.max_btc_sat ? " max_btc_sat=" + std::to_string(mc.max_btc_sat) + " max_din_una=" +
                                       std::to_string(mc.max_din_una)
                                 : std::string())
              << (inbox.empty() ? "" : " tower_inbox=" + inbox)
              << (tower_url.empty() ? "" : " tower_url=" + tower_url) << std::endl;
    return true;
}

bool SwapService::Start() {
    if (!manager_) return true;
    stopping_ = false;
    g_active = this;
    thread_ = std::thread(&SwapService::Loop, this);
    return true;
}

void SwapService::Stop() {
    std::lock_guard<std::mutex> one(g_start_mu);  // not while a start on request is under way
    g_instance = nullptr;
    if (!manager_) return;
    g_active = nullptr;
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SwapService::Loop() {
    while (true) {
        try {
            if (before_tick_) before_tick_(static_cast<uint32_t>(std::time(nullptr)));  // outside the manager's lock
            manager_->TickAll(static_cast<uint32_t>(std::time(nullptr)));
        } catch (const std::exception& e) {
            std::cerr << "[Swap] tick failed: " << e.what() << std::endl;
        }
        std::unique_lock<std::mutex> lock(wake_mu_);
        if (wake_.wait_for(lock, std::chrono::seconds(tick_seconds_), [this] { return stopping_; })) return;
    }
}

}  // namespace dinero
