#include "consensus/difficulty_validation.h"
#include "common/logger.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {

bool DifficultyValidation::isValidDifficultyBits(uint32_t bits) {
    // Check basic encoding validity
    if (!isValidCompactEncoding(bits)) {
        return false;
    }
    
    // Check bounds
    if (bits < MIN_DIFFICULTY_BITS || bits > MAX_DIFFICULTY_BITS) {
        return false;
    }
    
    // Convert to target and validate
    std::string target = bitsToTarget(bits);
    if (target.empty()) {
        return false;
    }
    
    return meetsMinimumWork(target);
}

bool DifficultyValidation::isValidDifficultyChange(uint32_t old_bits, uint32_t new_bits) {
    if (!isValidDifficultyBits(old_bits) || !isValidDifficultyBits(new_bits)) {
        return false;
    }
    
    double old_difficulty = getDifficultyFromBits(old_bits);
    double new_difficulty = getDifficultyFromBits(new_bits);
    
    if (old_difficulty <= 0 || new_difficulty <= 0) {
        return false;
    }
    
    double change_ratio = new_difficulty / old_difficulty;
    
    // Allow up to 4x change in either direction
    return (change_ratio >= 1.0 / MAX_DIFFICULTY_CHANGE_FACTOR && 
            change_ratio <= MAX_DIFFICULTY_CHANGE_FACTOR);
}

std::string DifficultyValidation::bitsToTarget(uint32_t bits) {
    if (!isValidCompactEncoding(bits)) {
        return "";
    }
    
    // Extract exponent and mantissa from compact representation
    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x00ffffff;
    
    // Check for overflow conditions
    if (exponent > 32) {
        return ""; // Target would be larger than 256 bits
    }
    
    if (mantissa > 0x7fffff) {
        return ""; // Mantissa too large
    }
    
    // Calculate target value
    // Target = mantissa * 256^(exponent - 3)
    std::string target(64, '0'); // 256 bits = 64 hex chars
    
    if (exponent <= 3) {
        // Small target, shift mantissa right
        uint32_t shifted_mantissa = mantissa >> (8 * (3 - exponent));
        if (shifted_mantissa == 0) {
            return ""; // Target would be zero
        }
        
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(6) << shifted_mantissa;
        std::string mantissa_hex = oss.str();
        
        // Place at the end (little-endian style for comparison)
        if (mantissa_hex.length() <= target.length()) {
            target.replace(target.length() - mantissa_hex.length(), mantissa_hex.length(), mantissa_hex);
        }
    } else {
        // Large target, place mantissa and pad with zeros
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(6) << mantissa;
        std::string mantissa_hex = oss.str();
        
        size_t zero_bytes = exponent - 3;
        if (zero_bytes * 2 + mantissa_hex.length() > target.length()) {
            return ""; // Target too large
        }
        
        // Place mantissa followed by zeros
        size_t start_pos = target.length() - (zero_bytes * 2 + mantissa_hex.length());
        target.replace(start_pos, mantissa_hex.length(), mantissa_hex);
    }
    
    return target;
}

double DifficultyValidation::getDifficultyFromBits(uint32_t bits) {
    if (!isValidCompactEncoding(bits)) {
        return 0.0;
    }
    
    // Difficulty = max_target / current_target
    // We'll use a simplified calculation based on the compact representation
    
    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x00ffffff;
    
    if (mantissa == 0) {
        return 0.0;
    }
    
    // Calculate relative difficulty compared to minimum
    uint32_t min_exponent = MIN_DIFFICULTY_BITS >> 24;
    uint32_t min_mantissa = MIN_DIFFICULTY_BITS & 0x00ffffff;
    
    // Simplified calculation: higher bits value = lower difficulty
    double current_work = static_cast<double>(mantissa) * (1ULL << (8 * (exponent - 3)));
    double min_work = static_cast<double>(min_mantissa) * (1ULL << (8 * (min_exponent - 3)));
    
    if (current_work <= 0) {
        return 0.0;
    }
    
    return min_work / current_work;
}

bool DifficultyValidation::meetsMinimumWork(const std::string& target_hex) {
    if (target_hex.empty() || target_hex.length() != 64) {
        return false;
    }
    
    // Convert minimum difficulty to target for comparison
    std::string min_target = bitsToTarget(MIN_DIFFICULTY_BITS);
    if (min_target.empty()) {
        return false;
    }
    
    // Target must be <= min_target (higher difficulty)
    // Compare as hex strings (lexicographic comparison works for same-length hex)
    return target_hex <= min_target;
}

bool DifficultyValidation::isValidCompactEncoding(uint32_t bits) {
    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x00ffffff;
    
    // Exponent must be reasonable (1-32 for 256-bit targets)
    if (exponent == 0 || exponent > 32) {
        return false;
    }
    
    // Mantissa must not be zero (would make target zero)
    if (mantissa == 0) {
        return false;
    }
    
    // Mantissa must not have leading zero byte (canonical form)
    if (mantissa <= 0x7fffff && exponent > 1) {
        return false;
    }
    
    // Check for negative targets (sign bit in mantissa)
    if (mantissa & 0x800000) {
        return false;
    }
    
    return true;
}

} // namespace dinero
