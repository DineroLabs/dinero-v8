#include "daemon/mining_target.h"
#include "daemon/bech32_decode.h"
#include "common/logger.h"
#include <sstream>

namespace dinero {

// Build coinbase scriptPubKey using stored witness data (preferred) or fallback to address decoding
bool BuildCoinbaseScriptPubKey(const MiningTarget& tgt, std::string& out_script, std::string* err) {
    // 1) Prefer the saved witness (can't fail if present)
    if (tgt.has_witness()) {
        if (tgt.witver == 1 && tgt.witprog.size() == 32) {
            // P2TR (Taproot): OP_1 <32-byte x-only pubkey>
            out_script = "5120"; // OP_1 (0x51) + push 32 bytes (0x20)
            for (uint8_t byte : tgt.witprog) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", byte);
                out_script += hex;
            }
            g_logger.info("Generated P2TR (Taproot) scriptPubKey from stored witness: " + out_script);
            return true;
        }
        if (tgt.is_p2wpkh()) {
            // P2WPKH: OP_0 <20-byte hash>
            out_script = "0014"; // OP_0 + push 20 bytes
            for (uint8_t byte : tgt.witprog) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", byte);
                out_script += hex;
            }
            g_logger.info("Generated P2WPKH scriptPubKey from stored witness: " + out_script);
            return true;
        }
        if (tgt.is_p2wsh()) {
            // P2WSH: OP_0 <32-byte hash>
            out_script = "0020"; // OP_0 + push 32 bytes
            for (uint8_t byte : tgt.witprog) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", byte);
                out_script += hex;
            }
            g_logger.info("Generated P2WSH scriptPubKey from stored witness: " + out_script);
            return true;
        }
        if (err) *err = "mining witness not supported: v" + std::to_string(tgt.witver) + ", len=" + std::to_string(tgt.witprog.size());
        return false;
    }

    // 2) Fallback: decode textual address (only if witness missing)
    if (tgt.addr.empty()) {
        if (err) *err = "no witness data and no address to decode";
        return false;
    }

    g_logger.info("No stored witness data, attempting to decode address: " + tgt.addr);

    // Try to decode using the Bech32 decoder from bech32_decode.h
    int decoded_witver = -1;
    std::vector<uint8_t> decoded_witprog;

    if (!dinero::mining::Bech32DecodeSegwit(tgt.addr, tgt.hrp, decoded_witver, decoded_witprog)) {
        if (err) *err = "bech32 decode failed for address: " + tgt.addr;
        return false;
    }

    // Generate scriptPubKey based on witness version and program length
    // Taproot (v1, 32 bytes)
    if (decoded_witver == 1 && decoded_witprog.size() == 32) {
        out_script = "5120"; // OP_1 (0x51) + push 32 bytes (0x20)
        for (uint8_t byte : decoded_witprog) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", byte);
            out_script += hex;
        }
        g_logger.info("Generated P2TR (Taproot) scriptPubKey from address: " + out_script);
        return true;
    }

    // SegWit v0: P2WPKH (20 bytes) or P2WSH (32 bytes)
    if (decoded_witver == 0 && (decoded_witprog.size() == 20 || decoded_witprog.size() == 32)) {
        if (decoded_witprog.size() == 20) {
            // P2WPKH
            out_script = "0014"; // OP_0 + push 20 bytes
        } else {
            // P2WSH
            out_script = "0020"; // OP_0 + push 32 bytes
        }
        for (uint8_t byte : decoded_witprog) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", byte);
            out_script += hex;
        }
        g_logger.info("Generated SegWit v0 scriptPubKey from address: " + out_script);
        return true;
    }

    if (err) *err = "unsupported witness version/length: v" + std::to_string(decoded_witver) + ", len=" + std::to_string(decoded_witprog.size());
    return false;
}

} // namespace dinero
