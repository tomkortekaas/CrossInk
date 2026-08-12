#include "DashboardSyncWakePolicy.h"

namespace dashboard_sync {

BootRoute selectBootRoute(const bool timerWake, const bool powerPressed) {
  if (powerPressed) {
    return BootRoute::NormalBoot;
  }
  if (timerWake) {
    return BootRoute::DashboardSync;
  }
  return BootRoute::NormalBoot;
}

}  // namespace dashboard_sync
