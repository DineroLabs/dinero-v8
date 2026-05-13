#pragma once

#include <cstdint>
#include <random>

// Ring 4 Phase 4b: Deterministic Environment
// Purpose: Seeded RNG and mock clock for reproducible tests
// Rule: NO system time, NO system randomness

namespace mining_test {

// ============================================================================
// DeterministicRNG - Seeded random number generator
// ============================================================================

class DeterministicRNG {
public:
    // Construct with seed
    explicit DeterministicRNG(uint64_t seed) : rng_(seed), seed_(seed) {}

    // Get next random uint64
    uint64_t next() {
        return rng_();
    }

    // Get random in range [0, max)
    uint64_t nextInRange(uint64_t max) {
        if (max == 0) return 0;
        return rng_() % max;
    }

    // Get random in range [min, max]
    uint64_t nextInRange(uint64_t min, uint64_t max) {
        if (min >= max) return min;
        return min + (rng_() % (max - min + 1));
    }

    // Get random boolean
    bool nextBool() {
        return (rng_() % 2) == 1;
    }

    // Get random double in [0.0, 1.0]
    double nextDouble() {
        return static_cast<double>(rng_()) / static_cast<double>(rng_.max());
    }

    // Reset to original seed
    void reset() {
        rng_.seed(seed_);
    }

    // Get current seed
    uint64_t getSeed() const {
        return seed_;
    }

private:
    std::mt19937_64 rng_;
    uint64_t seed_;
};

// ============================================================================
// DeterministicClock - Mock clock that only advances explicitly
// ============================================================================

class DeterministicClock {
public:
    // Construct at time zero
    DeterministicClock() : current_time_(0) {}

    // Construct at specific start time
    explicit DeterministicClock(uint64_t start_time) : current_time_(start_time) {}

    // Get current time (does NOT advance)
    uint64_t now() const {
        return current_time_;
    }

    // Advance time by delta
    void advance(uint64_t delta) {
        current_time_ += delta;
    }

    // Set time to specific value
    void setTime(uint64_t time) {
        current_time_ = time;
    }

    // Reset to zero
    void reset() {
        current_time_ = 0;
    }

private:
    uint64_t current_time_;
};

// ============================================================================
// DeterministicEnvironment - Combined RNG + Clock
// ============================================================================

class DeterministicEnvironment {
public:
    // Construct with seed
    explicit DeterministicEnvironment(uint64_t seed)
        : rng_(seed), clock_(0), seed_(seed) {}

    // Access RNG
    DeterministicRNG& rng() { return rng_; }
    const DeterministicRNG& rng() const { return rng_; }

    // Access clock
    DeterministicClock& clock() { return clock_; }
    const DeterministicClock& clock() const { return clock_; }

    // Get current time (convenience)
    uint64_t now() const { return clock_.now(); }

    // Advance time (convenience)
    void advanceTime(uint64_t delta) { clock_.advance(delta); }

    // Get seed
    uint64_t getSeed() const { return seed_; }

    // Reset both RNG and clock
    void reset() {
        rng_.reset();
        clock_.reset();
    }

private:
    DeterministicRNG rng_;
    DeterministicClock clock_;
    uint64_t seed_;
};

}  // namespace mining_test
