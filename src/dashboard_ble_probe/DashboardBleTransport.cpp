#include "DashboardBleTransport.h"

#if defined(CROSSINK_ENABLE_DASHBOARD_BLE_PROBE) && CROSSINK_ENABLE_DASHBOARD_BLE_PROBE

#include <cstring>

#include "Logging.h"
#include "DashboardBleSecurityPolicy.h"

namespace probe {

bool DashboardBleTransport::begin(DashboardBleProbe& probe, const PairingMode pairingMode) {
  probe_ = &probe;
  pairingMode_ = pairingMode;
  connectionCount_.store(0);
  hasConnected_.store(false);
  acceptedFrame_.reset();
  if (!NimBLEDevice::init(kAdvertisingName)) {
    LOG_ERR("BLE", "Dashboard probe: NimBLE init failed");
    return false;
  }

  initialized_ = true;
  NimBLEDevice::setSecurityAuth(kDashboardBleSecurity.bonding, kDashboardBleSecurity.mitm,
                                kDashboardBleSecurity.secureConnections);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  server_ = NimBLEDevice::createServer();
  if (server_ == nullptr) {
    LOG_ERR("BLE", "Dashboard probe: server creation failed");
    return false;
  }
  server_->setCallbacks(this, false);

  NimBLEService* service = server_->createService(kServiceUuid);
  if (service == nullptr) {
    LOG_ERR("BLE", "Dashboard probe: service creation failed");
    return false;
  }

  uint32_t writeProperties = NIMBLE_PROPERTY::WRITE;
  uint32_t statusProperties = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY;
  if (kDashboardBleSecurity.encryptedCharacteristics) {
    writeProperties |= NIMBLE_PROPERTY::WRITE_ENC;
    statusProperties |= NIMBLE_PROPERTY::READ_ENC;
  }
  if (kDashboardBleSecurity.authenticatedCharacteristics) {
    writeProperties |= NIMBLE_PROPERTY::WRITE_AUTHEN;
    statusProperties |= NIMBLE_PROPERTY::READ_AUTHEN;
  }
  NimBLECharacteristic* writeCharacteristic =
      service->createCharacteristic(kWriteUuid, writeProperties, kMaxFrameBytes);
  statusCharacteristic_ =
      service->createCharacteristic(kStatusUuid, statusProperties, sizeof(statusValue_));
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
  if (pairingMode == PairingMode::Disabled && kDashboardBleSecurity.bonding) {
    const int bondCount = NimBLEDevice::getNumBonds();
    for (int index = 0; index < bondCount; ++index) {
      NimBLEDevice::whiteListAdd(NimBLEDevice::getBondedAddress(index));
    }
    advertising->setScanFilter(false, true);
  }
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

bool DashboardBleTransport::end() {
  if (!initialized_) {
    return true;
  }
  NimBLEAdvertising* const advertising = NimBLEDevice::getAdvertising();
  LOG_INF("BLE", "Dashboard probe shutdown: stopping advertising");
  if (advertising != nullptr && advertising->isAdvertising()) {
    if (!advertising->stop()) {
      LOG_ERR("BLE", "Dashboard probe shutdown: advertising stop failed");
      return false;
    }
  }
  // ble_gap_adv_stop updates host state synchronously, but the ESP32-C3
  // controller completes the HCI disable asynchronously. Deinitializing while
  // that command is still in flight can wedge btdm_controller_task in an
  // interrupt-disabled section and trip the interrupt watchdog.
  delay(500);
  LOG_INF("BLE", "Dashboard probe shutdown: advertising settled");
  if (server_ != nullptr) {
    for (const uint16_t handle : server_->getPeerDevices()) {
      LOG_INF("BLE", "Dashboard probe shutdown: disconnecting handle=%u", static_cast<unsigned>(handle));
      server_->disconnect(handle);
    }
  }
  delay(500);
  LOG_INF("BLE", "Dashboard probe shutdown: deinitializing NimBLE");
  const bool result = NimBLEDevice::deinit(true);
  LOG_INF("BLE", "Dashboard probe shutdown: NimBLE deinit result=%u", static_cast<unsigned>(result));
  initialized_ = false;
  probe_ = nullptr;
  server_ = nullptr;
  statusCharacteristic_ = nullptr;
  return result;
}

void DashboardBleTransport::publish(const Status status, const bool hasMessageId, const uint32_t messageId) {
  if (statusCharacteristic_ == nullptr ||
      !formatStatus(status, hasMessageId, messageId, statusValue_, sizeof(statusValue_))) {
    return;
  }
  statusCharacteristic_->setValue(reinterpret_cast<const uint8_t*>(statusValue_), strlen(statusValue_));
  const bool notified = statusCharacteristic_->notify();
  if (status == Status::Accepted && notified) acceptedFrame_.mark();
}

void DashboardBleTransport::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) {
  const NimBLEAttValue& value = characteristic->getValue();
  const bool validLength = value.size() > 0 && value.size() <= kMaxFrameBytes;
  if (!validLength || probe_ == nullptr || !probe_->enqueue(value.data(), value.size())) {
    publish(Status::InvalidLength, false, 0);
  }
}

void DashboardBleTransport::onConnect(NimBLEServer*, NimBLEConnInfo&) {
  hasConnected_.store(true);
  connectionCount_.fetch_add(1);
  LOG_INF("BLE", "Dashboard probe: peer connected");
}

void DashboardBleTransport::onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {
  LOG_INF("BLE", "Dashboard probe: peer disconnected");
  if (initialized_) {
    NimBLEDevice::startAdvertising();
  }
}

void DashboardBleTransport::onAuthenticationComplete(NimBLEConnInfo& connection) {
  LOG_INF("BLE", "Dashboard probe: authentication complete encrypted=%u authenticated=%u bonded=%u",
          static_cast<unsigned>(connection.isEncrypted()), static_cast<unsigned>(connection.isAuthenticated()),
          static_cast<unsigned>(connection.isBonded()));
  if (kDashboardBleSecurity.encryptedCharacteristics && !connection.isEncrypted()) {
    if (server_ != nullptr) {
      server_->disconnect(connection.getConnHandle());
    }
    LOG_ERR("BLE", "Dashboard probe: unauthenticated connection rejected");
  }
}

}  // namespace probe
#endif
