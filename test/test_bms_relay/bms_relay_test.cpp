#include "bms_relay.h"

#include <unity.h>

#include <cstdio>
#include <deque>
#include <memory>
#include <string>

#include "packet.h"

std::unique_ptr<BmsRelay> relay;
std::deque<int> mockBmsData;
std::vector<uint8_t> mockDataOut;
unsigned long timeMillis = 0;

void setUp(void) {
  relay.reset(new BmsRelay(
      [&]() {
        if (mockBmsData.empty()) {
          return -1;
        }
        int data = mockBmsData.front();
        mockBmsData.pop_front();
        return data;
      },
      [&](uint8_t b) {
        mockDataOut.push_back(b);
        return 1;
      },
      [&]() { return timeMillis; }));
  mockBmsData.clear();
  mockDataOut.clear();
}

void addMockData(const std::vector<uint8_t>& data) {
  mockBmsData.insert(mockBmsData.end(), data.begin(), data.end());
}

void expectDataOut(const std::vector<uint8_t>& expected) {
  TEST_ASSERT_EQUAL(expected.size(), mockDataOut.size());
  if (expected.size() == 0) {
    return;
  }
  TEST_ASSERT_EQUAL_UINT8_ARRAY(&expected[0], &mockDataOut[0], expected.size());
  mockDataOut.clear();
}

void testUnknownBytesGetsForwardedImmediately(void) {
  addMockData({0x1, 0x2, 0x3});
  relay->loop();
  TEST_ASSERT_TRUE(mockBmsData.empty());
  expectDataOut({0x1, 0x2, 0x3});
}

void testUnknownDataAfterKnownPacketGetsFlushedImmediately(void) {
  addMockData({0xFF, 0x55, 0xAA, 0x6, 0x1, 0x2, 0x3, 0x4, 0x2, 0xE, 0x1});
  relay->loop();
  TEST_ASSERT_TRUE(mockBmsData.empty());
  expectDataOut({0xFF, 0x55, 0xAA, 0x6, 0x1, 0x2, 0x3, 0x4, 0x2, 0xE, 0x1});
}

void testSerialGetsRecordedAndIntercepted(void) {
  addMockData(
      {0x1, 0x2, 0x3, 0xFF, 0x55, 0xAA, 0x6, 0x1, 0x2, 0x3, 0x4, 0x2, 0xE});
  relay->setBMSSerialOverride(0x8040201);
  relay->loop();
  TEST_ASSERT_TRUE(mockBmsData.empty());
  TEST_ASSERT_EQUAL(0x1020304, relay->getCapturedBMSSerial());
  // Expect no data so far because we identify packets only when we see the
  // header of the next one.
  expectDataOut(
      {0x1, 0x2, 0x3, 0xFF, 0x55, 0xAA, 0x6, 0x8, 0x4, 0x2, 0x1, 0x2, 0x13});
}

void testPacketLengths() {
  std::vector<std::vector<uint8_t>> packets = {
      {0xff, 0x55, 0xaa, 0x00, 0x80, 0x02, 0x7e},
      {0xff, 0x55, 0xaa, 0x01, 0x1c, 0x02, 0x02, 0x1d},
      {0xff, 0x55, 0xaa, 0x02, 0x0f, 0x28, 0x0f, 0x2c, 0x0f, 0x2b,
       0x0f, 0x29, 0x0f, 0x2a, 0x0f, 0x2b, 0x0f, 0x2a, 0x0f, 0x2c,
       0x0f, 0x29, 0x0f, 0x2b, 0x0f, 0x29, 0x0f, 0x2a, 0x0f, 0x22,
       0x0f, 0x2a, 0x0f, 0x2a, 0x00, 0x2a, 0x05, 0x7b},
      {0xff, 0x55, 0xaa, 0x03, 0x29, 0x02, 0x2a},
      {0xff, 0x55, 0xaa, 0x04, 0x16, 0x17, 0x17, 0x17, 0x18, 0x02, 0x75},
      {0xff, 0x55, 0xaa, 0x05, 0x00, 0x01, 0x02, 0x04},
      {0xFF, 0x55, 0xAA, 0x6, 0x8, 0x4, 0x2, 0x1, 0x2, 0x13},
      {0xff, 0x55, 0xaa, 0x07, 0x10, 0xcc, 0x10, 0x57, 0x09, 0xc4, 0x50, 0x04,
       0x65},
      {0xff, 0x55, 0xaa, 0x08, 0x06, 0x02, 0x0c},
      {0xff, 0x55, 0xaa, 0x09, 0x03, 0x02, 0x0a},
      {0xff, 0x55, 0xaa, 0x0b, 0x0b, 0xc0, 0x02, 0xd4},
      {0xff, 0x55, 0xaa, 0x0c, 0x00, 0x00, 0x02, 0x0a},
      {0xff, 0x55, 0xaa, 0x0d, 0x02, 0xda, 0x47, 0x03, 0x2e},
      {0xff, 0x55, 0xaa, 0x0e, 0x01, 0x02, 0x0d},
      {0xff, 0x55, 0xaa, 0x0f, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0f},
      {0xff, 0x55, 0xaa, 0x10, 0x03, 0x03, 0x0b, 0x03, 0x03, 0x03, 0x03, 0x03,
       0x03, 0x03, 0x02, 0x34},
      {0xff, 0x55, 0xaa, 0x11, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0f}};
  std::vector<uint8_t> receivedPacket;
  relay->addReceivedPacketCallback([&](BmsRelay*, Packet* p) {
    TEST_ASSERT_TRUE(p->isValid());
    receivedPacket = std::vector<uint8_t>(p->start(), p->start() + p->len());
  });
  for (const auto& packet : packets) {
    addMockData(packet);
    relay->loop();
    TEST_ASSERT_EQUAL(packet.size(), receivedPacket.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&packet[0], &receivedPacket[0],
                                  packet.size());
  }
}

void testPacketCallback() {
  addMockData({0x1, 0x2, 0x3, 0xFF, 0x55, 0xAA, 0x6, 0x1, 0x2, 0x3, 0x4, 0x2,
               0xE, 0xFF, 0x55, 0xAA});
  std::vector<uint8_t> receivedPacket;
  relay->addReceivedPacketCallback([&](BmsRelay*, Packet* p) {
    const uint8_t* start = p->start();
    receivedPacket.assign(start, start + p->len());
  });
  relay->loop();
  std::vector<uint8_t> expected(
      {0xFF, 0x55, 0xAA, 0x6, 0x1, 0x2, 0x3, 0x4, 0x2, 0xE});
  TEST_ASSERT_EQUAL(expected.size(), receivedPacket.size());
  for (int i = 0; i < expected.size(); i++) {
    TEST_ASSERT_EQUAL(expected[i], receivedPacket[i]);
  }
}

void testBatterySocParsing() {
  addMockData({0xFF, 0x55, 0xAA, 0x3, 0x2B, 0x02, 0x2C, 0xFF, 0x55, 0xAA});
  relay->loop();
  TEST_ASSERT_EQUAL(43, relay->getBmsReportedSOC());
}

void testCurrentParsing() {
  addMockData({0xff, 0x55, 0xaa, 0x5, 0xff, 0xe8, 0x3, 0xea, 0xFF, 0x55, 0xAA});
  relay->loop();
  TEST_ASSERT_EQUAL(-1320, relay->getCurrentMilliamps());
}

void testCellVoltageParsing() {
  addMockData({0xff, 0x55, 0xaa, 0x02, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x14, 0x0f,
               0x13, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x13, 0x0f, 0x14,
               0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x14, 0x0f,
               0x14, 0x00, 0x2a, 0x04, 0x31, 0xFF, 0x55, 0xAA});
  relay->loop();
  uint16_t expected[15] = {3860, 3860, 3860, 3859, 3860, 3860, 3860, 3859,
                           3860, 3859, 3859, 3859, 3859, 3860, 3860};
  TEST_ASSERT_EQUAL_INT16_ARRAY(expected, relay->getCellMillivolts(), 15);
  TEST_ASSERT_EQUAL(57894, relay->getTotalVoltageMillivolts());
}

void testTemperatureParsing() {
  // (The checksum of this frame was wrong before and the test was never run.)
  addMockData({0xff, 0x55, 0xaa, 0x04, 0x13, 0x14, 0x14, 0x14, 0x16, 0x02, 0x67,
               0xFF, 0x55, 0xAA});
  relay->loop();
  uint8_t expected[5] = {19, 20, 20, 20, 22};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, relay->getTemperaturesCelsius(), 5);
  // Average over the four pack sensors only (79 / 4), not the board sensor.
  TEST_ASSERT_EQUAL(19, relay->getAverageTemperatureCelsius());
}

void testTemperatureParsingInvalidSensorAndHighValues() {
  // Sensor 1 out of range (0xFF), sensor 3 at 130 C (would be negative as
  // int8), board sensor 0xFF.
  addMockData({0xff, 0x55, 0xaa, 0x04, 0x13, 0xff, 0x14, 0x82, 0xff, 0x04, 0xa9,
               0xFF, 0x55, 0xAA});
  relay->loop();
  uint8_t expected[5] = {19, 0xff, 20, 130, 0xff};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, relay->getTemperaturesCelsius(), 5);
  TEST_ASSERT_EQUAL(BmsRelay::TEMPERATURE_INVALID,
                    relay->getTemperaturesCelsius()[1]);
  // (19 + 20 + 130) / 3 = 56
  TEST_ASSERT_EQUAL(56, relay->getAverageTemperatureCelsius());
}

void testBlocksStatusPacketsUnlessWarning() {
  addMockData({0xff, 0x55, 0xaa, 0x0, 0x0, 0x1, 0xFE, 0x1});
  relay->loop();
  expectDataOut({0x1});
  // Charging bit is present, expect all data to go through.
  addMockData({0xff, 0x55, 0xaa, 0x0, 0x21, 0x2, 0x1F, 0x1});
  relay->loop();
  expectDataOut({0xff, 0x55, 0xaa, 0x0, 0x21, 0x2, 0x1F, 0x1});
}

// Packets used by the "stock battery mode" pass-through tests.
const std::vector<uint8_t> kSocPacket = {0xff, 0x55, 0xaa, 0x3, 0x2b, 0x2, 0x2c};
const std::vector<uint8_t> kCurrentPacket = {0xff, 0x55, 0xaa, 0x5,
                                             0xff, 0xe8, 0x3,  0xea};
const std::vector<uint8_t> kStatusPacket = {0xff, 0x55, 0xaa, 0x0,
                                            0x0,  0x1,  0xfe};

void testDefaultsOverrideSocAndBlockCurrentAndStatus() {
  timeMillis = 0;
  // Default (original Owie) behavior: SOC is rewritten, current and status
  // packets are swallowed while not charging.
  addMockData(kSocPacket);
  relay->loop();
  // Fuel gauge has no data yet, so the SOC gets rewritten to 0 (+ new CRC).
  expectDataOut({0xff, 0x55, 0xaa, 0x3, 0x0, 0x2, 0x1});
  TEST_ASSERT_EQUAL(43, relay->getBmsReportedSOC());

  addMockData(kCurrentPacket);
  relay->loop();
  expectDataOut({});

  addMockData(kStatusPacket);
  relay->loop();
  expectDataOut({});
}

void testPassThroughSocForwardsBmsValueUntouched() {
  timeMillis = 0;
  relay->setPassThroughSoc(true);
  addMockData(kSocPacket);
  relay->loop();
  expectDataOut(kSocPacket);
  TEST_ASSERT_EQUAL(43, relay->getBmsReportedSOC());
}

void testPassThroughCurrentForwardsCurrentPacket() {
  timeMillis = 0;
  relay->setPassThroughCurrent(true);
  addMockData(kCurrentPacket);
  relay->loop();
  expectDataOut(kCurrentPacket);
  // Owie's own stats keep working.
  TEST_ASSERT_EQUAL(-1320, relay->getCurrentMilliamps());
}

void testPassThroughStatusForwardsStatusPacket() {
  timeMillis = 0;
  relay->setPassThroughStatus(true);
  addMockData(kStatusPacket);
  relay->loop();
  expectDataOut(kStatusPacket);
}

void testPassThroughFlagsAreIndependent() {
  timeMillis = 0;
  // Only SOC pass-through on: current and status must still be blocked.
  relay->setPassThroughSoc(true);
  addMockData(kCurrentPacket);
  relay->loop();
  expectDataOut({});
  addMockData(kStatusPacket);
  relay->loop();
  expectDataOut({});

  // Only current pass-through on: SOC must still be rewritten.
  setUp();
  relay->setPassThroughCurrent(true);
  addMockData(kSocPacket);
  relay->loop();
  expectDataOut({0xff, 0x55, 0xaa, 0x3, 0x0, 0x2, 0x1});
}

void testPacketReplay() {
  addMockData({0xff, 0x55, 0xaa, 0x0, 0x21, 0x2, 0x1F, 0x1});
  addMockData({0xff, 0x55, 0xaa, 0x08, 0x06, 0x02, 0x0c});
  timeMillis = 0;
  relay->loop();
  expectDataOut({0xff, 0x55, 0xaa, 0x0, 0x21, 0x2, 0x1F, 0x1, 0xff, 0x55, 0xaa,
                 0x08, 0x06, 0x02, 0x0c});
  // Under the status packet timeout limit
  timeMillis = 499;
  relay->loop();
  expectDataOut({});
  // Over the timeout, trigger replay
  timeMillis = 501;
  relay->loop();

  // Make sure that replayed packet reset the stats so we don't
  // end up spamming the controller
  expectDataOut({0xff, 0x55, 0xaa, 0x0, 0x21, 0x2, 0x1F});
  timeMillis = 601;
  relay->loop();
  expectDataOut({});

  // Rewind time to far future to trigger both slow and fast packet
  // replay.
  timeMillis = 10000;
  relay->loop();
  expectDataOut({0xff, 0x55, 0xaa, 0x0, 0x21, 0x2, 0x1F, 0xff, 0x55, 0xaa, 0x08,
                 0x06, 0x02, 0x0c});
}

void testStatusSocBucketFollowsOverriddenSoc() {
  // Cell voltages (min 3859 mV -> voltage based SOC 61 %), then the BMS's
  // own 43 % in message 0x3, then a status byte with bucket 11 (<= 20 %),
  // charging and "hot" bits set.
  addMockData({0xff, 0x55, 0xaa, 0x02, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x14, 0x0f,
               0x13, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x13, 0x0f, 0x14,
               0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x14, 0x0f,
               0x14, 0x00, 0x2a, 0x04, 0x31});
  addMockData({0xFF, 0x55, 0xAA, 0x03, 0x2B, 0x02, 0x2C});
  addMockData({0xff, 0x55, 0xaa, 0x00, 0xE1, 0x02, 0xDF});
  relay->loop();
  TEST_ASSERT_EQUAL(61, relay->getOverriddenSOC());
  // The BMS's view of the status byte is kept for the flag getters.
  TEST_ASSERT_TRUE(relay->isCharging());
  TEST_ASSERT_TRUE(relay->isBatteryTempOutOfRange());
  // Forwarded: cells unchanged, SOC rewritten to 61 (0x3D), status bucket
  // rewritten from 11 to 01 (50..80 %) with the other bits intact.
  expectDataOut({0xff, 0x55, 0xaa, 0x02, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x14, 0x0f,
                 0x13, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x13, 0x0f, 0x14,
                 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x14, 0x0f,
                 0x14, 0x00, 0x2a, 0x04, 0x31,
                 0xFF, 0x55, 0xAA, 0x03, 0x3D, 0x02, 0x3E,
                 0xff, 0x55, 0xaa, 0x00, 0x61, 0x02, 0x5F});
}

void testStockModeLeavesStatusSocBucketAlone() {
  // Same traffic as testStatusSocBucketFollowsOverriddenSoc, but with stock
  // battery mode on: nothing may be rewritten, the board sees exactly what the
  // BMS sent (SOC 43 % = 0x2B, status byte 0xE1 with bucket 11 untouched).
  relay->setPassThroughSoc(true);
  relay->setPassThroughStatus(true);
  const std::vector<uint8_t> cells = {
      0xff, 0x55, 0xaa, 0x02, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x14, 0x0f,
      0x13, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x14, 0x0f, 0x13, 0x0f, 0x14,
      0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x13, 0x0f, 0x14, 0x0f,
      0x14, 0x00, 0x2a, 0x04, 0x31};
  const std::vector<uint8_t> soc = {0xFF, 0x55, 0xAA, 0x03, 0x2B, 0x02, 0x2C};
  const std::vector<uint8_t> status = {0xff, 0x55, 0xaa, 0x00,
                                       0xE1, 0x02, 0xDF};
  addMockData(cells);
  addMockData(soc);
  addMockData(status);
  relay->loop();
  std::vector<uint8_t> expected;
  expected.insert(expected.end(), cells.begin(), cells.end());
  expected.insert(expected.end(), soc.begin(), soc.end());
  expected.insert(expected.end(), status.begin(), status.end());
  expectDataOut(expected);
}

void testBmsInfoParsing() {
  TEST_ASSERT_FALSE(relay->hasCycleInfo());
  TEST_ASSERT_EQUAL(0, relay->getBmsFirmwareVersion());
  // Frames captured from a Pint: thresholds, variant, last fault, version,
  // balancing mask, cycles + health.
  addMockData({0xff, 0x55, 0xaa, 0x07, 0x10, 0xcc, 0x10, 0x57, 0x09, 0xc4, 0x50,
               0x04, 0x65});
  addMockData({0xff, 0x55, 0xaa, 0x08, 0x06, 0x02, 0x0c});
  addMockData({0xff, 0x55, 0xaa, 0x09, 0x03, 0x02, 0x0a});
  addMockData({0xff, 0x55, 0xaa, 0x0b, 0x0b, 0xc0, 0x02, 0xd4});
  addMockData({0xff, 0x55, 0xaa, 0x0c, 0x00, 0x00, 0x02, 0x0a});
  addMockData({0xff, 0x55, 0xaa, 0x0d, 0x02, 0xda, 0x47, 0x03, 0x2e});
  relay->loop();
  TEST_ASSERT_EQUAL(4300, relay->getOverVoltageThresholdMillivolts());
  TEST_ASSERT_EQUAL(4183, relay->getChargeCompleteThresholdMillivolts());
  TEST_ASSERT_EQUAL(2500, relay->getUnderVoltageThresholdMillivolts());
  TEST_ASSERT_EQUAL(80, relay->getOverTemperatureThresholdCelsius());
  TEST_ASSERT_EQUAL(0x06, relay->getBmsVariant());
  TEST_ASSERT_EQUAL(3, relay->getLastShutdownReasonCode());
  TEST_ASSERT_EQUAL_STRING("BU - power button",
                           BmsRelay::shutdownReasonName(3));
  TEST_ASSERT_EQUAL_STRING("none", BmsRelay::shutdownReasonName(0));
  TEST_ASSERT_EQUAL_STRING("CC - charge over-current",
                           BmsRelay::shutdownReasonName(13));
  TEST_ASSERT_EQUAL_STRING("unknown", BmsRelay::shutdownReasonName(14));
  TEST_ASSERT_EQUAL(3008, relay->getBmsFirmwareVersion());
  TEST_ASSERT_EQUAL(0, relay->getBalancingMask());
  TEST_ASSERT_TRUE(relay->hasCycleInfo());
  TEST_ASSERT_EQUAL(730, relay->getCycleCount());
  TEST_ASSERT_EQUAL(71, relay->getStateOfHealthPercent());
}

void testShutdownWarningParsedAndNeverReplayed() {
  timeMillis = 1000;
  // 28 s left, reason 2 = under-voltage.
  addMockData({0xff, 0x55, 0xaa, 0x01, 0x1c, 0x02, 0x02, 0x1d});
  relay->loop();
  expectDataOut({0xff, 0x55, 0xaa, 0x01, 0x1c, 0x02, 0x02, 0x1d});
  TEST_ASSERT_EQUAL(28, relay->getShutdownCountdownSeconds());
  TEST_ASSERT_EQUAL(2, relay->getShutdownReasonMask());
  // Still valid just under 2 s later, gone after that.
  timeMillis = 2999;
  relay->loop();
  TEST_ASSERT_EQUAL(28, relay->getShutdownCountdownSeconds());
  timeMillis = 3001;
  relay->loop();
  TEST_ASSERT_EQUAL(-1, relay->getShutdownCountdownSeconds());
  // Far in the future: no replay of the warning, unlike regular packets.
  timeMillis = 100000;
  relay->loop();
  expectDataOut({});
}

void testShutdownCodeTriggersCallbackOnce() {
  std::string code;
  int fires = 0;
  relay->setShutdownCallback([&](const char* c) {
    code = c;
    fires++;
  });
  const std::vector<uint8_t> validPacket = {0xff, 0x55, 0xaa, 0x08,
                                            0x06, 0x02, 0x0c};
  // Garbage before the first valid packet is ignored.
  addMockData({'B', 'U'});
  relay->loop();
  TEST_ASSERT_EQUAL(0, fires);
  TEST_ASSERT_EQUAL_STRING("", relay->getShutdownCode());

  // Power button: "BU" right after a frame, followed by the 10-byte record
  // that 4210/Pint boards append. Exactly one callback.
  addMockData(validPacket);
  relay->loop();
  addMockData({'B', 'U'});
  relay->loop();
  TEST_ASSERT_EQUAL(1, fires);
  TEST_ASSERT_EQUAL_STRING("BU", code.c_str());
  TEST_ASSERT_EQUAL_STRING("BU", relay->getShutdownCode());
  addMockData({0x07, 0xd7, 0xff, 0xab, 0xcd, 0xef, 0x03, 0x00, 0x10, 0x62});
  relay->loop();
  TEST_ASSERT_EQUAL(1, fires);

  // Three-letter code.
  addMockData(validPacket);
  relay->loop();
  addMockData({'C', 'U', 'V'});
  relay->loop();
  TEST_ASSERT_EQUAL(2, fires);
  TEST_ASSERT_EQUAL_STRING("CUV", code.c_str());

  // "PR" carries three binary bytes after the text; still one callback.
  addMockData(validPacket);
  relay->loop();
  addMockData({'P', 'R', 0x50, 0x08, 0x00});
  relay->loop();
  TEST_ASSERT_EQUAL(3, fires);
  TEST_ASSERT_EQUAL_STRING("PR", code.c_str());

  // A run that does not start with a code never fires, even if a code
  // appears later in it.
  addMockData(validPacket);
  relay->loop();
  addMockData({'Z', 'Z', 'B', 'U'});
  relay->loop();
  TEST_ASSERT_EQUAL(3, fires);

  // Unknown bytes are still forwarded unchanged.
  TEST_ASSERT_TRUE(mockDataOut.size() > 0);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(testUnknownDataAfterKnownPacketGetsFlushedImmediately);
  RUN_TEST(testPacketLengths);
  RUN_TEST(testSerialGetsRecordedAndIntercepted);
  RUN_TEST(testUnknownBytesGetsForwardedImmediately);
  RUN_TEST(testPacketCallback);
  RUN_TEST(testBatterySocParsing);
  RUN_TEST(testCurrentParsing);
  RUN_TEST(testCellVoltageParsing);
  RUN_TEST(testTemperatureParsing);
  RUN_TEST(testTemperatureParsingInvalidSensorAndHighValues);
  RUN_TEST(testBlocksStatusPacketsUnlessWarning);
  RUN_TEST(testDefaultsOverrideSocAndBlockCurrentAndStatus);
  RUN_TEST(testPassThroughSocForwardsBmsValueUntouched);
  RUN_TEST(testPassThroughCurrentForwardsCurrentPacket);
  RUN_TEST(testPassThroughStatusForwardsStatusPacket);
  RUN_TEST(testPassThroughFlagsAreIndependent);
  RUN_TEST(testPacketReplay);
  RUN_TEST(testStatusSocBucketFollowsOverriddenSoc);
  RUN_TEST(testStockModeLeavesStatusSocBucketAlone);
  RUN_TEST(testBmsInfoParsing);
  RUN_TEST(testShutdownWarningParsedAndNeverReplayed);
  RUN_TEST(testShutdownCodeTriggersCallbackOnce);
  UNITY_END();

  return 0;
}