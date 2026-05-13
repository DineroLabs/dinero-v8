#pragma once
#include <string>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace ws {

// ---------------- base64 (encode) ----------------
inline std::string base64_encode(const uint8_t* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i+1]) << 8) | data[i+2];
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6)  & 0x3F]);
        out.push_back(tbl[v & 0x3F]);
        i += 3;
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        if (i + 1 < len) {
            v |= uint32_t(data[i+1]) << 8;
            out.push_back(tbl[(v >> 12) & 0x3F]);
            out.push_back(tbl[(v >> 6)  & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back(tbl[(v >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

// ---------------- minimal SHA-1 ----------------
struct Sha1 {
    std::array<uint32_t,5> h{};
    uint64_t bitlen = 0;
    uint8_t  buf[64];
    size_t   buflen = 0;

    static inline uint32_t rol(uint32_t x, int n){ return (x<<n) | (x>>(32-n)); }

    void init() {
        h = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
        bitlen = 0; buflen = 0;
    }

    static inline uint32_t be32(const uint8_t* p){
        return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
    }

    void process_block(const uint8_t* b) {
        uint32_t w[80];
        for (int i=0;i<16;i++) w[i] = be32(b + 4*i);
        for (int i=16;i<80;i++) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);

        uint32_t a=h[0], b0=h[1], c=h[2], d=h[3], e=h[4];
        for (int i=0;i<80;i++) {
            uint32_t f,k;
            if (i<20)      { f=(b0&c)|((~b0)&d);    k=0x5A827999u; }
            else if(i<40)  { f=b0^c^d;             k=0x6ED9EBA1u; }
            else if(i<60)  { f=(b0&c)|(b0&d)|(c&d);k=0x8F1BBCDCu; }
            else           { f=b0^c^d;             k=0xCA62C1D6u; }
            uint32_t temp = rol(a,5) + f + e + k + w[i];
            e=d; d=c; c=rol(b0,30); b0=a; a=temp;
        }
        h[0]+=a; h[1]+=b0; h[2]+=c; h[3]+=d; h[4]+=e;
    }

    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bitlen += uint64_t(len) * 8;
        while (len) {
            size_t take = std::min(len, 64 - buflen);
            std::memcpy(buf + buflen, p, take);
            buflen += take; p += take; len -= take;
            if (buflen == 64) { process_block(buf); buflen = 0; }
        }
    }

    void final(uint8_t out[20]) {
        // append 0x80
        buf[buflen++] = 0x80;
        if (buflen > 56) {
            while (buflen < 64) buf[buflen++] = 0;
            process_block(buf); buflen = 0;
        }
        while (buflen < 56) buf[buflen++] = 0;
        // length (big-endian)
        for (int i=7;i>=0;i--) buf[buflen++] = uint8_t((bitlen >> (i*8)) & 0xFF);
        process_block(buf);

        // output big-endian
        for (int i=0;i<5;i++){
            out[4*i+0] = uint8_t((h[i]>>24)&0xFF);
            out[4*i+1] = uint8_t((h[i]>>16)&0xFF);
            out[4*i+2] = uint8_t((h[i]>>8 )&0xFF);
            out[4*i+3] = uint8_t((h[i]    )&0xFF);
        }
    }
};

inline std::string sha1_base64(const std::string& ascii) {
    Sha1 s; s.init(); s.update(ascii.data(), ascii.size());
    uint8_t digest[20]; s.final(digest);
    return base64_encode(digest, sizeof(digest));
}

// ---------------- accept header ----------------
inline std::string websocket_accept(const std::string& sec_websocket_key_ascii) {
    static const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string m = sec_websocket_key_ascii;
    m += GUID;
    return sha1_base64(m);  // 28-char base64
}

} // namespace ws
