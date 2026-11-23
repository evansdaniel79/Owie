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
  // No cell voltages seen yet: like lolwheel v1.4.3 the SOC packet is swallowed.
  expectDataOut({});
  TEST_ASSERT_EQUAL(43, relay->getBmsReportedSOC());

  // Every cell at 3860 mV -> lookup table gives 61% (0x3d).
  {
    // 15 cells at 3860 mV (0x0f14), the extra word, checksum, next preamble.
    std::vector<uint8_t> cells = {0xff, 0x55, 0xaa, 0x02};
    for (int c = 0; c < 15; c++) {
      cells.push_back(0x0f);
      cells.push_back(0x14);
    }
    cells.push_back(0x00);
    cells.push_back(0x2a);
    uint16_t sum = 0;
    for (uint8_t b : cells) sum += b;
    cells.push_back(sum >> 8);
    cells.push_back(sum & 0xff);
    cells.insert(cells.end(), {0xff, 0x55, 0xaa});
    addMockData(cells);
  }
  relay->loop();
  mockDataOut.clear();
  // The trailing preamble above already started this SOC packet.
  addMockData({0x3, 0x2b, 0x2, 0x2c});
  relay->loop();
  expectDataOut({0xff, 0x55, 0xaa, 0x3, 0x3d, 0x2, 0x3e});
  TEST_ASSERT_EQUAL(61, relay->getOverriddenSOC());

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
  expectDataOut({});  // not forwarded untouched (swallowed: no voltage yet)
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
  RUN_TEST(testBlocksStatusPacketsUnlessWarning);
  RUN_TEST(testDefaultsOverrideSocAndBlockCurrentAndStatus);
  RUN_TEST(testPassThroughSocForwardsBmsValueUntouched);
  RUN_TEST(testPassThroughCurrentForwardsCurrentPacket);
  RUN_TEST(testPassThroughStatusForwardsStatusPacket);
  RUN_TEST(testPassThroughFlagsAreIndependent);
  RUN_TEST(testPacketReplay);
  RUN_TEST(testBmsInfoParsing);
  UNITY_END();

  return 0;
}