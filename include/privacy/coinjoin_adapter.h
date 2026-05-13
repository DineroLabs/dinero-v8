#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace din {

struct CJInputLite { 
    std::string txid; 
    uint32_t vout; 
    int64_t value; 
};

struct CJJoinParams {
    std::string base_url;
    int64_t     amount;        // equal-output value
    int64_t     feerate_una_vb;
    int         min_peers;
    std::string policy;        // optional: "fast" | "cheap" | "anon"
};

struct CJStatusLite {
    std::string phase;   // collect|psbt|sign|submitted|done|fail
    int         peers{0};
    std::string detail;
    std::string txid;    // when done
};

class ICoinJoinAdapter {
public:
    virtual ~ICoinJoinAdapter() = default;
    virtual std::string register_round(const CJJoinParams& p) = 0;
    virtual void        submit_inputs(const std::string& rid,
                                    const std::vector<CJInputLite>& ins,
                                    const std::string& equal_spk_hex,
                                    const std::string& change_spk_hex,
                                    int64_t feerate_una_vb) = 0;
    virtual CJStatusLite status(const std::string& rid) = 0;
    virtual std::string  fetch_psbt(const std::string& rid) = 0;
    virtual void        submit_signed(const std::string& rid, const std::string& psbt_b64) = 0;
    virtual void        cancel(const std::string& rid) = 0;
};

} // namespace din
