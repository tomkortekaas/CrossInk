#pragma once
#include "LiveNavigationSession.h"
namespace navigator {
enum class NavigationRefresh : uint8_t { None, Full, Fast };
// Scheduling only, not a power/sleep driver. Pocket receipt never redraws.
// Fast means the existing SDK differential full-frame mode, not a window.
class NavigationRefreshPolicy {
 public:
  void setActiveView(bool value) { activeView_ = value; }
  bool activeView() const { return activeView_; }
  // Routine redraw cadence. The receiver applies the active live session's
  // LIVE_START v2 mode and resets it to Economical when no session is active;
  // the policy itself fails closed to Economical (the historical 30-second
  // cadence) until a mode is set.
  void setMode(WalkingRefreshMode mode) { mode_ = mode; }
  NavigationRefresh decide(uint32_t now,const LivePosition* position,bool manual,bool viewportChanged) const;
  void rendered(uint32_t now,const LivePosition* position,NavigationRefresh mode);
 private:
  uint32_t routineIntervalMs() const;
  LivePosition last_{};
  uint32_t renderedAt_=0;
  uint8_t fastCount_=0;
  WalkingRefreshMode mode_ = WalkingRefreshMode::Economical;
  bool activeView_ = false, hasRendered_ = false, hadPosition_ = false;
};
}
