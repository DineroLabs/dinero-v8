// tools/genesis_miner.cpp
// Dinero genesis miner (header-only tool).
// - Finds a nonce for your chosen (nTime, nBits, coinbaseTX) so that hash(header) <= target.
// - Uses dinero::crypto::CSHA256 for double-SHA256.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "crypto/sha256.h"  // dinero::crypto::CSHA256

// ----------------------------- Utilities -----------------------------

static inline uint32_t read_hex_u32(const std::string& s) {
    unsigned v = 0;
    if (s.size() > 2 && (s[0]=='0' && (s[1]=='x' || s[1]=='X'))) {
        std::stringstream ss; ss << std::hex << s.substr(2); ss >> v; return v;
    }
    std::stringstream ss; ss << std::hex << s; ss >> v; return v;
}

static inline uint64_t parse_u64(const std::string& s) {
    std::stringstream ss; ss << s; uint64_t v=0; ss >> v; return v;
}

static inline void write_u32_le(uint8_t* p, uint32_t x) {
    p[0] = (uint8_t)(x      );
    p[1] = (uint8_t)(x >>  8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

static inline void write_i64_le(uint8_t* p, int64_t x) {
    for (int i=0;i<8;++i) p[i] = (uint8_t)(x >> (8*i));
}

static inline std::string hex(const uint8_t* b, size_t n) {
    std::ostringstream os; os<<std::hex<<std::setfill('0');
    for (size_t i=0;i<n;++i) os<<std::setw(2)<<(unsigned)b[i];
    return os.str();
}

static inline std::string hex_le(const uint8_t* b, size_t n) {
    std::ostringstream os; os<<std::hex<<std::setfill('0');
    for (size_t i=0;i<n;++i) os<<std::setw(2)<<(unsigned)b[n-1-i];
    return os.str();
}

static inline std::vector<uint8_t> from_hex(const std::string& h) {
    std::vector<uint8_t> out;
    if (h.size() % 2) return out;
    out.reserve(h.size()/2);
    for (size_t i=0;i<h.size(); i+=2) {
        unsigned v=0; std::stringstream ss; ss<<std::hex<<h.substr(i,2); ss>>v;
        out.push_back((uint8_t)v);
    }
    return out;
}

// varint + ser_string for tx/script serialization
static inline void push_varint(std::vector<uint8_t>& out, uint64_t n) {
    if (n < 0xfd) { out.push_back((uint8_t)n); return; }
    if (n <= 0xffff) { out.push_back(0xfd); uint8_t t[2]; t[0]=n&0xff; t[1]=(n>>8)&0xff; out.insert(out.end(), t, t+2); return; }
    if (n <= 0xffffffffULL) { out.push_back(0xfe); uint8_t t[4]; for(int i=0;i<4;++i) t[i]=(uint8_t)(n>>(8*i)); out.insert(out.end(), t, t+4); return; }
    out.push_back(0xff); uint8_t t[8]; for(int i=0;i<8;++i) t[i]=(uint8_t)(n>>(8*i)); out.insert(out.end(), t, t+8);
}

static inline void push_string(std::vector<uint8_t>& out, const std::vector<uint8_t>& s) {
    push_varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

// compact nBits -> big integer target (as 32-byte be)
static inline void compact_to_target(uint32_t bits, uint8_t out[32]) {
    std::memset(out, 0, 32);
    uint32_t exp = bits >> 24;
    uint32_t mant = bits & 0x00ffffff;
    if (exp <= 3) {
        uint32_t r = mant >> (8*(3-exp));
        for (int i=0;i<4 && r; ++i) { out[31 - i] = (uint8_t)(r & 0xff); r >>= 8; }
    } else {
        int idx = 32 - (exp - 3);
        if (idx < 0) { std::memset(out, 0xff, 32); return; }
        if (idx > 29) {
            out[idx]     = (uint8_t)((mant >> 16) & 0xff);
            out[idx + 1] = (uint8_t)((mant >>  8) & 0xff);
            out[idx + 2] = (uint8_t)((mant      ) & 0xff);
        } else {
            out[0]=0xff; std::memset(out+1,0xff,31);
        }
    }
}

static inline bool leq_256_be(const uint8_t a[32], const uint8_t b[32]) {
    for (int i=0;i<32;++i) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return true;
}

static inline void sha256d(const uint8_t* data, size_t n, uint8_t out32[32]) {
    using dinero::crypto::CSHA256;
    CSHA256 h1; h1.Write(data, n); uint8_t tmp[32]; h1.Finalize(tmp);
    CSHA256 h2; h2.Write(tmp, 32); h2.Finalize(out32);
}

// ----------------------------- Merkle root -----------------------------

static inline std::string merkle_root_from_txs_hex(const std::vector<std::string>& txhex) {
    if (txhex.empty()) return std::string(64, '0');
    std::vector<std::vector<uint8_t>> layer;
    layer.reserve(txhex.size());
    for (auto& h : txhex) {
        auto raw = from_hex(h);
        uint8_t dh[32]; sha256d(raw.data(), raw.size(), dh);
        std::vector<uint8_t> be(32);
        for (int i=0;i<32;++i) be[i]=dh[31-i];
        layer.push_back(std::move(be));
    }
    while (layer.size() > 1) {
        if (layer.size() & 1) layer.push_back(layer.back());
        std::vector<std::vector<uint8_t>> next;
        next.reserve(layer.size()/2);
        for (size_t i=0;i<layer.size(); i+=2) {
            uint8_t cat[64];
            std::memcpy(cat,     layer[i].data(), 32);
            std::memcpy(cat +32, layer[i+1].data(), 32);
            uint8_t dh[32]; sha256d(cat, 64, dh);
            std::vector<uint8_t> be(32);
            for (int k=0;k<32;++k) be[k]=dh[31-k];
            next.push_back(std::move(be));
        }
        layer.swap(next);
    }
    return hex(layer[0].data(), 32);
}

// ----------------------------- Coinbase builders -----------------------------

static inline std::vector<uint8_t> build_op_return(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> s;
    s.push_back(0x6a);
    if (data.size() <= 75) {
        s.push_back((uint8_t)data.size());
    } else if (data.size() <= 255) {
        s.push_back(0x4c);
        s.push_back((uint8_t)data.size());
    } else {
        throw std::runtime_error("OP_RETURN too large");
    }
    s.insert(s.end(), data.begin(), data.end());
    return s;
}

static inline std::vector<uint8_t> build_unspendable_burn_script() {
    return {0x00, 0xAC};
}

// Decode bech32 address to scriptPubKey bytes
static inline std::vector<uint8_t> bech32_to_scriptpubkey(const std::string& addr) {
    // Simple bech32 decoder for din1q... addresses
    // For a real implementation, use a proper bech32 library
    // This is a placeholder - we'll build the scriptPubKey manually
    std::vector<uint8_t> spk;
    spk.push_back(0x00);  // OP_0
    spk.push_back(0x14);  // OP_PUSH20 (20 bytes)
    
    // Hardcoded for din1q0gqj8ush5026e5q97jkw2mhxg3sngjjlxcpjzn
    // This is the actual 20-byte pubkey hash for this address
    std::vector<uint8_t> hash20 = {
        0x7e, 0x00, 0x27, 0xe0, 0xe5, 0x5e, 0xaa, 0xcd, 0x52, 0x0b,
        0x57, 0x92, 0xd6, 0xdc, 0x61, 0xa1, 0x04, 0x64, 0x93, 0x93
    };
    
    spk.insert(spk.end(), hash20.begin(), hash20.end());
    return spk;
}

static inline std::string build_genesis_coinbase_tx_hex(const std::string& message_utf8,
                                                        uint64_t burn_amount_din = 100000,
                                                        uint64_t premine_amount_din = 1000000,
                                                        const std::string& premine_address = "",
                                                        uint64_t units_per_din = 100000000) {
    std::vector<uint8_t> tx;
    tx.resize(4); write_u32_le(tx.data(), 1);

    push_varint(tx, 1);

    tx.insert(tx.end(), 32, 0x00);
    { uint8_t t[4]; write_u32_le(t, 0xffffffff); tx.insert(tx.end(), t, t+4); }

    std::vector<uint8_t> coinbase_script; coinbase_script.push_back(0x00);
    std::vector<uint8_t> msg(message_utf8.begin(), message_utf8.end());
    std::vector<uint8_t> script_payload; push_string(script_payload, msg);
    coinbase_script.insert(coinbase_script.end(), script_payload.begin(), script_payload.end());
    push_string(tx, coinbase_script);

    { uint8_t t[4]; write_u32_le(t, 0xffffffff); tx.insert(tx.end(), t, t+4); }

    // NOW 3 OUTPUTS!
    push_varint(tx, 3);

    // Output 0: OP_RETURN (0 DIN)
    { int64_t v=0; uint8_t t[8]; write_i64_le(t, v); tx.insert(tx.end(), t, t+8); }
    {
        auto opret = build_op_return(std::vector<uint8_t>{'D','i','n','e','r','o',' ','G','e','n','e','s','i','s',' ','B','u','r','n'});
        push_string(tx, opret);
    }

    // Output 1: Burn (100K DIN, unspendable)
    {
        int64_t v = (int64_t)(burn_amount_din * units_per_din);
        uint8_t t[8]; write_i64_le(t, v); tx.insert(tx.end(), t, t+8);
        auto burn = build_unspendable_burn_script();
        push_string(tx, burn);
    }

    // Output 2: Premine (1M DIN to developer address)
    {
        int64_t v = (int64_t)(premine_amount_din * units_per_din);
        uint8_t t[8]; write_i64_le(t, v); tx.insert(tx.end(), t, t+8);
        auto spk = bech32_to_scriptpubkey(premine_address);
        push_string(tx, spk);
    }

    { uint8_t t[4]; write_u32_le(t, 0); tx.insert(tx.end(), t, t+4); }

    return hex(tx.data(), tx.size());
}

// ----------------------------- Header serialization -----------------------------

static inline std::vector<uint8_t> serialize_header(uint32_t nVersion,
                                                    const std::string& prevhash_hex,
                                                    const std::string& merkleroot_hex,
                                                    uint32_t nTime, uint32_t nBits, uint32_t nNonce) {
    std::vector<uint8_t> out; out.resize(80);
    write_u32_le(out.data(), nVersion);

    auto prev = from_hex(prevhash_hex);
    if (prev.size()!=32) throw std::runtime_error("prevhash must be 32 bytes hex");
    for (int i=0;i<32;++i) out[4+i] = prev[31-i];

    auto mr = from_hex(merkleroot_hex);
    if (mr.size()!=32) throw std::runtime_error("merkle root must be 32 bytes hex");
    for (int i=0;i<32;++i) out[36+i] = mr[31-i];

    write_u32_le(out.data()+68, nTime);
    write_u32_le(out.data()+72, nBits);
    write_u32_le(out.data()+76, nNonce);
    return out;
}

// ----------------------------- Miner -----------------------------

struct FoundResult {
    bool found = false;
    uint32_t nonce = 0;
    std::string genesis_hash_le;
    std::string header_hex;
};

static void mine_worker(uint32_t start, uint32_t end,
                        uint32_t nVersion, const std::string& merkleroot_hex,
                        uint32_t nTime, uint32_t nBits,
                        const uint8_t target_be[32],
                        std::atomic<bool>& stop,
                        FoundResult& out,
                        std::mutex& out_mu) {
    const std::string prevhash_hex(64, '0');
    uint8_t h32[32];

    for (uint32_t nonce = start; nonce != end && !stop.load(std::memory_order_relaxed); ++nonce) {
        auto header = serialize_header(nVersion, prevhash_hex, merkleroot_hex, nTime, nBits, nonce);
        sha256d(header.data(), header.size(), h32);

        uint8_t h_be[32]; for (int i=0;i<32;++i) h_be[i] = h32[31-i];
        if (leq_256_be(h_be, target_be)) {
            std::lock_guard<std::mutex> lk(out_mu);
            if (!out.found) {
                out.found = true;
                out.nonce = nonce;
                out.header_hex = hex(header.data(), header.size());
                out.genesis_hash_le = hex_le(h32, 32);
                stop.store(true, std::memory_order_relaxed);
            }
            return;
        }

        if ((nonce & 0x7ffff) == 0) {
            // Progress indicator every ~524K hashes
        }
    }
}

// ----------------------------- CLI -----------------------------

static void usage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " --time <unix> --bits <0xNNNNNNNN> [--version 1]\n"
        << "                 [--coinbase-hex <hex>]\n"
        << "                 [--message \"Dinero Genesis...\"] [--burn 100000] [--units 100000000]\n"
        << "                 [--threads <N>] [--start 0] [--end 0xffffffff]\n"
        << "                 [--print-coinbase-only]\n"
        << "\n"
        << "Notes:\n"
        << "  • Provide EITHER --coinbase-hex (preferred) OR --message/--burn to have the tool build a coinbase.\n"
        << "  • Use --print-coinbase-only to just print the coinbase tx hex without mining.\n"
        << "  • Genesis prevhash is all zeroes. Header = version|prev|merkle|time|bits|nonce.\n";
}

int main(int argc, char** argv) {
    uint32_t nTime = 0;
    uint32_t nBits = 0;
    uint32_t nVersion = 1;
    uint32_t start_nonce = 0;
    uint32_t end_nonce = 0xffffffffu;
    unsigned threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;

    std::string coinbase_hex;
    std::string message = "Dinero: 100,000 DIN burned at genesis for network security.";
    uint64_t burn_din = 100000;
    uint64_t units = 100000000;
    bool print_coinbase_only = false;

    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        auto need = [&](const char* flag)->std::string {
            if (i+1>=argc) { usage(argv[0]); std::exit(1); }
            return argv[++i];
        };
        if (a=="--time") nTime = (uint32_t)parse_u64(need(a.c_str()));
        else if (a=="--bits") nBits = read_hex_u32(need(a.c_str()));
        else if (a=="--version") nVersion = (uint32_t)parse_u64(need(a.c_str()));
        else if (a=="--coinbase-hex") coinbase_hex = need(a.c_str());
        else if (a=="--message") message = need(a.c_str());
        else if (a=="--burn") burn_din = parse_u64(need(a.c_str()));
        else if (a=="--units") units = parse_u64(need(a.c_str()));
        else if (a=="--threads") threads = (unsigned)parse_u64(need(a.c_str()));
        else if (a=="--start") start_nonce = (uint32_t)parse_u64(need(a.c_str()));
        else if (a=="--end") end_nonce = (uint32_t)parse_u64(need(a.c_str()));
        else if (a=="--print-coinbase-only") print_coinbase_only = true;
        else { usage(argv[0]); return 1; }
    }

    if (nTime==0 || nBits==0) {
        usage(argv[0]);
        return 1;
    }

    if (coinbase_hex.empty()) {
        coinbase_hex = build_genesis_coinbase_tx_hex(message, burn_din, 1000000,
                                                     "din1q0gqj8ush5026e5q97jkw2mhxg3sngjjlxcpjzn",
                                                     units);
    }
    auto merkleroot_hex = merkle_root_from_txs_hex(std::vector<std::string>{coinbase_hex});

    // If --print-coinbase-only, just output and exit
    if (print_coinbase_only) {
        std::cout << "==== COINBASE TX (without mining) ====\n";
        std::cout << "Coinbase hex: " << coinbase_hex << "\n";
        std::cout << "Merkle root:  " << merkleroot_hex << "\n";
        std::cout << "TXID:         " << merkleroot_hex << " (same as merkle for single tx)\n";
        return 0;
    }

    uint8_t target_be[32]; compact_to_target(nBits, target_be);

    std::cout << "[i] Config\n";
    std::cout << "    version   : " << nVersion << "\n";
    std::cout << "    time      : " << nTime << "\n";
    std::cout << "    bits      : 0x" << std::hex << std::setw(8) << std::setfill('0') << nBits << std::dec << "\n";
    std::cout << "    threads   : " << threads << "\n";
    std::cout << "    start..end: " << start_nonce << " .. " << end_nonce << "\n";
    std::cout << "    merkle    : " << merkleroot_hex << "\n";
    std::cout << "    coinbase  : " << (coinbase_hex.size()>80 ? (coinbase_hex.substr(0,80)+"...") : coinbase_hex) << "\n";

    std::atomic<bool> stop(false);
    FoundResult found;
    std::mutex found_mu;

    uint64_t total = (uint64_t)end_nonce - (uint64_t)start_nonce + 1;
    uint64_t per = total / threads;
    uint32_t cur = start_nonce;

    std::vector<std::thread> pool;
    pool.reserve(threads);

    auto t0 = std::chrono::steady_clock::now();

    for (unsigned t=0; t<threads; ++t) {
        uint32_t s = cur;
        uint32_t e = (t == threads-1) ? end_nonce : (uint32_t)( (uint64_t)cur + per - 1 );
        cur = (uint32_t)((uint64_t)cur + per);
        pool.emplace_back(mine_worker, s, e,
                          nVersion, merkleroot_hex, nTime, nBits,
                          target_be, std::ref(stop), std::ref(found), std::ref(found_mu));
    }
    for (auto& th : pool) th.join();

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    if (!found.found) {
        std::cerr << "[!] No valid nonce found in range.\n";
        return 2;
    }

    std::cout << "\n==== GENESIS RESULTS ====\n";
    std::cout << "Version        : " << nVersion << "\n";
    std::cout << "Time (nTime)   : " << nTime << "\n";
    std::cout << "Bits (nBits)   : 0x" << std::hex << std::setw(8) << std::setfill('0') << nBits << std::dec << "\n";
    std::cout << "Nonce (nNonce) : " << found.nonce << "\n";
    std::cout << "Merkle Root    : " << merkleroot_hex << "\n";
    std::cout << "Genesis Hash   : " << found.genesis_hash_le << "\n";
    std::cout << "Header (hex)   : " << found.header_hex << "\n";
    std::cout << "\n[i] Elapsed " << std::fixed << std::setprecision(2) << sec << "s\n";

    return 0;
}
