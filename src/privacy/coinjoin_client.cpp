#include "privacy/coinjoin_client.h"
#include "daemon/execution_context.h"
#include "wallet/psbt.h"
#include "wallet/taproot_address.h"
#include "net/socks5.h"
#include "net/proxy_manager.h"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core.hpp>
#include <json/json.h>

#include <stdexcept>
#include <sstream>

namespace din {

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

// --- small URL splitter (https not handled here; terminate TLS upstream/proxy if needed) ---
static void parse_url(const std::string& full, std::string& host, std::string& port, std::string& target) {
  // Expect: http://host[:port]/path...
  std::string s = full;
  const std::string pre = "http://";
  if (s.rfind(pre, 0) == 0) s = s.substr(pre.size());
  auto slash = s.find('/');
  std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
  target = (slash == std::string::npos) ? "/" : s.substr(slash);
  auto colon = hostport.find(':');
  if (colon == std::string::npos) { host = hostport; port = "80"; }
  else { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }
}

CoinJoinClient::CoinJoinClient(ExecutionContext* ctx,
                               SelectInputsFn select_inputs,
                               BuildEqualOutputScriptFn build_equal_spk,
                               BuildChangeScriptFn build_change_spk,
                               SignPsbtFn sign_psbt)
  : ctx_(ctx),
    select_inputs_(std::move(select_inputs)),
    build_equal_spk_(std::move(build_equal_spk)),
    build_change_spk_(std::move(build_change_spk)),
    sign_psbt_(std::move(sign_psbt)) {}

// --- HTTP helpers (blocking, simple). In production you can reuse a shared io_context and timeouts.
std::string CoinJoinClient::http_post_json(const std::string& url, const std::string& body) {
  std::string host, port, target;
  parse_url(url, host, port, target);

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto const results = resolver.resolve(host, port);

  tcp::socket socket(ioc);
  
  // TODO: Add proxy support when ExecutionContext includes proxy_manager
  // For now, use direct connection
  boost::asio::connect(socket, results.begin(), results.end());

  http::request<http::string_body> req{http::verb::post, target, 11};
  req.set(http::field::host, host);
  req.set(http::field::content_type, "application/json");
  req.body() = body;
  req.prepare_payload();

  http::write(socket, req);
  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);
  boost::system::error_code ec;
  socket.shutdown(tcp::socket::shutdown_both, ec);

  if (res.result() != http::status::ok)
    throw std::runtime_error("HTTP POST failed: " + std::to_string((int)res.result()));
  return res.body();
}

std::string CoinJoinClient::http_get_json(const std::string& url) {
  std::string host, port, target;
  parse_url(url, host, port, target);

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto const results = resolver.resolve(host, port);

  tcp::socket socket(ioc);
  
  // TODO: Add proxy support when ExecutionContext includes proxy_manager
  // For now, use direct connection
  boost::asio::connect(socket, results.begin(), results.end());

  http::request<http::empty_body> req{http::verb::get, target, 11};
  req.set(http::field::host, host);
  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);
  boost::system::error_code ec;
  socket.shutdown(tcp::socket::shutdown_both, ec);

  if (res.result() != http::status::ok)
    throw std::runtime_error("HTTP GET failed: " + std::to_string((int)res.result()));
  return res.body();
}

// --- Coordinator API (minimal JSON contract)
// POST  {base}/register   -> { "round_id": "...", "min_peers": N }
// POST  {base}/inputs     -> { "round_id":"...", "inputs":[{txid,vout,value}], "equal_spk":"hex", "change_spk":"hex", "fee_rate": n }
// GET   {base}/status?round_id=... -> { "phase":"collect|psbt|sign|done|fail", "peers":N, "txid":"...", "detail":"..." }
// GET   {base}/psbt?round_id=...   -> { "psbt":"<base64>" }
// POST  {base}/submit              -> { "round_id":"...", "psbt":"<base64>" }
// POST  {base}/cancel              -> { "round_id":"..." }

std::string CoinJoinClient::join_round(const CJParams& p) {
  // 1) Register
  {
    Json::Value req; req["min_peers"] = p.min_peers;
    auto resp_s = http_post_json(p.coordinator_url + "/register", Json::writeString(Json::StreamWriterBuilder{}, req));
    Json::CharReaderBuilder rb; Json::Value rj; std::string errs;
    auto* rd = rb.newCharReader();
    if (!rd->parse(resp_s.data(), resp_s.data()+resp_s.size(), &rj, &errs)) { delete rd; throw std::runtime_error("bad register json"); }
    delete rd;
    if (!rj.isMember("round_id")) throw std::runtime_error("register: no round_id");
    // 2) Select inputs locally for target + fees
    auto inputs = select_inputs_(p.target_amt, p.fee_rate_una_vb);
    if (inputs.empty()) throw std::runtime_error("no spendable inputs");
    // 3) Build equal-output + (optional) change scripts
    auto equal_spk  = build_equal_spk_();
    auto change_spk = build_change_spk_(); // may be empty (coordinator will decide)
    // 4) Submit inputs & scripts
    Json::Value inreq;
    inreq["round_id"] = rj["round_id"];
    inreq["fee_rate"] = Json::Int64(p.fee_rate_una_vb);
    inreq["equal_spk"]  = /* hex */ [&](){
      static const char* hexd="0123456789abcdef"; std::string h; h.reserve(equal_spk.size()*2);
      for (auto b: equal_spk){ h.push_back(hexd[b>>4]); h.push_back(hexd[b&0xF]); } return h; }();
    inreq["change_spk"] = [&](){
      static const char* hexd="0123456789abcdef"; std::string h; h.reserve(change_spk.size()*2);
      for (auto b: change_spk){ h.push_back(hexd[b>>4]); h.push_back(hexd[b&0xF]); } return h; }();
    auto& arr = (inreq["inputs"] = Json::arrayValue);
    for (const auto& in : inputs) {
      Json::Value j; j["txid"]=in.txid; j["vout"]=in.vout; j["value"]=Json::Int64(in.value);
      arr.append(j);
    }
    auto inresp_s = http_post_json(p.coordinator_url + "/inputs", Json::writeString(Json::StreamWriterBuilder{}, inreq));
    // ignore body; coordinator now tracks us
    return rj["round_id"].asString();
  }
}

CJStatus CoinJoinClient::fetch_status(const std::string& base, const std::string& round_id) {
  auto s = http_get_json(base + "/status?round_id=" + round_id);
  Json::CharReaderBuilder rb; Json::Value j; std::string errs;
  auto* rd = rb.newCharReader();
  if (!rd->parse(s.data(), s.data()+s.size(), &j, &errs)) { delete rd; throw std::runtime_error("bad status json"); }
  delete rd;

  CJStatus st; st.round_id = round_id; st.peers = j.get("peers",0).asInt(); st.detail = j.get("detail","").asString();
  std::string ph = j.get("phase","").asString();
  if      (ph=="collect") st.phase = CJPhase::Waiting;
  else if (ph=="psbt")    st.phase = CJPhase::PsbtReady;
  else if (ph=="sign")    st.phase = CJPhase::Signing;
  else if (ph=="done")    { st.phase = CJPhase::Done; st.done=true; st.txid = j.get("txid","").asString(); }
  else if (ph=="fail")    st.phase = CJPhase::Failed;
  else                    st.phase = CJPhase::Unknown;
  return st;
}

std::string CoinJoinClient::fetch_psbt(const std::string& base, const std::string& round_id) {
  auto s = http_get_json(base + "/psbt?round_id=" + round_id);
  Json::CharReaderBuilder rb; Json::Value j; std::string errs;
  auto* rd = rb.newCharReader();
  if (!rd->parse(s.data(), s.data()+s.size(), &j, &errs)) { delete rd; throw std::runtime_error("bad psbt json"); }
  delete rd;
  if (!j.isMember("psbt")) throw std::runtime_error("no psbt");
  return j["psbt"].asString();
}

bool CoinJoinClient::submit_signed(const std::string& base, const std::string& round_id, const std::string& psbt_b64) {
  Json::Value req; req["round_id"] = round_id; req["psbt"] = psbt_b64;
  auto s = http_post_json(base + "/submit", Json::writeString(Json::StreamWriterBuilder{}, req));
  (void)s; // could parse ACK
  return true;
}

CJStatus CoinJoinClient::poll(const std::string& round_id) {
  // Coordinator URL is unknown here; we stash it in round_id or pass full URL elsewhere.
  // Simple convention: callers pass full base as "round_id@base" or store mapping externally.
  // For simplicity, expect "round_id|baseurl".
  auto sep = round_id.find('|');
  std::string rid = (sep==std::string::npos) ? round_id : round_id.substr(0,sep);
  std::string base= (sep==std::string::npos) ? ""       : round_id.substr(sep+1);
  if (base.empty()) throw std::runtime_error("poll: base url missing (use rid|base form)");

  CJStatus st = fetch_status(base, rid);

  if (st.phase == CJPhase::PsbtReady) {
    // Fetch PSBT, sign it, submit
    auto psbt_b64 = fetch_psbt(base, rid);
    auto signed_b64 = sign_psbt_(psbt_b64);
    if (!submit_signed(base, rid, signed_b64)) {
      st.phase = CJPhase::Failed; st.detail = "submit failed";
      return st;
    }
    st.phase = CJPhase::Submitted; st.psbt_b64 = ""; // we already submitted
    return st;
  }
  if (st.phase == CJPhase::Signing) {
    // Some coordinators separate 'psbt ready' and 'sign' phases; re-fetch to be safe
    auto psbt_b64 = fetch_psbt(base, rid);
    auto signed_b64 = sign_psbt_(psbt_b64);
    submit_signed(base, rid, signed_b64);
    st.phase = CJPhase::Submitted;
    return st;
  }
  return st;
}

bool CoinJoinClient::cancel(const std::string& round_id) {
  auto sep = round_id.find('|');
  std::string rid = (sep==std::string::npos) ? round_id : round_id.substr(0,sep);
  std::string base= (sep==std::string::npos) ? ""       : round_id.substr(sep+1);
  if (base.empty()) throw std::runtime_error("cancel: base url missing");
  Json::Value req; req["round_id"] = rid;
  (void)http_post_json(base + "/cancel", Json::writeString(Json::StreamWriterBuilder{}, req));
  return true;
}

} // namespace din
