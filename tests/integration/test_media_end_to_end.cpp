// The interoperation the whole plan rests on: libwebrtc on the client and
// libdatachannel on the server, negotiating and carrying audio between them.
//
// PLAN.md lists this as a high risk, and this is what retires it. Everything
// here is real: a real signaling server, a real SFU, real libwebrtc peer
// connections, real ICE, DTLS and SRTP. Only the network is local.
//
// Built only when DV_BUILD_CLIENT_MEDIA is on, because it needs the libwebrtc
// tree from docs/07-webrtc-toolchain.md.
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

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#endif

#include <iostream>
#include <string>
#include <vector>

#include "media_test_client.hpp"
#include "store/memory_store.hpp"
#include "tone_player.hpp"

namespace {

/// Whether a capture device is attached to this machine, or woken over a link
/// to another one.
///
/// This is the difference between measuring the program and measuring the
/// hardware, and only one of the two is worth asserting on. Switching to a
/// Continuity microphone asks macOS to wake an iPhone over the network; on the
/// machine this was written on that took 1808 ms, against 172 to 192 ms for
/// every device physically attached to it. None of that 1808 ms is spent in
/// code this repository owns.
///
/// Everything is local where the question cannot be asked, so Linux and
/// Windows keep exactly the behaviour they had.
[[nodiscard]] bool device_is_local([[maybe_unused]] const std::string& name) {
#ifndef __APPLE__
  return true;
#else
  // libwebrtc names the platform default "default (Something)" and every other
  // entry by the device's own name, so the inner name is what CoreAudio will
  // answer to. Unwrapping it is what makes the default entry classifiable at
  // all: without this, a machine whose default microphone is a Continuity one
  // would read as local and the measurement would be the phone again.
  constexpr std::string_view kDefaultPrefix = "default (";
  std::string wanted = name;
  if (wanted.starts_with(kDefaultPrefix) && wanted.ends_with(')')) {
    wanted = wanted.substr(kDefaultPrefix.size(), wanted.size() - kDefaultPrefix.size() - 1);
  }

  AudioObjectPropertyAddress address{.mSelector = kAudioHardwarePropertyDevices,
                                     .mScope = kAudioObjectPropertyScopeGlobal,
                                     .mElement = kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) !=
      noErr) {
    return true;
  }

  std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
  if (devices.empty() || AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                                    &size, devices.data()) != noErr) {
    return true;
  }

  for (const AudioObjectID device : devices) {
    address.mSelector = kAudioObjectPropertyName;
    CFStringRef device_name = nullptr;
    size = sizeof(device_name);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &device_name) != noErr ||
        device_name == nullptr) {
      continue;
    }
    char buffer[512] = {};
    const bool converted =
        CFStringGetCString(device_name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(device_name);
    if (!converted || wanted != buffer) {
      continue;
    }

    address.mSelector = kAudioDevicePropertyTransportType;
    UInt32 transport = 0;
    size = sizeof(transport);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &transport) != noErr) {
      return true;
    }
    // The two current Continuity spellings, and not Bluetooth: a headset that
    // is already paired and streaming switches as fast as a built-in
    // microphone, and excluding it would throw away a device worth measuring.
    //
    // The older `kAudioDeviceTransportTypeContinuityCapture` is not listed.
    // macOS 13 both introduced and deprecated it in favour of these two, and
    // 13 is this project's floor, so naming it would buy nothing and cost a
    // deprecation warning.
    return transport != kAudioDeviceTransportTypeContinuityCaptureWired &&
           transport != kAudioDeviceTransportTypeContinuityCaptureWireless;
  }

  // A name CoreAudio does not answer to. Nothing is known, so nothing is
  // excluded.
  return true;
#endif
}

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
  // And it arrives as RED: the offer carries it, and libwebrtc pairs it with
  // Opus only when every detail of that offer is right. A client that quietly
  // fell back to Opus alone would pass every other line of this test.
  EXPECT_TRUE(wait_until([&] { return router->audio_red_packets_received() > 0; }))
      << "the audio reached the SFU without redundancy, so the RED in the offer was not "
         "accepted";
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

  std::vector<media::AudioDevice> local;
  for (const media::AudioDevice& device : inputs.value()) {
    if (device_is_local(device.name)) {
      local.push_back(device);
    }
  }
  if (local.empty()) {
    GTEST_SKIP() << "every capture device here is woken over a link to another machine, so the "
                    "only switch available would time that link and not this code";
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

  // The second local device when there is one, otherwise the same device
  // again: either way the capture is stopped and started, which is what has to
  // stay quick. Section 9 of SPEC.md allows 500 ms.
  //
  // Local, and not simply the second entry of the list. See `device_is_local`:
  // the entry that happens to sit at index 1 can be a microphone on another
  // machine, and the wait for that one to wake is not a property of this code.
  const media::AudioDevice& target = local.size() > 1 ? local[1] : local[0];

  const auto switched_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(ana.session().set_input_device(target.id).ok()) << "could not select " << target.name;

  // The gap is measured from the audio the server actually receives, which is
  // the only place where an interruption is real rather than assumed.
  std::uint64_t before = router->audio_packets_received();
  ASSERT_TRUE(wait_until([&] { return router->audio_packets_received() > before + 5; }, 3000ms))
      << "audio never came back after the switch";

  // The second data point this comment used to ask for, measured on 2026-09-01
  // on macOS arm64 with real devices, switching to each of the four in turn:
  //
  //   default (MacBook Air Microphone)    188 ms
  //   Eder's iPhone Microphone           1808 ms   <- Continuity
  //   MacBook Air Microphone              192 ms
  //   Microsoft Teams Audio               172 ms
  //
  // So the path itself costs around 190 ms and the budget is not tight. What
  // used to fail here was the device selection: this test took the second
  // entry of the list, which on that machine was the phone, and then reported
  // the time macOS spends waking a phone. `device_is_local` is what stopped it
  // doing that.
  //
  // The GitHub runner is not explained by any of the above and is left open.
  // It failed at 2056 ms through a PulseAudio null sink, which is local by
  // every definition here, so this change does not address it: shared hardware
  // remains the likeliest reading, and a run that fails there is still worth
  // looking at rather than assuming.
  //
  // The 500 ms is kept either way. It is exactly the number a person notices
  // when they change microphone mid-call, so raising it to make a job green
  // would trade the only thing this test is for.
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

TEST_F(MediaEndToEndTest, AMicrophoneStillWorksAfterTheSignalingConnectionComesBack) {
  // A blip on the network, a server restart, a laptop waking up. The signaling
  // client reconnects on its own and walks back into the room it was in, which
  // is what makes those look like a pause rather than the end of the call.
  //
  // The accounts and the rooms outlive the restart here, the way they do
  // against a database: without that the clients come back to a server that
  // has never heard of them, and what is under test never happens.
  //
  // The SFU treats the walk back in as a fresh join and builds a new peer
  // connection for it. Whatever the client answers with has to carry a
  // microphone afterwards, or the call comes back with everybody in the
  // participant list and nobody audible.
  dv::server::store::MemoryUserStore users;
  dv::server::store::MemoryRoomStore rooms;

  const std::uint16_t port = server_->port();
  server_->stop();
  server_.reset();

  const auto make_server = [&] {
    SignalingServer::Options options;
    options.bind_address = "127.0.0.1";
    options.port = port;
    options.hub.max_participants_per_room = 5;
    options.hub.heartbeat_interval = 2000ms;
    options.hub.heartbeat_timeout = 60000ms;
    options.hub.users = &users;
    options.hub.rooms = &rooms;
    options.enable_sfu = true;
    options.sfu.ice_servers.clear();
    return std::make_unique<SignalingServer>(options);
  };

  server_ = make_server();
  ASSERT_TRUE(server_->add_user("ana", "password", "Ana").ok());
  ASSERT_TRUE(server_->add_user("bruno", "password", "Bruno").ok());
  server_->start();

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

  ASSERT_TRUE(ana.wait_until_sending_audio())
      << "no audio left this machine before the reconnection, so nothing here means anything";
  ASSERT_TRUE(wait_until([&] { return server_->media_router()->audio_packets_forwarded() > 0; }))
      << "the SFU forwarded nothing before the reconnection";

  const std::uint64_t sent_before = ana.session().stats().bytes_sent;

  // The server goes away and comes back on the same port, with the same
  // accounts and the same room. Every client sees a socket that dropped.
  server_->stop();
  server_.reset();
  server_ = make_server();
  server_->start();
  ASSERT_EQ(server_->port(), port);

  // Both of them find their way back in by themselves.
  ASSERT_TRUE(wait_until([&] { return server_->media_router()->session_count() == 2; }, 30000ms))
      << "the SFU never saw both participants again";
  EXPECT_TRUE(
      wait_until([&] { return ana.session().state() == CallSession::State::InCall; }, 30000ms))
      << "the client never got back into the call";

  // And the microphone is carrying again. Measured from where it was before
  // the restart, because bytes_sent only ever grows.
  EXPECT_TRUE(wait_until([&] { return ana.session().stats().bytes_sent > sent_before; }, 30000ms))
      << "the microphone sent nothing after the reconnection";

  EXPECT_TRUE(
      wait_until([&] { return server_->media_router()->audio_packets_forwarded() > 0; }, 30000ms))
      << "the SFU forwarded nothing after the reconnection: the call came back with everybody "
         "listed and nobody audible";
}

TEST_F(MediaEndToEndTest, AShareStartedAfterTheFirstSharerClosedTheirClientArrives) {
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  // The first sharer does not stop sharing: they close the program, which is
  // what somebody who is finished actually does. Everyone else then gets a
  // renegotiation, because the SFU drops the track that carried the person who
  // left. Bruno shares next, and Carla has to see it.
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

  Client& carla = add("carla");
  ASSERT_TRUE(carla.login());
  ASSERT_TRUE(carla.join(room));
  ASSERT_TRUE(carla.wait_until_in_call());

  const auto by_ana = ana.session().start_screen_share("");
  ASSERT_TRUE(by_ana.ok()) << by_ana.error().message;
  ASSERT_TRUE(wait_until([&] { return carla.remote_frames() > 0; }, 30000ms))
      << "the first share never arrived at all";

  const std::uint64_t before = carla.remote_frames();

  // Closed, not stopped. The room learns about it from the socket dropping.
  ana.session().disconnect();
  ASSERT_TRUE(wait_until([&] { return carla.participants().size() == 2; }, 30000ms))
      << "the room never noticed the first sharer had gone";
  ASSERT_TRUE(wait_until([&] { return carla.sharer().empty(); }))
      << "the room still believes the person who left is sharing";

  const auto by_bruno = bruno.session().start_screen_share("");
  ASSERT_TRUE(by_bruno.ok()) << by_bruno.error().message;
  ASSERT_TRUE(wait_until([&] { return carla.sharer() == bruno.session().local_user().id; }))
      << "the room was never told about the second share";

  EXPECT_TRUE(wait_until([&] { return carla.remote_frames() > before + 5; }, 30000ms))
      << "the second share decoded no frame at the viewer. The SFU received "
      << server_->media_router()->video_packets_received() << " video packets and forwarded "
      << server_->media_router()->video_packets_forwarded();
}

TEST_F(MediaEndToEndTest, AShareThatChangesHandsStillArrivesDecoded) {
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  // Ana shares, stops, and Bruno shares instead. Carla watches the whole time,
  // and has to end up looking at Bruno's screen rather than at the last frame
  // of Ana's.
  //
  // Three clients because nobody receives their own screen back: with only two
  // there is no one left watching once the floor changes hands.
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

  Client& carla = add("carla");
  ASSERT_TRUE(carla.login());
  ASSERT_TRUE(carla.join(room));
  ASSERT_TRUE(carla.wait_until_in_call());

  const auto ana_started = std::chrono::steady_clock::now();
  const auto by_ana = ana.session().start_screen_share("");
  ASSERT_TRUE(by_ana.ok()) << by_ana.error().message;
  ASSERT_TRUE(wait_until([&] { return carla.remote_frames() > 0; }, 30000ms))
      << "the first share never arrived at all";
  const auto first_picture = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - ana_started);

  ASSERT_TRUE(ana.session().stop_screen_share().ok());
  ASSERT_TRUE(wait_until([&] { return carla.sharer().empty(); }))
      << "the room was never told the first share had ended";

  // Whatever Carla decoded of Ana's screen is the baseline. Anything past it
  // has to be Bruno's.
  const std::uint64_t before_bruno = carla.remote_frames();

  const auto by_bruno = bruno.session().start_screen_share("");
  ASSERT_TRUE(by_bruno.ok()) << by_bruno.error().message;
  ASSERT_TRUE(wait_until([&] { return carla.sharer() == bruno.session().local_user().id; }))
      << "the room was never told about the second share";

  const auto bruno_started = std::chrono::steady_clock::now();
  EXPECT_TRUE(wait_until([&] { return carla.remote_frames() > before_bruno; }, 30000ms))
      << "the second share decoded no frame at the viewer. The SFU received "
      << server_->media_router()->video_packets_received() << " video packets and forwarded "
      << server_->media_router()->video_packets_forwarded() << ", and the viewer had decoded "
      << before_bruno << " frames of the first share";
  const auto second_picture = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - bruno_started);

  // The two shares travel the same path and are decoded by the same receiver,
  // so the second one appearing has to cost about what the first one did. A
  // handover that takes many times longer is a picture the room reads as
  // never having arrived: whoever is watching has already said it is broken.
  std::cerr << "first picture after " << first_picture.count() << " ms, second after "
            << second_picture.count() << " ms\n";
  EXPECT_LT(second_picture.count(), first_picture.count() + 3000)
      << "the second sharer took " << second_picture.count()
      << " ms to appear where the first took " << first_picture.count() << " ms";
}

#if defined(_WIN32)

/// The bitrate a client is sending and the other is receiving, averaged over a
/// stretch long enough for the statistics to have been collected twice.
///
/// The bitrate, and not the audio level: libwebrtc computes the level the RTP
/// header extension carries *before* the frame processor runs, so it reports
/// the microphone alone and never the screen audio mixed in after it. That is
/// the right answer for a speaking indicator and the wrong instrument for this
/// test - measured here, a track carrying 94 kbps of music reported a level of
/// 0.0002.
struct Flow {
  double sent = 0;
  double received = 0;
};

[[nodiscard]] Flow flow_between(Client& from, Client& to, std::chrono::milliseconds over) {
  Flow peak;
  const auto deadline = std::chrono::steady_clock::now() + over;
  while (std::chrono::steady_clock::now() < deadline) {
    peak.sent = std::max(peak.sent, from.session().stats().send_bitrate_kbps);
    peak.received = std::max(peak.received, to.session().stats().receive_bitrate_kbps);
    std::this_thread::sleep_for(100ms);
  }
  return peak;
}

TEST_F(MediaEndToEndTest, TheSoundOfASharedScreenReachesTheOtherParticipant) {
  // The whole feature, end to end and with nothing stubbed: this process plays
  // a tone, the loopback capture hears it, the mixer folds it into Ana's own
  // audio track after the echo canceller, Opus encodes it, the SFU forwards it,
  // and Bruno's client receives it.
  //
  // The share is started first and the tone second, so that what is compared is
  // the same share carrying silence and then carrying sound. Comparing against
  // "no share at all" would also be comparing mono against stereo.
  //
  // See docs/09-screen-audio.md.
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

  const std::string ana_id = ana.session().local_user().id;
  ASSERT_TRUE(wait_until([&] { return server_->media_router()->audio_packets_forwarded() > 0; }));

  // This test's own process tree, which is where the tone will come from. The
  // "share the browser tab" case, with the test standing in for the browser.
  const dv::client::app::ScreenAudio audio{
      .mode = dv::client::app::ScreenAudio::Mode::Application,
      .source_id = static_cast<std::uint32_t>(GetCurrentProcessId())};
  const auto shared = ana.session().start_screen_share({}, audio);
  ASSERT_TRUE(shared.ok()) << shared.error().message;

  if (!ana.session().screen_audio_active()) {
    GTEST_SKIP() << "the loopback would not start: "
                 << ana.session().screen_audio_failure().message;
  }

  // Bruno is told, over signaling, that this share has sound in it. Without it
  // he has no way to know why Ana's volume slider is now also the volume of
  // whatever she is playing.
  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : bruno.participants()) {
      if (participant.user.id == ana_id) {
        return participant.sharing_audio;
      }
    }
    return false;
  })) << "bruno was not told the share carries sound";

  // Ana's microphone is muted for the rest of this, and that is not a detail to
  // work around the room: it is the property phase 2 built. With a share on,
  // muting silences the microphone inside the mixer and leaves the track
  // carrying the share, so what crosses the wire below is the screen audio
  // alone.
  ASSERT_TRUE(ana.session().set_muted(true).ok());

  dv::testing::TonePlayer tone(0.2);
  if (!tone.start()) {
    GTEST_SKIP() << "this machine has no playback device to render a tone to";
  }
  const Flow flow = flow_between(ana, bruno, 3000ms);
  tone.stop();

  const media::AudioStats stats = ana.session().stats();

  // The assertion that matters, and the one this test was written a second time
  // to make: the sound reached the *encoder*. Everything upstream of that can
  // be perfectly healthy while nothing is mixed at all - measured here, on a
  // machine whose microphone captures at 16 kHz, where the mixer expected the
  // 48 kHz it had been given in every other setting and quietly passed the
  // frame through instead. The capture counters below were all green while the
  // share was silent for everybody.
  EXPECT_GT(stats.screen_audio_mixed_blocks, 0U) << "nothing was ever mixed into the track";
  EXPECT_LT(stats.screen_audio_starved_blocks, stats.screen_audio_mixed_blocks)
      << "the buffer between the capture and the encoder spent most of its time empty";
  EXPECT_EQ(stats.screen_audio_dropped_frames, 0U)
      << "the encoder could not keep up with the capture";

  // And the capture end was healthy too.
  EXPECT_TRUE(stats.screen_audio_active);
  EXPECT_GT(stats.screen_audio_blocks, 0U);
  EXPECT_LT(stats.screen_audio_silent_blocks, stats.screen_audio_blocks);

  // The track carried all of that with the microphone muted, which is the whole
  // of "muting your microphone must not mute the film".
  EXPECT_TRUE(ana.session().muted());

  // What crossed the wire. A stereo track carrying sound sits far above what a
  // muted microphone alone would cost, which is about a kilobit per second.
  //
  // Deliberately not a comparison against a "silent share" baseline: this
  // process is itself an audio application, and a loopback capture pointed at
  // it hears the call being played back. There is no silence to compare
  // against from in here.
  EXPECT_GT(flow.sent, 40.0) << "ana's track was not carrying sound";
  EXPECT_GT(flow.received, 40.0) << "it reached the encoder but not bruno";
}

TEST_F(MediaEndToEndTest, StoppingTheShareTakesTheSoundWithIt) {
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

  const dv::client::app::ScreenAudio audio{.mode = dv::client::app::ScreenAudio::Mode::System};
  const auto shared = ana.session().start_screen_share({}, audio);
  ASSERT_TRUE(shared.ok()) << shared.error().message;
  if (!ana.session().screen_audio_active()) {
    GTEST_SKIP() << "the loopback would not start: "
                 << ana.session().screen_audio_failure().message;
  }

  const std::string ana_id = ana.session().local_user().id;
  ASSERT_TRUE(wait_until([&] {
    for (const Participant& participant : bruno.participants()) {
      if (participant.user.id == ana_id) {
        return participant.sharing_audio;
      }
    }
    return false;
  }));

  ASSERT_TRUE(ana.session().stop_screen_share().ok());

  EXPECT_FALSE(ana.session().screen_audio_active());
  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : bruno.participants()) {
      if (participant.user.id == ana_id) {
        return !participant.sharing_audio && !participant.sharing_screen;
      }
    }
    return false;
  })) << "bruno still believes the share has sound";

  // The call is untouched by any of it, which is the property that makes a
  // share safe to start and stop in the middle of a conversation.
  EXPECT_EQ(ana.session().state(), CallSession::State::InCall);
  EXPECT_EQ(bruno.session().state(), CallSession::State::InCall);
}

#endif  // defined(_WIN32)

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
