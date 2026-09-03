// The audio half of the SFU's loss repair, standing alone.
//
// The NACK arithmetic itself is covered next door, in test_video_feedback.cpp,
// through the handler that drives it for the screen share. What is pinned here
// is the shape the audio installs: the repair on its own, with nothing else on
// the RTCP path, and the observer that counts the requests coming back from a
// listener.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rtc/rtc.hpp>

#include "sfu/loss_repair.hpp"

namespace {

using dv::server::sfu::LossRepair;
using dv::server::sfu::NackObserver;

constexpr std::uint32_t kSsrc = 0xAB12CD34;
constexpr std::uint8_t kTransportFeedback = 205;

[[nodiscard]] rtc::message_ptr rtp_packet(std::uint16_t sequence_number) {
  auto message = rtc::make_message(sizeof(rtc::RtpHeader) + 40, rtc::Message::Binary);
  auto* header = reinterpret_cast<rtc::RtpHeader*>(message->data());
  header->preparePacket();
  header->setSsrc(kSsrc);
  header->setSeqNumber(sequence_number);
  header->setPayloadType(63);
  return message;
}

/// What a listener sends back when it misses `sequence_number`.
[[nodiscard]] rtc::message_ptr nack_packet(std::uint16_t sequence_number) {
  auto message = rtc::make_message(rtc::RtcpNack::Size(1), rtc::Message::Control);
  auto* nack = reinterpret_cast<rtc::RtcpNack*>(message->data());
  nack->preparePacket(kSsrc, 1);
  unsigned int entries = 0;
  std::uint16_t pid = 0;
  nack->addMissingPacket(&entries, &pid, sequence_number);
  return message;
}

/// The other kind of RTCP on this path, and the one that carries no request.
[[nodiscard]] rtc::message_ptr sender_report() {
  auto message = rtc::make_message(rtc::RtcpSr::Size(0), rtc::Message::Control);
  auto* report = reinterpret_cast<rtc::RtcpSr*>(message->data());
  report->preparePacket(kSsrc, 0);
  return message;
}

[[nodiscard]] std::uint8_t payload_type(const rtc::message_ptr& message) {
  return reinterpret_cast<const rtc::RtcpHeader*>(message->data())->payloadType();
}

[[nodiscard]] std::vector<std::uint16_t> requested(const rtc::message_ptr& message) {
  // Not const: libdatachannel's readers are not.
  auto* nack = reinterpret_cast<rtc::RtcpNack*>(message->data());
  std::vector<std::uint16_t> numbers;
  for (unsigned int part = 0; part < nack->getSeqNoCount(); ++part) {
    for (const std::uint16_t number : nack->parts[part].getSequenceNumbers()) {
      numbers.push_back(number);
    }
  }
  return numbers;
}

struct Sent {
  std::vector<rtc::message_ptr> messages;
  [[nodiscard]] rtc::message_callback callback() {
    return [this](rtc::message_ptr message) { messages.push_back(std::move(message)); };
  }
};

[[nodiscard]] rtc::message_vector audio(std::initializer_list<std::uint16_t> numbers) {
  rtc::message_vector messages;
  for (const std::uint16_t number : numbers) {
    messages.push_back(rtp_packet(number));
  }
  return messages;
}

TEST(LossRepairTest, AHoleInTheAudioIsAskedForOnTheNextPacket) {
  // Twenty milliseconds of audio per packet, and the request leaves the moment
  // the packet after the hole shows what is missing, not on a timer: the
  // listener's jitter buffer is the clock this is racing.
  LossRepair repair;
  Sent sent;
  auto messages = audio({100, 101, 103});
  repair.incoming(messages, sent.callback());

  ASSERT_EQ(sent.messages.size(), 1U);
  EXPECT_EQ(payload_type(sent.messages.front()), kTransportFeedback);
  EXPECT_EQ(requested(sent.messages.front()), (std::vector<std::uint16_t>{102}));
  EXPECT_EQ(repair.requests_sent(), 1U);
  EXPECT_EQ(repair.packets_missing(), 1U);
  EXPECT_EQ(repair.ssrc(), kSsrc);
}

TEST(LossRepairTest, TheRepairSendsNothingButRequests) {
  // Standing alone on an audio track there is no bandwidth estimate and no
  // REMB: everything that leaves is a NACK, or nothing leaves at all.
  LossRepair repair;
  Sent sent;
  for (std::uint16_t number = 1; number <= 200; ++number) {
    auto messages = audio({number});
    repair.incoming(messages, sent.callback());
  }
  EXPECT_TRUE(sent.messages.empty()) << "a stream with no loss was asked for something";

  auto messages = audio({205});
  repair.incoming(messages, sent.callback());
  ASSERT_EQ(sent.messages.size(), 1U);
  for (const auto& message : sent.messages) {
    EXPECT_EQ(payload_type(message), kTransportFeedback);
  }
}

TEST(LossRepairTest, ThePacketThatComesBackIsCountedAsRepaired) {
  LossRepair repair;
  Sent sent;
  auto first = audio({10, 12});
  repair.incoming(first, sent.callback());
  ASSERT_EQ(repair.packets_missing(), 1U);

  auto retransmission = audio({11});
  repair.incoming(retransmission, sent.callback());
  EXPECT_EQ(repair.packets_repaired(), 1U);
  EXPECT_EQ(repair.packets_seen(), 3U);
}

TEST(LossRepairTest, TheAudioPassesThroughUntouched) {
  // The handler after this one is the one that forwards, and it has to see
  // exactly what arrived, holes included: the repair fills them by asking,
  // never by editing.
  LossRepair repair;
  Sent sent;
  auto messages = audio({7, 9});
  repair.incoming(messages, sent.callback());
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(reinterpret_cast<const rtc::RtpHeader*>(messages[0]->data())->seqNumber(), 7);
  EXPECT_EQ(reinterpret_cast<const rtc::RtpHeader*>(messages[1]->data())->seqNumber(), 9);
}

TEST(NackObserverTest, CountsEveryRequestAndLeavesItInPlace) {
  int nacks = 0;
  NackObserver observer([&nacks] { ++nacks; });
  Sent sent;

  rtc::message_vector messages;
  messages.push_back(sender_report());
  messages.push_back(nack_packet(5));
  messages.push_back(rtp_packet(7));
  messages.push_back(nack_packet(9));
  observer.incoming(messages, sent.callback());

  EXPECT_EQ(nacks, 2);
  // Counted, not consumed: the responder behind this one still has to answer.
  EXPECT_EQ(messages.size(), 4U);
  EXPECT_TRUE(sent.messages.empty());
}

TEST(NackObserverTest, SeesARequestInsideACompoundPacket) {
  // RTCP arrives compound more often than not: a report first, the request
  // after it, in one buffer. The observer walks the whole thing.
  const auto report = sender_report();
  const auto nack = nack_packet(3);
  auto compound = rtc::make_message(report->size() + nack->size(), rtc::Message::Control);
  std::memcpy(compound->data(), report->data(), report->size());
  std::memcpy(compound->data() + report->size(), nack->data(), nack->size());

  int nacks = 0;
  NackObserver observer([&nacks] { ++nacks; });
  Sent sent;
  rtc::message_vector messages;
  messages.push_back(std::move(compound));
  observer.incoming(messages, sent.callback());
  EXPECT_EQ(nacks, 1);
}

}  // namespace
