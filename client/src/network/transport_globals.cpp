#include "network/transport_globals.hpp"

#include <rtc/global.hpp>

namespace dv::client {

void preload_transport() {
  // Idempotent: libdatachannel counts its own users, and a second call only
  // adds one more.
  rtc::Preload();
}

}  // namespace dv::client
