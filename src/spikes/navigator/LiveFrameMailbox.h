#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace navigator {

// One bounded hand-off buffer between the BLE callback and the navigator loop.
// The caller provides synchronization. Normal 20-byte LIVE_FIX frames may
// coalesce while e-paper rendering blocks the loop: only the newest fix is
// useful, and dropping the older pending fix must not terminate the session.
class LiveFrameMailbox {
 public:
  static constexpr size_t kCapacity = 512;

  bool push(const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0 || length > kCapacity) {
      overflow_ = true;
      return false;
    }
    if (length_ != 0 &&
        !(isValidFix(bytes_, length_) && isValidFix(data, length) && u32(bytes_ + 1) == u32(data + 1))) {
      overflow_ = true;
      return false;
    }
    std::memcpy(bytes_, data, length);
    length_ = length;
    return true;
  }

  size_t take(uint8_t* output, size_t capacity) {
    if (output == nullptr || capacity < length_) {
      overflow_ = true;
      return 0;
    }
    const size_t result = length_;
    if (result != 0) std::memcpy(output, bytes_, result);
    length_ = 0;
    return result;
  }

  bool takeOverflow() {
    const bool result = overflow_;
    overflow_ = false;
    return result;
  }

  bool overflowed() const { return overflow_; }

  void reset() {
    length_ = 0;
    overflow_ = false;
  }

 private:
  static uint16_t u16(const uint8_t* data) { return uint16_t(data[0]) | (uint16_t(data[1]) << 8); }

  static uint32_t u32(const uint8_t* data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) |
           (uint32_t(data[3]) << 24);
  }

  static int32_t i32(const uint8_t* data) {
    const uint32_t value = u32(data);
    return value <= 0x7fffffffu ? int32_t(value) : int32_t(int64_t(value) - 0x100000000ll);
  }

  static bool isValidFix(const uint8_t* data, size_t length) {
    return length == 20 && data[0] == 0x09 && u32(data + 1) != 0 && i32(data + 7) >= -900000000 &&
           i32(data + 7) <= 900000000 && i32(data + 11) >= -1800000000 && i32(data + 11) <= 1800000000 &&
           u16(data + 15) >= 1 && u16(data + 15) <= 50 && u16(data + 17) <= 15000 && data[19] <= 3;
  }

  uint8_t bytes_[kCapacity]{};
  size_t length_ = 0;
  bool overflow_ = false;
};

}  // namespace navigator
