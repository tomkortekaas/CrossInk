#pragma once

#include "DashboardBleProbe.h"

#if defined(CROSSINK_ENABLE_DASHBOARD_BLE_PROBE) && CROSSINK_ENABLE_DASHBOARD_BLE_PROBE
#include <NimBLEDevice.h>

namespace probe {

class DashboardBleTransport final : public ProbeStatusSink,
                                    public NimBLECharacteristicCallbacks,
                                    public NimBLEServerCallbacks {
 public:
  bool begin(DashboardBleProbe& probe);
  void publish(Status status, bool hasMessageId, uint32_t messageId) override;

 private:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override;
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connection, int reason) override;

  DashboardBleProbe* probe_ = nullptr;
  NimBLECharacteristic* statusCharacteristic_ = nullptr;
  char statusValue_[32]{};
};

}  // namespace probe
#endif
