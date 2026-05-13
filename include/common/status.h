#pragma once

#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace dinero {

// Production-grade error taxonomy for all Phase 4+ components
enum class Status {
    Ok = 0,
    NotFound,
    AlreadyExists,
    Invalid,
    Serialization,
    Corruption,
    Io,
    Internal
};

// Convert Status to human-readable string
inline const char* StatusToString(Status s) {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::NotFound: return "NotFound";
        case Status::AlreadyExists: return "AlreadyExists";
        case Status::Invalid: return "Invalid";
        case Status::Serialization: return "Serialization";
        case Status::Corruption: return "Corruption";
        case Status::Io: return "Io";
        case Status::Internal: return "Internal";
    }
    return "Unknown";
}

// StatusOr<T> - Either a value T or an error Status
template<typename T>
class StatusOr {
public:
    // Construct from Status (error case)
    StatusOr(Status status) : status_(status), has_value_(false) {
        static_assert(!std::is_same_v<T, Status>, "StatusOr<Status> is not allowed");
    }
    
    // Construct from value (success case)
    StatusOr(T&& value) : status_(Status::Ok), has_value_(true) {
        new (&storage_) T(std::move(value));
    }
    
    StatusOr(const T& value) : status_(Status::Ok), has_value_(true) {
        new (&storage_) T(value);
    }
    
    // Copy constructor
    StatusOr(const StatusOr& other) : status_(other.status_), has_value_(other.has_value_) {
        if (has_value_) {
            new (&storage_) T(other.value());
        }
    }
    
    // Move constructor
    StatusOr(StatusOr&& other) noexcept : status_(other.status_), has_value_(other.has_value_) {
        if (has_value_) {
            new (&storage_) T(std::move(other.value()));
            other.has_value_ = false;
        }
    }
    
    // Destructor
    ~StatusOr() {
        if (has_value_) {
            reinterpret_cast<T*>(&storage_)->~T();
        }
    }
    
    // Assignment operators
    StatusOr& operator=(const StatusOr& other) {
        if (this != &other) {
            if (has_value_) {
                reinterpret_cast<T*>(&storage_)->~T();
            }
            status_ = other.status_;
            has_value_ = other.has_value_;
            if (has_value_) {
                new (&storage_) T(other.value());
            }
        }
        return *this;
    }
    
    StatusOr& operator=(StatusOr&& other) noexcept {
        if (this != &other) {
            if (has_value_) {
                reinterpret_cast<T*>(&storage_)->~T();
            }
            status_ = other.status_;
            has_value_ = other.has_value_;
            if (has_value_) {
                new (&storage_) T(std::move(other.value()));
                other.has_value_ = false;
            }
        }
        return *this;
    }
    
    // Check if contains value
    bool ok() const { return has_value_; }
    explicit operator bool() const { return has_value_; }
    
    // Get status (always valid)
    Status status() const { return status_; }
    
    // Get value (only valid if ok())
    const T& value() const & {
        if (!has_value_) {
            throw std::runtime_error("StatusOr: accessing value of error status");
        }
        return *reinterpret_cast<const T*>(&storage_);
    }
    
    T& value() & {
        if (!has_value_) {
            throw std::runtime_error("StatusOr: accessing value of error status");
        }
        return *reinterpret_cast<T*>(&storage_);
    }
    
    T&& value() && {
        if (!has_value_) {
            throw std::runtime_error("StatusOr: accessing value of error status");
        }
        return std::move(*reinterpret_cast<T*>(&storage_));
    }
    
    // Convenience operators
    const T& operator*() const & { return value(); }
    T& operator*() & { return value(); }
    T&& operator*() && { return std::move(value()); }
    
    const T* operator->() const { return &value(); }
    T* operator->() { return &value(); }
    
    // Get value or default
    template<typename U>
    T value_or(U&& default_value) const & {
        return has_value_ ? value() : static_cast<T>(std::forward<U>(default_value));
    }
    
    template<typename U>
    T value_or(U&& default_value) && {
        return has_value_ ? std::move(value()) : static_cast<T>(std::forward<U>(default_value));
    }

private:
    Status status_;
    bool has_value_;
    alignas(T) char storage_[sizeof(T)];
};

// Helper macros for error handling
#define RETURN_IF_ERROR(expr) \
    do { \
        auto _status = (expr); \
        if (_status != Status::Ok) { \
            return _status; \
        } \
    } while (0)

#define ASSIGN_OR_RETURN(lhs, rhs) \
    do { \
        auto _result = (rhs); \
        if (!_result.ok()) { \
            return _result.status(); \
        } \
        lhs = std::move(_result).value(); \
    } while (0)

} // namespace dinero
