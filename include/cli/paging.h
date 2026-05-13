#pragma once
#include "compat/jsoncpp_compat.h"
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace dinero::cli {

// Forward declaration
struct CliOverrides;

struct PagingOptions {
    int limit = 100;                          // Default 100, max 5000
    int offset = 0;                           // Start from this index
    std::optional<std::string> cursor;        // Opaque pagination token
    std::string filter;                       // Pattern to match (case-insensitive substring)
    std::optional<std::string> since;         // ISO8601 timestamp or block height
    std::optional<std::string> until;         // ISO8601 timestamp or block height
    bool all = false;                         // Bypass limits (JSON only)
    
    // Command-specific filters
    std::optional<int> minConf;
    std::optional<std::string> address;
    std::optional<std::string> txType;
    std::optional<std::string> label;
    std::optional<double> minAmount;
    std::optional<double> maxAmount;
    bool confirmedOnly = false;
    std::optional<std::string> state;
    std::optional<int> minVersion;
    std::optional<double> minFeeRate;
    std::optional<std::string> txid;
};

struct PagingResult {
    Json::Value data;
    Json::Value pageInfo;
    int totalItems;
    int filteredItems;
    int returnedItems;
    bool hasMore;
};

class DataPager {
public:
    // Safety: cap limit at 5000, validate --all usage
    static PagingOptions validateOptions(const PagingOptions& opts, bool isJsonFormat);
    
    // Apply paging and filtering to JSON array with full metadata
    static PagingResult applyPaging(const Json::Value& data, const PagingOptions& opts);
    
    // Apply paging and filtering to string vector
    static std::vector<std::string> applyPaging(const std::vector<std::string>& data, const PagingOptions& opts);
    
    // Filter predicate for JSON values (enhanced with command-specific filters)
    static bool matchesFilter(const Json::Value& item, const PagingOptions& opts);
    
    // Filter predicate for strings
    static bool matchesFilter(const std::string& item, const std::string& filter);
    
    // Create paging metadata for JSON envelope (backward-compatible)
    static Json::Value createPageInfo(int totalItems, int filteredItems, int returnedItems, 
                                     const PagingOptions& opts, bool hasMore);
    
    // Generate next cursor/offset for pagination
    static std::string generateNextCursor(const PagingOptions& opts, int returnedItems);
    
    // Convert CLI overrides to paging options
    static PagingOptions fromCliOverrides(const CliOverrides& overrides);
};

} // namespace dinero::cli
