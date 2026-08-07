#pragma once

class DashboardSleepScreen {
 public:
  virtual ~DashboardSleepScreen() = default;
  virtual bool renderForSleep() = 0;
};

inline bool tryRenderDashboardSleepScreen(const bool selected, DashboardSleepScreen* const screen) {
  return selected && screen != nullptr && screen->renderForSleep();
}
