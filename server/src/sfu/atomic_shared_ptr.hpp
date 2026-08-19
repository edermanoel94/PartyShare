// `std::atomic<std::shared_ptr<T>>` where the standard library has it, and the
// same three operations over a mutex where it does not.
//
// C++20 specifies the atomic specialisation, libstdc++ implements it, and the
// libc++ that ships with Xcode does not. There the primary template is chosen
// instead, which static_asserts that the type is trivially copyable, and a
// shared_ptr is not:
//
//   error: _Atomic cannot be applied to type 'std::shared_ptr<const T>' which
//   is not trivially copyable
//
// The fallback is a mutex, and a mutex is what the caller was avoiding. It is
// still the right trade: the section it guards is a pointer copy, so it is
// uncontended in practice, and a lock held for two instructions on one platform
// is better than the platform not building. The feature test macro decides, so
// the moment Apple ships the specialisation this compiles to the real thing
// with no edit here.

#pragma once

#include <memory>
#include <mutex>
#include <version>

namespace dv::server::sfu {

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L

template <typename T>
class AtomicSharedPtr {
 public:
  AtomicSharedPtr() = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> initial) : value_(std::move(initial)) {}

  [[nodiscard]] std::shared_ptr<T> load() const { return value_.load(); }
  void store(std::shared_ptr<T> next) { value_.store(std::move(next)); }

 private:
  std::atomic<std::shared_ptr<T>> value_;
};

#else

template <typename T>
class AtomicSharedPtr {
 public:
  AtomicSharedPtr() = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> initial) : value_(std::move(initial)) {}

  [[nodiscard]] std::shared_ptr<T> load() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return value_;
  }

  void store(std::shared_ptr<T> next) {
    const std::lock_guard<std::mutex> lock(mutex_);
    value_ = std::move(next);
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<T> value_;
};

#endif

}  // namespace dv::server::sfu
