// The sentence under the microphone box in Settings, for a device that
// captures less than a voice needs.
//
// Words are the whole feature here: a person reading them has to come away
// knowing what is wrong and what to do, and the two cases have two different
// fixes. So the words are pinned, away from any dialog.
#include <string>

#include <gtest/gtest.h>

#include "audio/microphone_hint.hpp"

namespace {

using dv::client::audio::kNarrowMicrophoneHz;
using dv::client::audio::microphone_rate_hint;

TEST(MicrophoneHint, SaysNothingForAMicrophoneThatCarriesTheVoice) {
  EXPECT_TRUE(microphone_rate_hint(48000, "Microfone (Realtek)").empty());
  EXPECT_TRUE(microphone_rate_hint(44100, "USB Audio").empty());
  EXPECT_TRUE(microphone_rate_hint(kNarrowMicrophoneHz, "Anything").empty());
}

TEST(MicrophoneHint, SaysNothingBeforeAnythingWasCaptured) {
  // Zero is "no frame yet", not a device at zero hertz.
  EXPECT_TRUE(microphone_rate_hint(0, "Microfone (Realtek)").empty());
  EXPECT_TRUE(microphone_rate_hint(-1, "Microfone (Realtek)").empty());
}

TEST(MicrophoneHint, NamesTheRateTheLossAndTheWindowsFix) {
  const std::string hint = microphone_rate_hint(16000, "Headset Microphone (Jabra)");
  EXPECT_NE(hint.find("16 kHz"), std::string::npos) << hint;
  EXPECT_NE(hint.find("above 8 kHz is lost"), std::string::npos) << hint;
  EXPECT_NE(hint.find("Windows sound settings"), std::string::npos) << hint;
  EXPECT_NE(hint.find("48000 Hz"), std::string::npos) << hint;
  EXPECT_EQ(hint.find("Bluetooth"), std::string::npos) << hint;
}

TEST(MicrophoneHint, KnowsTheHandsFreeProfileHasNoFix) {
  // Windows names the Bluetooth hands-free endpoint after the profile, and the
  // profile is what caps the rate: sending somebody into the sound settings
  // would send them nowhere.
  const std::string hint = microphone_rate_hint(16000, "Headset (WH-1000XM4 Hands-Free AG Audio)");
  EXPECT_NE(hint.find("hands-free profile"), std::string::npos) << hint;
  EXPECT_NE(hint.find("wired"), std::string::npos) << hint;
  EXPECT_EQ(hint.find("Windows sound settings"), std::string::npos) << hint;
}

TEST(MicrophoneHint, EightKilohertzIsATelephone) {
  const std::string hint = microphone_rate_hint(8000, "Old modem");
  EXPECT_NE(hint.find("8 kHz"), std::string::npos) << hint;
  EXPECT_NE(hint.find("above 4 kHz is lost"), std::string::npos) << hint;
}

}  // namespace
