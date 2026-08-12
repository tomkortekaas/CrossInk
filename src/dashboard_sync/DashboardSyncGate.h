#pragma once

#include <cstdint>

namespace dashboard_sync {

inline constexpr uint64_t kGateTimerWakeUs = 30ULL * 60ULL * 1000ULL * 1000ULL;
inline constexpr uint32_t kGateWindowMs = 10000;
inline constexpr uint32_t kConnectedSettleMs = 750;

enum class GateOutcome { InProgress, Connected, Timeout };

class GateState {
 public:
  GateState(uint32_t windowMs, uint32_t settleMs) : windowMs_(windowMs), settleMs_(settleMs) {}

  void startedAt(uint32_t nowMs);
  void connectedAt(uint32_t nowMs);
  GateOutcome outcomeAt(uint32_t nowMs) const;
  bool shouldStop(uint32_t nowMs) const { return outcomeAt(nowMs) != GateOutcome::InProgress; }

 private:
  uint32_t windowMs_;
  uint32_t settleMs_;
  uint32_t start_ = 0;
  uint32_t connect_ = 0;
  bool connected_ = false;
};

#if defined(CROSSINK_ENABLE_DASHBOARD_BLE_PROBE) && CROSSINK_ENABLE_DASHBOARD_BLE_PROBE
}  // namespace dashboard_sync

#include "dashboard_ble_probe/DashboardBleProbe.h"
#include "dashboard_ble_probe/DashboardBleTransport.h"

class HalGPIO;
class HalPowerManager;

namespace dashboard_sync {

[[noreturn]] void runConnectionGate(probe::DashboardBleTransport& transport, probe::DashboardBleProbe& probe,
                                    HalPowerManager& powerManager, HalGPIO& gpio);
#endif

}  // namespace dashboard_sync
