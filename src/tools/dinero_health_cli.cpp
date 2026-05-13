#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <iomanip>

using json = nlohmann::json;

// Temporary compatibility alias for easier migration
// JSON compatibility restored

class DineroHealthCLI {
private:
    std::string rpc_url;
    std::string cookie_path;
    std::string cookie_token;
    
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
        size_t totalSize = size * nmemb;
        s->append((char*)contents, totalSize);
        return totalSize;
    }
    
    std::string readCookie() {
        std::ifstream file(cookie_path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot read cookie file: " + cookie_path);
        }
        std::string token;
        std::getline(file, token);
        return token;
    }
    
    json rpcCall(const std::string& method, const json& params = json::object()) {
        CURL* curl;
        CURLcode res;
        std::string response;
        
        curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize curl");
        }
        
        // Build JSON-RPC request
        json request = {
            {"jsonrpc", "2.0"},
            {"method", method},
            {"params", params},
            {"id", 1}
        };
        
        const std::string jsonRequest = request.dump();
        
        // Set up curl
        curl_easy_setopt(curl, CURLOPT_URL, rpc_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonRequest.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, jsonRequest.length());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        
        // Set headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // Set authentication
        curl_easy_setopt(curl, CURLOPT_USERPWD, cookie_token.c_str());
        
        // Perform request
        res = curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            throw std::runtime_error("Curl error: " + std::string(curl_easy_strerror(res)));
        }
        
        // Parse response
        json result = json::parse(response);
        
        if (result.contains("error") && !result["error"].is_null()) {
            json error_obj = result["error"];
            std::string error_msg = error_obj.contains("message") ? error_obj["message"].get<std::string>() : "Unknown error";
            throw std::runtime_error("RPC error: " + error_msg);
        }
        
        return result["result"];
    }
    
public:
    DineroHealthCLI(const std::string& url, const std::string& cookie) 
        : rpc_url(url), cookie_path(cookie) {
        cookie_token = readCookie();
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    
    ~DineroHealthCLI() {
        curl_global_cleanup();
    }
    
    void displayHealth() {
        try {
            auto result = rpcCall("getmininginfo");
            
            std::cout << "\033[2J\033[H"; // Clear screen
            std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                    DINERO HEALTH DASHBOARD                   ║\n";
            std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
            
            std::cout << "║ Height: " << std::setw(8) << result.value("blocks", 0) 
                      << " │ Difficulty: " 
                      << result.value("difficulty", 0.0) << " ║\n";
            std::cout << "║ Hashrate: " 
                      << result.value("hashrate_hps", 0.0) << " H/s │ Target: " 
                      << result.value("target", std::string("unknown")).substr(0, 8) << "... ║\n";
            
            std::cout << "║ Mining: " << (result.value("mining_enabled", false) ? "ENABLED " : "DISABLED") 
                      << " │ Network: " << (result.value("testnet", false) ? "TESTNET" : "MAINNET") << " ║\n";
            
            std::cout << "║ Address: " << result.value("mining_address", std::string("none")).substr(0, 20) << "... ║\n";
            
            std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
            std::cout << "Updated: " << std::chrono::system_clock::now().time_since_epoch().count() / 1000000 << "ms\n";
            
        } catch (const std::exception& e) {
            std::cout << "\033[2J\033[H"; // Clear screen
            std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                    DINERO HEALTH DASHBOARD                   ║\n";
            std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
            std::cout << "║ ❌ ERROR: " << std::setw(50) << e.what() << " ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        }
    }
    
    void startMonitoring(int interval_seconds = 2) {
        std::cout << "Starting Dinero Health Monitor (Ctrl+C to stop)...\n";
        while (true) {
            displayHealth();
            std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
        }
    }
};

int main(int argc, char* argv[]) {
    std::string rpc_url = "http://127.0.0.1:20998";
    std::string cookie_path = "/Users/haydarevich/Documents/Dinero/data/mainnet/.cookie";
    int interval = 2;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--rpc" && i + 1 < argc) {
            rpc_url = argv[++i];
        } else if (arg == "--cookie" && i + 1 < argc) {
            cookie_path = argv[++i];
        } else if (arg == "--interval" && i + 1 < argc) {
            interval = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --rpc URL        RPC URL (default: http://127.0.0.1:20998)\n";
            std::cout << "  --cookie PATH    Cookie file path\n";
            std::cout << "  --interval SEC   Update interval (default: 2)\n";
            std::cout << "  --help           Show this help\n";
            return 0;
        }
    }
    
    try {
        DineroHealthCLI dashboard(rpc_url, cookie_path);
        dashboard.startMonitoring(interval);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
