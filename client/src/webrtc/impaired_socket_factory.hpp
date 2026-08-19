// A PacketSocketFactory that damages the packets going through it.
//
// libwebrtc lets an application supply the factory that creates every UDP
// socket a PeerConnection uses for ICE, DTLS and RTP. Wrapping that factory is
// the only seam in the stack where a packet can be thrown away or held back
// after the encoder has produced it and before the operating system sees it,
// without root and without a platform specific tool.
//
// What is injected is described in client/src/media/network_impairment.hpp,
// including why this exists next to scripts/netem.sh rather than instead of
// it.

#pragma once

#include <memory>

#include <api/packet_socket_factory.h>
#include <rtc_base/thread.h>

namespace dv::client::media {

/// Wraps `inner`, applying whatever impairment is in force when each packet
/// goes past. Inert, and close to free, while the impairment is inert.
///
/// `network_thread` is where the wrapped sockets live and where a held packet
/// is released, so it must be the thread the factory creates sockets on.
[[nodiscard]] std::unique_ptr<webrtc::PacketSocketFactory> make_impaired_socket_factory(
    std::unique_ptr<webrtc::PacketSocketFactory> inner, webrtc::Thread* network_thread);

}  // namespace dv::client::media
