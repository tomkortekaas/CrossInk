#pragma once
#include <cstddef>
#include <cstdint>
namespace navigator {
enum class LiveNavigationCode : uint8_t { Ready = 0x26, FixAccepted = 0x27, Stopped = 0x28, Invalid = 0x29 };
struct LiveNavigationStatus {
  LiveNavigationCode code = LiveNavigationCode::Invalid;
  uint32_t sessionId = 0;
  uint16_t sequence = 0;
  void writeEnvelope(uint8_t out[7]) const;
};
struct LivePosition {
  int32_t latitudeE7 = 0, longitudeE7 = 0;
  uint16_t accuracyMeters = 0;
  bool offRoute = false;
};
// One bounded RAM-only fix. Never mutates routes, files, display or radio.
// Caller provides monotonic milliseconds and the current validated route ID.
class LiveNavigationSession {
 public:
  LiveNavigationStatus receive(const uint8_t* bytes, size_t length, uint32_t activeRouteId, bool routeTransferring,
                               uint32_t now);
  bool position(uint32_t now, LivePosition& out) const;
  bool active() const { return active_; }
  void disconnect();
  void expire(uint32_t now);
  uint32_t sessionId() const { return sessionId_; }

 private:
  uint32_t routeId_ = 0, sessionId_ = 0, lastStoppedId_ = 0, lastActivity_ = 0, receivedAt_ = 0;
  uint8_t lastFrame_[20]{};
  bool active_ = false, hasFix_ = false;
};
static_assert(sizeof(LiveNavigationSession) <= 64, "Live state must stay bounded");
}  // namespace navigator
