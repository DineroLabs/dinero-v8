// Copyright (c) 2026 The Dinero developers
// Distributed under the MIT software license.
//
// Offline CCV reachability audit for archival blk*.dat files.
//
// This tool deliberately uses the node's canonical Block::Deserialize path.
// It detects OP_CHECKCONTRACTVERIFY only when 0xbe is an opcode in a revealed
// Taproot script-path leaf; bytes inside data pushes are not matches.

#include "primitives/block.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint8_t OP_PUSHDATA1 = 0x4c;
constexpr uint8_t OP_PUSHDATA2 = 0x4d;
constexpr uint8_t OP_PUSHDATA4 = 0x4e;
constexpr uint8_t OP_CHECKCONTRACTVERIFY = 0xbe;
constexpr uint8_t TAPROOT_ANNEX_TAG = 0x50;
constexpr uint8_t TAPSCRIPT_LEAF_VERSION = 0xc0;
constexpr size_t TAPROOT_CONTROL_BASE_SIZE = 33;
constexpr size_t TAPROOT_CONTROL_NODE_SIZE = 32;
constexpr size_t TAPROOT_CONTROL_MAX_NODES = 128;

struct ScriptScan {
  bool contains_ccv_opcode{false};
  bool malformed{false};
};

struct RevealedLeaf {
  size_t script_index{0};
  size_t control_index{0};
  bool has_annex{false};
};

struct Match {
  std::string file;
  uint64_t offset{0};
  std::string block_hash;
  std::string txid;
  size_t tx_index{0};
  size_t input_index{0};
};

struct Counters {
  uint64_t files{0};
  uint64_t bytes_at_scan_start{0};
  uint64_t records{0};
  uint64_t checksum_verified_records{0};
  uint64_t transactions{0};
  uint64_t inputs{0};
  uint64_t outputs{0};
  uint64_t p2tr_outputs{0};
  uint64_t revealed_tapscript_spends{0};
  uint64_t revealed_tapscript_spends_with_annex{0};
  uint64_t malformed_revealed_scripts{0};
  uint64_t ccv_byte_but_not_opcode{0};
};

struct Options {
  std::filesystem::path blocks_dir;
  uint32_t expected_magic{0};
  std::string expected_tip;
  bool self_test{false};
};

uint32_t Fnv1aChecksum(const uint8_t *data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

std::string LowerHex(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string JsonEscape(const std::string &value) {
  std::ostringstream out;
  for (const unsigned char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (c < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<unsigned>(c) << std::dec;
      } else {
        out << c;
      }
    }
  }
  return out.str();
}

bool ReadPushLength(const std::vector<uint8_t> &script, size_t &cursor,
                    uint8_t opcode, uint64_t &push_length) {
  if (opcode <= 0x4b) {
    push_length = opcode;
    return true;
  }

  size_t length_bytes = 0;
  if (opcode == OP_PUSHDATA1) {
    length_bytes = 1;
  } else if (opcode == OP_PUSHDATA2) {
    length_bytes = 2;
  } else if (opcode == OP_PUSHDATA4) {
    length_bytes = 4;
  } else {
    return false;
  }

  if (cursor > script.size() || script.size() - cursor < length_bytes) {
    return false;
  }

  push_length = 0;
  for (size_t i = 0; i < length_bytes; ++i) {
    push_length |= static_cast<uint64_t>(script[cursor + i]) << (8 * i);
  }
  cursor += length_bytes;
  return true;
}

ScriptScan ScanScript(const std::vector<uint8_t> &script) {
  ScriptScan result;
  size_t cursor = 0;
  while (cursor < script.size()) {
    const uint8_t opcode = script[cursor++];
    if (opcode == OP_CHECKCONTRACTVERIFY) {
      result.contains_ccv_opcode = true;
      continue;
    }

    uint64_t push_length = 0;
    if (opcode <= OP_PUSHDATA4) {
      if (!ReadPushLength(script, cursor, opcode, push_length) ||
          push_length > std::numeric_limits<size_t>::max() ||
          cursor > script.size() ||
          static_cast<size_t>(push_length) > script.size() - cursor) {
        result.malformed = true;
        return result;
      }
      cursor += static_cast<size_t>(push_length);
    }
  }
  return result;
}

bool IsTaprootControlBlock(const std::vector<uint8_t> &control) {
  if (control.size() < TAPROOT_CONTROL_BASE_SIZE ||
      control.size() >
          TAPROOT_CONTROL_BASE_SIZE +
              TAPROOT_CONTROL_NODE_SIZE * TAPROOT_CONTROL_MAX_NODES ||
      (control.size() - TAPROOT_CONTROL_BASE_SIZE) %
              TAPROOT_CONTROL_NODE_SIZE !=
          0) {
    return false;
  }
  return (control[0] & 0xfe) == TAPSCRIPT_LEAF_VERSION;
}

std::optional<RevealedLeaf>
FindRevealedLeaf(const std::vector<std::vector<uint8_t>> &witness) {
  if (witness.size() < 2) {
    return std::nullopt;
  }

  size_t control_index = witness.size() - 1;
  bool has_annex = false;
  if (!witness.back().empty() && witness.back()[0] == TAPROOT_ANNEX_TAG) {
    if (witness.size() < 3) {
      return std::nullopt;
    }
    has_annex = true;
    --control_index;
  }

  if (control_index == 0 || !IsTaprootControlBlock(witness[control_index])) {
    return std::nullopt;
  }

  return RevealedLeaf{
      control_index - 1,
      control_index,
      has_annex,
  };
}

bool IsP2tr(const std::vector<uint8_t> &script_pubkey) {
  return script_pubkey.size() == 34 && script_pubkey[0] == 0x51 &&
         script_pubkey[1] == 0x20;
}

uint32_t ParseUint32(const std::string &value, const char *option_name) {
  size_t consumed = 0;
  const unsigned long parsed = std::stoul(value, &consumed, 0);
  if (consumed != value.size() ||
      parsed > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("invalid ") + option_name + ": " +
                             value);
  }
  return static_cast<uint32_t>(parsed);
}

Options ParseOptions(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--self-test") {
      options.self_test = true;
    } else if (arg == "--blocks-dir" && i + 1 < argc) {
      options.blocks_dir = argv[++i];
    } else if (arg == "--expected-magic" && i + 1 < argc) {
      options.expected_magic = ParseUint32(argv[++i], "--expected-magic");
    } else if (arg == "--expected-tip" && i + 1 < argc) {
      options.expected_tip = LowerHex(argv[++i]);
    } else {
      throw std::runtime_error("unknown or incomplete argument: " + arg);
    }
  }

  if (!options.self_test &&
      (options.blocks_dir.empty() || options.expected_magic == 0 ||
       options.expected_tip.size() != 64)) {
    throw std::runtime_error(
        "usage: ccv-chain-audit --blocks-dir DIR "
        "--expected-magic 0xNNNNNNNN --expected-tip 64_HEX_CHARS");
  }
  return options;
}

bool HasRawCcvByte(const std::vector<uint8_t> &script) {
  return std::find(script.begin(), script.end(), OP_CHECKCONTRACTVERIFY) !=
         script.end();
}

void Require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(std::string("self-test failed: ") + message);
  }
}

int SelfTest() {
  Require(ScanScript({OP_CHECKCONTRACTVERIFY}).contains_ccv_opcode,
          "direct CCV opcode must match");
  Require(!ScanScript({0x01, OP_CHECKCONTRACTVERIFY}).contains_ccv_opcode,
          "CCV byte inside direct push must not match");
  Require(!ScanScript({OP_PUSHDATA1, 0x01, OP_CHECKCONTRACTVERIFY})
               .contains_ccv_opcode,
          "CCV byte inside PUSHDATA1 must not match");
  Require(!ScanScript({OP_PUSHDATA2, 0x01, 0x00, OP_CHECKCONTRACTVERIFY})
               .contains_ccv_opcode,
          "CCV byte inside PUSHDATA2 must not match");
  Require(!ScanScript(
               {OP_PUSHDATA4, 0x01, 0x00, 0x00, 0x00, OP_CHECKCONTRACTVERIFY})
               .contains_ccv_opcode,
          "CCV byte inside PUSHDATA4 must not match");
  Require(ScanScript({OP_PUSHDATA2, 0x01}).malformed,
          "truncated PUSHDATA2 must be malformed");

  std::vector<uint8_t> control(TAPROOT_CONTROL_BASE_SIZE, 0);
  control[0] = TAPSCRIPT_LEAF_VERSION;
  const std::vector<uint8_t> script{OP_CHECKCONTRACTVERIFY};
  const std::vector<uint8_t> annex{TAPROOT_ANNEX_TAG, 0x01};
  const std::vector<uint8_t> signature(64, 0x01);

  const std::vector<std::vector<uint8_t>> no_annex_witness{signature, script,
                                                           control};
  auto no_annex = FindRevealedLeaf(no_annex_witness);
  Require(no_annex.has_value(), "script-path witness must be found");
  Require(no_annex_witness[no_annex->script_index] == script &&
              !no_annex->has_annex,
          "script-path leaf without annex must be selected");

  const std::vector<std::vector<uint8_t>> annex_witness{signature, script,
                                                        control, annex};
  auto with_annex = FindRevealedLeaf(annex_witness);
  Require(with_annex.has_value() &&
              annex_witness[with_annex->script_index] == script &&
              with_annex->has_annex,
          "trailing annex must not be mistaken for control block");
  Require(!FindRevealedLeaf({signature}).has_value(),
          "key-path witness must not be classified as script path");

  std::cout << "{\"self_test\":\"pass\",\"cases\":10}\n";
  return 0;
}

std::vector<std::filesystem::path>
FindBlockFiles(const std::filesystem::path &blocks_dir) {
  if (!std::filesystem::is_directory(blocks_dir)) {
    throw std::runtime_error("blocks directory does not exist: " +
                             blocks_dir.string());
  }

  std::vector<std::filesystem::path> files;
  for (const auto &entry : std::filesystem::directory_iterator(blocks_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.size() == 12 && name.rfind("blk", 0) == 0 &&
        name.substr(8) == ".dat" &&
        std::all_of(name.begin() + 3, name.begin() + 8,
                    [](unsigned char c) { return std::isdigit(c); })) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    throw std::runtime_error("no blkNNNNN.dat files found in: " +
                             blocks_dir.string());
  }
  return files;
}

void ReadExact(std::ifstream &input, void *destination, size_t size,
               const std::filesystem::path &file, uint64_t offset,
               const char *field) {
  input.read(static_cast<char *>(destination),
             static_cast<std::streamsize>(size));
  if (input.gcount() != static_cast<std::streamsize>(size)) {
    throw std::runtime_error("incomplete " + std::string(field) + " in " +
                             file.string() + " at offset " +
                             std::to_string(offset));
  }
}

void ScanBlockFile(const std::filesystem::path &file, uint32_t expected_magic,
                   const std::string &expected_tip, Counters &counters,
                   bool &expected_tip_seen, std::vector<Match> &matches) {
  const uint64_t file_size = std::filesystem::file_size(file);
  counters.files++;
  counters.bytes_at_scan_start += file_size;

  std::ifstream input(file, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open block file: " + file.string());
  }

  uint64_t offset = 0;
  while (offset < file_size) {
    uint32_t magic = 0;
    uint32_t block_size = 0;
    ReadExact(input, &magic, sizeof(magic), file, offset, "magic");
    ReadExact(input, &block_size, sizeof(block_size), file, offset + 4,
              "block size");

    if (magic != expected_magic) {
      std::ostringstream error;
      error << "unexpected network magic 0x" << std::hex << magic << " in "
            << file.string() << " at offset " << std::dec << offset;
      throw std::runtime_error(error.str());
    }
    if (block_size == 0 || block_size > 32u * 1024u * 1024u) {
      throw std::runtime_error(
          "invalid block size " + std::to_string(block_size) + " in " +
          file.string() + " at offset " + std::to_string(offset));
    }
    if (offset + 12ull + block_size > file_size) {
      throw std::runtime_error("partial block record in " + file.string() +
                               " at offset " + std::to_string(offset));
    }

    std::vector<uint8_t> block_data(block_size);
    ReadExact(input, block_data.data(), block_data.size(), file, offset + 8,
              "block body");
    uint32_t stored_checksum = 0;
    ReadExact(input, &stored_checksum, sizeof(stored_checksum), file,
              offset + 8ull + block_size, "checksum");
    if (stored_checksum !=
        Fnv1aChecksum(block_data.data(), block_data.size())) {
      throw std::runtime_error("checksum mismatch in " + file.string() +
                               " at offset " + std::to_string(offset));
    }

    counters.records++;
    counters.checksum_verified_records++;

    auto parsed = dinero::Block::Deserialize(block_data);
    if (!parsed.has_value()) {
      throw std::runtime_error("canonical block deserialization failed in " +
                               file.string() + " at offset " +
                               std::to_string(offset));
    }

    const std::string block_hash = LowerHex(parsed->GetHash().GetHex());
    if (block_hash == expected_tip) {
      expected_tip_seen = true;
    }

    for (size_t tx_index = 0; tx_index < parsed->vtx.size(); ++tx_index) {
      const auto &tx = parsed->vtx[tx_index];
      counters.transactions++;
      counters.inputs += tx.vin.size();
      counters.outputs += tx.vout.size();
      for (const auto &output : tx.vout) {
        if (IsP2tr(output.scriptPubKey)) {
          counters.p2tr_outputs++;
        }
      }

      for (size_t input_index = 0; input_index < tx.vin.size(); ++input_index) {
        const auto &witness = tx.vin[input_index].witness;
        const auto leaf = FindRevealedLeaf(witness);
        if (!leaf.has_value()) {
          continue;
        }
        counters.revealed_tapscript_spends++;
        if (leaf->has_annex) {
          counters.revealed_tapscript_spends_with_annex++;
        }

        const auto &script = witness[leaf->script_index];
        const ScriptScan scan = ScanScript(script);
        if (scan.malformed) {
          counters.malformed_revealed_scripts++;
        }
        if (HasRawCcvByte(script) && !scan.contains_ccv_opcode) {
          counters.ccv_byte_but_not_opcode++;
        }
        if (scan.contains_ccv_opcode) {
          matches.push_back(Match{
              file.filename().string(),
              offset,
              block_hash,
              tx.GetTxid().AsUint256().GetHex(),
              tx_index,
              input_index,
          });
        }
      }
    }

    offset += 12ull + block_size;
  }
}

void PrintReport(const Options &options, const Counters &counters,
                 bool expected_tip_seen, const std::vector<Match> &matches) {
  std::cout << "{\n"
            << "  \"schema\": \"dinero.ccv-chain-audit.v1\",\n"
            << "  \"blocks_dir\": \"" << JsonEscape(options.blocks_dir.string())
            << "\",\n"
            << "  \"expected_magic\": \"0x" << std::hex << std::setw(8)
            << std::setfill('0') << options.expected_magic << std::dec
            << "\",\n"
            << "  \"expected_tip\": \"" << options.expected_tip << "\",\n"
            << "  \"expected_tip_seen\": "
            << (expected_tip_seen ? "true" : "false") << ",\n"
            << "  \"files\": " << counters.files << ",\n"
            << "  \"bytes_at_scan_start\": " << counters.bytes_at_scan_start
            << ",\n"
            << "  \"block_records\": " << counters.records << ",\n"
            << "  \"checksum_verified_records\": "
            << counters.checksum_verified_records << ",\n"
            << "  \"transactions\": " << counters.transactions << ",\n"
            << "  \"inputs\": " << counters.inputs << ",\n"
            << "  \"outputs\": " << counters.outputs << ",\n"
            << "  \"p2tr_outputs\": " << counters.p2tr_outputs << ",\n"
            << "  \"revealed_tapscript_spends\": "
            << counters.revealed_tapscript_spends << ",\n"
            << "  \"revealed_tapscript_spends_with_annex\": "
            << counters.revealed_tapscript_spends_with_annex << ",\n"
            << "  \"malformed_revealed_scripts\": "
            << counters.malformed_revealed_scripts << ",\n"
            << "  \"ccv_byte_but_not_opcode\": "
            << counters.ccv_byte_but_not_opcode << ",\n"
            << "  \"ccv_opcode_matches\": " << matches.size() << ",\n"
            << "  \"matches\": [";

  for (size_t i = 0; i < matches.size(); ++i) {
    const auto &match = matches[i];
    std::cout << (i == 0 ? "\n" : ",\n") << "    {\"file\":\""
              << JsonEscape(match.file) << "\",\"offset\":" << match.offset
              << ",\"block_hash\":\"" << match.block_hash << "\",\"txid\":\""
              << match.txid << "\",\"tx_index\":" << match.tx_index
              << ",\"input_index\":" << match.input_index << "}";
  }
  if (!matches.empty()) {
    std::cout << "\n  ";
  }
  std::cout
      << "],\n"
      << "  \"conclusion\": \""
      << (matches.empty() ? "no_revealed_ccv_spends_in_scanned_block_store"
                          : "revealed_ccv_spends_detected")
      << "\",\n"
      << "  \"limitation\": "
         "\"unspent Taproot outputs hide their leaves; absence of live CCV "
         "outputs requires independent construction provenance\"\n"
      << "}\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = ParseOptions(argc, argv);
    if (options.self_test) {
      return SelfTest();
    }

    Counters counters;
    bool expected_tip_seen = false;
    std::vector<Match> matches;
    for (const auto &file : FindBlockFiles(options.blocks_dir)) {
      ScanBlockFile(file, options.expected_magic, options.expected_tip,
                    counters, expected_tip_seen, matches);
    }

    PrintReport(options, counters, expected_tip_seen, matches);
    if (!expected_tip_seen) {
      std::cerr
          << "audit failed: expected tip was not present in scanned files\n";
      return 4;
    }
    return matches.empty() ? 0 : 3;
  } catch (const std::exception &error) {
    std::cerr << "ccv-chain-audit: " << error.what() << "\n";
    return 2;
  }
}
