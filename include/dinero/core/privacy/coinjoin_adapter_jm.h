#pragma once
#include "privacy/coinjoin_adapter.h"
#include <string>

namespace din {

class CoinJoinAdapterJM : public ICoinJoinAdapter {
public:
    explicit CoinJoinAdapterJM(std::string base_url);
    
    std::string register_round(const CJJoinParams& p) override;
    void submit_inputs(const std::string& rid,
                      const std::vector<CJInputLite>& ins,
                      const std::string& equal_spk_hex,
                      const std::string& change_spk_hex,
                      int64_t feerate_una_vb) override;
    CJStatusLite status(const std::string& rid) override;
    std::string fetch_psbt(const std::string& rid) override;
    void submit_signed(const std::string& rid, const std::string& psbt_b64) override;
    void cancel(const std::string& rid) override;

private:
    std::string base_url_;
    std::string http_post_json(const std::string& path, const std::string& body);
    std::string http_get(const std::string& path);
};

} // namespace din
