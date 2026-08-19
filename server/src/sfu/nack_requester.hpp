#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

#include <rtc/rtc.hpp>

namespace dv::server::sfu {

/// Asks the sender to send a packet again when one goes missing on the way in.
///
/// libdatachannel answers a NACK for us, through rtc::RtcpNackResponder, but it
/// never asks for one. For an SFU that gap is the difference between a screen
/// share that degrades and one that freezes: a viewer that misses a packet can
/// only ask for a new intra frame, an intra frame of a 720p screen is more than
/// a hundred packets, and on a 5% loss link almost none of them arrive whole.
/// The measurement that led to this class is in docs/benchmarks.md.
///
/// So the SFU repairs the incoming stream instead. A gap in the sequence
/// numbers becomes an RTCP Generic NACK, RFC 4585 section 6.2.1, and the
/// sender, which keeps a history of what it sent, puts the packet back on the
/// wire. What the SFU then forwards is a stream with the hole filled, and the
/// viewer never needs to ask for anything.
///
/// A missing packet is asked for a few times and then given up on, because a
/// packet that has not arrived after a couple of round trips is one the decoder
/// has already moved past, and asking forever would add traffic to a link that
/// is already losing it.
class NackRequester final : public rtc::MediaHandler {
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

  NackRequester() : NackRequester(Options{}) {}
  explicit NackRequester(Options options) : options_(options) {}

  void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;

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

 private:
  using Clock = std::chrono::steady_clock;

  struct Missing {
    Clock::time_point first_seen;
    Clock::time_point last_requested;
    int requests = 0;
  };

  /// Takes one RTP packet into account. Returns nothing: what it produces is a
  /// change to the pending set, which `request` then acts on.
  void observe(std::uint32_t ssrc, std::uint16_t sequence_number, Clock::time_point now);

  /// Sends one NACK for everything now worth asking for, if anything is.
  void request(const rtc::message_callback& send, Clock::time_point now);

  Options options_;
  std::mutex mutex_;
  std::map<std::uint16_t, Missing> missing_;
  std::uint32_t ssrc_ = 0;
  std::uint16_t expected_ = 0;
  bool started_ = false;

  std::atomic<std::uint64_t> requests_sent_{0};
  std::atomic<std::uint64_t> packets_missing_{0};
  std::atomic<std::uint64_t> packets_repaired_{0};
};

}  // namespace dv::server::sfu
