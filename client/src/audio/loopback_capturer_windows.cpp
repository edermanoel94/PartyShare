// WASAPI process loopback, the Windows half of audio::LoopbackCapturer.
//
// See docs/09-screen-audio.md, section 2. The stub that takes its
// place on every other platform lives in client/src/audio/loopback_stub.cpp.
//
// The shape of the activation follows Microsoft's own ApplicationLoopback
// sample, minus its Media Foundation work queues: this needs one thread that
// waits on an event, and MFStartup plus a shared work queue would be a
// dependency bought for nothing.

#include "audio/loopback_capturer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <dv/logging/logger.hpp>

#include "audio/block_pacer.hpp"

// Windows 10 build 20348 brought both the header and the feature. A Windows SDK
// older than that has no per-process loopback to compile against, and this
// translation unit falls back to the same answer the other platforms give.
#if __has_include(<audioclientactivationparams.h>)
#define DV_HAS_PROCESS_LOOPBACK 1
#include <audioclientactivationparams.h>
#else
#define DV_HAS_PROCESS_LOOPBACK 0
#endif

namespace dv::client::audio {

#if DV_HAS_PROCESS_LOOPBACK

namespace {

/// The build that introduced AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK.
constexpr DWORD kMinimumBuild = 20348;

constexpr auto kBlockInterval = std::chrono::milliseconds(10);
/// How long the capture thread is willing to sleep waiting for audio before it
/// looks at the clock again. Shorter than a block, so a block is never late
/// because the thread was still waiting.
constexpr DWORD kWaitMilliseconds = 5;
/// If the thread loses more than this to being descheduled, the block clock is
/// resynchronised instead of firing a burst of catch-up blocks.
constexpr auto kResyncThreshold = std::chrono::milliseconds(200);

/// The Windows version as the kernel knows it.
///
/// GetVersionEx reports what the application manifest asks for rather than what
/// is running, so a client built without a compatibility manifest is told it is
/// on Windows 8. RtlGetVersion is not shimmed.
[[nodiscard]] DWORD windows_build() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return 0;
  }
  // NOLINTNEXTLINE(clang-diagnostic-cast-function-type-strict): the cast is how
  // GetProcAddress is used, and the signature is documented.
  const auto get_version = reinterpret_cast<RtlGetVersionFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
  if (get_version == nullptr) {
    return 0;
  }
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (get_version(&info) != 0) {
    return 0;
  }
  return info.dwBuildNumber;
}

[[nodiscard]] std::string hresult_message(HRESULT result) {
  char buffer[64];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(result));
  return buffer;
}

/// Receives the audio client the asynchronous activation produces.
///
/// Windows calls back on a thread in the multi-threaded apartment while the
/// thread that asked is still blocked waiting, which is exactly the shape that
/// deadlocks a proxied object. IAgileObject is what says "call me on any
/// thread, no marshalling"; without it the callback comes back as
/// E_ILLEGAL_METHOD_CALL.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler,
                                public IAgileObject {
 public:
  explicit ActivationHandler(HANDLE done) : done_(done) {}

  // --- IUnknown --------------------------------------------------------------

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
      *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
    } else if (id == __uuidof(IAgileObject)) {
      *object = static_cast<IAgileObject*>(this);
    } else {
      *object = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --references_;
    if (left == 0) {
      delete this;
    }
    return left;
  }

  // --- IActivateAudioInterfaceCompletionHandler ------------------------------

  HRESULT STDMETHODCALLTYPE
  ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
    HRESULT activation = E_UNEXPECTED;
    IUnknown* unknown = nullptr;
    result_ = operation->GetActivateResult(&activation, &unknown);
    if (SUCCEEDED(result_)) {
      result_ = activation;
    }
    if (SUCCEEDED(result_) && unknown != nullptr) {
      result_ = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client_));
    }
    if (unknown != nullptr) {
      unknown->Release();
    }
    SetEvent(done_);
    return S_OK;
  }

  [[nodiscard]] HRESULT result() const { return result_; }

  /// Hands the client over. The caller owns the reference from here on.
  [[nodiscard]] IAudioClient* take_client() {
    IAudioClient* client = client_;
    client_ = nullptr;
    return client;
  }

 private:
  // Private and non-virtual, which is the COM shape: the only thing that ever
  // destroys one of these is Release() on the concrete type, and IUnknown has
  // no virtual destructor to override.
  ~ActivationHandler() {
    if (client_ != nullptr) {
      client_->Release();
    }
  }

  std::atomic<ULONG> references_{1};
  HANDLE done_;
  HRESULT result_ = E_UNEXPECTED;
  IAudioClient* client_ = nullptr;
};

class WindowsLoopbackCapturer final : public LoopbackCapturer {
 public:
  WindowsLoopbackCapturer(BlockSink blocks, ErrorSink errors)
      : blocks_(std::move(blocks)), errors_(std::move(errors)) {}

  ~WindowsLoopbackCapturer() override { stop(); }

  Result<std::monostate> start(LoopbackMode mode, std::uint32_t process_id) override {
    if (running_.load()) {
      return std::monostate{};
    }
    if (mode == LoopbackMode::Process && process_id == 0) {
      return Result<std::monostate>::failure("invalid_value",
                                             "a process loopback needs a process id");
    }
    if (const DWORD build = windows_build(); build != 0 && build < kMinimumBuild) {
      return Result<std::monostate>::failure(
          "capture_unavailable", "Windows build " + std::to_string(build) +
                                     " has no per-process audio loopback; " +
                                     std::to_string(kMinimumBuild) + " or newer is needed");
    }

    pacer_.clear();
    std::promise<std::optional<Error>> opened;
    std::future<std::optional<Error>> ready = opened.get_future();

    running_ = true;
    thread_ = std::thread([this, mode, process_id, opened = std::move(opened)]() mutable {
      run(mode, process_id, opened);
    });

    std::optional<Error> failure = ready.get();
    if (failure.has_value()) {
      running_ = false;
      if (thread_.joinable()) {
        thread_.join();
      }
      return Result<std::monostate>::failure(std::move(*failure));
    }
    return std::monostate{};
  }

  void stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    // Not from the capture thread: stop() is documented as safe to call from
    // inside a sink, and a thread cannot join itself.
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
      thread_.join();
    } else if (thread_.joinable()) {
      thread_.detach();
    }
  }

  [[nodiscard]] bool capturing() const override { return running_.load(); }

  [[nodiscard]] LoopbackStats stats() const override {
    const BlockPacer::Stats paced = pacer_.stats();
    LoopbackStats result;
    result.blocks_delivered = paced.blocks_taken;
    result.blocks_silent = paced.blocks_silent;
    result.frames_captured = paced.frames_pushed;
    result.frames_dropped = paced.frames_dropped;
    return result;
  }

 private:
  /// Everything the capture thread owns, so that it is all released on the same
  /// thread that created it.
  struct Session {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE ready = nullptr;

    ~Session() {
      if (capture != nullptr) {
        capture->Release();
      }
      if (client != nullptr) {
        client->Stop();
        client->Release();
      }
      if (ready != nullptr) {
        CloseHandle(ready);
      }
    }

    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
  };

  void run(LoopbackMode mode, std::uint32_t process_id,
           std::promise<std::optional<Error>>& opened) {
    // Multi-threaded, because the activation calls back from the MTA and this
    // thread is blocked waiting for it.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialise = SUCCEEDED(com);

    // The scope is load bearing. Every interface in the Session belongs to the
    // apartment CoInitializeEx just opened, and releasing one after
    // CoUninitialize has closed it is an access violation on a freed vtable -
    // reliably, a few milliseconds after a capture stops. The session has to be
    // gone before the apartment is.
    {
      Session session;
      if (std::optional<Error> failure = open(mode, process_id, session); failure.has_value()) {
        opened.set_value(std::move(failure));
        running_ = false;
      } else {
        opened.set_value(std::nullopt);
        loop(session);
      }
    }

    if (uninitialise) {
      CoUninitialize();
    }
  }

  [[nodiscard]] std::optional<Error> open(LoopbackMode mode, std::uint32_t process_id,
                                          Session& session) {
    session.ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (session.ready == nullptr) {
      return Error{.code = "capture_unavailable", .message = "could not create the capture event"};
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activation{};
    activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation.ProcessLoopbackParams.ProcessLoopbackMode =
        mode == LoopbackMode::Process ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                                      : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    // In System mode the process to exclude is us. That is the whole of the
    // feedback protection: everything else the machine plays is fair game, and
    // what we play never is.
    activation.ProcessLoopbackParams.TargetProcessId =
        mode == LoopbackMode::Process ? static_cast<DWORD>(process_id) : GetCurrentProcessId();

    PROPVARIANT parameters{};
    parameters.vt = VT_BLOB;
    parameters.blob.cbSize = sizeof(activation);
    parameters.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);

    HANDLE activated = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (activated == nullptr) {
      return Error{.code = "capture_unavailable",
                   .message = "could not create the activation event"};
    }

    auto* handler = new ActivationHandler(activated);
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT result =
        ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                    &parameters, handler, &operation);
    if (SUCCEEDED(result)) {
      WaitForSingleObject(activated, INFINITE);
      result = handler->result();
      session.client = handler->take_client();
    }
    if (operation != nullptr) {
      operation->Release();
    }
    handler->Release();
    CloseHandle(activated);

    if (FAILED(result) || session.client == nullptr) {
      return Error{
          .code = "capture_unavailable",
          .message = "the audio loopback could not be activated (" + hresult_message(result) + ")"};
    }

    // The format is ours to choose rather than the device's to report: with
    // AUTOCONVERTPCM the audio engine resamples and remixes into whatever is
    // asked for. Asking for the format the rest of the path already speaks
    // means there is no conversion left to write here.
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(kChannels);
    format.nSamplesPerSec = static_cast<DWORD>(kSampleRateHz);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    result = session.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format, nullptr);
    if (FAILED(result)) {
      return Error{.code = "capture_unavailable",
                   .message = "the audio client refused the capture format (" +
                              hresult_message(result) + ")"};
    }

    result = session.client->GetService(__uuidof(IAudioCaptureClient),
                                        reinterpret_cast<void**>(&session.capture));
    if (FAILED(result)) {
      return Error{.code = "capture_unavailable",
                   .message = "no capture client (" + hresult_message(result) + ")"};
    }

    result = session.client->SetEventHandle(session.ready);
    if (FAILED(result)) {
      return Error{.code = "capture_unavailable",
                   .message = "the capture event was refused (" + hresult_message(result) + ")"};
    }

    result = session.client->Start();
    if (FAILED(result)) {
      return Error{.code = "capture_unavailable",
                   .message = "the capture would not start (" + hresult_message(result) + ")"};
    }

    DV_LOG_INFO("Loopback: capturing {}",
                mode == LoopbackMode::Process
                    ? "process " + std::to_string(process_id) + " and its children"
                    : std::string("everything but this process"));
    return std::nullopt;
  }

  void loop(Session& session) {
    std::vector<std::int16_t> block(kSamplesPerBlock, 0);
    auto next = std::chrono::steady_clock::now();

    while (running_.load()) {
      WaitForSingleObject(session.ready, kWaitMilliseconds);

      // Drained on every pass, event or not. The event says "there is
      // something"; it does not say "this is all there is", and a quiet
      // application never fires it at all.
      if (!drain(session)) {
        break;
      }

      const auto now = std::chrono::steady_clock::now();
      if (now - next > kResyncThreshold) {
        // The thread lost a stretch of time to the scheduler. Firing one block
        // per lost 10 ms would deliver a burst of stale audio; starting over
        // from now costs the gap once.
        next = now;
      }
      while (running_.load() && std::chrono::steady_clock::now() >= next) {
        next += kBlockInterval;
        pacer_.take(block);
        if (blocks_) {
          blocks_(block);
        }
      }
    }
  }

  /// Reads every packet Windows has accumulated. Returns false when the stream
  /// died, having reported why.
  bool drain(Session& session) {
    UINT32 waiting = 0;
    while (SUCCEEDED(session.capture->GetNextPacketSize(&waiting)) && waiting > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 position = 0;
      UINT64 counter = 0;
      const HRESULT result =
          session.capture->GetBuffer(&data, &frames, &flags, &position, &counter);
      if (result == AUDCLNT_S_BUFFER_EMPTY) {
        break;
      }
      if (FAILED(result)) {
        fail("the capture stream ended (" + hresult_message(result) + ")");
        return false;
      }

      if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        // Windows says "this packet is silence" with a flag rather than with a
        // buffer of zeros, and the buffer may hold anything at all.
        pacer_.push_silence(frames);
      } else {
        pacer_.push({reinterpret_cast<const std::int16_t*>(data),
                     static_cast<std::size_t>(frames) * kChannels});
      }
      session.capture->ReleaseBuffer(frames);
    }
    return true;
  }

  void fail(std::string message) {
    DV_LOG_WARN("Loopback: {}", message);
    running_ = false;
    if (errors_) {
      errors_(Error{.code = "capture_failed", .message = std::move(message)});
    }
  }

  BlockSink blocks_;
  ErrorSink errors_;
  BlockPacer pacer_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace

bool loopback_capture_is_available() noexcept {
  const DWORD build = windows_build();
  return build == 0 || build >= kMinimumBuild;
}

Result<std::unique_ptr<LoopbackCapturer>> create_loopback_capturer(
    LoopbackCapturer::BlockSink blocks, LoopbackCapturer::ErrorSink errors) {
  if (!loopback_capture_is_available()) {
    return Result<std::unique_ptr<LoopbackCapturer>>::failure(
        "capture_unavailable", "this Windows is older than build " + std::to_string(kMinimumBuild) +
                                   ", which is where per-process audio loopback starts");
  }
  return std::unique_ptr<LoopbackCapturer>(
      std::make_unique<WindowsLoopbackCapturer>(std::move(blocks), std::move(errors)));
}

#else  // DV_HAS_PROCESS_LOOPBACK

// A Windows SDK older than 10.0.20348. Nothing to compile against, so this
// build answers the way the other platforms do.

bool loopback_capture_is_available() noexcept {
  return false;
}

// NOLINTBEGIN(performance-unnecessary-value-param)
Result<std::unique_ptr<LoopbackCapturer>> create_loopback_capturer(
    LoopbackCapturer::BlockSink /*blocks*/, LoopbackCapturer::ErrorSink /*errors*/) {
  // NOLINTEND(performance-unnecessary-value-param)
  return Result<std::unique_ptr<LoopbackCapturer>>::failure(
      "capture_unavailable",
      "this client was built with a Windows SDK that predates per-process audio loopback");
}

#endif  // DV_HAS_PROCESS_LOOPBACK

}  // namespace dv::client::audio
