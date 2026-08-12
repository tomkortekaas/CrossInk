#pragma once

#include <atomic>

#include "DashboardBleProbe.h"
#include "dashboard_sync/DashboardBleWindow.h"

#if defined(CROSSINK_ENABLE_DASHBOARD_BLE_PROBE) && CROSSINK_ENABLE_DASHBOARD_BLE_PROBE
#include <NimBLEDevice.h>

namespace probe {

enum class PairingMode { Disabled, Onboarding };

class PasskeyDisplay {
 public:
  virtual ~PasskeyDisplay() = default;
  virtual void showPasskey(uint32_t passkey) = 0;
};

class DashboardBleTransport final : public ProbeStatusSink,
                                    public dashboard_sync::BleWindowTransport,
                                    public NimBLECharacteristicCallbacks,
                                    public NimBLEServerCallbacks {
 public:
  bool begin(DashboardBleProbe& probe, PairingMode pairingMode = PairingMode::Disabled,
             PasskeyDisplay* passkeyDisplay = nullptr);
  bool end() override;
  bool hasConnected() const { return hasConnected_.load(); }
  uint32_t connectionCount() const { return connectionCount_.load(); }
  void publish(Status status, bool hasMessageId, uint32_t messageId) override;

 private:
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connection) override;
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override;
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connection, int reason) override;
  uint32_t onPassKeyDisplay() override;
  void onAuthenticationComplete(NimBLEConnInfo& connection) override;

  DashboardBleProbe* probe_ = nullptr;
  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* statusCharacteristic_ = nullptr;
  PairingMode pairingMode_ = PairingMode::Disabled;
  PasskeyDisplay* passkeyDisplay_ = nullptr;
  uint32_t passkey_ = 0;
  std::atomic<uint32_t> connectionCount_{0};
  std::atomic<bool> hasConnected_{false};
  bool initialized_ = false;
  char statusValue_[32]{};
};

}  // namespace probe
#endif
