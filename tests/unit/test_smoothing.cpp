// The arithmetic that makes a moving number look like movement: the meter
// ballistics behind the microphone bar, and the easing every animated part of
// the interface is advanced with.

#include <cmath>

#include <gtest/gtest.h>

#include "app/smoothing.hpp"

namespace {

using dv::client::app::approach;
using dv::client::app::kMeterFloorDb;
using dv::client::app::LevelMeter;
using dv::client::app::meter_fraction;

TEST(ApproachTest, MovesTowardsTheTargetWithoutPassingIt) {
  const double moved = approach(0.0, 1.0, 50.0, 200.0);
  EXPECT_GT(moved, 0.0);
  EXPECT_LT(moved, 1.0);
}

TEST(ApproachTest, OneTimeConstantCoversAboutTwoThirds) {
  // The definition of the time constant, and the reason the numbers in the
  // interface are chosen in milliseconds rather than in frames.
  EXPECT_NEAR(approach(0.0, 1.0, 100.0, 100.0), 0.632, 0.001);
}

TEST(ApproachTest, ArrivesRatherThanHalvingForever) {
  // Without the snap the distance halves every time constant and never
  // reaches zero, so nothing driving an animation would ever find a reason to
  // stop. See MainWindow::animate_level.
  double value = 0.0;
  for (int frame = 0; frame < 200; ++frame) {
    value = approach(value, 1.0, 16.0, 60.0);
  }
  EXPECT_DOUBLE_EQ(value, 1.0);
}

TEST(ApproachTest, ASlowerTimerCoversTheSameGroundInTheSameTime) {
  // The whole point of easing on elapsed time rather than stepping per frame:
  // an animation must not run at half speed because the machine is busy.
  double quick = 0.0;
  for (int frame = 0; frame < 20; ++frame) {
    quick = approach(quick, 1.0, 8.0, 100.0);
  }

  double slow = 0.0;
  for (int frame = 0; frame < 4; ++frame) {
    slow = approach(slow, 1.0, 40.0, 100.0);
  }

  EXPECT_NEAR(quick, slow, 0.01);
}

TEST(ApproachTest, NoTimeConstantMeansArriveNow) {
  EXPECT_DOUBLE_EQ(approach(0.0, 7.0, 16.0, 0.0), 7.0);
}

TEST(ApproachTest, NoTimePassingMovesNothing) {
  EXPECT_DOUBLE_EQ(approach(3.0, 7.0, 0.0, 100.0), 3.0);
}

TEST(MeterFractionTest, SilenceIsEmptyAndFullScaleIsFull) {
  EXPECT_DOUBLE_EQ(meter_fraction(0.0), 0.0);
  EXPECT_DOUBLE_EQ(meter_fraction(1.0), 1.0);
}

TEST(MeterFractionTest, RoomNoiseBelowTheFloorShowsNothing) {
  // Below -60 dBFS is the microphone's own hiss. A bar that shows hiss is a
  // bar that is never empty, and one that is never empty says nothing when
  // somebody starts talking.
  const double below_floor = std::pow(10.0, (kMeterFloorDb - 6.0) / 20.0);
  EXPECT_DOUBLE_EQ(meter_fraction(below_floor), 0.0);
}

TEST(MeterFractionTest, HalfAmplitudeIsNotHalfTheBar) {
  // The reason the scale is logarithmic at all. Half the amplitude is -6 dB,
  // which is a tenth off the top of the bar rather than half of it, and that
  // tenth is what a person hears.
  EXPECT_NEAR(meter_fraction(0.5), 0.9, 0.01);
}

TEST(LevelMeterTest, RisesFasterThanItFalls) {
  // A meter that drops as fast as it rises is a flicker: the peak is gone
  // before the eye that is looking for it arrives.
  LevelMeter rising;
  rising.observe(1.0);
  const double up = rising.advance(60.0);

  LevelMeter falling;
  falling.value = 1.0;
  falling.observe(0.0);
  const double down = 1.0 - falling.advance(60.0);

  EXPECT_GT(up, down);
}

TEST(LevelMeterTest, AMeasurementOutOfRangeIsClamped) {
  LevelMeter meter;
  meter.observe(4.0);
  EXPECT_DOUBLE_EQ(meter.target, 1.0);

  meter.observe(-1.0);
  EXPECT_DOUBLE_EQ(meter.target, 0.0);
}

TEST(LevelMeterTest, ComesToRestSoWhateverDrivesItCanStop) {
  LevelMeter meter;
  meter.observe(0.7);
  EXPECT_FALSE(meter.at_rest());

  for (int frame = 0; frame < 100; ++frame) {
    meter.advance(16.0);
  }
  EXPECT_TRUE(meter.at_rest());
  EXPECT_DOUBLE_EQ(meter.value, 0.7);
}

TEST(LevelMeterTest, AFreshMeterIsAlreadyAtRest) {
  // Nothing measured and nothing drawn. Starting the frame timer for this
  // would be sixty repaints a second of a bar that is empty and staying empty.
  EXPECT_TRUE(LevelMeter{}.at_rest());
}

}  // namespace
