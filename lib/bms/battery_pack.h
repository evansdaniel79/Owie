#ifndef BATTERY_PACK_H
#define BATTERY_PACK_H

#include <stdint.h>

// Pack description chosen by the user in Settings. Everything here is derived
// from the cell type and the S/P layout; nothing is board specific, so it
// works the same on a Pint, Pint X, XR or an upgraded pack. A pack that is not
// set up (cell type 0) simply has no rated figures.
namespace battery_pack {

enum CellType : uint32_t {
  CELL_NOT_SET = 0,
  CELL_SAMSUNG_30Q = 1,
  CELL_MOLICEL_P30B = 2,
  CELL_SONY_VTC6 = 3,
  CELL_CUSTOM = 4,
};

constexpr uint32_t MAX_SERIES = 30;
constexpr uint32_t MAX_PARALLEL = 20;
constexpr uint32_t MIN_CUSTOM_CELL_MAH = 500;
constexpr uint32_t MAX_CUSTOM_CELL_MAH = 10000;
// Nominal cell voltage used for the energy figure (all supported cells are
// 3.6 V nominal Li-ion; custom cells are assumed the same).
constexpr float NOMINAL_CELL_VOLTS = 3.6f;

inline const char* cellName(uint32_t type) {
  switch (type) {
    case CELL_SAMSUNG_30Q: return "Samsung 30Q";
    case CELL_MOLICEL_P30B: return "Molicel P30B";
    case CELL_SONY_VTC6: return "Sony VTC6";
    case CELL_CUSTOM: return "Custom cell";
    default: return "";
  }
}

// Rated capacity of one cell in mAh, 0 if unknown.
inline uint32_t cellRatedMah(uint32_t type, uint32_t customMah) {
  switch (type) {
    case CELL_SAMSUNG_30Q:
    case CELL_MOLICEL_P30B:
    case CELL_SONY_VTC6:
      return 3000;
    case CELL_CUSTOM:
      return customMah;
    default:
      return 0;
  }
}

inline bool isConfigured(uint32_t type, uint32_t series, uint32_t parallel,
                         uint32_t customMah) {
  return cellRatedMah(type, customMah) > 0 && series >= 1 && parallel >= 1;
}

// Rated pack capacity in mAh (cell capacity x parallel cells), 0 if not set up.
inline uint32_t ratedMah(uint32_t type, uint32_t series, uint32_t parallel,
                         uint32_t customMah) {
  return isConfigured(type, series, parallel, customMah)
             ? cellRatedMah(type, customMah) * parallel
             : 0;
}

// Rated pack energy in Wh, 0 if not set up.
inline uint32_t ratedWh(uint32_t type, uint32_t series, uint32_t parallel,
                        uint32_t customMah) {
  return uint32_t(ratedMah(type, series, parallel, customMah) * series *
                      NOMINAL_CELL_VOLTS / 1000.0f +
                  0.5f);
}

inline bool validSettings(uint32_t type, uint32_t series, uint32_t parallel,
                          uint32_t customMah) {
  if (type == CELL_NOT_SET) return true;
  if (type > CELL_CUSTOM) return false;
  if (series < 1 || series > MAX_SERIES) return false;
  if (parallel < 1 || parallel > MAX_PARALLEL) return false;
  if (type == CELL_CUSTOM &&
      (customMah < MIN_CUSTOM_CELL_MAH || customMah > MAX_CUSTOM_CELL_MAH)) {
    return false;
  }
  return true;
}

}  // namespace battery_pack

#endif  // BATTERY_PACK_H
