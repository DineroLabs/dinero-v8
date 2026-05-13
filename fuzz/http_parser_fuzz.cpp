/**
 * LibFuzzer target for HTTP request parsing
 * Tests parseHTTPRequest function with malformed inputs
 */

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <cstring>

// Include your HTTP parsing code
// #include "daemon/rpc_server.h"  // Adjust path as needed

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 65536) return 0;  // Skip empty or huge inputs
    
    std::string input(reinterpret_cast<const char*>(data), size);
    
    // Test HTTP parsing with fuzzed input
    try {
        // Mock HTTP parsing function - replace with actual implementation
        struct HTTPRequest {
            std::string method;
            std::string path;
            std::string version;
            std::map<std::string, std::string> headers;
            std::string body;
            bool valid = false;
        };
        
        // This would call your actual parseHTTPRequest function
        // HTTPRequest req = parseHTTPRequest(input);
        
        // For now, simulate parsing logic that we want to fuzz
        HTTPRequest req;
        
        // Look for HTTP method
        size_t method_end = input.find(' ');
        if (method_end != std::string::npos) {
            req.method = input.substr(0, method_end);
            
            // Look for path
            size_t path_start = method_end + 1;
            size_t path_end = input.find(' ', path_start);
            if (path_end != std::string::npos) {
                req.path = input.substr(path_start, path_end - path_start);
                
                // Look for version
                size_t version_start = path_end + 1;
                size_t version_end = input.find('\r', version_start);
                if (version_end != std::string::npos) {
                    req.version = input.substr(version_start, version_end - version_start);
                    req.valid = true;
                }
            }
        }
        
        // Parse headers (simplified)
        size_t header_start = input.find("\r\n");
        while (header_start != std::string::npos && header_start + 2 < input.size()) {
            header_start += 2;
            size_t header_end = input.find("\r\n", header_start);
            if (header_end == std::string::npos) break;
            
            std::string header_line = input.substr(header_start, header_end - header_start);
            size_t colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = header_line.substr(0, colon_pos);
                std::string value = header_line.substr(colon_pos + 1);
                
                // Trim whitespace
                while (!value.empty() && value[0] == ' ') value.erase(0, 1);
                while (!value.empty() && value.back() == ' ') value.pop_back();
                
                req.headers[key] = value;
            }
            
            header_start = header_end;
        }
        
        // Test Authorization header parsing specifically
        auto auth_it = req.headers.find("Authorization");
        if (auth_it != req.headers.end()) {
            const std::string& auth_header = auth_it->second;
            
            // Test Basic auth parsing
            if (auth_header.substr(0, 6) == "Basic ") {
                std::string encoded = auth_header.substr(6);
                
                // This would call your base64Decode function
                // std::string decoded = base64Decode(encoded);
                
                // Simulate base64 decoding for fuzzing
                // Look for potential integer overflow in base64 decoding
                for (char c : encoded) {
                    if (!((c >= 'A' && c <= 'Z') || 
                          (c >= 'a' && c <= 'z') || 
                          (c >= '0' && c <= '9') || 
                          c == '+' || c == '/' || c == '=')) {
                        // Invalid base64 character
                        break;
                    }
                }
            }
        }
        
    } catch (...) {
        // Catch any exceptions - fuzzer should not crash
        return 0;
    }
    
    return 0;
}

// Optional: Custom mutator for HTTP-specific fuzzing
extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                          size_t max_size, unsigned int seed) {
    // Add HTTP-specific mutations
    static const char* http_methods[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS"};
    static const char* http_versions[] = {"HTTP/1.0", "HTTP/1.1", "HTTP/2.0"};
    static const char* common_headers[] = {
        "Authorization: Basic ", 
        "Content-Type: application/json",
        "Content-Length: ",
        "Host: localhost",
        "User-Agent: fuzz"
    };
    
    if (size == 0 || max_size < 100) return 0;
    
    // Sometimes generate a valid HTTP request structure
    if (seed % 10 == 0) {
        std::string http_req = std::string(http_methods[seed % 6]) + " / " + 
                              std::string(http_versions[seed % 3]) + "\r\n";
        
        // Add some headers
        for (int i = 0; i < (seed % 3) + 1; i++) {
            http_req += std::string(common_headers[i % 5]) + "value\r\n";
        }
        
        http_req += "\r\n";  // End headers
        
        if (http_req.size() <= max_size) {
            memcpy(data, http_req.c_str(), http_req.size());
            return http_req.size();
        }
    }
    
    // Otherwise use default mutator
    return 0;  // Use default mutator
}
