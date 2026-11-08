#include "battery_fuel_gauge.h"

#include <unity.h>

#include <memory>

std::unique_ptr<BatteryFuelGauge> gauge;

void setUp(void) { gauge.reset(new BatteryFuelGauge()); }

void testUninitializedGauge() {
  TEST_ASSERT_EQUAL(0, gauge->getSoc());
  TEST_ASSERT_EQUAL(0, gauge->getMilliampSecondsDischarged());
  TEST_ASSERT_EQUAL(0, gauge->getMilliampSecondsRecharged());
}

void testLearningProgress() {
  TEST_ASSERT_EQUAL(0, gauge->getLearningProgressPercent());
  TEST_ASSERT_FALSE(gauge->isLearned());
  FuelGaugeState state;
  state.bottomMilliampSeconds = 3600 * 1000;
  state.topSoc = 100;
  state.bottomSoc = 70;  // 30 of 60 points
  gauge->restoreState(state);
  TEST_ASSERT_EQUAL(50, gauge->getLearningProgressPercent());
  TEST_ASSERT_FALSE(gauge->isLearned());
  state.bottomSoc = 30;  // 70 points, past the threshold
  gauge = std::make_unique<BatteryFuelGauge>();
  gauge->restoreState(state);
  TEST_ASSERT_EQUAL(100, gauge->getLearningProgressPercent());
  TEST_ASSERT_TRUE(gauge->isLearned());
}

void testEstimatedCapacity() {
  TEST_ASSERT_EQUAL(-1, gauge->getEstimatedCapacityMah());
  FuelGaugeState state;
  // 3600 mAh drawn while the tracked range covers 100% -> 40% (60 points):
  // implies 6000 mAh.
  state.bottomMilliampSeconds = 3600 * 3600;
  state.currentMilliampSeconds = 0;
  state.topSoc = 100;
  state.bottomSoc = 40;
  gauge->restoreState(state);
  TEST_ASSERT_EQUAL(6000, gauge->getEstimatedCapacityMah());
  // A range narrower than 40 points is not trusted.
  state.bottomSoc = 70;
  gauge = std::make_unique<BatteryFuelGauge>();
  gauge->restoreState(state);
  TEST_ASSERT_EQUAL(-1, gauge->getEstimatedCapacityMah());
}

void testCurrentRideSpentAndRegeneratedStats() {
  gauge->updateCurrent(6666, 0);  // The first current is disregarded
  gauge->updateCurrent(1000, 123);
  TEST_ASSERT_EQUAL(0, gauge->getMilliampSecondsRecharged());
  TEST_ASSERT_EQUAL(123, gauge->getMilliampSecondsDischarged());
  gauge->updateCurrent(-1000, 456);
  TEST_ASSERT_EQUAL(333, gauge->getMilliampSecondsRecharged());
  TEST_ASSERT_EQUAL(123, gauge->getMilliampSecondsDischarged());
}

void testSocAfterFirstVoltageMessageAndStateSaving() {
  gauge->updateVoltage(3800, 0);
  TEST_ASSERT_EQUAL(53, gauge->getSoc());
  auto state = gauge->getState();
  gauge.reset(new BatteryFuelGauge());
  gauge->restoreState(state);
  TEST_ASSERT_EQUAL(53, gauge->getSoc());
}

/**
 * Emulates the following scenario:
 * 1. Turns on the board in chargin mode at 18% SOC based on voltage
 * 2. Charges the board with 2 amps for an hour. Emulates the target voltage
 * based SOC of 68%
 * 3. Turns off the board.
 * 4. Turns on and discharges at 6 amps for 10 minutes.
 */
void testSimpleChargeAndHalfwayDischarge() {
  gauge->updateVoltage(3400, 0);
  TEST_ASSERT_EQUAL(18, gauge->getSoc());
  for (int32_t time = 0; time <= 3600 * 1000; time += 100) {
    gauge->updateCurrent(-2000, time);
    // Also update the voltage with the target voltage so that
    // we saturate the low pass filter
    gauge->updateVoltage(3908, time);
  }
  TEST_ASSERT_EQUAL(68, gauge->getSoc());
  TEST_ASSERT_EQUAL(2 * 3600 * 1000, gauge->getMilliampSecondsRecharged());
  TEST_ASSERT_EQUAL(0, gauge->getMilliampSecondsDischarged());

  // Recreate based on state
  auto state = gauge->getState();
  gauge.reset(new BatteryFuelGauge());
  gauge->restoreState(state);
  TEST_ASSERT_EQUAL(68, gauge->getSoc());

  for (int32_t time = 0; time <= 10 * 60 * 1000; time += 100) {
    gauge->updateCurrent(6000, time);
    // Note that we're updating voltage with a value that is off.
    // The fuel gauge must correctly go based solely off Ah calculations.
    gauge->updateVoltage(3908, time);
  }
  state = gauge->getState();
  TEST_ASSERT_EQUAL(43, gauge->getSoc());
  TEST_ASSERT_EQUAL(6 * 10 * 60 * 1000, gauge->getMilliampSecondsDischarged());
  TEST_ASSERT_EQUAL(0, gauge->getMilliampSecondsRecharged());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(testUninitializedGauge);
  RUN_TEST(testLearningProgress);
  RUN_TEST(testEstimatedCapacity);
  RUN_TEST(testCurrentRideSpentAndRegeneratedStats);
  RUN_TEST(testSocAfterFirstVoltageMessageAndStateSaving);
  RUN_TEST(testSimpleChargeAndHalfwayDischarge);
  UNITY_END();

  return 0;
}