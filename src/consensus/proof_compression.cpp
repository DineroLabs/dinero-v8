#include "consensus/proof_compression.h"
#include <cstring>

namespace dinero {
namespace consensus {

// ProofCompressionManager implementation

ProofCompressionManager::ProofCompressionManager() {
    // Initialize with default thresholds
}

CompressionResult ProofCompressionManager::CompressProof(const BlockUtreexoData& proof) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Serialize proof to get original size
    std::vector<uint8_t> original_data = proof.serialize();
    size_t original_size = original_data.size();

    // Select best compression method
    CompressionMethod method = SelectMethod(original_size);

    // Compress using selected method
    CompressionResult result;
    result.original_size = original_size;
    result.method_used = method;

    switch (method) {
        case CompressionMethod::NONE: {
            // No compression - return original data
            result.data = original_data;
            result.compressed_size = original_size;
            stats_.uncompressed_count++;
            break;
        }

        case CompressionMethod::DEDUPLICATED: {
            // Use v2 format (deduplication only)
            result.data = proof.spend_proof.serializeCompressed();
            result.compressed_size = result.data.size();
            stats_.deduplication_count++;
            stats_.total_deduped_bytes += result.compressed_size;
            break;
        }

        case CompressionMethod::ZSTD: {
            // Use v3 format (deduplication + zstd)
            result.data = proof.spend_proof.serializeCompressedWithZstd();
            result.compressed_size = result.data.size();

            // Check if zstd was actually used (might fallback to v2)
            if (result.data.size() > 0 && result.data[0] == 3) {
                stats_.zstd_count++;
                stats_.total_zstd_bytes += result.compressed_size;
            } else {
                // Fell back to v2
                stats_.deduplication_count++;
                stats_.total_deduped_bytes += result.compressed_size;
            }
            break;
        }

        case CompressionMethod::AUTO:
            // Should not happen (SelectMethod returns specific method)
            result.data = original_data;
            result.compressed_size = original_size;
            break;
    }

    // Update statistics
    stats_.proofs_compressed++;
    stats_.total_original_bytes += original_size;
    stats_.total_compressed_bytes += result.compressed_size;

    return result;
}

CompressionResult ProofCompressionManager::CompressProofWithMethod(
    const BlockUtreexoData& proof, CompressionMethod method) {

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint8_t> original_data = proof.serialize();
    size_t original_size = original_data.size();

    CompressionResult result;
    result.original_size = original_size;
    result.method_used = method;

    switch (method) {
        case CompressionMethod::NONE: {
            result.data = original_data;
            result.compressed_size = original_size;
            stats_.uncompressed_count++;
            break;
        }

        case CompressionMethod::DEDUPLICATED: {
            result.data = proof.spend_proof.serializeCompressed();
            result.compressed_size = result.data.size();
            stats_.deduplication_count++;
            stats_.total_deduped_bytes += result.compressed_size;
            break;
        }

        case CompressionMethod::ZSTD: {
            result.data = proof.spend_proof.serializeCompressedWithZstd();
            result.compressed_size = result.data.size();

            if (result.data.size() > 0 && result.data[0] == 3) {
                stats_.zstd_count++;
                stats_.total_zstd_bytes += result.compressed_size;
            } else {
                stats_.deduplication_count++;
                stats_.total_deduped_bytes += result.compressed_size;
            }
            break;
        }

        case CompressionMethod::AUTO: {
            // Recursively call with auto-selected method
            CompressionMethod selected = SelectMethod(original_size);
            return CompressProofWithMethod(proof, selected);
        }
    }

    stats_.proofs_compressed++;
    stats_.total_original_bytes += original_size;
    stats_.total_compressed_bytes += result.compressed_size;

    return result;
}

std::optional<BlockUtreexoData> ProofCompressionManager::DecompressProof(
    const std::vector<uint8_t>& data) {

    if (data.size() == 0) {
        return std::nullopt;
    }

    // Detect format from version byte
    CompressionMethod method = DetectCompressionMethod(data);

    BlockUtreexoData proof_data;

    switch (method) {
        case CompressionMethod::NONE: {
            // v1 format - uncompressed
            proof_data = BlockUtreexoData::deserialize(data);
            break;
        }

        case CompressionMethod::DEDUPLICATED: {
            // v2 format - deduplicated
            BlockUtreexoProof decompressed = BlockUtreexoProof::deserializeCompressed(data);
            // Check if deserialization failed (empty proof)
            if (decompressed.targets.empty() && decompressed.proof_hashes.empty()) {
                return std::nullopt;
            }
            proof_data.spend_proof = decompressed;
            break;
        }

        case CompressionMethod::ZSTD: {
            // v3 format - deduplicated + zstd
            BlockUtreexoProof decompressed = BlockUtreexoProof::deserializeCompressedWithZstd(data);
            // Check if deserialization failed (empty proof)
            if (decompressed.targets.empty() && decompressed.proof_hashes.empty()) {
                return std::nullopt;
            }
            proof_data.spend_proof = decompressed;
            break;
        }

        default:
            return std::nullopt;
    }

    return proof_data;
}

size_t ProofCompressionManager::EstimateCompressedSize(const BlockUtreexoData& proof) const {
    // Use estimateCompressedSize() from BlockUtreexoProof
    return proof.spend_proof.estimateCompressedSize();
}

CompressionStats ProofCompressionManager::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void ProofCompressionManager::ClearStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = CompressionStats();
}

void ProofCompressionManager::SetCompressionThreshold(size_t threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    compression_threshold_ = threshold;
}

void ProofCompressionManager::SetZstdThreshold(size_t threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    zstd_threshold_ = threshold;
}

CompressionMethod ProofCompressionManager::SelectMethod(size_t proof_size) const {
    // No mutex needed - called from locked methods

    if (proof_size < compression_threshold_) {
        // Too small - compression overhead not worth it
        return CompressionMethod::NONE;
    }

    if (proof_size < zstd_threshold_) {
        // Medium size - deduplication only
        return CompressionMethod::DEDUPLICATED;
    }

    // Large - use zstd for maximum compression
    return CompressionMethod::ZSTD;
}

void ProofCompressionManager::UpdateStats(const CompressionResult& result) {
    // No mutex needed - called from locked methods
    stats_.proofs_compressed++;
    stats_.total_original_bytes += result.original_size;
    stats_.total_compressed_bytes += result.compressed_size;
}

// Utility functions

CompressionMethod DetectCompressionMethod(const std::vector<uint8_t>& data) {
    if (data.size() == 0) {
        return CompressionMethod::NONE;
    }

    uint8_t version = data[0];

    // Check if first byte looks like a version marker
    if (version == 2) {
        return CompressionMethod::DEDUPLICATED;
    }

    if (version == 3) {
        return CompressionMethod::ZSTD;
    }

    // Assume v1 format (no version byte)
    return CompressionMethod::NONE;
}

const char* CompressionMethodName(CompressionMethod method) {
    switch (method) {
        case CompressionMethod::NONE:
            return "None (v1)";
        case CompressionMethod::DEDUPLICATED:
            return "Deduplicated (v2)";
        case CompressionMethod::ZSTD:
            return "Zstd (v3)";
        case CompressionMethod::AUTO:
            return "Auto";
        default:
            return "Unknown";
    }
}

} // namespace consensus
} // namespace dinero
