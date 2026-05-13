// LEGACY PRIVACY WALLET CODE (din::)
//
// This file implements privacy-related PSBT flows (payjoin / coinjoin).
// These features are currently DISABLED and FROZEN.
// Do NOT extend or modify.
// Scheduled for full removal in a future cleanup PR.

#include "dinero/core/wallet/psbt.h"
#include "wallet/psbt_witness_utxo_decode.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>

namespace din {

// ---------- CompactSize varint ----------
static void put_compact_size(std::vector<uint8_t>& out, uint64_t v) {
  if (v < 0xFD) {
    out.push_back(uint8_t(v));
  } else if (v <= 0xFFFF) {
    out.push_back(0xFD);
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t((v >> 8) & 0xFF));
  } else if (v <= 0xFFFF'FFFFULL) {
    out.push_back(0xFE);
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
  } else {
    out.push_back(0xFF);
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
  }
}

static bool get_compact_size(const uint8_t*& p, const uint8_t* e, uint64_t& v) {
  if (p >= e) return false;
  uint8_t d = *p++;
  if (d < 0xFD) { v = d; return true; }
  if (d == 0xFD) {
    if (e - p < 2) return false;
    v = uint64_t(p[0]) | (uint64_t(p[1]) << 8);
    p += 2; return true;
  }
  if (d == 0xFE) {
    if (e - p < 4) return false;
    v = uint64_t(p[0]) | (uint64_t(p[1]) << 8) | (uint64_t(p[2]) << 16) | (uint64_t(p[3]) << 24);
    p += 4; return true;
  }
  if (e - p < 8) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t(p[i]) << (8 * i));
  p += 8; return true;
}

// ---------- LE helpers ----------
static inline void put_le32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i=0;i<4;i++) b.push_back(uint8_t((v>>(8*i)) & 0xFF));
}
static inline void put_le64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i=0;i<8;i++) b.push_back(uint8_t((v>>(8*i)) & 0xFF));
}
static inline bool get_le32(const uint8_t*& p, const uint8_t* e, uint32_t& v) {
  if (e - p < 4) return false; v = 0;
  for (int i=0;i<4;i++) v |= (uint32_t(p[i]) << (8*i));
  p += 4; return true;
}
static inline bool get_le64(const uint8_t*& p, const uint8_t* e, uint64_t& v) {
  if (e - p < 8) return false; v = 0;
  for (int i=0;i<8;i++) v |= (uint64_t(p[i]) << (8*i));
  p += 8; return true;
}

// ---------- Minimal unsigned-tx parser ----------
static bool parse_unsigned_tx_counts(const std::vector<uint8_t>& tx, uint64_t& n_in, uint64_t& n_out) {
  const uint8_t* p = tx.data();
  const uint8_t* e = tx.data() + tx.size();
  uint32_t nVersion = 0;
  if (!get_le32(p, e, nVersion)) return false;

  // vin
  if (!get_compact_size(p, e, n_in)) return false;
  for (uint64_t i = 0; i < n_in; ++i) {
    // prevout: 32-byte hash + 4-byte index
    if (e - p < 36) return false;
    p += 36;
    // scriptSig length + scriptSig
    uint64_t script_len = 0;
    if (!get_compact_size(p, e, script_len)) return false;
    if (e - p < (ptrdiff_t)script_len) return false;
    p += script_len;
    // sequence
    if (e - p < 4) return false;
    p += 4;
  }

  // vout
  if (!get_compact_size(p, e, n_out)) return false;
  for (uint64_t i = 0; i < n_out; ++i) {
    // value
    uint64_t value = 0;
    if (!get_le64(p, e, value)) return false;
    // scriptPubKey
    uint64_t pk_len = 0;
    if (!get_compact_size(p, e, pk_len)) return false;
    if (e - p < (ptrdiff_t)pk_len) return false;
    p += pk_len;
  }

  // locktime
  uint32_t nLockTime = 0;
  if (!get_le32(p, e, nLockTime)) return false;

  return true;
}

// ---------- PSBT serialization ----------
static const uint8_t PSBT_MAGIC[5] = { 0x70, 0x73, 0x62, 0x74, 0xFF };

static void write_map(std::vector<uint8_t>& out, const std::vector<PsbtMapKV>& kvs) {
  for (const auto& kv : kvs) {
    put_compact_size(out, kv.key.size());
    out.insert(out.end(), kv.key.begin(), kv.key.end());
    put_compact_size(out, kv.value.size());
    out.insert(out.end(), kv.value.begin(), kv.value.end());
  }
  out.push_back(0x00); // map terminator
}

static bool read_map(const uint8_t*& p, const uint8_t* e, std::vector<PsbtMapKV>& kvs) {
  for (;;) {
    if (p >= e) {
      return false;
    }
    uint64_t klen = 0;
    if (!get_compact_size(p, e, klen)) {
      return false;
    }
    if (klen == 0) {
      return true; // terminator
    }
    
    if (e - p < (ptrdiff_t)klen) {
      return false;
    }
    std::vector<uint8_t> key(p, p + klen);
    p += klen;

    uint64_t vlen = 0;
    if (!get_compact_size(p, e, vlen)) {
      return false;
    }
    if (e - p < (ptrdiff_t)vlen) {
      return false;
    }
    std::vector<uint8_t> val(p, p + vlen);
    p += vlen;

    kvs.push_back({ std::move(key), std::move(val) });
  }
}

std::vector<uint8_t> serialize(const Psbt& psbt) {
  std::vector<uint8_t> out;
  out.reserve(1024);
  out.insert(out.end(), PSBT_MAGIC, PSBT_MAGIC + 5);
  write_map(out, psbt.globals);
  for (const auto& in : psbt.inputs) write_map(out, in.kv);
  for (const auto& o : psbt.outputs) write_map(out, o.kv);
  return out;
}

bool deserialize(const std::vector<uint8_t>& raw, Psbt& out_psbt) {
  out_psbt = Psbt{};
  if (raw.size() < 5) return false;
  if (!std::equal(PSBT_MAGIC, PSBT_MAGIC + 5, raw.begin())) return false;

  const uint8_t* p = raw.data() + 5;
  const uint8_t* e = raw.data() + raw.size();

  for (int i = 0; i < 20 && p + i < e; ++i) {
  }

  if (!read_map(p, e, out_psbt.globals)) return false;

  // Determine PSBT version
  for (const auto& kv : out_psbt.globals) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtGlobal::Version)) {
      if (kv.value.size() == 1) {
        out_psbt.version = kv.value[0];
      }
      break;
    }
  }

  uint64_t n_in = 0, n_out = 0;
  
  if (out_psbt.version == 0) {
    // PSBTv0: Find unsigned tx to get input/output counts
    std::vector<uint8_t> unsigned_tx;
    for (const auto& kv : out_psbt.globals) {
      if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtGlobal::UnsignedTx)) {
        unsigned_tx = kv.value;
        break;
      }
    }
    if (unsigned_tx.empty()) return false;
    
    if (!parse_unsigned_tx_counts(unsigned_tx, n_in, n_out)) return false;
  } else if (out_psbt.version == 2) {
    // PSBTv2: Get counts from global fields or derive from input/output maps
    auto input_count = get_global_input_count(out_psbt);
    auto output_count = get_global_output_count(out_psbt);
    
    if (input_count) {
      n_in = *input_count;
    }
    if (output_count) {
      n_out = *output_count;
    }
    
    // If no global counts, we'll derive from the number of input/output maps
    // This will be handled after reading the maps
  } else {
    return false; // Unsupported version
  }

  // For PSBTv2, pre-create empty maps according to global counts
  if (out_psbt.version >= 2) {
    if (n_in > 0) {
      out_psbt.inputs.resize(n_in);
    }
    if (n_out > 0) {
      out_psbt.outputs.resize(n_out);
    }
  }

  // Read input maps
  size_t input_maps_read = 0;
  while (p < e) {
    if (out_psbt.version == 0 && input_maps_read >= n_in) {
      break; // We've read all expected inputs for v0
    }

    // Try to read an input map
    const uint8_t* save_p = p;
    std::vector<PsbtMapKV> input_kv;
    if (!read_map(p, e, input_kv)) {
      p = save_p; // Restore position
      break; // No more input maps
    }


    // For PSBTv2, use global input count to determine when to stop reading inputs
    if (out_psbt.version == 2) {
      auto input_count = get_global_input_count(out_psbt);
      if (input_count && input_maps_read >= *input_count) {
        p = save_p; // Restore position
        break; // Switch to output reading
      }
    }

    // Ensure we have space for this input
    if (out_psbt.inputs.size() <= input_maps_read) {
      out_psbt.inputs.resize(input_maps_read + 1);
    }
    out_psbt.inputs[input_maps_read].kv = std::move(input_kv);
    input_maps_read++;

    if (out_psbt.version == 0 && input_maps_read >= n_in) {
      break; // We've read all expected inputs for v0
    }
  }

  // Debug: Log how many input maps we read
  
  // For PSBTv2, if we didn't have global counts, use the number of maps we read
  if (out_psbt.version == 2 && !get_global_input_count(out_psbt)) {
    n_in = input_maps_read;
  }

  // Read output maps
  size_t output_maps_read = 0;
  while (p < e) {
    if (out_psbt.version == 0 && output_maps_read >= n_out) {
      break; // We've read all expected outputs for v0
    }
    
    // For PSBTv2, respect the global count
    if (out_psbt.version == 2 && output_maps_read >= n_out) {
      break; // We've read all expected outputs for v2
    }
    
    // Try to read an output map
    const uint8_t* save_p = p;
    std::vector<PsbtMapKV> output_kv;
    if (!read_map(p, e, output_kv)) {
      p = save_p; // Restore position
      break; // No more output maps
    }
    
    // For PSBTv2, populate pre-allocated outputs; for v0, append new ones
    if (out_psbt.version >= 2) {
      if (output_maps_read < out_psbt.outputs.size()) {
        out_psbt.outputs[output_maps_read].kv = std::move(output_kv);
      } else {
        out_psbt.outputs.emplace_back();
        out_psbt.outputs.back().kv = std::move(output_kv);
      }
    } else {
      out_psbt.outputs.emplace_back();
      out_psbt.outputs.back().kv = std::move(output_kv);
    }
    output_maps_read++;
    
    if (out_psbt.version == 0 && output_maps_read >= n_out) {
      break; // We've read all expected outputs for v0
    }
  }
  
  // For PSBTv2, if we didn't have global counts, use the number of maps we read
  if (out_psbt.version == 2 && !get_global_output_count(out_psbt)) {
    n_out = output_maps_read;
  }
  
  // Allow extra bytes at the end - return true if we successfully parsed the core structure
  // This is more tolerant of additional data that might be added by signing or other operations
  return true;
}

// ---------- Base64 ----------
static const char B64_TBL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string to_base64(const std::vector<uint8_t>& raw) {
  std::string out;
  out.reserve(((raw.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= raw.size()) {
    uint32_t n = (uint32_t(raw[i]) << 16) | (uint32_t(raw[i+1]) << 8) | uint32_t(raw[i+2]);
    out.push_back(B64_TBL[(n >> 18) & 63]);
    out.push_back(B64_TBL[(n >> 12) & 63]);
    out.push_back(B64_TBL[(n >> 6)  & 63]);
    out.push_back(B64_TBL[(n)       & 63]);
    i += 3;
  }
  if (i + 1 == raw.size()) {
    uint32_t n = (uint32_t(raw[i]) << 16);
    out.push_back(B64_TBL[(n >> 18) & 63]);
    out.push_back(B64_TBL[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == raw.size()) {
    uint32_t n = (uint32_t(raw[i]) << 16) | (uint32_t(raw[i+1]) << 8);
    out.push_back(B64_TBL[(n >> 18) & 63]);
    out.push_back(B64_TBL[(n >> 12) & 63]);
    out.push_back(B64_TBL[(n >> 6)  & 63]);
    out.push_back('=');
  }
  return out;
}

static inline int b64_index(char c) {
  if ('A' <= c && c <= 'Z') return c - 'A';
  if ('a' <= c && c <= 'z') return c - 'a' + 26;
  if ('0' <= c && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<uint8_t> from_base64(const std::string& b64) {
  std::vector<uint8_t> out;
  out.reserve((b64.size() / 4) * 3);
  uint32_t buf = 0;
  int bits = 0;
  for (char c : b64) {
    if (c == '=') break;
    int v = b64_index(c);
    if (v < 0) continue;
    buf = (buf << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t((buf >> bits) & 0xFF));
    }
  }
  return out;
}

// ---------- Field builders ----------
static void ensure_input(Psbt& psbt, size_t idx) {
  if (psbt.inputs.size() <= idx) psbt.inputs.resize(idx + 1);
}
static void ensure_output(Psbt& psbt, size_t idx) {
  if (psbt.outputs.size() <= idx) psbt.outputs.resize(idx + 1);
}

void add_global_unsigned_tx(Psbt& psbt, const std::vector<uint8_t>& tx_bytes) {
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtGlobal::UnsignedTx) };
  kv.value = tx_bytes;
  psbt.globals.push_back(std::move(kv));
}

void add_in_witness_utxo(Psbt& psbt, size_t idx, const std::vector<uint8_t>& scriptpk, uint64_t value) {
  ensure_input(psbt, idx);
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtIn::WitnessUtxo) };
  put_le64(kv.value, value);
  put_compact_size(kv.value, scriptpk.size());
  kv.value.insert(kv.value.end(), scriptpk.begin(), scriptpk.end());
  psbt.inputs[idx].kv.push_back(std::move(kv));
}

void add_in_nonwitness_utxo(Psbt& psbt, size_t idx, const std::vector<uint8_t>& full_tx) {
  ensure_input(psbt, idx);
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtIn::NonWitnessUtxo) };
  kv.value = full_tx;
  psbt.inputs[idx].kv.push_back(std::move(kv));
}

void add_in_bip32_deriv(Psbt& psbt, size_t idx, const std::vector<uint8_t>& pubkey33, const std::vector<uint8_t>& fp4, const std::vector<uint32_t>& path) {
  ensure_input(psbt, idx);
  PsbtMapKV kv;
  kv.key.reserve(1 + pubkey33.size());
  kv.key.push_back(static_cast<uint8_t>(PsbtIn::Bip32Deriv));
  kv.key.insert(kv.key.end(), pubkey33.begin(), pubkey33.end());
  kv.value.reserve(4 + 4 * path.size());
  kv.value.insert(kv.value.end(), fp4.begin(), fp4.end());
  for (uint32_t i : path) put_le32(kv.value, i);
  psbt.inputs[idx].kv.push_back(std::move(kv));
}

void add_in_sighash(Psbt& psbt, size_t idx, uint32_t sighash) {
  ensure_input(psbt, idx);
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtIn::SighashType) };
  put_le32(kv.value, sighash);
  psbt.inputs[idx].kv.push_back(std::move(kv));
}

void add_in_partial_sig(Psbt& psbt, size_t idx, const std::vector<uint8_t>& pubkey33, const std::vector<uint8_t>& signature) {
  ensure_input(psbt, idx);
  PsbtMapKV kv;
  kv.key.reserve(1 + pubkey33.size());
  kv.key.push_back(static_cast<uint8_t>(PsbtIn::PartialSig));
  kv.key.insert(kv.key.end(), pubkey33.begin(), pubkey33.end());
  kv.value = signature;
  psbt.inputs[idx].kv.push_back(std::move(kv));
}

void add_out_bip32_deriv(Psbt& psbt, size_t idx, const std::vector<uint8_t>& pubkey33, const std::vector<uint8_t>& fp4, const std::vector<uint32_t>& path) {
  ensure_output(psbt, idx);
  PsbtMapKV kv;
  kv.key.reserve(1 + pubkey33.size());
  kv.key.push_back(static_cast<uint8_t>(PsbtOut::Bip32Deriv));
  kv.key.insert(kv.key.end(), pubkey33.begin(), pubkey33.end());
  kv.value.reserve(4 + 4 * path.size());
  kv.value.insert(kv.value.end(), fp4.begin(), fp4.end());
  for (uint32_t i : path) put_le32(kv.value, i);
  psbt.outputs[idx].kv.push_back(std::move(kv));
}

// ---------- Field extractors ----------
std::optional<std::vector<uint8_t>> get_global_unsigned_tx(const Psbt& psbt) {
  for (const auto& kv : psbt.globals) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtGlobal::UnsignedTx)) {
      return kv.value;
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::vector<uint8_t>, uint64_t>> get_in_witness_utxo(const Psbt& psbt, size_t idx) {
  if (idx >= psbt.inputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.inputs[idx].kv) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtIn::WitnessUtxo)) {
      const auto decoded = din::psbt::DecodeWitnessUtxoValue(kv.value);
      if (!decoded.ok) return std::nullopt;
      return std::make_pair(decoded.script_pubkey, decoded.amount);
    }
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> get_in_nonwitness_utxo(const Psbt& psbt, size_t idx) {
  if (idx >= psbt.inputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.inputs[idx].kv) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtIn::NonWitnessUtxo)) {
      return kv.value;
    }
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> get_in_partial_sig(const Psbt& psbt, size_t idx, const std::vector<uint8_t>& pubkey33) {
  if (idx >= psbt.inputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.inputs[idx].kv) {
    if (kv.key.size() == 1 + pubkey33.size() && 
        kv.key[0] == static_cast<uint8_t>(PsbtIn::PartialSig) &&
        std::equal(kv.key.begin() + 1, kv.key.end(), pubkey33.begin())) {
      return kv.value;
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> get_in_sighash(const Psbt& psbt, size_t idx) {
  if (idx >= psbt.inputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.inputs[idx].kv) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtIn::SighashType)) {
      if (kv.value.size() != 4) return std::nullopt;
      
      const uint8_t* p = kv.value.data();
      const uint8_t* e = kv.value.data() + kv.value.size();
      
      uint32_t sighash = 0;
      if (!get_le32(p, e, sighash)) return std::nullopt;
      
      return sighash;
    }
  }
  return std::nullopt;
}

// ---------- Validation ----------
static bool Fail(std::string& err, const char* fmt, ...) {
  char buf[512];
  va_list ap; 
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  err.assign(buf);
  return false;
}

bool validate_psbt(const Psbt& psbt, std::string& error) {
  
  // Check PSBT version
  if (psbt.version == 0) {
    return Fail(error, "V0 not supported here; expected PSBTv2 (global.version=2)");
  }
  if (psbt.version != 2) {
    return Fail(error, "Unsupported PSBT version: %d (expected 2)", psbt.version);
  }
  
  // PSBTv2 validation
  {
    // PSBTv2 uses input/output maps and global counts
    auto input_count = get_global_input_count(psbt);
    auto output_count = get_global_output_count(psbt);
    
    if (!input_count) {
      return Fail(error, "Missing global_input_count");
    }
    
    // Allow 0 inputs for unfunded PSBTs (they will be funded later)
    if (*input_count == 0 && psbt.inputs.empty()) {
      // This is a valid unfunded PSBT
    } else if (*input_count != psbt.inputs.size()) {
      return Fail(error, "Input count mismatch: PSBT has %zu inputs, global count is %u", 
                  psbt.inputs.size(), *input_count);
    }
    
    if (!output_count) {
      return Fail(error, "Missing global_output_count");
    }

    if (*output_count != psbt.outputs.size()) {
      return Fail(error, "Output count mismatch: PSBT has %zu outputs, global count is %u",
                  psbt.outputs.size(), *output_count);
    }
    
    // Check that each input has required fields
    for (size_t i = 0; i < psbt.inputs.size(); ++i) {
      const auto txid = get_in_prev_txid(psbt, i);
      if (!txid || txid->empty()) {
        return Fail(error, "input[%zu]: missing prev_txid (0x0e)", i);
      }
      
      const auto output_index = get_in_output_index(psbt, i);
      if (!output_index) {
        return Fail(error, "input[%zu]: missing output_index (0x0f)", i);
      }
      
      if (*output_index > 0xFFFF'FFFFu) {
        return Fail(error, "input[%zu]: output_index out of range (%u)", i, *output_index);
      }
      
      // Funding info for segwit path
      const auto wutxo = get_in_witness_utxo(psbt, i);
      const auto nutxo = get_in_nonwitness_utxo(psbt, i);
      if (!wutxo.has_value() && !nutxo.has_value()) {
        return Fail(error, "input[%zu]: neither witness_utxo nor non_witness_utxo present", i);
      }
    }
    
    // Check that each output has required fields
    for (size_t i = 0; i < psbt.outputs.size(); ++i) {
      const auto& output = psbt.outputs[i];

      if (output.kv.empty()) {
        return Fail(error, "output[%zu]: empty output map", i);
      }
      
      const auto amount = get_out_amount(psbt, i);
      if (!amount) {
        return Fail(error, "output[%zu]: missing AMOUNT", i);
      }
      
      const auto script = get_out_script(psbt, i);
      if (!script || script->empty()) {
        return Fail(error, "output[%zu]: missing SCRIPT", i);
      }
    }
  }
  
  return true;
}

Json::Value per_input_missing_report(const Psbt& psbt) {
  Json::Value arr(Json::arrayValue);
  const size_t N = psbt.inputs.size();
  
  for (size_t i = 0; i < N; ++i) {
    Json::Value m(Json::objectValue);
    
    // Check PSBTv2 tuple fields
    m["has_prev_txid"] = get_in_prev_txid(psbt, i).has_value() && !get_in_prev_txid(psbt, i)->empty();
    m["has_output_index"] = get_in_output_index(psbt, i).has_value();
    m["has_sequence"] = get_in_sequence(psbt, i).has_value();
    
    // Check UTXO data
    m["has_witness_utxo"] = get_in_witness_utxo(psbt, i).has_value();
    m["has_non_witness_utxo"] = get_in_nonwitness_utxo(psbt, i).has_value();
    
    // Check signatures
    // Note: get_in_partial_sig requires pubkey33 parameter, so we'll check differently
    bool has_partial_sigs = false;
    for (const auto& kv : psbt.inputs[i].kv) {
      if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtIn::PartialSig)) {
        has_partial_sigs = true;
        break;
      }
    }
    m["has_partial_sigs"] = has_partial_sigs;
    
    // Check final scripts
    bool has_final_scriptsig = false;
    bool has_final_scriptwitness = false;
    for (const auto& kv : psbt.inputs[i].kv) {
      if (!kv.key.empty()) {
        if (kv.key[0] == static_cast<uint8_t>(PsbtIn::FinalScriptSig)) {
          has_final_scriptsig = true;
        } else if (kv.key[0] == static_cast<uint8_t>(PsbtIn::FinalScriptWit)) {
          has_final_scriptwitness = true;
        }
      }
    }
    m["has_final_scriptsig"] = has_final_scriptsig;
    m["has_final_scriptwitness"] = has_final_scriptwitness;
    
    // Sighash type
    const auto sighash = get_in_sighash(psbt, i);
    m["sighash_type"] = sighash.has_value() ? Json::UInt(*sighash) : Json::nullValue;
    
    arr.append(m);
  }
  
  return arr;
}

bool is_psbt_complete(const Psbt& psbt) {
  
  for (size_t i = 0; i < psbt.inputs.size(); ++i) {
    const auto& input = psbt.inputs[i];
    bool has_final_scriptsig = false;
    bool has_final_scriptwitness = false;
    
    
    for (const auto& kv : input.kv) {
      if (!kv.key.empty()) {
        if (kv.key[0] == static_cast<uint8_t>(PsbtIn::FinalScriptSig)) {
          has_final_scriptsig = true;
        } else if (kv.key[0] == static_cast<uint8_t>(PsbtIn::FinalScriptWit)) {
          has_final_scriptwitness = true;
        }
      }
    }
    
    if (!has_final_scriptsig && !has_final_scriptwitness) {
      return false;
    }
  }
  
  return true;
}

std::optional<uint32_t> get_global_locktime(const Psbt& psbt) {
  for (const auto& kv : psbt.globals) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtGlobal::FallbackLocktime)) {
      if (kv.value.size() == 4) {
        const uint8_t* p = kv.value.data();
        const uint8_t* e = kv.value.data() + kv.value.size();
        uint32_t locktime = 0;
        if (get_le32(p, e, locktime)) {
          return locktime;
        }
      }
    }
  }
  return std::nullopt;
}

std::vector<uint8_t> extract_transaction(const Psbt& psbt) {
  
  // Handle PSBTv0 vs PSBTv2 differently
  if (psbt.version == 0) {
    // PSBTv0: return the global unsigned transaction
    auto unsigned_tx = get_global_unsigned_tx(psbt);
    if (unsigned_tx) {
      return *unsigned_tx;
    }
    return {};
  } else if (psbt.version == 2) {
    // PSBTv2: reconstruct transaction from input/output maps
    std::vector<uint8_t> tx;
    
    // Get transaction version
    auto tx_version = get_global_tx_version(psbt);
    uint32_t version = tx_version ? *tx_version : 2; // Default to version 2
    
    // Write version (little-endian)
    tx.push_back(version & 0xFF);
    tx.push_back((version >> 8) & 0xFF);
    tx.push_back((version >> 16) & 0xFF);
    tx.push_back((version >> 24) & 0xFF);
    
    // Write input count
    put_compact_size(tx, psbt.inputs.size());
    
    // Write inputs
    for (size_t i = 0; i < psbt.inputs.size(); ++i) {
      // Get previous txid and output index
      auto prev_txid = get_in_prev_txid(psbt, i);
      auto output_index = get_in_output_index(psbt, i);
      auto sequence = get_in_sequence(psbt, i);
      
      if (!prev_txid || !output_index) {
        return {}; // Missing required input data
      }
      
      // Write previous txid (32 bytes)
      tx.insert(tx.end(), prev_txid->begin(), prev_txid->end());
      
      // Write output index (4 bytes, little-endian)
      tx.push_back(*output_index & 0xFF);
      tx.push_back((*output_index >> 8) & 0xFF);
      tx.push_back((*output_index >> 16) & 0xFF);
      tx.push_back((*output_index >> 24) & 0xFF);
      
      // Write script length (0 for finalized inputs)
      tx.push_back(0x00);
      
      // Write sequence (4 bytes, little-endian)
      uint32_t seq = sequence ? *sequence : 0xFFFFFFFF;
      tx.push_back(seq & 0xFF);
      tx.push_back((seq >> 8) & 0xFF);
      tx.push_back((seq >> 16) & 0xFF);
      tx.push_back((seq >> 24) & 0xFF);
    }
    
    // Count valid outputs first
    size_t valid_output_count = 0;
    for (size_t i = 0; i < psbt.outputs.size(); ++i) {
      auto amount = get_out_amount(psbt, i);
      auto script = get_out_script(psbt, i);
      if (amount && script) {
        valid_output_count++;
      }
    }
    
    if (valid_output_count == 0) {
      return {}; // No valid outputs
    }
    
    // Write output count (only valid outputs)
    put_compact_size(tx, valid_output_count);
    
    // Write outputs - only process outputs that have data
    for (size_t i = 0; i < psbt.outputs.size(); ++i) {
      // Get amount and script
      auto amount = get_out_amount(psbt, i);
      auto script = get_out_script(psbt, i);
      
      if (!amount || !script) {
        continue; // Skip empty outputs
      }
      
      // Write amount (8 bytes, little-endian)
      for (int j = 0; j < 8; ++j) {
        tx.push_back((*amount >> (j * 8)) & 0xFF);
      }
      
      // Write script length
      put_compact_size(tx, script->size());
      
      // Write script
      tx.insert(tx.end(), script->begin(), script->end());
    }
    
    // Write locktime (4 bytes, little-endian)
    auto locktime = get_global_locktime(psbt);
    uint32_t lt = locktime ? *locktime : 0;
    tx.push_back(lt & 0xFF);
    tx.push_back((lt >> 8) & 0xFF);
    tx.push_back((lt >> 16) & 0xFF);
    tx.push_back((lt >> 24) & 0xFF);
    
    return tx;
  }
  
  return {};
}

// ---------- PSBTv2 Field Builders ----------
void add_global_version(Psbt& psbt, uint8_t version) {
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtGlobal::Version) };
  kv.value = { version };
  psbt.globals.push_back(std::move(kv));
  psbt.version = version;
}

void add_global_tx_version(Psbt& psbt, uint32_t tx_version) {
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtGlobal::TxVersion) };
  put_le32(kv.value, tx_version);
  psbt.globals.push_back(std::move(kv));
}

void add_global_input_count(Psbt& psbt, uint64_t count) {
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtGlobal::InputCount) };
  put_compact_size(kv.value, count);
  psbt.globals.push_back(std::move(kv));
}

void add_global_output_count(Psbt& psbt, uint64_t count) {
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtGlobal::OutputCount) };
  put_compact_size(kv.value, count);
  psbt.globals.push_back(std::move(kv));
}

void add_in_prev_txid(Psbt& psbt, size_t idx, const std::vector<uint8_t>& txid) {
  ensure_input(psbt, idx);
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtIn::PrevTxId) };
  kv.value = txid;
  psbt.inputs[idx].kv.push_back(std::move(kv));
}

void add_in_output_index(Psbt& psbt, size_t idx, uint32_t vout) {
  ensure_input(psbt, idx);
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtIn::OutputIndex) };
  put_le32(kv.value, vout);
  psbt.inputs[idx].kv.push_back(std::move(kv));
}

void add_in_sequence(Psbt& psbt, size_t idx, uint32_t sequence) {
  ensure_input(psbt, idx);
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtIn::Sequence) };
  put_le32(kv.value, sequence);
  psbt.inputs[idx].kv.push_back(std::move(kv));
}

void add_out_amount(Psbt& psbt, size_t idx, uint64_t amount) {
  ensure_output(psbt, idx);
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtOut::Amount) };
  put_le64(kv.value, amount);
  psbt.outputs[idx].kv.push_back(std::move(kv));
}

void add_out_script(Psbt& psbt, size_t idx, const std::vector<uint8_t>& script) {
  ensure_output(psbt, idx);
  PsbtMapKV kv;
  kv.key = { static_cast<uint8_t>(PsbtOut::Script) };
  kv.value = script;
  psbt.outputs[idx].kv.push_back(std::move(kv));
}

// ---------- PSBTv2 Field Extractors ----------
std::optional<uint8_t> get_global_version(const Psbt& psbt) {
  for (const auto& kv : psbt.globals) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtGlobal::Version)) {
      if (kv.value.size() == 1) {
        return kv.value[0];
      }
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> get_global_tx_version(const Psbt& psbt) {
  for (const auto& kv : psbt.globals) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtGlobal::TxVersion)) {
      if (kv.value.size() == 4) {
        const uint8_t* p = kv.value.data();
        const uint8_t* e = kv.value.data() + kv.value.size();
        uint32_t version = 0;
        if (get_le32(p, e, version)) {
          return version;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<uint64_t> get_global_input_count(const Psbt& psbt) {
  for (const auto& kv : psbt.globals) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtGlobal::InputCount)) {
      const uint8_t* p = kv.value.data();
      const uint8_t* e = kv.value.data() + kv.value.size();
      uint64_t count = 0;
      if (get_compact_size(p, e, count)) {
        return count;
      }
    }
  }
  return std::nullopt;
}

std::optional<uint64_t> get_global_output_count(const Psbt& psbt) {
  for (const auto& kv : psbt.globals) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtGlobal::OutputCount)) {
      const uint8_t* p = kv.value.data();
      const uint8_t* e = kv.value.data() + kv.value.size();
      uint64_t count = 0;
      if (get_compact_size(p, e, count)) {
        return count;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> get_in_prev_txid(const Psbt& psbt, size_t idx) {
  if (idx >= psbt.inputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.inputs[idx].kv) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtIn::PrevTxId)) {
      return kv.value;
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> get_in_output_index(const Psbt& psbt, size_t idx) {
  if (idx >= psbt.inputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.inputs[idx].kv) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtIn::OutputIndex)) {
      if (kv.value.size() == 4) {
        const uint8_t* p = kv.value.data();
        const uint8_t* e = kv.value.data() + kv.value.size();
        uint32_t vout = 0;
        if (get_le32(p, e, vout)) {
          return vout;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> get_in_sequence(const Psbt& psbt, size_t idx) {
  if (idx >= psbt.inputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.inputs[idx].kv) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtIn::Sequence)) {
      if (kv.value.size() == 4) {
        const uint8_t* p = kv.value.data();
        const uint8_t* e = kv.value.data() + kv.value.size();
        uint32_t sequence = 0;
        if (get_le32(p, e, sequence)) {
          return sequence;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<uint64_t> get_out_amount(const Psbt& psbt, size_t idx) {
  if (idx >= psbt.outputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.outputs[idx].kv) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtOut::Amount)) {
      if (kv.value.size() == 8) {
        const uint8_t* p = kv.value.data();
        const uint8_t* e = kv.value.data() + kv.value.size();
        uint64_t amount = 0;
        if (get_le64(p, e, amount)) {
          return amount;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> get_out_script(const Psbt& psbt, size_t idx) {
  if (idx >= psbt.outputs.size()) return std::nullopt;
  
  for (const auto& kv : psbt.outputs[idx].kv) {
    if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtOut::Script)) {
      return kv.value;
    }
  }
  return std::nullopt;
}

// ---------- Utility Functions ----------
std::string to_hex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : data) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> result;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        result.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
    return result;
}

// ---------- PSBT Analysis and Combination ----------
PsbtAnalysis analyze_psbt(const Psbt& psbt) {
  PsbtAnalysis analysis;
  analysis.is_final = false;
  analysis.is_complete = false;
  analysis.estimated_vsize = 0;
  analysis.estimated_fee = 0;
  
  // Check for PSBT version
  auto version = get_global_version(psbt);
  if (!version) {
    analysis.warnings.push_back("PSBT version not specified, assuming v0");
  } else if (*version > 2) {
    analysis.errors.push_back("Unsupported PSBT version: " + std::to_string(*version));
  }
  
  // Handle PSBTv0 vs PSBTv2 differently
  uint64_t n_in = 0, n_out = 0;
  if (psbt.version == 0) {
    // PSBTv0 requires global unsigned transaction
    auto unsigned_tx = get_global_unsigned_tx(psbt);
    if (!unsigned_tx) {
      analysis.errors.push_back("Missing required global unsigned transaction");
      analysis.missing.push_back("unsigned_tx");
      return analysis; // Can't analyze further without unsigned tx
    }
    
    // Parse transaction to get input/output counts
    if (!parse_unsigned_tx_counts(*unsigned_tx, n_in, n_out)) {
      analysis.errors.push_back("Invalid unsigned transaction format");
      return analysis;
    }
    
    // Validate input/output counts match
    if (psbt.inputs.size() != n_in) {
      analysis.errors.push_back("Input count mismatch: PSBT has " + std::to_string(psbt.inputs.size()) + 
                               " inputs, transaction has " + std::to_string(n_in));
    }
    if (psbt.outputs.size() != n_out) {
      analysis.errors.push_back("Output count mismatch: PSBT has " + std::to_string(psbt.outputs.size()) + 
                               " outputs, transaction has " + std::to_string(n_out));
    }
  } else if (psbt.version == 2) {
    // PSBTv2: Check for required global fields
    auto tx_version = get_global_tx_version(psbt);
    if (!tx_version) {
      analysis.errors.push_back("Missing required TX_VERSION in PSBTv2");
      analysis.missing.push_back("tx_version");
      return analysis;
    }
    
    // Derive input/output counts from maps (preferred over global counts)
    n_in = psbt.inputs.size();
    n_out = psbt.outputs.size();
    
    // Validate global counts if present
    auto input_count = get_global_input_count(psbt);
    auto output_count = get_global_output_count(psbt);
    
    if (input_count && *input_count != n_in) {
      analysis.warnings.push_back("Global input count (" + std::to_string(*input_count) + 
                                 ") doesn't match actual input maps (" + std::to_string(n_in) + ")");
    }
    if (output_count && *output_count != n_out) {
      analysis.warnings.push_back("Global output count (" + std::to_string(*output_count) + 
                                 ") doesn't match actual output maps (" + std::to_string(n_out) + ")");
    }
    
    // Check for v0/v2 mixing (should not have global unsigned tx in v2)
    auto unsigned_tx = get_global_unsigned_tx(psbt);
    if (unsigned_tx) {
      analysis.errors.push_back("PSBTv2 should not contain global unsigned transaction");
      return analysis;
    }
  }
  
  // Analyze inputs and estimate vsize
  size_t total_vsize = 10; // Base transaction overhead
  bool needs_funding = false;
  bool needs_signing = false;
  bool needs_finalization = false;
  
  for (size_t i = 0; i < psbt.inputs.size(); ++i) {
    // PSBTv2: Check for required input fields
    if (psbt.version == 2) {
      auto prev_txid = get_in_prev_txid(psbt, i);
      auto output_index = get_in_output_index(psbt, i);
      
      if (!prev_txid) {
        analysis.errors.push_back("Input " + std::to_string(i) + " missing PREVIOUS_TXID");
        analysis.missing.push_back("prev_txid for input " + std::to_string(i));
      }
      if (!output_index) {
        analysis.errors.push_back("Input " + std::to_string(i) + " missing OUTPUT_INDEX");
        analysis.missing.push_back("output_index for input " + std::to_string(i));
      }
    }
    
    bool has_witness_utxo = get_in_witness_utxo(psbt, i).has_value();
    bool has_non_witness_utxo = get_in_nonwitness_utxo(psbt, i).has_value();
    bool has_final_scriptsig = false;
    bool has_final_scriptwitness = false;
    bool has_partial_sig = false;
    bool has_bip32_deriv = false;
    
    // Check for UTXO data
    if (!has_witness_utxo && !has_non_witness_utxo) {
      analysis.missing.push_back("utxo");
      needs_funding = true;
    } else {
      // PSBTv2: Must have exactly one of witness_utxo or non_witness_utxo
      if (psbt.version == 2 && has_witness_utxo && has_non_witness_utxo) {
        analysis.errors.push_back("Input " + std::to_string(i) + " has both witness_utxo and non_witness_utxo (must have exactly one)");
      }
      
      // Estimate input vsize based on UTXO type
      if (has_witness_utxo) {
        total_vsize += 68; // P2WPKH input: 32 (prevout) + 4 (sequence) + 32 (witness)
      } else {
        total_vsize += 148; // P2PKH input: 32 (prevout) + 4 (sequence) + 112 (scriptsig)
      }
    }
    
    // Check for key derivation
    for (const auto& kv : psbt.inputs[i].kv) {
      if (!kv.key.empty()) {
        if (kv.key[0] == static_cast<uint8_t>(PsbtIn::Bip32Deriv)) {
          has_bip32_deriv = true;
        } else if (kv.key[0] == static_cast<uint8_t>(PsbtIn::PartialSig)) {
          has_partial_sig = true;
        } else if (kv.key[0] == static_cast<uint8_t>(PsbtIn::FinalScriptSig)) {
          has_final_scriptsig = true;
        } else if (kv.key[0] == static_cast<uint8_t>(PsbtIn::FinalScriptWit)) {
          has_final_scriptwitness = true;
        }
      }
    }
    
    if (!has_bip32_deriv) {
      analysis.warnings.push_back("Input " + std::to_string(i) + " missing key derivation info");
    }
    
    if (!has_partial_sig) {
      needs_signing = true;
    }
    
    if (!has_final_scriptsig && !has_final_scriptwitness) {
      needs_finalization = true;
    }
  }
  
  // Analyze outputs and estimate vsize
  for (size_t i = 0; i < psbt.outputs.size(); ++i) {
    // PSBTv2: Check for required output fields
    if (psbt.version == 2) {
      auto amount = get_out_amount(psbt, i);
      auto script = get_out_script(psbt, i);
      
      if (!amount) {
        analysis.errors.push_back("Output " + std::to_string(i) + " missing AMOUNT");
        analysis.missing.push_back("amount for output " + std::to_string(i));
      }
      if (!script) {
        analysis.errors.push_back("Output " + std::to_string(i) + " missing SCRIPT");
        analysis.missing.push_back("script for output " + std::to_string(i));
      }
    }
    
    // Estimate output vsize (assume P2WPKH for now)
    total_vsize += 31; // P2WPKH output: 8 (value) + 1 (script length) + 22 (script)
    
    // Check for key derivation on outputs
    bool has_bip32_deriv = false;
    for (const auto& kv : psbt.outputs[i].kv) {
      if (!kv.key.empty() && kv.key[0] == static_cast<uint8_t>(PsbtOut::Bip32Deriv)) {
        has_bip32_deriv = true;
        break;
      }
    }
    
    if (!has_bip32_deriv) {
      analysis.warnings.push_back("Output " + std::to_string(i) + " missing key derivation info");
    }
  }
  
  analysis.estimated_vsize = total_vsize;
  analysis.estimated_fee = total_vsize * 1; // Assume 1 sat/vB fee rate
  
  // Determine next steps
  if (needs_funding) {
    analysis.next_steps.push_back("fund");
  }
  if (needs_signing) {
    analysis.next_steps.push_back("sign");
  }
  if (needs_finalization) {
    analysis.next_steps.push_back("finalize");
  }
  
  // Check completion and finalization
  analysis.is_complete = is_psbt_complete(psbt);
  if (analysis.is_complete) {
    analysis.next_steps.push_back("submit");
    analysis.is_final = true;
  }
  
  return analysis;
}

bool combine_psbt(Psbt& target, const Psbt& source) {
  // Combine global fields
  for (const auto& source_kv : source.globals) {
    bool found = false;
    for (const auto& target_kv : target.globals) {
      if (target_kv.key == source_kv.key) {
        found = true;
        break;
      }
    }
    if (!found) {
      target.globals.push_back(source_kv);
    }
  }
  
  // Ensure input/output counts match
  if (target.inputs.size() != source.inputs.size()) {
    return false;
  }
  if (target.outputs.size() != source.outputs.size()) {
    return false;
  }
  
  // Combine input fields
  for (size_t i = 0; i < target.inputs.size(); ++i) {
    for (const auto& source_kv : source.inputs[i].kv) {
      bool found = false;
      for (const auto& target_kv : target.inputs[i].kv) {
        if (target_kv.key == source_kv.key) {
          found = true;
          break;
        }
      }
      if (!found) {
        target.inputs[i].kv.push_back(source_kv);
      }
    }
  }
  
  // Combine output fields
  for (size_t i = 0; i < target.outputs.size(); ++i) {
    for (const auto& source_kv : source.outputs[i].kv) {
      bool found = false;
      for (const auto& target_kv : target.outputs[i].kv) {
        if (target_kv.key == source_kv.key) {
          found = true;
          break;
        }
      }
      if (!found) {
        target.outputs[i].kv.push_back(source_kv);
      }
    }
  }
  
  return true;
}

} // namespace din
