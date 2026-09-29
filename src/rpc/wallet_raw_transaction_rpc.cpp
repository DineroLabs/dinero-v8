#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/relay_transaction_reader.h"
#include "util/hex.h"
#include "consensus/shielded/resource_limits.h"
#include <sstream>
#include <iomanip>
#if DINERO_WALLET_RAW_ORCHARD
#include "orchard_transaction.h"
#endif
namespace {
template <typename OutputLike>
void PopulateWalletOutputDisplay(din::Json& output_obj, const OutputLike& output) {
    output_obj["is_confidential"] = output.is_confidential;
    output_obj["amount_hidden"] = output.is_confidential;
    if (output.is_confidential) {
        output_obj["display_amount"] = "confidential";
        output_obj["commitment"] = ::util::hex(output.commitment);
        output_obj["range_proof_bytes"] = static_cast<Json::UInt64>(output.range_proof.size());
        output_obj["nonce_bytes"] = static_cast<Json::UInt64>(output.nonce.size());
    } else {
        output_obj["value"] = static_cast<double>(output.value.GetUna()) / 1e8;
        output_obj["display_amount"] = static_cast<double>(output.value.GetUna()) / 1e8;
    }
}
din::Json DescribeOrchard(const dinero::MempoolTransaction& body) {
#if DINERO_WALLET_RAW_ORCHARD
    const auto& tx=body.Orchard();din::Json result;
    result["format"]="orchard";result["txid"]=body.GetTxid().AsUint256().GetHex();
    result["wtxid"]=body.GetWtxid().AsUint256().GetHex();result["hex"]=::util::hex(body.Serialize());
    result["locktime"]=Json::UInt64(tx.LockTime());result["size"]=Json::UInt64(body.GetSize());
    result["vsize"]=Json::UInt64(body.GetVirtualSize());result["weight"]=Json::UInt64(body.GetWeight());
    result["fee_una"]=Json::UInt64(tx.ExplicitFee());
    din::Json inputs=din::arr();
    for(const auto& input:tx.Inputs()) {
        din::Json item;dinero::uint256 hash;std::copy(input.txid_wire.begin(),input.txid_wire.end(),hash.begin());
        item["txid"]=hash.GetHex();item["vout"]=Json::UInt64(input.output_index);
        item["sequence"]=Json::UInt64(input.sequence);item["scriptSig"]=::util::hex(input.script_sig);
        if(!input.witness.empty()) {din::Json witness=din::arr();for(const auto& w:input.witness)witness.append(::util::hex(w));item["txinwitness"]=witness;}
        inputs.append(item);
    }
    result["vin"]=inputs;din::Json outputs=din::arr();
    for(size_t i=0;i<tx.Outputs().size();++i) {
        const auto& output=tx.Outputs()[i];din::Json item;item["n"]=Json::UInt64(i);
        item["value_una"]=Json::UInt64(output.amount_una);item["value"]=double(output.amount_una)/1e8;
        item["display_amount"]=double(output.amount_una)/1e8;item["is_confidential"]=false;item["amount_hidden"]=false;
        const auto& script=output.script_pub_key;
        item["scriptPubKey"]["hex"]=::util::hex(script);
        item["scriptPubKey"]["type"]=(script.size()==22 && script[0]==0 && script[1]==20) ? "witness_v0_keyhash" :
            (script.size()==34 && script[0]==0 && script[1]==32) ? "witness_v0_scripthash" :
            (script.size()==34 && script[0]==0x51 && script[1]==32) ? "witness_v1_taproot" : "unknown";
        outputs.append(item);
    }
    result["vout"]=outputs;
    // Decoding describes public envelope fields; it does not verify proofs or
    // infer private recipients, amounts, wallet ownership or confirmations.
    return result;
#else
    (void)body;throw std::runtime_error("Orchard transaction reader unavailable");
#endif
}
}
din::Json rpc_context_wallet_decoderawtransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!params.isArray() || params.size() != 1 || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.decoderawtransaction \"hex\"";
        return result;
    }

    try {
        std::string tx_hex = params[0].as<std::string>();

        if (tx_hex.empty() || tx_hex.size() > 2 * std::max<size_t>(dinero::consensus::shielded::kAuthMaxTxBytes, dinero::consensus::MAX_TX_SIZE) || tx_hex.size() % 2 ||
            tx_hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::invalid_argument("Invalid transaction hex");
        const auto body = dinero::DecodeRelayTransaction(::util::HexToBytes(tx_hex),
            dinero::RelayTransactionReadMode::AvailableFamilies);
        if (body.IsOrchard()) return DescribeOrchard(body);
        const auto& tx = body.Historical();

        result["txid"] = tx.GetTxid().AsUint256().GetHex();
        result["version"] = tx.version;
        result["locktime"] = static_cast<int>(tx.lockTime);
        result["size"] = static_cast<int>(tx_hex.length() / 2);

        // Inputs
        din::Json vin_arr = din::arr();
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            const auto& input = tx.vin[i];
            din::Json inp;
            inp["txid"] = input.prevout.txid.AsUint256().GetHex();
            inp["vout"] = static_cast<int>(input.prevout.vout);
            inp["sequence"] = static_cast<int64_t>(input.sequence);

            // ScriptSig hex
            std::ostringstream script_hex;
            for (uint8_t b : input.scriptSig) {
                script_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            }
            inp["scriptSig"] = script_hex.str();

            // Witness
            if (!input.witness.empty()) {
                din::Json witness_arr = din::arr();
                for (const auto& w : input.witness) {
                    std::ostringstream w_hex;
                    for (uint8_t b : w) {
                        w_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                    }
                    witness_arr.append(w_hex.str());
                }
                inp["txinwitness"] = witness_arr;
            }

            vin_arr.append(inp);
        }
        result["vin"] = vin_arr;

        // Outputs
        din::Json vout_arr = din::arr();
        for (size_t i = 0; i < tx.vout.size(); ++i) {
            const auto& output = tx.vout[i];
            din::Json outp;
            outp["n"] = static_cast<int>(i);
            PopulateWalletOutputDisplay(outp, output);

            // ScriptPubKey
            din::Json spk;
            std::ostringstream spk_hex;
            for (uint8_t b : output.scriptPubKey) {
                spk_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            }
            spk["hex"] = spk_hex.str();

            // Determine type
            if (output.IsSegWitV0()) {
                spk["type"] = output.scriptPubKey.size() == 22 ? "witness_v0_keyhash" : "witness_v0_scripthash";
            } else if (output.IsTaproot()) {
                spk["type"] = "witness_v1_taproot";
            } else {
                spk["type"] = "unknown";
            }

            outp["scriptPubKey"] = spk;
            vout_arr.append(outp);
        }
        result["vout"] = vout_arr;

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to decode transaction: ") + e.what();
    }

    return result;
}

din::Json rpc_context_wallet_getrawtransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    if (!params.isArray() || params.size()<1 || params.size()>2 || !params[0].isString() ||
        (params.size()==2 && !params[1].isBool())) {
        result["error"]="Usage: wallet.getrawtransaction \"txid\" [verbose=false]";return result;
    }
    try {
        const auto text=params[0].asString();dinero::uint256 txid;
        if (text.size()!=64 || text.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos ||
            !dinero::uint256::FromHex(text,txid)) {result["error"]="Invalid transaction ID";return result;}
        std::optional<dinero::MempoolTransaction> body;
        if (ctx.daemon && ctx.daemon->mempool) {
            const auto service=std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if (!service) throw std::runtime_error("Mempool service unavailable");
            auto use=dinero::MempoolService::AcquirePoolUse(service);
            const auto entry=use->Pool().getMempoolEntry(txid);
            if (entry) body=entry->tx;
        }
        if (!body) {
            const auto service=ctx.daemon ? std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate) : nullptr;
            if (!service) throw std::runtime_error("Chainstate service unavailable");
            const auto captured=service->getTransactionBody(txid);
            if (!captured.ok()) {
                result["error"]=captured.status()==dinero::Status::NotFound ? "Transaction not found" : "Transaction data unavailable";return result;
            }
            body=*captured;
        }
        if (body->GetTxid().AsUint256()!=txid) throw std::runtime_error("Transaction identity mismatch");
        const auto bytes=::util::hex(body->Serialize());
        if (params.size()==2 && params[1].asBool()) {
            din::Json decode=din::arr();decode.append(bytes);return rpc_context_wallet_decoderawtransaction(ctx,decode);
        }
        result["hex"]=bytes;
    } catch(const std::exception& e) {result["error"]=std::string("Failed to get transaction: ")+e.what();}
    return result;
}
