// A shared library opened by name at runtime.
//
// Loaded rather than linked, and that is the whole point of this file. The
// libraries a hardware encoder needs do not ship with this program: they come
// with a graphics driver, or with a Windows edition that has the Media Feature
// Pack. A binary that linked them would refuse to start on a machine that has
// neither, and that machine is exactly the one the software encoder exists for.
//
// The platform calls live in the .cpp so that <windows.h> stays out of every
// translation unit that includes this. It defines ERROR, min and max as macros,
// and libwebrtc and Qt both have their own opinions about those names.

#pragma once

namespace dv::client::media {

/// One library, closed when this goes out of scope.
///
/// Move-only, because a handle that gets closed twice is a handle that closes
/// something else's library the second time.
class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  /// Opens `name`, which is the platform's own file name: "nvcuda.dll" on
  /// Windows, "libcuda.so.1" elsewhere. The system search path applies, so a
  /// driver library installed where drivers go is found without a path.
  ///
  /// False means it is not there, which for a driver library is an ordinary
  /// answer rather than an error: it is how a machine says it has no card.
  [[nodiscard]] bool open(const char* name);

  [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

  /// The address of `name`, or nullptr when the library does not export it.
  ///
  /// A library that opens and then turns out to be missing a symbol is worth
  /// telling apart from one that is absent: it usually means a driver too old
  /// for what is being asked of it.
  [[nodiscard]] void* symbol(const char* name) const;

  void close() noexcept;

 private:
  void* handle_ = nullptr;
};

/// `library.symbol(name)` with the cast that every caller would write anyway.
///
/// Casting an object pointer to a function pointer is not something ISO C++
/// blesses, but it is what dlsym and GetProcAddress are for and every platform
/// this runs on defines it. Kept in one place so it is written once.
template <typename Function>
[[nodiscard]] Function symbol_as(const DynamicLibrary& library, const char* name) {
  return reinterpret_cast<Function>(library.symbol(name));
}

}  // namespace dv::client::media
