#include "cli/paging.h"
#include "cli/overrides.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iostream>

namespace dinero::cli {

PagingOptions DataPager::validateOptions(const PagingOptions& opts, bool isJsonFormat) {
    PagingOptions validated = opts;
    
    // Safety: cap limit at 5000
    if (validated.limit > 5000) {
        validated.limit = 5000;
        std::cerr << "Warning: --limit capped at 5000 for safety" << std::endl;
    }
    
    // Safety: refuse --all unless JSON format
    if (validated.all && !isJsonFormat) {
        std::cerr << "Error: --all flag requires --format json to prevent terminal overflow" << std::endl;
        validated.all = false;
    }
    
    // If --all is set, bypass limit
    if (validated.all) {
        validated.limit = 0; // 0 = no limit
    }
    
    return validated;
}

PagingResult DataPager::applyPaging(const Json::Value& data, const PagingOptions& opts) {
    PagingResult result;
    result.data = Json::Value(Json::arrayValue);
    
    if (!data.isArray()) {
        result.totalItems = 1;
        result.filteredItems = 1;
        result.returnedItems = 1;
        result.hasMore = false;
        result.data = data;
        result.pageInfo = createPageInfo(1, 1, 1, opts, false);
        return result;
    }
    
    std::vector<Json::Value> filtered;
    result.totalItems = data.size();
    
    // Apply filtering first
    for (const auto& item : data) {
        if (matchesFilter(item, opts)) {
            filtered.push_back(item);
        }
    }
    
    result.filteredItems = filtered.size();
    
    // Apply offset and limit
    int start = std::max(0, opts.offset);
    int end = filtered.size();
    
    if (opts.limit > 0) {
        end = std::min(end, start + opts.limit);
    }
    
    for (int i = start; i < end; ++i) {
        result.data.append(filtered[i]);
    }
    
    result.returnedItems = result.data.size();
    result.hasMore = (opts.limit > 0) && ((start + opts.limit) < result.filteredItems);
    result.pageInfo = createPageInfo(result.totalItems, result.filteredItems, 
                                   result.returnedItems, opts, result.hasMore);
    
    return result;
}

std::vector<std::string> DataPager::applyPaging(const std::vector<std::string>& data, const PagingOptions& opts) {
    std::vector<std::string> filtered;
    
    // Apply filtering first
    for (const auto& item : data) {
        if (opts.filter.empty() || matchesFilter(item, opts.filter)) {
            filtered.push_back(item);
        }
    }
    
    // Apply offset and limit
    int start = std::max(0, opts.offset);
    int end = filtered.size();
    
    if (opts.limit > 0) {
        end = std::min(end, start + opts.limit);
    }
    
    std::vector<std::string> result;
    for (int i = start; i < end; ++i) {
        result.push_back(filtered[i]);
    }
    
    return result;
}

bool DataPager::matchesFilter(const Json::Value& item, const PagingOptions& opts) {
    // Basic text filter
    if (!opts.filter.empty()) {
        std::string lowerFilter = opts.filter;
        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
        
        std::string itemStr;
        if (item.isString()) {
            itemStr = item.asString();
        } else {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            itemStr = Json::writeString(builder, item);
        }
        
        std::transform(itemStr.begin(), itemStr.end(), itemStr.begin(), ::tolower);
        if (itemStr.find(lowerFilter) == std::string::npos) {
            return false;
        }
    }
    
    // Command-specific filters for transactions
    if (opts.minConf && item.isMember("confirmations")) {
        if (item["confirmations"].asInt() < *opts.minConf) return false;
    }
    
    if (opts.address && item.isMember("address")) {
        if (item["address"].asString() != *opts.address) return false;
    }
    
    if (opts.txType && item.isMember("category")) {
        if (item["category"].asString() != *opts.txType) return false;
    }
    
    if (opts.label && item.isMember("label")) {
        if (item["label"].asString() != *opts.label) return false;
    }
    
    // Command-specific filters for UTXOs
    if (opts.minAmount && item.isMember("amount")) {
        if (item["amount"].asDouble() < *opts.minAmount) return false;
    }
    
    if (opts.maxAmount && item.isMember("amount")) {
        if (item["amount"].asDouble() > *opts.maxAmount) return false;
    }
    
    if (opts.confirmedOnly && item.isMember("confirmations")) {
        if (item["confirmations"].asInt() == 0) return false;
    }
    
    // Command-specific filters for peers
    if (opts.state && item.isMember("connection_type")) {
        if (item["connection_type"].asString() != *opts.state) return false;
    }
    
    if (opts.minVersion && item.isMember("version")) {
        if (item["version"].asInt() < *opts.minVersion) return false;
    }
    
    // Command-specific filters for mempool
    if (opts.minFeeRate && item.isMember("fee_rate")) {
        if (item["fee_rate"].asDouble() < *opts.minFeeRate) return false;
    }
    
    if (opts.txid && item.isMember("txid")) {
        if (item["txid"].asString() != *opts.txid) return false;
    }
    
    return true;
}

bool DataPager::matchesFilter(const std::string& item, const std::string& filter) {
    if (filter.empty()) return true;
    
    std::string lowerItem = item;
    std::string lowerFilter = filter;
    
    std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(), ::tolower);
    std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
    
    return lowerItem.find(lowerFilter) != std::string::npos;
}

Json::Value DataPager::createPageInfo(int totalItems, int filteredItems, int returnedItems, 
                                     const PagingOptions& opts, bool hasMore) {
    Json::Value page(Json::objectValue);
    
    // Core pagination info (backward-compatible with din.cli.v1)
    page["limit"] = opts.limit;
    page["returned"] = returnedItems;
    page["offset"] = opts.offset;
    page["has_more"] = hasMore;
    
    // Additional metadata
    page["total_items"] = totalItems;
    page["filtered_items"] = filteredItems;
    
    if (hasMore) {
        page["next_offset"] = opts.offset + returnedItems;
    }
    
    // Include cursor if provided
    if (opts.cursor) {
        page["cursor"] = *opts.cursor;
    }
    
    // Include active filters
    if (!opts.filter.empty()) {
        page["filter"] = opts.filter;
    }
    
    if (opts.since) page["since"] = *opts.since;
    if (opts.until) page["until"] = *opts.until;
    
    return page;
}

std::string DataPager::generateNextCursor(const PagingOptions& opts, int returnedItems) {
    // Simple cursor implementation: base64-encoded offset
    std::ostringstream oss;
    oss << (opts.offset + returnedItems);
    return oss.str(); // In production, this would be base64-encoded
}

PagingOptions DataPager::fromCliOverrides(const CliOverrides& overrides) {
    PagingOptions opts;
    opts.limit = overrides.limit;
    opts.offset = overrides.offset;
    opts.cursor = overrides.cursor;
    opts.filter = overrides.filter.value_or("");
    opts.since = overrides.since;
    opts.until = overrides.until;
    opts.all = overrides.all;
    
    // Command-specific filters
    opts.minConf = overrides.minConf;
    opts.address = overrides.address;
    opts.txType = overrides.txType;
    opts.label = overrides.label;
    opts.minAmount = overrides.minAmount;
    opts.maxAmount = overrides.maxAmount;
    opts.confirmedOnly = overrides.confirmedOnly;
    opts.state = overrides.state;
    opts.minVersion = overrides.minVersion;
    opts.minFeeRate = overrides.minFeeRate;
    opts.txid = overrides.txid;
    
    return opts;
}

} // namespace dinero::cli
