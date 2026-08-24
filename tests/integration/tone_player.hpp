// Renders a quiet tone to the default playback device, from the test process
// itself.
//
// It exists so that a loopback capture has something known to hear that is
// entirely under the test's control: no second process to launch, no media file
// to ship, and nothing to go looking for when an assertion fails. Quiet and
// short on purpose - it does come out of the speakers.
//
// Windows only, like the capture it feeds.

#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <thread>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

namespace dv::testing {

/// The frequency every test in this project listens for. One number, so that a
/// detector and a player cannot disagree about it.
inline constexpr double kTestToneHz = 440.0;

class TonePlayer {
 public:
  explicit TonePlayer(double amplitude = 0.05) : amplitude_(amplitude) {}

  ~TonePlayer() { stop(); }

  TonePlayer(const TonePlayer&) = delete;
  TonePlayer& operator=(const TonePlayer&) = delete;
  TonePlayer(TonePlayer&&) = delete;
  TonePlayer& operator=(TonePlayer&&) = delete;

  /// False when this machine has no playback device, which is what a bare CI
  /// runner is.
  bool start() {
    std::promise<bool> opened;
    std::future<bool> ready = opened.get_future();
    running_ = true;
    thread_ = std::thread([this, &opened] { render(opened); });
    if (!ready.get()) {
      stop();
      return false;
    }
    return true;
  }

  void stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void render(std::promise<bool>& opened) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialise = SUCCEEDED(com);
    {
      IMMDeviceEnumerator* enumerator = nullptr;
      IMMDevice* device = nullptr;
      IAudioClient* client = nullptr;
      IAudioRenderClient* render_client = nullptr;
      WAVEFORMATEX* mix = nullptr;
      bool announced = false;

      const auto answer = [&](bool ok) {
        if (!announced) {
          opened.set_value(ok);
          announced = true;
        }
      };

      if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof(IMMDeviceEnumerator),
                                     reinterpret_cast<void**>(&enumerator))) &&
          SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
          SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&client))) &&
          SUCCEEDED(client->GetMixFormat(&mix)) &&
          SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10'000'000, 0, mix, nullptr)) &&
          SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient),
                                       reinterpret_cast<void**>(&render_client))) &&
          SUCCEEDED(client->Start())) {
        answer(true);
        pump(*client, *render_client, *mix);
        client->Stop();
      } else {
        answer(false);
      }

      if (mix != nullptr) {
        CoTaskMemFree(mix);
      }
      if (render_client != nullptr) {
        render_client->Release();
      }
      if (client != nullptr) {
        client->Release();
      }
      if (device != nullptr) {
        device->Release();
      }
      if (enumerator != nullptr) {
        enumerator->Release();
      }
    }
    if (uninitialise) {
      CoUninitialize();
    }
  }

  void pump(IAudioClient& client, IAudioRenderClient& render_client, const WAVEFORMATEX& mix) {
    UINT32 buffer_frames = 0;
    if (FAILED(client.GetBufferSize(&buffer_frames))) {
      return;
    }
    // The mix format is float32 on every desktop Windows in existence; the
    // 16-bit branch is there so a machine that surprises us produces a quiet
    // test failure rather than a scream.
    const bool is_float = mix.wBitsPerSample == 32;
    double phase = 0;
    const double step = 2.0 * 3.14159265358979323846 * kTestToneHz / mix.nSamplesPerSec;

    while (running_.load()) {
      UINT32 padding = 0;
      if (FAILED(client.GetCurrentPadding(&padding))) {
        return;
      }
      const UINT32 free_frames = buffer_frames - padding;
      if (free_frames == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      BYTE* data = nullptr;
      if (FAILED(render_client.GetBuffer(free_frames, &data)) || data == nullptr) {
        return;
      }
      for (UINT32 frame = 0; frame < free_frames; ++frame) {
        const double value = std::sin(phase) * amplitude_;
        phase += step;
        for (WORD channel = 0; channel < mix.nChannels; ++channel) {
          const std::size_t index = (static_cast<std::size_t>(frame) * mix.nChannels) + channel;
          if (is_float) {
            reinterpret_cast<float*>(data)[index] = static_cast<float>(value);
          } else {
            reinterpret_cast<std::int16_t*>(data)[index] =
                static_cast<std::int16_t>(value * 32767.0);
          }
        }
      }
      render_client.ReleaseBuffer(free_frames, 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  const double amplitude_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace dv::testing

#endif  // defined(_WIN32)
