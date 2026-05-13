#pragma once

#include <chrono>
#include <random>
#include <memory>

namespace dinero {

/**
 * @brief Test-friendly clock interface for deterministic timing
 */
class TestClock {
public:
    virtual ~TestClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
    virtual void advance(std::chrono::milliseconds duration) = 0;
    virtual void freeze() = 0;
    virtual void unfreeze() = 0;
};

/**
 * @brief Real clock implementation
 */
class RealClock : public TestClock {
public:
    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::now();
    }
    
    void advance(std::chrono::milliseconds duration) override {
        // No-op for real clock
    }
    
    void freeze() override {
        // No-op for real clock
    }
    
    void unfreeze() override {
        // No-op for real clock
    }
};

/**
 * @brief Mock clock for testing
 */
class MockClock : public TestClock {
private:
    std::chrono::steady_clock::time_point current_time_;
    bool frozen_ = false;
    
public:
    MockClock() : current_time_(std::chrono::steady_clock::now()) {}
    
    std::chrono::steady_clock::time_point now() const override {
        return current_time_;
    }
    
    void advance(std::chrono::milliseconds duration) override {
        if (!frozen_) {
            current_time_ += duration;
        }
    }
    
    void freeze() override {
        frozen_ = true;
    }
    
    void unfreeze() override {
        frozen_ = false;
    }
};

/**
 * @brief Test-friendly RNG interface for deterministic randomness
 */
class TestRNG {
public:
    virtual ~TestRNG() = default;
    virtual uint32_t next() = 0;
    virtual void seed(uint32_t seed) = 0;
    virtual std::mt19937& get_generator() = 0;
};

/**
 * @brief Real RNG implementation
 */
class RealRNG : public TestRNG {
private:
    std::random_device rd_;
    std::mt19937 gen_;
    
public:
    RealRNG() : gen_(rd_()) {}
    
    uint32_t next() override {
        return gen_();
    }
    
    void seed(uint32_t seed) override {
        gen_.seed(seed);
    }
    
    std::mt19937& get_generator() override {
        return gen_;
    }
};

/**
 * @brief Mock RNG for testing
 */
class MockRNG : public TestRNG {
private:
    std::mt19937 gen_;
    
public:
    MockRNG() : gen_(12345) {} // Fixed seed for reproducibility
    
    uint32_t next() override {
        return gen_();
    }
    
    void seed(uint32_t seed) override {
        gen_.seed(seed);
    }
    
    std::mt19937& get_generator() override {
        return gen_;
    }
};

/**
 * @brief Test utilities for mining components
 */
class MiningTestUtils {
public:
    static std::unique_ptr<TestClock> createMockClock() {
        return std::make_unique<MockClock>();
    }
    
    static std::unique_ptr<TestRNG> createMockRNG() {
        return std::make_unique<MockRNG>();
    }
    
    static std::unique_ptr<TestClock> createRealClock() {
        return std::make_unique<RealClock>();
    }
    
    static std::unique_ptr<TestRNG> createRealRNG() {
        return std::make_unique<RealRNG>();
    }
};

} // namespace dinero
