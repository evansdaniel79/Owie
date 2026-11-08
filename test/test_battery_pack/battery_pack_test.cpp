#include "battery_pack.h"

#include <unity.h>

using namespace battery_pack;

void setUp(void) {}
void tearDown(void) {}

void test15S2PSamsung30Q() {
  TEST_ASSERT_EQUAL(6000, ratedMah(CELL_SAMSUNG_30Q, 15, 2, 0));
  // 6 Ah * 15 cells * 3.6 V = 324 Wh
  TEST_ASSERT_EQUAL(324, ratedWh(CELL_SAMSUNG_30Q, 15, 2, 0));
  TEST_ASSERT_EQUAL_STRING("Samsung 30Q", cellName(CELL_SAMSUNG_30Q));
}

void testAllPresetsRated3000() {
  TEST_ASSERT_EQUAL(3000, cellRatedMah(CELL_MOLICEL_P30B, 0));
  TEST_ASSERT_EQUAL(3000, cellRatedMah(CELL_SONY_VTC6, 0));
}

void testCustomCell() {
  TEST_ASSERT_EQUAL(9000, ratedMah(CELL_CUSTOM, 15, 3, 3000));
  TEST_ASSERT_EQUAL(0, ratedMah(CELL_CUSTOM, 15, 3, 0));
}

void testNotSetHasNoRatedFigures() {
  TEST_ASSERT_FALSE(isConfigured(CELL_NOT_SET, 15, 2, 0));
  TEST_ASSERT_EQUAL(0, ratedMah(CELL_NOT_SET, 15, 2, 0));
  TEST_ASSERT_EQUAL(0, ratedWh(CELL_NOT_SET, 15, 2, 0));
}

void testValidation() {
  TEST_ASSERT_TRUE(validSettings(CELL_NOT_SET, 0, 0, 0));
  TEST_ASSERT_TRUE(validSettings(CELL_SAMSUNG_30Q, 15, 2, 0));
  TEST_ASSERT_FALSE(validSettings(CELL_SAMSUNG_30Q, 0, 2, 0));
  TEST_ASSERT_FALSE(validSettings(CELL_SAMSUNG_30Q, 15, 21, 0));
  TEST_ASSERT_FALSE(validSettings(CELL_SAMSUNG_30Q, 31, 2, 0));
  TEST_ASSERT_FALSE(validSettings(99, 15, 2, 0));
  TEST_ASSERT_FALSE(validSettings(CELL_CUSTOM, 15, 2, 100));
  TEST_ASSERT_TRUE(validSettings(CELL_CUSTOM, 15, 2, 2500));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test15S2PSamsung30Q);
  RUN_TEST(testAllPresetsRated3000);
  RUN_TEST(testCustomCell);
  RUN_TEST(testNotSetHasNoRatedFigures);
  RUN_TEST(testValidation);
  return UNITY_END();
}
