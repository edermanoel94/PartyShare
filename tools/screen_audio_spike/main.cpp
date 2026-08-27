// Phase 0 of the shared screen audio work. See docs/09-screen-audio.md.
//
// O plano inteiro depende de duas afirmações sobre o libwebrtc do dist m152.
// Este executável prova ou desmente as duas, sem placa de som e sem rede:
//
//   1. PeerConnectionFactoryDependencies::audio_frame_processor é de fato
//      honrado. O campo existe no cabeçalho; isso não quer dizer que
//      EnableMedia() e CreateModularPeerConnectionFactory() o usem.
//   2. Um AudioFrame estéreo devolvido por esse processador chega estéreo do
//      outro lado, com o Opus negociado em stereo=1.
//
// E, de brinde, a terceira afirmação da seção 1 do documento: que o processador
// roda DEPOIS do módulo de processamento de áudio (AEC3, supressão de ruído,
// AGC), que é o que permite misturar música sem que o supressor a destrua.
//
// Como funciona
// -------------
// Duas PeerConnections no mesmo processo, ligadas por candidatos de host em
// loopback. Nenhuma placa de som participa: as duas pontas usam um
// AudioDeviceModule próprio, construído sobre AudioDeviceModuleDefault.
//
//   emissor   ADM injeta um tom de 20 Hz  ->  APM  ->  AudioFrameProcessor
//             o processador descarta o que recebeu e escreve estéreo:
//             440 Hz à esquerda, 1000 Hz à direita           ->  Opus  ->  RTP
//
//   receptor  RTP -> Opus -> mixer -> ADM puxa 2 canais e mede a energia
//             em 440 e em 1000 Hz, em cada canal
//
// O tom de entrada é de 20 Hz porque o filtro passa-alta do APM o destrói. Se o
// processador enxergar 20 Hz intactos, ele roda antes do APM; se enxergar quase
// silêncio, roda depois. A ordem também é medida diretamente, por um
// CustomProcessing instalado como pós-processamento de captura: quem for
// chamado primeiro pega a senha 0.
//
// O veredito de estéreo é uma medida só: se o canal esquerdo tiver 440 Hz e o
// direito tiver 1000 Hz, então o que o processador escreveu chegou à outra
// ponta e chegou com dois canais distintos. Um caminho mono entregaria os dois
// canais idênticos.
//
// Modo mixer: `screen-audio-spike mixer`
// -------------------------------------
// A fase 2 usa o mesmo arranjo para uma pergunta diferente: não mais "isto é
// possível", e sim "o que foi construído sobre aquela resposta funciona". No
// lugar do processador escrito aqui entram o audio::ScreenAudioMixer e o
// media::ScreenAudioFrameProcessor do produto, e o áudio de tela é empurrado
// pela mesma costura que a captura do Windows usa. Sem WASAPI: que a captura
// ouve o que um processo toca já está medido em
// tests/integration/test_loopback_capture.cpp.
//
//   emissor   ADM injeta 300 Hz  ->  ScreenAudioMixer  ->  Opus  ->  RTP
//                                        + 440 Hz esquerda
//                                        + 1000 Hz direita
//
// O que se espera do outro lado: 300 Hz nos dois canais, porque a voz é mono e
// vai para os dois ouvidos, mais 440 só à esquerda e 1000 só à direita. O APM
// fica desligado nesse modo - a pergunta é sobre a mistura, e um supressor de
// ruído em kHigh apagaria um tom constante.
//
// Descartável. Nada em client/ ou server/ depende disto. Enable com
// -DDV_ENABLE_SCREEN_AUDIO_SPIKE=ON.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef WEBRTC_WIN
#include <winsock2.h>
#endif

#include <api/audio/audio_device.h>
#include <api/audio/audio_frame.h>
#include <api/audio/audio_frame_processor.h>
#include <api/audio/audio_processing.h>
#include <api/audio/builtin_audio_processing_builder.h>
#include <api/audio/channel_layout.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/audio_options.h>
#include <api/create_modular_peer_connection_factory.h>
#include <api/enable_media.h>
#include <api/environment/environment.h>
#include <api/environment/environment_factory.h>
#include <api/jsep.h>
#include <api/make_ref_counted.h>
#include <api/media_stream_interface.h>
#include <api/media_types.h>
#include <api/peer_connection_interface.h>
#include <api/rtp_transceiver_direction.h>
#include <api/rtp_transceiver_interface.h>
#include <api/scoped_refptr.h>
#include <api/set_local_description_observer_interface.h>
#include <api/set_remote_description_observer_interface.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <modules/audio_device/include/audio_device_default.h>
#include <rtc_base/logging.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>

#include "audio/screen_audio_mixer.hpp"
#include "webrtc/screen_audio_frame_processor.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;

constexpr int kSampleRate = 48000;
/// Um bloco de 10 ms, que é a moeda do ADM.
constexpr std::size_t kFramesPerBlock = kSampleRate / 100;

/// O que o ADM do emissor injeta. Escolhido abaixo do corte do filtro
/// passa-alta do APM, de propósito: ver o cabeçalho.
constexpr double kMicToneHz = 20.0;
constexpr double kMicToneAmplitude = 0.5;

/// What the two modes are.
enum class Mode : std::uint8_t {
  /// Fase 0. A processor written here, to answer whether the hook exists, when
  /// it runs and whether stereo survives.
  Probe,
  /// Fase 2. The product's own audio::ScreenAudioMixer and
  /// media::ScreenAudioFrameProcessor, to answer whether the thing that was
  /// built on those answers works.
  Mixer,
};

Mode mode = Mode::Probe;

/// In mixer mode the injected microphone is a tone the audio processing lets
/// through, because what is being measured is the mixing rather than the
/// filtering. Twenty hertz would be gone before the mixer ever saw it.
constexpr double kVoiceToneHz = 300.0;
constexpr double kVoiceToneAmplitude = 0.2;

/// O que o processador escreve por cima, um tom por canal.
constexpr double kLeftToneHz = 440.0;
constexpr double kRightToneHz = 1000.0;
constexpr double kToneAmplitude = 0.3;

/// O perfil que o SFU já oferta hoje, palavra por palavra: é a constante
/// DEFAULT_OPUS_AUDIO_PROFILE do libdatachannel, que media_router.cpp herda ao
/// chamar addOpusCodec sem segundo argumento.
constexpr const char* kOpusProfile =
    "minptime=10;maxaveragebitrate=96000;stereo=1;sprop-stereo=1;useinbandfec=1";

constexpr auto kCallDuration = std::chrono::seconds(5);
/// ICE, DTLS e o neteq precisam de um momento antes que a medida signifique
/// alguma coisa.
constexpr auto kWarmup = std::chrono::milliseconds(1500);

int failures = 0;

void report(const char* step, bool ok, const std::string& detail = {}) {
  std::printf("[%s] %-34s %s\n", ok ? " OK " : "FAIL", step, detail.c_str());
  std::fflush(stdout);
  if (!ok) {
    ++failures;
  }
}

void note(const char* step, const std::string& detail) {
  std::printf("[ .. ] %-34s %s\n", step, detail.c_str());
  std::fflush(stdout);
}

// --- as senhas de ordem -------------------------------------------------------

/// Quem for chamado primeiro pega o número mais baixo. Global porque tanto o
/// pós-processamento do APM quanto o processador de frames pertencem à factory
/// depois de construídos, e ler contadores de dentro deles seria mais trabalho
/// do que vale um spike.
struct Order {
  std::atomic<int> next{0};
  std::atomic<int> apm{-1};
  std::atomic<int> processor{-1};
  std::atomic<std::uint64_t> apm_calls{0};
  std::atomic<std::uint64_t> processor_calls{0};
  /// A energia que o processador enxerga chegando, somada, para decidir se o
  /// tom de 20 Hz sobreviveu ao caminho até ele.
  std::atomic<std::uint64_t> processor_rms_micros{0};
  std::atomic<std::uint64_t> processor_rms_blocks{0};
};

Order order;

/// O misturador do produto, um por processo, como no Engine de
/// client/src/webrtc/libwebrtc_media_session.cpp.
dv::client::audio::ScreenAudioMixer& screen_audio_mixer() {
  static dv::client::audio::ScreenAudioMixer mixer;
  return mixer;
}

/// Empurra blocos de audio de tela pela mesma costura que a captura do Windows
/// usa, a cem por segundo.
///
/// Sem WASAPI de proposito. Que a captura ouve o que um processo toca ja esta
/// medido em tests/integration/test_loopback_capture.cpp; o que falta medir e o
/// que acontece com esses blocos depois que eles chegam ao misturador.
class ScreenAudioFeeder {
 public:
  ~ScreenAudioFeeder() { stop(); }

  void start() {
    running_ = true;
    thread_ = std::thread([this] { loop(); });
  }

  void stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void loop() {
    std::vector<std::int16_t> block(kFramesPerBlock * 2, 0);
    auto deadline = std::chrono::steady_clock::now();
    double left_phase = 0;
    double right_phase = 0;
    const double left_step = 2.0 * kPi * kLeftToneHz / kSampleRate;
    const double right_step = 2.0 * kPi * kRightToneHz / kSampleRate;

    while (running_.load()) {
      for (std::size_t i = 0; i < kFramesPerBlock; ++i) {
        block[i * 2] = static_cast<std::int16_t>(std::sin(left_phase) * kToneAmplitude * 32767.0);
        block[(i * 2) + 1] =
            static_cast<std::int16_t>(std::sin(right_phase) * kToneAmplitude * 32767.0);
        left_phase += left_step;
        right_phase += right_step;
      }
      screen_audio_mixer().push_screen_audio(block);
      deadline += std::chrono::milliseconds(10);
      std::this_thread::sleep_until(deadline);
    }
  }

  std::atomic<bool> running_{false};
  std::thread thread_;
};

void take_ticket(std::atomic<int>& slot) {
  int expected = -1;
  if (slot.load(std::memory_order_relaxed) == -1) {
    const int ticket = order.next.fetch_add(1, std::memory_order_relaxed);
    if (!slot.compare_exchange_strong(expected, ticket)) {
      // Outra thread chegou primeiro; a senha tirada aqui se perde, o que só
      // afeta a numeração e não a ordem relativa.
    }
  }
}

/// Instalado como pós-processamento de captura do APM. Não toca no áudio: só
/// registra que o APM passou por aqui, e quando.
class ApmOrderProbe : public webrtc::CustomProcessing {
 public:
  void Initialize(int /*sample_rate_hz*/, int /*num_channels*/) override {}

  void Process(webrtc::AudioBuffer* /*audio*/) override {
    take_ticket(order.apm);
    order.apm_calls.fetch_add(1, std::memory_order_relaxed);
  }

  [[nodiscard]] std::string ToString() const override { return "dv-apm-order-probe"; }
};

/// O que a Opção A do documento propõe, reduzido ao mínimo que prova o ponto:
/// descarta o microfone e escreve dois tons, um por canal.
///
/// Na versão de verdade o microfone é preservado e a música entra por cima. Aqui
/// ele é descartado de propósito, porque um canal esquerdo que contenha só
/// 440 Hz é a prova de que o que chegou na outra ponta veio daqui, e não do ADM.
class SpikeFrameProcessor : public webrtc::AudioFrameProcessor {
 public:
  void Process(std::unique_ptr<webrtc::AudioFrame> frame) override {
    take_ticket(order.processor);
    order.processor_calls.fetch_add(1, std::memory_order_relaxed);

    measure_incoming(*frame);

    const std::size_t samples = frame->samples_per_channel();
    const int rate = frame->sample_rate_hz();

    frame->SetLayoutAndNumChannels(webrtc::CHANNEL_LAYOUT_STEREO, 2);
    auto view = frame->mutable_data(samples, 2);

    const double left_step = 2.0 * kPi * kLeftToneHz / rate;
    const double right_step = 2.0 * kPi * kRightToneHz / rate;
    for (std::size_t i = 0; i < samples; ++i) {
      view[(i * 2)] = to_pcm(std::sin(left_phase_) * kToneAmplitude);
      view[(i * 2) + 1] = to_pcm(std::sin(right_phase_) * kToneAmplitude);
      left_phase_ += left_step;
      right_phase_ += right_step;
    }
    if (left_phase_ > 2.0 * kPi) {
      left_phase_ -= 2.0 * kPi;
    }
    if (right_phase_ > 2.0 * kPi) {
      right_phase_ -= 2.0 * kPi;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    if (sink_) {
      sink_(std::move(frame));
    }
  }

  void SetSink(OnAudioFrameCallback sink) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
  }

 private:
  /// Quanto do tom de 20 Hz sobrou quando o frame chegou aqui.
  static void measure_incoming(const webrtc::AudioFrame& frame) {
    if (frame.muted()) {
      order.processor_rms_blocks.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const auto view = frame.data_view();
    double sum = 0;
    for (std::size_t i = 0; i < view.size(); ++i) {
      const double sample = view[i] / 32768.0;
      sum += sample * sample;
    }
    const double rms = view.size() == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(view.size()));
    order.processor_rms_micros.fetch_add(static_cast<std::uint64_t>(rms * 1e6),
                                         std::memory_order_relaxed);
    order.processor_rms_blocks.fetch_add(1, std::memory_order_relaxed);
  }

  static std::int16_t to_pcm(double value) {
    const double scaled = value * 32767.0;
    if (scaled > 32767.0) {
      return 32767;
    }
    if (scaled < -32768.0) {
      return -32768;
    }
    return static_cast<std::int16_t>(scaled);
  }

  double left_phase_ = 0;
  double right_phase_ = 0;
  std::mutex mutex_;
  OnAudioFrameCallback sink_;
};

// --- medida do outro lado -----------------------------------------------------

/// Correlaciona o que chega com dois tons, canal a canal, em janelas de 100 ms.
///
/// Uma janela em vez de uma correlação sobre a gravação inteira porque a fase
/// escorrega devagar: o neteq estica e encolhe blocos, e uma correlação longa
/// borraria o pico. Cem milissegundos é curto o bastante para a fase se manter e
/// longo o bastante para separar 440 de 1000 Hz com folga.
class ToneAnalyzer {
 public:
  static constexpr std::size_t kWindow = kSampleRate / 10;

  void push(const std::int16_t* interleaved, std::size_t frames) {
    for (std::size_t i = 0; i < frames; ++i) {
      left_.push_back(interleaved[(i * 2)] / 32768.0);
      right_.push_back(interleaved[(i * 2) + 1] / 32768.0);
      if (left_.size() == kWindow) {
        close_window();
      }
    }
  }

  struct Summary {
    double left_at_440 = 0;
    double left_at_1000 = 0;
    double right_at_440 = 0;
    double right_at_1000 = 0;
    double left_at_voice = 0;
    double right_at_voice = 0;
    double channel_difference = 0;
    int windows = 0;
  };

  [[nodiscard]] Summary summary() const {
    Summary result;
    for (const Window& window : windows_) {
      if (window.rms < 0.01) {
        continue;  // silêncio: nada a dizer
      }
      result.left_at_440 += window.left_at_440;
      result.left_at_1000 += window.left_at_1000;
      result.right_at_440 += window.right_at_440;
      result.right_at_1000 += window.right_at_1000;
      result.left_at_voice += window.left_at_voice;
      result.right_at_voice += window.right_at_voice;
      result.channel_difference += window.channel_difference;
      ++result.windows;
    }
    if (result.windows > 0) {
      const auto n = static_cast<double>(result.windows);
      result.left_at_440 /= n;
      result.left_at_1000 /= n;
      result.right_at_440 /= n;
      result.right_at_1000 /= n;
      result.left_at_voice /= n;
      result.right_at_voice /= n;
      result.channel_difference /= n;
    }
    return result;
  }

 private:
  struct Window {
    double left_at_440 = 0;
    double left_at_1000 = 0;
    double right_at_440 = 0;
    double right_at_1000 = 0;
    double left_at_voice = 0;
    double right_at_voice = 0;
    double channel_difference = 0;
    double rms = 0;
  };

  /// Magnitude da correlação com um tom de `hz`, invariante à fase.
  static double magnitude(const std::vector<double>& samples, double hz) {
    double in_phase = 0;
    double quadrature = 0;
    const double step = 2.0 * kPi * hz / kSampleRate;
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const double angle = step * static_cast<double>(i);
      in_phase += samples[i] * std::cos(angle);
      quadrature += samples[i] * std::sin(angle);
    }
    const auto n = static_cast<double>(samples.size());
    return 2.0 * std::sqrt((in_phase * in_phase) + (quadrature * quadrature)) / n;
  }

  static double rms_of(const std::vector<double>& samples) {
    double sum = 0;
    for (const double sample : samples) {
      sum += sample * sample;
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
  }

  void close_window() {
    Window window;
    window.left_at_440 = magnitude(left_, kLeftToneHz);
    window.left_at_1000 = magnitude(left_, kRightToneHz);
    window.right_at_440 = magnitude(right_, kLeftToneHz);
    window.right_at_1000 = magnitude(right_, kRightToneHz);
    window.left_at_voice = magnitude(left_, kVoiceToneHz);
    window.right_at_voice = magnitude(right_, kVoiceToneHz);
    window.rms = std::max(rms_of(left_), rms_of(right_));

    double difference = 0;
    for (std::size_t i = 0; i < left_.size(); ++i) {
      difference += std::abs(left_[i] - right_[i]);
    }
    window.channel_difference = difference / static_cast<double>(left_.size());

    windows_.push_back(window);
    left_.clear();
    right_.clear();
  }

  std::vector<double> left_;
  std::vector<double> right_;
  std::vector<Window> windows_;
};

// --- o dispositivo de áudio falso ---------------------------------------------

/// Um ADM que não fala com o sistema: injeta blocos de 10 ms num lado e puxa
/// blocos de 10 ms no outro, no relógio, e entrega o que puxou ao analisador.
///
/// É o mesmo mecanismo que a Opção B do documento usaria de verdade. Aqui ele
/// só existe para tirar a placa de som do caminho, e para que a entrada do
/// emissor seja um sinal conhecido.
class SpikeAudioDevice
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule> {
 public:
  SpikeAudioDevice(bool record, bool playout, ToneAnalyzer* analyzer)
      : record_(record), playout_(playout), analyzer_(analyzer) {}

  ~SpikeAudioDevice() override { stop(); }

  std::int32_t RegisterAudioCallback(webrtc::AudioTransport* callback) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    transport_ = callback;
    return 0;
  }

  std::int32_t StartRecording() override {
    recording_ = record_;
    ensure_thread();
    return 0;
  }
  bool Recording() const override { return recording_.load(); }
  std::int32_t StopRecording() override {
    recording_ = false;
    return 0;
  }

  std::int32_t StartPlayout() override {
    playing_ = playout_;
    ensure_thread();
    return 0;
  }
  bool Playing() const override { return playing_.load(); }
  std::int32_t StopPlayout() override {
    playing_ = false;
    return 0;
  }

  std::int32_t StereoPlayoutIsAvailable(bool* available) const override {
    *available = true;
    return 0;
  }
  std::int32_t StereoRecordingIsAvailable(bool* available) const override {
    *available = false;
    return 0;
  }

  void stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] std::uint64_t blocks_recorded() const { return blocks_recorded_.load(); }
  [[nodiscard]] std::uint64_t blocks_played() const { return blocks_played_.load(); }

 private:
  void ensure_thread() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
      return;
    }
    running_ = true;
    thread_ = std::thread([this] { loop(); });
  }

  void loop() {
    // Prazo absoluto em vez de sleep_for acumulado: dez milissegundos de deriva
    // por segundo bastam para o neteq começar a esticar blocos, e aí a medida
    // do outro lado deixa de ser sobre o que este spike quer medir.
    auto deadline = std::chrono::steady_clock::now();
    std::vector<std::int16_t> capture(kFramesPerBlock, 0);
    std::vector<std::int16_t> render(kFramesPerBlock * 2, 0);

    while (running_) {
      deadline += std::chrono::milliseconds(10);
      std::this_thread::sleep_until(deadline);

      webrtc::AudioTransport* transport = nullptr;
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
      }
      if (transport == nullptr) {
        continue;
      }

      if (recording_.load()) {
        const double hertz = mode == Mode::Mixer ? kVoiceToneHz : kMicToneHz;
        const double amplitude = mode == Mode::Mixer ? kVoiceToneAmplitude : kMicToneAmplitude;
        const double step = 2.0 * kPi * hertz / kSampleRate;
        for (std::size_t i = 0; i < kFramesPerBlock; ++i) {
          capture[i] = static_cast<std::int16_t>(std::sin(mic_phase_) * amplitude * 32767.0);
          mic_phase_ += step;
        }
        if (mic_phase_ > 2.0 * kPi) {
          mic_phase_ -= 2.0 * kPi;
        }
        std::uint32_t new_mic_level = 0;
        transport->RecordedDataIsAvailable(capture.data(), kFramesPerBlock, sizeof(std::int16_t), 1,
                                           kSampleRate, 0, 0, 0, false, new_mic_level);
        blocks_recorded_.fetch_add(1, std::memory_order_relaxed);
      }

      if (playing_.load()) {
        std::size_t produced = 0;
        std::int64_t elapsed_ms = 0;
        std::int64_t ntp_ms = 0;
        // Dois canais de propósito: é o pedido que revela se o que chegou é
        // estéreo de verdade. Uma fonte mono voltaria com os dois canais
        // idênticos, porque o mixer duplica.
        transport->NeedMorePlayData(kFramesPerBlock, sizeof(std::int16_t) * 2, 2, kSampleRate,
                                    render.data(), produced, &elapsed_ms, &ntp_ms);
        blocks_played_.fetch_add(1, std::memory_order_relaxed);
        // nSamplesOut conta amostras, não quadros: vem como canais vezes
        // amostras por canal. Tratá-lo como quadros lê o dobro do buffer.
        const std::size_t frames = std::min(produced / 2, render.size() / 2);
        if (analyzer_ != nullptr && analyzing_.load() && frames > 0) {
          analyzer_->push(render.data(), frames);
        }
      }
    }
  }

 public:
  /// Liga o analisador depois que ICE, DTLS e o neteq assentaram.
  void begin_analysis() { analyzing_ = true; }

 private:
  const bool record_;
  const bool playout_;
  ToneAnalyzer* const analyzer_;

  std::mutex mutex_;
  webrtc::AudioTransport* transport_ = nullptr;

  std::atomic<bool> started_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> recording_{false};
  std::atomic<bool> playing_{false};
  std::atomic<bool> analyzing_{false};
  std::atomic<std::uint64_t> blocks_recorded_{0};
  std::atomic<std::uint64_t> blocks_played_{0};
  double mic_phase_ = 0;
  std::thread thread_;
};

// --- observadores -------------------------------------------------------------

class LocalDescriptionObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    error_ = error.message();
    ok_ = error.ok();
    done_ = true;
  }
  [[nodiscard]] bool done() const { return done_; }
  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  std::atomic<bool> done_{false};
  std::atomic<bool> ok_{false};
  std::string error_;
};

class RemoteDescriptionObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    error_ = error.message();
    ok_ = error.ok();
    done_ = true;
  }
  [[nodiscard]] bool done() const { return done_; }
  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  std::atomic<bool> done_{false};
  std::atomic<bool> ok_{false};
  std::string error_;
};

class OfferObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  void OnSuccess(webrtc::SessionDescriptionInterface* description) override {
    description->ToString(&sdp_);
    done_ = true;
  }
  void OnFailure(webrtc::RTCError error) override {
    error_ = error.message();
    done_ = true;
  }
  [[nodiscard]] bool done() const { return done_; }
  [[nodiscard]] const std::string& sdp() const { return sdp_; }
  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  std::atomic<bool> done_{false};
  std::string sdp_;
  std::string error_;
};

/// Guarda os candidatos até que a outra ponta tenha uma descrição remota, que é
/// quando ela aceita recebê-los.
struct CandidateBox {
  std::mutex mutex;
  std::vector<std::string> pending;  // "mid|index|candidate"
};

class Observer : public webrtc::PeerConnectionObserver {
 public:
  explicit Observer(CandidateBox* box) : box_(box) {}

  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnRenegotiationNeeded() override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    std::string sdp;
    if (!candidate->ToString(&sdp)) {
      return;
    }
    const std::lock_guard<std::mutex> lock(box_->mutex);
    box_->pending.push_back(candidate->sdp_mid() + "|" +
                            std::to_string(candidate->sdp_mline_index()) + "|" + sdp);
  }

  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override {
    state_ = state;
  }

  [[nodiscard]] webrtc::PeerConnectionInterface::PeerConnectionState state() const {
    return state_.load();
  }

 private:
  CandidateBox* const box_;
  std::atomic<webrtc::PeerConnectionInterface::PeerConnectionState> state_{
      webrtc::PeerConnectionInterface::PeerConnectionState::kNew};
};

// --- SDP ----------------------------------------------------------------------

/// Reescreve o fmtp do Opus para o perfil que o SFU já oferta.
///
/// A oferta que o libwebrtc escreve sozinho traz Opus mono. Em produção quem
/// oferta é o servidor, com stereo=1, e é essa oferta que decide o que este
/// lado codifica. Sem esta reescrita o spike mediria uma negociação que não é a
/// do produto.
std::string with_opus_profile(const std::string& sdp, std::string* payload_type) {
  const std::size_t rtpmap = sdp.find("opus/48000/2");
  if (rtpmap == std::string::npos) {
    return sdp;
  }
  const std::size_t line_start = sdp.rfind("a=rtpmap:", rtpmap);
  if (line_start == std::string::npos) {
    return sdp;
  }
  const std::size_t pt_start = line_start + std::strlen("a=rtpmap:");
  const std::size_t pt_end = sdp.find(' ', pt_start);
  if (pt_end == std::string::npos) {
    return sdp;
  }
  const std::string pt = sdp.substr(pt_start, pt_end - pt_start);
  if (payload_type != nullptr) {
    *payload_type = pt;
  }

  const std::string fmtp_prefix = "a=fmtp:" + pt + " ";
  std::string result = sdp;
  const std::size_t fmtp = result.find(fmtp_prefix);
  if (fmtp != std::string::npos) {
    const std::size_t value_start = fmtp + fmtp_prefix.size();
    std::size_t value_end = result.find('\r', value_start);
    if (value_end == std::string::npos) {
      value_end = result.find('\n', value_start);
    }
    if (value_end == std::string::npos) {
      return result;
    }
    result.replace(value_start, value_end - value_start, kOpusProfile);
    return result;
  }

  std::size_t insert_at = result.find('\n', rtpmap);
  if (insert_at == std::string::npos) {
    return result;
  }
  ++insert_at;
  result.insert(insert_at, fmtp_prefix + kOpusProfile + "\r\n");
  return result;
}

bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return condition();
}

// --- as duas pontas -----------------------------------------------------------

/// Uma factory completa, com as threads que ela exige e o ADM que a alimenta.
struct Side {
  std::unique_ptr<webrtc::Thread> network;
  std::unique_ptr<webrtc::Thread> worker;
  std::unique_ptr<webrtc::Thread> signaling;
  webrtc::scoped_refptr<SpikeAudioDevice> device;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;

  bool build(const char* name, bool record, bool playout, ToneAnalyzer* analyzer,
             bool with_frame_processor) {
    network = webrtc::Thread::CreateWithSocketServer();
    worker = webrtc::Thread::Create();
    signaling = webrtc::Thread::Create();
    network->SetName((std::string(name) + "-net").c_str(), nullptr);
    worker->SetName((std::string(name) + "-work").c_str(), nullptr);
    signaling->SetName((std::string(name) + "-sig").c_str(), nullptr);
    if (!network->Start() || !worker->Start() || !signaling->Start()) {
      return false;
    }

    device = webrtc::make_ref_counted<SpikeAudioDevice>(record, playout, analyzer);

    webrtc::PeerConnectionFactoryDependencies dependencies;
    dependencies.network_thread = network.get();
    dependencies.worker_thread = worker.get();
    dependencies.signaling_thread = signaling.get();
    dependencies.env = webrtc::CreateEnvironment();
    dependencies.adm = device;
    dependencies.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    dependencies.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    dependencies.video_encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
    dependencies.video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();

    // A mesma configuração de client/src/webrtc/libwebrtc_media_session.cpp, de
    // onde vem a pergunta sobre a ordem.
    //
    // Desligada no modo mixer: la a pergunta e sobre a mistura, e um supressor
    // de ruido em kHigh trata um tom constante como ruido estacionario e o
    // apaga, o que faria o teste falhar pelo motivo errado. Que o processador
    // roda depois do APM ja foi medido no modo probe.
    webrtc::AudioProcessing::Config processing;
    const bool filtering = mode != Mode::Mixer;
    processing.echo_canceller.enabled = filtering;
    processing.noise_suppression.enabled = filtering;
    processing.noise_suppression.level =
        webrtc::AudioProcessing::Config::NoiseSuppression::Level::kHigh;
    processing.high_pass_filter.enabled = filtering;
    processing.gain_controller1.enabled = filtering;
    processing.gain_controller1.mode =
        webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;
    processing.gain_controller1.analog_gain_controller.enabled = filtering;
    processing.pipeline.multi_channel_capture = false;
    processing.pipeline.multi_channel_render = false;
    processing.pipeline.maximum_internal_processing_rate = kSampleRate;

    auto builder = std::make_unique<webrtc::BuiltinAudioProcessingBuilder>(processing);
    if (with_frame_processor) {
      if (mode == Mode::Mixer) {
        // O adaptador do produto, sobre o misturador do produto. Uma copia
        // deles aqui provaria alguma coisa sobre a copia.
        dependencies.audio_frame_processor =
            std::make_unique<dv::client::media::ScreenAudioFrameProcessor>(&screen_audio_mixer());
      } else {
        builder->SetCapturePostProcessing(std::make_unique<ApmOrderProbe>());
        dependencies.audio_frame_processor = std::make_unique<SpikeFrameProcessor>();
      }
    }
    dependencies.audio_processing_builder = std::move(builder);

    webrtc::EnableMedia(dependencies);
    factory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
    return factory != nullptr;
  }

  void shutdown() {
    factory = nullptr;
    if (device != nullptr) {
      device->stop();
    }
    device = nullptr;
    if (signaling) {
      signaling->Stop();
    }
    if (worker) {
      worker->Stop();
    }
    if (network) {
      network->Stop();
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "mixer") == 0) {
    mode = Mode::Mixer;
  }
  // Sem buffer: quando um RTC_CHECK derruba o processo, o buffer do stdout vai
  // junto, e o spike deixa de dizer onde estava.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::printf("\nShared screen audio spike - phase 0\n");
  std::printf("See docs/09-screen-audio.md.\n\n");

  if (std::getenv("DV_SPIKE_VERBOSE") != nullptr) {
    webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
    webrtc::LogMessage::SetLogToStderr(true);
  }

#ifdef WEBRTC_WIN
  // InitializeSSL não abre o Winsock, e sem ele todo socket de ICE falha com
  // WSANOTINITIALISED. O cliente não precisa disto porque o Qt já abriu; um
  // executável nu precisa.
  WSADATA winsock;
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
    report("winsock", false, "WSAStartup falhou");
    return 1;
  }
#endif

  webrtc::InitializeSSL();

  ToneAnalyzer analyzer;

  Side sender;
  Side receiver;

  if (!sender.build("send", /*record=*/true, /*playout=*/false, nullptr,
                    /*with_frame_processor=*/true)) {
    report("factory do emissor", false, "não foi possível criar");
    return 1;
  }
  report("factory do emissor", true, "com audio_frame_processor");

  if (!receiver.build("recv", /*record=*/false, /*playout=*/true, &analyzer,
                      /*with_frame_processor=*/false)) {
    report("factory do receptor", false, "não foi possível criar");
    sender.shutdown();
    return 1;
  }
  report("factory do receptor", true, {});

  CandidateBox sender_candidates;
  CandidateBox receiver_candidates;
  Observer sender_observer(&sender_candidates);
  Observer receiver_observer(&receiver_candidates);

  webrtc::PeerConnectionInterface::RTCConfiguration configuration;
  configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

  auto sender_pc = sender.factory->CreatePeerConnectionOrError(
      configuration, webrtc::PeerConnectionDependencies(&sender_observer));
  auto receiver_pc = receiver.factory->CreatePeerConnectionOrError(
      configuration, webrtc::PeerConnectionDependencies(&receiver_observer));
  if (!sender_pc.ok() || !receiver_pc.ok()) {
    report("peer connections", false, "não foi possível criar");
    sender.shutdown();
    receiver.shutdown();
    return 1;
  }
  auto a = sender_pc.MoveValue();
  auto b = receiver_pc.MoveValue();
  report("peer connections", true, {});

  // As PeerConnections têm de morrer antes das threads que as servem. Parar as
  // threads com elas vivas derruba o processo com STATUS_STACK_BUFFER_OVERRUN,
  // depois de quatro linhas de "Thread was prematurely terminated" — que é o
  // libwebrtc dizendo exatamente isso.
  const auto teardown = [&] {
    if (a != nullptr) {
      a->Close();
      a = nullptr;
    }
    if (b != nullptr) {
      b->Close();
      b = nullptr;
    }
    sender.shutdown();
    receiver.shutdown();
    webrtc::CleanupSSL();
  };

  // O emissor põe a trilha antes de qualquer negociação, como o cliente faz.
  webrtc::AudioOptions audio_options;
  audio_options.echo_cancellation = true;
  audio_options.noise_suppression = true;
  audio_options.auto_gain_control = true;
  auto source = sender.factory->CreateAudioSource(audio_options);
  auto track = sender.factory->CreateAudioTrack("dv-spike", source.get());
  if (track == nullptr || !a->AddTrack(track, {"dv-spike"}).ok()) {
    report("trilha de áudio", false, "não foi possível adicionar");
    teardown();
    return 1;
  }
  report("trilha de áudio", true, {});

  // O receptor oferta, como o SFU faz.
  webrtc::RtpTransceiverInit init;
  init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
  if (!b->AddTransceiver(webrtc::MediaType::AUDIO, init).ok()) {
    report("m-line de recepção", false, "não foi possível adicionar");
    teardown();
    return 1;
  }

  auto offer_observer = webrtc::make_ref_counted<OfferObserver>();
  b->CreateOffer(offer_observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  if (!wait_until([&] { return offer_observer->done(); }, std::chrono::seconds(5)) ||
      offer_observer->sdp().empty()) {
    report("oferta", false, offer_observer->error());
    teardown();
    return 1;
  }

  std::string payload_type;
  const std::string offer_sdp = with_opus_profile(offer_observer->sdp(), &payload_type);
  const bool offer_is_stereo = offer_sdp.find("stereo=1") != std::string::npos;
  report("oferta com o perfil do SFU", offer_is_stereo,
         "Opus no payload type " + payload_type + ", " + kOpusProfile);

  auto b_local = webrtc::make_ref_counted<LocalDescriptionObserver>();
  {
    webrtc::SdpParseError parse_error;
    auto description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, offer_sdp, &parse_error);
    if (description == nullptr) {
      report("oferta reescrita", false, parse_error.description);
      teardown();
      return 1;
    }
    b->SetLocalDescription(std::move(description), b_local);
  }
  if (!wait_until([&] { return b_local->done(); }, std::chrono::seconds(5)) || !b_local->ok()) {
    report("oferta aplicada no receptor", false, b_local->error());
    teardown();
    return 1;
  }

  auto a_remote = webrtc::make_ref_counted<RemoteDescriptionObserver>();
  {
    webrtc::SdpParseError parse_error;
    auto description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, offer_sdp, &parse_error);
    a->SetRemoteDescription(std::move(description), a_remote);
  }
  if (!wait_until([&] { return a_remote->done(); }, std::chrono::seconds(5)) || !a_remote->ok()) {
    report("oferta aplicada no emissor", false, a_remote->error());
    teardown();
    return 1;
  }

  auto a_local = webrtc::make_ref_counted<LocalDescriptionObserver>();
  a->SetLocalDescription(a_local);
  if (!wait_until([&] { return a_local->done(); }, std::chrono::seconds(5)) || !a_local->ok()) {
    report("resposta", false, a_local->error());
    teardown();
    return 1;
  }

  std::string answer_sdp;
  a->local_description()->ToString(&answer_sdp);
  // Anotação, não veredito. O fmtp que o emissor escreve na resposta diz o que
  // ele aceita RECEBER, e esta m-line é sendonly para ele. O que decide se ele
  // CODIFICA em estéreo é o stereo=1 da oferta, que já foi verificado acima. A
  // prova de que os dois canais existem é a medida 2b, na outra ponta.
  const bool answer_is_stereo = answer_sdp.find("stereo=1") != std::string::npos;
  note("resposta", answer_is_stereo ? "traz stereo=1" : "sem stereo=1 no fmtp (não decide nada)");

  auto b_remote = webrtc::make_ref_counted<RemoteDescriptionObserver>();
  {
    webrtc::SdpParseError parse_error;
    auto description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, answer_sdp, &parse_error);
    b->SetRemoteDescription(std::move(description), b_remote);
  }
  if (!wait_until([&] { return b_remote->done(); }, std::chrono::seconds(5)) || !b_remote->ok()) {
    report("resposta aplicada", false, b_remote->error());
    teardown();
    return 1;
  }

  // Candidatos, dos dois lados, até a conexão fechar.
  const auto drain = [](CandidateBox& box, webrtc::PeerConnectionInterface* target) {
    std::vector<std::string> batch;
    {
      const std::lock_guard<std::mutex> lock(box.mutex);
      batch.swap(box.pending);
    }
    for (const std::string& entry : batch) {
      const std::size_t first = entry.find('|');
      const std::size_t second = entry.find('|', first + 1);
      if (first == std::string::npos || second == std::string::npos) {
        continue;
      }
      const std::string mid = entry.substr(0, first);
      const int index = std::stoi(entry.substr(first + 1, second - first - 1));
      const std::string sdp = entry.substr(second + 1);
      webrtc::SdpParseError parse_error;
      std::unique_ptr<webrtc::IceCandidate> candidate(
          webrtc::CreateIceCandidate(mid, index, sdp, &parse_error));
      if (candidate != nullptr) {
        target->AddIceCandidate(std::move(candidate), [](webrtc::RTCError) {});
      }
    }
  };

  const bool connected = wait_until(
      [&] {
        drain(sender_candidates, b.get());
        drain(receiver_candidates, a.get());
        return sender_observer.state() ==
                   webrtc::PeerConnectionInterface::PeerConnectionState::kConnected &&
               receiver_observer.state() ==
                   webrtc::PeerConnectionInterface::PeerConnectionState::kConnected;
      },
      std::chrono::seconds(15));
  report("ICE e DTLS", connected, connected ? "as duas pontas conectadas" : "não conectou");
  if (!connected) {
    teardown();
    return 1;
  }

  // Quantos canais o codificador do emissor realmente ficou. É o número que
  // decide se um frame estéreo entregue pelo processador vale alguma coisa: com
  // um canal, RemixAndResample mistura os dois num só antes do Opus.
  for (const auto& rtp_sender : a->GetSenders()) {
    const auto parameters = rtp_sender->GetParameters();
    for (const auto& codec : parameters.codecs) {
      note("codec do emissor", codec.name + ", " + std::to_string(codec.num_channels.value_or(0)) +
                                   " canal(is), pt " + std::to_string(codec.payload_type));
    }
  }

  ScreenAudioFeeder feeder;
  if (mode == Mode::Mixer) {
    feeder.start();
  }

  std::this_thread::sleep_for(kWarmup);
  receiver.device->begin_analysis();
  note("medindo", std::to_string(kCallDuration.count()) + " segundos de áudio");
  std::this_thread::sleep_for(kCallDuration);

  feeder.stop();

  // --- veredito ---------------------------------------------------------------

  std::printf("\n");

  const ToneAnalyzer::Summary summary = analyzer.summary();
  {
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "esquerda 440=%.4f 1000=%.4f voz=%.4f | direita 440=%.4f 1000=%.4f voz=%.4f "
                  "| %d janelas",
                  summary.left_at_440, summary.left_at_1000, summary.left_at_voice,
                  summary.right_at_440, summary.right_at_1000, summary.right_at_voice,
                  summary.windows);
    note("energia recebida", detail);
  }

  if (mode == Mode::Probe) {
    const std::uint64_t processor_calls = order.processor_calls.load();
    report("1. audio_frame_processor e chamado", processor_calls > 0,
           std::to_string(processor_calls) + " frames processados, " +
               std::to_string(sender.device->blocks_recorded()) + " blocos injetados");

    const int apm_ticket = order.apm.load();
    const int processor_ticket = order.processor.load();
    if (apm_ticket < 0) {
      note("   ordem contra o APM",
           "o pos-processamento de captura nunca foi chamado; ver o tom de 20 Hz");
    } else {
      report("   roda depois do APM", apm_ticket < processor_ticket,
             "senha do APM " + std::to_string(apm_ticket) + ", do processador " +
                 std::to_string(processor_ticket) + " (" + std::to_string(order.apm_calls.load()) +
                 " chamadas ao APM)");
    }

    const std::uint64_t rms_blocks = order.processor_rms_blocks.load();
    const double incoming_rms = rms_blocks == 0
                                    ? 0.0
                                    : static_cast<double>(order.processor_rms_micros.load()) / 1e6 /
                                          static_cast<double>(rms_blocks);
    {
      char detail[256];
      std::snprintf(detail, sizeof(detail),
                    "injetado %.3f a 20 Hz, o processador ve %.4f (%.1f%% do que entrou)",
                    kMicToneAmplitude / std::sqrt(2.0), incoming_rms,
                    100.0 * incoming_rms / (kMicToneAmplitude / std::sqrt(2.0)));
      note("   evidencia do tom de 20 Hz", detail);
    }

    const bool audio_arrived =
        summary.windows > 0 && (summary.left_at_440 > 0.02 || summary.right_at_1000 > 0.02);
    report("2a. o audio do processador chega", audio_arrived,
           std::to_string(receiver.device->blocks_played()) + " blocos puxados no receptor");

    const bool stereo = audio_arrived && summary.left_at_440 > 4.0 * summary.left_at_1000 &&
                        summary.right_at_1000 > 4.0 * summary.right_at_440 &&
                        summary.channel_difference > 0.01;
    {
      char detail[256];
      std::snprintf(detail, sizeof(detail), "diferenca media entre os canais %.4f",
                    summary.channel_difference);
      report("2b. chega estereo", stereo, detail);
    }

    std::printf("\n");
    if (failures == 0) {
      std::printf("Veredito: a Opcao A do documento se sustenta.\n");
      std::printf("A mistura entra por AudioFrameProcessor, depois do APM, em estereo,\n");
      std::printf("sem tocar no servidor.\n\n");
    } else {
      std::printf("Veredito: %d verificacao(oes) falharam na fase 0.\n\n", failures);
    }
  } else {
    // O microfone injetado chega ao misturador, e que ele o tenha visto e a
    // prova de que mix() rodou de verdade e nao foi contornado.
    const double level = screen_audio_mixer().microphone_level();
    {
      char detail[128];
      std::snprintf(detail, sizeof(detail), "nivel do microfone, medido antes da mistura: %.4f",
                    level);
      report("1. o misturador do produto roda", level > 0.05, detail);
    }

    // A voz e mono e vai para os dois ouvidos; o audio da tela mantem os seus.
    const bool voice_both_ears = summary.left_at_voice > 0.05 && summary.right_at_voice > 0.05 &&
                                 std::abs(summary.left_at_voice - summary.right_at_voice) < 0.05;
    {
      char detail[192];
      std::snprintf(detail, sizeof(detail), "%.0f Hz: esquerda %.4f, direita %.4f", kVoiceToneHz,
                    summary.left_at_voice, summary.right_at_voice);
      report("2. a voz chega nos dois canais", voice_both_ears, detail);
    }

    const bool screen_stereo = summary.left_at_440 > 0.05 && summary.right_at_1000 > 0.05 &&
                               summary.left_at_440 > 4.0 * summary.left_at_1000 &&
                               summary.right_at_1000 > 4.0 * summary.right_at_440;
    {
      char detail[192];
      std::snprintf(detail, sizeof(detail),
                    "440 a esquerda %.4f (vazamento %.4f), 1000 a direita %.4f (vazamento %.4f)",
                    summary.left_at_440, summary.left_at_1000, summary.right_at_1000,
                    summary.right_at_440);
      report("3. o audio da tela chega em estereo", screen_stereo, detail);
    }

    std::printf("\n");
    if (failures == 0) {
      std::printf("Veredito: a fase 2 entrega o que a fase 0 disse ser possivel.\n");
      std::printf("A voz e o audio da tela viajam numa trilha so, cada um no seu lugar.\n\n");
    } else {
      std::printf("Veredito: %d verificacao(oes) falharam na mistura.\n\n", failures);
    }
  }

  teardown();
  return failures == 0 ? 0 : 1;
}
