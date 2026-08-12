#pragma once

namespace dashboard_sync {

enum class BootRoute {
  NormalBoot,
  DashboardSync,
};

BootRoute selectBootRoute(bool timerWake, bool powerPressed);

}  // namespace dashboard_sync
