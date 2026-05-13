#pragma once
#include <string>
#include <optional>
#include <filesystem>

namespace dinero::cli {

struct ConnInput {
    // Optional inputs from flags / nodeinfo / defaults
    std::optional<std::string> rpc_url_flag;
    std::optional<std::string> cookie_file_flag;
    
    std::optional<std::string> nodeinfo_path;
    std::optional<std::string> nodeinfo_rpc_url;
    std::optional<std::string> nodeinfo_cookie_path;
    std::optional<std::string> nodeinfo_cookie_literal; // only used with --accept-insecure-cookie
    
    std::optional<std::string> platform_default_nodeinfo; // resolved path or nullopt
    
    bool accept_insecure_cookie = false;
    bool no_nodeinfo = false; // --no-nodeinfo flag
};

struct ConnResolved {
    std::string rpc_url;
    std::string cookie_path;          // preferred
    std::optional<std::string> cookie_literal; // only if insecure allowed
    enum class Source { Flags, Nodeinfo, Mixed } source;
    
    // Detailed provenance for verbose output
    std::string rpc_url_source;
    std::string cookie_source;
    std::string discovery_method;
    
    // Connection summary for non-verbose output
    std::string connection_summary;
};

/**
 * Resolves connection details from flags, nodeinfo, and defaults.
 * 
 * Algorithm (deterministic and small):
 * 
 * RPC URL:
 *   if rpc_url_flag → use it
 *   else if nodeinfo_rpc_url → use it  
 *   else → error "missing rpc.url"
 * 
 * Cookie:
 *   if cookie_file_flag → use it
 *   else if nodeinfo_cookie_path → use it
 *   else if nodeinfo_cookie_literal and accept_insecure_cookie → use literal
 *   else → error "missing cookie path"
 * 
 * Nodeinfo search order (build the ConnInput before resolve):
 *   if --nodeinfo PATH given → only read that (do not hit defaults)
 *   else try OS default
 *   else skip
 */
ConnResolved resolve(const ConnInput& input);

} // namespace dinero::cli
