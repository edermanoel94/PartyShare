// The screen share encoded by the graphics card, through NVENC.
//
// Task 4 of M8. The plan names VAAPI for Linux, and VAAPI is the right answer
// on Intel and AMD; it is not an answer on NVIDIA, whose driver does no
// encoding through it. NVENC is what an NVIDIA machine has, it is the same API
// on Linux and Windows, and it is what could actually be verified on the
// machine this was written on.
//
// Nothing here is linked. libnvidia-encode comes with the driver and is opened
// at runtime, so a binary built with this file runs unchanged on a machine
// with no NVIDIA card at all: the support query answers no and the software
// encoder is used.

#include "webrtc/hardware_encoder.hpp"

#ifdef DV_WITH_NVENC

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/h264_profile_level_id.h>
#include <api/video_codecs/video_codec.h>
#include <api/video_codecs/video_encoder.h>
#include <dlfcn.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <dv/logging/logger.hpp>

#include "nvEncodeAPI.h"

namespace dv::client::media {
namespace {

/// The slice of the CUDA driver API this needs.
///
/// Declared here rather than vendored: NVENC needs a CUDA context to hang an
/// encoder session on, and creating one is five functions. Pulling in the
/// whole CUDA toolkit for five functions would be a heavy dependency for a
/// small need.
struct CudaApi {
  using Init = int (*)(unsigned int);
  using DeviceGetCount = int (*)(int*);
  using DeviceGet = int (*)(int*, int);
  using DeviceGetName = int (*)(char*, int, int);
  using CtxCreate = int (*)(void**, unsigned int, int);
  using CtxDestroy = int (*)(void*);
  using CtxPushCurrent = int (*)(void*);
  using CtxPopCurrent = int (*)(void**);

  Init init = nullptr;
  DeviceGetCount device_count = nullptr;
  DeviceGet device_get = nullptr;
  DeviceGetName device_name = nullptr;
  CtxCreate context_create = nullptr;
  CtxDestroy context_destroy = nullptr;
  CtxPushCurrent context_push = nullptr;
  CtxPopCurrent context_pop = nullptr;

  [[nodiscard]] bool complete() const {
    return init != nullptr && device_count != nullptr && device_get != nullptr &&
           context_create != nullptr && context_destroy != nullptr && context_push != nullptr &&
           context_pop != nullptr;
  }
};

/// Both libraries, loaded once, plus the answer to "can this machine encode".
///
/// Held as a singleton because the answer cannot change while the process
/// runs: a card does not appear, and a driver that mismatches its own kernel
/// module keeps mismatching until the machine is rebooted.
class Nvenc {
 public:
  static Nvenc& instance() {
    static Nvenc nvenc;
    return nvenc;
  }

  [[nodiscard]] bool available() const { return available_; }
  [[nodiscard]] const std::string& detail() const { return detail_; }
  [[nodiscard]] const std::string& device_name() const { return device_name_; }
  [[nodiscard]] const NV_ENCODE_API_FUNCTION_LIST& api() const { return api_; }
  [[nodiscard]] const CudaApi& cuda() const { return cuda_; }

  /// A CUDA context of its own for each encoder, so that two screen shares in
  /// one process cannot corrupt each other's state.
  [[nodiscard]] void* create_context() {
    void* context = nullptr;
    if (cuda_.context_create(&context, 0, device_) != 0) {
      return nullptr;
    }
    return context;
  }

 private:
  Nvenc() { probe(); }

  void probe() {
    cuda_library_ = ::dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (cuda_library_ == nullptr) {
      detail_ = "libcuda.so.1 is not installed, so there is no NVIDIA driver here";
      return;
    }

    cuda_.init = reinterpret_cast<CudaApi::Init>(::dlsym(cuda_library_, "cuInit"));
    cuda_.device_count =
        reinterpret_cast<CudaApi::DeviceGetCount>(::dlsym(cuda_library_, "cuDeviceGetCount"));
    cuda_.device_get = reinterpret_cast<CudaApi::DeviceGet>(::dlsym(cuda_library_, "cuDeviceGet"));
    cuda_.device_name =
        reinterpret_cast<CudaApi::DeviceGetName>(::dlsym(cuda_library_, "cuDeviceGetName"));
    cuda_.context_create =
        reinterpret_cast<CudaApi::CtxCreate>(::dlsym(cuda_library_, "cuCtxCreate_v2"));
    cuda_.context_destroy =
        reinterpret_cast<CudaApi::CtxDestroy>(::dlsym(cuda_library_, "cuCtxDestroy_v2"));
    cuda_.context_push =
        reinterpret_cast<CudaApi::CtxPushCurrent>(::dlsym(cuda_library_, "cuCtxPushCurrent_v2"));
    cuda_.context_pop =
        reinterpret_cast<CudaApi::CtxPopCurrent>(::dlsym(cuda_library_, "cuCtxPopCurrent_v2"));
    if (!cuda_.complete()) {
      detail_ = "libcuda.so.1 is missing symbols this needs";
      return;
    }

    // 803 is CUDA_ERROR_SYSTEM_DRIVER_MISMATCH, which is what a machine
    // answers after its driver was upgraded and it was not rebooted. Worth
    // naming, because the card is right there and the message otherwise says
    // nothing.
    if (const int status = cuda_.init(0); status != 0) {
      detail_ = status == 803 ? "the NVIDIA driver does not match its own kernel module, which is "
                                "what an upgrade without a reboot leaves behind"
                              : "cuInit failed with " + std::to_string(status);
      return;
    }

    int devices = 0;
    if (cuda_.device_count(&devices) != 0 || devices == 0) {
      detail_ = "the NVIDIA driver is loaded but reports no device";
      return;
    }
    if (cuda_.device_get(&device_, 0) != 0) {
      detail_ = "the first CUDA device could not be opened";
      return;
    }
    if (cuda_.device_name != nullptr) {
      std::array<char, 128> name{};
      if (cuda_.device_name(name.data(), static_cast<int>(name.size()) - 1, device_) == 0) {
        device_name_ = name.data();
      }
    }

    encode_library_ = ::dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    if (encode_library_ == nullptr) {
      detail_ = "libnvidia-encode.so.1 is not installed, so this driver has no encoder";
      return;
    }

    using CreateInstance = NVENCSTATUS (*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto* create =
        reinterpret_cast<CreateInstance>(::dlsym(encode_library_, "NvEncodeAPICreateInstance"));
    using MaxVersion = NVENCSTATUS (*)(uint32_t*);
    auto* max_version =
        reinterpret_cast<MaxVersion>(::dlsym(encode_library_, "NvEncodeAPIGetMaxSupportedVersion"));
    if (create == nullptr) {
      detail_ = "libnvidia-encode.so.1 has no NvEncodeAPICreateInstance";
      return;
    }

    // The driver refuses a header newer than itself, and says so with a
    // version error that is easy to mistake for a broken installation.
    if (max_version != nullptr) {
      uint32_t supported = 0;
      if (max_version(&supported) == NV_ENC_SUCCESS) {
        const uint32_t wanted = (NVENCAPI_MAJOR_VERSION << 4U) | NVENCAPI_MINOR_VERSION;
        if (supported < wanted) {
          detail_ = "the driver supports NVENC API " + std::to_string(supported >> 4U) + "." +
                    std::to_string(supported & 0xFU) + ", older than the " +
                    std::to_string(NVENCAPI_MAJOR_VERSION) + "." +
                    std::to_string(NVENCAPI_MINOR_VERSION) + " this was built against";
          return;
        }
      }
    }

    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (const NVENCSTATUS status = create(&api_); status != NV_ENC_SUCCESS) {
      detail_ = "NvEncodeAPICreateInstance failed with " + std::to_string(status);
      return;
    }

    available_ = true;
    detail_ = device_name_.empty() ? "NVENC is available" : "NVENC on " + device_name_;
  }

  void* cuda_library_ = nullptr;
  void* encode_library_ = nullptr;
  CudaApi cuda_;
  NV_ENCODE_API_FUNCTION_LIST api_{};
  int device_ = 0;
  std::string device_name_;
  std::string detail_ = "not probed";
  bool available_ = false;
};

/// Makes a CUDA context current for as long as it is in scope.
///
/// libwebrtc calls an encoder from its own queue, and CUDA is a per thread
/// affair: without this, the second call from a different thread encodes into
/// nothing.
class ScopedContext {
 public:
  ScopedContext(const CudaApi& cuda, void* context) : cuda_(cuda) {
    pushed_ = context != nullptr && cuda_.context_push(context) == 0;
  }
  ~ScopedContext() {
    if (pushed_) {
      void* previous = nullptr;
      cuda_.context_pop(&previous);
    }
  }

  ScopedContext(const ScopedContext&) = delete;
  ScopedContext& operator=(const ScopedContext&) = delete;
  ScopedContext(ScopedContext&&) = delete;
  ScopedContext& operator=(ScopedContext&&) = delete;

  [[nodiscard]] bool ok() const { return pushed_; }

 private:
  const CudaApi& cuda_;
  bool pushed_ = false;
};

/// H.264 on the card, as a libwebrtc encoder.
class NvencVideoEncoder : public webrtc::VideoEncoder {
 public:
  ~NvencVideoEncoder() override { Release(); }

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const webrtc::VideoEncoder::Settings& /*settings*/) override {
    if (codec_settings == nullptr || codec_settings->codecType != webrtc::kVideoCodecH264) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (codec_settings->width <= 0 || codec_settings->height <= 0) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }

    Release();

    Nvenc& nvenc = Nvenc::instance();
    if (!nvenc.available()) {
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    width_ = codec_settings->width;
    height_ = codec_settings->height;
    framerate_ = std::max<uint32_t>(1, codec_settings->maxFramerate);
    bitrate_bps_ = codec_settings->startBitrate > 0 ? codec_settings->startBitrate * 1000
                                                    : codec_settings->maxBitrate * 1000;
    if (bitrate_bps_ == 0) {
      bitrate_bps_ = 1500000;
    }

    context_ = nvenc.create_context();
    if (context_ == nullptr) {
      DV_LOG_WARN("NVENC: no CUDA context, falling back to software");
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    const ScopedContext current(nvenc.cuda(), context_);
    if (!current.ok()) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open.device = context_;
    open.apiVersion = NVENCAPI_VERSION;
    if (failed("nvEncOpenEncodeSessionEx",
               nvenc.api().nvEncOpenEncodeSessionEx(&open, &session_))) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    if (!configure()) {
      Release();
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    DV_LOG_INFO("NVENC: encoding {}x{} at up to {} fps, {} kbps", width_, height_, framerate_,
                bitrate_bps_ / 1000);
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    Nvenc& nvenc = Nvenc::instance();
    if (session_ != nullptr) {
      const ScopedContext current(nvenc.cuda(), context_);
      if (input_ != nullptr) {
        nvenc.api().nvEncDestroyInputBuffer(session_, input_);
        input_ = nullptr;
      }
      if (output_ != nullptr) {
        nvenc.api().nvEncDestroyBitstreamBuffer(session_, output_);
        output_ = nullptr;
      }
      nvenc.api().nvEncDestroyEncoder(session_);
      session_ = nullptr;
    }
    if (context_ != nullptr) {
      nvenc.cuda().context_destroy(context_);
      context_ = nullptr;
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Encode(const webrtc::VideoFrame& frame,
                 const std::vector<webrtc::VideoFrameType>* frame_types) override {
    if (session_ == nullptr) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }

    webrtc::EncodedImageCallback* callback = nullptr;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      callback = callback_;
    }
    if (callback == nullptr) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }

    const bool keyframe = frame_types != nullptr && !frame_types->empty() &&
                          frame_types->front() == webrtc::VideoFrameType::kVideoFrameKey;

    Nvenc& nvenc = Nvenc::instance();
    const ScopedContext current(nvenc.cuda(), context_);
    if (!current.ok()) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // The frame arrives as I420 and NVENC takes the same three planes under
    // the name IYUV, so there is no colour conversion here at all: the copy
    // below is the only thing between the capture buffer and the card.
    webrtc::scoped_refptr<const webrtc::I420BufferInterface> buffer =
        frame.video_frame_buffer()->ToI420();
    if (buffer == nullptr) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (buffer->width() != width_ || buffer->height() != height_) {
      // A frame of a size this session was not initialised for. libwebrtc
      // normally reinitialises the encoder when the resolution changes, so
      // this is the unexpected case, and the honest answer is the code that
      // asks the wrapper to carry on in software.
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    if (!copy_into_input(buffer)) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    NV_ENC_PIC_PARAMS picture = {};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputBuffer = input_;
    picture.outputBitstream = output_;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_IYUV;
    picture.inputWidth = static_cast<uint32_t>(width_);
    picture.inputHeight = static_cast<uint32_t>(height_);
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = frame.rtp_timestamp();
    if (keyframe) {
      picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    if (failed("nvEncEncodePicture", nvenc.api().nvEncEncodePicture(session_, &picture))) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    return deliver(frame, callback);
  }

  void SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) override {
    if (session_ == nullptr) {
      return;
    }
    const uint32_t bitrate = parameters.bitrate.get_sum_bps();
    const uint32_t framerate =
        std::max<uint32_t>(1, static_cast<uint32_t>(parameters.framerate_fps + 0.5));
    if (bitrate == 0 || (bitrate == bitrate_bps_ && framerate == framerate_)) {
      return;
    }

    bitrate_bps_ = bitrate;
    framerate_ = framerate;

    Nvenc& nvenc = Nvenc::instance();
    const ScopedContext current(nvenc.cuda(), context_);
    if (!current.ok()) {
      return;
    }

    // Reconfiguring rather than recreating: a new session would start with a
    // keyframe, and the congestion controller changes the rate every second.
    NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
    reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    apply_rate_control();
    reconfigure.reInitEncodeParams = init_params_;
    (void)failed("nvEncReconfigureEncoder",
                 nvenc.api().nvEncReconfigureEncoder(session_, &reconfigure));
  }

  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.implementation_name = "NVENC";
    info.is_hardware_accelerated = true;
    info.supports_native_handle = false;
    info.supports_simulcast = false;
    // The card's own rate control is what is driving the bitrate, so libwebrtc
    // should not second guess it by dropping frames on top.
    info.has_trusted_rate_controller = true;
    info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
    // NVENC wants even dimensions, like every H.264 encoder.
    info.requested_resolution_alignment = 2;
    return info;
  }

 private:
  [[nodiscard]] static bool failed(const char* what, NVENCSTATUS status) {
    if (status == NV_ENC_SUCCESS) {
      return false;
    }
    DV_LOG_WARN("NVENC: {} failed with {}", what, static_cast<int>(status));
    return true;
  }

  /// Fills `init_params_` rate control from the current bitrate and framerate.
  void apply_rate_control() {
    config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config_.rcParams.averageBitRate = bitrate_bps_;
    // A one second buffer: enough for the encoder to spend bits on a frame
    // that needs them, small enough that it cannot run far ahead of the link.
    config_.rcParams.vbvBufferSize = bitrate_bps_;
    config_.rcParams.vbvInitialDelay = bitrate_bps_;
    config_.rcParams.maxBitRate = bitrate_bps_;
    init_params_.frameRateNum = framerate_;
    init_params_.frameRateDen = 1;
    init_params_.encodeConfig = &config_;
  }

  [[nodiscard]] bool configure() {
    Nvenc& nvenc = Nvenc::instance();

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    // P3 is the middle of the speed and quality range, and low latency tuning
    // is what turns off everything that would buffer frames to look ahead.
    if (failed("nvEncGetEncodePresetConfigEx",
               nvenc.api().nvEncGetEncodePresetConfigEx(session_, NV_ENC_CODEC_H264_GUID,
                                                        NV_ENC_PRESET_P3_GUID,
                                                        NV_ENC_TUNING_INFO_LOW_LATENCY, &preset))) {
      return false;
    }
    config_ = preset.presetCfg;

    init_params_ = {};
    init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params_.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init_params_.presetGUID = NV_ENC_PRESET_P3_GUID;
    init_params_.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init_params_.encodeWidth = static_cast<uint32_t>(width_);
    init_params_.encodeHeight = static_cast<uint32_t>(height_);
    init_params_.darWidth = static_cast<uint32_t>(width_);
    init_params_.darHeight = static_cast<uint32_t>(height_);
    init_params_.maxEncodeWidth = static_cast<uint32_t>(width_);
    init_params_.maxEncodeHeight = static_cast<uint32_t>(height_);
    // Synchronous: the encoded frame is ready when nvEncEncodePicture returns,
    // which is what lets Encode hand it to libwebrtc on the same call rather
    // than needing a thread of its own to wait on an event.
    init_params_.enableEncodeAsync = 0;
    init_params_.enablePTD = 1;

    // The profile the offer names, 42e01f, is constrained baseline. Encoding
    // anything richer than what was negotiated is how a receiver ends up with
    // a stream it cannot decode.
    config_.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    // No B frames: they reorder, and a call cannot wait for a frame that has
    // not been captured yet.
    config_.frameIntervalP = 1;
    config_.gopLength = NVENC_INFINITE_GOPLENGTH;
    config_.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    // Every intra frame carries its own parameter sets, so a viewer that joins
    // mid transmission can decode the first one it receives.
    config_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    config_.encodeCodecConfig.h264Config.outputAUD = 0;
    config_.encodeCodecConfig.h264Config.sliceMode = 0;
    config_.encodeCodecConfig.h264Config.sliceModeData = 0;
    apply_rate_control();

    if (failed("nvEncInitializeEncoder",
               nvenc.api().nvEncInitializeEncoder(session_, &init_params_))) {
      return false;
    }

    NV_ENC_CREATE_INPUT_BUFFER input = {};
    input.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    input.width = static_cast<uint32_t>(width_);
    input.height = static_cast<uint32_t>(height_);
    input.bufferFmt = NV_ENC_BUFFER_FORMAT_IYUV;
    if (failed("nvEncCreateInputBuffer", nvenc.api().nvEncCreateInputBuffer(session_, &input))) {
      return false;
    }
    input_ = input.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER output = {};
    output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (failed("nvEncCreateBitstreamBuffer",
               nvenc.api().nvEncCreateBitstreamBuffer(session_, &output))) {
      return false;
    }
    output_ = output.bitstreamBuffer;
    return true;
  }

  [[nodiscard]] bool copy_into_input(
      const webrtc::scoped_refptr<const webrtc::I420BufferInterface>& buffer) {
    Nvenc& nvenc = Nvenc::instance();

    NV_ENC_LOCK_INPUT_BUFFER lock = {};
    lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lock.inputBuffer = input_;
    if (failed("nvEncLockInputBuffer", nvenc.api().nvEncLockInputBuffer(session_, &lock))) {
      return false;
    }

    // IYUV in NVENC's buffer is the three planes one after the other, the
    // chroma ones at half the pitch and half the height.
    auto* destination = static_cast<std::uint8_t*>(lock.bufferDataPtr);
    const uint32_t pitch = lock.pitch;
    copy_plane(destination, pitch, buffer->DataY(), buffer->StrideY(), width_, height_);

    std::uint8_t* chroma = destination + (static_cast<std::size_t>(pitch) * height_);
    const uint32_t chroma_pitch = pitch / 2;
    const int chroma_width = (width_ + 1) / 2;
    const int chroma_height = (height_ + 1) / 2;
    copy_plane(chroma, chroma_pitch, buffer->DataU(), buffer->StrideU(), chroma_width,
               chroma_height);
    chroma += static_cast<std::size_t>(chroma_pitch) * chroma_height;
    copy_plane(chroma, chroma_pitch, buffer->DataV(), buffer->StrideV(), chroma_width,
               chroma_height);

    return !failed("nvEncUnlockInputBuffer", nvenc.api().nvEncUnlockInputBuffer(session_, input_));
  }

  static void copy_plane(std::uint8_t* destination, uint32_t destination_pitch,
                         const std::uint8_t* source, int source_stride, int width, int height) {
    for (int row = 0; row < height; ++row) {
      std::memcpy(destination + (static_cast<std::size_t>(destination_pitch) * row),
                  source + (static_cast<std::size_t>(source_stride) * row),
                  static_cast<std::size_t>(width));
    }
  }

  /// Takes the bitstream off the card and hands it to libwebrtc.
  int32_t deliver(const webrtc::VideoFrame& frame, webrtc::EncodedImageCallback* callback) {
    Nvenc& nvenc = Nvenc::instance();

    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = output_;
    lock.doNotWait = 0;
    if (failed("nvEncLockBitstream", nvenc.api().nvEncLockBitstream(session_, &lock))) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    webrtc::EncodedImage image;
    image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
        static_cast<const std::uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes));
    image._encodedWidth = static_cast<uint32_t>(width_);
    image._encodedHeight = static_cast<uint32_t>(height_);
    image.SetRtpTimestamp(frame.rtp_timestamp());
    image.SetColorSpace(frame.color_space());
    image.capture_time_ms_ = frame.render_time_ms();
    image.ntp_time_ms_ = frame.ntp_time_ms();
    image.rotation_ = frame.rotation();
    image._frameType =
        lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I
            ? webrtc::VideoFrameType::kVideoFrameKey
            : webrtc::VideoFrameType::kVideoFrameDelta;
    image.qp_ = static_cast<int>(lock.frameAvgQP);

    webrtc::CodecSpecificInfo codec_specific;
    codec_specific.codecType = webrtc::kVideoCodecH264;
    // Non interleaved is what the SDP negotiates with
    // packetization-mode=1, and what lets a NAL unit larger than the MTU be
    // fragmented instead of dropped.
    codec_specific.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;
    codec_specific.codecSpecific.H264.idr_frame =
        image._frameType == webrtc::VideoFrameType::kVideoFrameKey;

    const webrtc::EncodedImageCallback::Result result =
        callback->OnEncodedImage(image, &codec_specific);

    if (failed("nvEncUnlockBitstream", nvenc.api().nvEncUnlockBitstream(session_, output_))) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (result.error != webrtc::EncodedImageCallback::Result::OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }

  std::mutex mutex_;
  webrtc::EncodedImageCallback* callback_ = nullptr;

  void* context_ = nullptr;
  void* session_ = nullptr;
  NV_ENC_INPUT_PTR input_ = nullptr;
  NV_ENC_OUTPUT_PTR output_ = nullptr;
  NV_ENC_INITIALIZE_PARAMS init_params_{};
  NV_ENC_CONFIG config_{};

  int width_ = 0;
  int height_ = 0;
  uint32_t framerate_ = 30;
  uint32_t bitrate_bps_ = 0;
};

}  // namespace

HardwareEncoderSupport hardware_encoder_support() {
  const Nvenc& nvenc = Nvenc::instance();
  return HardwareEncoderSupport{
      .compiled_in = true,
      .available = nvenc.available(),
      .implementation = "NVENC",
      .detail = nvenc.detail(),
  };
}

std::unique_ptr<webrtc::VideoEncoder> create_hardware_encoder(
    const webrtc::Environment& /*env*/, const webrtc::SdpVideoFormat& format) {
  if (!Nvenc::instance().available()) {
    return nullptr;
  }
  // H.264 only. VP8, VP9 and AV1 are all things this card can do and this
  // project does not negotiate, so asking the hardware for them would be
  // answering a question nobody asked.
  if (!absl::EqualsIgnoreCase(format.name, "H264")) {
    return nullptr;
  }
  return std::make_unique<NvencVideoEncoder>();
}

}  // namespace dv::client::media

#endif  // DV_WITH_NVENC
