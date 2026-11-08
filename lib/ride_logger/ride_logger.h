#ifndef RIDE_LOGGER_H
#define RIDE_LOGGER_H

#include <stddef.h>
#include <stdint.h>

#include <new>

// Keeps the most recent part of a ride in RAM: one compact sample every
// INTERVAL_MS (averages and peaks over that window), in a ring buffer. It runs
// on the board itself, so recording does not depend on a phone staying
// connected or its screen being on. The log is lost when the board powers off.
class RideLogger {
 public:
  static constexpr uint32_t INTERVAL_MS = 5000;

  struct Sample {
    uint16_t total_mv;
    int16_t current_ma_avg;
    int16_t current_ma_peak;  // highest (discharge) current in the window
    uint16_t min_cell_mv;
    uint16_t max_cell_mv;
    int8_t soc;
    int8_t bms_soc;
    int8_t voltage_soc;
    uint8_t temp_max_c;  // 255 = no valid pack temperature
  };

  ~RideLogger() { delete[] samples_; }

  // Allocates room for `capacity` samples; returns false (and records nothing)
  // if memory is not available.
  bool begin(size_t capacity) {
    delete[] samples_;
    samples_ = nullptr;
    capacity_ = 0;
    clear();
    if (capacity == 0) return false;
    samples_ = new (std::nothrow) Sample[capacity];
    if (samples_ == nullptr) return false;
    capacity_ = capacity;
    return true;
  }

  size_t capacity() const { return capacity_; }
  size_t size() const { return total_written_ < capacity_ ? total_written_ : capacity_; }
  bool active() const { return capacity_ > 0; }

  void clear() {
    total_written_ = 0;
    window_started_ = false;
    acc_count_ = 0;
  }

  // Call regularly (a few times a second is plenty) with the latest readings.
  void observe(uint32_t now_ms, uint16_t total_mv, int16_t current_ma,
               uint16_t min_cell_mv, uint16_t max_cell_mv, int8_t soc,
               int8_t bms_soc, int8_t voltage_soc, uint8_t temp_max_c) {
    if (capacity_ == 0) return;
    if (!window_started_) {
      window_started_ = true;
      window_start_ms_ = now_ms;
      if (total_written_ == 0) log_start_ms_ = now_ms;
    }
    acc_sum_ += current_ma;
    if (acc_count_ == 0 || current_ma > acc_peak_) acc_peak_ = current_ma;
    acc_count_++;

    if (now_ms - window_start_ms_ < INTERVAL_MS) return;
    Sample& s = samples_[total_written_ % capacity_];
    s.total_mv = total_mv;
    s.current_ma_avg = (int16_t)(acc_sum_ / (int32_t)acc_count_);
    s.current_ma_peak = acc_peak_;
    s.min_cell_mv = min_cell_mv;
    s.max_cell_mv = max_cell_mv;
    s.soc = soc;
    s.bms_soc = bms_soc;
    s.voltage_soc = voltage_soc;
    s.temp_max_c = temp_max_c;
    total_written_++;
    window_start_ms_ = now_ms;
    acc_sum_ = 0;
    acc_count_ = 0;
  }

  // Index 0 is the oldest sample still kept.
  const Sample& at(size_t i) const {
    return samples_[(total_written_ - size() + i) % capacity_];
  }

  // Seconds since the log started for the sample at index i (oldest = 0 until
  // the buffer wraps).
  uint32_t secondsAt(size_t i) const {
    return (uint32_t)(total_written_ - size() + i + 1) * (INTERVAL_MS / 1000);
  }

  // Uptime (ms) when the first sample's window began, for aligning with boot.
  uint32_t logStartMillis() const { return log_start_ms_; }

 private:
  Sample* samples_ = nullptr;
  size_t capacity_ = 0;
  size_t total_written_ = 0;
  bool window_started_ = false;
  uint32_t window_start_ms_ = 0;
  uint32_t log_start_ms_ = 0;
  int32_t acc_sum_ = 0;
  uint32_t acc_count_ = 0;
  int16_t acc_peak_ = 0;
};

#endif  // RIDE_LOGGER_H
