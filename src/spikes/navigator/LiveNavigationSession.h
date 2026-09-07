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
  // Only meaningful for a live protocol v3 session: the iPhone tracker's
  // accepted distance from the route start in whole metres. Legacy v1/v2
  // sessions keep it absent (false/0).
  bool hasRouteProgress = false;
  uint32_t distanceFromStartMeters = 0;
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
  uint8_t lastFrame_[24]{};  // 20-byte legacy FIX or 24-byte v3 FIX with progress
  bool active_ = false, hasFix_ = false, forceRefreshPending_ = false;
  WalkingRefreshMode mode_ = WalkingRefreshMode::Economical;
  // Negotiated live protocol version (1, 2 or 3) while a session is active;
  // 0 after every session exit. Version 3 selects the 24-byte FIX layout and
  // is what makes distanceFromStartMeters available to position().
  uint8_t version_ = 0;
};
static_assert(sizeof(LiveNavigationSession) <= 48, "Live state must stay bounded");
}  // namespace navigator
