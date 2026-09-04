#pragma once
#include "LiveNavigationSession.h"
namespace navigator {
enum class NavigationRefresh : uint8_t { None, Full, Fast };
// Scheduling only, not a power/sleep driver. Pocket receipt never redraws.
// Fast means the existing SDK differential full-frame mode, not a window.
class NavigationRefreshPolicy {
 public:
  void setActiveView(bool value) { activeView_=value; }
  bool activeView() const { return activeView_; }
  NavigationRefresh decide(uint32_t now,const LivePosition* position,bool manual,bool viewportChanged) const;
  void rendered(uint32_t now,const LivePosition* position,NavigationRefresh mode);
 private:
  LivePosition last_{};
  uint32_t renderedAt_=0;
  uint8_t fastCount_=0;
  bool activeView_=false,hasRendered_=false,hadPosition_=false;
};
}
