#pragma once

#include <optional>
#include <vector>
#include <mutex>
#include <cstdint>
#include "primitives/uint256.h"
#include "primitives/block.h"

namespace dinero {
namespace consensus {

/**
 * Compression method selection
 *
 * Phase 9.4: Choose between different compression strategies
 */
enum class CompressionMethod {
    NONE = 0,           // No compression (v1 format)
    DEDUPLICATED = 1,   // Dictionary-based deduplication (v2 format)
    ZSTD = 2,           // Deduplication + zstd (v3 format)
    AUTO = 3            // Automatically select best method
};

/**
 * Compression statistics for monitoring
 *
 * Phase 9.4: Track compression effectiveness
 */
struct CompressionStats {
    uint64_t proofs_compressed = 0;
    uint64_t total_original_bytes = 0;
    uint64_t total_compressed_bytes = 0;
    uint64_t total_deduped_bytes = 0;
    uint64_t total_zstd_bytes = 0;

    uint64_t deduplication_count = 0;  // Used v2 format
    uint64_t zstd_count = 0;            // Used v3 format
    uint64_t uncompressed_count = 0;    // Used v1 format (too small)

    /**
     * Calculate average compression ratio
     * Returns ratio (original / compressed), e.g., 3.5 = 3.5:1 compression
     */
    double AverageCompressionRatio() const {
        if (total_compressed_bytes == 0) return 1.0;
        return static_cast<double>(total_original_bytes) / total_compressed_bytes;
    }

    /**
     * Calculate space saved (percentage)
     */
    double SpaceSavedPercent() const {
        if (total_original_bytes == 0) return 0.0;
        uint64_t saved = total_original_bytes - total_compressed_bytes;
        return (static_cast<double>(saved) / total_original_bytes) * 100.0;
    }
};

/**
 * Compression result with metadata
 *
 * Phase 9.4: Return compressed data plus statistics
 */
struct CompressionResult {
    std::vector<uint8_t> data;
    CompressionMethod method_used;
    size_t original_size;
    size_t compressed_size;

    /**
     * Calculate compression ratio for this proof
     */
    double CompressionRatio() const {
        if (compressed_size == 0) return 1.0;
        return static_cast<double>(original_size) / compressed_size;
    }
};

/**
 * Proof compression manager
 *
 * Phase 9.4: Coordinate proof compression for network efficiency
 *
 * Wraps existing BlockUtreexoProof compression methods:
 * - serializeCompressed() (v2): Dictionary-based deduplication
 * - serializeCompressedWithZstd() (v3): Deduplication + zstd
 *
 * Strategy:
 * 1. Small proofs (< 256 bytes): No compression (overhead not worth it)
 * 2. Medium proofs (256-1024 bytes): Deduplication only (v2)
 * 3. Large proofs (> 1024 bytes): Deduplication + zstd (v3)
 *
 * Non-consensus guarantees:
 * - Compression failure never blocks proof transmission (fallback to uncompressed)
 * - Invalid compressed data is safely rejected
 * - Decompression bomb protection (max 100 KB uncompressed)
 */
class ProofCompressionManager {
public:
    ProofCompressionManager();
    ~ProofCompressionManager() = default;

    /**
     * Compress proof using automatic method selection
     *
     * @param proof Proof to compress
     * @return Compression result with metadata
     */
    CompressionResult CompressProof(const BlockUtreexoData& proof);

    /**
     * Compress proof using specific method
     *
     * @param proof Proof to compress
     * @param method Compression method to use
     * @return Compression result with metadata
     */
    CompressionResult CompressProofWithMethod(const BlockUtreexoData& proof, CompressionMethod method);

    /**
     * Decompress proof (auto-detects format from version byte)
     *
     * @param data Compressed proof data
     * @return Decompressed proof, or nullopt if invalid
     */
    std::optional<BlockUtreexoData> DecompressProof(const std::vector<uint8_t>& data);

    /**
     * Estimate compressed size without actually compressing
     *
     * @param proof Proof to estimate
     * @return Estimated compressed size in bytes
     */
    size_t EstimateCompressedSize(const BlockUtreexoData& proof) const;

    /**
     * Get compression statistics
     */
    CompressionStats GetStats() const;

    /**
     * Clear compression statistics (for testing)
     */
    void ClearStats();

    /**
     * Configuration: Set compression threshold
     * Proofs smaller than this won't be compressed (default: 256 bytes)
     */
    void SetCompressionThreshold(size_t threshold);

    /**
     * Configuration: Set zstd threshold
     * Proofs smaller than this use deduplication only (default: 1024 bytes)
     */
    void SetZstdThreshold(size_t threshold);

private:
    // Compression thresholds
    size_t compression_threshold_ = 256;   // Don't compress below this
    size_t zstd_threshold_ = 1024;         // Use zstd above this

    // Statistics
    mutable CompressionStats stats_;

    // Thread safety
    mutable std::mutex mutex_;

    /**
     * Select best compression method for given proof size
     */
    CompressionMethod SelectMethod(size_t proof_size) const;

    /**
     * Update statistics after compression
     */
    void UpdateStats(const CompressionResult& result);
};

/**
 * Utility: Detect compression method from data
 *
 * @param data Proof data
 * @return Detected compression method based on version byte
 */
CompressionMethod DetectCompressionMethod(const std::vector<uint8_t>& data);

/**
 * Utility: Get human-readable method name
 */
const char* CompressionMethodName(CompressionMethod method);

} // namespace consensus
} // namespace dinero
