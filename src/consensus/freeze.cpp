/**
 * Phase D.4: Consensus Freeze Implementation
 *
 * Runtime verification of consensus integrity.
 *
 * SPDX-License-Identifier: MIT
 */

#include "consensus/freeze.h"
#include "crypto/sha256.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <vector>

namespace dinero {
namespace consensus {

namespace {

/**
 * Compute SHA256 hash of a file using the project SHA256 primitive
 *
 * Returns lowercase hex-encoded SHA256 (64 characters), or empty string on error.
 */
std::string ComputeFileSHA256(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return "";  // File not found
    }

    crypto::CSHA256 sha256_ctx;

    // Read file in chunks and update hash
    constexpr size_t BUFFER_SIZE = 8192;
    char buffer[BUFFER_SIZE];

    while (file.good()) {
        file.read(buffer, BUFFER_SIZE);
        std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            sha256_ctx.Write(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(bytes_read));
        }
    }
    file.close();

    // Finalize and get hash
    uint8_t hash[32];
    sha256_ctx.Finalize(hash);

    // Convert to lowercase hex string
    std::ostringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');
    for (int i = 0; i < 32; ++i) {
        hex_stream << std::setw(2) << static_cast<int>(hash[i]);
    }

    return hex_stream.str();
}

} // anonymous namespace

//==============================================================================
// PUBLIC API
//==============================================================================

bool VerifyConsensusIntegrity(std::string& error_msg) {
    // Phase D.4: Full consensus integrity verification
    //
    // 1. Iterate through CONSENSUS_MANIFEST
    // 2. Verify each file exists
    // 3. Compute SHA256 of each file
    // 4. Compare with expected checksum
    // 5. Return false if any mismatch

    for (const auto& entry : CONSENSUS_MANIFEST) {
        // Check file exists
        std::ifstream file(entry.filename);
        if (!file.is_open()) {
            error_msg = "Consensus file missing: ";
            error_msg += entry.filename;
            return false;
        }
        file.close();

        // ========================================================================
        // SECURITY: Reject development placeholders in production
        // ========================================================================
        // TODO_COMPUTE_HASH was a development placeholder that allowed files
        // to bypass integrity verification. This is a security hole - any file
        // with this placeholder would be silently accepted without verification.
        // In production, this MUST cause verification to fail.
        if (std::strcmp(entry.sha256_hex, "TODO_COMPUTE_HASH") == 0) {
            error_msg = "SECURITY ERROR: File has TODO_COMPUTE_HASH placeholder: ";
            error_msg += entry.filename;
            error_msg += " - All consensus files must have computed SHA256 checksums";
            return false;
        }

        // SELF_REFERENCE: A file cannot contain its own correct SHA256 (circular dependency)
        // This is the only legitimate case for skipping verification.
        // freeze.h necessarily has this because it defines the manifest.
        if (std::strcmp(entry.sha256_hex, "SELF_REFERENCE") == 0) {
            continue;  // Skip verification for self-referential file only
        }

        // Compute actual SHA256 and compare
        std::string computed = ComputeFileSHA256(entry.filename);
        if (computed.empty()) {
            error_msg = "Failed to compute SHA256 for: ";
            error_msg += entry.filename;
            return false;
        }

        if (computed != entry.sha256_hex) {
            error_msg = "Consensus file integrity check FAILED: ";
            error_msg += entry.filename;
            error_msg += " (expected: ";
            error_msg += entry.sha256_hex;
            error_msg += ", computed: ";
            error_msg += computed;
            error_msg += ")";
            return false;
        }
    }

    return true;  // All checks passed
}

std::string GetConsensusFreezeReport() {
    std::ostringstream oss;

    oss << "========================================\n";
    oss << "Dinero Consensus Freeze Report\n";
    oss << "========================================\n\n";

    // Version
    oss << "Consensus Version: " << GetConsensusVersion() << "\n";
    oss << "Freeze Status:     " << (CONSENSUS_FROZEN ? "🔒 FROZEN" : "⚠️  UNFROZEN") << "\n";
    oss << "Freeze Date:       " << CONSENSUS_FREEZE_DATE << "\n";
    oss << "Freeze Phase:      " << CONSENSUS_FREEZE_PHASE << "\n\n";

    // Phase D completion
    oss << "Phase D Status:    " << (PHASE_D_COMPLETE ? "✅ COMPLETE" : "⚠️  INCOMPLETE") << "\n\n";

    // File manifest
    oss << "Consensus Manifest (" << CONSENSUS_MANIFEST.size() << " files):\n";
    oss << "----------------------------------------\n";
    for (const auto& entry : CONSENSUS_MANIFEST) {
        oss << "  • " << entry.filename << "\n";

        // Check if file exists
        std::ifstream file(entry.filename);
        if (file.is_open()) {
            oss << "    Status: ✅ Found\n";
            file.close();
        } else {
            oss << "    Status: ❌ MISSING\n";
        }

        // Checksum status
        if (std::strcmp(entry.sha256_hex, "TODO_COMPUTE_HASH") == 0) {
            oss << "    SHA256: ❌ SECURITY ERROR - TODO_COMPUTE_HASH placeholder in production!\n";
        } else if (std::strcmp(entry.sha256_hex, "SELF_REFERENCE") == 0) {
            oss << "    SHA256: 🔄 Self-reference (cannot verify own hash)\n";
        } else {
            oss << "    Expected: " << entry.sha256_hex << "\n";
            // Compute and show actual hash
            std::string computed = ComputeFileSHA256(entry.filename);
            if (!computed.empty()) {
                if (computed == entry.sha256_hex) {
                    oss << "    Computed: " << computed << " ✅\n";
                } else {
                    oss << "    Computed: " << computed << " ❌ MISMATCH\n";
                }
            } else {
                oss << "    Computed: ❌ Failed to compute\n";
            }
        }
        oss << "\n";
    }

    // Integrity check
    oss << "----------------------------------------\n";
    std::string error_msg;
    bool integrity_ok = VerifyConsensusIntegrity(error_msg);
    if (integrity_ok) {
        oss << "Integrity Check: ✅ PASSED\n";
    } else {
        oss << "Integrity Check: ❌ FAILED\n";
        oss << "Error: " << error_msg << "\n";
    }

    oss << "========================================\n";

    return oss.str();
}

} // namespace consensus
} // namespace dinero
