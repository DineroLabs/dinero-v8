#include "consensus/script.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {
namespace consensus {

// ============================================================================
// Script Data Operations
// ============================================================================

void Script::pushData(const std::vector<uint8_t>& data) {
    size_t size = data.size();

    if (size < OP_PUSHDATA1) {
        // Direct push (0-75 bytes)
        data_.push_back(static_cast<uint8_t>(size));
    } else if (size <= 0xFF) {
        // OP_PUSHDATA1 <1 byte length> <data>
        data_.push_back(OP_PUSHDATA1);
        data_.push_back(static_cast<uint8_t>(size));
    } else if (size <= 0xFFFF) {
        // OP_PUSHDATA2 <2 bytes length> <data>
        data_.push_back(OP_PUSHDATA2);
        data_.push_back(static_cast<uint8_t>(size & 0xFF));
        data_.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    } else {
        // OP_PUSHDATA4 <4 bytes length> <data>
        data_.push_back(OP_PUSHDATA4);
        data_.push_back(static_cast<uint8_t>(size & 0xFF));
        data_.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
        data_.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
        data_.push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
    }

    data_.insert(data_.end(), data.begin(), data.end());
}

void Script::pushInt64(int64_t n) {
    if (n == -1 || (n >= 1 && n <= 16)) {
        // Use direct opcodes for -1, 1-16
        data_.push_back(n == -1 ? OP_1NEGATE : static_cast<uint8_t>(OP_1 + (n - 1)));
    } else if (n == 0) {
        data_.push_back(OP_0);
    } else {
        pushData(scriptNumEncode(n));
    }
}

// ============================================================================
// Standard Script Detection
// ============================================================================

bool Script::isPayToPubKeyHash() const {
    // P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
    return (size() == 25 &&
            data_[0] == OP_DUP &&
            data_[1] == OP_HASH160 &&
            data_[2] == 20 &&
            data_[23] == OP_EQUALVERIFY &&
            data_[24] == OP_CHECKSIG);
}

bool Script::isPayToScriptHash() const {
    // P2SH: OP_HASH160 <20 bytes> OP_EQUAL
    return (size() == 23 &&
            data_[0] == OP_HASH160 &&
            data_[1] == 20 &&
            data_[22] == OP_EQUAL);
}

bool Script::isPayToWitnessPubKeyHash() const {
    // P2WPKH: OP_0 <20 bytes>
    return (size() == 22 &&
            data_[0] == OP_0 &&
            data_[1] == 20);
}

bool Script::isPayToWitnessScriptHash() const {
    // P2WSH: OP_0 <32 bytes>
    return (size() == 34 &&
            data_[0] == OP_0 &&
            data_[1] == 32);
}

bool Script::isPayToTaproot() const {
    // P2TR: OP_1 <32 bytes>
    return (size() == 34 &&
            data_[0] == OP_1 &&
            data_[1] == 32);
}

bool Script::isWitnessProgram(int& version, std::vector<uint8_t>& program) const {
    if (size() < 4 || size() > 42) {
        return false;
    }

    // First byte must be OP_0 to OP_16 (witness version)
    if (data_[0] != OP_0 && (data_[0] < OP_1 || data_[0] > OP_16)) {
        return false;
    }

    // Second byte is program length
    size_t program_len = data_[1];
    if (program_len < 2 || program_len > 40) {
        return false;
    }

    if (size() != program_len + 2) {
        return false;
    }

    version = (data_[0] == OP_0) ? 0 : (data_[0] - OP_1 + 1);
    program.assign(data_.begin() + 2, data_.end());
    return true;
}

bool Script::isPushOnly() const {
    size_t i = 0;
    while (i < data_.size()) {
        opcodetype opcode = static_cast<opcodetype>(data_[i]);

        if (opcode > OP_16) {
            // Any opcode > OP_16 is not a push operation
            return false;
        }

        // Skip past push data
        if (opcode <= OP_PUSHDATA4) {
            size_t push_size = 0;
            if (opcode < OP_PUSHDATA1) {
                push_size = opcode;
                i += 1 + push_size;
            } else if (opcode == OP_PUSHDATA1) {
                if (i + 1 >= data_.size()) return false;
                push_size = data_[i + 1];
                i += 2 + push_size;
            } else if (opcode == OP_PUSHDATA2) {
                if (i + 2 >= data_.size()) return false;
                push_size = data_[i + 1] | (data_[i + 2] << 8);
                i += 3 + push_size;
            } else if (opcode == OP_PUSHDATA4) {
                if (i + 4 >= data_.size()) return false;
                push_size = data_[i + 1] | (data_[i + 2] << 8) |
                           (data_[i + 3] << 16) | (data_[i + 4] << 24);
                i += 5 + push_size;
            }
        } else {
            // OP_1NEGATE, OP_RESERVED, or OP_1 through OP_16
            i++;
        }
    }
    return true;
}

// ============================================================================
// Script Analysis
// ============================================================================

size_t Script::getSigOpCount(bool accurate) const {
    size_t count = 0;
    const uint8_t* pc = data_.data();
    const uint8_t* end = pc + data_.size();

    opcodetype last_opcode = OP_INVALIDOPCODE;

    while (pc < end) {
        opcodetype opcode = static_cast<opcodetype>(*pc++);

        if (opcode == OP_CHECKSIG || opcode == OP_CHECKSIGVERIFY) {
            count++;
        } else if (opcode == OP_CHECKMULTISIG || opcode == OP_CHECKMULTISIGVERIFY) {
            if (accurate && last_opcode >= OP_1 && last_opcode <= OP_16) {
                count += (last_opcode - OP_1 + 1);
            } else {
                count += 20;  // Max multisig pubkeys
            }
        }

        // Skip data pushes
        if (opcode >= 0 && opcode <= OP_PUSHDATA4) {
            size_t size = 0;
            if (opcode < OP_PUSHDATA1) {
                size = opcode;
            } else if (opcode == OP_PUSHDATA1 && pc < end) {
                size = *pc++;
            } else if (opcode == OP_PUSHDATA2 && pc + 1 < end) {
                size = pc[0] | (pc[1] << 8);
                pc += 2;
            } else if (opcode == OP_PUSHDATA4 && pc + 3 < end) {
                size = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
            }
            pc += size;
        }

        last_opcode = opcode;
    }

    return count;
}

std::string Script::toString() const {
    std::ostringstream oss;
    const uint8_t* pc = data_.data();
    const uint8_t* end = pc + data_.size();

    while (pc < end) {
        if (oss.tellp() > 0) oss << " ";

        opcodetype opcode = static_cast<opcodetype>(*pc++);

        // Handle data pushes
        if (opcode >= 0 && opcode <= OP_PUSHDATA4) {
            size_t size = 0;
            if (opcode < OP_PUSHDATA1) {
                size = opcode;
            } else if (opcode == OP_PUSHDATA1 && pc < end) {
                size = *pc++;
            } else if (opcode == OP_PUSHDATA2 && pc + 1 < end) {
                size = pc[0] | (pc[1] << 8);
                pc += 2;
            } else if (opcode == OP_PUSHDATA4 && pc + 3 < end) {
                size = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
            }

            if (pc + size <= end) {
                oss << "0x";
                for (size_t i = 0; i < std::min(size, size_t(10)); i++) {
                    oss << std::hex << std::setw(2) << std::setfill('0') << (int)pc[i];
                }
                if (size > 10) oss << "...";
                pc += size;
            }
        } else {
            oss << getOpcodeName(opcode);
        }
    }

    return oss.str();
}

// ============================================================================
// Opcode Utilities
// ============================================================================

const char* getOpcodeName(opcodetype opcode) {
    switch (opcode) {
        case OP_0: return "OP_0";
        case OP_PUSHDATA1: return "OP_PUSHDATA1";
        case OP_PUSHDATA2: return "OP_PUSHDATA2";
        case OP_PUSHDATA4: return "OP_PUSHDATA4";
        case OP_1NEGATE: return "OP_1NEGATE";
        case OP_1: return "OP_1";
        case OP_2: return "OP_2";
        case OP_3: return "OP_3";
        case OP_4: return "OP_4";
        case OP_5: return "OP_5";
        case OP_6: return "OP_6";
        case OP_7: return "OP_7";
        case OP_8: return "OP_8";
        case OP_9: return "OP_9";
        case OP_10: return "OP_10";
        case OP_11: return "OP_11";
        case OP_12: return "OP_12";
        case OP_13: return "OP_13";
        case OP_14: return "OP_14";
        case OP_15: return "OP_15";
        case OP_16: return "OP_16";
        case OP_NOP: return "OP_NOP";
        case OP_IF: return "OP_IF";
        case OP_NOTIF: return "OP_NOTIF";
        case OP_ELSE: return "OP_ELSE";
        case OP_ENDIF: return "OP_ENDIF";
        case OP_VERIFY: return "OP_VERIFY";
        case OP_RETURN: return "OP_RETURN";
        case OP_TOALTSTACK: return "OP_TOALTSTACK";
        case OP_FROMALTSTACK: return "OP_FROMALTSTACK";
        case OP_2DROP: return "OP_2DROP";
        case OP_2DUP: return "OP_2DUP";
        case OP_3DUP: return "OP_3DUP";
        case OP_2OVER: return "OP_2OVER";
        case OP_2ROT: return "OP_2ROT";
        case OP_2SWAP: return "OP_2SWAP";
        case OP_IFDUP: return "OP_IFDUP";
        case OP_DEPTH: return "OP_DEPTH";
        case OP_DROP: return "OP_DROP";
        case OP_DUP: return "OP_DUP";
        case OP_NIP: return "OP_NIP";
        case OP_OVER: return "OP_OVER";
        case OP_PICK: return "OP_PICK";
        case OP_ROLL: return "OP_ROLL";
        case OP_ROT: return "OP_ROT";
        case OP_SWAP: return "OP_SWAP";
        case OP_TUCK: return "OP_TUCK";
        case OP_SIZE: return "OP_SIZE";
        case OP_EQUAL: return "OP_EQUAL";
        case OP_EQUALVERIFY: return "OP_EQUALVERIFY";
        case OP_1ADD: return "OP_1ADD";
        case OP_1SUB: return "OP_1SUB";
        case OP_NEGATE: return "OP_NEGATE";
        case OP_ABS: return "OP_ABS";
        case OP_NOT: return "OP_NOT";
        case OP_0NOTEQUAL: return "OP_0NOTEQUAL";
        case OP_ADD: return "OP_ADD";
        case OP_SUB: return "OP_SUB";
        case OP_BOOLAND: return "OP_BOOLAND";
        case OP_BOOLOR: return "OP_BOOLOR";
        case OP_NUMEQUAL: return "OP_NUMEQUAL";
        case OP_NUMEQUALVERIFY: return "OP_NUMEQUALVERIFY";
        case OP_NUMNOTEQUAL: return "OP_NUMNOTEQUAL";
        case OP_LESSTHAN: return "OP_LESSTHAN";
        case OP_GREATERTHAN: return "OP_GREATERTHAN";
        case OP_LESSTHANOREQUAL: return "OP_LESSTHANOREQUAL";
        case OP_GREATERTHANOREQUAL: return "OP_GREATERTHANOREQUAL";
        case OP_MIN: return "OP_MIN";
        case OP_MAX: return "OP_MAX";
        case OP_WITHIN: return "OP_WITHIN";
        case OP_RIPEMD160: return "OP_RIPEMD160";
        case OP_SHA1: return "OP_SHA1";
        case OP_SHA256: return "OP_SHA256";
        case OP_HASH160: return "OP_HASH160";
        case OP_HASH256: return "OP_HASH256";
        case OP_CODESEPARATOR: return "OP_CODESEPARATOR";
        case OP_CHECKSIG: return "OP_CHECKSIG";
        case OP_CHECKSIGVERIFY: return "OP_CHECKSIGVERIFY";
        case OP_CHECKMULTISIG: return "OP_CHECKMULTISIG";
        case OP_CHECKMULTISIGVERIFY: return "OP_CHECKMULTISIGVERIFY";
        case OP_NOP1: return "OP_NOP1";
        case OP_CHECKLOCKTIMEVERIFY: return "OP_CHECKLOCKTIMEVERIFY";
        case OP_CHECKSEQUENCEVERIFY: return "OP_CHECKSEQUENCEVERIFY";
        case OP_CHECKTEMPLATEVERIFY: return "OP_CHECKTEMPLATEVERIFY";  // Phase 28: Also OP_NOP4
        case OP_NOP5: return "OP_NOP5";
        case OP_NOP6: return "OP_NOP6";
        case OP_NOP7: return "OP_NOP7";
        case OP_NOP8: return "OP_NOP8";
        case OP_NOP9: return "OP_NOP9";
        case OP_NOP10: return "OP_NOP10";
        case OP_CHECKSIGADD: return "OP_CHECKSIGADD";
        // Phase 28: Covenant opcodes
        case OP_CHECKSIGFROMSTACK: return "OP_CHECKSIGFROMSTACK";
        case OP_CHECKSIGFROMSTACKVERIFY: return "OP_CHECKSIGFROMSTACKVERIFY";
        case OP_TXHASH: return "OP_TXHASH";
        case OP_CHECKCONTRACTVERIFY: return "OP_CHECKCONTRACTVERIFY";
        default: return "OP_UNKNOWN";
    }
}

bool isOpcodeDisabled(opcodetype opcode) {
    switch (opcode) {
        case OP_CAT:
        case OP_SUBSTR:
        case OP_LEFT:
        case OP_RIGHT:
        case OP_INVERT:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_2MUL:
        case OP_2DIV:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_LSHIFT:
        case OP_RSHIFT:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// Script Number Encoding (Bitcoin's Little-Endian Format)
// ============================================================================

std::vector<uint8_t> scriptNumEncode(int64_t value) {
    if (value == 0) {
        return {};
    }

    std::vector<uint8_t> result;
    const bool negative = value < 0;
    uint64_t abs_value = negative ? -value : value;

    while (abs_value > 0) {
        result.push_back(abs_value & 0xFF);
        abs_value >>= 8;
    }

    // If the most significant byte is >= 0x80 and the value is positive,
    // add a zero byte to prevent it from being interpreted as negative
    if (result.back() & 0x80) {
        result.push_back(negative ? 0x80 : 0);
    } else if (negative) {
        result.back() |= 0x80;
    }

    return result;
}

int64_t scriptNumDecode(const std::vector<uint8_t>& data, bool require_minimal) {
    if (data.empty()) {
        return 0;
    }

    if (require_minimal) {
        // Check for overly long encoding
        if (data.size() > 1) {
            if (data.back() == 0x00 && !(data[data.size() - 2] & 0x80)) {
                // Non-minimally encoded
                return 0;  // Or throw error
            }
            if (data.back() == 0x80 && !(data[data.size() - 2] & 0x80)) {
                // Non-minimally encoded
                return 0;  // Or throw error
            }
        }
    }

    int64_t result = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        result |= static_cast<int64_t>(data[i]) << (8 * i);
    }

    // Check sign bit (most significant bit of last byte)
    if (data.back() & 0x80) {
        // Negative number - clear sign bit and negate
        return -(result & ~(0x80ULL << (8 * (data.size() - 1))));
    }

    return result;
}

bool isMinimallyEncoded(const std::vector<uint8_t>& data, size_t max_size) {
    if (data.size() > max_size) {
        return false;
    }

    if (data.empty()) {
        return true;
    }

    // Check that the number is encoded with the minimum possible number of bytes
    //
    // If the most-significant-byte (excluding the sign bit) is zero, then we're not minimal.
    // Note how this test also rejects the negative-zero encoding, 0x80.
    if ((data.back() & 0x7f) == 0) {
        // One byte of zero is not minimal (should be empty)
        // Also catch unnecessary leading zeros
        if (data.size() <= 1 || (data[data.size() - 2] & 0x80) == 0) {
            return false;
        }
    }

    return true;
}

} // namespace consensus
} // namespace dinero
