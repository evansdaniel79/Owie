#include <algorithm>
#include <cmath>
#include <cstdint>

#include "bms_relay.h"
#include "packet.h"

namespace {

inline int16_t int16FromNetworkOrder(const void* const p) {
  uint8_t* const charPointer = (uint8_t* const)p;
  return ((uint16_t)(*charPointer)) << 8 | *(charPointer + 1);
}

}  // namespace

void BmsRelay::batteryPercentageParser(Packet& p) {
  if (p.getType() != 3) {
    return;
  }
  // 0x3 message is just one byte containing battery percentage.
  bms_soc_percent_ = *(int8_t*)p.data();
  // Same rule as the released lolwheel v1.4.3: percentage comes from the
  // smoothed lowest cell voltage through the lookup table. (The coulomb
  // counting gauge on lolwheel's main branch is unfinished and not used here;
  // it only feeds the used/regen statistics.)
  overridden_soc_percent_ = battery_fuel_gauge_.getVoltageBasedSoc();
  if (pass_through_soc_) {
    return;  // stock battery % switch: BMS value forwarded untouched
  }
  if (overridden_soc_percent_ < 0) {
    p.setShouldForward(false);
    return;
  }
  p.data()[0] = overridden_soc_percent_;
}

void BmsRelay::currentParser(Packet& p) {
  if (p.getType() != 5) {
    return;
  }
  p.setShouldForward(pass_through_current_ || isCharging());

  // 0x5 message encodes current as signed int16.
  // The scaling factor (tested on a Pint) seems to be 0.055
  // i.e. 1 in the data message below corresponds to 55 milliamps.
  current_milliamps_ = int16FromNetworkOrder(p.data()) * CURRENT_SCALER;
  battery_fuel_gauge_.updateCurrent(current_milliamps_, now_millis_);
}

void BmsRelay::bmsSerialParser(Packet& p) {
  if (p.getType() != 6) {
    return;
  }
  // 0x6 message has the BMS serial number encoded inside of it.
  // It is the last seven digits from the sticker on the back of the BMS.
  if (captured_serial_ == 0) {
    for (int i = 0; i < 4; i++) {
      captured_serial_ |= p.data()[i] << (8 * (3 - i));
    }
  }
  if (serial_override_ == 0) {
    return;
  }
  uint32_t serial_override_copy = serial_override_;
  for (int i = 3; i >= 0; i--) {
    p.data()[i] = serial_override_copy & 0xFF;
    serial_override_copy >>= 8;
  }
}

void BmsRelay::cellVoltageParser(Packet& p) {
  if (p.getType() != 2) {
    return;
  }
  // The data in this packet is 16 int16-s. First 15 of them is
  // individual cell voltages in millivolts. The last value is mysterious.
  const uint8_t* const data = p.data();
  uint16_t total_voltage = 0;
  uint16_t min_voltage = 0xFFFF;
  for (int i = 0; i < 15; i++) {
    uint16_t cellVoltage = int16FromNetworkOrder(data + (i << 1));
    total_voltage += cellVoltage;
    if (cellVoltage < min_voltage) {
      min_voltage = cellVoltage;
    }
    cell_millivolts_[i] = cellVoltage;
  }
  total_voltage_millivolts_ = total_voltage;
  battery_fuel_gauge_.updateVoltage(min_voltage, now_millis_);
}

void BmsRelay::temperatureParser(Packet& p) {
  if (p.getType() != 4) {
    return;
  }
  int8_t* const temperatures = (int8_t*)p.data();
  int16_t temperature_sum = 0;
  for (int i = 0; i < 5; i++) {
    int8_t temp = temperatures[i];
    temperature_sum += temp;
    temperatures_celsius_[i] = temp;
  }
  avg_temperature_celsius_ = temperature_sum / 5;
}

void BmsRelay::bmsStatusParser(Packet& p) {
  if (p.getType() != 0) {
    return;
  }
  last_status_byte_ = p.data()[0];

  // Forwarding the status packet during normal operation seems to drive
  // an event loop in the controller that tallies used Ah and eventually
  // makes the board throw Error 23. Swallowing the packet does the trick.
  p.setShouldForward(pass_through_status_ || isCharging() || isBatteryEmpty() ||
                     isBatteryTempOutOfRange() || isBatteryOvercharged());
  battery_fuel_gauge_.updateChargingStatus(isCharging());
}

void BmsRelay::bmsInfoParser(Packet& p) {
  const uint8_t* const data = p.data();
  switch (p.getType()) {
    case 0x07:
      // Thresholds: over-voltage mV, charge-complete mV, under-voltage mV
      // (uint16 each), over-temperature C.
      ov_threshold_mv_ = int16FromNetworkOrder(data);
      full_threshold_mv_ = int16FromNetworkOrder(data + 2);
      uv_threshold_mv_ = int16FromNetworkOrder(data + 4);
      ot_threshold_c_ = data[6];
      break;
    case 0x08:
      bms_variant_ = data[0];
      break;
    case 0x09:
      last_shutdown_reason_code_ = data[0];
      break;
    case 0x0B:
      bms_firmware_version_ = int16FromNetworkOrder(data);
      break;
    case 0x0C:
      balancing_mask_ = int16FromNetworkOrder(data);
      break;
    case 0x0D:
      cycle_count_ = int16FromNetworkOrder(data);
      state_of_health_percent_ = data[2];
      has_cycle_info_ = true;
      break;
    default:
      break;
  }
}

const char* BmsRelay::shutdownReasonName(uint8_t code) {
  static const char* const NAMES[] = {
      "none",
      "DC - done charging",
      "CD - charger disconnected",
      "BU - power button",
      "QC - hardware fault input",
      "OC - over-current",
      "UV - under-voltage",
      "CUV - critical under-voltage",
      "OT - over-temperature",
      "OV - over-voltage",
      "PR - pre-charge failed",
      "ZC - idle timeout",
      "CS - current sensor fault",
      "CC - charge over-current"};
  if (code >= sizeof(NAMES) / sizeof(*NAMES)) {
    return "unknown";
  }
  return NAMES[code];
}
