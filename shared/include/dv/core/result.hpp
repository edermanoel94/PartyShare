#pragma once

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace dv {

/// A recoverable failure. `code` is a stable machine readable identifier and is
/// safe to compare against; `message` is for humans and logs only.
struct Error {
  std::string code;
  std::string message;
};

/// Explicit error handling, as required by section 26 of SPEC.md.
///
/// Exceptions are never used to signal expected failures such as malformed
/// input. The only exception this type can raise comes from calling `value()`
/// or `error()` on the wrong state, which is a programming mistake, not a
/// runtime condition.
template <typename T>
class Result {
 public:
  using ValueType = T;

  Result(T value)  // NOLINT(google-explicit-constructor): implicit is the point
      : storage_(std::move(value)) {}

  static Result failure(Error error) { return Result(std::move(error)); }

  static Result failure(std::string code, std::string message) {
    return Result(Error{.code = std::move(code), .message = std::move(message)});
  }

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }

  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const T& value() const& {
    require(ok(), "Result::value() called on a failed Result");
    return std::get<T>(storage_);
  }

  [[nodiscard]] T&& take() && {
    require(ok(), "Result::take() called on a failed Result");
    return std::get<T>(std::move(storage_));
  }

  [[nodiscard]] const Error& error() const& {
    require(!ok(), "Result::error() called on a successful Result");
    return std::get<Error>(storage_);
  }

  /// Returns the contained value, or `fallback` when this Result failed.
  ///
  /// By const reference and not by value: the body copies out of the parameter
  /// either way, so taking it by value copies twice whenever the caller passes
  /// something it already had.
  [[nodiscard]] T value_or(const T& fallback) const& {
    return ok() ? std::get<T>(storage_) : fallback;
  }

 private:
  explicit Result(Error error) : storage_(std::move(error)) {}

  static void require(bool condition, const char* what) {
    if (!condition) {
      throw std::logic_error(what);
    }
  }

  std::variant<T, Error> storage_;
};

}  // namespace dv
