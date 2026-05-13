#pragma once

#include <string>
#include <variant>
#include <stdexcept>

/**
 * @brief Rust-style Result type for error handling
 *
 * Result<T> represents either a successful value (Ok) or an error (Err).
 * This provides explicit error handling without exceptions.
 *
 * Usage:
 *   Result<int> divide(int a, int b) {
 *     if (b == 0) return Result<int>::Err("Division by zero");
 *     return Result<int>::Ok(a / b);
 *   }
 *
 *   auto result = divide(10, 2);
 *   if (result.isOk()) {
 *     std::cout << "Result: " << result.value() << std::endl;
 *   } else {
 *     std::cout << "Error: " << result.error() << std::endl;
 *   }
 */
template<typename T>
class Result {
private:
    std::variant<T, std::string> data_;
    bool is_ok_;

    // Use tag dispatch to avoid overload ambiguity when T = std::string
    struct OkTag {};
    struct ErrTag {};

    Result(T value, OkTag) : data_(std::move(value)), is_ok_(true) {}
    Result(std::string error, ErrTag) : data_(std::move(error)), is_ok_(false) {}

public:
    // Factory methods
    static Result<T> Ok(T value) {
        return Result(std::move(value), OkTag{});
    }

    static Result<T> Err(std::string error) {
        return Result(std::move(error), ErrTag{});
    }

    // Status checks
    bool isOk() const { return is_ok_; }
    bool isErr() const { return !is_ok_; }

    // Value access (throws if called on Err)
    const T& value() const {
        if (!is_ok_) {
            throw std::runtime_error("Called value() on Err Result: " + std::get<std::string>(data_));
        }
        return std::get<T>(data_);
    }

    T& value() {
        if (!is_ok_) {
            throw std::runtime_error("Called value() on Err Result: " + std::get<std::string>(data_));
        }
        return std::get<T>(data_);
    }

    // Error access (throws if called on Ok)
    const std::string& error() const {
        if (is_ok_) {
            throw std::runtime_error("Called error() on Ok Result");
        }
        return std::get<std::string>(data_);
    }

    // Alias for Lightning Network compatibility
    const std::string& err() const {
        return error();
    }

    // Convenience: unwrap (throws on Err, returns value on Ok)
    T unwrap() {
        if (!is_ok_) {
            throw std::runtime_error("Called unwrap() on Err Result: " + std::get<std::string>(data_));
        }
        return std::get<T>(data_);
    }

    // Convenience: unwrap_or (returns default on Err)
    T unwrap_or(T default_value) {
        if (!is_ok_) {
            return default_value;
        }
        return std::get<T>(data_);
    }
};

// Specialization for std::string (to avoid variant<string, string>)
template<>
class Result<std::string> {
private:
    std::string value_;
    std::string error_;
    bool is_ok_;

    Result(std::string value, bool is_ok, bool is_value)
        : is_ok_(is_ok) {
        if (is_value) {
            value_ = std::move(value);
        } else {
            error_ = std::move(value);
        }
    }

public:
    static Result<std::string> Ok(std::string value) {
        return Result(std::move(value), true, true);
    }

    static Result<std::string> Err(std::string error) {
        return Result(std::move(error), false, false);
    }

    bool isOk() const { return is_ok_; }
    bool isErr() const { return !is_ok_; }

    const std::string& value() const {
        if (!is_ok_) {
            throw std::runtime_error("Called value() on Err Result: " + error_);
        }
        return value_;
    }

    std::string& value() {
        if (!is_ok_) {
            throw std::runtime_error("Called value() on Err Result: " + error_);
        }
        return value_;
    }

    const std::string& error() const {
        if (is_ok_) {
            throw std::runtime_error("Called error() on Ok Result");
        }
        return error_;
    }

    const std::string& err() const {
        return error();
    }

    std::string unwrap() {
        if (!is_ok_) {
            throw std::runtime_error("Called unwrap() on Err Result: " + error_);
        }
        return value_;
    }

    std::string unwrap_or(std::string default_value) {
        if (!is_ok_) {
            return default_value;
        }
        return value_;
    }
};

// Specialization for void (Result<void> for operations that don't return a value)
template<>
class Result<void> {
private:
    std::string error_;
    bool is_ok_;

    Result(bool is_ok, std::string error = "") : error_(std::move(error)), is_ok_(is_ok) {}

public:
    // Explicitly default copy and move constructors
    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;

    static Result<void> Ok() {
        return Result(true);
    }

    static Result<void> Err(std::string error) {
        return Result(false, std::move(error));
    }

    bool isOk() const { return is_ok_; }
    bool isErr() const { return !is_ok_; }

    const std::string& error() const {
        if (is_ok_) {
            throw std::runtime_error("Called error() on Ok Result");
        }
        return error_;
    }

    // Alias for Lightning Network compatibility
    const std::string& err() const {
        return error();
    }

    void unwrap() {
        if (!is_ok_) {
            throw std::runtime_error("Called unwrap() on Err Result: " + error_);
        }
    }
};
