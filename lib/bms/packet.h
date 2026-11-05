#ifndef PACKET_H
#define PACKET_H

#include <cstdint>

// Total frame length (3 byte preamble + type + payload + 2 byte checksum) per
// message type, -1 = unknown. 0x01 is the shutdown countdown warning (seconds,
// reason), 0x0E is the Pint command acknowledgement; both are only sent in
// special situations and must never be replayed (see bms_relay.cpp).
// A bunch of places use sizeof(), don't change type.
static const int8_t PACKET_LENGTHS_BY_TYPE[] = {
    7, 8, 38, 7, 11, 8, 10, 13, 7, 7, -1, 8, 8, 9, 7, 11, 16, 10};

class Packet {
 public:
  Packet(uint8_t* start, uint8_t len) : start_(start), len_(len) { validate(); }

  int getType() const {
    if (!valid_) {
      return -1;
    }
    return start_[3];
  };

  bool isValid() const { return valid_; }

  uint8_t* data() const {
    if (!valid_) {
      return nullptr;
    }
    return start_ + 4;
  }

  int dataLength() const {
    if (!valid_) {
      return -1;
    }
    // 3 byte preamble + 1 byte type + 2 byte CRC
    return len_ - 6;
  }

  void setShouldForward(bool shouldForward) {
    this->shouldForward_ = shouldForward;
  }

  bool shouldForward() { return this->shouldForward_; }

  void recalculateCrcIfValid();

  uint8_t* const start() const { return start_; }

  uint8_t len() const { return len_; }

 private:
  void validate();
  uint8_t* start_;
  uint8_t len_;
  bool valid_ = false;
  bool shouldForward_ = true;
};

#endif  // PACKET_H