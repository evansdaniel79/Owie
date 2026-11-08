#include <Arduino.h>

#include "battery_fuel_gauge.h"
#include "bms_relay.h"
#include "network.h"
#include "packet.h"
#include "settings.h"
#include "task_queue.h"

// UART RX is connected to the *BMS* White line
// UART TX is connected to the *MB* White line
// TX_INPUT_PIN must be soldered to the UART TX
#define TX_INPUT_PIN 4
// Connected to the MB B line
#define TX_INVERSE_OUT_PIN 5

namespace {

// Emulate the RS485 B line by bitbanging the inverse
// of the TX A line.
void IRAM_ATTR txPinRiseInterrupt() { digitalWrite(TX_INVERSE_OUT_PIN, 0); }
void IRAM_ATTR txPinFallInterrupt() { digitalWrite(TX_INVERSE_OUT_PIN, 1); }

#ifdef NO_GLOBAL_INSTANCES
HardwareSerial Serial(0);
#endif
}  // namespace

BmsRelay *relay;

void bms_setup() {
  relay = new BmsRelay([]() { return Serial.read(); },
                       [](uint8_t b) {
                         // This if statement is what implements locking.
                         if (!Settings->is_locked) {
                           Serial.write(b);
                         }
                       },
                       millis);
  Serial.begin(115200);

  // The B line idle is 0
  digitalWrite(TX_INVERSE_OUT_PIN, 0);
  pinMode(TX_INVERSE_OUT_PIN, OUTPUT);

  pinMode(TX_INPUT_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(TX_INPUT_PIN), txPinRiseInterrupt,
                  RISING);
  attachInterrupt(digitalPinToInterrupt(TX_INPUT_PIN), txPinFallInterrupt,
                  FALLING);

  relay->addReceivedPacketCallback([](BmsRelay *, Packet *packet) {
    static uint8_t ledState = 0;
    digitalWrite(LED_BUILTIN, ledState);
    ledState = 1 - ledState;
    streamBMSPacket(packet->start(), packet->len());
  });
  relay->setUnknownDataCallback([](uint8_t b) {
    static std::vector<uint8_t> unknownData = {0};
    if (unknownData.size() > 128) {
      return;
    }
    unknownData.push_back(b);
    streamBMSPacket(&unknownData[0], unknownData.size());
  });

  if (Settings->has_battery_state) {
    FuelGaugeState gaugeState;
    gaugeState.bottomMilliampSeconds =
        Settings->battery_state.bottom_milliamp_seconds;
    gaugeState.currentMilliampSeconds =
        Settings->battery_state.current_milliamp_seconds;
    gaugeState.bottomSoc = Settings->battery_state.bottom_soc;
    gaugeState.topSoc = Settings->battery_state.top_soc;
    relay->getBatteryFuelGauge().restoreState(gaugeState);
  }

  // The BMS sends a header-less fault code ("BU" for the power button, ...)
  // right before it powers off. It then drains its TX buffer and waits about
  // a second before releasing its power latch, but it also drops a
  // peripheral-enable output (RC0) earlier in that sequence, so how long
  // Owie's 5V survives is to be confirmed on a board. Saving settings takes
  // tens of milliseconds, so it is done immediately.
  relay->setShutdownCallback([](const char *) {
    Settings->graceful_shutdown_count++;
    const FuelGaugeState &gaugeState = relay->getBatteryFuelGauge().getState();
    Settings->has_battery_state = true;
    Settings->battery_state.bottom_milliamp_seconds =
        gaugeState.bottomMilliampSeconds;
    Settings->battery_state.current_milliamp_seconds =
        gaugeState.currentMilliampSeconds;
    Settings->battery_state.bottom_soc = gaugeState.bottomSoc;
    Settings->battery_state.top_soc = gaugeState.topSoc;
    saveSettings();
  });

  // Accepted by controllers regardless of the real BMS serial (see the
  // README). The same value has been seen programmed as the user ID of a
  // stock late-model XR BMS, so it looks like a factory default rather than
  // a random pick.
  relay->setBMSSerialOverride(0xFFABCDEF);

  // "Stock battery mode" switches from the Settings page (default off).
  relay->setPassThroughSoc(false);  // Owie's own percentage is always used
  relay->setPassThroughCurrent(Settings->pass_through_current);
  relay->setPassThroughStatus(Settings->pass_through_status);

  setupWifi();
  setupWebServer(relay);
  TaskQueue.postRecurringTask([]() { relay->loop(); });
}
