#include "LiveNavigationSession.h"

#include <cstring>
namespace navigator {
namespace {
uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
int32_t i32(const uint8_t* p) {
  const uint32_t v = u32(p);
  return v <= 0x7fffffffu ? int32_t(v) : int32_t(int64_t(v) - 0x100000000ll);
}
}  // namespace
void LiveNavigationStatus::writeEnvelope(uint8_t out[7]) const {
  out[0] = static_cast<uint8_t>(code);
  for (int i = 0; i < 4; ++i) out[1 + i] = static_cast<uint8_t>(sessionId >> (8 * i));
  out[5] = sequence & 255;
  out[6] = sequence >> 8;
}
void LiveNavigationSession::disconnect() {
  active_ = false;
  hasFix_ = false;
}
void LiveNavigationSession::expire(uint32_t now) {
  if (active_ && uint32_t(now - lastActivity_) >= 90000) disconnect();
}
bool LiveNavigationSession::position(uint32_t now, LivePosition& out) const {
  if (!active_ || !hasFix_ || uint64_t(uint32_t(now - receivedAt_)) + u16(lastFrame_ + 17) > 30000) return false;
  out = {i32(lastFrame_ + 7), i32(lastFrame_ + 11), u16(lastFrame_ + 15), lastFrame_[19] != 0};
  return true;
}
LiveNavigationStatus LiveNavigationSession::receive(const uint8_t* b, size_t n, uint32_t route, bool busy,
                                                    uint32_t now) {
  uint32_t id = 0;
  uint16_t sequence = 0;
  if (b && n >= 5) {
    if (b[0] == 8) {
      if (n >= 10) id = u32(b + 6);
    } else
      id = u32(b + 1);
  }
  if (b && n >= 7 && b[0] == 9) sequence = u16(b + 5);
  const LiveNavigationStatus invalid{LiveNavigationCode::Invalid, id, sequence};
  if (!b || !n || !id) return invalid;
  expire(now);
  if (b[0] == 8) {
    if (n != 10 || b[1] != 1 || !route || u32(b + 2) != route || busy) return invalid;
    if (active_) {
      if (id == sessionId_ && route == routeId_) return {LiveNavigationCode::Ready, id, 0};
      return invalid;
    }
    // A stopped/expired session cannot be revived by replaying its START.
    if (id == sessionId_ || id == lastStoppedId_) return invalid;
    sessionId_ = id;
    routeId_ = route;
    active_ = true;
    hasFix_ = false;
    lastActivity_ = now;
    return {LiveNavigationCode::Ready, id, 0};
  }
  if (b[0] == 11) {
    if (n != 5) return invalid;
    if (!active_ && id == lastStoppedId_) return {LiveNavigationCode::Stopped, id, 0};
    if (!active_ || id != sessionId_) return invalid;
    lastStoppedId_ = id;
    disconnect();
    return {LiveNavigationCode::Stopped, id, 0};
  }
  if (b[0] != 9 || n != 20 || !active_ || id != sessionId_ || busy) return invalid;
  if (route != routeId_) {
    disconnect();
    return invalid;
  }
  if (i32(b + 7) < -900000000 || i32(b + 7) > 900000000 || i32(b + 11) < -1800000000 || i32(b + 11) > 1800000000 ||
      u16(b + 15) < 1 || u16(b + 15) > 50 || u16(b + 17) > 15000 || b[19] > 1)
    return invalid;
  if (hasFix_) {
    const uint16_t advance = static_cast<uint16_t>(sequence - u16(lastFrame_ + 5));
    if (!advance) {
      if (std::memcmp(b, lastFrame_, 20) == 0) return {LiveNavigationCode::FixAccepted, id, sequence};
      return invalid;
    }
    if (advance >= 32768) return invalid;
  }
  std::memcpy(lastFrame_, b, 20);
  hasFix_ = true;
  receivedAt_ = now;
  lastActivity_ = now;
  return {LiveNavigationCode::FixAccepted, id, sequence};
}
}  // namespace navigator
