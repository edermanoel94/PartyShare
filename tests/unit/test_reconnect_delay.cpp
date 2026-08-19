// The reconnection delay, on its own.
//
// Everything else about reconnecting needs a server to go away and come back,
// which is an integration test. The schedule itself is arithmetic, and the
// cases worth pinning down are the edges: the first attempt, the ceiling, and
// what happens when the ceiling is not a power of two away from the start.

#include <chrono>

#include <gtest/gtest.h>

#include "network/signaling_client.hpp"

namespace {

using namespace std::chrono_literals;
using dv::client::reconnect_delay;
using dv::client::SignalingClient;

[[nodiscard]] SignalingClient::Options options(std::chrono::milliseconds initial,
                                               std::chrono::milliseconds maximum) {
  SignalingClient::Options result;
  result.reconnect_initial_delay = initial;
  result.reconnect_max_delay = maximum;
  return result;
}

TEST(ReconnectDelayTest, TheFirstAttemptWaitsTheInitialDelay) {
  const auto opts = options(500ms, 30000ms);
  EXPECT_EQ(reconnect_delay(1, opts), 500ms);
}

TEST(ReconnectDelayTest, ItDoublesWithEachAttempt) {
  const auto opts = options(500ms, 30000ms);
  EXPECT_EQ(reconnect_delay(2, opts), 1000ms);
  EXPECT_EQ(reconnect_delay(3, opts), 2000ms);
  EXPECT_EQ(reconnect_delay(4, opts), 4000ms);
  EXPECT_EQ(reconnect_delay(5, opts), 8000ms);
  EXPECT_EQ(reconnect_delay(6, opts), 16000ms);
}

TEST(ReconnectDelayTest, ItStopsAtTheCeiling) {
  // A client left running overnight against a server that is gone should knock
  // once every half minute, not once every half second.
  const auto opts = options(500ms, 30000ms);
  EXPECT_EQ(reconnect_delay(7, opts), 30000ms);
  EXPECT_EQ(reconnect_delay(20, opts), 30000ms);
  EXPECT_EQ(reconnect_delay(1000, opts), 30000ms);
}

TEST(ReconnectDelayTest, TheCeilingHoldsEvenWhenItIsNotAPowerOfTwoAway) {
  const auto opts = options(300ms, 1000ms);
  EXPECT_EQ(reconnect_delay(1, opts), 300ms);
  EXPECT_EQ(reconnect_delay(2, opts), 600ms);
  EXPECT_EQ(reconnect_delay(3, opts), 1000ms) << "the delay overshot the ceiling";
  EXPECT_EQ(reconnect_delay(4, opts), 1000ms);
}

TEST(ReconnectDelayTest, AnAttemptNumberBelowOneIsTreatedAsTheFirst) {
  const auto opts = options(500ms, 30000ms);
  EXPECT_EQ(reconnect_delay(0, opts), 500ms);
  EXPECT_EQ(reconnect_delay(-3, opts), 500ms);
}

TEST(ReconnectDelayTest, AHugeAttemptNumberDoesNotOverflowIntoASmallDelay) {
  // Doubling a duration enough times wraps round, and a wrapped delay is a
  // client hammering a server that is already in trouble.
  const auto opts = options(1ms, 30000ms);
  for (int attempt = 1; attempt < 200; ++attempt) {
    const auto delay = reconnect_delay(attempt, opts);
    EXPECT_GE(delay, 1ms) << "attempt " << attempt;
    EXPECT_LE(delay, 30000ms) << "attempt " << attempt;
  }
}

}  // namespace
