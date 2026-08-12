#pragma once

#include <cstdint>

namespace dashboard_sync {

// Normal boot allows enough time for manual AccessorySetupKit onboarding. This
// is separate from the fixed 10-second timer-wake connection gate. Entering a
// reader always closes this window early.
constexpr uint32_t kNormalBleWindowMs = 60000;

class BleWindowTransport {
 public:
  virtual ~BleWindowTransport() = default;
  virtual bool end() = 0;
};

class DashboardBleWindow {
 public:
  explicit DashboardBleWindow(BleWindowTransport& transport, uint32_t windowMs = kNormalBleWindowMs)
      : transport_(transport), windowMs_(windowMs) {}

  void startedAt(uint32_t nowMs);
  bool tick(uint32_t nowMs);
  bool beforeReaderEnter();
  bool isActive() const { return active_; }

 private:
  bool stop();

  BleWindowTransport& transport_;
  uint32_t windowMs_;
  uint32_t startedAtMs_ = 0;
  bool active_ = false;
};

}  // namespace dashboard_sync
