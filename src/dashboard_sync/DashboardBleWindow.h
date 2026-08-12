#pragma once

#include <cstdint>

namespace dashboard_sync {

constexpr uint32_t kNormalBleWindowMs = 60000;
constexpr uint32_t kAcceptedSettleMs = 750;

class DashboardBleWindow {
 public:
  using ExpiryCallback = void (*)(void* context);

  explicit DashboardBleWindow(uint32_t windowMs = kNormalBleWindowMs,
                              uint32_t acceptedSettleMs = kAcceptedSettleMs)
      : windowMs_(windowMs), acceptedSettleMs_(acceptedSettleMs) {}

  void setExpiryCallback(ExpiryCallback callback, void* context) {
    expiryCallback_ = callback;
    expiryContext_ = context;
  }
  void startedAt(uint32_t nowMs);
  void acceptedAt(uint32_t nowMs);
  bool tick(uint32_t nowMs);
  void beforeReaderEnter();
  bool isActive() const { return active_; }

 private:
  void requestRestart();

  uint32_t windowMs_;
  uint32_t acceptedSettleMs_;
  uint32_t startedAtMs_ = 0;
  uint32_t acceptedAtMs_ = 0;
  bool active_ = false;
  bool accepted_ = false;
  ExpiryCallback expiryCallback_ = nullptr;
  void* expiryContext_ = nullptr;
};

}  // namespace dashboard_sync
