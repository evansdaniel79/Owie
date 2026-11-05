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

// Bits 7:6 of the BMS status byte (message 0x0) are the BMS's own coarse
// state of charge: 00 = above 80 %, 01 = 50..80 %, 10 = 20..50 %,
// 11 = 20 % or below.
constexpr uint8_t STATUS_SOC_BUCKET_MASK = 0xC0;

uint8_t socBucketBits(int soc_percent) {
  if (soc_percent > 80) {
    return 0x00;
  }
  if (soc_percent > 50) {
    return 0x40;
  }
  if (soc_percent > 20) {
    return 0x80;
  }
  return 0xC0;
}

}  // namespace

void BmsRelay::batteryPercentageParser(Packet& p) {
  if (p.getType() != 3) {
    return;
  }
  // 0x3 message is just one byte containing battery percentage.
  bms_soc_percent_ = *(int8_t*)p.data();
  overridden_soc_percent_ = battery_fuel_gauge_.getSoc();
  if (pass_through_soc_) {
    // Stock battery mode: leave the BMS value alone and forward it as is.
    // overridden_soc_percent_ is still computed for the monitor page.
    return;
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

  // 0x5 message encodes current as signed int16 in raw BMS ADC counts,
  // positive = discharge. The scaling factor (tested on a Pint) is 0.055,
  // i.e. 1 count corresponds to 55 milliamps. That matches the BMS hardware
  // (12-bit ADC, 4.5 V reference, 1 mOhm shunt behind a gain-20 amplifier
  // -> 54.9 mA/count) and the BMS's own state-of-charge full-scale
  // constants, so it applies to XR and Pint boards alike.
  current_milliamps_ = int16FromNetworkOrder(p.data()) * CURRENT_SCALER;
  battery_fuel_gauge_.updateCurrent(current_milliamps_, now_millis_);
}

void BmsRelay::bmsSerialParser(Packet& p) {
  if (p.getType() != 6) {
    return;
  }
  // 0x6 message has the BMS serial number encoded inside of it.
  // It is the last seven digits from the sticker on the back of the BMS. On
  // the BMS side these are the low bytes of the four user-ID words of its
  // microcontroller; the BMS only reports them and never checks anything, so
  // whatever pairing exists is decided by the controller. XR BMSes with
  // firmware 2006 (hardware 4206-4209) do not send this message at all;
  // 4210 and Pint boards do.
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
  // The data in this packet is 16 uint16-s. The first 15 are the individual
  // cell voltages in millivolts. The 16th is not a cell: the BMS's send loop
  // runs one word past its 15-entry cell array, so the word that follows it
  // in BMS RAM goes out too - the BMS's millisecond counter since its
  // one-second tick, typically 42 (the time its measurement block needs to
  // get to this message).
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
  // 0x4 message: five unsigned bytes in degrees Celsius. The BMS clamps at
  // 0 C from below and reports 0xFF for a reading above the clamp (its
  // "sensor error" case, e.g. a shorted thermistor). The
  // first four are the pack thermistors; the fifth sits on the BMS board
  // (different NTC curve, 175 C clamp) and the BMS itself leaves it out of
  // its own min/max logic, so it is left out of the average here too.
  const uint8_t* const temperatures = p.data();
  uint16_t temperature_sum = 0;
  uint8_t valid_count = 0;
  for (int i = 0; i < 5; i++) {
    const uint8_t temp = temperatures[i];
    temperatures_celsius_[i] = temp;
    if (i == BOARD_TEMPERATURE_INDEX || temp == TEMPERATURE_INVALID) {
      continue;
    }
    temperature_sum += temp;
    valid_count++;
  }
  avg_temperature_celsius_ =
      valid_count == 0 ? 0 : temperature_sum / valid_count;
}

void BmsRelay::shutdownWarningParser(Packet& p) {
  if (p.getType() != 1) {
    return;
  }
  // 0x1 message: the BMS is counting down to a protective power-off. Sent
  // every 100 ms while any countdown is below 29 s. Byte 0 = seconds left,
  // byte 1 = reason mask (1 over-voltage, 2 under-voltage, 4 over-temperature,
  // 8 idle timer). Forwarded unchanged.
  shutdown_countdown_seconds_ = p.data()[0];
  shutdown_reason_mask_ = p.data()[1];
  shutdown_warning_millis_ = now_millis_;
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

void BmsRelay::bmsStatusParser(Packet& p) {
  if (p.getType() != 0) {
    return;
  }
  last_status_byte_ = p.data()[0];

  // Forwarding the status packet during normal operation seems to drive
  // an event loop in the controller that tallies used Ah and eventually
  // makes the board throw Error 23. Swallowing the packet does the trick.
  // Stock battery mode (pass_through_status_) forwards it regardless.
  p.setShouldForward(pass_through_status_ || isCharging() || isBatteryEmpty() ||
                     isBatteryTempOutOfRange() || isBatteryOvercharged());
  battery_fuel_gauge_.updateChargingStatus(isCharging());

  // When the packet does go out, make its SOC bucket (bits 7:6) agree with
  // the percentage we report in message 0x3 instead of the BMS's own idea of
  // the pack, which is wrong for an expanded battery. In stock battery mode
  // message 0x3 is the BMS's own value, so leave the bucket alone too.
  if (!pass_through_soc_ && overridden_soc_percent_ >= 0) {
    p.data()[0] = (last_status_byte_ & ~STATUS_SOC_BUCKET_MASK) |
                  socBucketBits(overridden_soc_percent_);
  }
}
