// The applications the machine can currently be heard playing, read from the
// audio session manager of the default playback device.
//
// The counterpart for every other platform lives in
// client/src/audio/loopback_stub.cpp.

#include "audio/audio_sources.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <windows.h>

namespace dv::client::audio {

namespace {

/// Initialises COM for this thread, but only if nobody already has.
///
/// The Qt client's thread is initialised long before this is called; a test, a
/// tool or a worker thread is not, and there the device enumerator fails with
/// CO_E_NOTINITIALIZED, which reads exactly like "this machine has no sound
/// card". Doing it here means the caller does not have to know.
///
/// S_FALSE means the thread was already initialised and the count went up, so
/// the matching CoUninitialize is still ours to make. RPC_E_CHANGED_MODE means
/// it is initialised in the other apartment - fine for what follows, and not
/// ours to undo.
class ComScope {
 public:
  ComScope()
      : owned_(
            SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
  }

  ~ComScope() {
    if (owned_) {
      CoUninitialize();
    }
  }

  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  ComScope(ComScope&&) = delete;
  ComScope& operator=(ComScope&&) = delete;

 private:
  bool owned_;
};

/// Releases a COM interface on the way out of a scope. The alternative is a
/// return path per failure, each remembering a different subset of what is
/// open, which is how leaks are written.
template <typename T>
class Owned {
 public:
  Owned() = default;
  ~Owned() { reset(); }

  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;
  Owned(Owned&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
  Owned& operator=(Owned&& other) noexcept {
    if (this != &other) {
      reset();
      pointer_ = std::exchange(other.pointer_, nullptr);
    }
    return *this;
  }

  T** put() { return &pointer_; }
  void** put_void() { return reinterpret_cast<void**>(&pointer_); }
  T* get() const { return pointer_; }
  T* operator->() const { return pointer_; }
  explicit operator bool() const { return pointer_ != nullptr; }

  void reset() {
    if (pointer_ != nullptr) {
      pointer_->Release();
      pointer_ = nullptr;
    }
  }

 private:
  T* pointer_ = nullptr;
};

[[nodiscard]] std::string to_utf8(const wchar_t* wide) {
  if (wide == nullptr || *wide == L'\0') {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) {
    return {};
  }
  std::string result(static_cast<std::size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), length, nullptr, nullptr);
  return result;
}

/// The executable's name without its path or extension: "chrome", not
/// "C:\Program Files\Google\Chrome\Application\chrome.exe".
///
/// PROCESS_QUERY_LIMITED_INFORMATION rather than PROCESS_QUERY_INFORMATION on
/// purpose: the wider right is refused for processes at a higher integrity
/// level, and the narrower one is all this needs.
[[nodiscard]] std::string process_name(DWORD process_id) {
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) {
    return {};
  }
  wchar_t path[MAX_PATH];
  DWORD length = MAX_PATH;
  const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
  CloseHandle(process);
  if (ok == FALSE) {
    return {};
  }

  std::string name = to_utf8(path);
  if (const std::size_t slash = name.find_last_of("\\/"); slash != std::string::npos) {
    name.erase(0, slash + 1);
  }
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) {
    name.erase(name.size() - 4);
  }
  return name;
}

}  // namespace

Result<std::vector<AudioSource>> audio_sources() {
  // Outermost, so that every interface below is released before the apartment
  // it belongs to closes.
  const ComScope com;

  Owned<IMMDeviceEnumerator> enumerator;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), enumerator.put_void());
  if (FAILED(result) || !enumerator) {
    return Result<std::vector<AudioSource>>::failure("capture_unavailable",
                                                     "no audio device enumerator");
  }

  Owned<IMMDevice> device;
  result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
  if (FAILED(result) || !device) {
    return Result<std::vector<AudioSource>>::failure("capture_unavailable",
                                                     "this machine has no default playback device");
  }

  Owned<IAudioSessionManager2> manager;
  result =
      device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void());
  if (FAILED(result) || !manager) {
    return Result<std::vector<AudioSource>>::failure("capture_unavailable",
                                                     "the audio session manager is unavailable");
  }

  Owned<IAudioSessionEnumerator> sessions;
  result = manager->GetSessionEnumerator(sessions.put());
  if (FAILED(result) || !sessions) {
    return Result<std::vector<AudioSource>>::failure("capture_unavailable",
                                                     "the audio sessions could not be listed");
  }

  int count = 0;
  if (FAILED(sessions->GetCount(&count))) {
    return Result<std::vector<AudioSource>>::failure("capture_unavailable",
                                                     "the audio sessions could not be counted");
  }

  const DWORD self = GetCurrentProcessId();
  std::vector<AudioSource> found;
  std::unordered_set<DWORD> seen;

  for (int i = 0; i < count; ++i) {
    Owned<IAudioSessionControl> control;
    if (FAILED(sessions->GetSession(i, control.put())) || !control) {
      continue;
    }
    Owned<IAudioSessionControl2> details;
    if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), details.put_void())) ||
        !details) {
      continue;
    }
    // The chime a notification makes is not something anybody wants to share.
    if (details->IsSystemSoundsSession() == S_OK) {
      continue;
    }

    DWORD process_id = 0;
    if (FAILED(details->GetProcessId(&process_id)) || process_id == 0 || process_id == self) {
      continue;
    }

    AudioSessionState state = AudioSessionStateInactive;
    if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired) {
      // Expired means the process is gone and only the session object is left.
      continue;
    }
    const bool playing = state == AudioSessionStateActive;

    // One process can hold several sessions. They are one choice to the user,
    // because the capture targets the process either way.
    if (const auto [_, inserted] = seen.insert(process_id); !inserted) {
      if (playing) {
        const auto existing =
            std::find_if(found.begin(), found.end(), [process_id](const AudioSource& source) {
              return source.process_id == static_cast<std::uint32_t>(process_id);
            });
        if (existing != found.end()) {
          existing->playing = true;
        }
      }
      continue;
    }

    std::string name = process_name(process_id);
    if (name.empty()) {
      // GetDisplayName is usually empty for a desktop application, which is why
      // it is the fallback rather than the first choice.
      LPWSTR display = nullptr;
      if (SUCCEEDED(details->GetDisplayName(&display)) && display != nullptr) {
        name = to_utf8(display);
      }
      if (display != nullptr) {
        CoTaskMemFree(display);
      }
    }
    if (name.empty()) {
      name = "process " + std::to_string(process_id);
    }

    found.push_back(AudioSource{.process_id = static_cast<std::uint32_t>(process_id),
                                .name = std::move(name),
                                .playing = playing});
  }

  // What is making noise right now goes first; the rest keeps the order the
  // session manager reported, which is stable enough not to make a menu jump.
  std::stable_sort(found.begin(), found.end(),
                   [](const AudioSource& left, const AudioSource& right) {
                     return left.playing && !right.playing;
                   });
  return found;
}

}  // namespace dv::client::audio
