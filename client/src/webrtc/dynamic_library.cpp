#include "webrtc/dynamic_library.hpp"

#include <utility>

#if defined(_WIN32)
// NOMINMAX and WIN32_LEAN_AND_MEAN before the include, not after: this file is
// the one place <windows.h> is allowed in, and it is allowed in on the
// condition that it brings nothing with it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dv::client::media {

DynamicLibrary::~DynamicLibrary() {
  close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

bool DynamicLibrary::open(const char* name) {
  close();
  if (name == nullptr) {
    return false;
  }

#if defined(_WIN32)
  handle_ = static_cast<void*>(::LoadLibraryA(name));
#else
  // NOW rather than LAZY: the point of opening these is to find out whether
  // the machine can encode, and a symbol that only fails to resolve later
  // would turn that answer into a crash in the middle of a call.
  //
  // LOCAL so that a driver library cannot answer a symbol lookup meant for
  // something else in this process.
  handle_ = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
  return handle_ != nullptr;
}

void* DynamicLibrary::symbol(const char* name) const {
  if (handle_ == nullptr || name == nullptr) {
    return nullptr;
  }

#if defined(_WIN32)
  // Through a function pointer first: GetProcAddress answers FARPROC, and
  // casting that straight to void* is the one direction MSVC warns about.
  FARPROC address = ::GetProcAddress(static_cast<HMODULE>(handle_), name);
  return reinterpret_cast<void*>(address);
#else
  return ::dlsym(handle_, name);
#endif
}

void DynamicLibrary::close() noexcept {
  if (handle_ == nullptr) {
    return;
  }

#if defined(_WIN32)
  ::FreeLibrary(static_cast<HMODULE>(handle_));
#else
  ::dlclose(handle_);
#endif
  handle_ = nullptr;
}

}  // namespace dv::client::media
