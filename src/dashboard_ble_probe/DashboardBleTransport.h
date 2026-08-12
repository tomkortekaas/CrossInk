#pragma once

#include <atomic>

#include "DashboardBleProbe.h"

namespace probe {

class AcceptedFrameSignal {
 public:
  void mark() { pending_.store(true); }
  bool take() { return pending_.exchange(false); }
  void reset() { pending_.store(false); }

 private:
  std::atomic<bool> pending_{false};
};

}  // namespace probe

#if defined(CROSSINK_ENABLE_DASHBOARD_BLE_PROBE) && CROSSINK_ENABLE_DASHBOARD_BLE_PROBE
#include <NimBLEDevice.h>

namespace probe {

enum class PairingMode { Disabled, Onboarding };

class DashboardBleTransport final : public ProbeStatusSink,
                                    public NimBLECharacteristicCallbacks,
                                    public NimBLEServerCallbacks {
 public:
  bool begin(DashboardBleProbe& probe, PairingMode pairingMode = PairingMode::Disabled);
  bool end();
  bool hasConnected() const { return hasConnected_.load(); }
  uint32_t connectionCount() const { return connectionCount_.load(); }
  bool takeAcceptedFrame() { return acceptedFrame_.take(); }
  void publish(Status status, bool hasMessageId, uint32_t messageId) override;

 private:
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connection) override;
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override;
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connection, int reason) override;
  void onAuthenticationComplete(NimBLEConnInfo& connection) override;

  DashboardBleProbe* probe_ = nullptr;
  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* statusCharacteristic_ = nullptr;
  PairingMode pairingMode_ = PairingMode::Disabled;
  std::atomic<uint32_t> connectionCount_{0};
  std::atomic<bool> hasConnected_{false};
  AcceptedFrameSignal acceptedFrame_;
  bool initialized_ = false;
  char statusValue_[32]{};
};

}  // namespace probe
#endif
