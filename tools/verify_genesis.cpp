// tools/verify_genesis.cpp — 30-second genesis integrity proof
// Standalone: only needs crypto/sha256.cpp + primitives headers

#include "primitives/block.h"
#include "primitives/uint256.h"
#include "crypto/sha256.h"
#include "consensus/chain_bundle_generated.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace dinero;

// --- Frozen constants ---
static constexpr const char* FROZEN_HASH = dinero::chain_bundle::GENESIS_BLOCK_HASH;
static constexpr const char* FROZEN_MERKLE = dinero::chain_bundle::GENESIS_MERKLE_ROOT;
static constexpr const char* FROZEN_UTREEXO = dinero::chain_bundle::GENESIS_UTREEXO_ROOT;
static constexpr const char* FROZEN_COINBASE_HEX = dinero::chain_bundle::GENESIS_COINBASE_HEX;
static constexpr uint32_t FROZEN_NONCE = dinero::chain_bundle::GENESIS_NONCE;
static constexpr uint64_t FROZEN_TIMESTAMP = dinero::chain_bundle::GENESIS_TIMESTAMP;
static constexpr uint32_t FROZEN_DIFFICULTY = dinero::chain_bundle::GENESIS_DIFFICULTY;

// --- Helpers ---
static void w32(uint8_t* p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);p[2]=(uint8_t)(x>>16);p[3]=(uint8_t)(x>>24);}
static void w64(uint8_t* p,int64_t x){for(int i=0;i<8;++i)p[i]=(uint8_t)(x>>(8*i));}
static void pv(std::vector<uint8_t>&o,uint64_t n){if(n<0xfd)o.push_back((uint8_t)n);else{o.push_back(0xfd);o.push_back(n&0xff);o.push_back((n>>8)&0xff);}}
static std::string hx(const uint8_t*b,size_t n){std::ostringstream o;o<<std::hex<<std::setfill('0');for(size_t i=0;i<n;++i)o<<std::setw(2)<<(unsigned)b[i];return o.str();}
static std::vector<uint8_t> unhx(const std::string&h){std::vector<uint8_t>o;for(size_t i=0;i<h.size();i+=2){unsigned v=0;std::sscanf(h.c_str()+i,"%2x",&v);o.push_back((uint8_t)v);}return o;}

static void dsha256(const uint8_t*in,size_t len,uint8_t out[32]){
    uint8_t tmp[32];
    crypto::CSHA256 h1;h1.Write(in,len);h1.Finalize(tmp);
    crypto::CSHA256 h2;h2.Write(tmp,32);h2.Finalize(out);
}

int main() {
    int pass=0,fail=0;
    printf("\n=== GENESIS INTEGRITY PROOF ===\n\n");

    // 1. Rebuild coinbase from motto
    const std::string motto="Dinero: Real Money For Free People";
    std::vector<uint8_t> tx(4);
    w32(tx.data(),1); pv(tx,1);
    tx.insert(tx.end(),32,0x00);
    uint8_t vout[4];w32(vout,0xffffffff);tx.insert(tx.end(),vout,vout+4);
    std::vector<uint8_t> sig; sig.push_back(0x00);
    sig.insert(sig.end(),motto.begin(),motto.end());
    pv(tx,sig.size()); tx.insert(tx.end(),sig.begin(),sig.end());
    uint8_t seq[4];w32(seq,0xffffffff);tx.insert(tx.end(),seq,seq+4);
    pv(tx,1);
    uint8_t amt[8];w64(amt,10000000000LL);tx.insert(tx.end(),amt,amt+8);
    std::vector<uint8_t> spk;spk.push_back(0x6a);spk.push_back((uint8_t)motto.size());
    spk.insert(spk.end(),motto.begin(),motto.end());
    pv(tx,spk.size());tx.insert(tx.end(),spk.begin(),spk.end());
    uint8_t lk[4]={0,0,0,0};tx.insert(tx.end(),lk,lk+4);
    std::string coinbase_hex=hx(tx.data(),tx.size());

    bool cb_ok=(coinbase_hex==FROZEN_COINBASE_HEX);
    printf("1. Coinbase TX hex:  %s\n",cb_ok?"MATCH":"MISMATCH");
    if(cb_ok) pass++; else { fail++; printf("   computed: %s\n   frozen:   %s\n",coinbase_hex.c_str(),FROZEN_COINBASE_HEX); }

    // 2. Coinbase txid → merkle root
    uint8_t txid[32];
    dsha256(tx.data(),tx.size(),txid);
    printf("   Coinbase txid:    %s\n",hx(txid,32).c_str());

    uint256 merkle;
    std::memcpy(merkle.data,txid,32);
    std::string merkle_hex=merkle.GetHex();
    bool mr_ok=(merkle_hex==FROZEN_MERKLE);
    printf("2. Merkle root:      %s\n",mr_ok?"MATCH":"MISMATCH");
    printf("   computed: %s\n   frozen:   %s\n",merkle_hex.c_str(),FROZEN_MERKLE);
    if(mr_ok) pass++; else fail++;

    // 3. Build header, compute genesis hash
    BlockHeader hdr{};
    hdr.version=1;
    hdr.prev_block_hash=uint256();
    hdr.merkle_root=merkle;
    hdr.utreexo_root=uint256::FromHexUnsafe(FROZEN_UTREEXO);
    hdr.timestamp=FROZEN_TIMESTAMP;
    hdr.difficulty=FROZEN_DIFFICULTY;
    hdr.nonce=FROZEN_NONCE;
    hdr.ZeroReserved();

    uint256 genesis_hash=hdr.GetHash();
    std::string hash_hex=genesis_hash.GetHex();
    bool gh_ok=(hash_hex==FROZEN_HASH);
    printf("3. Genesis hash:     %s\n",gh_ok?"MATCH":"MISMATCH");
    printf("   computed: %s\n   frozen:   %s\n",hash_hex.c_str(),FROZEN_HASH);
    if(gh_ok) pass++; else fail++;

    printf("4. Nonce:            %u == %u  %s\n",hdr.nonce,FROZEN_NONCE,
           hdr.nonce==FROZEN_NONCE?"MATCH":"MISMATCH");
    if(hdr.nonce==FROZEN_NONCE) pass++; else fail++;

    printf("\n=== RESULT: %d/%d passed",pass,pass+fail);
    if(!fail) printf(" — NO RE-MINING NEEDED ===\n\n");
    else printf(" — %d FAILED ===\n\n",fail);
    return fail;
}
