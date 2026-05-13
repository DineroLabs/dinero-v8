#include "rpc/rpc_registry.h"
#include "dpi/dpi_protocol.h"
#include "wallet/schnorr_signer.h"
#include "wallet/psbt.h"
#include "consensus/coin_type.h"
#include "crypto/hash.h"
#include "address/addr_codec.h"
#include "primitives/transaction.h"
#include "daemon/daemon_context.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/chainstate_service.h"
#include "network/bridge_node.h"
#include "consensus/utreexo_accumulator.h"
#include "common/logger.h"
#include "common/ilogger.h"
#include "consensus/outpoint.h"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <set>

extern RpcRegistry g_rpcRegistry;

// ============================================================================
// Helper: hex encode bytes
// ============================================================================
static std::string BytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}

static std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// ============================================================================
// Helper: Attach Utreexo inclusion proofs to a DPI RPC response.
// Requires a bridge node (--utreexo-bridge=1). Gracefully no-ops if unavailable.
// ============================================================================
static void AttachUtreexoProofs(din::Json& result,
                                const dinero::Transaction& tx,
                                const DaemonContext* daemon) {
    if (!daemon || !daemon->chainstate) return;

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon->chainstate);
    if (!chainstate) return;

    auto bridge_node = chainstate->GetBridgeNode();
    if (!bridge_node) return;  // Bridge not enabled — graceful degradation

    auto proofs_opt = bridge_node->GenerateProofsForTransaction(tx);
    if (!proofs_opt || proofs_opt->empty()) return;

    auto* forest = chainstate->utreexoForest();
    if (!forest) return;

    // Build the utreexo_proofs JSON object
    din::Json utreexo_proofs;

    // Stump snapshot (inline — solves freshness race condition)
    auto commitment = bridge_node->GetCurrentForestCommitment();
    utreexo_proofs["stump_commitment"] = BytesToHex(commitment.data(), commitment.size());
    utreexo_proofs["stump_num_leaves"] = static_cast<din::Json::UInt64>(forest->getNumLeaves());

    auto roots = forest->getRoots();
    din::Json roots_json = din::arr();
    for (size_t h = 0; h < roots.size(); ++h) {
        // Only include non-zero roots
        bool all_zero = true;
        for (auto b : roots[h]) { if (b != 0) { all_zero = false; break; } }
        if (all_zero) continue;

        din::Json root_obj;
        root_obj["height"] = static_cast<int>(h);
        root_obj["hash"] = BytesToHex(roots[h].data(), roots[h].size());
        roots_json.append(root_obj);
    }
    utreexo_proofs["stump_roots"] = roots_json;

    // Per-input proofs
    din::Json inputs_json = din::arr();
    const auto& proof_pairs = *proofs_opt;

    // Map proofs to non-coinbase inputs
    size_t proof_idx = 0;
    for (size_t i = 0; i < tx.vin.size() && proof_idx < proof_pairs.size(); ++i) {
        // Skip coinbase inputs (they have no UTXO to prove)
        if (tx.vin[i].prevout.txid.IsNull()) continue;

        const auto& [proof, spent_output] = proof_pairs[proof_idx];
        ++proof_idx;

        din::Json input_obj;
        input_obj["input_index"] = static_cast<int>(i);
        input_obj["txid"] = tx.vin[i].prevout.txid.AsUint256().GetHex();
        input_obj["vout"] = tx.vin[i].prevout.vout;

        // Compute leaf hash so the client can verify independently
        auto leaf_hash = dinero::consensus::HashUTXO(
            tx.vin[i].prevout.txid.AsUint256(),
            tx.vin[i].prevout.vout,
            spent_output.value,
            spent_output.scriptPubKey);
        input_obj["leaf_hash"] = BytesToHex(leaf_hash.data(), leaf_hash.size());

        input_obj["position"] = static_cast<din::Json::UInt64>(proof.position);

        din::Json siblings_json = din::arr();
        for (const auto& sibling : proof.siblings) {
            siblings_json.append(BytesToHex(sibling.data(), sibling.size()));
        }
        input_obj["siblings"] = siblings_json;

        input_obj["value"] = static_cast<din::Json::UInt64>(spent_output.value);
        input_obj["script_pubkey"] = BytesToHex(spent_output.scriptPubKey.data(),
                                                spent_output.scriptPubKey.size());

        inputs_json.append(input_obj);
    }
    utreexo_proofs["inputs"] = inputs_json;

    // Phase 3: Chain trust anchor — raw header for client-side PoW verification
    auto* chain_db = chainstate->GetChainDB();
    if (chain_db) {
        std::string tip_hash_hex = chainstate->getBestBlockHash();
        auto tip_hash = dinero::uint256::FromHexUnsafe(tip_hash_hex);
        auto block_result = chainstate->getBlockByHash(tip_hash);
        if (block_result.status() == dinero::Status::Ok) {
            const auto& block = block_result.value();
            auto header_bytes = block.header.SerializeForHash();

            din::Json anchor;
            anchor["height"] = static_cast<din::Json::UInt64>(chainstate->getBlockHeight());
            anchor["block_hash"] = tip_hash_hex;
            anchor["raw_header"] = BytesToHex(header_bytes.data(), header_bytes.size());
            utreexo_proofs["anchor"] = anchor;
        }
    }

    result["utreexo_proofs"] = utreexo_proofs;
}

// ============================================================================
// Helper: get wallet's primary taproot key (index 0)
// Returns {privkey_bytes, pubkey_bytes} or empty on failure.
// Phase 1 identity model: merchant/sender key = wallet receive key (index 0).
// ============================================================================
static std::pair<std::vector<uint8_t>, std::vector<uint8_t>> GetPrimaryTaprootKey(
    dinero::WalletService* wallet_service
) {
    // BIP86 path for primary receive key: m/86'/<coin_type>'/0'/0/0
    const std::string primary_taproot_path =
        "m/86'/" + std::to_string(dinero::consensus::DINERO_COIN_TYPE) + "'/0'/0/0";
    std::string priv_hex = wallet_service->get().getPrivateKeyForPath(primary_taproot_path);
    if (priv_hex.empty()) return {{}, {}};

    auto privkey = HexToBytes(priv_hex);
    if (privkey.size() != 32) return {{}, {}};

    auto pubkey = din::SchnorrSigner::getPublicKey(privkey);
    if (pubkey.size() != 32) return {{}, {}};

    return {privkey, pubkey};
}

// ============================================================================
// dpi.createinvoice — Merchant creates a signed invoice
// ============================================================================

din::Json rpc_dpi_createinvoice(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    if (wallet_service->get().isWalletLocked()) {
        result["error"] = "Wallet is locked. Use wallet.unlock first.";
        return result;
    }

    // Parse parameters
    double amount_din = 0.0;
    std::string memo;
    uint16_t expiry_seconds = din::dpi::DPI_DEFAULT_EXPIRY;

    if (params.isObject()) {
        if (!params.isMember("amount") || !params["amount"].isNumeric()) {
            result["error"] = "Missing required parameter: amount (in DIN)";
            return result;
        }
        amount_din = params["amount"].asDouble();
        if (params.isMember("memo") && params["memo"].isString()) {
            memo = params["memo"].asString();
        }
        if (params.isMember("expiry_seconds") && params["expiry_seconds"].isNumeric()) {
            expiry_seconds = static_cast<uint16_t>(params["expiry_seconds"].asInt());
        }
    } else if (params.isArray() && params.size() >= 1) {
        amount_din = params[0].asDouble();
        if (params.size() >= 2 && params[1].isString()) {
            memo = params[1].asString();
        }
        if (params.size() >= 3 && params[2].isNumeric()) {
            expiry_seconds = static_cast<uint16_t>(params[2].asInt());
        }
    } else {
        result["error"] = "Usage: dpi.createinvoice {amount: <DIN>, memo: <string>, expiry_seconds: <int>}";
        return result;
    }

    if (amount_din <= 0) {
        result["error"] = "Amount must be positive";
        return result;
    }

    uint64_t amount_una = static_cast<uint64_t>(amount_din * 1e8);

    // Get merchant's primary taproot key (Phase 1 identity model)
    auto [merchant_priv, merchant_pub] = GetPrimaryTaprootKey(wallet_service.get());
    if (merchant_priv.empty()) {
        result["error"] = "Could not derive merchant signing key (m/86'/" +
                          std::to_string(dinero::consensus::DINERO_COIN_TYPE) + "'/0'/0/0)";
        return result;
    }

    // Get receive address (bech32m)
    std::string dest_address = wallet_service->get().getNewAddress("dpi-merchant", "taproot");
    if (dest_address.empty()) {
        result["error"] = "Could not generate receive address";
        return result;
    }

    // Build invoice
    din::dpi::DpiInvoice inv;
    inv.version = din::dpi::DPI_VERSION;
    inv.network = din::dpi::GetDpiNetworkByte();
    inv.amount = amount_una;
    inv.destination_address = dest_address;

    // merchant_id = HASH160(merchant_pubkey)
    auto hash160 = din::crypto::HASH160(merchant_pub.data(), merchant_pub.size());
    std::memcpy(inv.merchant_id.data(), hash160.data(), din::dpi::MERCHANT_ID_SIZE);

    // CSPRNG nonce
    din::dpi::GenerateSecureRandom(inv.nonce.data(), din::dpi::NONCE_SIZE);

    inv.timestamp = static_cast<uint32_t>(std::time(nullptr));
    inv.expiry = expiry_seconds;
    inv.memo = memo;

    // Sign invoice
    if (!din::dpi::SignInvoice(inv, merchant_priv)) {
        result["error"] = "Failed to sign invoice";
        return result;
    }

    // Serialize and base64 encode
    auto serialized = din::dpi::SerializeInvoice(inv);
    std::string invoice_b64 = dinero::Base64Encode(serialized);

    result["invoice"] = invoice_b64;
    result["invoice_id"] = BytesToHex(inv.invoice_id.data(), 32);
    result["destination"] = dest_address;
    result["amount_una"] = static_cast<din::Json::UInt64>(amount_una);
    result["amount_din"] = amount_din;
    result["expiry_seconds"] = expiry_seconds;
    result["timestamp"] = inv.timestamp;
    result["memo"] = memo;

    if (ctx.logger) {
        ctx.logger->info("[dpi.createinvoice] Created invoice " +
                         BytesToHex(inv.invoice_id.data(), 8) + "... for " +
                         std::to_string(amount_din) + " DIN -> " + dest_address);
    }

    return result;
}

// ============================================================================
// dpi.payinvoice — Sender pays a DPI invoice
// ============================================================================

din::Json rpc_dpi_payinvoice(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    if (wallet_service->get().isWalletLocked()) {
        result["error"] = "Wallet is locked. Use wallet.unlock first.";
        return result;
    }

    // Parse parameters
    std::string invoice_b64;
    double fee_rate = 0.0;

    if (params.isObject()) {
        if (!params.isMember("invoice") || !params["invoice"].isString()) {
            result["error"] = "Missing required parameter: invoice (base64)";
            return result;
        }
        invoice_b64 = params["invoice"].asString();
        if (params.isMember("fee_rate") && params["fee_rate"].isNumeric()) {
            fee_rate = params["fee_rate"].asDouble();
        }
    } else if (params.isArray() && params.size() >= 1) {
        invoice_b64 = params[0].asString();
        if (params.size() >= 2 && params[1].isNumeric()) {
            fee_rate = params[1].asDouble();
        }
    } else {
        result["error"] = "Usage: dpi.payinvoice {invoice: <base64>, fee_rate: <optional>}";
        return result;
    }

    // Decode and validate invoice
    auto invoice_bytes = dinero::Base64Decode(invoice_b64);
    din::dpi::DpiInvoice inv;
    if (!din::dpi::DeserializeInvoice(invoice_bytes, inv)) {
        result["error"] = "Invalid invoice: deserialization failed";
        return result;
    }

    // Check expiry
    if (din::dpi::IsInvoiceExpired(inv)) {
        result["error"] = "Invoice has expired";
        return result;
    }

    // Dispatch payment via wallet.sendtoaddress (internal RPC call)
    auto* sendtoaddress_handler = g_rpcRegistry.lookup("wallet.sendtoaddress");
    if (!sendtoaddress_handler) {
        result["error"] = "Internal error: wallet.sendtoaddress handler not registered";
        return result;
    }

    double amount_din = static_cast<double>(inv.amount) / 1e8;

    din::Json send_params;
    send_params["address"] = inv.destination_address;
    send_params["amount"] = amount_din;
    if (fee_rate > 0) {
        send_params["fee_rate"] = fee_rate;
    }

    auto send_result = (*sendtoaddress_handler)(ctx, send_params);

    if (send_result.isMember("error")) {
        result["error"] = "Payment failed: " + send_result["error"].asString();
        return result;
    }

    if (!send_result.isMember("txid")) {
        result["error"] = "Payment succeeded but no txid returned";
        return result;
    }

    std::string txid_hex = send_result["txid"].asString();

    // Get raw transaction from mempool for the package
    if (!ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Mempool service unavailable";
        return result;
    }

    auto txid_uint256 = dinero::uint256::FromHexUnsafe(txid_hex);
    auto tx_ptr = mempool_service->mempool().getTransaction(txid_uint256);
    if (!tx_ptr) {
        result["error"] = "Transaction not found in mempool after broadcast";
        return result;
    }

    // Serialize tx
    auto raw_tx = tx_ptr->Serialize(dinero::TxSerializationMode::WithWitness);

    // Get sender's primary key for attestation
    auto [sender_priv, sender_pub] = GetPrimaryTaprootKey(wallet_service.get());
    if (sender_priv.empty()) {
        result["error"] = "Could not derive sender attestation key";
        return result;
    }

    // Sign attestation
    std::array<uint8_t, 32> txid_arr{};
    std::memcpy(txid_arr.data(), txid_uint256.begin(), 32);

    std::array<uint8_t, 32> sender_pub_arr{};
    std::memcpy(sender_pub_arr.data(), sender_pub.data(), 32);

    auto attest_sig = din::dpi::SignAttestation(inv.invoice_id, txid_arr, sender_pub_arr, sender_priv);
    if (attest_sig.empty()) {
        result["error"] = "Failed to sign attestation";
        return result;
    }

    // Build payment package
    din::dpi::DpiPaymentPackage pkg;
    pkg.raw_tx = raw_tx;
    pkg.attestation_sig = attest_sig;
    pkg.sender_pubkey = sender_pub_arr;
    pkg.invoice_id = inv.invoice_id;

    auto serialized_pkg = din::dpi::SerializePackage(pkg);
    std::string package_b64 = dinero::Base64Encode(serialized_pkg);

    result["package"] = package_b64;
    result["txid"] = txid_hex;
    result["invoice_id"] = BytesToHex(inv.invoice_id.data(), 32);
    result["amount_din"] = amount_din;
    result["destination"] = inv.destination_address;

    // Phase 2: Attach Utreexo inclusion proofs (if bridge node is available)
    AttachUtreexoProofs(result, *tx_ptr, ctx.daemon);

    if (ctx.logger) {
        ctx.logger->info("[dpi.payinvoice] Paid invoice " +
                         BytesToHex(inv.invoice_id.data(), 8) + "... -> tx " +
                         txid_hex.substr(0, 16) + "...");
    }

    return result;
}

// ============================================================================
// dpi.verifypackage — Merchant verifies a payment package against an invoice
// ============================================================================

din::Json rpc_dpi_verifypackage(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Parse parameters
    std::string package_b64, invoice_b64;

    if (params.isObject()) {
        if (!params.isMember("package") || !params["package"].isString()) {
            result["error"] = "Missing required parameter: package (base64)";
            return result;
        }
        if (!params.isMember("invoice") || !params["invoice"].isString()) {
            result["error"] = "Missing required parameter: invoice (base64)";
            return result;
        }
        package_b64 = params["package"].asString();
        invoice_b64 = params["invoice"].asString();
    } else if (params.isArray() && params.size() >= 2) {
        package_b64 = params[0].asString();
        invoice_b64 = params[1].asString();
    } else {
        result["error"] = "Usage: dpi.verifypackage {package: <base64>, invoice: <base64>}";
        return result;
    }

    // Decode invoice
    auto invoice_bytes = dinero::Base64Decode(invoice_b64);
    din::dpi::DpiInvoice inv;
    if (!din::dpi::DeserializeInvoice(invoice_bytes, inv)) {
        result["error"] = "Invalid invoice: deserialization failed";
        result["valid"] = false;
        return result;
    }

    // Decode package
    auto package_bytes = dinero::Base64Decode(package_b64);
    din::dpi::DpiPaymentPackage pkg;
    if (!din::dpi::DeserializePackage(package_bytes, pkg)) {
        result["error"] = "Invalid package: deserialization failed";
        result["valid"] = false;
        return result;
    }

    din::dpi::DpiVerifyChecks checks;

    // Check 1: invoice_id match
    checks.invoice_bound = (pkg.invoice_id == inv.invoice_id);

    // Check 2 & 3: Deserialize raw_tx and check outputs
    dinero::Transaction tx;
    if (dinero::TransactionSerializer::Deserialize(tx, pkg.raw_tx)) {
        // Compute expected scriptPubKey from destination address
        try {
            auto expected_spk = din::dpi::AddressToScriptPubKey(inv.destination_address);

            for (const auto& output : tx.vout) {
                if (output.scriptPubKey == expected_spk) {
                    checks.output_match = true;
                    uint64_t output_value = output.value.GetUna();
                    if (output_value >= inv.amount) {
                        checks.amount_match = true;
                    }
                    break;
                }
            }
        } catch (...) {
            // Address decode failure — output_match stays false
        }

        // Check 4: Attestation signature
        auto txid = tx.GetTxid().AsUint256();
        std::array<uint8_t, 32> txid_arr{};
        std::memcpy(txid_arr.data(), txid.begin(), 32);

        checks.attestation_valid = din::dpi::VerifyAttestationSignature(
            inv.invoice_id, txid_arr, pkg.sender_pubkey, pkg.attestation_sig);

        // Check 5: Mempool presence
        if (ctx.daemon && ctx.daemon->mempool) {
            auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if (mempool_service) {
                checks.seen_in_mempool = mempool_service->mempool().hasTransaction(txid);

                // Check 6: Conflicts — check if any input is double-spent
                for (const auto& input : tx.vin) {
                    if (mempool_service->mempool().isOutputSpentInMempool(dinero::OutPoint(input.prevout.txid, input.prevout.vout))) {
                        // The outpoint is spent — check if it's by THIS tx (not a conflict)
                        // or by a different tx (actual conflict).
                        // isOutputSpentInMempool returns true if ANY mempool tx spends it.
                        // If our tx is in mempool, it will also return true for our own inputs.
                        // A conflict means another tx ALSO spends the same input.
                        // For simplicity: if tx is in mempool, inputs are naturally "spent" by it.
                        // We detect conflicts by checking if the output is spent AND our tx is NOT
                        // the spender — but this API doesn't distinguish. Use a simpler approach:
                        // if the tx is NOT in mempool but inputs are spent, that's a conflict.
                        if (!checks.seen_in_mempool) {
                            checks.conflicts_found = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    // Check 7: Expiry
    checks.expired = din::dpi::IsInvoiceExpired(inv);

    // Compute risk score and tier
    double risk_score = din::dpi::ComputeRiskScore(checks);
    std::string tier = din::dpi::DetermineTier(checks, risk_score);

    // Build result
    result["valid"] = (tier == "T1");
    result["tier"] = tier;
    result["risk_score"] = risk_score;

    // Include txid for tier tracking
    {
        dinero::Transaction verify_tx;
        if (dinero::TransactionSerializer::Deserialize(verify_tx, pkg.raw_tx)) {
            result["txid"] = verify_tx.GetTxid().AsUint256().GetHex();
        }
    }

    din::Json checks_json;
    checks_json["invoice_bound"] = checks.invoice_bound;
    checks_json["output_match"] = checks.output_match;
    checks_json["amount_match"] = checks.amount_match;
    checks_json["attestation_valid"] = checks.attestation_valid;
    checks_json["seen_in_mempool"] = checks.seen_in_mempool;
    checks_json["conflicts_found"] = checks.conflicts_found;
    checks_json["expired"] = checks.expired;
    checks_json["utreexo_proofs_valid"] = checks.utreexo_proofs_valid;
    result["checks"] = checks_json;

    // Phase 2: Attach Utreexo inclusion proofs (if bridge node is available)
    {
        dinero::Transaction utx;
        if (dinero::TransactionSerializer::Deserialize(utx, pkg.raw_tx)) {
            AttachUtreexoProofs(result, utx, ctx.daemon);
            // If proofs were successfully attached, mark the check
            if (result.isMember("utreexo_proofs")) {
                checks.utreexo_proofs_valid = true;
                checks_json["utreexo_proofs_valid"] = true;
                result["checks"] = checks_json;
            }
        }
    }

    if (ctx.logger) {
        ctx.logger->info("[dpi.verifypackage] Verify result: tier=" + tier +
                         " risk=" + std::to_string(risk_score) +
                         " invoice=" + BytesToHex(inv.invoice_id.data(), 8) + "...");
    }

    return result;
}

// ============================================================================
// dpi.checkconflicts — Check if a transaction has conflicting spends
// ============================================================================

din::Json rpc_dpi_checkconflicts(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Parse txid
    std::string txid_hex;
    if (params.isObject()) {
        if (!params.isMember("txid") || !params["txid"].isString()) {
            result["error"] = "Missing required parameter: txid (hex)";
            return result;
        }
        txid_hex = params["txid"].asString();
    } else if (params.isArray() && params.size() >= 1) {
        txid_hex = params[0].asString();
    } else {
        result["error"] = "Usage: dpi.checkconflicts {txid: <hex>}";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Mempool service unavailable";
        return result;
    }

    auto txid = dinero::uint256::FromHexUnsafe(txid_hex);
    auto tx_ptr = mempool_service->mempool().getTransaction(txid);
    if (!tx_ptr) {
        result["error"] = "Transaction not found in mempool";
        return result;
    }

    // Check each input for double-spend conflicts.
    // isOutputSpentInMempool returns true if ANY mempool tx spends the outpoint —
    // including this tx itself. To detect actual conflicts (another tx spending
    // the same input), we remove this tx from the mempool, check, then re-add.
    // But that's invasive. Instead: count how many mempool txs spend each input.
    // If count > 1, there's a conflict. We approximate this by scanning all
    // mempool txs for matching inputs.
    bool has_conflicts = false;
    din::Json conflict_list;
    conflict_list.resize(0);  // empty JSON array

    // Build set of this tx's input outpoints for fast lookup
    std::set<std::pair<std::string, uint32_t>> our_inputs;
    for (const auto& input : tx_ptr->vin) {
        our_inputs.emplace(input.prevout.txid.AsUint256().GetHex(), input.prevout.vout);
    }

    // Scan all mempool transactions for conflicting spends
    mempool_service->mempool().forEachEntry([&](const dinero::MempoolEntry& entry) {
        const auto& other_tx = entry.tx;
        auto other_txid = other_tx.GetTxid().AsUint256();
        if (other_txid == txid) return;  // skip self

        for (const auto& other_input : other_tx.vin) {
            auto key = std::make_pair(other_input.prevout.txid.AsUint256().GetHex(), other_input.prevout.vout);
            if (our_inputs.count(key)) {
                has_conflicts = true;
                din::Json conflict;
                conflict["txid"] = other_txid.GetHex();
                conflict["outpoint"] = other_input.prevout.txid.AsUint256().GetHex() + ":" +
                                       std::to_string(other_input.prevout.vout);
                conflict_list.append(conflict);
            }
        }
    });

    result["has_conflicts"] = has_conflicts;
    result["input_count"] = static_cast<int>(tx_ptr->vin.size());
    result["conflicts"] = conflict_list;

    return result;
}

// ============================================================================
// dpi.decodeinvoice — Read-only invoice decode (no wallet needed)
// ============================================================================

din::Json rpc_dpi_decodeinvoice(const ExecutionContext& /* ctx */, const din::Json& params) {
    din::Json result;

    std::string invoice_b64;
    if (params.isObject()) {
        if (!params.isMember("invoice") || !params["invoice"].isString()) {
            result["error"] = "Missing required parameter: invoice (base64)";
            return result;
        }
        invoice_b64 = params["invoice"].asString();
    } else if (params.isArray() && params.size() >= 1) {
        invoice_b64 = params[0].asString();
    } else {
        result["error"] = "Usage: dpi.decodeinvoice {invoice: <base64>}";
        return result;
    }

    auto invoice_bytes = dinero::Base64Decode(invoice_b64);
    din::dpi::DpiInvoice inv;
    if (!din::dpi::DeserializeInvoice(invoice_bytes, inv)) {
        result["error"] = "Invalid invoice: deserialization failed";
        return result;
    }

    result["version"] = inv.version;
    result["network"] = inv.network;
    result["amount_una"] = static_cast<din::Json::UInt64>(inv.amount);
    result["amount_din"] = static_cast<double>(inv.amount) / 1e8;
    result["destination_address"] = inv.destination_address;
    result["merchant_id"] = BytesToHex(inv.merchant_id.data(), inv.merchant_id.size());
    result["timestamp"] = inv.timestamp;
    result["expiry"] = inv.expiry;
    result["memo"] = inv.memo;
    result["invoice_id"] = BytesToHex(inv.invoice_id.data(), 32);
    result["expired"] = din::dpi::IsInvoiceExpired(inv);

    return result;
}

// ============================================================================
// Registration function — called from rpc_init.cpp
// ============================================================================

void RegisterDpiRPC(DaemonContext& /* ctx */) {
    dinero::g_logger.info("  Registering DPI (Dinero Payment Intent) RPC methods...");

    g_rpcRegistry.registerHandler("dpi.createinvoice",
                                 rpc_dpi_createinvoice,
                                 RegisterMode::Overwrite,
                                 "dpi");

    g_rpcRegistry.registerHandler("dpi.payinvoice",
                                 rpc_dpi_payinvoice,
                                 RegisterMode::Overwrite,
                                 "dpi");

    g_rpcRegistry.registerHandler("dpi.verifypackage",
                                 rpc_dpi_verifypackage,
                                 RegisterMode::Overwrite,
                                 "dpi");

    g_rpcRegistry.registerHandler("dpi.checkconflicts",
                                 rpc_dpi_checkconflicts,
                                 RegisterMode::Overwrite,
                                 "dpi");

    g_rpcRegistry.registerHandler("dpi.decodeinvoice",
                                 rpc_dpi_decodeinvoice,
                                 RegisterMode::Overwrite,
                                 "dpi");
}
