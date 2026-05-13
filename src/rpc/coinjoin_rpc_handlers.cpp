#include "privacy/coinjoin_client.h"
#include "daemon/execution_context.h"
#include <json/json.h>

namespace din::rpc {

static CoinJoinClient* g_cj = nullptr; // wire in daemon init with proper callbacks

Json::Value coinjoinjoin(ExecutionContext& ctx, const Json::Value& req) {
  if (!g_cj) throw std::runtime_error("coinjoin disabled");
  CJParams p;
  p.coordinator_url   = req.get("coordinator","").asString();
  p.target_amt        = req.get("amount", 0).asInt64();
  p.fee_rate_una_vb   = req.get("feerate", 5).asInt64();
  p.min_peers         = req.get("min_peers", 3).asInt();

  auto rid = g_cj->join_round(p);
  // Return compound token rid|base so poll/cancel can work statelessly
  Json::Value out; out["round_id"] = rid + "|" + p.coordinator_url; return out;
}

Json::Value coinjoinstatus(ExecutionContext& ctx, const Json::Value& req) {
  if (!g_cj) throw std::runtime_error("coinjoin disabled");
  auto rid = req.get("round_id","").asString();
  auto st = g_cj->poll(rid);
  Json::Value out;
  out["round_id"] = st.round_id;
  switch (st.phase) {
    case CJPhase::Registering: out["phase"]="registering"; break;
    case CJPhase::Waiting:     out["phase"]="waiting"; break;
    case CJPhase::PsbtReady:   out["phase"]="psbt"; break;
    case CJPhase::Signing:     out["phase"]="sign"; break;
    case CJPhase::Submitted:   out["phase"]="submitted"; break;
    case CJPhase::Done:        out["phase"]="done"; out["txid"]=st.txid; out["done"]=true; break;
    case CJPhase::Failed:      out["phase"]="fail"; break;
    default:                   out["phase"]="unknown"; break;
  }
  out["peers"]  = st.peers;
  out["detail"] = st.detail;
  return out;
}

Json::Value coinjoincancel(ExecutionContext& ctx, const Json::Value& req) {
  if (!g_cj) throw std::runtime_error("coinjoin disabled");
  auto rid = req.get("round_id","").asString();
  bool ok = g_cj->cancel(rid);
  Json::Value out; out["ok"] = ok; return out;
}

} // namespace din::rpc
