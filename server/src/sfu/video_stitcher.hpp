#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace dv::server::sfu {

/// Makes everything that is ever shared in a room look, to one viewer, like one
/// camera that never stopped.
///
/// A viewer is given a single outbound video track when it joins, under a
/// single SSRC, and that track carries whoever holds the screen share floor at
/// the moment. Section 5.2 of SPEC.md allows one sharer at a time and
/// RoomManager enforces it, so the track is never carrying two people at once -
/// but over the life of a call it carries several, one after another.
///
/// Rewriting the SSRC per destination, which is all the SFU used to do, is not
/// enough to make that one stream. RFC 3550 section 5.1 has every source choose
/// its own starting sequence number and its own starting timestamp at random,
/// and a receiver orders, dejitters and times a stream by exactly those two
/// fields under exactly that SSRC. So the moment the floor changes hands the
/// viewer sees the clock jump - measured on this project, from 987000 straight
/// back to 5000 on the first packet of the second sharer. libwebrtc survives it
/// by flushing its packet buffer and asking for an intra frame, which costs
/// about 150 ms on a loopback and a great deal more anywhere a keyframe request
/// and a 720p intra frame have to survive a lossy link.
///
/// This is the piece that keeps the two fields moving forwards across the
/// handover.
///
/// # Why an offset and not a counter
///
/// The obvious implementation, numbering the outgoing packets 0, 1, 2, is
/// wrong. A gap in the sequence numbers is not noise, it is how the viewer
/// learns it missed a packet and asks for it again, and rtc::RtcpNackResponder,
/// which is already on this track's handler chain, answers that question by
/// sequence number out of a cache of what was sent. Renumbering densely would
/// hide every loss from the viewer and make the cache answer with the wrong
/// packet.
///
/// So the deltas a source produces are preserved exactly, including its holes
/// and its reordering, and only the whole series is shifted.
///
/// # What counts as a new stream
///
/// A different participant, obviously. But also the same participant sharing
/// again after stopping: that is a new encoder, and a new encoder picks new
/// starting points, so `sfu-screen` would break in the same way for one person
/// alone. Both are recognised the same way, as a discontinuity too large to be
/// ordinary loss or reordering.
///
/// # One caveat
///
/// If two participants really did send video at once, which the hub does not
/// allow, each packet would look like a new stream to the one before it and the
/// output would be an unwatchable interleaving. It would still be strictly
/// forwards, so it stays a picture that cannot be decoded rather than a
/// receiver wedged on a clock that went backwards, and it drains as soon as one
/// of them stops.
class VideoStitcher {
 public:
  /// Where a packet ends up on the wire.
  struct Rewritten {
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    /// True when this packet began a new series, which is the handover itself.
    /// Only the log reads it.
    bool rebased = false;
  };

  /// Called from a libdatachannel thread, once per packet per destination.
  /// Takes only its own lock, and nothing else takes it.
  [[nodiscard]] Rewritten rewrite(const std::string& source_user_id, std::uint16_t sequence,
                                  std::uint32_t timestamp);

  /// Who is feeding this track right now. Empty before the first packet.
  [[nodiscard]] std::string source() const;

 private:
  /// A jump larger than this is a new encoder rather than a burst of loss.
  /// Generous on purpose: a false rebase costs a discontinuity, which is the
  /// thing being avoided, while a late one costs a few packets the decoder
  /// discards anyway.
  static constexpr std::uint16_t kMaxOrdinaryJump = 3000;

  /// The same for the clock, which at 90 kHz is a little over a second. Only
  /// backwards, because a forward jump is what a sender that paused and
  /// resumed produces and rebasing on it would renumber a stream that was
  /// perfectly continuous.
  static constexpr std::uint32_t kMaxOrdinaryRewind = 90000;

  /// How far the clock is pushed forwards at a handover: one frame at 30 FPS
  /// on the 90 kHz clock H.264 uses. Any positive number would do. This one
  /// makes the seam read as an ordinary frame boundary.
  static constexpr std::uint32_t kHandoverTicks = 3000;

  mutable std::mutex mutex_;
  bool started_ = false;
  std::string source_;

  /// The last packet accepted from the source, to measure the next one against.
  std::uint16_t last_in_sequence_ = 0;
  std::uint32_t last_in_timestamp_ = 0;

  /// The furthest this track has been sent, which is what a new series has to
  /// start after.
  std::uint16_t last_out_sequence_ = 0;
  std::uint32_t last_out_timestamp_ = 0;

  /// Added to whatever the source produces. Unsigned on purpose: RTP sequence
  /// numbers and timestamps wrap, and wrapping arithmetic is the correct
  /// arithmetic for both.
  std::uint16_t sequence_offset_ = 0;
  std::uint32_t timestamp_offset_ = 0;
};

}  // namespace dv::server::sfu
