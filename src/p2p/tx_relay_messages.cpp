#include "p2p/tx_relay_manager.h"
#include "common/serialization.h"
#include "compat/jsoncpp_compat.h"
#include <sstream>

namespace dinero {
namespace p2p_messages {

// InvMessage implementation
std::string InvMessage::Serialize() const {
    din::Json json = din::obj();
    json["type"] = "inv";
    
    din::Json inv_array = din::arr();
    for (const auto& inv : inventory) {
        din::Json inv_obj = din::obj();
        inv_obj["type"] = static_cast<uint32_t>(inv.type);
        inv_obj["hash"] = inv.hash;
        inv_array.append(inv_obj);
    }
    json["inventory"] = inv_array;
    
    return json.toStyledString();
}

bool InvMessage::Deserialize(const std::string& data) {
    try {
        din::Json json;
        std::istringstream stream(data);
        stream >> json;
        
        if (!json.isMember("type") || json["type"].asString() != "inv") {
            return false;
        }
        
        if (!json.isMember("inventory") || !json["inventory"].isArray()) {
            return false;
        }
        
        inventory.clear();
        for (const auto& inv_obj : json["inventory"]) {
            if (!inv_obj.isMember("type") || !inv_obj.isMember("hash")) {
                return false;
            }
            
            auto type = static_cast<P2PMessageType>(inv_obj["type"].asUInt());
            std::string hash = inv_obj["hash"].asString();
            
            inventory.emplace_back(type, hash);
        }
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// GetDataMessage implementation
std::string GetDataMessage::Serialize() const {
    din::Json json = din::obj();
    json["type"] = "getdata";
    
    din::Json inv_array = din::arr();
    for (const auto& inv : inventory) {
        din::Json inv_obj = din::obj();
        inv_obj["type"] = static_cast<uint32_t>(inv.type);
        inv_obj["hash"] = inv.hash;
        inv_array.append(inv_obj);
    }
    json["inventory"] = inv_array;
    
    return json.toStyledString();
}

bool GetDataMessage::Deserialize(const std::string& data) {
    try {
        din::Json json;
        std::istringstream stream(data);
        stream >> json;
        
        if (!json.isMember("type") || json["type"].asString() != "getdata") {
            return false;
        }
        
        if (!json.isMember("inventory") || !json["inventory"].isArray()) {
            return false;
        }
        
        inventory.clear();
        for (const auto& inv_obj : json["inventory"]) {
            if (!inv_obj.isMember("type") || !inv_obj.isMember("hash")) {
                return false;
            }
            
            auto type = static_cast<P2PMessageType>(inv_obj["type"].asUInt());
            std::string hash = inv_obj["hash"].asString();
            
            inventory.emplace_back(type, hash);
        }
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// TxMessage implementation
std::string TxMessage::Serialize() const {
    din::Json json = din::obj();
    json["type"] = "tx";
    json["txid"] = transaction.GetTxId();
    json["transaction"] = transaction.Serialize();
    
    return json.toStyledString();
}

bool TxMessage::Deserialize(const std::string& data) {
    try {
        din::Json json;
        std::istringstream stream(data);
        stream >> json;
        
        if (!json.isMember("type") || json["type"].asString() != "tx") {
            return false;
        }
        
        if (!json.isMember("transaction")) {
            return false;
        }
        
        // For now, just store the serialized data
        // Real implementation would deserialize the transaction
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// FeeFilterMessage implementation
std::string FeeFilterMessage::Serialize() const {
    din::Json json = din::obj();
    json["type"] = "feefilter";
    json["feerate"] = static_cast<din::Json::Int64>(feerate);
    
    return json.toStyledString();
}

bool FeeFilterMessage::Deserialize(const std::string& data) {
    try {
        din::Json json;
        std::istringstream stream(data);
        stream >> json;
        
        if (!json.isMember("type") || json["type"].asString() != "feefilter") {
            return false;
        }
        
        if (!json.isMember("feerate")) {
            return false;
        }
        
        feerate = json["feerate"].asUInt64();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// NotFoundMessage implementation
std::string NotFoundMessage::Serialize() const {
    din::Json json = din::obj();
    json["type"] = "notfound";
    
    din::Json inv_array = din::arr();
    for (const auto& inv : inventory) {
        din::Json inv_obj = din::obj();
        inv_obj["type"] = static_cast<uint32_t>(inv.type);
        inv_obj["hash"] = inv.hash;
        inv_array.append(inv_obj);
    }
    json["inventory"] = inv_array;
    
    return json.toStyledString();
}

bool NotFoundMessage::Deserialize(const std::string& data) {
    try {
        din::Json json;
        std::istringstream stream(data);
        stream >> json;
        
        if (!json.isMember("type") || json["type"].asString() != "notfound") {
            return false;
        }
        
        if (!json.isMember("inventory") || !json["inventory"].isArray()) {
            return false;
        }
        
        inventory.clear();
        for (const auto& inv_obj : json["inventory"]) {
            if (!inv_obj.isMember("type") || !inv_obj.isMember("hash")) {
                return false;
            }
            
            auto type = static_cast<P2PMessageType>(inv_obj["type"].asUInt());
            std::string hash = inv_obj["hash"].asString();
            
            inventory.emplace_back(type, hash);
        }
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace p2p_messages
} // namespace dinero
