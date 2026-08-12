#include "DashboardBleWindow.h"

namespace dashboard_sync {

void DashboardBleWindow::startedAt(const uint32_t nowMs) {
  startedAtMs_ = nowMs;
  acceptedAtMs_ = 0;
  accepted_ = false;
  active_ = true;
}

void DashboardBleWindow::acceptedAt(const uint32_t nowMs) {
  if (!active_ || accepted_) return;
  acceptedAtMs_ = nowMs;
  accepted_ = true;
}

bool DashboardBleWindow::tick(const uint32_t nowMs) {
  if (!active_) return true;
  if (nowMs - startedAtMs_ >= windowMs_ || (accepted_ && nowMs - acceptedAtMs_ >= acceptedSettleMs_)) {
    requestRestart();
    return false;
  }
  return true;
}

void DashboardBleWindow::beforeReaderEnter() { requestRestart(); }

void DashboardBleWindow::requestRestart() {
  if (!active_) return;
  active_ = false;
  if (expiryCallback_ != nullptr) expiryCallback_(expiryContext_);
}

}  // namespace dashboard_sync
