#pragma once

namespace dv::client {

/// Brings libdatachannel's process wide state up, before anything asks for it.
///
/// libdatachannel initialises itself lazily, on the first WebSocket or peer
/// connection, and tears itself down again when the last one goes away. Part
/// of that state is libsrtp, which it initialises whether or not any media
/// runs through it: see `Init::doInit` in the library, which calls
/// `DtlsSrtpTransport::Init()` unconditionally.
///
/// That matters here because libwebrtc carries its own copy of libsrtp, and a
/// static link resolves both copies to one. libsrtp refuses a second
/// `srtp_init()` on the same crypto kernel, so exactly one of the two
/// libraries may perform it. Preloading makes libdatachannel that one, at a
/// moment we choose rather than whenever a socket happens to be created, and
/// keeps it initialised for as long as the process lives. The other half of
/// the arrangement is `webrtc::ProhibitLibsrtpInitialization()` in
/// client/src/webrtc/libwebrtc_media_session.cpp.
void preload_transport();

}  // namespace dv::client
