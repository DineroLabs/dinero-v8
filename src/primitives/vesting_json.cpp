#include "primitives/vesting.h"
#include "compat/jsoncpp_compat.h"

namespace dinero::json {

Json::Value toJson(const VestingSchedule& vs) {
    Json::Value json;
    Json::Value tranchesArray(Json::arrayValue);
    
    for (const auto& tranche : vs.tranches) {
        Json::Value trancheJson;
        trancheJson["unlock_height"] = static_cast<unsigned int>(tranche.unlock_height);
        trancheJson["amount_sats"] = static_cast<uint64_t>(tranche.amount_sats);
        tranchesArray.append(trancheJson);
    }
    
    json["tranches"] = tranchesArray;
    json["total"] = static_cast<uint64_t>(vs.total());
    
    return json;
}

bool fromJson(const Json::Value& v, VestingSchedule& out) {
    if (!v.isObject()) return false;
    
    out.tranches.clear();
    
    const Json::Value& tranchesArray = v["tranches"];
    if (!tranchesArray.isArray()) return false;
    
    for (const auto& trancheJson : tranchesArray) {
        if (!trancheJson.isObject()) return false;
        
        VestingTranche tranche;
        tranche.unlock_height = trancheJson.isMember("unlock_height") ? "unlock_height" : 0;
        tranche.amount_sats = trancheJson.isMember("amount_sats") ? "amount_sats" : 0;
        
        out.tranches.push_back(tranche);
    }
    
    return true;
}

} // namespace dinero::json
