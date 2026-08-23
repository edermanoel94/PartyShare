// The screen share encoded by the graphics card, through Media Foundation.
//
// The other half of task 4 of M8 on Windows. NVENC covers NVIDIA and nothing
// else; Media Foundation is how Intel QuickSync and AMD VCN are reached, and
// section 6 of SPEC.md names it as the Windows answer alongside NVENC.
//
// Nothing is linked at load time either. mfplat.dll is part of Windows, but
// not of every Windows: the N and KN editions ship without the Media Feature
// Pack and have no mfplat.dll at all. An executable that imported it would
// fail to start there rather than fall back to the software encoder, which is
// the opposite of what a fallback is for. So it is opened through
// webrtc/dynamic_library.hpp like the NVIDIA libraries are.
//
// mfuuid.lib and strmiids.lib are linked, and that is not a contradiction:
// they are static libraries of GUID constants, bytes in the executable, with
// no DLL behind them.
//
// A hardware MFT is always asynchronous, and that shapes this whole file. It
// is not asked to encode a frame; it says when it wants one, and says
// separately when it has output ready, both on its own threads. So Encode()
// does not encode: it converts, and either hands the frame over or queues it
// for the next time the encoder asks. See the event loop in EventSink.

#include "webrtc/hardware_encoder.hpp"

#ifdef DV_WITH_MEDIA_FOUNDATION

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/video_codec.h>
#include <api/video_codecs/video_encoder.h>
#include <libyuv/convert.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <dv/logging/logger.hpp>

#include "webrtc/dynamic_library.hpp"
#include "webrtc/hardware_encoder_backend.hpp"

// Last, and with the macros turned off. <windows.h> defines min, max and ERROR
// as macros, and libwebrtc has its own names for at least two of those.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <codecapi.h>
#include <windows.h>
// codecapi.h declares the property GUIDs and the enums; the interface that
// takes them is declared separately, and without this ICodecAPI is a name
// nothing has defined.
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace dv::client::media {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kMediaFoundationLibrary = "mfplat.dll";

/// How many converted frames wait for the encoder to ask for one.
///
/// Small on purpose. A hardware encoder that has fallen this far behind is not
/// going to catch up, and a deeper queue would only add delay to a picture
/// that is already late. The oldest goes, for the same reason FrameQueue drops
/// the oldest: on a screen, the newest frame is the true one.
constexpr std::size_t kMaxQueuedFrames = 3;

/// The mfplat.dll entry points this needs.
///
/// STDAPICALLTYPE is __stdcall, which x64 ignores, but it is what the headers
/// declare and writing it keeps these types comparable with the real ones.
struct MfApi {
  using Startup = HRESULT(STDAPICALLTYPE*)(ULONG, DWORD);
  using Shutdown = HRESULT(STDAPICALLTYPE*)();
  using EnumEx = HRESULT(STDAPICALLTYPE*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                                          const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*);
  using CreateMediaType = HRESULT(STDAPICALLTYPE*)(IMFMediaType**);
  using CreateSample = HRESULT(STDAPICALLTYPE*)(IMFSample**);
  using CreateMemoryBuffer = HRESULT(STDAPICALLTYPE*)(DWORD, IMFMediaBuffer**);

  Startup startup = nullptr;
  Shutdown shutdown = nullptr;
  EnumEx enum_ex = nullptr;
  CreateMediaType create_media_type = nullptr;
  CreateSample create_sample = nullptr;
  CreateMemoryBuffer create_memory_buffer = nullptr;

  [[nodiscard]] bool complete() const {
    return startup != nullptr && shutdown != nullptr && enum_ex != nullptr &&
           create_media_type != nullptr && create_sample != nullptr &&
           create_memory_buffer != nullptr;
  }
};

/// Media Foundation, started once, plus the answer to "can this machine
/// encode H.264 in hardware".
///
/// A singleton for the reason the NVENC one is: the answer cannot change while
/// the process runs.
class MediaFoundation {
 public:
  static MediaFoundation& instance() {
    static MediaFoundation platform;
    return platform;
  }

  [[nodiscard]] bool available() const { return available_; }
  [[nodiscard]] const std::string& detail() const { return detail_; }
  [[nodiscard]] const MfApi& api() const { return api_; }

  /// A transform of its own for each encoder. Activating rather than holding
  /// one open, because two screen shares in one process want two sessions and
  /// an MFT is not reentrant.
  [[nodiscard]] ComPtr<IMFTransform> activate() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!available_ || activator_ == nullptr) {
      return nullptr;
    }
    ComPtr<IMFTransform> transform;
    if (FAILED(activator_->ActivateObject(IID_PPV_ARGS(&transform)))) {
      return nullptr;
    }
    return transform;
  }

 private:
  MediaFoundation() { probe(); }

  void probe() {
    if (!library_.open(kMediaFoundationLibrary)) {
      detail_ = std::string(kMediaFoundationLibrary) +
                " is not installed, which is what a Windows N edition without the Media Feature "
                "Pack looks like";
      return;
    }

    api_.startup = symbol_as<MfApi::Startup>(library_, "MFStartup");
    api_.shutdown = symbol_as<MfApi::Shutdown>(library_, "MFShutdown");
    api_.enum_ex = symbol_as<MfApi::EnumEx>(library_, "MFTEnumEx");
    api_.create_media_type = symbol_as<MfApi::CreateMediaType>(library_, "MFCreateMediaType");
    api_.create_sample = symbol_as<MfApi::CreateSample>(library_, "MFCreateSample");
    api_.create_memory_buffer =
        symbol_as<MfApi::CreateMemoryBuffer>(library_, "MFCreateMemoryBuffer");
    if (!api_.complete()) {
      detail_ = std::string(kMediaFoundationLibrary) + " is missing symbols this needs";
      return;
    }

    // LITE leaves out the sockets and the platform's own media sources, none
    // of which an encoder uses.
    if (const HRESULT started = api_.startup(MF_VERSION, MFSTARTUP_LITE); FAILED(started)) {
      detail_ = "MFStartup failed with 0x" + hex(started);
      return;
    }
    started_ = true;

    find_encoder();
  }

  void find_encoder() {
    // Matched on what comes out, with the input left open: every hardware
    // encoder takes some flavour of YUV in, and which one is a negotiation
    // rather than a filter.
    MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video, MFVideoFormat_H264};

    // HARDWARE and nothing else. Without it the enumeration also answers with
    // the inbox software encoder, and this whole seam would then report
    // "hardware" for something running on the processor - which would tell
    // libwebrtc it may push harder than it should, and would make the log line
    // a lie.
    IMFActivate** activators = nullptr;
    UINT32 count = 0;
    const HRESULT found = api_.enum_ex(MFT_CATEGORY_VIDEO_ENCODER,
                                       MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                       nullptr, &output, &activators, &count);
    if (FAILED(found)) {
      detail_ = "MFTEnumEx failed with 0x" + hex(found);
      return;
    }

    // Success with nothing in it is the ordinary answer on a machine whose
    // graphics driver has no H.264 encoder, and dereferencing the array
    // without looking is the classic way to crash here.
    if (count == 0) {
      if (activators != nullptr) {
        ::CoTaskMemFree(activators);
      }
      detail_ = "no graphics driver here registers a hardware H.264 encoder";
      return;
    }

    // First, because SORTANDFILTER already put them in merit order.
    activator_ = activators[0];
    name_ = friendly_name(activators[0]);
    for (UINT32 index = 1; index < count; ++index) {
      activators[index]->Release();
    }
    ::CoTaskMemFree(activators);

    available_ = true;
    detail_ = name_.empty() ? "Media Foundation is available" : "Media Foundation on " + name_;
  }

  /// MFT_FRIENDLY_NAME_Attribute, narrowed for the log. Empty when the driver
  /// does not set one, which is allowed.
  [[nodiscard]] static std::string friendly_name(IMFActivate* activator) {
    LPWSTR wide = nullptr;
    UINT32 length = 0;
    if (FAILED(activator->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wide, &length)) ||
        wide == nullptr) {
      return {};
    }

    std::string narrow;
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), nullptr, 0,
                                             nullptr, nullptr);
    if (needed > 0) {
      narrow.resize(static_cast<std::size_t>(needed));
      ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), narrow.data(), needed,
                            nullptr, nullptr);
    }
    ::CoTaskMemFree(wide);
    return narrow;
  }

  [[nodiscard]] static std::string hex(HRESULT status) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(status));
    return buffer;
  }

  mutable std::mutex mutex_;
  DynamicLibrary library_;
  MfApi api_;
  ComPtr<IMFActivate> activator_;
  std::string name_;
  std::string detail_ = "not probed";
  bool available_ = false;
  bool started_ = false;
};

/// What has to survive the trip through the encoder to come back out.
///
/// An asynchronous transform hands back a sample with no memory of the
/// webrtc::VideoFrame that produced it, and libwebrtc needs the timestamps of
/// that frame on the encoded image. Kept against the sample time, which the
/// MFT does preserve.
struct FrameMetadata {
  std::uint32_t rtp_timestamp = 0;
  std::int64_t capture_time_ms = 0;
  std::int64_t ntp_time_ms = 0;
  webrtc::VideoRotation rotation = webrtc::kVideoRotation_0;
};

class MediaFoundationVideoEncoder;

/// Receives the transform's events, on the transform's own threads.
///
/// A COM object of its own rather than the encoder implementing
/// IMFAsyncCallback directly. The encoder is owned by a std::unique_ptr that
/// libwebrtc holds, and an object cannot be owned by a unique_ptr and by COM
/// reference counting at the same time without one of them destroying it while
/// the other is still using it.
class EventSink final : public IMFAsyncCallback {
 public:
  explicit EventSink(MediaFoundationVideoEncoder* owner) : owner_(owner) {}

  /// Waits for any event being handled right now, then stops forwarding.
  ///
  /// The waiting is the point. Without it the encoder can be destroyed while
  /// Invoke is halfway through using it, which is a crash that only happens
  /// when a call ends at the wrong moment.
  void detach() {
    const std::lock_guard<std::mutex> lock(mutex_);
    owner_ = nullptr;
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (id == IID_IUnknown || id == IID_IMFAsyncCallback) {
      *out = static_cast<IMFAsyncCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(::InterlockedIncrement(&references_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG remaining = ::InterlockedDecrement(&references_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  // IMFAsyncCallback
  HRESULT STDMETHODCALLTYPE GetParameters(DWORD* flags, DWORD* queue) override {
    if (flags == nullptr || queue == nullptr) {
      return E_POINTER;
    }
    *flags = 0;
    *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override;

 private:
  ~EventSink() = default;

  std::mutex mutex_;
  MediaFoundationVideoEncoder* owner_ = nullptr;
  LONG references_ = 1;
};

class MediaFoundationVideoEncoder final : public webrtc::VideoEncoder {
 public:
  ~MediaFoundationVideoEncoder() override { Release(); }

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const webrtc::VideoEncoder::Settings& /*settings*/) override {
    if (codec_settings == nullptr || codec_settings->codecType != webrtc::kVideoCodecH264) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (codec_settings->width <= 0 || codec_settings->height <= 0) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }

    Release();

    MediaFoundation& platform = MediaFoundation::instance();
    if (!platform.available()) {
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    width_ = codec_settings->width;
    height_ = codec_settings->height;
    framerate_ = std::max<UINT32>(1, codec_settings->maxFramerate);
    bitrate_bps_ = codec_settings->startBitrate > 0 ? codec_settings->startBitrate * 1000
                                                    : codec_settings->maxBitrate * 1000;
    if (bitrate_bps_ == 0) {
      bitrate_bps_ = 1500000;
    }

    transform_ = platform.activate();
    if (transform_ == nullptr) {
      DV_LOG_WARN("Media Foundation: the transform would not activate, falling back to software");
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    if (!unlock() || !configure()) {
      Release();
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    if (!start_streaming()) {
      Release();
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    DV_LOG_INFO("Media Foundation: encoding {}x{} at up to {} fps, {} kbps", width_, height_,
                framerate_, bitrate_bps_ / 1000);
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    // The sink first, and it waits: after this returns, no event is inside
    // this object and none ever will be again, so the rest can be torn down
    // without racing the transform's threads.
    if (sink_ != nullptr) {
      sink_->detach();
      sink_->Release();
      sink_ = nullptr;
    }
    events_.Reset();

    if (transform_ != nullptr) {
      (void)transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      (void)transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      (void)transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);

      ComPtr<IMFShutdown> shutdown;
      if (SUCCEEDED(transform_.As(&shutdown))) {
        (void)shutdown->Shutdown();
      }
      transform_.Reset();
    }

    codec_api_.Reset();

    const std::lock_guard<std::mutex> lock(mutex_);
    queued_.clear();
    metadata_.clear();
    pending_requests_ = 0;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Encode(const webrtc::VideoFrame& frame,
                 const std::vector<webrtc::VideoFrameType>* frame_types) override {
    if (transform_ == nullptr) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (callback_ == nullptr) {
        return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
      }
    }

    const bool keyframe = frame_types != nullptr && !frame_types->empty() &&
                          (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey;

    // One tick per frame at the negotiated rate. Only ever used to find the
    // metadata again on the way out, so it has to be unique and monotonic and
    // nothing more.
    const LONGLONG sample_time =
        static_cast<LONGLONG>(frame_index_++) * (10'000'000LL / static_cast<LONGLONG>(framerate_));

    ComPtr<IMFSample> sample = to_nv12_sample(frame, sample_time);
    if (sample == nullptr) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    Pending pending;
    pending.sample = sample;
    pending.keyframe = keyframe;

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      metadata_[sample_time] = FrameMetadata{
          .rtp_timestamp = frame.rtp_timestamp(),
          .capture_time_ms = frame.render_time_ms(),
          .ntp_time_ms = frame.ntp_time_ms(),
          .rotation = frame.rotation(),
      };

      if (pending_requests_ == 0) {
        if (queued_.size() >= kMaxQueuedFrames) {
          // Oldest out. See kMaxQueuedFrames.
          if (const auto stale = queued_.front(); stale.sample != nullptr) {
            LONGLONG when = 0;
            if (SUCCEEDED(stale.sample->GetSampleTime(&when))) {
              metadata_.erase(when);
            }
          }
          queued_.pop_front();
        }
        queued_.push_back(pending);
        return WEBRTC_VIDEO_CODEC_OK;
      }
      --pending_requests_;
    }

    return feed(pending) ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
  }

  void SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) override {
    if (codec_api_ == nullptr) {
      return;
    }
    const UINT32 bitrate = parameters.bitrate.get_sum_bps();
    if (bitrate == 0 || bitrate == bitrate_bps_) {
      return;
    }
    bitrate_bps_ = bitrate;

    // Settable while encoding on Windows 8 and later, where it also takes
    // precedence over MF_MT_AVG_BITRATE on the output type. Reconfiguring
    // rather than restarting, because the congestion controller moves this
    // every second and a restart would mean a keyframe every second.
    VARIANT value;
    ::VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = bitrate;
    (void)codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
  }

  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override {
    webrtc::VideoEncoder::EncoderInfo info;
    info.implementation_name = "MediaFoundation";
    info.is_hardware_accelerated = true;
    // Frames arrive as I420 and are converted here, so there is no platform
    // handle for libwebrtc to hand straight through.
    info.supports_native_handle = false;
    return info;
  }

  /// For the sink, which needs it to end and re-arm each event. Safe for it to
  /// hold: the sink is detached, and waits for any event in flight, before
  /// this is reset in Release().
  [[nodiscard]] IMFMediaEventGenerator* events_generator() const { return events_.Get(); }

  /// Called by the sink, on the transform's threads.
  void on_need_input() {
    Pending pending;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (queued_.empty()) {
        // Nothing converted yet. Remembered, so the next Encode can go
        // straight in instead of waiting for another request.
        ++pending_requests_;
        return;
      }
      pending = queued_.front();
      queued_.pop_front();
    }
    (void)feed(pending);
  }

  void on_have_output() {
    if (transform_ == nullptr) {
      return;
    }

    MFT_OUTPUT_DATA_BUFFER output = {};
    output.dwStreamID = 0;
    // Left null: a hardware transform allocates its own output, which
    // MFT_OUTPUT_STREAM_PROVIDES_SAMPLES on the stream info says it will, and
    // handing it one of ours would be refused.
    output.pSample = nullptr;
    DWORD status = 0;

    HRESULT produced = S_OK;
    {
      const std::lock_guard<std::mutex> lock(transform_mutex_);
      produced = transform_->ProcessOutput(0, 1, &output, &status);
    }

    if (output.pEvents != nullptr) {
      output.pEvents->Release();
      output.pEvents = nullptr;
    }
    if (FAILED(produced) || output.pSample == nullptr) {
      return;
    }

    ComPtr<IMFSample> sample;
    sample.Attach(output.pSample);
    deliver(sample.Get());
  }

 private:
  struct Pending {
    ComPtr<IMFSample> sample;
    bool keyframe = false;
  };

  [[nodiscard]] bool unlock() {
    ComPtr<IMFAttributes> attributes;
    if (FAILED(transform_->GetAttributes(&attributes)) || attributes == nullptr) {
      return false;
    }

    // Until this is set, almost every method of the transform answers
    // MF_E_TRANSFORM_ASYNC_LOCKED. The Media Session would do it for us; there
    // is no Media Session here, so it is ours to do.
    if (FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool configure() {
    if (FAILED(transform_.As(&codec_api_))) {
      // Not fatal on its own, but it means no rate control, and a screen share
      // whose bitrate cannot be steered is one that will flood the link.
      DV_LOG_WARN("Media Foundation: the transform has no ICodecAPI, so its rate cannot be set");
    }

    apply_static_codec_settings();

    // Output first. The H.264 encoder answers MF_E_TRANSFORM_TYPE_NOT_SET on
    // the input until its output type is set, so this order is not a style
    // choice.
    if (!set_output_type() || !set_input_type()) {
      return false;
    }

    apply_dynamic_codec_settings();
    return true;
  }

  void apply_static_codec_settings() {
    if (codec_api_ == nullptr) {
      return;
    }

    // Before SetOutputType, which is where the encoder reads it. Zero B
    // pictures because a B picture is a frame that cannot be sent until the
    // one after it has been encoded, and on a screen share that delay buys
    // nothing anybody wants.
    set_codec_value(CODECAPI_AVEncMPVDefaultBPictureCount, VT_UI4, 0U);
  }

  void apply_dynamic_codec_settings() {
    if (codec_api_ == nullptr) {
      return;
    }

    set_codec_value(CODECAPI_AVEncCommonRateControlMode, VT_UI4,
                    static_cast<ULONG>(eAVEncCommonRateControlMode_CBR));
    set_codec_value(CODECAPI_AVEncCommonMeanBitRate, VT_UI4, bitrate_bps_);
    // One second of video in the leaky bucket.
    set_codec_value(CODECAPI_AVEncCommonBufferSize, VT_UI4, bitrate_bps_);

    // Named rather than left to the encoder: libwebrtc asks for a keyframe
    // when it needs one, and an encoder inserting its own on a schedule of its
    // choosing spends bandwidth nobody asked it to spend. Ten seconds is long
    // enough to be effectively "only when asked".
    set_codec_value(CODECAPI_AVEncMPVGOPSize, VT_UI4, framerate_ * 10);

    // VT_BOOL here, and VT_UI4 on the decoder. That asymmetry is documented
    // and is a trap: a VT_UI4 written here is quietly ignored, and the encoder
    // keeps reordering frames.
    //
    // Probed rather than assumed, because low latency mode is optional even
    // for a certified hardware encoder.
    if (codec_api_->IsSupported(&CODECAPI_AVLowLatencyMode) == S_OK) {
      VARIANT value;
      ::VariantInit(&value);
      value.vt = VT_BOOL;
      value.boolVal = VARIANT_TRUE;
      (void)codec_api_->SetValue(&CODECAPI_AVLowLatencyMode, &value);
    }
  }

  void set_codec_value(const GUID& property, VARTYPE type, ULONG number) {
    if (codec_api_ == nullptr || codec_api_->IsSupported(&property) != S_OK) {
      return;
    }
    VARIANT value;
    ::VariantInit(&value);
    value.vt = type;
    value.ulVal = number;
    (void)codec_api_->SetValue(&property, &value);
  }

  [[nodiscard]] bool set_output_type() {
    const MfApi& api = MediaFoundation::instance().api();
    ComPtr<IMFMediaType> type;
    if (FAILED(api.create_media_type(&type))) {
      return false;
    }

    if (FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
        FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps_)) ||
        FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_),
                                  static_cast<UINT32>(height_))) ||
        FAILED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, framerate_, 1)) ||
        FAILED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) {
      return false;
    }

    // Baseline: it is what section 6 of SPEC.md negotiates, and what every
    // decoder on the other end of a call is certain to have.
    //
    // MF_MT_MPEG2_LEVEL is deliberately not set. Left alone the encoder works
    // the level out from the size and the rate, and a level named by hand that
    // does not cover 1080p60 is a refusal with no explanation attached.
    (void)type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);

    const std::lock_guard<std::mutex> lock(transform_mutex_);
    return SUCCEEDED(transform_->SetOutputType(0, type.Get(), 0));
  }

  [[nodiscard]] bool set_input_type() {
    const MfApi& api = MediaFoundation::instance().api();
    ComPtr<IMFMediaType> type;
    if (FAILED(api.create_media_type(&type))) {
      return false;
    }

    if (FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
        FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width_))) ||
        FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_),
                                  static_cast<UINT32>(height_))) ||
        FAILED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, framerate_, 1)) ||
        FAILED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) {
      return false;
    }

    const std::lock_guard<std::mutex> lock(transform_mutex_);
    return SUCCEEDED(transform_->SetInputType(0, type.Get(), 0));
  }

  [[nodiscard]] bool start_streaming() {
    if (FAILED(transform_.As(&events_)) || events_ == nullptr) {
      DV_LOG_WARN(
          "Media Foundation: the transform generates no events, which an asynchronous "
          "one has to");
      return false;
    }

    sink_ = new EventSink(this);

    // Armed before streaming starts, or the first request arrives with nobody
    // listening for it.
    if (FAILED(events_->BeginGetEvent(sink_, nullptr))) {
      return false;
    }

    const std::lock_guard<std::mutex> lock(transform_mutex_);
    if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) {
      return false;
    }
    // This is the one that unblocks the first METransformNeedInput. Nothing
    // else does, which is also why a drain has to be followed by another one
    // of these before the transform will accept anything again.
    return SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
  }

  /// Hands one frame to the transform, forcing a keyframe first when asked.
  [[nodiscard]] bool feed(const Pending& pending) {
    if (transform_ == nullptr || pending.sample == nullptr) {
      return false;
    }

    const std::lock_guard<std::mutex> lock(transform_mutex_);

    // Set here, immediately before the frame it applies to, and on the same
    // thread. The property applies to the next sample given to ProcessInput
    // and then resets itself, so setting it anywhere else races whatever
    // frames are already in flight and forces a keyframe on the wrong one.
    if (pending.keyframe && codec_api_ != nullptr) {
      VARIANT value;
      ::VariantInit(&value);
      value.vt = VT_UI4;
      value.ulVal = 1;
      (void)codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
    }

    return SUCCEEDED(transform_->ProcessInput(0, pending.sample.Get(), 0));
  }

  /// I420 in, an NV12 sample out.
  ///
  /// NV12 because that is what a hardware encoder wants: its two planes are
  /// how the silicon reads chroma, and handing it I420 makes the driver do
  /// this same conversion somewhere less visible.
  [[nodiscard]] ComPtr<IMFSample> to_nv12_sample(const webrtc::VideoFrame& frame,
                                                 LONGLONG sample_time) {
    const webrtc::scoped_refptr<const webrtc::I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (i420 == nullptr) {
      return nullptr;
    }

    const MfApi& api = MediaFoundation::instance().api();
    const DWORD luma = static_cast<DWORD>(width_) * static_cast<DWORD>(height_);
    // One byte per luma sample, plus one interleaved chroma pair per two by
    // two block, which is half again.
    const DWORD size = luma + (luma / 2);

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(api.create_memory_buffer(size, &buffer))) {
      return nullptr;
    }

    BYTE* data = nullptr;
    DWORD capacity = 0;
    if (FAILED(buffer->Lock(&data, &capacity, nullptr)) || capacity < size) {
      if (data != nullptr) {
        (void)buffer->Unlock();
      }
      return nullptr;
    }

    const int converted = libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(),
                                             i420->StrideU(), i420->DataV(), i420->StrideV(), data,
                                             width_, data + luma, width_, width_, height_);
    (void)buffer->Unlock();
    if (converted != 0) {
      return nullptr;
    }
    if (FAILED(buffer->SetCurrentLength(size))) {
      return nullptr;
    }

    ComPtr<IMFSample> sample;
    if (FAILED(api.create_sample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) {
      return nullptr;
    }
    if (FAILED(sample->SetSampleTime(sample_time)) ||
        FAILED(sample->SetSampleDuration(10'000'000LL / static_cast<LONGLONG>(framerate_)))) {
      return nullptr;
    }
    return sample;
  }

  void deliver(IMFSample* sample) {
    LONGLONG sample_time = 0;
    (void)sample->GetSampleTime(&sample_time);

    webrtc::EncodedImageCallback* callback = nullptr;
    FrameMetadata meta;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      callback = callback_;
      if (const auto found = metadata_.find(sample_time); found != metadata_.end()) {
        meta = found->second;
        metadata_.erase(found);
      }
    }
    if (callback == nullptr) {
      return;
    }

    // Flattened rather than walked: the transform is allowed to hand back a
    // sample of several buffers, and libwebrtc wants one run of bytes.
    ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(sample->ConvertToContiguousBuffer(&contiguous)) || contiguous == nullptr) {
      return;
    }

    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(contiguous->Lock(&data, nullptr, &length)) || data == nullptr || length == 0) {
      return;
    }

    webrtc::EncodedImage image;
    image.SetEncodedData(webrtc::EncodedImageBuffer::Create(data, length));
    (void)contiguous->Unlock();

    // MFSampleExtension_CleanPoint is how a transform says IDR. Absent means
    // it is not one; the attribute is only set when it is.
    UINT32 clean = 0;
    const bool keyframe =
        SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean != 0;

    image._encodedWidth = static_cast<uint32_t>(width_);
    image._encodedHeight = static_cast<uint32_t>(height_);
    image.SetRtpTimestamp(meta.rtp_timestamp);
    image.capture_time_ms_ = meta.capture_time_ms;
    image.ntp_time_ms_ = meta.ntp_time_ms;
    image.rotation_ = meta.rotation;
    image._frameType = keyframe ? webrtc::VideoFrameType::kVideoFrameKey
                                : webrtc::VideoFrameType::kVideoFrameDelta;

    webrtc::CodecSpecificInfo codec_specific;
    codec_specific.codecType = webrtc::kVideoCodecH264;
    codec_specific.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;
    codec_specific.codecSpecific.H264.idr_frame = keyframe;

    (void)callback->OnEncodedImage(image, &codec_specific);
  }

  ComPtr<IMFTransform> transform_;
  ComPtr<IMFMediaEventGenerator> events_;
  ComPtr<ICodecAPI> codec_api_;
  /// Raw, and reference counted by hand: it is a COM object, and Release()
  /// hands the last reference back when the encoder is done with it.
  EventSink* sink_ = nullptr;

  /// Every call into the transform is serialised through this. IMFTransform
  /// makes no promise about being used from two threads at once, and here
  /// there are at least two: libwebrtc's encoder thread and the transform's
  /// own event threads.
  std::mutex transform_mutex_;

  mutable std::mutex mutex_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
  std::deque<Pending> queued_;
  /// Requests for input the transform has made and we could not answer yet.
  int pending_requests_ = 0;
  std::map<LONGLONG, FrameMetadata> metadata_;

  int width_ = 0;
  int height_ = 0;
  UINT32 framerate_ = 30;
  UINT32 bitrate_bps_ = 0;
  std::uint64_t frame_index_ = 0;
};

HRESULT STDMETHODCALLTYPE EventSink::Invoke(IMFAsyncResult* result) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (owner_ == nullptr) {
    return S_OK;
  }

  ComPtr<IMFMediaEvent> event;
  if (owner_->events_generator() == nullptr ||
      FAILED(owner_->events_generator()->EndGetEvent(result, &event)) || event == nullptr) {
    return S_OK;
  }

  MediaEventType type = MEUnknown;
  if (FAILED(event->GetType(&type))) {
    return S_OK;
  }

  switch (type) {
    case METransformNeedInput:
      owner_->on_need_input();
      break;
    case METransformHaveOutput:
      owner_->on_have_output();
      break;
    default:
      break;
  }

  // Re-armed for the next one. An event generator hands over exactly one event
  // per BeginGetEvent, so forgetting this stops the encoder dead with no error
  // anywhere.
  if (type != MEError && owner_ != nullptr && owner_->events_generator() != nullptr) {
    (void)owner_->events_generator()->BeginGetEvent(this, nullptr);
  }
  return S_OK;
}

HardwareEncoderProbe probe_media_foundation() {
  const MediaFoundation& platform = MediaFoundation::instance();
  return HardwareEncoderProbe{.available = platform.available(), .detail = platform.detail()};
}

std::unique_ptr<webrtc::VideoEncoder> create_media_foundation_encoder(
    const webrtc::Environment& /*env*/, const webrtc::SdpVideoFormat& format) {
  if (!MediaFoundation::instance().available()) {
    return nullptr;
  }
  if (!absl::EqualsIgnoreCase(format.name, "H264")) {
    return nullptr;
  }
  return std::make_unique<MediaFoundationVideoEncoder>();
}

}  // namespace

HardwareEncoderBackend media_foundation_backend() {
  return HardwareEncoderBackend{
      .name = "Media Foundation",
      .slug = "mediafoundation",
      .probe = &probe_media_foundation,
      .create = &create_media_foundation_encoder,
  };
}

}  // namespace dv::client::media

#endif  // DV_WITH_MEDIA_FOUNDATION
