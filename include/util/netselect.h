#pragma once

#include "consensus/chainparams.h"
#include <filesystem>
#include <optional>
#include <string>

namespace dinero {

/**
 * Deterministic network chain selection
 * 
 * Selection order (highest → lowest priority):
 * 1. CLI: -chain=<mainnet|testnet|regtest>
 * 2. Aliases: -regtest, -testnet (mutually exclusive; error if both given)
 * 3. Config (dinero.conf): chain=<...> (ignored if CLI specified)
 * 4. Auto (-chain=auto): inspect -datadir for exactly one known subdir containing a network marker
 * 5. Default: mainnet
 */
Chain DetermineChain(int argc, char* argv[], const std::filesystem::path& datadir);

/**
 * Validate network selection for conflicts
 * Throws std::runtime_error on conflicts like -regtest and -testnet both set
 */
void ValidateNetworkArgs(int argc, char* argv[]);

/**
 * Auto-detect network from datadir markers
 * Returns nullopt if no clear network can be determined
 * Throws if multiple conflicting networks detected
 */
std::optional<Chain> AutoDetectNetwork(const std::filesystem::path& datadir);

/**
 * Check if datadir contains network-specific markers
 */
bool HasNetworkMarkers(const std::filesystem::path& netdir);

/**
 * Create network-specific subdirectory and change to it
 */
void SetupNetworkDataDir(const std::filesystem::path& base_datadir, Chain chain);

/**
 * Cross-check selected network with database metadata
 * Throws if mismatch detected (unless allow_mismatch is true)
 */
void ValidateNetworkConsistency(Chain selected_chain, const std::string& db_network, bool allow_mismatch = false);

/**
 * Log network selection result with full context
 */
void LogNetworkSelection(Chain chain, const std::string& selection_reason);

} // namespace dinero
