#include "DashboardSyncGate.h"

namespace dashboard_sync {

void GateState::startedAt(const uint32_t nowMs) {
  start_ = nowMs;
  connected_ = false;
}

void GateState::connectedAt(const uint32_t nowMs) {
  connected_ = true;
  connect_ = nowMs;
}

GateOutcome GateState::outcomeAt(const uint32_t nowMs) const {
  const uint32_t elapsedSinceStart = nowMs - start_;
  if (elapsedSinceStart >= windowMs_) {
    return GateOutcome::Timeout;
  }
  if (connected_ && (nowMs - connect_) >= settleMs_) {
    return GateOutcome::Connected;
  }
  return GateOutcome::InProgress;
}

}  // namespace dashboard_sync

#if defined(CROSSINK_ENABLE_DASHBOARD_BLE_PROBE) && CROSSINK_ENABLE_DASHBOARD_BLE_PROBE

#include <Arduino.h>
#include <esp_attr.h>

#include "Logging.h"
#include "HalGPIO.h"
#include "HalPowerManager.h"

namespace dashboard_sync {
namespace {
RTC_DATA_ATTR uint32_t syncGateRunCounter = 0;
}

[[noreturn]] void runConnectionGate(probe::DashboardBleTransport& transport, probe::DashboardBleProbe& probe,
                                    HalPowerManager& powerManager, HalGPIO& gpio) {
  const uint32_t run = ++syncGateRunCounter;
  const uint32_t startedAtMs = millis();
  GateState state(kGateWindowMs, kConnectedSettleMs);
  state.startedAt(startedAtMs);
  bool connectionRecorded = false;

  if (!transport.begin(probe, probe::PairingMode::Disabled)) {
    LOG_ERR("SYNC", "Dashboard sync gate: NimBLE init failed");
    LOG_INF("SYNC", "SYNC_GATE run=%lu result=timeout connected_ms=-1 window_ms=%lu",
            static_cast<unsigned long>(run), static_cast<unsigned long>(kGateWindowMs));
    if (!transport.end()) {
      LOG_ERR("SYNC", "Dashboard sync gate: NimBLE deinit failed after init error");
    }
    powerManager.startDeepSleep(gpio, kGateTimerWakeUs);
    while (true) {
      delay(1000);
    }
  }
  probe.begin();

  while (!state.shouldStop(millis())) {
    if (!connectionRecorded && transport.hasConnected()) {
      connectionRecorded = true;
      state.connectedAt(millis());
    }
    delay(10);
  }

  const uint32_t finishedAtMs = millis();
  const GateOutcome outcome = state.outcomeAt(finishedAtMs);
  const long connectedMs = connectionRecorded ? static_cast<long>(finishedAtMs - startedAtMs) : -1L;
  LOG_INF("SYNC", "SYNC_GATE run=%lu result=%s connected_ms=%ld window_ms=%lu", static_cast<unsigned long>(run),
          outcome == GateOutcome::Connected ? "connected" : "timeout", connectedMs,
          static_cast<unsigned long>(kGateWindowMs));
  if (!transport.end()) {
    LOG_ERR("SYNC", "Dashboard sync gate: NimBLE deinit failed");
  }
  powerManager.startDeepSleep(gpio, kGateTimerWakeUs);
  while (true) {
    delay(1000);
  }
}

}  // namespace dashboard_sync
#endif
