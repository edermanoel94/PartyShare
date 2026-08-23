// Which hardware encoder this machine gets, out of the ones this build has.
//
// Replaces the arrangement where one backend was chosen by #ifdef and a stub
// file covered the case of none. That worked while a platform had at most one
// backend. Windows has two: NVENC on NVIDIA cards, Media Foundation on Intel
// and AMD, and no build-time switch can know which card the machine running
// the binary has in it.
//
// So the backends are a list, probed in order, and the first one that finds
// usable hardware wins. A build with an empty list answers exactly what the
// stub used to answer.

#include "webrtc/hardware_encoder.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include <dv/logging/logger.hpp>

#include "webrtc/hardware_encoder_backend.hpp"

namespace dv::client::media {
namespace {

/// The backends this build has, in the order they are tried.
///
/// NVENC first where both exist. On an NVIDIA machine the Media Foundation
/// transform is a wrapper around NVENC anyway, so going through it would add a
/// layer and take away control; and NVENC direct is the path this project has
/// actually measured, in docs/benchmarks.md. On an Intel or AMD machine the
/// NVENC probe fails cheaply - nvcuda.dll is simply not there - and the chain
/// carries on to the next one.
[[nodiscard]] std::vector<HardwareEncoderBackend> compiled_backends() {
  std::vector<HardwareEncoderBackend> list;
#ifdef DV_WITH_NVENC
  list.push_back(nvenc_backend());
#endif
#ifdef DV_WITH_MEDIA_FOUNDATION
  list.push_back(media_foundation_backend());
#endif
  return list;
}

/// DV_HARDWARE_ENCODER, lowercased. Empty when it is not set.
///
/// The escape hatch has to name a backend rather than just being on or off,
/// because on a machine with two of them "compare the hardware against the
/// software" is not the only comparison worth making.
/// DV_DISABLE_HARDWARE_ENCODER still works and is handled a layer up, in
/// libwebrtc_media_session.cpp.
[[nodiscard]] std::string requested_backend() {
  const char* value = std::getenv("DV_HARDWARE_ENCODER");
  if (value == nullptr) {
    return {};
  }
  std::string wanted = value;
  for (char& letter : wanted) {
    letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
  }
  return wanted;
}

/// Every backend's name, for the case where none of them worked.
///
/// hardware_encoder.hpp promises `implementation` is empty only when nothing
/// was compiled in, so a build that has backends has to name them even when
/// none of them found hardware.
[[nodiscard]] std::string names_of(const std::vector<HardwareEncoderBackend>& backends) {
  std::string names;
  for (const HardwareEncoderBackend& backend : backends) {
    if (!names.empty()) {
      names += ", ";
    }
    names += backend.name;
  }
  return names;
}

/// The backends, probed once, and the one that won.
class Chain {
 public:
  static const Chain& instance() {
    static const Chain chain;
    return chain;
  }

  [[nodiscard]] const HardwareEncoderSupport& support() const { return support_; }

  [[nodiscard]] std::unique_ptr<webrtc::VideoEncoder> create(
      const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) const {
    if (chosen_ == nullptr) {
      return nullptr;
    }
    return chosen_->create(env, format);
  }

 private:
  Chain() : backends_(compiled_backends()) { probe(); }

  void probe() {
    support_.compiled_in = !backends_.empty();
    if (backends_.empty()) {
      support_.detail = "this build has no hardware encoder backend for this platform";
      return;
    }

    support_.implementation = names_of(backends_);

    const std::string wanted = requested_backend();
    if (wanted == "none") {
      support_.detail = "DV_HARDWARE_ENCODER=none, so the screen is encoded in software";
      return;
    }

    // Every reason kept, not just the last one. "no hardware encoding" with
    // nothing after it is what sends somebody through driver documentation for
    // an afternoon, and on a machine with two backends there are two reasons.
    std::string reasons;
    bool named_one = false;

    for (const HardwareEncoderBackend& backend : backends_) {
      if (!wanted.empty() && wanted != backend.slug) {
        continue;
      }
      named_one = true;

      const HardwareEncoderProbe probed = backend.probe();
      if (probed.available) {
        chosen_ = &backend;
        support_.available = true;
        support_.implementation = backend.name;
        support_.detail = probed.detail;
        return;
      }

      if (!reasons.empty()) {
        reasons += "; ";
      }
      reasons += backend.name;
      reasons += ": ";
      reasons += probed.detail;
    }

    if (!wanted.empty() && !named_one) {
      // A typo in the variable is worth saying out loud. Silently falling back
      // to software would look exactly like the hardware not working.
      support_.detail = "DV_HARDWARE_ENCODER names " + wanted + ", which this build does not have";
      DV_LOG_WARN("Media: {}, the ones it has are {}", support_.detail, names_of(backends_));
      return;
    }

    support_.detail = std::move(reasons);
  }

  std::vector<HardwareEncoderBackend> backends_;
  /// Into `backends_`, which outlives it: both belong to the same immortal
  /// singleton and the vector is never touched again after probing.
  const HardwareEncoderBackend* chosen_ = nullptr;
  HardwareEncoderSupport support_;
};

}  // namespace

HardwareEncoderSupport hardware_encoder_support() {
  return Chain::instance().support();
}

std::unique_ptr<webrtc::VideoEncoder> create_hardware_encoder(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) {
  return Chain::instance().create(env, format);
}

}  // namespace dv::client::media
