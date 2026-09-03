#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

#include <rtc/rtc.hpp>

namespace dv::server::sfu {

/// Repairs a stream arriving at the SFU by asking its sender for what went
/// missing.
///
/// libdatachannel answers a NACK for us, through rtc::RtcpNackResponder, but it
/// never asks for one. For an SFU that gap matters more than for a client:
/// what the SFU forwards is what it got, so a hole here is a hole at every
/// listener at once. So the SFU repairs the incoming stream instead. A gap in
/// the sequence numbers becomes an RTCP Generic NACK, RFC 4585 section 6.2.1,
/// and the sender, which keeps a history of what it sent, puts the packet back
/// on the wire. What the SFU then forwards is a stream with the hole filled.
///
/// Written for the screen share, where a viewer that misses a packet can only
/// ask for a new intra frame, and an intra frame of a 720p screen is more than
/// a hundred packets that almost never all arrive on a 5% loss link; the
/// measurement is in docs/11-benchmarks.md. The audio installs the same thing
/// since docs/16-audio-plan.md, step 2: a lost audio packet is twenty
/// milliseconds the listener's jitter buffer has to invent, and RED only
/// covers the isolated ones.
///
/// A missing packet is asked for a few times and then given up on, because a
/// packet that has not arrived after a couple of round trips is one the
/// decoder has already moved past, and asking forever would add traffic to a
/// link that is already losing it.
///
/// Installed on its own on an audio track, or driven by sfu::VideoFeedback on
/// a video one, where the same observation also feeds the bandwidth estimate.
class LossRepair final : public rtc::MediaHandler {
 public:
  struct Options {
    /// A jump larger than this is treated as the stream having restarted
    /// rather than as a thousand lost packets. A screen share that stops and
    /// starts is exactly that case.
    std::uint16_t max_gap = 256;
    /// How many times one packet is asked for before it is written off.
    int max_requests = 3;
    /// The wait between two requests for the same packet. Roughly a round trip
    /// on a bad link: asking again sooner only duplicates a retransmission
    /// that is already on its way.
    std::chrono::milliseconds retry_after{40};
    /// After this long the packet is written off even if it was never asked
    /// for the full number of times.
    std::chrono::milliseconds give_up_after{500};
  };

  using Clock = std::chrono::steady_clock;

  LossRepair() : LossRepair(Options{}) {}
  explicit LossRepair(Options options) : options_(options) {}

  /// Takes every RTP packet in `messages` into account, asks for whatever is
  /// now worth asking for, and leaves the packets themselves untouched for
  /// the handler after this one.
  void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;

  /// The two halves of `incoming`, for a caller with its own reason to walk
  /// the packets.
  ///
  /// `observe` takes one RTP packet into account. It returns nothing: what it
  /// produces is a change to the pending set, which `request` then acts on by
  /// sending one NACK for everything now worth asking for, if anything is.
  void observe(std::uint32_t ssrc, std::uint16_t sequence_number, Clock::time_point now);
  void request(const rtc::message_callback& send, Clock::time_point now);

  /// How many RTCP NACK packets were put on the wire.
  [[nodiscard]] std::uint64_t requests_sent() const {
    return requests_sent_.load(std::memory_order_relaxed);
  }
  /// How many packets were noticed missing, whether or not they came back.
  [[nodiscard]] std::uint64_t packets_missing() const {
    return packets_missing_.load(std::memory_order_relaxed);
  }
  /// How many of those did come back, which is the number that says whether
  /// any of this is working.
  [[nodiscard]] std::uint64_t packets_repaired() const {
    return packets_repaired_.load(std::memory_order_relaxed);
  }
  /// How many RTP packets went past, across every source.
  [[nodiscard]] std::uint64_t packets_seen() const {
    return packets_seen_.load(std::memory_order_relaxed);
  }
  /// The source being tracked, or zero before the first packet.
  [[nodiscard]] std::uint32_t ssrc() const { return ssrc_.load(std::memory_order_relaxed); }

 private:
  struct Missing {
    Clock::time_point first_seen;
    Clock::time_point last_requested;
    int requests = 0;
  };

  Options options_;
  std::mutex mutex_;
  std::map<std::uint16_t, Missing> missing_;
  std::uint16_t expected_ = 0;
  bool started_ = false;

  std::atomic<std::uint32_t> ssrc_{0};
  std::atomic<std::uint64_t> packets_seen_{0};
  std::atomic<std::uint64_t> requests_sent_{0};
  std::atomic<std::uint64_t> packets_missing_{0};
  std::atomic<std::uint64_t> packets_repaired_{0};
};

/// Counts the NACKs a receiver sends back on an outgoing track.
///
/// rtc::RtcpNackResponder answers them out of its cache and says nothing, so
/// this sits in front of it in the chain and is how the SFU knows the other
/// half of the loop is closed: a listener that noticed a hole and asked.
class NackObserver final : public rtc::MediaHandler {
 public:
  explicit NackObserver(std::function<void()> on_nack) : on_nack_(std::move(on_nack)) {}

  void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;

 private:
  std::function<void()> on_nack_;
};

}  // namespace dv::server::sfu
