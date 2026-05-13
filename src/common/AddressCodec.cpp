#include "AddressCodec.h"

#include <stdexcept>
#include <algorithm>
#include <cctype>

#include "include/daemon/bech32_decode.h"           // Bech32 v0 decode
#include "include/wallet/address.h"                 // Base58Check encode/decode

namespace {

static inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return char(::tolower(c)); });
    return s;
}

static inline Network netFromHrp(const std::string &hrp) {
    if (hrp == "din") return Network::MAIN;
    if (hrp == "tdin") return Network::TEST;
    if (hrp == "rdin") return Network::REGTEST;
    throw std::runtime_error("Unknown HRP");
}

static inline std::string hrpFromNet(Network n) {
    switch (n) {
        case Network::MAIN: return "din";
        case Network::TEST: return "tdin";
        case Network::REGTEST: return "rdin";
    }
    return "din";
}

// Minimal bech32/bech32m encoder (for P2TR bech32m). We keep v0 encoding delegated to daemon/offline code.
static uint32_t polymod(const std::vector<uint8_t>& values) {
    uint32_t chk = 1;
    for (uint8_t v : values) {
        uint8_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ v;
        if (top & 1) chk ^= 0x3b6a57b2;
        if (top & 2) chk ^= 0x26508e6d;
        if (top & 4) chk ^= 0x1ea119fa;
        if (top & 8) chk ^= 0x3d4233dd;
        if (top & 16) chk ^= 0x2a1462b3;
    }
    return chk;
}

static std::vector<uint8_t> hrpExpand(const std::string& hrp) {
    std::vector<uint8_t> ret;
    ret.reserve(hrp.size() * 2 + 1);
    for (char c : hrp) ret.push_back((uint8_t)(std::tolower(c)) >> 5);
    ret.push_back(0);
    for (char c : hrp) ret.push_back((uint8_t)(std::tolower(c)) & 31);
    return ret;
}

static bool convertBits(std::vector<uint8_t>& out, const std::vector<uint8_t>& in, int fromBits, int toBits, bool pad) {
    uint32_t acc = 0;
    int bits = 0;
    const uint32_t maxv = (1u << toBits) - 1u;
    for (uint8_t value : in) {
        if ((value >> fromBits) != 0) return false; // invalid high bits
        acc = (acc << fromBits) | value;
        bits += fromBits;
        while (bits >= toBits) {
            bits -= toBits;
            out.push_back((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits) out.push_back((acc << (toBits - bits)) & maxv);
    } else if (bits >= fromBits || ((acc << (toBits - bits)) & maxv)) {
        return false;
    }
    return true;
}

static std::string encodeBech32Variant(const std::string& hrp, int witver, const std::vector<uint8_t>& witprog, bool bech32m) {
    static const char* ALPHABET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    if (hrp.empty() || witver < 0 || witver > 16) return {};
    if (witprog.size() < 2 || witprog.size() > 40) return {};

    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>(witver));
    if (!convertBits(data, witprog, 8, 5, true)) return {};

    // checksum creation differs by constant xor at end
    std::vector<uint8_t> vals = hrpExpand(hrp);
    vals.insert(vals.end(), data.begin(), data.end());
    // Append 6 zeroes for checksum space
    std::vector<uint8_t> valsWithPad = vals;
    valsWithPad.insert(valsWithPad.end(), {0,0,0,0,0,0});
    uint32_t pm = polymod(valsWithPad) ^ (bech32m ? 0x2bc830a3 : 1);
    std::vector<uint8_t> checksum(6);
    for (int i = 0; i < 6; ++i) checksum[i] = (pm >> (5 * (5 - i))) & 31;

    std::string out = hrp;
    out.push_back('1');
    for (uint8_t v : data) out.push_back(ALPHABET[v]);
    for (uint8_t v : checksum) out.push_back(ALPHABET[v]);
    // force lowercase
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

} // namespace

DecodedAddr AddressCodec::decode(const std::string &s) {
    // Bech32/Bech32m path
    const auto pos = s.rfind('1');
    if (pos != std::string::npos) {
        std::string hrpLower = toLower(s.substr(0, pos));
        if (hrpLower == "din" || hrpLower == "tdin" || hrpLower == "rdin") {
            // Map chars to 5-bit values using Bech32 alphabet
            static const char* ALPHABET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
            std::string dataPart = s.substr(pos + 1);
            std::vector<uint8_t> vals; vals.reserve(dataPart.size());
            for (char c : dataPart) {
                const char* p = std::strchr(ALPHABET, std::tolower(static_cast<unsigned char>(c)));
                if (!p) throw std::runtime_error("invalid bech32 charset");
                vals.push_back(static_cast<uint8_t>(p - ALPHABET));
            }
            if (vals.size() < 7) throw std::runtime_error("bech32 too short");
            // Verify checksum for both variants
            std::vector<uint8_t> expand = hrpExpand(hrpLower);
            std::vector<uint8_t> all = expand; all.insert(all.end(), vals.begin(), vals.end());
            uint32_t pm = polymod(all);
            bool isBech32 = (pm == 1);
            bool isBech32m = (pm == 0x2bc830a3);
            if (!isBech32 && !isBech32m) throw std::runtime_error("bech32 checksum fail");
            // Strip checksum
            vals.resize(vals.size() - 6);
            if (vals.empty()) throw std::runtime_error("bech32 no payload");
            int witver = vals[0]; if (witver < 0 || witver > 16) throw std::runtime_error("bad witver");
            std::vector<uint8_t> prog5(vals.begin() + 1, vals.end());
            std::vector<uint8_t> witprog;
            if (!convertBits(witprog, prog5, 5, 8, false)) throw std::runtime_error("convertbits fail");

            Network net = netFromHrp(hrpLower);
            if (witver == 0 && isBech32) {
                if (witprog.size() == 20) return {AddrType::P2WPKH, net, witprog};
                if (witprog.size() == 32) return {AddrType::P2WSH,  net, witprog};
                throw std::runtime_error("bad v0 prog len");
            }
            if (witver == 1 && isBech32m) {
                if (witprog.size() == 32) return {AddrType::P2TR, net, witprog};
                throw std::runtime_error("bad v1 prog len");
            }
            throw std::runtime_error("checksum variant mismatch");
        }
    }
    // Fallback to Base58Check using existing code
    std::vector<uint8_t> payload;
    if (!dinero::Address::decodeBase58Check(s, payload)) {
        throw std::runtime_error("not bech32 and base58check failed");
    }
    if (payload.size() != 21) throw std::runtime_error("legacy payload wrong size");
    uint8_t ver = payload[0];
    std::vector<uint8_t> h160(payload.begin()+1, payload.end());
    if (ver == 0x28) return {AddrType::P2PKH, Network::MAIN, h160};
    if (ver == 0x10) return {AddrType::P2SH,  Network::MAIN, h160};
    if (ver == 0x6F) return {AddrType::P2PKH, Network::TEST, h160};
    if (ver == 0xC4) return {AddrType::P2SH,  Network::TEST, h160};
    throw std::runtime_error("unknown legacy version byte");
}

std::string AddressCodec::encodeP2WPKH(Network net, const std::vector<uint8_t>& h160) {
    if (h160.size() != 20) throw std::runtime_error("bad h160");
    return encodeBech32Variant(hrpFromNet(net), 0, h160, /*bech32m=*/false);
}

std::string AddressCodec::encodeP2TR(Network net, const std::vector<uint8_t>& x32) {
    if (x32.size() != 32) throw std::runtime_error("bad xonly pubkey");
    return encodeBech32Variant(hrpFromNet(net), 1, x32, /*bech32m=*/true);
}

std::string AddressCodec::encodeP2PKH(Network net, const std::vector<uint8_t>& h160) {
    if (h160.size() != 20) throw std::runtime_error("bad h160");
    std::vector<uint8_t> raw; raw.reserve(21);
    raw.push_back(net == Network::MAIN ? 0x28 : 0x6F);
    raw.insert(raw.end(), h160.begin(), h160.end());
    return dinero::Address::encodeBase58Check(raw);
}

std::string AddressCodec::encodeP2SH(Network net, const std::vector<uint8_t>& h160) {
    if (h160.size() != 20) throw std::runtime_error("bad h160");
    std::vector<uint8_t> raw; raw.reserve(21);
    raw.push_back(net == Network::MAIN ? 0x10 : 0xC4);
    raw.insert(raw.end(), h160.begin(), h160.end());
    return dinero::Address::encodeBase58Check(raw);
}
