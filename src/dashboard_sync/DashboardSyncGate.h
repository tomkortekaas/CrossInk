#pragma once

#include <cstdint>

namespace dashboard_sync {

// Seconds between dashboard timer wakes. Overridable at build time so a battery test
// can run a short cycle without shipping one: -DCROSSINK_DASHBOARD_TIMER_WAKE_S=60.
// It belongs here rather than at the sleep call site, because runConnectionGate()
// re-arms the timer itself after every window — an override that reached only the
// first sleep would silently leave every later cycle at the default.
#ifndef CROSSINK_DASHBOARD_TIMER_WAKE_S
#define CROSSINK_DASHBOARD_TIMER_WAKE_S 1800
#endif
inline constexpr uint64_t kGateTimerWakeUs = static_cast<uint64_t>(CROSSINK_DASHBOARD_TIMER_WAKE_S) * 1000000ULL;
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
