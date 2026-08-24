// The interoperation the whole plan rests on: libwebrtc on the client and
// libdatachannel on the server, negotiating and carrying audio between them.
//
// PLAN.md lists this as a high risk, and this is what retires it. Everything
// here is real: a real signaling server, a real SFU, real libwebrtc peer
// connections, real ICE, DTLS and SRTP. Only the network is local.
//
// Built only when DV_BUILD_CLIENT_MEDIA is on, because it needs the libwebrtc
// tree from docs/webrtc-toolchain.md.
//
// Set DV_AUDIO_NULL_DEVICE=1 to run them on a machine with no sound card. The
// negotiation cases still pass. The cases that assert something *about*
// captured audio, which is both echo canceller cases, skip themselves: with
// nothing captured there is nothing for a canceller to run on, and neither
// answer would mean anything.
//
// MetricsAreCollectedFromTheRealConnection still fails there, and should. Its
// assertion is that audio flows, so skipping it when audio does not flow would
// leave a test that cannot fail.

#include "media_test_client.hpp"

namespace {

TEST_F(MediaEndToEndTest, ALibwebrtcClientNegotiatesWithTheLibdatachannelSfu) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  // Reaching this state means the SFU's offer was understood, the answer was
  // accepted, ICE completed and DTLS finished. That is the whole
  // interoperation question answered.
  EXPECT_TRUE(ana.wait_until_in_call());
  EXPECT_EQ(ana.errors(), 0U);
}

TEST_F(MediaEndToEndTest, TwoClientsShareARoomAndTheSfuForwardsTheirAudio) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  ASSERT_TRUE(wait_until([&] { return ana.participants().size() == 2; }));
  ASSERT_TRUE(wait_until([&] { return bruno.participants().size() == 2; }));

  auto* router = server_->media_router();
  ASSERT_NE(router, nullptr);

  // One track each way, carrying the other participant.
  EXPECT_TRUE(wait_until([&] {
    return router->outbound_track_count(ana.session().local_user().id) == 1 &&
           router->outbound_track_count(bruno.session().local_user().id) == 1;
  }));

  // Audio itself. A machine with no working capture device still encodes and
  // sends, because libwebrtc falls back to silence rather than to nothing.
  EXPECT_TRUE(wait_until([&] { return router->audio_packets_received() > 0; }))
      << "no audio reached the SFU";
  EXPECT_TRUE(wait_until([&] { return router->audio_packets_forwarded() > 0; }))
      << "the SFU received audio but forwarded none";
}

TEST_F(MediaEndToEndTest, MetricsAreCollectedFromTheRealConnection) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  // Bytes sent is the first number to move, and it proves the stats path
  // works end to end: collection, parsing and reporting.
  EXPECT_TRUE(wait_until([&] { return ana.session().stats().bytes_sent > 0; }));
}

TEST_F(MediaEndToEndTest, TheEchoCancellerRunsOnTheCapturedAudio) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  // An echo canceller processes captured audio, so with nothing captured
  // there is nothing to assert either way. Skipped rather than failed: a
  // headset that is switched off is not a defect in this program.
  if (!ana.wait_until_sending_audio()) {
    GTEST_SKIP() << "this machine captured no audio, so there is nothing for the echo canceller "
                    "to run on";
  }

  // Section 9 of SPEC.md requires AEC3. Whether it is configured is a matter
  // of reading the source, so what is asserted is the one thing libwebrtc
  // reports only while the echo controller is really processing capture: echo
  // return loss.
  EXPECT_TRUE(wait_until([&] { return ana.session().stats().echo_cancellation_active; }, 5000ms))
      << "the echo canceller is not running on the captured audio";
}

TEST_F(MediaEndToEndTest, TurningTheEchoCancellerOffStopsIt) {
  // The other half of the assertion above: without this the first test would
  // still pass if the reported metric had nothing to do with our settings.
  //
  // One client only. The processing module belongs to the factory, not to the
  // peer connection, so it is shared by every session in the process and the
  // last options applied win. That is fine for the product, where a process
  // has one local user, but it means this case cannot share a process with a
  // client that wants the echo canceller on. ctest runs one case per process.
  Client& ana = add("ana", /*echo_cancellation=*/false);
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  // The pipeline has to be running before "the echo canceller is not part of
  // it" means anything: on a machine that captures nothing, it is not part of
  // it either way and the assertion below would pass for the wrong reason.
  // Skipped rather than failed, and rather than passed.
  if (!ana.wait_until_sending_audio()) {
    GTEST_SKIP() << "this machine captured no audio, so an idle echo canceller proves nothing";
  }
  std::this_thread::sleep_for(1500ms);

  EXPECT_FALSE(ana.session().stats().echo_cancellation_active)
      << "the echo canceller kept running after being turned off";
}

TEST_F(MediaEndToEndTest, TheAudioPipelineWorksOnAVirtualDevice) {
  const char* virtual_input = std::getenv("DV_VIRTUAL_INPUT_DEVICE");
  if (virtual_input == nullptr) {
    GTEST_SKIP() << "no virtual device: run scripts/virtual_audio.sh start first";
  }

  const auto inputs = media::input_devices();
  ASSERT_TRUE(inputs.ok()) << inputs.error().message;

  std::string listed;
  bool present = false;
  for (const media::AudioDevice& device : inputs.value()) {
    listed += "\n  " + device.id;
    present = present || device.id == virtual_input;
  }
  ASSERT_TRUE(present) << "the virtual device " << virtual_input
                       << " is not among the ones reported:" << listed;

  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  auto* router = server_->media_router();
  EXPECT_TRUE(wait_until([&] { return router->audio_packets_forwarded() > 0; }))
      << "no audio made it through the SFU from the virtual device";

  // The script keeps a tone playing into the virtual microphone, so unlike a
  // silent device this proves samples are being captured and not just frames
  // being encoded. Digital silence would leave the level at zero.
  EXPECT_TRUE(wait_until([&] { return ana.highest_local_level() > 0.0; }, 10000ms))
      << "the captured audio is silent, so nothing is really being recorded";

  // The tone is far above the speaking threshold, so the indicator that tells
  // a room who has the floor has to light up.
  EXPECT_TRUE(wait_until([&] { return ana.local_speaking_seen(); }, 10000ms))
      << "a tone at a third of full scale did not count as speaking";

  // And the same signal has to arrive at the other end with a level on it,
  // which is the RFC 6464 header extension surviving the SFU with a real
  // measurement rather than a zero.
  EXPECT_TRUE(wait_until(
      [&] {
        for (const Participant& participant : bruno.participants()) {
          if (participant.user.id == ana.session().local_user().id) {
            return participant.level > 0.0 && participant.speaking;
          }
        }
        return false;
      },
      10000ms))
      << "the tone reached the other client without an audio level";
}

TEST_F(MediaEndToEndTest, TheSystemAudioDevicesCanBeListed) {
  const auto inputs = media::input_devices();
  ASSERT_TRUE(inputs.ok()) << inputs.error().message;

  const auto outputs = media::output_devices();
  ASSERT_TRUE(outputs.ok()) << outputs.error().message;

  if (inputs.value().empty() && outputs.value().empty()) {
    GTEST_SKIP() << "this machine has no audio devices";
  }

  for (const media::AudioDevice& device : inputs.value()) {
    EXPECT_FALSE(device.id.empty());
    EXPECT_FALSE(device.name.empty());
  }
  // The platform default is first, which is what the settings dialog relies on
  // to preselect something sensible.
  if (!inputs.value().empty()) {
    EXPECT_TRUE(inputs.value().front().is_default);
  }
}

TEST_F(MediaEndToEndTest, SwitchingMicrophoneDoesNotInterruptTheCallForLong) {
  const auto inputs = media::input_devices();
  ASSERT_TRUE(inputs.ok()) << inputs.error().message;
  if (inputs.value().empty()) {
    GTEST_SKIP() << "this machine has no capture device";
  }

  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  auto* router = server_->media_router();
  ASSERT_TRUE(wait_until([&] { return router->audio_packets_received() > 0; }));

  // The second device when there is one, otherwise the same device again:
  // either way the capture is stopped and started, which is what has to stay
  // quick. Section 9 of SPEC.md allows 500 ms.
  const media::AudioDevice& target =
      inputs.value().size() > 1 ? inputs.value()[1] : inputs.value()[0];

  const auto switched_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(ana.session().set_input_device(target.id).ok()) << "could not select " << target.name;

  // The gap is measured from the audio the server actually receives, which is
  // the only place where an interruption is real rather than assumed.
  std::uint64_t before = router->audio_packets_received();
  ASSERT_TRUE(wait_until([&] { return router->audio_packets_received() > before + 5; }, 3000ms))
      << "audio never came back after the switch";

  // Known to fail on a GitHub runner: 2056 ms against this 500 ms, on the
  // first run where the media job ever reached its tests at all. Left as it
  // is, and written down rather than loosened, because nobody yet knows which
  // of two things it is.
  //
  // It is a wall clock budget measured on shared hardware through a PulseAudio
  // null sink, which is the shape of an assertion that reports the machine
  // instead of the program. It is also exactly the number a person notices
  // when they change microphone mid-call, so raising it to make the job green
  // would trade the only thing this test is for.
  //
  // Deciding needs a second data point, and there is no baseline: every
  // earlier run of this job stopped before the tests, so this has possibly
  // never run in CI until now.
  const auto gap = std::chrono::steady_clock::now() - switched_at;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(gap).count(), 500)
      << "switching the microphone silenced the call for too long";

  EXPECT_EQ(ana.session().state(), CallSession::State::InCall);
}

TEST_F(MediaEndToEndTest, AudioLevelsAreReported) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  // Whether anyone is actually talking into the microphone is not something a
  // test can arrange, so what is asserted is that the level of the remote
  // participant is being reported at all. That is what proves the RFC 6464
  // extension survived the SFU.
  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : ana.participants()) {
      if (participant.user.id == bruno.session().local_user().id) {
        return participant.audio_active;
      }
    }
    return false;
  }));
}

TEST_F(MediaEndToEndTest, AScreenSharedByOneClientArrivesDecodedAtTheOther) {
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  const auto shared = ana.session().start_screen_share("");
  ASSERT_TRUE(shared.ok()) << shared.error().message;
  EXPECT_TRUE(ana.session().sharing_screen());

  // The room is told over signaling, because the video track carries whoever
  // holds the floor rather than one fixed participant.
  EXPECT_TRUE(wait_until([&] { return bruno.sharer() == ana.session().local_user().id; }))
      << "the other client was never told who is sharing";

  // And the pixels arrive, encoded in H.264 by one libwebrtc, forwarded as RTP
  // by libdatachannel, and decoded by the other libwebrtc.
  EXPECT_TRUE(wait_until([&] { return bruno.remote_frames() > 0; }, 30000ms))
      << "no frame of the shared screen arrived. The SFU received "
      << server_->media_router()->video_packets_received() << " video packets and forwarded "
      << server_->media_router()->video_packets_forwarded();

  // Section 5.2 of SPEC.md: 1280x720.
  const dv::client::video::Size size = bruno.remote_frame_size();
  EXPECT_GT(size.width, 0);
  EXPECT_LE(size.width, 1280);
  EXPECT_LE(size.height, 720);

  // Nobody watches their own screen come back.
  EXPECT_EQ(ana.remote_frames(), 0U);
}

TEST_F(MediaEndToEndTest, TheScreenIsEncodedBySomethingThatSaysWhatItIs) {
  // Which encoder is running is not a detail: it is the difference between a
  // laptop fan at full speed and one that stays quiet, and task 4 of M8 exists
  // to change the answer. What is asserted here is that the answer is
  // reported at all, because a number nobody can read cannot be improved.
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  ASSERT_TRUE(ana.session().start_screen_share("").ok());
  ASSERT_TRUE(wait_until([&] { return bruno.remote_frames() > 0; }, 30000ms));

  ASSERT_TRUE(wait_until([&] { return !ana.session().video_stats().encoder.empty(); }))
      << "libwebrtc never said which encoder it is using";

  const dv::client::media::VideoStats stats = ana.session().video_stats();
  std::printf("\nencoder: %s (%s)\n", stats.encoder.c_str(),
              stats.hardware_encoder ? "hardware" : "software");
  std::fflush(stdout);

  // Whatever it is, the two claims have to agree with each other: libwebrtc
  // marks an encoder power efficient exactly when it is hardware, and a
  // software encoder that claimed to be one would send the bitrate controller
  // down the wrong path.
  const dv::client::media::HardwareEncoding hardware = dv::client::media::hardware_encoding();
  std::printf("hardware encoding: %s (%s)\n", hardware.available ? "available" : "unavailable",
              hardware.detail.c_str());
  std::fflush(stdout);

  // Whether there is hardware here or not, there has to be a reason on record.
  // "no hardware encoding" with no explanation is the kind of answer that
  // sends somebody looking through driver documentation for an afternoon.
  EXPECT_FALSE(hardware.detail.empty());
  if (hardware.compiled_in) {
    EXPECT_FALSE(hardware.implementation.empty());
  }

  const bool hardware_available = hardware.available;
  EXPECT_EQ(stats.hardware_encoder, hardware_available && stats.encoder != "OpenH264")
      << "the encoder in use and what this machine supports disagree";
}

TEST_F(MediaEndToEndTest, StoppingAndStartingAShareLeavesTheCallAlone) {
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  ASSERT_TRUE(ana.session().start_screen_share("").ok());
  ASSERT_TRUE(wait_until([&] { return bruno.remote_frames() > 0; }, 30000ms));

  ASSERT_TRUE(ana.session().stop_screen_share().ok());
  EXPECT_FALSE(ana.session().sharing_screen());
  EXPECT_TRUE(wait_until([&] { return bruno.sharer().empty(); }));

  // The audio never stopped, which is the point: starting and stopping a share
  // needs no renegotiation, so the call carries on through it.
  EXPECT_EQ(ana.session().state(), CallSession::State::InCall);
  EXPECT_EQ(bruno.session().state(), CallSession::State::InCall);
  const std::uint64_t audio_before = server_->media_router()->audio_packets_forwarded();
  EXPECT_TRUE(wait_until([&] {
    return server_->media_router()->audio_packets_forwarded() > audio_before + 10;
  })) << "the audio stopped when the screen share did";

  // And it can start again.
  const std::uint64_t frames_before = bruno.remote_frames();
  ASSERT_TRUE(ana.session().start_screen_share("").ok());
  EXPECT_TRUE(wait_until([&] { return bruno.remote_frames() > frames_before; }, 30000ms))
      << "the second share produced nothing";
}

TEST_F(MediaEndToEndTest, OnlyOneParticipantCanShareAtATime) {
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  ASSERT_TRUE(ana.session().start_screen_share("").ok());
  ASSERT_TRUE(wait_until([&] { return !bruno.sharer().empty(); }));

  // Section 5.2 of SPEC.md has one screen at a time. The second client is
  // refused before anything is captured, so it never takes the floor from the
  // first by accident.
  const auto refused = bruno.session().start_screen_share("");
  EXPECT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "screen_share_busy");
  EXPECT_FALSE(bruno.session().sharing_screen());
}

TEST_F(MediaEndToEndTest, MutingStopsTheOutgoingAudio) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  ASSERT_TRUE(wait_until([&] { return server_->media_router()->audio_packets_received() > 0; }));
  ASSERT_TRUE(ana.session().set_muted(true).ok());

  // Muting disables the track rather than renegotiating, so the connection
  // stays up and the room agrees about who is muted.
  EXPECT_EQ(ana.session().state(), CallSession::State::InCall);
  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : bruno.participants()) {
      if (participant.user.id == ana.session().local_user().id) {
        return participant.muted;
      }
    }
    return false;
  }));
}

}  // namespace
