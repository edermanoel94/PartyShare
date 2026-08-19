#include "sfu/video_feedback.hpp"

#include <cstring>
#include <vector>

namespace dv::server::sfu {

void VideoFeedback::incoming(rtc::message_vector& messages, const rtc::message_callback& send) {
  const auto now = Clock::now();

  for (const auto& message : messages) {
    if (message == nullptr || message->size() < sizeof(rtc::RtpHeader) || rtc::IsRtcp(*message)) {
      continue;
    }
    const auto* header = reinterpret_cast<const rtc::RtpHeader*>(message->data());
    if (header->version() != 2) {
      continue;
    }
    observe(header->ssrc(), header->seqNumber(), now);
  }

  request(send, now);
  report_bandwidth(send, now);
}

void VideoFeedback::set_bandwidth_ceiling(int kbps) {
  const std::lock_guard<std::mutex> lock(mutex_);
  bandwidth_.set_ceiling(kbps);
}

int VideoFeedback::target_kbps() const {
  return target_kbps_.load(std::memory_order_relaxed);
}

void VideoFeedback::observe(std::uint32_t ssrc, std::uint16_t sequence_number,
                            Clock::time_point now) {
  const std::lock_guard<std::mutex> lock(mutex_);

  if (ssrc != ssrc_) {
    // A different source on this track means a different sequence space, and
    // carrying the old expectation across would report the whole space as
    // missing.
    ssrc_ = ssrc;
    started_ = false;
    missing_.clear();
  }

  ++packets_seen_;
  if (!started_) {
    started_ = true;
    expected_ = static_cast<std::uint16_t>(sequence_number + 1);
    return;
  }

  // Signed on purpose: sequence numbers wrap at 65535, and the difference read
  // as a signed 16 bit number is what says which side of the wrap this packet
  // is on.
  const auto distance = static_cast<std::int16_t>(sequence_number - expected_);

  if (distance == 0) {
    expected_ = static_cast<std::uint16_t>(sequence_number + 1);
    return;
  }

  if (distance < 0) {
    // Older than expected: either the retransmission that was asked for, or
    // plain reordering. Both mean the hole is filled.
    if (missing_.erase(sequence_number) > 0) {
      packets_repaired_.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }

  if (static_cast<std::uint16_t>(distance) > options_.max_gap) {
    // Not a loss: a stream that stopped and started again, or one this handler
    // joined in the middle of. Asking for the packets in between would be a
    // burst of requests for packets that were never sent.
    missing_.clear();
    expected_ = static_cast<std::uint16_t>(sequence_number + 1);
    return;
  }

  for (std::uint16_t lost = expected_; lost != sequence_number; ++lost) {
    const auto [entry, inserted] = missing_.try_emplace(
        lost, Missing{.first_seen = now, .last_requested = Clock::time_point{}, .requests = 0});
    if (inserted) {
      packets_missing_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  expected_ = static_cast<std::uint16_t>(sequence_number + 1);
}

void VideoFeedback::request(const rtc::message_callback& send, Clock::time_point now) {
  std::vector<std::uint16_t> wanted;
  std::uint32_t ssrc = 0;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    ssrc = ssrc_;
    for (auto it = missing_.begin(); it != missing_.end();) {
      Missing& entry = it->second;
      if (entry.requests >= options_.max_requests ||
          now - entry.first_seen >= options_.give_up_after) {
        it = missing_.erase(it);
        continue;
      }
      if (entry.requests > 0 && now - entry.last_requested < options_.retry_after) {
        ++it;
        continue;
      }
      entry.last_requested = now;
      ++entry.requests;
      wanted.push_back(it->first);
      ++it;
    }
  }

  if (wanted.empty() || !send) {
    return;
  }

  // Packed first into a scratch buffer, because how many feedback entries the
  // list needs is only known once it has been packed: one entry carries a
  // sequence number and a bitmask of the sixteen that follow it, so a run of
  // losses close together costs one entry and a run spread out costs several.
  // Sending a packet whose header claims more entries than were written would
  // ask for sequence number zero, over and over.
  std::vector<std::byte> scratch(rtc::RtcpNack::Size(static_cast<unsigned int>(wanted.size())));
  auto* draft = reinterpret_cast<rtc::RtcpNack*>(scratch.data());
  draft->preparePacket(ssrc, static_cast<unsigned int>(wanted.size()));

  unsigned int entries = 0;
  std::uint16_t pid = 0;
  for (const std::uint16_t sequence_number : wanted) {
    draft->addMissingPacket(&entries, &pid, sequence_number);
  }
  if (entries == 0) {
    return;
  }

  auto message = rtc::make_message(rtc::RtcpNack::Size(entries), rtc::Message::Control);
  auto* nack = reinterpret_cast<rtc::RtcpNack*>(message->data());
  nack->preparePacket(ssrc, entries);
  std::memcpy(reinterpret_cast<std::byte*>(nack) + sizeof(rtc::RtcpFbHeader),
              scratch.data() + sizeof(rtc::RtcpFbHeader), entries * sizeof(rtc::RtcpNackPart));

  requests_sent_.fetch_add(1, std::memory_order_relaxed);
  send(std::move(message));
}

void VideoFeedback::report_bandwidth(const rtc::message_callback& send, Clock::time_point now) {
  int target = 0;
  std::uint32_t ssrc = 0;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || !send) {
      return;
    }
    if (last_bandwidth_report_ == Clock::time_point{}) {
      // The first packet only starts the clock. Reporting immediately would
      // report on an interval that never happened.
      last_bandwidth_report_ = now;
      packets_seen_at_report_ = packets_seen_;
      packets_missing_at_report_ = packets_missing_.load(std::memory_order_relaxed);
      return;
    }
    if (now - last_bandwidth_report_ < options_.bandwidth_interval) {
      return;
    }

    const std::uint64_t missing_now = packets_missing_.load(std::memory_order_relaxed);
    const std::uint64_t received = packets_seen_ - packets_seen_at_report_;
    const std::uint64_t lost = missing_now - packets_missing_at_report_;
    last_bandwidth_report_ = now;
    packets_seen_at_report_ = packets_seen_;
    packets_missing_at_report_ = missing_now;

    // Packets that came back after being asked for are not loss the sender has
    // to slow down for: the repair worked. Only what stayed missing counts,
    // and the counter cannot tell the two apart within one interval, so this
    // errs on the side of asking for less rather than more.
    bandwidth_.update(received, lost);
    target = bandwidth_.target_kbps();
    ssrc = ssrc_;
  }

  if (ssrc == 0) {
    return;
  }

  auto message = rtc::make_message(rtc::RtcpRemb::SizeWithSSRCs(1), rtc::Message::Control);
  auto* remb = reinterpret_cast<rtc::RtcpRemb*>(message->data());
  remb->preparePacket(ssrc, 1, static_cast<unsigned int>(target) * 1000);
  remb->setSSRC(0, ssrc);

  target_kbps_.store(target, std::memory_order_relaxed);
  bandwidth_reports_sent_.fetch_add(1, std::memory_order_relaxed);
  send(std::move(message));
}

}  // namespace dv::server::sfu
