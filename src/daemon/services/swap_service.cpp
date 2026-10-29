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
#include "wallet/swap/tower.h"
#include "wallet/wallet_manager.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>

namespace dinero {
namespace {

std::atomic<SwapService*> g_active{nullptr};

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

bool SwapService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;
    auto config = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    if (!config || !config->GetBool("swap.enable", false)) return true;  // disabled: nothing at all

    const std::string btc = config->GetString("swap.btc_rpc", "");
    const auto colon = btc.rfind(':');
    if (colon == std::string::npos) {
        std::cerr << "[Swap] swap.enable=1 needs swap.btc_rpc=HOST:PORT (Bitcoin Core RPC)" << std::endl;
        return false;
    }
    auto client = std::make_shared<rpc::RpcClient>(btc.substr(0, colon), uint16_t(std::stoi(btc.substr(colon + 1))),
                                                   config->GetString("swap.btc_rpc_user", ""),
                                                   config->GetString("swap.btc_rpc_pass", ""));
    swap::BtcRpc btc_rpc = [client](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
        const auto r = client->call(m, p);
        if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) return std::nullopt;
        return (*r)["result"];
    };

    const ChainParams& params = dinero::Params();
    swap::SwapManagerConfig mc;
    mc.dir = config->DataDir() + "/swaps";
    mc.network = NetworkOf(params);
    mc.runner.din_hrp = params.hrp;
    mc.runner.btc_hrp = mc.network == swap::SwapNetwork::Mainnet ? "bc"
                      : mc.network == swap::SwapNetwork::Testnet ? "tb" : "bcrt";
    mc.runner.btc_chain = mc.network == swap::SwapNetwork::Mainnet ? "main"
                        : mc.network == swap::SwapNetwork::Testnet ? "test" : "regtest";
    const int64_t din_fee = config->GetInt("swap.din_fee_una", int(mc.runner.din_fee_una));
    const int64_t din_fee_urgent = config->GetInt("swap.din_fee_urgent_una", int(mc.runner.din_fee_urgent_una));
    const int64_t btc_fee = config->GetInt("swap.btc_fee_sat", int(mc.runner.btc_fee_sat));
    if (const auto problem = swap::FeeConfigProblem(din_fee, din_fee_urgent, btc_fee)) {
        std::cerr << "[Swap] " << *problem << std::endl;
        return false;
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
        std::cerr << "[Swap] swap.max_btc_sat / swap.max_din_una must be whole numbers" << std::endl;
        return false;
    }
    const auto beta = swap::BetaPolicy(mc.network, config->GetBool("swap.mainnet_beta", false), *max_btc, *max_din);
    if (beta.refusal) {
        std::cerr << "[Swap] " << *beta.refusal << std::endl;
        return false;
    }
    mc.max_btc_sat = beta.max_btc_sat;
    mc.require_tower_for_bob = mc.network == swap::SwapNetwork::Mainnet;
    mc.max_din_una = beta.max_din_una;

    // Swap keys from the wallet seed at m/SWAP'/... (see swap_manager.h).
    swap::KeyDeriver derive = [&ctx](const std::vector<std::vector<uint32_t>>& paths, bool require_funding_identity)
        -> std::optional<swap::DerivedKeyBatch> {
        if (!ctx.wallet) return std::nullopt;
        if (paths.empty() || paths.size() > 4) throw std::invalid_argument("invalid swap key batch size");
        auto wallet_use = WalletService::AcquireWalletUse(ctx.wallet);
        auto lease = wallet_use->Wallet().AcquireDatabaseLease();
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
    manager_ = std::make_unique<swap::SwapManager>(mc, derive, InProcessRpc(&ctx), btc_rpc, MakeRetainedSwapFundingOwner(ctx));
    const std::string inbox = config->GetString("swap.tower_inbox", "");
    if (!inbox.empty()) {
        manager_->SetTowerSink([inbox](const std::string& package) { swap::WriteTowerInbox(inbox, package); });
        manager_->SetTowerAck([inbox](const std::string& id, const std::string& hash) {
            return swap::TowerAckFresh(inbox, id, hash, static_cast<uint32_t>(std::time(nullptr)));
        });
    }
    std::cout << "[Swap] enabled: dir=" << mc.dir << " btc_rpc=" << btc << " tick=" << tick_seconds_ << "s"
              << (mc.max_btc_sat ? " max_btc_sat=" + std::to_string(mc.max_btc_sat) + " max_din_una=" +
                                       std::to_string(mc.max_din_una)
                                 : std::string())
              << (inbox.empty() ? "" : " tower_inbox=" + inbox) << std::endl;
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
            manager_->TickAll(static_cast<uint32_t>(std::time(nullptr)));
        } catch (const std::exception& e) {
            std::cerr << "[Swap] tick failed: " << e.what() << std::endl;
        }
        std::unique_lock<std::mutex> lock(wake_mu_);
        if (wake_.wait_for(lock, std::chrono::seconds(tick_seconds_), [this] { return stopping_; })) return;
    }
}

}  // namespace dinero
