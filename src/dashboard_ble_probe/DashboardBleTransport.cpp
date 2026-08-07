#include "DashboardBleTransport.h"

#if defined(CROSSINK_ENABLE_DASHBOARD_BLE_PROBE) && CROSSINK_ENABLE_DASHBOARD_BLE_PROBE

#include <cstring>

#include "Logging.h"

namespace probe {

bool DashboardBleTransport::begin(DashboardBleProbe& probe) {
  probe_ = &probe;
  if (!NimBLEDevice::init(kAdvertisingName)) {
    LOG_ERR("BLE", "Dashboard probe: NimBLE init failed");
    return false;
  }

  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEServer* server = NimBLEDevice::createServer();
  if (server == nullptr) {
    LOG_ERR("BLE", "Dashboard probe: server creation failed");
    return false;
  }
  server->setCallbacks(this);

  NimBLEService* service = server->createService(kServiceUuid);
  if (service == nullptr) {
    LOG_ERR("BLE", "Dashboard probe: service creation failed");
    return false;
  }

  NimBLECharacteristic* writeCharacteristic =
      service->createCharacteristic(kWriteUuid, NIMBLE_PROPERTY::WRITE, kMaxFrameBytes);
  statusCharacteristic_ =
      service->createCharacteristic(kStatusUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, sizeof(statusValue_));
  if (writeCharacteristic == nullptr || statusCharacteristic_ == nullptr) {
    LOG_ERR("BLE", "Dashboard probe: characteristic creation failed");
    return false;
  }
  writeCharacteristic->setCallbacks(this);

  if (!service->start()) {
    LOG_ERR("BLE", "Dashboard probe: service start failed");
    return false;
  }
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  // Enable the secondary payload before setting the name so NimBLE places the
  // name in the scan response and leaves room for the 128-bit service UUID.
  advertising->enableScanResponse(true);
  if (!advertising->setName(kAdvertisingName)) {
    LOG_ERR("BLE", "Dashboard probe: advertising name rejected");
    return false;
  }
  if (!advertising->addServiceUUID(kServiceUuid)) {
    LOG_ERR("BLE", "Dashboard probe: advertising UUID rejected");
    return false;
  }
  if (!advertising->start() || !advertising->isAdvertising()) {
    LOG_ERR("BLE", "Dashboard probe: advertising start failed");
    return false;
  }
  LOG_INF("BLE", "Dashboard probe advertising active: %s", NimBLEDevice::getAddress().toString().c_str());
  return true;
}

void DashboardBleTransport::publish(const Status status, const bool hasMessageId, const uint32_t messageId) {
  if (statusCharacteristic_ == nullptr ||
      !formatStatus(status, hasMessageId, messageId, statusValue_, sizeof(statusValue_))) {
    return;
  }
  statusCharacteristic_->setValue(reinterpret_cast<const uint8_t*>(statusValue_), strlen(statusValue_));
  statusCharacteristic_->notify();
}

void DashboardBleTransport::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) {
  const NimBLEAttValue& value = characteristic->getValue();
  const bool validLength = value.size() > 0 && value.size() <= kMaxFrameBytes;
  if (!validLength || probe_ == nullptr || !probe_->enqueue(value.data(), value.size())) {
    publish(Status::InvalidLength, false, 0);
  }
}

void DashboardBleTransport::onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {
  NimBLEDevice::startAdvertising();
}

}  // namespace probe
#endif
