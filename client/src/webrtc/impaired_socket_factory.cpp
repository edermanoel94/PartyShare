#include "webrtc/impaired_socket_factory.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <api/units/time_delta.h>
#include <rtc_base/async_packet_socket.h>
#include <rtc_base/network/received_packet.h>
#include <rtc_base/socket_address.h>

#include "media/network_impairment.hpp"

namespace dv::client::media {
namespace {

/// Seeds each socket differently, so that two sockets in the same process do
/// not throw away the same packets in lockstep.
std::uint64_t next_seed() {
  static std::atomic<std::uint64_t> counter{0x243F6A8885A308D3ULL};
  return counter.fetch_add(0x9E3779B97F4A7C15ULL, std::memory_order_relaxed);
}

/// One UDP socket with a fault injector in front of it.
///
/// Everything runs on the network thread: libwebrtc creates the socket there,
/// sends from there, and delivers what arrives from there. A held packet is
/// released by a delayed task on that same thread, so the class needs no lock
/// of its own.
class ImpairedUdpSocket : public webrtc::AsyncPacketSocket {
 public:
  ImpairedUdpSocket(std::unique_ptr<webrtc::AsyncPacketSocket> inner,
                    webrtc::Thread* network_thread)
      : inner_(std::move(inner)), network_thread_(network_thread), sampler_(next_seed()) {
    // Everything the socket emits has to be re-emitted by the wrapper, because
    // the wrapper is what the rest of libwebrtc holds.
    inner_->RegisterReceivedPacketCallback(
        [this](webrtc::AsyncPacketSocket*, const webrtc::ReceivedIpPacket& packet) {
          on_packet(packet);
        });
    inner_->SubscribeSentPacket(
        this, [this](webrtc::AsyncPacketSocket*, const webrtc::SentPacketInfo& info) {
          NotifySentPacket(this, info);
        });
    inner_->SubscribeReadyToSend(this,
                                 [this](webrtc::AsyncPacketSocket*) { NotifyReadyToSend(this); });
    inner_->SubscribeAddressReady(
        this, [this](webrtc::AsyncPacketSocket*, const webrtc::SocketAddress& address) {
          NotifyAddressReady(this, address);
        });
    inner_->SubscribeConnect(this, [this](webrtc::AsyncPacketSocket*) { NotifyConnect(this); });
    inner_->SubscribeCloseEvent(
        this, [this](webrtc::AsyncPacketSocket*, int error) { NotifyClosed(error); });
  }

  ~ImpairedUdpSocket() override {
    inner_->DeregisterReceivedPacketCallback();
    inner_->UnsubscribeSentPacket(this);
    inner_->UnsubscribeReadyToSend(this);
    inner_->UnsubscribeAddressReady(this);
    inner_->UnsubscribeConnect(this);
    inner_->UnsubscribeCloseEvent(this);
  }

  ImpairedUdpSocket(const ImpairedUdpSocket&) = delete;
  ImpairedUdpSocket& operator=(const ImpairedUdpSocket&) = delete;
  ImpairedUdpSocket(ImpairedUdpSocket&&) = delete;
  ImpairedUdpSocket& operator=(ImpairedUdpSocket&&) = delete;

  webrtc::SocketAddress GetLocalAddress() const override { return inner_->GetLocalAddress(); }
  webrtc::SocketAddress GetRemoteAddress() const override { return inner_->GetRemoteAddress(); }

  int Send(const void* data, size_t size,
           const webrtc::AsyncSocketPacketOptions& options) override {
    return SendTo(data, size, inner_->GetRemoteAddress(), options);
  }

  int SendTo(const void* data, size_t size, const webrtc::SocketAddress& address,
             const webrtc::AsyncSocketPacketOptions& options) override {
    const NetworkImpairment impairment = network_impairment();
    if (impairment.inert()) {
      return inner_->SendTo(data, size, address, options);
    }

    if (sampler_.drops(impairment.loss)) {
      impairment_internal::count_sent(true);
      // Reported as sent. A packet lost on the wire is not an error the sender
      // finds out about, and telling libwebrtc the socket failed would make it
      // tear the connection down instead of coping with the loss, which is the
      // opposite of what is being measured.
      return static_cast<int>(size);
    }
    impairment_internal::count_sent(false);

    const std::chrono::milliseconds hold = sampler_.holds(impairment);
    if (hold <= std::chrono::milliseconds::zero()) {
      return inner_->SendTo(data, size, address, options);
    }

    impairment_internal::count_delayed();
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    network_thread_->PostDelayedTask(
        [this, alive = std::weak_ptr<Lifetime>(alive_),
         copy = std::vector<std::uint8_t>(bytes, bytes + size), address, options]() mutable {
          if (alive.expired()) {
            return;
          }
          inner_->SendTo(copy.data(), copy.size(), address, options);
        },
        webrtc::TimeDelta::Millis(hold.count()));
    return static_cast<int>(size);
  }

  int Close() override { return inner_->Close(); }
  State GetState() const override { return inner_->GetState(); }
  int GetOption(webrtc::Socket::Option option, int* value) override {
    return inner_->GetOption(option, value);
  }
  int SetOption(webrtc::Socket::Option option, int value) override {
    return inner_->SetOption(option, value);
  }
  int GetError() const override { return inner_->GetError(); }
  void SetError(int error) override { inner_->SetError(error); }

 private:
  /// Only there to be pointed at: a delayed task holds a weak reference to it
  /// and gives up if the socket is gone by the time the task runs.
  struct Lifetime {};

  void on_packet(const webrtc::ReceivedIpPacket& packet) {
    const NetworkImpairment impairment = network_impairment();
    if (impairment.inert()) {
      NotifyPacketReceived(packet);
      return;
    }

    if (sampler_.drops(impairment.loss)) {
      impairment_internal::count_received(true);
      return;
    }
    impairment_internal::count_received(false);

    const std::chrono::milliseconds hold = sampler_.holds(impairment);
    if (hold <= std::chrono::milliseconds::zero()) {
      NotifyPacketReceived(packet);
      return;
    }

    impairment_internal::count_delayed();
    // The payload and the address both have to be copied: ReceivedIpPacket
    // holds a view of the first and a reference to the second, and both belong
    // to the socket that is about to reuse its buffer.
    const std::span<const std::uint8_t> payload = packet.payload();
    network_thread_->PostDelayedTask(
        [this, alive = std::weak_ptr<Lifetime>(alive_),
         copy = std::vector<std::uint8_t>(payload.begin(), payload.end()),
         source = packet.source_address(), ecn = packet.ecn()]() mutable {
          if (alive.expired()) {
            return;
          }
          // Arrival time deliberately left unset: it would be the time the
          // packet was really received, and the whole point is that it arrived
          // late.
          NotifyPacketReceived(webrtc::ReceivedIpPacket(copy, source, std::nullopt, ecn));
        },
        webrtc::TimeDelta::Millis(hold.count()));
  }

  std::unique_ptr<webrtc::AsyncPacketSocket> inner_;
  webrtc::Thread* network_thread_;
  ImpairmentSampler sampler_;
  std::shared_ptr<Lifetime> alive_ = std::make_shared<Lifetime>();
};

class ImpairedSocketFactory : public webrtc::PacketSocketFactory {
 public:
  ImpairedSocketFactory(std::unique_ptr<webrtc::PacketSocketFactory> inner,
                        webrtc::Thread* network_thread)
      : inner_(std::move(inner)), network_thread_(network_thread) {}

  std::unique_ptr<webrtc::AsyncPacketSocket> CreateUdpSocket(const webrtc::Environment& env,
                                                             const webrtc::SocketAddress& address,
                                                             uint16_t min_port,
                                                             uint16_t max_port) override {
    auto socket = inner_->CreateUdpSocket(env, address, min_port, max_port);
    if (socket == nullptr) {
      return nullptr;
    }
    return std::make_unique<ImpairedUdpSocket>(std::move(socket), network_thread_);
  }

  // TCP is only used for TCP candidates and for TURN over TCP, neither of
  // which this client negotiates, and a stream socket does not lose packets in
  // any case: what would be simulated there is a stall, not a loss.
  std::unique_ptr<webrtc::AsyncListenSocket> CreateServerTcpSocket(
      const webrtc::Environment& env, const webrtc::SocketAddress& local_address, uint16_t min_port,
      uint16_t max_port, int opts) override {
    return inner_->CreateServerTcpSocket(env, local_address, min_port, max_port, opts);
  }

  std::unique_ptr<webrtc::AsyncPacketSocket> CreateClientTcpSocket(
      const webrtc::Environment& env, const webrtc::SocketAddress& local_address,
      const webrtc::SocketAddress& remote_address,
      const webrtc::PacketSocketTcpOptions& options) override {
    return inner_->CreateClientTcpSocket(env, local_address, remote_address, options);
  }

  std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAsyncDnsResolver() override {
    return inner_->CreateAsyncDnsResolver();
  }

 private:
  std::unique_ptr<webrtc::PacketSocketFactory> inner_;
  webrtc::Thread* network_thread_;
};

}  // namespace

std::unique_ptr<webrtc::PacketSocketFactory> make_impaired_socket_factory(
    std::unique_ptr<webrtc::PacketSocketFactory> inner, webrtc::Thread* network_thread) {
  return std::make_unique<ImpairedSocketFactory>(std::move(inner), network_thread);
}

}  // namespace dv::client::media
