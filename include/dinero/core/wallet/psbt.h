#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "compat/jsoncpp_compat.h"

// JsonCPP helpers for cleaner array/object construction
inline Json::Value JArr() { return Json::Value(Json::arrayValue); }
inline Json::Value JObj() { return Json::Value(Json::objectValue); }

namespace din {

// PSBT types (v0 and v2 compatible)
enum class PsbtGlobal : uint8_t { 
  UnsignedTx = 0x00, 
  Xpub = 0x01,
  // PSBTv2 fields
  TxVersion = 0x02,
  FallbackLocktime = 0x03,
  InputCount = 0x04,
  OutputCount = 0x05,
  TxModifiable = 0x06,
  Version = 0xfb  // PSBT version
};
enum class PsbtIn    : uint8_t {
  NonWitnessUtxo = 0x00,
  WitnessUtxo    = 0x01,
  PartialSig     = 0x02,
  SighashType    = 0x03,
  Bip32Deriv     = 0x06,
  FinalScriptSig = 0x07,
  FinalScriptWit = 0x08,
  // PSBTv2 fields
  PrevTxId = 0x0e,
  OutputIndex = 0x0f,
  Sequence = 0x10,
  RequiredTimeLocktime = 0x11,
  RequiredHeightLocktime = 0x12,
  // Taproot fields (BIP-371)
  TapInternalKey = 0x16,
  TapMerkleRoot  = 0x17,
  TapLeafScript  = 0x18,
  TapBip32Derivation = 0x19
};
enum class PsbtOut   : uint8_t { 
  Bip32Deriv = 0x02,
  // PSBTv2 fields
  Amount = 0x03,
  Script = 0x04,
  TapInternalKey = 0x05,
  TapTree = 0x06,
  TapBip32Derivation = 0x07
};

struct PsbtMapKV {
  std::vector<uint8_t> key;   // includes type byte + keydata
  std::vector<uint8_t> value; // raw
  
  PsbtMapKV() = default;
  PsbtMapKV(std::vector<uint8_t> k, std::vector<uint8_t> v) 
    : key(std::move(k)), value(std::move(v)) {}
};

struct PsbtInput  { 
  std::vector<PsbtMapKV> kv; 
  
  PsbtInput() = default;
};

struct PsbtOutput { 
  std::vector<PsbtMapKV> kv; 
  
  PsbtOutput() = default;
};

struct Psbt {
  std::vector<PsbtMapKV> globals;
  std::vector<PsbtInput> inputs;
  std::vector<PsbtOutput> outputs;
  
  // PSBT version (0 for v0, 2 for v2)
  uint8_t version = 0;
  
  Psbt() = default;
};

/**
 * @brief PSBT serialization and parsing
 */
std::vector<uint8_t> serialize(const Psbt& psbt);     // raw bytes
bool                 deserialize(const std::vector<uint8_t>& raw, Psbt& out);
std::string          to_base64(const std::vector<uint8_t>& raw);
std::vector<uint8_t> from_base64(const std::string& b64);

/**
 * @brief Helpers to build PSBT fields (typed K/V insertion)
 */
void add_global_unsigned_tx(Psbt& psbt, const std::vector<uint8_t>& tx_bytes);
void add_in_witness_utxo(Psbt& psbt, size_t idx, const std::vector<uint8_t>& scriptpk, uint64_t value);
void add_in_nonwitness_utxo(Psbt& psbt, size_t idx, const std::vector<uint8_t>& full_tx);
void add_in_bip32_deriv(Psbt& psbt, size_t idx, const std::vector<uint8_t>& pubkey33, 
                        const std::vector<uint8_t>& fp4, const std::vector<uint32_t>& path);
void add_in_sighash(Psbt& psbt, size_t idx, uint32_t sighash);
void add_in_partial_sig(Psbt& psbt, size_t idx, const std::vector<uint8_t>& pubkey33, 
                        const std::vector<uint8_t>& signature);
void add_out_bip32_deriv(Psbt& psbt, size_t idx, const std::vector<uint8_t>& pubkey33, 
                         const std::vector<uint8_t>& fp4, const std::vector<uint32_t>& path);

// PSBTv2 helpers
void add_global_version(Psbt& psbt, uint8_t version);
void add_global_tx_version(Psbt& psbt, uint32_t tx_version);
void add_global_input_count(Psbt& psbt, uint64_t count);
void add_global_output_count(Psbt& psbt, uint64_t count);
void add_in_prev_txid(Psbt& psbt, size_t idx, const std::vector<uint8_t>& txid);
void add_in_output_index(Psbt& psbt, size_t idx, uint32_t vout);
void add_in_sequence(Psbt& psbt, size_t idx, uint32_t sequence);
void add_out_amount(Psbt& psbt, size_t idx, uint64_t amount);
void add_out_script(Psbt& psbt, size_t idx, const std::vector<uint8_t>& script);

/**
 * @brief PSBT field extraction helpers
 */
std::optional<std::vector<uint8_t>> get_global_unsigned_tx(const Psbt& psbt);
std::optional<std::pair<std::vector<uint8_t>, uint64_t>> get_in_witness_utxo(const Psbt& psbt, size_t idx);
std::optional<std::vector<uint8_t>> get_in_nonwitness_utxo(const Psbt& psbt, size_t idx);
std::optional<std::vector<uint8_t>> get_in_partial_sig(const Psbt& psbt, size_t idx, 
                                                       const std::vector<uint8_t>& pubkey33);
std::optional<uint32_t> get_in_sighash(const Psbt& psbt, size_t idx);

// PSBTv2 field extractors
std::optional<uint8_t> get_global_version(const Psbt& psbt);
std::optional<uint32_t> get_global_tx_version(const Psbt& psbt);
std::optional<uint32_t> get_global_locktime(const Psbt& psbt);
std::optional<uint64_t> get_global_input_count(const Psbt& psbt);
std::optional<uint64_t> get_global_output_count(const Psbt& psbt);
std::optional<std::vector<uint8_t>> get_in_prev_txid(const Psbt& psbt, size_t idx);
std::optional<uint32_t> get_in_output_index(const Psbt& psbt, size_t idx);
std::optional<uint32_t> get_in_sequence(const Psbt& psbt, size_t idx);
std::optional<uint64_t> get_out_amount(const Psbt& psbt, size_t idx);
std::optional<std::vector<uint8_t>> get_out_script(const Psbt& psbt, size_t idx);

// Utility functions
std::string to_hex(const std::vector<uint8_t>& data);
std::vector<uint8_t> from_hex(const std::string& hex);

/**
 * @brief PSBT validation and completion checking
 */
bool validate_psbt(const Psbt& psbt, std::string& error);
bool is_psbt_complete(const Psbt& psbt);
std::vector<uint8_t> extract_transaction(const Psbt& psbt); // Only if complete

// Debugging helpers
::Json::Value per_input_missing_report(const Psbt& psbt);

// PSBT analysis and combination
struct PsbtAnalysis {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  std::vector<std::string> next_steps;
  bool is_final;
  bool is_complete;
  size_t estimated_vsize;
  uint64_t estimated_fee;
  std::vector<std::string> missing;
  std::vector<std::string> unknown;
};

PsbtAnalysis analyze_psbt(const Psbt& psbt);
bool combine_psbt(Psbt& target, const Psbt& source);

/**
 * @brief PSBT signing interface
 */
class IPsbtSigner {
public:
    virtual ~IPsbtSigner() = default;
    
    /**
     * @brief Sign PSBT inputs with available keys
     * 
     * @param psbt PSBT to sign (modified in place)
     * @return Number of inputs successfully signed
     */
    virtual size_t signPsbt(Psbt& psbt) = 0;
    
    /**
     * @brief Check if signer has key for given pubkey
     */
    virtual bool hasKey(const std::vector<uint8_t>& pubkey33) const = 0;
};

} // namespace din