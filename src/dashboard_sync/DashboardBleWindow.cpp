#include "DashboardBleWindow.h"

namespace dashboard_sync {

void DashboardBleWindow::startedAt(const uint32_t nowMs) {
  startedAtMs_ = nowMs;
  active_ = true;
}

bool DashboardBleWindow::tick(const uint32_t nowMs) {
  if (active_ && nowMs - startedAtMs_ >= windowMs_) {
    return stop();
  }
  return true;
}

bool DashboardBleWindow::beforeReaderEnter() { return stop(); }

bool DashboardBleWindow::stop() {
  if (!active_) {
    return true;
  }
  active_ = false;
  return transport_.end();
}

}  // namespace dashboard_sync
