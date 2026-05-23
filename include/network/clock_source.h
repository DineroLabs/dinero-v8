// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <chrono>
#include <memory>

namespace dinero::network {

// Abstraction over time sources so TTL / expiry tests can advance time
// deterministically without std::this_thread::sleep_for. Scope is
// intentionally limited to the RELAY_HINTS lifecycle code path; do not
// retrofit unrelated time reads (YAGNI).
class ClockSource {
public:
    virtual ~ClockSource() = default;
    virtual std::chrono::steady_clock::time_point SteadyNow() const = 0;
    virtual std::chrono::system_clock::time_point SystemNow() const = 0;
};

class SystemClockSource final : public ClockSource {
public:
    std::chrono::steady_clock::time_point SteadyNow() const override {
        return std::chrono::steady_clock::now();
    }
    std::chrono::system_clock::time_point SystemNow() const override {
        return std::chrono::system_clock::now();
    }
};

// Test-only. Holds its own "current time" that callers advance manually.
class FakeClockSource final : public ClockSource {
public:
    FakeClockSource()
        : steady_(std::chrono::steady_clock::time_point{}),
          system_(std::chrono::system_clock::time_point{}) {}

    std::chrono::steady_clock::time_point SteadyNow() const override {
        return steady_;
    }
    std::chrono::system_clock::time_point SystemNow() const override {
        return system_;
    }

    void AdvanceSteady(std::chrono::nanoseconds delta) { steady_ += delta; }
    void AdvanceSystem(std::chrono::nanoseconds delta) { system_ += delta; }

private:
    std::chrono::steady_clock::time_point steady_;
    std::chrono::system_clock::time_point system_;
};

}  // namespace dinero::network
