#include "privacy/payjoin_receiver.h"
#include "dinero/core/daemon/execution_context.h"
#include "dinero/core/wallet/tx_builder_iface.h"
#include "dinero/core/wallet/psbt.h"
#include "dinero/core/wallet/taproot_address.h"
#include <json/json.h>
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace din {

// --- tiny tx (unsigned) codec (legacy format, no witnesses) ---
namespace txcodec {

struct Out { int64_t value; std::vector<uint8_t> spk; };
struct In  { std::array<uint8_t,32> prev; uint32_t vout; std::vector<uint8_t> scriptSig; uint32_t seq; };
struct Tx  { int32_t ver; std::vector<In> vin; std::vector<Out> vout; uint32_t lock; };

static void put_u32(std::vector<uint8_t>& b, uint32_t v){ for(int i=0;i<4;i++) b.push_back(uint8_t((v>>(8*i))&0xFF)); }
static void put_u64(std::vector<uint8_t>& b, uint64_t v){ for(int i=0;i<8;i++) b.push_back(uint8_t((v>>(8*i))&0xFF)); }
static void put_var(std::vector<uint8_t>& b, uint64_t v){
  if (v<0xFD) b.push_back(uint8_t(v));
  else if (v<=0xFFFF){ b.push_back(0xFD); b.push_back(v&0xFF); b.push_back((v>>8)&0xFF); }
  else if (v<=0xFFFF'FFFFULL){ b.push_back(0xFE); for(int i=0;i<4;i++) b.push_back((v>>(8*i))&0xFF); }
  else { b.push_back(0xFF); for(int i=0;i<8;i++) b.push_back((v>>(8*i))&0xFF); }
}
static bool get_u32(const uint8_t*& p, const uint8_t* e, uint32_t& v){ if(e-p<4) return false; v=0; for(int i=0;i<4;i++) v|=(uint32_t(p[i])<<(8*i)); p+=4; return true; }
static bool get_u64(const uint8_t*& p, const uint8_t* e, uint64_t& v){ if(e-p<8) return false; v=0; for(int i=0;i<8;i++) v|=(uint64_t(p[i])<<(8*i)); p+=8; return true; }
static bool get_var(const uint8_t*& p, const uint8_t* e, uint64_t& v){
  if(p>=e) return false; uint8_t d=*p++;
  if(d<0xFD){ v=d; return true; }
  if(d==0xFD){ if(e-p<2) return false; v=uint64_t(p[0])|(uint64_t(p[1])<<8); p+=2; return true; }
  if(d==0xFE){ if(e-p<4) return false; v=uint64_t(p[0])|(uint64_t(p[1])<<8)|(uint64_t(p[2])<<16)|(uint64_t(p[3])<<24); p+=4; return true; }
  if(e-p<8) return false; v=0; for(int i=0;i<8;i++) v|=(uint64_t(p[i])<<(8*i)); p+=8; return true;
}

static bool parse(const std::vector<uint8_t>& raw, Tx& tx){
  const uint8_t* p=raw.data(); const uint8_t* e=raw.data()+raw.size();
  uint32_t ver=0; if(!get_u32(p,e,ver)) return false; tx.ver=int32_t(ver);
  uint64_t nin=0; if(!get_var(p,e,nin)) return false;
  tx.vin.resize(nin);
  for(size_t i=0;i<nin;i++){
    if(e-p<36) return false; std::copy(p,p+32,tx.vin[i].prev.begin()); p+=32;
    uint32_t vout=0; if(!get_u32(p,e,vout)) return false; tx.vin[i].vout=vout;
    uint64_t sl=0; if(!get_var(p,e,sl)) return false; if(e-p<(ptrdiff_t)sl) return false;
    tx.vin[i].scriptSig.assign(p,p+sl); p+=sl;
    if(!get_u32(p,e,tx.vin[i].seq)) return false;
  }
  uint64_t nout=0; if(!get_var(p,e,nout)) return false;
  tx.vout.resize(nout);
  for(size_t i=0;i<nout;i++){
    uint64_t val=0; if(!get_u64(p,e,val)) return false; tx.vout[i].value=(int64_t)val;
    uint64_t pl=0; if(!get_var(p,e,pl)) return false; if(e-p<(ptrdiff_t)pl) return false;
    tx.vout[i].spk.assign(p,p+pl); p+=pl;
  }
  uint32_t lock=0; if(!get_u32(p,e,lock)) return false; tx.lock=lock;
  return p==e;
}

static std::vector<uint8_t> serialize(const Tx& tx){
  std::vector<uint8_t> b; b.reserve(64 + tx.vin.size()*64 + tx.vout.size()*40);
  put_u32(b, uint32_t(tx.ver));
  put_var(b, tx.vin.size());
  for(const auto& in: tx.vin){
    b.insert(b.end(), in.prev.begin(), in.prev.end());
    put_u32(b, in.vout);
    put_var(b, in.scriptSig.size());
    b.insert(b.end(), in.scriptSig.begin(), in.scriptSig.end());
    put_u32(b, in.seq);
  }
  put_var(b, tx.vout.size());
  for(const auto& o: tx.vout){
    put_u64(b, (uint64_t)o.value);
    put_var(b, o.spk.size());
    b.insert(b.end(), o.spk.begin(), o.spk.end());
  }
  put_u32(b, tx.lock);
  return b;
}

} // namespace txcodec

// --- utility: hex -> bytes (txid big-endian to little-endian) ---
static std::vector<uint8_t> hex2(const std::string& h){
  auto hexv=[&](char c)->int{ if('0'<=c&&c<='9')return c-'0'; if('a'<=c&&c<='f')return c-'a'+10; if('A'<=c&&c<='F')return c-'A'+10; return -1; };
  std::vector<uint8_t> o; o.reserve(h.size()/2);
  for(size_t i=0;i+1<h.size();i+=2){ int hi=hexv(h[i]), lo=hexv(h[i+1]); if(hi<0||lo<0) break; o.push_back(uint8_t((hi<<4)|lo)); }
  return o;
}
static void txid_be_to_le32(const std::string& txid_be, std::array<uint8_t,32>& out){
  auto v=hex2(txid_be); // big-endian display → bytes (big)
  // TXIDs are printed reversed; UnsignedTx expects little-endian internal
  std::reverse(v.begin(), v.end());
  if(v.size()!=32) throw std::runtime_error("bad txid len");
  std::copy(v.begin(), v.end(), out.begin());
}

// --- PayjoinReceiver impl ---

PayjoinReceiver::PayjoinReceiver(ExecutionContext* ctx,
                                 PayjoinOffer offer,
                                 SelectUtxoFn select,
                                 GetChangeScriptFn get_change_spk,
                                 int64_t min_relay_feerate_una_vb)
: ctx_(ctx), offer_(std::move(offer)), select_(std::move(select)),
  get_change_spk_(std::move(get_change_spk)),
  min_relay_feerate_una_vb_(min_relay_feerate_una_vb) {}

int64_t PayjoinReceiver::dust_threshold_p2wpkh(int64_t minrelay){
  // Typical: 3 * in_vbytes_to_spend_output * feerate
  constexpr int in_vb_spend_p2wpkh = 68;
  return 3 * in_vb_spend_p2wpkh * std::max<int64_t>(1, minrelay);
}
int64_t PayjoinReceiver::dust_threshold_p2tr(int64_t minrelay){
  constexpr int in_vb_spend_p2tr = 57;
  return 3 * in_vb_spend_p2tr * std::max<int64_t>(1, minrelay);
}

std::string PayjoinReceiver::handle(const std::string& form_psbt_b64,
                                    int64_t min_feerate_una_vb,
                                    bool disable_output_substitution) {
  using namespace din;

  const int64_t feerate = (min_feerate_una_vb > 0) ? min_feerate_una_vb : min_relay_feerate_una_vb_;

  // 1) Parse incoming PSBT
  auto raw_in = din::from_base64(form_psbt_b64);
  din::Psbt psbt_in;
  if (!din::deserialize(raw_in, psbt_in)) {
    throw std::runtime_error("invalid PSBT");
  }

  // Extract unsigned tx to locate the payee output (invoice_spk)
  std::vector<uint8_t> unsigned_tx;
  for (const auto& kv : psbt_in.globals) {
    if (!kv.key.empty() && kv.key[0] == 0x00) { // UnsignedTx
      unsigned_tx = kv.value; break;
    }
  }
  txcodec::Tx tx{};
  if (!txcodec::parse(unsigned_tx, tx)) {
    throw std::runtime_error("invalid unsigned tx");
  }

  // 2) Find the payee output (must match invoice script)
  int payee_index = -1;
  for (size_t i=0;i<tx.vout.size();++i) {
    if (tx.vout[i].spk == offer_.invoice_spk) { payee_index = int(i); break; }
  }
  if (payee_index < 0) throw std::runtime_error("no invoice output found");

  // 3) Select a receiver UTXO to contribute (≥ dust + fee headroom)
  const int64_t min_contrib = std::max<int64_t>(offer_.amount / 20, 2000); // policy: at least 2k sats or 5%
  auto utxo_opt = select_(min_contrib);
  if (!utxo_opt) throw std::runtime_error("no suitable receiver utxo");

  const ReceiverUtxo utxo = *utxo_opt;
  const bool is_tr = (utxo.type == ReceiverUtxo::Type::P2TR);

  // 4) Compute fee delta for adding one input (+ optional change)
  const int in_vb   = is_tr ? 58 : 68;
  const int chg_vb  = is_tr ? 43 : 31; // P2TR or P2WPKH change
  const int64_t add_fee_for_input = in_vb * feerate;

  // Decide if we add change; if change would be dust, omit it and pay as fee.
  const int64_t dust = is_tr ? dust_threshold_p2tr(feerate) : dust_threshold_p2wpkh(feerate);

  int64_t change_value = 0;
  int64_t add_fee_for_change = chg_vb * feerate;

  // Draft reduction of payee output to preserve sender outputs
  int64_t hypothetical_reduction_with_change = utxo.value - add_fee_for_input - add_fee_for_change - dust;
  bool will_add_change = hypothetical_reduction_with_change >= 0;

  if (will_add_change) {
    change_value = utxo.value - add_fee_for_input - add_fee_for_change;
  } else {
    change_value = 0;
  }

  // 5) Apply the transformation: add our input, optional change, adjust payee output
  txcodec::In nin{};
  txid_be_to_le32(utxo.txid, nin.prev);
  nin.vout = utxo.vout;
  nin.scriptSig.clear();
  nin.seq = 0xFFFFFFFD; // RBF
  tx.vin.push_back(std::move(nin));

  // Adjust payee output (reduce it)
  int64_t reduction = utxo.value - add_fee_for_input - (will_add_change ? add_fee_for_change + change_value : 0);
  if (!will_add_change) {
    reduction = utxo.value - add_fee_for_input;
  }
  if (reduction <= 0) throw std::runtime_error("receiver utxo too small after fees");

  if (tx.vout[payee_index].value - reduction <= 0) {
    throw std::runtime_error("payee output would be negative");
  }
  tx.vout[payee_index].value -= reduction;

  // Optional change output at end
  if (will_add_change) {
    auto chg_spk = get_change_spk_();
    if (!chg_spk.empty()) {
      txcodec::Out chg{ change_value, std::move(chg_spk) };
      const int64_t chg_dust = is_tr ? dust_threshold_p2tr(feerate) : dust_threshold_p2wpkh(feerate);
      if (chg.value >= chg_dust) {
        tx.vout.push_back(std::move(chg));
      }
    }
  }

  // 6) Rebuild UnsignedTx in PSBT
  auto unsigned_new = txcodec::serialize(tx);

  // Replace global unsigned tx in psbt
  for (auto& kv : psbt_in.globals) {
    if (!kv.key.empty() && kv.key[0] == 0x00) { kv.value = unsigned_new; }
  }

  // Add PSBT input map for the newly appended input
  const size_t new_in_idx = psbt_in.vin.size();
  psbt_in.vin.resize(psbt_in.vin.size() + 1);

  // Attach UTXO info + derivation for receiver input
  if (is_tr) {
    din::add_in_witness_utxo(psbt_in, new_in_idx, utxo.scriptPubKey, (uint64_t)utxo.value);
  } else {
    din::add_in_witness_utxo(psbt_in, new_in_idx, utxo.scriptPubKey, (uint64_t)utxo.value);
    if (!utxo.pubkey33.empty()) {
      din::add_in_bip32_deriv(psbt_in, new_in_idx, utxo.pubkey33, std::vector<uint8_t>(utxo.master_fpr.begin(), utxo.master_fpr.end()), utxo.path);
    }
  }

  // 7) Sign receiver input (single-round PayJoin)
  if (ctx_ && ctx_->tx_builder) {
    // TODO[wallet][P1]: sign receiver input for PayJoin in receiver path
  }

  // 8) Disable output substitution if requested
  (void)disable_output_substitution;

  // Return PSBT base64
  auto raw_out = din::serialize(psbt_in);
  return din::to_base64(raw_out);
}

} // namespace din