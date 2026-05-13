#include "address_validate.hpp"
#include "external/bech32/bech32.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cctype>

static std::string hex(const std::vector<uint8_t>& v){
    std::ostringstream o; o<<std::hex<<std::nouppercase<<std::setfill('0');
    for (auto b: v) o<<std::setw(2)<<(int)b; return o.str();
}
static bool convertbits(std::vector<uint8_t>& out, int outbits,
                        const std::vector<uint8_t>& in, int inbits, bool pad){
    uint32_t acc=0; int bits=0; uint32_t maxv=(1u<<outbits)-1u;
    for(uint8_t v: in){ if((v>>inbits)!=0) return false; acc=(acc<<inbits)|v; bits+=inbits;
        while(bits>=outbits){ bits-=outbits; out.push_back((acc>>bits)&maxv); } }
    if(pad){ if(bits) out.push_back((acc<<(outbits-bits))&maxv); }
    else if(bits>=inbits || ((acc<<(outbits-bits))&maxv)) return false;
    return true;
}
static uint8_t op_n(int n){ return n==0 ? 0x00 : uint8_t(0x50+n); }

bool IsValidDnrAddress(const std::string& addr, DnrAddressInfo& out){
    out = DnrAddressInfo{};
    if (addr.empty()) { out.error="empty"; return false; }

    bool lo=false, up=false; 
    for(char c: addr){ 
        if(std::isalpha((unsigned char)c)){ 
            lo|=std::islower((unsigned char)c); 
            up|=std::isupper((unsigned char)c);
        } 
    }
    if (lo && up) { out.error="mixed case"; return false; }

    // Try to decode using the project's bech32 implementation
    auto decoded = bech32::Decode("din", addr);
    if (!decoded.has_value()) {
        out.error="bech32 decode failed"; 
        return false; 
    }

    int v = decoded->witver;
    std::vector<uint8_t> prog = decoded->program;

    if (v < 0 || v > 16) { out.error="invalid witness version"; return false; }

    // Validate program size
    if (v == 0){
        if (!(prog.size()==20 || prog.size()==32)) { out.error="v0 program length"; return false; }
    } else {
        if (prog.size() < 2 || prog.size() > 40) { out.error="v>=1 program length"; return false; }
    }

    std::vector<uint8_t> spk; spk.reserve(2+prog.size());
    spk.push_back(op_n(v));
    spk.push_back(uint8_t(prog.size()));
    spk.insert(spk.end(), prog.begin(), prog.end());

    out.ok = true; out.witness_version = v; out.program = std::move(prog);
    out.program_hex = hex(out.program);
    out.script_pubkey_hex = hex(spk);
    if (v==0 && out.program.size()==20)      out.type="witness_v0_keyhash";
    else if (v==0 && out.program.size()==32) out.type="witness_v0_scripthash";
    else                                     out.type="witness_unknown";
    return true;
}
