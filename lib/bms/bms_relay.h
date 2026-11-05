#ifndef BMS_RELAY_H
#define BMS_RELAY_H

#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include "battery_fuel_gauge.h"
#include "packet_tracker.h"

class Packet;

class BmsRelay {
 public:
  /**
   * @brief Function polled for new byte from BMS. Expected to return negative
   * value when there's no data available on the wire.
   */
  typedef std::function<int()> Source;
  /**
   * @brief Function called to send data to the MB.
   */
  typedef std::function<void(uint8_t)> Sink;

  /**
   * @brief Packet callback.
   */
  typedef std::function<void(BmsRelay*, Packet*)> PacketCallback;

  typedef std::function<unsigned long()> MillisProvider;

  /**
   * @brief Called once when the BMS announces that it is powering off.
   * Right before shutting down the BMS sends a bare ASCII fault code without
   * any frame header ("BU" = power button, "DC" = done charging, "CD" =
   * charger disconnected, "ZC" = idle timeout, "UV"/"CUV"/"OV"/"OT"/"OC"/
   * "QC"/"PR"/"CS"/"CC" = protections), then drains its TX buffer and waits
   * about a second before cutting power. The argument is that code.
   */
  typedef std::function<void(const char*)> ShutdownCallback;

  BmsRelay(const Source& source, const Sink& sink,
           const MillisProvider& millisProvider);

  /**
   * @brief All of the data ingestion, processing and forwarding is done here.
   * Must be called continuously from arduino loop.
   */
  void loop();

  void addReceivedPacketCallback(const PacketCallback& callback) {
    receivedPacketCallbacks_.push_back(callback);
  }

  void addForwardedPacketCallback(const PacketCallback& callback) {
    forwardedPacketCallbacks_.push_back(callback);
  }

  void setUnknownDataCallback(const Sink& c) { unknownDataCallback_ = c; }

  void setShutdownCallback(const ShutdownCallback& c) { shutdownCallback_ = c; }

  /**
   * @brief Seconds until the BMS shuts itself down, as last announced in
   * message 0x01, or -1 when no warning was received in the last 2 seconds.
   */
  int getShutdownCountdownSeconds();

  /**
   * @brief Reason bit mask of the last 0x01 warning: 1 = over-voltage,
   * 2 = under-voltage, 4 = over-temperature, 8 = idle (zero current) timer.
   */
  uint8_t getShutdownReasonMask() { return shutdown_reason_mask_; }

  /**
   * @brief Fault code text the BMS sent before powering off ("BU", "DC",
   * ...), empty string if none was seen yet.
   */
  const char* getShutdownCode() { return shutdown_code_; }

  /**
   * @brief If set to non-zero value, spoofs captured BMS serial
   * with the number provided here. The serial number can be found
   * on the sticker on the bottom side of BMS. The lower number on
   * the sticker without the 4 leading (most significant) digits
   * goes here.
   */
  void setBMSSerialOverride(uint32_t serial) { serial_override_ = serial; }

  /**
   * @brief Get the captured BMS serial number.
   *
   * @return non-zero serial, if it's already been captured.
   */
  uint32_t getCapturedBMSSerial() { return captured_serial_; }

  /**
   * @brief Get the BMS serial override value.
   *
   * @return The serial override value, or 0 if not set.
   */
  uint32_t getBMSSerialOverride() { return serial_override_; }

  /**
   * @brief Battery percentage as reported by the BMS.
   */
  int8_t getBmsReportedSOC() { return bms_soc_percent_; }

  /**
   * @brief Spoofed battery percentage sent to the controller.
   */
  int8_t getOverriddenSOC() { return overridden_soc_percent_; }
  /**
   * @brief Cell voltages in millivolts.
   * @return pointer to a 15 element array.
   */
  uint16_t* const getCellMillivolts() { return cell_millivolts_; }

  uint16_t getTotalVoltageMillivolts() { return total_voltage_millivolts_; }

  /**
   * @brief Value the BMS reports when a thermistor reads above its clamp
   * (105 C for the pack sensors, 175 C for the board sensor), e.g. a shorted
   * sensor - the BMS's own "sensor error" case. An open sensor reads as 0 C.
   */
  static constexpr uint8_t TEMPERATURE_INVALID = 0xFF;

  /**
   * @brief Temperatures in degrees Celsius, unsigned (0 = 0 C or below).
   * Indices 0..3 are the pack thermistors, index 4 (BOARD_TEMPERATURE_INDEX)
   * is a thermistor on the BMS board itself with a different NTC curve - it
   * is not part of the pack temperature. TEMPERATURE_INVALID marks a bad
   * reading.
   * @return pointer to a 5 element array.
   */
  const uint8_t* getTemperaturesCelsius() { return temperatures_celsius_; }

  static constexpr int BOARD_TEMPERATURE_INDEX = 4;

  /**
   * @brief Average of the valid pack thermistors (indices 0..3), 0 if none is
   * valid.
   */
  uint8_t getAverageTemperatureCelsius() { return avg_temperature_celsius_; }

  /**
   * @brief Current In Amps.
   */
  int32_t getCurrentMilliamps() { return current_milliamps_; }

  int32_t getUsedChargeMah() {
    return battery_fuel_gauge_.getMilliampSecondsDischarged() / 3600;
  }
  int32_t getRegeneratedChargeMah() {
    return battery_fuel_gauge_.getMilliampSecondsRecharged() / 3600;
  }

  // Status byte (message 0x0) as received from the BMS: bits 1:0 temperature
  // (01 hot, 10 cold, 11 sensor error), bits 3:2 voltage (01 under-voltage,
  // 10 over-voltage), bit 4 balancing, bit 5 charger detected, bits 7:6 the
  // BMS's own SOC bucket (rewritten on forward, see bmsStatusParser).
  bool isCharging() { return last_status_byte_ & 0x20; }
  bool isBatteryEmpty() { return last_status_byte_ & 0x4; }
  bool isBatteryTempOutOfRange() {
    // Both these bits throw the same error, one is prob hot and the other is
    // cold.
    return last_status_byte_ & 3;
  }
  bool isBatteryOvercharged() { return last_status_byte_ & 8; }

  // ---- BMS identity and health, from messages 0x07/0x08/0x09/0x0B/0x0C/0x0D ----

  /**
   * @brief Firmware version from the boot message 0x0B: 2006 = XR hardware
   * 4206-4209, 2007 = XR 4210, 3004/3005/3008 = Pint. 0 until seen (the
   * message is only sent once at power up).
   */
  uint16_t getBmsFirmwareVersion() { return bms_firmware_version_; }

  /**
   * @brief Hardware/variant byte from message 0x08: 0x02 = XR 4208-4210,
   * 0x04 = XR 4206, 0x06 = Pint. 0 until seen.
   */
  uint8_t getBmsVariant() { return bms_variant_; }

  /**
   * @brief Reason for the last BMS power-off, from message 0x09 (the code the
   * BMS stored in its EEPROM when it shut down): 1 DC done charging, 2 CD
   * charger disconnected, 3 BU power button, 4 QC hardware fault input,
   * 5 OC over-current, 6 UV under-voltage, 7 CUV critical under-voltage,
   * 8 OT over-temperature, 9 OV over-voltage, 10 PR pre-charge failed,
   * 11 ZC idle timeout, 12 CS current sensor fault, 13 CC charge over-current
   * (Pint). 0 = none / not seen.
   */
  uint8_t getLastShutdownReasonCode() { return last_shutdown_reason_code_; }
  static const char* shutdownReasonName(uint8_t code);

  // Protection thresholds the BMS reports in message 0x07, 0 until seen.
  uint16_t getOverVoltageThresholdMillivolts() { return ov_threshold_mv_; }
  uint16_t getChargeCompleteThresholdMillivolts() { return full_threshold_mv_; }
  uint16_t getUnderVoltageThresholdMillivolts() { return uv_threshold_mv_; }
  uint8_t getOverTemperatureThresholdCelsius() { return ot_threshold_c_; }

  /** @brief Cell balancing mask from message 0x0C, one bit per cell. */
  uint16_t getBalancingMask() { return balancing_mask_; }

  /**
   * @brief Cycle counter and state of health from message 0x0D. Only XR 4210
   * and Pint BMSes send it; hasCycleInfo() tells whether it was seen.
   */
  bool hasCycleInfo() { return has_cycle_info_; }
  uint16_t getCycleCount() { return cycle_count_; }
  uint8_t getStateOfHealthPercent() { return state_of_health_percent_; }

  const PacketTracker& getPacketTracker() const { return packet_tracker_; }

  BatteryFuelGauge& getBatteryFuelGauge() { return battery_fuel_gauge_; }

  /**
   * @brief "Stock battery mode" switches. All default to false (original Owie
   * behavior). Only enable these on a board with a non-upgraded stock battery.
   *
   * Owie still parses every packet and keeps its own stats either way; these
   * only control what is sent on to the board.
   */
  /** Forward the BMS battery percentage untouched instead of Owie's estimate. */
  void setPassThroughSoc(bool enabled) { pass_through_soc_ = enabled; }
  /** Always forward the current packet (restores regen percent and range). */
  void setPassThroughCurrent(bool enabled) { pass_through_current_ = enabled; }
  /** Always forward the status packet, not only on warnings or charging. */
  void setPassThroughStatus(bool enabled) { pass_through_status_ = enabled; }

 private:
  // BMS current units to milliamps.
  static constexpr int CURRENT_SCALER = 55;
  void processNextByte();
  void purgeUnknownData();
  void trackUnknownByte(uint8_t b);
  void maybeReplayPackets();
  void ingestPacket(Packet& p);

  std::vector<PacketCallback> receivedPacketCallbacks_;
  std::vector<PacketCallback> forwardedPacketCallbacks_;
  Sink unknownDataCallback_;
  ShutdownCallback shutdownCallback_;

  // Detection of the header-less fault code that precedes a BMS power-off:
  // the first bytes of an unknown-data run that directly follows a valid
  // packet.
  bool seen_valid_packet_ = false;
  uint8_t unknown_run_[3] = {0};
  uint8_t unknown_run_len_ = 0;
  bool unknown_run_fired_ = false;
  char shutdown_code_[4] = {0};

  int8_t shutdown_countdown_seconds_ = -1;
  uint8_t shutdown_reason_mask_ = 0;
  int32_t shutdown_warning_millis_ = 0;

  std::vector<uint8_t> sourceBuffer_;
  uint32_t serial_override_ = 0;
  uint32_t captured_serial_ = 0;
  int16_t current_milliamps_ = 0;

  int8_t bms_soc_percent_ = -1;
  int8_t overridden_soc_percent_ = -1;
  uint16_t cell_millivolts_[15] = {0};
  uint16_t total_voltage_millivolts_ = 0;

  uint8_t temperatures_celsius_[5] = {0};
  uint8_t avg_temperature_celsius_ = 0;
  uint8_t last_status_byte_ = 0;
  bool pass_through_soc_ = false;
  bool pass_through_current_ = false;
  bool pass_through_status_ = false;
  uint16_t bms_firmware_version_ = 0;
  uint8_t bms_variant_ = 0;
  uint8_t last_shutdown_reason_code_ = 0;
  uint16_t ov_threshold_mv_ = 0;
  uint16_t full_threshold_mv_ = 0;
  uint16_t uv_threshold_mv_ = 0;
  uint8_t ot_threshold_c_ = 0;
  uint16_t balancing_mask_ = 0;
  bool has_cycle_info_ = false;
  uint16_t cycle_count_ = 0;
  uint8_t state_of_health_percent_ = 0;
  const Source source_;
  const Sink sink_;
  const MillisProvider millis_provider_;
  int32_t now_millis_;
  PacketTracker packet_tracker_;
  BatteryFuelGauge battery_fuel_gauge_;

  void bmsStatusParser(Packet& p);
  void shutdownWarningParser(Packet& p);
  void bmsSerialParser(Packet& p);
  void currentParser(Packet& p);
  void batteryPercentageParser(Packet& p);
  void cellVoltageParser(Packet& p);
  void temperatureParser(Packet& p);
  void bmsInfoParser(Packet& p);
};

#endif  // BMS_RELAY_H