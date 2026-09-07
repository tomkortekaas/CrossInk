#pragma once
#include <cstddef>
#include <cstdint>
namespace navigator {
// Routine redraw cadence of a live walking session, carried in byte 10 of
// LIVE_START v2: 0 balanced, 1 fast, 2 economical. A legacy LIVE_START v1
// (10 bytes) always means Economical. Modes fail closed: the session reports
// Economical unless a valid v2 START named another mode, and every exit path
// (disconnect, stop, expiry) resets the mode to Economical.
enum class WalkingRefreshMode : uint8_t { Balanced = 0, Fast = 1, Economical = 2 };
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
  bool position(uint32_t now, LivePosition& out, bool linkActive = true) const;
  // Returns a force-refresh request once for each newly accepted FIX carrying
  // flags bit 1. Replayed FIX frames never re-arm it.
  bool takeForceRefresh();
  WalkingRefreshMode refreshMode() const { return mode_; }
  bool active() const { return active_; }
  void disconnect();
  // The inactivity guard is only a disconnected-session fallback. A connected
  // stationary walk may legitimately receive no new CLLocation callbacks.
  void expire(uint32_t now, bool linkActive = false);
  uint32_t sessionId() const { return sessionId_; }

 private:
  uint32_t routeId_ = 0, sessionId_ = 0, lastStoppedId_ = 0, lastActivity_ = 0;
  uint8_t lastFrame_[20]{};
  bool active_ = false, hasFix_ = false, forceRefreshPending_ = false;
  WalkingRefreshMode mode_ = WalkingRefreshMode::Economical;
};
static_assert(sizeof(LiveNavigationSession) <= 64, "Live state must stay bounded");
}  // namespace navigator
