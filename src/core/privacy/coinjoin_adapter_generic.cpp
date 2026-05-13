#include "privacy/coinjoin_adapter_generic.h"
#ifdef __APPLE__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core.hpp>
#include <sstream>
#include <stdexcept>

namespace din {
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

CoinJoinAdapterGeneric::CoinJoinAdapterGeneric(std::string base_url) : base_url_(std::move(base_url)) {}

std::string CoinJoinAdapterGeneric::register_round(const CJJoinParams& p) {
    Json::Value req;
    req["amount"] = Json::Int64(p.amount);
    req["feerate"] = Json::Int64(p.feerate_una_vb);
    req["min_peers"] = p.min_peers;
    req["policy"] = p.policy;
    
    std::string body = Json::writeString(Json::StreamWriterBuilder{}, req);
    std::string response = http_post_json("/register", body);
    
    Json::CharReaderBuilder reader_builder;
    Json::Value resp;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
    
    if (!reader->parse(response.data(), response.data() + response.size(), &resp, &errors)) {
        throw std::runtime_error("Failed to parse register response: " + errors);
    }
    
    if (!resp.isMember("round_id")) {
        throw std::runtime_error("Missing round_id in register response");
    }
    
    return resp["round_id"].asString();
}

void CoinJoinAdapterGeneric::submit_inputs(const std::string& rid,
                                          const std::vector<CJInputLite>& ins,
                                          const std::string& equal_spk_hex,
                                          const std::string& change_spk_hex,
                                          int64_t feerate_una_vb) {
    Json::Value req;
    req["round_id"] = rid;
    req["feerate"] = Json::Int64(feerate_una_vb);
    req["equal_spk"] = equal_spk_hex;
    req["change_spk"] = change_spk_hex;
    
    Json::Value inputs_array(Json::arrayValue);
    for (const auto& input : ins) {
        Json::Value input_obj;
        input_obj["txid"] = input.txid;
        input_obj["vout"] = input.vout;
        input_obj["value"] = Json::Int64(input.value);
        inputs_array.append(input_obj);
    }
    req["inputs"] = inputs_array;
    
    std::string body = Json::writeString(Json::StreamWriterBuilder{}, req);
    http_post_json("/inputs", body);
}

CJStatusLite CoinJoinAdapterGeneric::status(const std::string& rid) {
    std::string response = http_get("/status?round_id=" + rid);
    
    Json::CharReaderBuilder reader_builder;
    Json::Value resp;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
    
    if (!reader->parse(response.data(), response.data() + response.size(), &resp, &errors)) {
        throw std::runtime_error("Failed to parse status response: " + errors);
    }
    
    CJStatusLite status;
    status.phase = resp.get("phase", "unknown").asString();
    status.peers = resp.get("peers", 0).asInt();
    status.detail = resp.get("detail", "").asString();
    status.txid = resp.get("txid", "").asString();
    
    return status;
}

std::string CoinJoinAdapterGeneric::fetch_psbt(const std::string& rid) {
    std::string response = http_get("/psbt?round_id=" + rid);
    
    Json::CharReaderBuilder reader_builder;
    Json::Value resp;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
    
    if (!reader->parse(response.data(), response.data() + response.size(), &resp, &errors)) {
        throw std::runtime_error("Failed to parse psbt response: " + errors);
    }
    
    if (!resp.isMember("psbt")) {
        throw std::runtime_error("Missing psbt in response");
    }
    
    return resp["psbt"].asString();
}

void CoinJoinAdapterGeneric::submit_signed(const std::string& rid, const std::string& psbt_b64) {
    Json::Value req;
    req["round_id"] = rid;
    req["psbt"] = psbt_b64;
    
    std::string body = Json::writeString(Json::StreamWriterBuilder{}, req);
    http_post_json("/submit", body);
}

void CoinJoinAdapterGeneric::cancel(const std::string& rid) {
    Json::Value req;
    req["round_id"] = rid;
    
    std::string body = Json::writeString(Json::StreamWriterBuilder{}, req);
    http_post_json("/cancel", body);
}

std::string CoinJoinAdapterGeneric::http_post_json(const std::string& path, const std::string& body) {
    try {
        boost::asio::io_context ioc;
        tcp::resolver resolver(ioc);
        tcp::socket socket(ioc);
        
        // Parse URL
        std::string host = base_url_.substr(7); // Remove "http://"
        size_t slash_pos = host.find('/');
        if (slash_pos != std::string::npos) {
            host = host.substr(0, slash_pos);
        }
        
        // Resolve and connect
        auto const results = resolver.resolve(host, "80");
        boost::asio::connect(socket, results.begin(), results.end());
        
        // Prepare request
        http::request<http::string_body> req{http::verb::post, path, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "DineroCoinJoinClient/1.0");
        req.set(http::field::content_type, "application/json");
        req.set(http::field::content_length, std::to_string(body.length()));
        req.body() = body;
        
        // Send request
        http::write(socket, req);
        
        // Read response
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);
        
        return res.body();
    } catch (const std::exception& e) {
        throw std::runtime_error("HTTP POST failed: " + std::string(e.what()));
    }
}

std::string CoinJoinAdapterGeneric::http_get(const std::string& path) {
    try {
        boost::asio::io_context ioc;
        tcp::resolver resolver(ioc);
        tcp::socket socket(ioc);
        
        // Parse URL
        std::string host = base_url_.substr(7); // Remove "http://"
        size_t slash_pos = host.find('/');
        if (slash_pos != std::string::npos) {
            host = host.substr(0, slash_pos);
        }
        
        // Resolve and connect
        auto const results = resolver.resolve(host, "80");
        boost::asio::connect(socket, results.begin(), results.end());
        
        // Prepare request
        http::request<http::empty_body> req{http::verb::get, path, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "DineroCoinJoinClient/1.0");
        
        // Send request
        http::write(socket, req);
        
        // Read response
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);
        
        return res.body();
    } catch (const std::exception& e) {
        throw std::runtime_error("HTTP GET failed: " + std::string(e.what()));
    }
}

} // namespace din