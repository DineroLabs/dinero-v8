#pragma once

#include <cstdint>
#include <string>

namespace dinero {

/**
 * @brief Difficulty validation and bounds checking
 * 
 * Ensures difficulty transitions are sane and prevents degenerate cases
 * that could break consensus or allow attacks.
 */
class DifficultyValidation {
public:
    // Difficulty bounds (Dinero unified - 50× easier than Bitcoin genesis)
    static constexpr uint32_t MIN_DIFFICULTY_BITS = 0x1d31ffce; // Dinero's floor (50× easier)
    static constexpr uint32_t MAX_DIFFICULTY_BITS = 0x207fffff; // Maximum allowed
    
    // Maximum difficulty change per adjustment (4x up or down)
    static constexpr uint32_t MAX_DIFFICULTY_CHANGE_FACTOR = 4;
    
    /**
     * @brief Validate difficulty bits value
     * @param bits Compact difficulty representation
     * @return true if bits value is valid
     */
    static bool isValidDifficultyBits(uint32_t bits);
    
    /**
     * @brief Check if difficulty change is within allowed bounds
     * @param old_bits Previous difficulty
     * @param new_bits New difficulty
     * @return true if change is acceptable
     */
    static bool isValidDifficultyChange(uint32_t old_bits, uint32_t new_bits);
    
    /**
     * @brief Convert compact bits to target value
     * @param bits Compact difficulty representation
     * @return 256-bit target as hex string (or empty if invalid)
     */
    static std::string bitsToTarget(uint32_t bits);
    
    /**
     * @brief Get difficulty as a floating point number
     * @param bits Compact difficulty representation
     * @return Difficulty relative to minimum (1.0 = minimum difficulty)
     */
    static double getDifficultyFromBits(uint32_t bits);
    
    /**
     * @brief Validate that target meets minimum work requirement
     * @param target_hex 256-bit target as hex string
     * @return true if target represents sufficient work
     */
    static bool meetsMinimumWork(const std::string& target_hex);
    
private:
    /**
     * @brief Check if compact bits encoding is valid
     * @param bits Compact representation
     * @return true if encoding is valid (no overflow, proper format)
     */
    static bool isValidCompactEncoding(uint32_t bits);
};
