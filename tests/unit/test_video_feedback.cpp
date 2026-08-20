// The repair the SFU performs on an incoming screen share.
//
// This is protocol code: it reads sequence numbers off the wire and writes an
// RFC 4585 feedback packet back. Both halves are worth testing away from a
// network, because a wrong sequence number asks for a packet that was never
// lost, and a malformed feedback packet is ignored by the sender without
// anybody noticing that the repair stopped working.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "sfu/video_feedback.hpp"

namespace {

using dv::server::sfu::VideoFeedback;
using namespace std::chrono_literals;

constexpr std::uint32_t kSsrc = 0x1234ABCD;

[[nodiscard]] rtc::message_ptr rtp_packet(std::uint16_t sequence_number,
                                          std::uint32_t ssrc = kSsrc) {
  auto message = rtc::make_message(sizeof(rtc::RtpHeader) + 16, rtc::Message::Binary);
  auto* header = reinterpret_cast<rtc::RtpHeader*>(message->data());
  header->preparePacket();
  header->setSsrc(ssrc);
  header->setSeqNumber(sequence_number);
  header->setPayloadType(96);
  return message;
}

/// A sender report, which is the other thing that arrives on this track.
[[nodiscard]] rtc::message_ptr rtcp_packet() {
  auto message = rtc::make_message(rtc::RtcpSr::Size(0), rtc::Message::Control);
  auto* report = reinterpret_cast<rtc::RtcpSr*>(message->data());
  report->preparePacket(kSsrc, 0);
  return message;
}

/// Collects what the handler sends, and hands back the sequence numbers each
/// feedback packet asked for.
class Sink {
 public:
  [[nodiscard]] rtc::message_callback callback() {
    return [this](rtc::message_ptr message) { sent_.push_back(std::move(message)); };
  }

  /// Only the NACKs. REMB travels on the same path and would otherwise be
  /// counted as one.
  [[nodiscard]] std::size_t count() const {
    std::size_t nacks = 0;
    for (const auto& message : sent_) {
      if (payload_type(message) == 205) {
        ++nacks;
      }
    }
    return nacks;
  }

  /// The bitrate each REMB asked for, in kbps, in the order they were sent.
  [[nodiscard]] std::vector<unsigned int> bandwidth_reports() const {
    std::vector<unsigned int> reports;
    for (const auto& message : sent_) {
      if (payload_type(message) != 206) {
        continue;
      }
      auto* remb = reinterpret_cast<rtc::RtcpRemb*>(message->data());
      reports.push_back(remb->getBitrate() / 1000);
    }
    return reports;
  }

  /// The most recent REMB.
  [[nodiscard]] const rtc::Message& last_report() const {
    for (auto it = sent_.rbegin(); it != sent_.rend(); ++it) {
      if (payload_type(*it) == 206) {
        return **it;
      }
    }
    throw std::out_of_range("no report was sent");
  }

  static std::uint8_t payload_type(const rtc::message_ptr& message) {
    return reinterpret_cast<const rtc::RtcpHeader*>(message->data())->payloadType();
  }

  [[nodiscard]] std::vector<std::uint16_t> requested(std::size_t index) const {
    auto* nack = reinterpret_cast<rtc::RtcpNack*>(nack_at(index)->data());
    std::vector<std::uint16_t> numbers;
    for (unsigned int part = 0; part < nack->getSeqNoCount(); ++part) {
      for (const std::uint16_t number : nack->parts[part].getSequenceNumbers()) {
        numbers.push_back(number);
      }
    }
    return numbers;
  }

  /// Everything asked for across every packet sent so far.
  [[nodiscard]] std::vector<std::uint16_t> all_requested() const {
    std::vector<std::uint16_t> numbers;
    for (std::size_t i = 0; i < count(); ++i) {
      for (const std::uint16_t number : requested(i)) {
        numbers.push_back(number);
      }
    }
    return numbers;
  }

  /// How many feedback entries the packet carries, as opposed to how many
  /// sequence numbers they name between them.
  [[nodiscard]] unsigned int entries(std::size_t index) const {
    return reinterpret_cast<rtc::RtcpNack*>(nack_at(index)->data())->getSeqNoCount();
  }

  [[nodiscard]] const rtc::Message& message(std::size_t index) const { return *nack_at(index); }

 private:
  [[nodiscard]] const rtc::message_ptr& nack_at(std::size_t index) const {
    std::size_t seen = 0;
    for (const auto& message : sent_) {
      if (payload_type(message) == 205 && seen++ == index) {
        return message;
      }
    }
    throw std::out_of_range("no such NACK");
  }

  std::vector<rtc::message_ptr> sent_;
};

void feed(VideoFeedback& requester, Sink& sink, std::vector<rtc::message_ptr> packets) {
  rtc::message_vector messages(std::make_move_iterator(packets.begin()),
                               std::make_move_iterator(packets.end()));
  requester.incoming(messages, sink.callback());
}

void feed(VideoFeedback& requester, Sink& sink, std::initializer_list<std::uint16_t> numbers) {
  std::vector<rtc::message_ptr> packets;
  for (const std::uint16_t number : numbers) {
    packets.push_back(rtp_packet(number));
  }
  feed(requester, sink, std::move(packets));
}

TEST(VideoFeedbackTest, AStreamWithNoLossIsNeverAskedForAnything) {
  VideoFeedback requester;
  Sink sink;
  for (std::uint16_t number = 1; number <= 50; ++number) {
    feed(requester, sink, {number});
  }
  EXPECT_EQ(sink.count(), 0U);
  EXPECT_EQ(requester.packets_missing(), 0U);
}

TEST(VideoFeedbackTest, AGapIsAskedForByNumber) {
  VideoFeedback requester;
  Sink sink;
  feed(requester, sink, {1, 2, 5});

  ASSERT_EQ(sink.count(), 1U);
  EXPECT_EQ(sink.requested(0), (std::vector<std::uint16_t>{3, 4}));
  EXPECT_EQ(requester.packets_missing(), 2U);
  EXPECT_EQ(requester.requests_sent(), 1U);
}

TEST(VideoFeedbackTest, TheFeedbackPacketIsAGenericNack) {
  VideoFeedback requester;
  Sink sink;
  feed(requester, sink, {1, 3});
  ASSERT_EQ(sink.count(), 1U);

  auto* nack = reinterpret_cast<rtc::RtcpNack*>(const_cast<std::byte*>(sink.message(0).data()));
  // RFC 4585: payload type 205, feedback message type 1, and the media source
  // is the stream the packets are missing from. A sender that disagrees about
  // any of the three throws the packet away in silence.
  EXPECT_EQ(nack->header.header.payloadType(), 205);
  EXPECT_EQ(nack->header.header.reportCount(), 1);
  EXPECT_EQ(nack->header.mediaSourceSSRC(), kSsrc);
  EXPECT_EQ(sink.message(0).type, rtc::Message::Control);
  // The length field counts 32 bit words after the first, and has to match
  // what was actually written.
  EXPECT_EQ(nack->header.header.lengthInBytes(), sink.message(0).size());
}

TEST(VideoFeedbackTest, PacketsLostTogetherTravelInOneRequest) {
  // One feedback entry carries a sequence number and a mask of the sixteen
  // that follow it, which is what keeps a burst of loss from turning into a
  // burst of feedback.
  VideoFeedback requester;
  Sink sink;
  feed(requester, sink, {10, 20});

  ASSERT_EQ(sink.count(), 1U);
  EXPECT_EQ(sink.requested(0).size(), 9U);
  EXPECT_EQ(sink.entries(0), 1U) << "nine consecutive losses did not fit in one entry";
}

TEST(VideoFeedbackTest, APacketThatComesBackIsNotAskedForAgain) {
  VideoFeedback requester{{.max_requests = 5, .retry_after = 1ms}};
  Sink sink;
  feed(requester, sink, {1, 4});
  ASSERT_EQ(sink.count(), 1U);

  // The retransmission arrives, out of order, which is exactly how a repaired
  // packet looks.
  feed(requester, sink, {2, 3});
  EXPECT_EQ(requester.packets_repaired(), 2U);

  std::this_thread::sleep_for(5ms);
  feed(requester, sink, {5});
  EXPECT_EQ(sink.count(), 1U) << "a packet that had already arrived was asked for again";
}

TEST(VideoFeedbackTest, APacketIsNotAskedForForever) {
  // A packet that has not arrived after a couple of tries is one the decoder
  // has moved past, and asking again would add traffic to a link already
  // losing it.
  VideoFeedback requester{{.max_requests = 2, .retry_after = 1ms}};
  Sink sink;
  feed(requester, sink, {1, 3});

  for (int round = 0; round < 10; ++round) {
    std::this_thread::sleep_for(2ms);
    feed(requester, sink, {static_cast<std::uint16_t>(4 + round)});
  }

  std::vector<std::uint16_t> asked = sink.all_requested();
  EXPECT_EQ(asked.size(), 2U) << "packet 2 was asked for " << asked.size() << " times";
  for (const std::uint16_t number : asked) {
    EXPECT_EQ(number, 2);
  }
}

TEST(VideoFeedbackTest, AJumpTooBigToBeLossIsTreatedAsARestart) {
  // A share that stops and starts again, or a handler that joined in the
  // middle. Asking for the thousands of numbers in between would be a flood of
  // requests for packets that were never sent.
  VideoFeedback requester{{.max_gap = 256}};
  Sink sink;
  feed(requester, sink, {100, 5000});

  EXPECT_EQ(sink.count(), 0U);
  EXPECT_EQ(requester.packets_missing(), 0U);

  // And the new sequence space is followed from there.
  feed(requester, sink, {5003});
  EXPECT_EQ(requester.packets_missing(), 2U);
}

TEST(VideoFeedbackTest, TheSequenceNumberWraps) {
  VideoFeedback requester;
  Sink sink;
  feed(requester, sink, {65534, 65535, 0, 1});
  EXPECT_EQ(sink.count(), 0U) << "an ordered stream across the wrap looked like loss";

  feed(requester, sink, {4});
  ASSERT_EQ(sink.count(), 1U);
  EXPECT_EQ(sink.requested(0), (std::vector<std::uint16_t>{2, 3}));
}

TEST(VideoFeedbackTest, ALossAcrossTheWrapIsAskedForByNumber) {
  VideoFeedback requester;
  Sink sink;
  feed(requester, sink, {65534, 1});

  ASSERT_EQ(sink.count(), 1U);
  // Named in numerical order rather than in the order they were sent, which
  // costs one extra feedback entry on the single packet in every sixty five
  // thousand that straddles the wrap. Both numbers are asked for, which is
  // what matters, so the comparison sorts instead of pinning an order the
  // implementation is free to choose.
  std::vector<std::uint16_t> asked = sink.requested(0);
  std::ranges::sort(asked);
  EXPECT_EQ(asked, (std::vector<std::uint16_t>{0, 65535}));
  EXPECT_EQ(sink.entries(0), 2U);
}

TEST(VideoFeedbackTest, ANewSourceStartsANewSequenceSpace) {
  VideoFeedback requester;
  Sink sink;
  feed(requester, sink, {5000});

  std::vector<rtc::message_ptr> packets;
  packets.push_back(rtp_packet(7, 0x99999999));
  packets.push_back(rtp_packet(8, 0x99999999));
  feed(requester, sink, std::move(packets));

  EXPECT_EQ(sink.count(), 0U) << "the change of source was read as thousands of lost packets";
  EXPECT_EQ(requester.packets_missing(), 0U);
}

TEST(VideoFeedbackTest, ReportsAndShortPacketsAreLeftAlone) {
  VideoFeedback requester;
  Sink sink;

  std::vector<rtc::message_ptr> packets;
  packets.push_back(rtcp_packet());
  packets.push_back(rtc::make_message(std::size_t{4}, rtc::Message::Binary));
  packets.push_back(rtp_packet(1));
  packets.push_back(rtcp_packet());
  packets.push_back(rtp_packet(2));
  feed(requester, sink, std::move(packets));

  EXPECT_EQ(sink.count(), 0U);
  EXPECT_EQ(requester.packets_missing(), 0U);
}

TEST(VideoFeedbackTest, TheIncomingStreamIsPassedOnUntouched) {
  // The handler observes; it must not swallow or reorder what it is watching,
  // because the next handler in the chain is the one that forwards it.
  VideoFeedback requester;
  Sink sink;

  rtc::message_vector messages;
  messages.push_back(rtp_packet(1));
  messages.push_back(rtp_packet(3));
  const auto* first = messages.front().get();

  requester.incoming(messages, sink.callback());

  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(messages.front().get(), first);
}

TEST(VideoFeedbackTest, TheSenderIsToldWhatToAimFor) {
  VideoFeedback feedback{{.bandwidth_interval = 10ms,
                          .bandwidth = {.start_kbps = 1500, .min_kbps = 300, .max_kbps = 3000}}};
  Sink sink;

  feed(feedback, sink, {1});
  EXPECT_TRUE(sink.bandwidth_reports().empty())
      << "the first packet reported on an interval that had not happened yet";

  std::this_thread::sleep_for(15ms);
  feed(feedback, sink, {2});

  ASSERT_EQ(sink.bandwidth_reports().size(), 1U);
  // A clean interval, so the target grew from where it started.
  EXPECT_GT(sink.bandwidth_reports().front(), 1500U);
  EXPECT_EQ(feedback.bandwidth_reports_sent(), 1U);
  EXPECT_EQ(feedback.target_kbps(), static_cast<int>(sink.bandwidth_reports().front()));
}

TEST(VideoFeedbackTest, TheReportIsARemb) {
  VideoFeedback feedback{{.bandwidth_interval = 10ms}};
  Sink sink;
  feed(feedback, sink, {1});
  std::this_thread::sleep_for(15ms);
  feed(feedback, sink, {2});

  ASSERT_EQ(sink.bandwidth_reports().size(), 1U);
  const auto& message = sink.last_report();
  auto* remb = reinterpret_cast<rtc::RtcpRemb*>(const_cast<std::byte*>(message.data()));
  // RFC 4585 payload specific feedback, with the application layer format and
  // the four character identifier that says which application.
  EXPECT_EQ(remb->header.header.payloadType(), 206);
  EXPECT_EQ(remb->header.header.reportCount(), 15);
  EXPECT_TRUE(remb->hasValidId());
  EXPECT_EQ(remb->getSSRCCount(), 1);
  EXPECT_EQ(remb->getSSRC(0), kSsrc);
  EXPECT_EQ(message.type, rtc::Message::Control);
}

TEST(VideoFeedbackTest, HeavyLossMakesTheSenderBeAskedForLess) {
  VideoFeedback feedback{{.bandwidth_interval = 10ms,
                          .bandwidth = {.start_kbps = 3000, .min_kbps = 300, .max_kbps = 3000}}};
  Sink sink;

  // A stream where every fifth packet never arrives, fed a few intervals long.
  std::uint16_t sequence_number = 1;
  for (int round = 0; round < 40; ++round) {
    feed(feedback, sink,
         {sequence_number, static_cast<std::uint16_t>(sequence_number + 1),
          static_cast<std::uint16_t>(sequence_number + 2),
          static_cast<std::uint16_t>(sequence_number + 3)});
    // The fifth is skipped, which is the loss.
    sequence_number = static_cast<std::uint16_t>(sequence_number + 5);
    std::this_thread::sleep_for(2ms);
  }

  const auto reports = sink.bandwidth_reports();
  ASSERT_GE(reports.size(), 2U);
  EXPECT_LT(reports.back(), 3000U) << "20% loss did not bring the target down";
  EXPECT_GE(reports.back(), 300U) << "the target fell below the floor";
}

TEST(VideoFeedbackTest, TheSlowestViewerCapsTheReport) {
  VideoFeedback feedback{{.bandwidth_interval = 10ms,
                          .bandwidth = {.start_kbps = 3000, .min_kbps = 300, .max_kbps = 3000}}};
  Sink sink;
  feedback.set_bandwidth_ceiling(700);

  feed(feedback, sink, {1});
  std::this_thread::sleep_for(15ms);
  feed(feedback, sink, {2});

  ASSERT_EQ(sink.bandwidth_reports().size(), 1U);
  EXPECT_EQ(sink.bandwidth_reports().front(), 700U);
}

TEST(VideoFeedbackTest, AStreamThatStoppedIsNotReportedOn) {
  // incoming() only runs when a packet arrives, so a share that stops stops
  // the feedback too. Worth pinning down: a REMB sent about an interval with
  // no traffic would be a statement about a link nothing was using.
  VideoFeedback feedback{{.bandwidth_interval = 10ms}};
  Sink sink;
  feed(feedback, sink, {1});
  std::this_thread::sleep_for(30ms);

  EXPECT_TRUE(sink.bandwidth_reports().empty());
  EXPECT_EQ(feedback.bandwidth_reports_sent(), 0U);
}

}  // namespace
