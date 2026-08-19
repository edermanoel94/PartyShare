#include "media/network_impairment.hpp"

#include <atomic>
#include <cstdint>

namespace dv::client::media {
namespace {

/// Held as separate atomics rather than behind a mutex because the read
/// happens on the network thread for every packet, and a packet is the one
/// place in this project where a lock would be paid for tens of thousands of
/// times a second.
///
/// The three settings are therefore not read atomically with respect to each
/// other. A packet caught mid change can see the new loss and the old delay,
/// which is a distinction without a difference for a fault injector.
struct State {
  std::atomic<double> loss{0.0};
  std::atomic<int> delay_ms{0};
  std::atomic<int> jitter_ms{0};

  std::atomic<std::uint64_t> packets_sent{0};
  std::atomic<std::uint64_t> dropped_outbound{0};
  std::atomic<std::uint64_t> packets_received{0};
  std::atomic<std::uint64_t> dropped_inbound{0};
  std::atomic<std::uint64_t> delayed{0};
};

/// A function local static rather than a global, so that it is constructed
/// before its first use whatever order the translation units are initialised
/// in. Sockets are created long after main starts, but nothing here promises
/// that.
State& state() {
  static State state;
  return state;
}

}  // namespace

void set_network_impairment(const NetworkImpairment& impairment) noexcept {
  state().loss.store(impairment.loss, std::memory_order_relaxed);
  state().delay_ms.store(static_cast<int>(impairment.delay.count()), std::memory_order_relaxed);
  state().jitter_ms.store(static_cast<int>(impairment.jitter.count()), std::memory_order_relaxed);
}

NetworkImpairment network_impairment() noexcept {
  return NetworkImpairment{
      .loss = state().loss.load(std::memory_order_relaxed),
      .delay = std::chrono::milliseconds{state().delay_ms.load(std::memory_order_relaxed)},
      .jitter = std::chrono::milliseconds{state().jitter_ms.load(std::memory_order_relaxed)},
  };
}

NetworkImpairmentCounters network_impairment_counters() noexcept {
  return NetworkImpairmentCounters{
      .packets_sent = state().packets_sent.load(std::memory_order_relaxed),
      .packets_dropped_outbound = state().dropped_outbound.load(std::memory_order_relaxed),
      .packets_received = state().packets_received.load(std::memory_order_relaxed),
      .packets_dropped_inbound = state().dropped_inbound.load(std::memory_order_relaxed),
      .packets_delayed = state().delayed.load(std::memory_order_relaxed),
  };
}

void reset_network_impairment_counters() noexcept {
  state().packets_sent.store(0, std::memory_order_relaxed);
  state().dropped_outbound.store(0, std::memory_order_relaxed);
  state().packets_received.store(0, std::memory_order_relaxed);
  state().dropped_inbound.store(0, std::memory_order_relaxed);
  state().delayed.store(0, std::memory_order_relaxed);
}

namespace impairment_internal {

void count_sent(bool dropped) noexcept {
  state().packets_sent.fetch_add(1, std::memory_order_relaxed);
  if (dropped) {
    state().dropped_outbound.fetch_add(1, std::memory_order_relaxed);
  }
}

void count_received(bool dropped) noexcept {
  state().packets_received.fetch_add(1, std::memory_order_relaxed);
  if (dropped) {
    state().dropped_inbound.fetch_add(1, std::memory_order_relaxed);
  }
}

void count_delayed() noexcept {
  state().delayed.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace impairment_internal
}  // namespace dv::client::media
