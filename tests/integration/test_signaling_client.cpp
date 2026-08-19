// End to end tests for the client side of the signaling protocol.
//
// The counterpart of test_signaling_server.cpp: there the server is driven by a
// bare WebSocket, here the real SignalingClient is driven against the real
// server. That is what proves the two halves agree, rather than each half
// agreeing with the test's idea of the protocol.

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "network/signaling_client.hpp"
#include "signaling/server.hpp"

namespace {

using namespace std::chrono_literals;
using dv::client::SignalingClient;
using dv::server::SignalingServer;

namespace proto = dv::protocol;

constexpr auto kTimeout = 3000ms;

/// Collects what the client reports, from whatever thread it reports it on.
class Recorder {
 public:
  void attach(SignalingClient& client) {
    client.on_message([this](proto::Message message) {
      const std::lock_guard<std::mutex> lock(mutex_);
      received_.push_back(std::move(message));
      changed_.notify_all();
    });
    client.on_state([this](SignalingClient::State state, const std::string& detail) {
      const std::lock_guard<std::mutex> lock(mutex_);
      states_.push_back({state, detail});
      changed_.notify_all();
    });
  }

  template <typename T>
  [[nodiscard]] std::optional<T> wait_for(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      while (!received_.empty()) {
        proto::Message message = std::move(received_.front());
        received_.pop_front();
        if (std::holds_alternative<T>(message)) {
          return std::get<T>(message);
        }
      }
      if (changed_.wait_until(lock, deadline) == std::cv_status::timeout && received_.empty()) {
        return std::nullopt;
      }
    }
  }

  [[nodiscard]] bool wait_for_state(SignalingClient::State state,
                                    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] {
      for (const auto& entry : states_) {
        if (entry.first == state) {
          return true;
        }
      }
      return false;
    });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<proto::Message> received_;
  std::vector<std::pair<SignalingClient::State, std::string>> states_;
};

class SignalingClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SignalingServer::Options options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    // These suites are about signaling alone, so media routing stays off.
    // With the SFU on, joining a room also produces an offer from the server
    // itself, which has nothing to do with what is under test here and is
    // covered by test_sfu.cpp.
    options.enable_sfu = false;
    options.hub.max_participants_per_room = 5;
    options.hub.heartbeat_interval = 300ms;
    options.hub.heartbeat_timeout = 2000ms;

    server_ = std::make_unique<SignalingServer>(options);
    ASSERT_TRUE(server_->add_user("ana", "senha", "Ana").ok());
    ASSERT_TRUE(server_->add_user("bruno", "senha", "Bruno").ok());
    server_->start();
    ASSERT_NE(server_->port(), 0);
  }

  void TearDown() override {
    clients_.clear();
    server_->stop();
    server_.reset();
  }

  [[nodiscard]] std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(server_->port());
  }

  /// A recorder owned by the fixture, so that it outlives every client.
  Recorder& new_recorder() {
    recorders_.push_back(std::make_unique<Recorder>());
    return *recorders_.back();
  }

  /// Connects a client, waits for the socket, and logs in.
  std::pair<SignalingClient*, dv::models::User> login(const std::string& username,
                                                      Recorder& recorder) {
    auto client = std::make_unique<SignalingClient>(SignalingClient::Options{url()});
    recorder.attach(*client);
    EXPECT_TRUE(client->connect().ok());
    EXPECT_TRUE(recorder.wait_for_state(SignalingClient::State::Connected, kTimeout));

    EXPECT_TRUE(client->send(proto::Authenticate{username, "senha"}).ok());
    const auto authenticated = recorder.wait_for<proto::Authenticated>(kTimeout);
    EXPECT_TRUE(authenticated.has_value()) << "could not authenticate " << username;

    SignalingClient* raw = client.get();
    clients_.push_back(std::move(client));
    return {raw, authenticated ? authenticated->user : dv::models::User{}};
  }

  /// Declared before `clients_` so that it is destroyed after them. A client
  /// calls into its recorder from libdatachannel's threads, so the recorder has
  /// to outlive the client, not the other way round.
  std::vector<std::unique_ptr<Recorder>> recorders_;
  std::unique_ptr<SignalingServer> server_;
  std::vector<std::unique_ptr<SignalingClient>> clients_;
};

TEST_F(SignalingClientTest, RejectsAUrlThatIsNotWebSocket) {
  SignalingClient client(SignalingClient::Options{"http://127.0.0.1:8080"});
  const auto connected = client.connect();
  ASSERT_FALSE(connected.ok());
  EXPECT_EQ(connected.error().code, "invalid_value");
  EXPECT_EQ(client.state(), SignalingClient::State::Disconnected);
}

TEST_F(SignalingClientTest, RefusesToSendBeforeConnecting) {
  SignalingClient client(SignalingClient::Options{url()});
  const auto sent = client.send(proto::Ping{"n1"});
  ASSERT_FALSE(sent.ok());
  EXPECT_EQ(sent.error().code, "not_connected");
}

TEST_F(SignalingClientTest, ConnectsAndAuthenticates) {
  Recorder& recorder = new_recorder();
  const auto [client, user] = login("ana", recorder);
  EXPECT_TRUE(client->is_connected());
  EXPECT_FALSE(user.id.empty());
  EXPECT_EQ(user.display_name, "Ana");
}

TEST_F(SignalingClientTest, ReportsAServerThatIsNotThere) {
  // Port 1 is reserved and nothing listens there, so this is a refused
  // connection rather than a timeout.
  //
  // The recorder comes from the fixture so that it outlives the client: the
  // client reports the failure from a libdatachannel thread, which can still be
  // running when this scope ends.
  Recorder& recorder = new_recorder();
  SignalingClient client(SignalingClient::Options{"ws://127.0.0.1:1"});
  recorder.attach(client);

  ASSERT_TRUE(client.connect().ok());
  EXPECT_TRUE(recorder.wait_for_state(SignalingClient::State::Failed, kTimeout));
}

TEST_F(SignalingClientTest, CreatesAndJoinsARoom) {
  Recorder& recorder = new_recorder();
  const auto [client, user] = login("ana", recorder);

  ASSERT_TRUE(client->send(proto::CreateRoom{user.id, "sala-dev"}).ok());
  const auto created = recorder.wait_for<proto::RoomCreated>(kTimeout);
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(created->room_id.size(), 6U);

  ASSERT_TRUE(client->send(proto::JoinRoom{created->room_id, user.id, "Ana"}).ok());
  const auto joined = recorder.wait_for<proto::UserJoined>(kTimeout);
  ASSERT_TRUE(joined.has_value());
  EXPECT_EQ(joined->user.id, user.id);
}

TEST_F(SignalingClientTest, AnswersTheServerHeartbeatWithoutHelp) {
  Recorder& recorder = new_recorder();
  const auto [client, user] = login("ana", recorder);

  // The heartbeat is transport level, so it has to work with nobody listening
  // above. The server closes a connection that stops answering after 2s, and
  // pings every 300ms.
  std::this_thread::sleep_for(1500ms);

  EXPECT_GE(client->pings_answered(), 2U);
  EXPECT_TRUE(client->is_connected());

  // A ping is answered, never handed up: the layer above has no business
  // seeing it.
  EXPECT_FALSE(recorder.wait_for<proto::Ping>(100ms).has_value());
}

TEST_F(SignalingClientTest, RelaysAnOfferBetweenTwoClients) {
  Recorder& ana_recorder = new_recorder();
  Recorder& bruno_recorder = new_recorder();
  const auto [ana, ana_user] = login("ana", ana_recorder);
  const auto [bruno, bruno_user] = login("bruno", bruno_recorder);

  ASSERT_TRUE(ana->send(proto::CreateRoom{ana_user.id, "sala-dev"}).ok());
  const auto created = ana_recorder.wait_for<proto::RoomCreated>(kTimeout);
  ASSERT_TRUE(created.has_value());
  const std::string room = created->room_id;

  ASSERT_TRUE(ana->send(proto::JoinRoom{room, ana_user.id, "Ana"}).ok());
  ASSERT_TRUE(ana_recorder.wait_for<proto::UserJoined>(kTimeout).has_value());
  ASSERT_TRUE(bruno->send(proto::JoinRoom{room, bruno_user.id, "Bruno"}).ok());
  ASSERT_TRUE(bruno_recorder.wait_for<proto::UserJoined>(kTimeout).has_value());

  ASSERT_TRUE(ana->send(proto::Offer{room, ana_user.id, bruno_user.id, "v=0 fake"}).ok());

  const auto offer = bruno_recorder.wait_for<proto::Offer>(kTimeout);
  ASSERT_TRUE(offer.has_value());
  EXPECT_EQ(offer->from_user_id, ana_user.id);
  EXPECT_EQ(offer->sdp, "v=0 fake");
}

TEST_F(SignalingClientTest, DisconnectIsNotReportedAsAFailure) {
  Recorder& recorder = new_recorder();
  const auto [client, user] = login("ana", recorder);

  client->disconnect();
  EXPECT_TRUE(recorder.wait_for_state(SignalingClient::State::Disconnected, kTimeout));
  EXPECT_EQ(client->state(), SignalingClient::State::Disconnected);
}

}  // namespace
