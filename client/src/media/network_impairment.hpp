#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace dv::client::media {

/// Deliberate damage applied to the media path, so that a bad network can be
/// measured instead of imagined.
///
/// Section 22 of SPEC.md asks the call to survive 5% packet loss with a smooth
/// degradation and no drop. The usual way to produce that loss on Linux is
/// `tc netem` on the loopback interface, which scripts/netem.sh does, but that
/// needs root, only exists on Linux, and impairs every process on the machine
/// at once. This is the portable half: the client applies the loss to its own
/// UDP sockets, below DTLS and above the operating system, so what breaks is
/// exactly the one participant's link to the SFU.
///
/// Both halves measure the same thing and neither replaces the other. netem is
/// the honest one, because it also impairs the kernel's own queues and the
/// server side; this one is the one that runs unprivileged, on every platform,
/// and in CI.
///
/// The settings are process wide because the libwebrtc factory is: one engine,
/// one set of sockets, one impairment. They can be changed at any time, which
/// is what lets a test establish a healthy call first and break the network
/// afterwards - the interesting question is not whether a call starts on a bad
/// network, it is whether a running call survives one going bad.
///
/// Inert by default. Nothing in the interface, the configuration file or the
/// command line turns this on; only a caller inside the process can.
struct NetworkImpairment {
  /// Probability that a packet is thrown away, from 0 to 1, applied
  /// independently in each direction. A value of 0.05 means 5% out and 5% in,
  /// which is what a 5% loss link does to a two way flow.
  double loss = 0.0;

  /// Added to every packet, in one direction. A 50 ms delay here is a 100 ms
  /// round trip, which is what a call across a continent looks like.
  std::chrono::milliseconds delay{0};

  /// Spread around `delay`, uniform, so a packet is held for
  /// `delay` +- `jitter`. Never held for less than nothing.
  std::chrono::milliseconds jitter{0};

  [[nodiscard]] bool inert() const noexcept {
    return loss <= 0.0 && delay <= std::chrono::milliseconds::zero() &&
           jitter <= std::chrono::milliseconds::zero();
  }
};

/// What the impairment actually did, as opposed to what it was asked to do.
///
/// A test that asks for 5% loss and then asserts on the call quality is only
/// meaningful if the loss really happened, and a call that never sent a packet
/// would pass it silently. These counters are the evidence.
struct NetworkImpairmentCounters {
  std::uint64_t packets_sent = 0;
  std::uint64_t packets_dropped_outbound = 0;
  std::uint64_t packets_received = 0;
  std::uint64_t packets_dropped_inbound = 0;
  std::uint64_t packets_delayed = 0;
};

/// Takes effect on the next packet, on whatever sockets already exist.
void set_network_impairment(const NetworkImpairment& impairment) noexcept;

[[nodiscard]] NetworkImpairment network_impairment() noexcept;

[[nodiscard]] NetworkImpairmentCounters network_impairment_counters() noexcept;

void reset_network_impairment_counters() noexcept;

/// The draw itself, kept out of the socket so that it can be tested without
/// one.
///
/// splitmix64: three lines, no allocation, no lock, and good enough for
/// deciding whether to throw a packet away. Each socket owns its own, seeded
/// differently, so that two sockets do not lose the same packets.
class ImpairmentSampler {
 public:
  explicit ImpairmentSampler(std::uint64_t seed) noexcept : state_(seed) {}

  /// True when this packet should be lost.
  [[nodiscard]] bool drops(double probability) noexcept {
    if (probability <= 0.0) {
      return false;
    }
    if (probability >= 1.0) {
      return true;
    }
    return uniform() < probability;
  }

  /// How long to hold a packet, given the impairment in force.
  [[nodiscard]] std::chrono::milliseconds holds(const NetworkImpairment& impairment) noexcept {
    if (impairment.jitter <= std::chrono::milliseconds::zero()) {
      return std::max(impairment.delay, std::chrono::milliseconds::zero());
    }
    const auto spread = static_cast<double>(impairment.jitter.count());
    const double offset = ((uniform() * 2.0) - 1.0) * spread;
    // Rounded rather than truncated. Truncation always moves towards zero, so
    // a band asked to average forty milliseconds would average thirty nine and
    // a half, and every latency measured under it would read half a
    // millisecond low.
    const auto held = std::llround(static_cast<double>(impairment.delay.count()) + offset);
    return std::chrono::milliseconds{std::max<std::int64_t>(held, 0)};
  }

 private:
  /// A double in [0, 1).
  [[nodiscard]] double uniform() noexcept {
    constexpr double kScale = 1.0 / 9007199254740992.0;  // 2^-53
    return static_cast<double>(next() >> 11U) * kScale;
  }

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  std::uint64_t state_;
};

/// Counting lives here so that the counters have one owner, and is called from
/// the socket wrapper in client/src/webrtc, which is the only caller. Not part
/// of the interface above.
namespace impairment_internal {

void count_sent(bool dropped) noexcept;
void count_received(bool dropped) noexcept;
void count_delayed() noexcept;

}  // namespace impairment_internal

}  // namespace dv::client::media
