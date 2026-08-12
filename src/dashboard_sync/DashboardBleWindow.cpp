#include "DashboardBleWindow.h"

namespace dashboard_sync {

void DashboardBleWindow::startedAt(const uint32_t nowMs) {
  startedAtMs_ = nowMs;
  active_ = true;
}

bool DashboardBleWindow::tick(const uint32_t nowMs) {
  if (active_ && nowMs - startedAtMs_ >= windowMs_) {
    // ESP32-C3 can wedge inside NimBLEAdvertising::stop(). The caller handles
    // expiry by performing a controlled restart into a one-shot BLE-free boot.
    active_ = false;
    if (expiryCallback_ != nullptr) {
      expiryCallback_(expiryContext_);
    }
    return false;
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
