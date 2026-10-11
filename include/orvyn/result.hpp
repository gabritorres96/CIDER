#pragma once
// Minimal error-or-value types (C++20 has no std::expected).
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace orvyn {

enum class ErrorCode {
    Io,
    TooLarge,
    Truncated,
    BadMagic,
    Unsupported,
    Malformed,
    OutOfBounds,
    Overflow,
    Overlap,
    NotFound,
    InvalidArgument,
    LimitExceeded,
};

const char* to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code;
    std::string message;
};

// Success-or-error without a payload.
class Status {
public:
    Status() = default;
    Status(Error e) : err_(std::move(e)) {}  // NOLINT: implicit by design
    bool ok() const noexcept { return !err_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    const Error& error() const { return *err_; }

private:
    std::optional<Error> err_;
};

template <class T>
class Result {
public:
    Result(T v) : v_(std::move(v)) {}      // NOLINT: implicit by design
    Result(Error e) : v_(std::move(e)) {}  // NOLINT: implicit by design
    bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }
    T& value() { return std::get<0>(v_); }
    const T& value() const { return std::get<0>(v_); }
    const Error& error() const { return std::get<1>(v_); }

private:
    std::variant<T, Error> v_;
};

}  // namespace orvyn

// Evaluate a Result; on error return it from the enclosing function, otherwise bind `var`.
#define ORVYN_TRY(var, expr)                         \
    auto var##_result_ = (expr);                     \
    if (!var##_result_.ok()) return var##_result_.error(); \
    auto& var = var##_result_.value()

// Evaluate a Status/Result; on error return it from the enclosing function.
#define ORVYN_CHECK(expr)                                  \
    do {                                                   \
        if (auto orvyn_st_ = (expr); !orvyn_st_.ok())      \
            return orvyn_st_.error();                      \
    } while (0)
