#include "sfu/video_feedback.hpp"

#include <cstdint>

namespace dv::server::sfu {

void VideoFeedback::incoming(rtc::message_vector& messages, const rtc::message_callback& send) {
  repair_.incoming(messages, send);
  report_bandwidth(send, Clock::now());
}

void VideoFeedback::set_bandwidth_ceiling(int kbps) {
  const std::lock_guard<std::mutex> lock(mutex_);
  bandwidth_.set_ceiling(kbps);
}

int VideoFeedback::target_kbps() const {
  return target_kbps_.load(std::memory_order_relaxed);
}

void VideoFeedback::report_bandwidth(const rtc::message_callback& send, Clock::time_point now) {
  int target = 0;
  std::uint32_t ssrc = 0;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t seen_now = repair_.packets_seen();
    if (seen_now == 0 || !send) {
      return;
    }
    if (last_bandwidth_report_ == Clock::time_point{}) {
      // The first packet only starts the clock. Reporting immediately would
      // report on an interval that never happened.
      last_bandwidth_report_ = now;
      packets_seen_at_report_ = seen_now;
      packets_missing_at_report_ = repair_.packets_missing();
      return;
    }
    if (now - last_bandwidth_report_ < options_.bandwidth_interval) {
      return;
    }

    const std::uint64_t missing_now = repair_.packets_missing();
    const std::uint64_t received = seen_now - packets_seen_at_report_;
    const std::uint64_t lost = missing_now - packets_missing_at_report_;
    last_bandwidth_report_ = now;
    packets_seen_at_report_ = seen_now;
    packets_missing_at_report_ = missing_now;

    // Packets that came back after being asked for are not loss the sender has
    // to slow down for: the repair worked. Only what stayed missing counts,
    // and the counter cannot tell the two apart within one interval, so this
    // errs on the side of asking for less rather than more.
    bandwidth_.update(received, lost);
    target = bandwidth_.target_kbps();
    ssrc = repair_.ssrc();
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
