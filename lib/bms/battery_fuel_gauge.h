#ifndef BATTERY_FUEL_GAUGE_H
#define BATTERY_FUEL_GAUGE_H

#include <stdint.h>

#include "filter.h"

struct FuelGaugeState {
  // A positive number, mAs of the deepest discharge seen.
  int32_t bottomMilliampSeconds = 0;
  // A positive number, mAs currently discharged compared to the highest charge
  // seen.
  int32_t currentMilliampSeconds = 0;
  // Voltage-based SOC estimate corresponding to the top of the tracked SOC
  // range.
  int32_t topSoc = 0;
  // Voltage-based SOC estimate corresponding to the bottom of the tracked SOC
  // range.
  int32_t bottomSoc = 0;
};

/**
 * Assumes that the class gets to see all of the energy going into and out of
 * the battery.
 */
class BatteryFuelGauge {
 public:
  // restoreState must be called exatly once and before any other calls on this
  // object.
  void restoreState(const FuelGaugeState& from) { state_ = from; };
  const FuelGaugeState& getState() const { return state_; }

  // Takes a single cell voltage in millivolts.
  void updateVoltage(int32_t voltageMillivolts, int32_t nowMillis);
  void updateCurrent(int32_t currentMilliamps, int32_t nowMillis);
  // Only transition from true to false is expected.
  void updateChargingStatus(bool charging) { charging_ = charging; }
  int32_t getSoc() const;
  int32_t getVoltageBasedSoc() const { return voltage_based_soc_; }

  // Pack capacity in mAh implied by the tracked range: the charge drawn across
  // the range divided by the share of the pack that range covers. -1 until the
  // range spans at least MIN_CAPACITY_WINDOW_SOC points; narrower ranges are
  // too noisy to mean anything. Works for any pack, set up in Settings or not.
  static constexpr int32_t MIN_CAPACITY_WINDOW_SOC = 40;
  int32_t getEstimatedCapacityMah() const {
    const int32_t window = state_.topSoc - state_.bottomSoc;
    if (window < MIN_CAPACITY_WINDOW_SOC || state_.bottomMilliampSeconds <= 0) {
      return -1;
    }
    return (int32_t)((int64_t)state_.bottomMilliampSeconds / 3600 * 100 /
                     window);
  }

  int32_t getMilliampSecondsDischarged() {
    return milliamp_seconds_discharged_;
  }
  int32_t getMilliampSecondsRecharged() { return milliamp_seconds_recharged_; }

  void reset() { state_ = FuelGaugeState(); }

 private:
  void onHighestCharge();
  void onHighestDischarge();

  int32_t voltage_millivolts_ = -1;
  LowPassFilter filtered_voltage_millivolts_;
  int32_t voltage_based_soc_ = -1;
  int32_t last_current_update_time_millis_ = -1;
  int32_t milliamp_seconds_discharged_ = 0;
  int32_t milliamp_seconds_recharged_ = 0;
  bool charging_ = false;
  FuelGaugeState state_;
};

#endif  // BATTERY_FUEL_GAUGE_H
