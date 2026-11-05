#include "bms_relay.h"

#include <cstring>
#include <limits>
#include <string.h>

#include "packet.h"

namespace {
const uint8_t PREAMBLE[] = {0xFF, 0x55, 0xAA};

unsigned long packetTypeRebroadcastTimeout(int type) {
  if (type == 0 || type == 5) {
    return 500;
  }
  // Never replay one-off messages: 0x0B is the boot message (sent once at
  // power up), 0x01 is the shutdown countdown warning and 0x0E a command
  // acknowledgement. Replaying a stale countdown would tell the controller
  // the BMS is about to power off when it is not.
  if (type == 1 || type == 11 || type == 14) {
    return std::numeric_limits<unsigned long>::max();
  }
  return 3000;
}

// Header-less ASCII strings the BMS sends right before it powers off. "PR"
// and "CS" are followed by a few binary bytes, "CUV" is the only 3-letter one.
const char* const SHUTDOWN_CODES[] = {"DC", "CD", "BU", "QC", "OC", "UV", "CUV",
                                      "OT", "OV", "PR", "ZC", "CS", "CC"};

}  // namespace

BmsRelay::BmsRelay(const Source& source, const Sink& sink,
                   const MillisProvider& millis)
    : source_(source), sink_(sink), millis_provider_(millis) {
  sourceBuffer_.reserve(64);
}

void BmsRelay::loop() {
  while (true) {
    int byte = source_();
    now_millis_ = millis_provider_();
    if (byte < 0) {
      maybeReplayPackets();
      return;
    }
    sourceBuffer_.push_back(byte);
    processNextByte();
  }
}

void BmsRelay::maybeReplayPackets() {
  for (const IndividualPacketStat& stat :
       packet_tracker_.getIndividualPacketStats()) {
    if (stat.total_num < 1) {
      continue;
    }
    if ((now_millis_ - stat.last_packet_millis) <
        packetTypeRebroadcastTimeout(stat.id)) {
      continue;
    }
    std::vector<uint8_t> data_copy(stat.last_seen_valid_packet);
    Packet p(data_copy.data(), data_copy.size());
    ingestPacket(p);
  }
}

void BmsRelay::purgeUnknownData() {
  for (uint8_t b : sourceBuffer_) {
    sink_(b);
  }
  if (unknownDataCallback_) {
    for (uint8_t b : sourceBuffer_) {
      unknownDataCallback_(b);
    }
  }
  packet_tracker_.unknownBytes(sourceBuffer_.size());
  for (uint8_t b : sourceBuffer_) {
    trackUnknownByte(b);
  }
  sourceBuffer_.clear();
}

void BmsRelay::trackUnknownByte(uint8_t b) {
  // The fault code is queued by the BMS right after a complete frame, so it
  // is always at the very start of an unknown-data run. Runs before the first
  // valid packet are start-up garbage and ignored.
  if (!seen_valid_packet_ || unknown_run_fired_ ||
      unknown_run_len_ >= sizeof(unknown_run_)) {
    if (unknown_run_len_ < sizeof(unknown_run_)) {
      unknown_run_len_++;
    }
    return;
  }
  unknown_run_[unknown_run_len_++] = b;
  for (const char* code : SHUTDOWN_CODES) {
    const size_t code_len = strlen(code);
    if (code_len != unknown_run_len_ ||
        memcmp(code, unknown_run_, code_len) != 0) {
      continue;
    }
    unknown_run_fired_ = true;
    memcpy(shutdown_code_, code, code_len + 1);
    if (shutdownCallback_) {
      shutdownCallback_(shutdown_code_);
    }
    return;
  }
}

int BmsRelay::getShutdownCountdownSeconds() {
  if (shutdown_countdown_seconds_ < 0 ||
      now_millis_ - shutdown_warning_millis_ > 2000) {
    return -1;
  }
  return shutdown_countdown_seconds_;
}

// Called with every new byte.
void BmsRelay::processNextByte() {
  // If up to first three bytes of the sourceBuffer don't match expected
  // preamble, flush the data unchanged.
  for (unsigned int i = 0; i < std::min(sizeof(PREAMBLE), sourceBuffer_.size());
       i++) {
    if (sourceBuffer_[i] != PREAMBLE[i]) {
      purgeUnknownData();
      return;
    }
  }
  // Check if we have the message type.
  if (sourceBuffer_.size() < 4) {
    return;
  }
  uint8_t type = sourceBuffer_[3];
  if (type >= sizeof(PACKET_LENGTHS_BY_TYPE) ||
      PACKET_LENGTHS_BY_TYPE[type] < 0) {
    purgeUnknownData();
    return;
  }
  uint8_t len = PACKET_LENGTHS_BY_TYPE[type];
  if (sourceBuffer_.size() < len) {
    return;
  }
  Packet p(sourceBuffer_.data(), len);
  ingestPacket(p);
  sourceBuffer_.clear();
}

void BmsRelay::ingestPacket(Packet& p) {
  if (p.isValid()) {
    seen_valid_packet_ = true;
    unknown_run_len_ = 0;
    unknown_run_fired_ = false;
  }
  packet_tracker_.processPacket(p, now_millis_);
  for (auto& callback : receivedPacketCallbacks_) {
    callback(this, &p);
  };
  bmsStatusParser(p);
  shutdownWarningParser(p);
  bmsSerialParser(p);
  currentParser(p);
  batteryPercentageParser(p);
  cellVoltageParser(p);
  temperatureParser(p);
  bmsInfoParser(p);
  //  Recalculate CRC so that logging callbacks see the correct CRCs
  p.recalculateCrcIfValid();
  if (p.shouldForward()) {
    for (auto& callback : forwardedPacketCallbacks_) {
      callback(this, &p);
    }
  }
  p.recalculateCrcIfValid();
  if (p.shouldForward()) {
    for (int i = 0; i < p.len(); i++) {
      sink_(p.start()[i]);
    }
  }
}