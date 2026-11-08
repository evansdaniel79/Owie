#include "ride_logger.h"

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void testNotActiveWithoutMemory() {
  RideLogger log;
  TEST_ASSERT_FALSE(log.active());
  log.observe(0, 1, 1, 1, 1, 1, 1, 1, 1);
  TEST_ASSERT_EQUAL(0, log.size());
  TEST_ASSERT_FALSE(log.begin(0));
}

void testAveragesAndPeakOverWindow() {
  RideLogger log;
  TEST_ASSERT_TRUE(log.begin(10));
  log.observe(0, 58000, 1000, 3850, 3870, 80, 85, 82, 25);
  log.observe(2000, 57900, 3000, 3845, 3868, 80, 85, 82, 26);
  TEST_ASSERT_EQUAL(0, log.size());
  log.observe(5000, 57800, -2000, 3840, 3866, 79, 84, 81, 27);
  TEST_ASSERT_EQUAL(1, log.size());
  const RideLogger::Sample& s = log.at(0);
  TEST_ASSERT_EQUAL(57800, s.total_mv);
  TEST_ASSERT_EQUAL(666, s.current_ma_avg);  // (1000+3000-2000)/3
  TEST_ASSERT_EQUAL(3000, s.current_ma_peak);
  TEST_ASSERT_EQUAL(3840, s.min_cell_mv);
  TEST_ASSERT_EQUAL(79, s.soc);
  TEST_ASSERT_EQUAL(27, s.temp_max_c);
  TEST_ASSERT_EQUAL(5, log.secondsAt(0));
}

void testRingKeepsNewestAndTimesStayCorrect() {
  RideLogger log;
  TEST_ASSERT_TRUE(log.begin(3));
  for (uint32_t i = 0; i <= 5; i++) {
    log.observe(i * 5000, 50000 + i, 0, 0, 0, 0, 0, 0, 0);
  }
  // Windows closed at 5,10,15,20,25 s -> 5 samples written, 3 kept.
  TEST_ASSERT_EQUAL(3, log.size());
  TEST_ASSERT_EQUAL(50003, log.at(0).total_mv);
  TEST_ASSERT_EQUAL(50005, log.at(2).total_mv);
  TEST_ASSERT_EQUAL(15, log.secondsAt(0));
  TEST_ASSERT_EQUAL(25, log.secondsAt(2));
}

void testClear() {
  RideLogger log;
  TEST_ASSERT_TRUE(log.begin(4));
  log.observe(0, 1, 1, 1, 1, 1, 1, 1, 1);
  log.observe(5000, 1, 1, 1, 1, 1, 1, 1, 1);
  TEST_ASSERT_EQUAL(1, log.size());
  log.clear();
  TEST_ASSERT_EQUAL(0, log.size());
  TEST_ASSERT_TRUE(log.active());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(testNotActiveWithoutMemory);
  RUN_TEST(testAveragesAndPeakOverWindow);
  RUN_TEST(testRingKeepsNewestAndTimesStayCorrect);
  RUN_TEST(testClear);
  return UNITY_END();
}
